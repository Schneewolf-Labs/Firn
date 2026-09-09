#include "firn/io_psp.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>

#include "firn/io.h"
#include "stb/stb_image.h"  // declarations only; the implementation lives in io.cpp

namespace firn::io {
namespace {

// Block ids (from the version 8 file format specification).
enum : uint16_t {
    kImageBlock = 0, kCreatorBlock = 1, kColorBlock = 2, kLayerStartBlock = 3, kLayerBlock = 4,
    kChannelBlock = 5, kSelectionBlock = 6, kCompositeImageBlock = 9, kCompositeBankBlock = 16,
    kCompositeAttrBlock = 17, kJpegBlock = 18,
};
enum : uint16_t { kCompNone = 0, kCompRle = 1, kCompLz77 = 2, kCompJpeg = 3 };
// Bitmap (DIB) types. Layers use 0/1; thumbnails 5/6; composites 8/9.
enum : uint16_t { kDibImage = 0, kDibTransMask = 1, kDibThumbnail = 5, kDibThumbnailTrans = 6, kDibComposite = 8, kDibCompositeTrans = 9 };
bool is_image_dib(uint16_t t) { return t == kDibImage || t == kDibThumbnail || t == kDibComposite; }
bool is_trans_dib(uint16_t t) { return t == kDibTransMask || t == kDibThumbnailTrans || t == kDibCompositeTrans; }
enum : uint8_t { kLayerRaster = 1, kLayerVector = 3, kLayerAdjustment = 4, kLayerGroup = 5, kLayerMask = 6, kLayerArtMedia = 7 };

const char kSignature[] = "Paint Shop Pro Image File\n\x1a";

struct Reader {
    const uint8_t* p;
    size_t n;
    bool ok(size_t off, size_t len) const { return off <= n && len <= n - off; }
    uint8_t u8(size_t o) const { return p[o]; }
    uint16_t u16(size_t o) const { return static_cast<uint16_t>(p[o] | (p[o + 1] << 8)); }
    uint32_t u32(size_t o) const { return static_cast<uint32_t>(p[o] | (p[o + 1] << 8) | (p[o + 2] << 16) | (static_cast<uint32_t>(p[o + 3]) << 24)); }
    int32_t i32(size_t o) const { return static_cast<int32_t>(u32(o)); }
};

struct Block {
    uint16_t id;
    size_t start, end;  // payload range
};

// Iterates "~BK\0" blocks in [start, end). Stops at the first malformed one.
std::vector<Block> blocks(const Reader& r, size_t start, size_t end) {
    std::vector<Block> out;
    size_t o = start;
    while (o + 10 <= end) {
        if (std::memcmp(r.p + o, "~BK\0", 4) != 0) break;
        const uint16_t id = r.u16(o + 4);
        const size_t len = r.u32(o + 6);
        if (!r.ok(o + 10, len) || o + 10 + len > end) break;
        out.push_back({id, o + 10, o + 10 + len});
        o += 10 + len;
    }
    return out;
}

std::vector<uint8_t> decode_rle(const uint8_t* src, size_t len, size_t want) {
    std::vector<uint8_t> out;
    out.reserve(want);
    size_t i = 0;
    while (i < len && out.size() < want) {
        const unsigned run = src[i++];
        if (run > 128) {
            if (i >= len) break;
            out.insert(out.end(), run - 128, src[i++]);
        } else {
            const size_t k = std::min<size_t>(run, len - i);
            out.insert(out.end(), src + i, src + i + k);
            i += k;
        }
    }
    return out;
}

bool decompress(uint16_t comp, const uint8_t* src, size_t len, size_t want, std::vector<uint8_t>& out, std::string& err) {
    if (comp == kCompNone) {
        out.assign(src, src + std::min(len, want));
    } else if (comp == kCompRle) {
        out = decode_rle(src, len, want);
    } else if (comp == kCompLz77) {
        int outlen = 0;
        char* buf = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(src), static_cast<int>(len), &outlen);
        if (!buf) { err = "zlib decode failed"; return false; }
        out.assign(buf, buf + outlen);
        free(buf);
    } else {
        err = "unsupported channel compression " + std::to_string(comp);
        return false;
    }
    if (out.size() < want) { err = "channel data truncated"; return false; }
    return true;
}

struct Palette {
    std::vector<Color> entries;
};

struct Header {
    int width = 0, height = 0;
    uint16_t compression = kCompLz77;
    uint16_t depth = 24;
    bool greyscale = false;
    int active_layer = 0;
};

// Reads the channel sub-blocks of one raster bitmap into an RGBA image of
// (w x h). Missing colour channels stay 0; missing alpha stays opaque.
bool read_channels(const Reader& r, const std::vector<Block>& subs, uint16_t comp, uint16_t depth,
                   const Palette* pal, bool grey, int w, int h, Image& out, std::string& err) {
    out = Image(w, h, {0, 0, 0, 255});
    const size_t npx = static_cast<size_t>(w) * h;
    const int bytes_per_sample = depth == 48 ? 2 : 1;
    for (const Block& b : subs) {
        if (b.id != kChannelBlock) continue;
        if (!r.ok(b.start, 16)) { err = "short channel block"; return false; }
        const size_t chunk = r.u32(b.start);
        const size_t clen = r.u32(b.start + 4);
        const uint16_t bitmap_type = r.u16(b.start + 12);
        const uint16_t channel_type = r.u16(b.start + 14);
        const size_t data_at = b.start + chunk;
        if (!r.ok(data_at, clen) || data_at + clen > b.end) { err = "channel data out of range"; return false; }
        if (!is_image_dib(bitmap_type) && !is_trans_dib(bitmap_type)) continue;

        const int bps = is_image_dib(bitmap_type) ? bytes_per_sample : 1;
        std::vector<uint8_t> data;
        if (!decompress(comp, r.p + data_at, clen, npx * bps, data, err)) return false;

        uint8_t* px = out.data();
        if (is_trans_dib(bitmap_type)) {
            for (size_t i = 0; i < npx; ++i) px[i * 4 + 3] = data[i];
        } else if (channel_type >= 1 && channel_type <= 3) {
            const int c = channel_type - 1;
            for (size_t i = 0; i < npx; ++i) px[i * 4 + c] = bps == 2 ? data[i * 2 + 1] : data[i];
        } else {
            // Composite channel: palette index or grey level.
            for (size_t i = 0; i < npx; ++i) {
                const uint8_t v = bps == 2 ? data[i * 2 + 1] : data[i];
                Color c{v, v, v, 255};
                if (pal && !grey && v < pal->entries.size()) c = pal->entries[v];
                else if (pal && v < pal->entries.size()) c = pal->entries[v];
                px[i * 4 + 0] = c.r; px[i * 4 + 1] = c.g; px[i * 4 + 2] = c.b;
            }
        }
    }
    return true;
}

BlendMode map_blend(uint8_t v) {
    if (v <= 16) return static_cast<BlendMode>(v);
    if (v >= 17 && v <= 20) return static_cast<BlendMode>(v - 14);  // "True" hue/sat/colour/lightness
    return BlendMode::Normal;
}

bool read_layer(const Reader& r, const Block& lb, const Header& hdr, const Palette* pal, Document& doc,
                std::vector<std::string>* warnings, std::string& err) {
    if (!r.ok(lb.start, 6)) { err = "short layer block"; return false; }
    const size_t chunk = r.u32(lb.start);
    size_t o = lb.start + 4;
    const uint16_t name_len = r.u16(o); o += 2;
    if (!r.ok(o, name_len + 1u + 32u + 5u)) { err = "short layer header"; return false; }
    std::string name(reinterpret_cast<const char*>(r.p + o), name_len);
    o += name_len;
    const uint8_t type = r.u8(o++);
    const int32_t rect[4] = {r.i32(o), r.i32(o + 4), r.i32(o + 8), r.i32(o + 12)}; o += 16;
    const int32_t saved[4] = {r.i32(o), r.i32(o + 4), r.i32(o + 8), r.i32(o + 12)}; o += 16;
    const uint8_t opacity = r.u8(o), blend = r.u8(o + 1), visible = r.u8(o + 2);

    static const char* kTypeNames[] = {"undefined", "raster", "floating selection", "vector", "adjustment", "group", "mask", "art media"};
    if (type != kLayerRaster) {
        if (warnings) warnings->push_back("Skipped " + std::string(type < 8 ? kTypeNames[type] : "unknown") + " layer \"" + name + "\"");
        return true;
    }

    // Layer bitmap information chunk, then channel sub-blocks.
    const size_t bo = lb.start + chunk;
    if (!r.ok(bo, 8) || bo + 8 > lb.end) { err = "missing layer bitmap chunk"; return false; }
    const size_t bchunk = r.u32(bo);
    const std::vector<Block> subs = blocks(r, bo + bchunk, lb.end);

    // Pixel data covers the saved rect, positioned relative to the layer rect.
    const int sw = saved[2] - saved[0], sh = saved[3] - saved[1];
    const int ox = rect[0] + saved[0], oy = rect[1] + saved[1];
    Layer& L = doc.add_layer(name);
    L.opacity = opacity / 255.0f;
    L.blend = map_blend(blend);
    L.visible = visible != 0;
    if (sw <= 0 || sh <= 0) return true;  // empty layer

    Image tile;
    if (!read_channels(r, subs, hdr.compression, hdr.depth, pal, hdr.greyscale, sw, sh, tile, err)) return false;
    Image& dst = L.pixels;
    for (int y = 0; y < sh; ++y) {
        const int dy = oy + y;
        if (dy < 0 || dy >= dst.height()) continue;
        const int x0 = std::max(0, -ox), x1 = std::min(sw, dst.width() - ox);
        if (x1 <= x0) continue;
        std::memcpy(dst.data() + (static_cast<size_t>(dy) * dst.width() + ox + x0) * 4,
                    tile.data() + (static_cast<size_t>(y) * sw + x0) * 4, static_cast<size_t>(x1 - x0) * 4);
    }
    // Pixels outside the saved rect are transparent; a layer with no
    // transparency channel is the opaque Background.
    bool has_alpha = false;
    for (const Block& b : subs)
        if (b.id == kChannelBlock && r.ok(b.start, 16) && is_trans_dib(r.u16(b.start + 12))) has_alpha = true;
    if (!has_alpha && doc.layer_count() == 1 && sw == doc.width() && sh == doc.height()) L.background = true;
    return true;
}

// Fallback: the full-size composite stored in the composite image bank.
// The bank lists all attribute blocks first, then one data block per
// attribute in the same order: a JPEG block, or a composite block holding a
// bitmap chunk followed by channel blocks.
bool read_composite(const Reader& r, const Block& bank, const Header& hdr, const Palette* pal, Document& doc,
                    std::string& err) {
    if (!r.ok(bank.start, 8)) return false;
    const size_t chunk = r.u32(bank.start);
    std::vector<Block> attrs, datas;
    for (const Block& b : blocks(r, bank.start + chunk, bank.end)) {
        if (b.id == kCompositeAttrBlock) attrs.push_back(b);
        else if (b.id == kJpegBlock || b.id == kCompositeImageBlock) datas.push_back(b);
    }
    for (size_t i = 0; i < attrs.size() && i < datas.size(); ++i) {
        const Block& a = attrs[i];
        if (!r.ok(a.start, 24)) continue;
        const int w = r.i32(a.start + 4), h = r.i32(a.start + 8);
        const uint16_t depth = r.u16(a.start + 12), comp = r.u16(a.start + 14), ctype = r.u16(a.start + 22);
        if (ctype != 0) continue;  // 0 = full size, 1 = thumbnail
        const Block& d = datas[i];
        Image img;
        if (comp == kCompJpeg && d.id == kJpegBlock && r.ok(d.start, 14)) {
            const size_t jchunk = r.u32(d.start), jlen = r.u32(d.start + 4);
            if (!r.ok(d.start + jchunk, jlen)) continue;
            int iw = 0, ih = 0, n = 0;
            unsigned char* px = stbi_load_from_memory(r.p + d.start + jchunk, static_cast<int>(jlen), &iw, &ih, &n, 4);
            if (!px) continue;
            img = Image(iw, ih);
            std::memcpy(img.data(), px, img.size_bytes());
            stbi_image_free(px);
        } else if (d.id == kCompositeImageBlock && r.ok(d.start, 8)) {
            const size_t bchunk = r.u32(d.start);
            const std::vector<Block> chans = blocks(r, d.start + bchunk, d.end);
            if (chans.empty() || !read_channels(r, chans, comp, depth, pal, hdr.greyscale, w, h, img, err)) continue;
        } else {
            continue;
        }
        Layer& L = doc.add_layer("Background");
        L.background = true;
        const int cw = std::min(img.width(), doc.width()), ch = std::min(img.height(), doc.height());
        for (int y = 0; y < ch; ++y)
            std::memcpy(L.pixels.data() + static_cast<size_t>(y) * doc.width() * 4,
                        img.data() + static_cast<size_t>(y) * img.width() * 4, static_cast<size_t>(cw) * 4);
        return true;
    }
    return false;
}

}  // namespace

std::unique_ptr<Document> load_psp_from_memory(const uint8_t* data, size_t size, std::string* err,
                                               std::vector<std::string>* warnings) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return std::unique_ptr<Document>(); };
    const Reader r{data, size};
    if (size < 36 || std::memcmp(data, kSignature, sizeof(kSignature) - 1) != 0) return fail("not a Paint Shop Pro image file");
    const uint16_t major = r.u16(32);
    if (major < 4) return fail("file version " + std::to_string(major) + ".x is too old (need 4.0 or later)");

    Header hdr;
    Palette pal;
    bool have_palette = false;
    const std::vector<Block> top = blocks(r, 36, size);
    const Block* layer_bank = nullptr;
    const Block* composite_bank = nullptr;
    bool have_header = false;
    for (const Block& b : top) {
        if (b.id == kImageBlock && r.ok(b.start, 42)) {
            hdr.width = r.i32(b.start + 4);
            hdr.height = r.i32(b.start + 8);
            hdr.compression = r.u16(b.start + 21);
            hdr.depth = r.u16(b.start + 23);
            hdr.greyscale = r.u8(b.start + 31) != 0;
            hdr.active_layer = r.i32(b.start + 36);
            have_header = true;
        } else if (b.id == kColorBlock && r.ok(b.start, 8)) {
            const size_t chunk = r.u32(b.start);
            const uint32_t count = r.u32(b.start + 4);
            if (r.ok(b.start + chunk, static_cast<size_t>(count) * 4)) {
                for (uint32_t i = 0; i < count && i < 256; ++i) {
                    const uint8_t* e = r.p + b.start + chunk + static_cast<size_t>(i) * 4;
                    pal.entries.push_back({e[2], e[1], e[0], 255});  // stored B, G, R, reserved
                }
                have_palette = true;
            }
        } else if (b.id == kLayerStartBlock) {
            layer_bank = &b;
        } else if (b.id == kCompositeBankBlock) {
            composite_bank = &b;
        }
    }
    if (!have_header) return fail("missing image attributes block");
    if (hdr.width <= 0 || hdr.height <= 0 || hdr.width > 65536 || hdr.height > 65536) return fail("bad image size");
    if (hdr.depth != 8 && hdr.depth != 24 && hdr.depth != 48)
        return fail("unsupported bit depth " + std::to_string(hdr.depth));

    auto doc = std::make_unique<Document>(hdr.width, hdr.height);
    std::string e;
    if (layer_bank) {
        // The bank holds layer blocks directly; older writers prefix a chunk.
        size_t start = layer_bank->start;
        if (r.ok(start, 4) && std::memcmp(r.p + start, "~BK\0", 4) != 0 && r.ok(start, 4)) start += r.u32(start);
        for (const Block& lb : blocks(r, start, layer_bank->end)) {
            if (lb.id != kLayerBlock) continue;
            if (!read_layer(r, lb, hdr, have_palette ? &pal : nullptr, *doc, warnings, e)) return fail(e);
        }
    }
    if (doc->layer_count() == 0) {
        if (composite_bank && read_composite(r, *composite_bank, hdr, have_palette ? &pal : nullptr, *doc, e)) {
            if (warnings) warnings->push_back("No raster layers; loaded the flattened composite instead");
        } else {
            return fail(e.empty() ? "no readable layers" : e);
        }
    }
    doc->set_active_layer(std::clamp(hdr.active_layer, 0, static_cast<int>(doc->layer_count()) - 1));
    return doc;
}

std::unique_ptr<Document> load_psp(const std::string& path, std::string* err, std::vector<std::string>* warnings) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot open " + path; return nullptr; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return load_psp_from_memory(data.data(), data.size(), err, warnings);
}

bool is_psp_extension(const std::string& path) {
    const auto dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == "pspimage" || ext == "psp" || ext == "psptube" || ext == "pspframe" || ext == "pspselection" || ext == "pspbrush";
}

std::unique_ptr<Document> load_document(const std::string& path, std::string* err, std::vector<std::string>* warnings) {
    if (is_psp_extension(path)) return load_psp(path, err, warnings);
    auto img = load(path, err);
    if (!img) return nullptr;
    auto doc = std::make_unique<Document>(img->width(), img->height());
    Layer& bg = doc->add_layer("Background");
    bg.background = true;
    bg.pixels = std::move(*img);
    return doc;
}

}  // namespace firn::io
