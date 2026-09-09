#include "firn/io_psp.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>

#include "firn/io.h"
#include "stb/stb_image.h"        // declarations only; the implementation lives in io.cpp
#include "stb/stb_image_write.h"  // in-memory JPEG

// Defined by the stb_image_write implementation in io.cpp but only declared
// inside its implementation section.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace firn::io {
namespace {

// Block ids (from the version 8 file format specification).
enum : uint16_t {
    kImageBlock = 0, kCreatorBlock = 1, kColorBlock = 2, kLayerStartBlock = 3, kLayerBlock = 4,
    kChannelBlock = 5, kSelectionBlock = 6, kCompositeImageBlock = 9, kCompositeBankBlock = 16,
    kCompositeAttrBlock = 17, kJpegBlock = 18, kGroupExtBlock = 25, kMaskExtBlock = 26, kTubeBlock = 11,
};
enum : uint16_t { kCompNone = 0, kCompRle = 1, kCompLz77 = 2, kCompJpeg = 3 };
// Bitmap (DIB) types. Layers use 0/1; thumbnails 5/6; composites 8/9.
enum : uint16_t { kDibImage = 0, kDibTransMask = 1, kDibUserMask = 2, kDibThumbnail = 5, kDibThumbnailTrans = 6, kDibComposite = 8, kDibCompositeTrans = 9 };
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
    bool grayscale = false;
    int active_layer = 0;
};

// Reads the channel sub-blocks of one raster bitmap into an RGBA image of
// (w x h). Missing color channels stay 0; missing alpha stays opaque.
bool read_channels(const Reader& r, const std::vector<Block>& subs, uint16_t comp, uint16_t depth,
                   const Palette* pal, bool gray, int w, int h, Image& out, std::string& err) {
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
            // Composite channel: palette index or gray level.
            for (size_t i = 0; i < npx; ++i) {
                const uint8_t v = bps == 2 ? data[i * 2 + 1] : data[i];
                Color c{v, v, v, 255};
                if (pal && !gray && v < pal->entries.size()) c = pal->entries[v];
                else if (pal && v < pal->entries.size()) c = pal->entries[v];
                px[i * 4 + 0] = c.r; px[i * 4 + 1] = c.g; px[i * 4 + 2] = c.b;
            }
        }
    }
    return true;
}

BlendMode map_blend(uint8_t v) {
    if (v <= 16) return static_cast<BlendMode>(v);
    if (v >= 17 && v <= 20) return static_cast<BlendMode>(v - 14);  // "True" hue/sat/color/lightness
    return BlendMode::Normal;
}

// Group state while walking the layer bank: children follow their group
// block; a mask layer applies to the layers below it in the same group (or
// every layer below it at top level).
struct GroupCtx {
    int remaining = 0;          // children still to read
    size_t group_index = 0;     // document index of the Group layer
};

struct BankCtx {
    std::vector<GroupCtx> groups;
};

// Reads a mask layer's single channel into a document-sized mask; pixels
// outside the saved mask rect take the extension block's `outside` value.
bool read_mask_layer(const Reader& r, const Block& lb, size_t info_end, const Header& hdr, const int32_t mask_rect[4],
                     const int32_t saved_mask[4], int W, int H, Mask& out, std::string& err) {
    const std::vector<Block> subs = blocks(r, info_end, lb.end);
    uint8_t outside = 255;
    size_t bitmap_at = info_end;
    for (const Block& b : subs) {
        if (b.id == kMaskExtBlock && r.ok(b.start, 8)) { outside = static_cast<uint8_t>(std::min<uint32_t>(r.u32(b.start + 4), 255)); bitmap_at = b.end; }
    }
    if (!r.ok(bitmap_at, 8)) return true;
    const size_t bchunk = r.u32(bitmap_at);
    const std::vector<Block> chans = blocks(r, bitmap_at + bchunk, lb.end);
    const int sw = saved_mask[2] - saved_mask[0], sh = saved_mask[3] - saved_mask[1];
    const int ox = mask_rect[0] + saved_mask[0], oy = mask_rect[1] + saved_mask[1];
    std::vector<uint8_t> tile;
    for (const Block& b : chans) {
        if (b.id != kChannelBlock || !r.ok(b.start, 16)) continue;
        const size_t chunk = r.u32(b.start), clen = r.u32(b.start + 4);
        if (r.u16(b.start + 12) != kDibUserMask) continue;
        if (!r.ok(b.start + chunk, clen) || sw <= 0 || sh <= 0) continue;
        if (!decompress(hdr.compression, r.p + b.start + chunk, clen, static_cast<size_t>(sw) * sh, tile, err)) return false;
        break;
    }
    out = Mask(W, H, outside);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int tx = x - ox, ty = y - oy;
            if (!tile.empty() && tx >= 0 && ty >= 0 && tx < sw && ty < sh) out.at(x, y) = tile[static_cast<size_t>(ty) * sw + tx];
        }
    return true;
}

bool read_layer(const Reader& r, const Block& lb, const Header& hdr, const Palette* pal, Document& doc,
                BankCtx& ctx, std::vector<std::string>* warnings, std::string& err) {
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
    o += 5;  // opacity, blend, visible, protected, link group
    const int32_t mask_rect[4] = {r.i32(o), r.i32(o + 4), r.i32(o + 8), r.i32(o + 12)}; o += 16;
    const int32_t saved_mask[4] = {r.i32(o), r.i32(o + 4), r.i32(o + 8), r.i32(o + 12)}; o += 16;
    const bool mask_disabled = r.ok(o, 2) && r.u8(o + 1) != 0;

    // Group bookkeeping: this layer is a child of the innermost open group.
    while (!ctx.groups.empty() && ctx.groups.back().remaining == 0) ctx.groups.pop_back();
    GroupCtx* group = ctx.groups.empty() ? nullptr : &ctx.groups.back();
    if (group) --group->remaining;
    const int depth = static_cast<int>(ctx.groups.size());

    static const char* kTypeNames[] = {"undefined", "raster", "floating selection", "vector", "adjustment", "group", "mask", "art media"};
    if (type == kLayerGroup) {
        Layer& G = doc.add_layer(name);
        G.type = LayerType::Group;
        G.pixels = Image();
        G.depth = depth;
        G.opacity = opacity / 255.0f;
        G.blend = map_blend(blend);
        G.visible = visible != 0;
        GroupCtx g;
        g.group_index = doc.layer_count() - 1;
        for (const Block& b : blocks(r, lb.start + chunk, lb.end))
            if (b.id == kGroupExtBlock && r.ok(b.start, 8)) g.remaining = static_cast<int>(r.u32(b.start + 4));
        ctx.groups.push_back(g);
        return true;
    }
    if (type == kLayerMask) {
        // A mask inside a group masks the group; at top level it masks the
        // layer directly beneath it.
        Mask m;
        if (!read_mask_layer(r, lb, lb.start + chunk, hdr, mask_rect, saved_mask, doc.width(), doc.height(), m, err)) return false;
        int target = group ? static_cast<int>(group->group_index) : static_cast<int>(doc.layer_count()) - 1;
        if (target < 0) { if (warnings) warnings->push_back("Skipped mask \"" + name + "\" with nothing to mask"); return true; }
        Layer& T = doc.layer(target);
        T.mask = std::move(m);
        T.mask_enabled = visible != 0 && !mask_disabled;
        return true;
    }
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
    L.depth = depth;
    L.opacity = opacity / 255.0f;
    L.blend = map_blend(blend);
    L.visible = visible != 0;
    if (sw <= 0 || sh <= 0) return true;  // empty layer

    Image tile;
    if (!read_channels(r, subs, hdr.compression, hdr.depth, pal, hdr.grayscale, sw, sh, tile, err)) return false;
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
                    std::string& err, bool allow_jpeg = true) {
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
            if (!allow_jpeg) continue;
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
            if (chans.empty() || !read_channels(r, chans, comp, depth, pal, hdr.grayscale, w, h, img, err)) continue;
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
            hdr.grayscale = r.u8(b.start + 31) != 0;
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
        BankCtx ctx;
        for (const Block& lb : blocks(r, start, layer_bank->end)) {
            if (lb.id != kLayerBlock) continue;
            if (!read_layer(r, lb, hdr, have_palette ? &pal : nullptr, *doc, ctx, warnings, e)) return fail(e);
        }
        // The original expresses "a layer with a mask" as a group holding
        // that one layer plus a mask layer. Collapse those back to a masked
        // layer so the palette shows what the user made.
        {
            std::vector<Layer> layers = doc->clone_layers();
            auto group_end_of = [&](size_t g) {
                size_t j = g + 1;
                while (j < layers.size() && layers[j].depth > layers[g].depth) ++j;
                return j;
            };
            for (size_t i = 0; i < layers.size(); ++i) {
                if (layers[i].type != LayerType::Group || !layers[i].has_mask()) continue;
                const size_t end = group_end_of(i);
                if (end != i + 2 || !layers[i + 1].is_raster() || layers[i + 1].has_mask()) continue;
                Layer merged = layers[i + 1];
                merged.depth = layers[i].depth;
                merged.mask = std::move(layers[i].mask);
                merged.mask_enabled = layers[i].mask_enabled;
                merged.visible = merged.visible && layers[i].visible;
                merged.opacity *= layers[i].opacity;
                layers.erase(layers.begin() + i + 1);
                layers[i] = std::move(merged);
            }
            if (layers.size() != doc->layer_count()) doc->replace_layers(layers, 0);
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

std::optional<TubeInfo> load_psp_tube_info(const uint8_t* data, size_t size) {
    const Reader r{data, size};
    if (size < 36 || std::memcmp(data, kSignature, sizeof(kSignature) - 1) != 0) return std::nullopt;
    for (const Block& b : blocks(r, 36, size)) {
        if (b.id != kTubeBlock || !r.ok(b.start, 30)) continue;
        // chunk_len u32, u16 (0 in every sample), step u32, columns u32, rows u32,
        // total cells u32, placement mode u32, selection mode u32.
        TubeInfo t;
        t.step = r.i32(b.start + 6);
        t.columns = std::max(1, r.i32(b.start + 10));
        t.rows = std::max(1, r.i32(b.start + 14));
        t.total = std::clamp(r.i32(b.start + 18), 1, t.columns * t.rows);
        t.placement = r.i32(b.start + 22);
        t.selection = r.i32(b.start + 26);
        return t;
    }
    return std::nullopt;
}

std::optional<TubeInfo> load_psp_tube_info(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return load_psp_tube_info(data.data(), data.size());
}

std::optional<Image> load_psp_stored_composite(const uint8_t* data, size_t size) {
    const Reader r{data, size};
    if (size < 36 || std::memcmp(data, kSignature, sizeof(kSignature) - 1) != 0) return std::nullopt;
    Header hdr;
    Palette pal;
    bool have_palette = false;
    const Block* bank = nullptr;
    for (const Block& b : blocks(r, 36, size)) {
        if (b.id == kImageBlock && r.ok(b.start, 42)) {
            hdr.width = r.i32(b.start + 4); hdr.height = r.i32(b.start + 8);
            hdr.compression = r.u16(b.start + 21); hdr.depth = r.u16(b.start + 23); hdr.grayscale = r.u8(b.start + 31) != 0;
        } else if (b.id == kColorBlock && r.ok(b.start, 8)) {
            const size_t chunk = r.u32(b.start);
            const uint32_t count = r.u32(b.start + 4);
            if (r.ok(b.start + chunk, static_cast<size_t>(count) * 4)) {
                for (uint32_t i = 0; i < count && i < 256; ++i) {
                    const uint8_t* e = r.p + b.start + chunk + static_cast<size_t>(i) * 4;
                    pal.entries.push_back({e[2], e[1], e[0], 255});
                }
                have_palette = true;
            }
        } else if (b.id == kCompositeBankBlock) bank = &b;
    }
    if (!bank || hdr.width <= 0) return std::nullopt;
    Document tmp(hdr.width, hdr.height);
    std::string err;
    if (!read_composite(r, *bank, hdr, have_palette ? &pal : nullptr, tmp, err, /*allow_jpeg=*/false) || tmp.layer_count() == 0) return std::nullopt;
    return tmp.layer(0).pixels;
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

// --- Writer --------------------------------------------------------------

namespace {

struct Writer {
    std::vector<uint8_t> out;
    void u8(int v) { out.push_back(static_cast<uint8_t>(v)); }
    void u16(int v) { u8(v & 255); u8((v >> 8) & 255); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) u8((v >> (8 * i)) & 255); }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void f64(double v) { uint8_t b[8]; std::memcpy(b, &v, 8); out.insert(out.end(), b, b + 8); }
    void bytes(const std::vector<uint8_t>& b) { out.insert(out.end(), b.begin(), b.end()); }
    void bytes(const uint8_t* b, size_t n) { out.insert(out.end(), b, b + n); }
    void block(uint16_t id, const std::vector<uint8_t>& payload) {
        out.insert(out.end(), {'~', 'B', 'K', 0});
        u16(id);
        u32(static_cast<uint32_t>(payload.size()));
        bytes(payload);
    }
    // "~FL" field inside creator / extended data blocks.
    void field(uint16_t id, const std::vector<uint8_t>& payload) {
        out.insert(out.end(), {'~', 'F', 'L', 0});
        u16(id);
        u32(static_cast<uint32_t>(payload.size()));
        bytes(payload);
    }
};

std::vector<uint8_t> zlib_compress(const std::vector<uint8_t>& data) {
    int len = 0;
    unsigned char* z = stbi_zlib_compress(const_cast<unsigned char*>(data.data()), static_cast<int>(data.size()), &len, 8);
    std::vector<uint8_t> out(z, z + len);
    free(z);
    return out;
}

// One channel block: `plane` is sw*sh bytes.
std::vector<uint8_t> channel_block(uint16_t dib_type, uint16_t channel_type, const std::vector<uint8_t>& plane, size_t nominal_len) {
    Writer w;
    const std::vector<uint8_t> z = zlib_compress(plane);
    Writer payload;
    payload.u32(16);
    payload.u32(static_cast<uint32_t>(z.size()));
    payload.u32(static_cast<uint32_t>(nominal_len));
    payload.u16(dib_type);
    payload.u16(channel_type);
    payload.bytes(z);
    w.block(kChannelBlock, payload.out);
    return w.out;
}

// Splits an RGBA tile into planes and writes bitmap chunk + channel blocks.
// `dib_image` / `dib_trans` select layer (0/1) or composite (8/9) types.
std::vector<uint8_t> bitmap_and_channels(const Image& tile, bool with_alpha, uint16_t dib_image, uint16_t dib_trans) {
    const size_t npx = static_cast<size_t>(tile.width()) * tile.height();
    std::vector<uint8_t> planes[4];
    for (auto& p : planes) p.resize(npx);
    const uint8_t* s = tile.data();
    for (size_t i = 0; i < npx; ++i)
        for (int c = 0; c < 4; ++c) planes[c][i] = s[i * 4 + c];
    Writer w;
    w.u32(8);
    w.u16(with_alpha ? 2 : 1);
    w.u16(with_alpha ? 4 : 3);
    const size_t padded = (static_cast<size_t>(tile.width()) + 3) / 4 * 4 * tile.height();
    for (int c = 0; c < 3; ++c) w.bytes(channel_block(dib_image, static_cast<uint16_t>(c + 1), planes[c], padded * 3));
    if (with_alpha) w.bytes(channel_block(dib_trans, 0, planes[3], padded));
    return w.out;
}

raster::Rect content_bounds(const Image& img) {
    raster::Rect r{img.width(), img.height(), 0, 0};
    const uint8_t* p = img.data();
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (p[(static_cast<size_t>(y) * img.width() + x) * 4 + 3]) {
                r.x0 = std::min(r.x0, x); r.x1 = std::max(r.x1, x + 1);
                r.y0 = std::min(r.y0, y); r.y1 = y + 1;
            }
    return r.empty() ? raster::Rect{} : r;
}

bool any_transparency(const Image& img) {
    const uint8_t* p = img.data();
    for (size_t i = 3; i < img.size_bytes(); i += 4)
        if (p[i] != 255) return true;
    return false;
}

// Trailing bytes of a version 6 layer info chunk: invert-mask flag, blend
// range count, five default blend ranges. Identical in every sample file.
const uint8_t kLayerInfoTail[43] = {
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
    0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
    0xff, 0xff, 0x00};

// Layer info chunk shared by raster, group and mask layer blocks.
std::vector<uint8_t> layer_info(const std::string& raw_name, uint8_t type, const int32_t rect[4], const int32_t saved[4],
                                float opacity, BlendMode blend, bool visible, const int32_t mask_rect[4], const int32_t saved_mask[4],
                                bool mask_disabled) {
    Writer info;
    std::string name = raw_name.substr(0, 255);
    info.u32(0);  // chunk length, patched below
    info.u16(static_cast<int>(name.size()));
    info.bytes(reinterpret_cast<const uint8_t*>(name.data()), name.size());
    info.u8(type);
    for (int i = 0; i < 4; ++i) info.i32(rect[i]);
    for (int i = 0; i < 4; ++i) info.i32(saved[i]);
    info.u8(static_cast<int>(std::clamp(opacity, 0.0f, 1.0f) * 255.0f + 0.5f));
    info.u8(static_cast<int>(blend));
    info.u8(visible ? 1 : 0);
    info.u8(0);  // transparency protected
    info.u8(0);  // link group
    for (int i = 0; i < 4; ++i) info.i32(mask_rect[i]);
    for (int i = 0; i < 4; ++i) info.i32(saved_mask[i]);
    info.u8(0); info.u8(mask_disabled ? 1 : 0);  // mask linked, mask disabled
    info.bytes(kLayerInfoTail, sizeof(kLayerInfoTail));
    const uint32_t len = static_cast<uint32_t>(info.out.size());
    for (int i = 0; i < 4; ++i) info.out[i] = static_cast<uint8_t>((len >> (8 * i)) & 255);
    return info.out;
}

const int32_t kZeroRect[4] = {0, 0, 0, 0};

std::vector<uint8_t> group_block(const Layer& G, uint32_t member_count) {
    Writer payload;
    payload.bytes(layer_info(G.name, kLayerGroup, kZeroRect, kZeroRect, G.opacity, G.blend, G.visible, kZeroRect, kZeroRect, false));
    Writer ext;
    ext.u32(9); ext.u32(member_count); ext.u8(0);
    payload.block(kGroupExtBlock, ext.out);
    payload.u32(8); payload.u16(0); payload.u16(0);  // empty bitmap chunk, as the original writes
    Writer w;
    w.block(kLayerBlock, payload.out);
    return w.out;
}

std::vector<uint8_t> mask_block(const std::string& owner_name, const Mask& m, bool enabled, int doc_w, int doc_h) {
    const int32_t full[4] = {0, 0, doc_w, doc_h};
    Writer payload;
    payload.bytes(layer_info("Mask - " + owner_name, kLayerMask, kZeroRect, kZeroRect, 1.0f, BlendMode::Normal, true, full, full, !enabled));
    Writer ext;
    ext.u32(9); ext.u32(255); ext.u8(0x32);
    payload.block(kMaskExtBlock, ext.out);
    payload.u32(8); payload.u16(1); payload.u16(1);
    std::vector<uint8_t> plane(m.data(), m.data() + m.size());
    payload.bytes(channel_block(kDibUserMask, 0, plane, (static_cast<size_t>(doc_w) + 3) / 4 * 4 * doc_h));
    Writer w;
    w.block(kLayerBlock, payload.out);
    return w.out;
}

std::vector<uint8_t> layer_block(const Layer& L, int doc_w, int doc_h) {
    const int32_t rect[4] = {0, 0, doc_w, doc_h};
    // Only a Background layer omits the transparency channel; the original
    // writes one for every other layer even when it is fully opaque, and the
    // reader relies on that to tell them apart.
    const bool with_alpha = !L.background;
    raster::Rect saved = with_alpha ? content_bounds(L.pixels) : raster::Rect{0, 0, doc_w, doc_h};
    const int32_t saved_rect[4] = {saved.x0, saved.y0, saved.x1, saved.y1};
    Writer payload;
    payload.bytes(layer_info(L.name, kLayerRaster, rect, saved_rect, L.opacity, L.blend, L.visible, kZeroRect, kZeroRect, false));
    if (!saved.empty()) {
        Image tile = raster::crop(L.pixels, saved);
        if (!with_alpha) {  // opaque layer: drop alpha so readers see a solid Background
            uint8_t* p = tile.data();
            for (size_t i = 3; i < tile.size_bytes(); i += 4) p[i] = 255;
        }
        payload.bytes(bitmap_and_channels(tile, with_alpha, kDibImage, kDibTransMask));
    } else {
        payload.u32(8); payload.u16(2); payload.u16(4);  // empty layer: no channel data
    }
    Writer w;
    w.block(kLayerBlock, payload.out);
    return w.out;
}

void jpeg_sink(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
}

std::vector<uint8_t> composite_bank(const Image& flat) {
    // Thumbnail: JPEG, longest side 200, flattened onto white.
    const int tw = flat.width() >= flat.height() ? 200 : std::max(1, 200 * flat.width() / flat.height());
    const int th = flat.width() >= flat.height() ? std::max(1, 200 * flat.height() / flat.width()) : 200;
    Image thumb = raster::resample(flat, std::min(tw, flat.width()), std::min(th, flat.height()), raster::Filter::Bilinear);
    std::vector<uint8_t> rgb(static_cast<size_t>(thumb.width()) * thumb.height() * 3);
    for (size_t i = 0; i < rgb.size() / 3; ++i) {
        const uint8_t* s = thumb.data() + i * 4;
        const int a = s[3];
        for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = static_cast<uint8_t>((s[c] * a + 255 * (255 - a) + 127) / 255);
    }
    std::vector<uint8_t> jpg;
    stbi_write_jpg_to_func(jpeg_sink, &jpg, thumb.width(), thumb.height(), 3, rgb.data(), 85);

    Writer attr_thumb;
    attr_thumb.u32(24); attr_thumb.i32(thumb.width()); attr_thumb.i32(thumb.height()); attr_thumb.u16(24); attr_thumb.u16(kCompJpeg);
    attr_thumb.u16(1); attr_thumb.u32(16777216); attr_thumb.u16(1);
    Writer attr_full;
    attr_full.u32(24); attr_full.i32(flat.width()); attr_full.i32(flat.height()); attr_full.u16(24); attr_full.u16(kCompLz77);
    attr_full.u16(1); attr_full.u32(16777216); attr_full.u16(0);
    Writer jpeg_payload;
    jpeg_payload.u32(14); jpeg_payload.u32(static_cast<uint32_t>(jpg.size())); jpeg_payload.u32(0); jpeg_payload.u16(5);
    jpeg_payload.bytes(jpg);

    Writer bank;
    bank.u32(8); bank.u32(2);
    bank.block(kCompositeAttrBlock, attr_thumb.out);
    bank.block(kCompositeAttrBlock, attr_full.out);
    bank.block(kJpegBlock, jpeg_payload.out);
    bank.block(kCompositeImageBlock, bitmap_and_channels(flat, any_transparency(flat), kDibComposite, kDibCompositeTrans));
    Writer w;
    w.block(kCompositeBankBlock, bank.out);
    return w.out;
}

}  // namespace

std::vector<uint8_t> save_psp_to_memory(const Document& doc) {
    Writer w;
    w.bytes(reinterpret_cast<const uint8_t*>(kSignature), sizeof(kSignature) - 1);
    while (w.out.size() < 32) w.u8(0);
    w.u16(6); w.u16(0);

    const Image flat = doc.composite();
    const bool single_opaque = doc.layer_count() == 1 && doc.layer(0).background;
    uint32_t contents = 0x00000001u | 0x01000000u | 0x04000000u;  // raster layers, thumbnail, composite
    if (single_opaque) contents |= 0x10000000u;                    // flat image
    if (any_transparency(flat)) contents |= 0x08000000u;          // composite transparency

    // Layer bank first: the header needs the number of layer blocks written,
    // which exceeds the document's layer count when groups and masks are
    // expanded into their own blocks.
    Writer bank;
    uint32_t block_count = 0;
    bool has_groups = false, has_masks = false;
    {
        size_t i = 0;
        while (i < doc.layer_count()) {
            const Layer& L = doc.layer(i);
            if (L.type == LayerType::Group) {
                const size_t end = doc.group_end(i);
                uint32_t members = 0;
                for (size_t j = i + 1; j < end;) {
                    ++members;
                    const Layer& M = doc.layer(j);
                    j = M.type == LayerType::Group ? doc.group_end(j) : j + 1;
                }
                if (L.has_mask()) ++members;
                bank.bytes(group_block(L, members));
                ++block_count; has_groups = true;
                ++i;
                continue;
            }
            if (L.has_mask()) {
                Layer g = L;
                g.type = LayerType::Group;
                g.mask = Mask();
                bank.bytes(group_block(g, 2));
                Layer plain = L;
                plain.opacity = 1.0f; plain.blend = BlendMode::Normal; plain.visible = true;
                bank.bytes(layer_block(plain, doc.width(), doc.height()));
                bank.bytes(mask_block(L.name, L.mask, L.mask_enabled, doc.width(), doc.height()));
                block_count += 3; has_groups = true; has_masks = true;
            } else {
                bank.bytes(layer_block(L, doc.width(), doc.height()));
                ++block_count;
            }
            ++i;
            // Emit the mask layer of any group that has just closed at this index.
            for (int g = doc.parent_group(i - 1); g >= 0; g = doc.parent_group(g)) {
                if (doc.group_end(g) != i) break;
                if (doc.layer(g).has_mask()) {
                    bank.bytes(mask_block(doc.layer(g).name, doc.layer(g).mask, doc.layer(g).mask_enabled, doc.width(), doc.height()));
                    ++block_count; has_masks = true;
                }
            }
        }
    }
    if (has_groups) contents |= 0x00000008u;  // group layers
    if (has_masks) contents |= 0x00000010u;   // mask layers

    Writer img;
    img.u32(46); img.i32(doc.width()); img.i32(doc.height()); img.f64(72.0); img.u8(1);
    img.u16(kCompLz77); img.u16(24); img.u16(1); img.u32(16777216); img.u8(0);
    img.u32(static_cast<uint32_t>(doc.width()) * doc.height() * 3);
    img.i32(std::max(0, doc.active_layer())); img.u16(static_cast<int>(block_count)); img.u32(contents);
    w.block(kImageBlock, img.out);

    Writer creator;
    const uint32_t now = static_cast<uint32_t>(std::time(nullptr));
    Writer t; t.u32(now);
    creator.field(1, t.out);  // created
    creator.field(2, t.out);  // modified
    Writer app; app.u32(1);
    creator.field(6, app.out);  // application id
    Writer ver; ver.u32(0x08000001u);
    creator.field(7, ver.out);  // application version
    w.block(kCreatorBlock, creator.out);

    w.bytes(composite_bank(flat));

    w.block(kLayerStartBlock, bank.out);
    return w.out;
}

bool save_psp(const Document& doc, const std::string& path, std::string* err) {
    const std::vector<uint8_t> data = save_psp_to_memory(doc);
    std::ofstream f(path, std::ios::binary);
    if (!f || !f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
        if (err) *err = "cannot write " + path;
        return false;
    }
    return true;
}

bool save_document(const Document& doc, const std::string& path, std::string* err, int jpeg_quality) {
    if (is_psp_extension(path)) return save_psp(doc, path, err);
    return save(doc.composite(), path, err, jpeg_quality);
}

}  // namespace firn::io
