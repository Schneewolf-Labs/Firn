#include "firn/adjust.h"
#include "firn/io_psp.h"
#include "firn/json.h"
#include "firn/raster16.h"
#include "firn/text.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <functional>
#include <future>
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
    kAlphaBankBlock = 7, kAlphaChannelBlock = 8, kAdjustmentExtBlock = 12, kVectorExtBlock = 13, kShapeBlock = 14, kPaintStyleBlock = 15, kLineStyleBlock = 19,

};
enum : uint16_t { kCompNone = 0, kCompRle = 1, kCompLz77 = 2, kCompJpeg = 3 };
// Bitmap (DIB) types. Layers use 0/1; thumbnails 5/6; composites 8/9.
enum : uint16_t { kDibImage = 0, kDibTransMask = 1, kDibUserMask = 2, kDibAlphaMask = 4, kDibThumbnail = 5, kDibThumbnailTrans = 6, kDibAdjustment = 7, kDibComposite = 8, kDibCompositeTrans = 9 };
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

// 16-bit sample to its 8-bit mirror, rounded like `to_image8`.
inline uint8_t sample8(int v) { return static_cast<uint8_t>((v + 128) / 257); }

// Reads the channel sub-blocks of one raster bitmap into an RGBA image of
// (w x h). Missing color channels stay 0; missing alpha stays opaque.
// 16-bit samples are kept when `deep` is given (48-bit files).
bool read_channels(const Reader& r, const std::vector<Block>& subs, uint16_t comp, uint16_t depth,
                   const Palette* pal, bool gray, int w, int h, Image& out, std::string& err, Image16* deep = nullptr) {
    out = Image(w, h, {0, 0, 0, 255});
    if (deep && depth == 48) *deep = Image16(w, h, 0, 0, 0, 65535);
    else deep = nullptr;
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

        // In 48-bit files the transparency mask is 16-bit too (that is how
        // GIMP's reader, written against real files, treats it); an 8-bit
        // mask is accepted when the data is only large enough for one.
        int bps = bytes_per_sample;
        std::vector<uint8_t> data;
        if (is_trans_dib(bitmap_type) && bps == 2 && !decompress(comp, r.p + data_at, clen, npx * 2, data, err)) bps = 1;
        if (bps == 1 || !is_trans_dib(bitmap_type))
            if (!decompress(comp, r.p + data_at, clen, npx * bps, data, err)) return false;

        uint8_t* px = out.data();
        if (is_trans_dib(bitmap_type)) {
            for (size_t i = 0; i < npx; ++i) px[i * 4 + 3] = bps == 2 ? sample8(data[i * 2] | (data[i * 2 + 1] << 8)) : data[i];
            if (deep) for (size_t i = 0; i < npx; ++i) deep->data()[i * 4 + 3] = bps == 2 ? static_cast<uint16_t>(data[i * 2] | (data[i * 2 + 1] << 8)) : static_cast<uint16_t>(data[i] * 257);
        } else if (channel_type >= 1 && channel_type <= 3) {
            const int c = channel_type - 1;
            for (size_t i = 0; i < npx; ++i) px[i * 4 + c] = bps == 2 ? sample8(data[i * 2] | (data[i * 2 + 1] << 8)) : data[i];
            if (deep && bps == 2) for (size_t i = 0; i < npx; ++i) deep->data()[i * 4 + c] = static_cast<uint16_t>(data[i * 2] | (data[i * 2 + 1] << 8));
        } else {
            // Composite channel: palette index or gray level.
            for (size_t i = 0; i < npx; ++i) {
                const uint8_t v = bps == 2 ? sample8(data[i * 2] | (data[i * 2 + 1] << 8)) : data[i];
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
// Adjustment layer extension (docs/FORMAT.md): info chunk {len, type u16}
// then one definition chunk whose layout depends on the type.
void read_adjustment(const Reader& r, const Block& b, Adjustment& a) {
    if (!r.ok(b.start, 6)) return;
    size_t p = b.start;
    const size_t c0 = r.u32(p);
    a.kind = static_cast<Adjustment::Kind>(r.u16(p + 4));
    p += c0;
    if (!r.ok(p, 4)) return;
    const size_t len = r.u32(p);
    auto f64 = [&](size_t off) { double v; std::memcpy(&v, r.p + off, 8); return v; };
    auto i32 = [&](size_t off) { return r.i32(off); };
    switch (a.kind) {
        case Adjustment::Kind::Levels:
            if (len >= 4 + 32 + 64) {
                for (int c = 0; c < 4; ++c) {
                    Adjustment::Levels& l = a.levels[c];
                    l.gamma = static_cast<float>(f64(p + 4 + c * 8));
                    l.in_high = i32(p + 36 + c * 4); l.in_low = i32(p + 52 + c * 4);
                    l.out_high = i32(p + 68 + c * 4); l.out_low = i32(p + 84 + c * 4);
                }
            }
            break;
        case Adjustment::Kind::Curves: {
            // Four chunks: RGB, red, green, blue. {len, freehand u8, count u16, 18 x (in, out), 256-byte table}.
            size_t q = p;
            for (int c = 0; c < 4 && r.ok(q, 4); ++c) {
                const size_t cl = r.u32(q);
                if (cl < 7 || !r.ok(q, cl)) break;
                const bool freehand = r.u8(q + 4) != 0;
                const int count = std::min<int>(r.u16(q + 5), 18);
                a.curves[c].clear();
                if (freehand && cl >= 4 + 3 + 36 + 256) {
                    for (int i = 0; i < 256; i += 15) a.curves[c].emplace_back(static_cast<float>(i), static_cast<float>(r.u8(q + 43 + i)));
                    a.curves[c].emplace_back(255.0f, static_cast<float>(r.u8(q + 43 + 255)));
                } else {
                    for (int i = 0; i < count && q + 7 + i * 2 + 1 < q + cl; ++i)
                        a.curves[c].emplace_back(static_cast<float>(r.u8(q + 7 + i * 2)), static_cast<float>(r.u8(q + 8 + i * 2)));
                }
                if (a.curves[c].size() < 2) a.curves[c] = {{0.0f, 0.0f}, {255.0f, 255.0f}};
                q += cl;
            }
            break;
        }
        case Adjustment::Kind::BrightnessContrast:
            if (len >= 12) { a.brightness = i32(p + 4); a.contrast = i32(p + 8); }
            break;
        case Adjustment::Kind::ColorBalance:
            if (len >= 5 + 36) {
                a.color_balance.preserve_luminosity = r.u8(p + 4) != 0;
                for (int i = 0; i < 3; ++i) {
                    a.color_balance.highlights[i] = i32(p + 5 + i * 4);
                    a.color_balance.midtones[i] = i32(p + 17 + i * 4);
                    a.color_balance.shadows[i] = i32(p + 29 + i * 4);
                }
            }
            break;
        case Adjustment::Kind::HSL:
            if (len >= 5 + 24) {
                a.colorize = r.u8(p + 4) != 0;
                a.hue = i32(p + 5); a.saturation = i32(p + 9); a.lightness = i32(p + 13);
                a.colorize_hue = i32(p + 17); a.colorize_saturation = i32(p + 21);
                for (int rng = 0; rng < 6; ++rng)
                    for (int k = 0; k < 7; ++k) {
                        const size_t off = p + 29 + (rng * 7 + k) * 4;
                        if (off + 4 <= p + len) a.hsl_ranges[rng][k] = i32(off);
                    }
            }
            break;
        case Adjustment::Kind::ChannelMixer:
            if (len >= 5 + 48) {
                a.mixer.monochrome = r.u8(p + 4) != 0;
                // Stored blue, green, red rows; each red, green, blue, constant.
                for (int row = 0; row < 3; ++row) {
                    const int out = 2 - row;
                    for (int i = 0; i < 3; ++i) a.mixer.mix[out][i] = static_cast<float>(i32(p + 5 + (row * 4 + i) * 4));
                    a.mixer.constant[out] = static_cast<float>(i32(p + 5 + (row * 4 + 3) * 4));
                }
            }
            break;
        case Adjustment::Kind::Threshold: if (len >= 8) a.threshold = i32(p + 4); break;
        case Adjustment::Kind::Posterize: if (len >= 8) a.posterize = i32(p + 4); break;
        default: break;
    }
}

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


// --- Vector shapes -----------------------------------------------------------
// Layout (docs/FORMAT.md): the vector extension block holds a chunk
// {8, shape count} followed by shape blocks. A shape block is a sequence of
// chunks and sub-blocks: info chunk {name, type u16 (2 path, 5 group), a u32,
// shape id u32, c u32}, attribute chunk (60 bytes: u8 stroke on, u8 fill on,
// u8 antialias, stroke width f64, two {u8, u8, f64, f64} records, u8, miter
// f64), paint style block (stroke), paint style block (fill), line style
// block, then per path {8, node count} and 55-byte nodes.

namespace {
template <class R> float rd_f64(const R& r, size_t o) { double d; std::memcpy(&d, r.p + o, 8); return static_cast<float>(d); }

bool read_paint_style(const Reader& r, const Block& b, vec::PaintStyle& out) {
    size_t p = b.start;
    if (!r.ok(p, 6)) return false;
    const size_t c0 = r.u32(p);
    out.kind = static_cast<vec::PaintStyle::Kind>(r.u16(p + 4));
    p += c0;
    if (out.kind == vec::PaintStyle::Kind::Solid) {
        if (r.ok(p, 8) && p + r.u32(p) <= b.end) out.color = {r.u8(p + 4), r.u8(p + 5), r.u8(p + 6), 255};
    } else if (out.kind == vec::PaintStyle::Kind::Gradient) {
        vec::Gradient& g = out.gradient;
        g.colors.clear();
        g.opacities.clear();
        if (r.ok(p, 4)) {
            const size_t cl = r.u32(p);
            // Payload: style u16, u32 (0xffffffff), invert u8, center x u32, center y
            // u32, angle f64, repeats u32, color stop count u16, opacity stop count u16.
            if (cl >= 35 && r.ok(p, cl)) {
                g.style = static_cast<vec::GradientStyle>(r.u16(p + 4));
                g.invert = r.u8(p + 10) != 0;
                g.center_x = static_cast<float>(r.u32(p + 11));
                g.center_y = static_cast<float>(r.u32(p + 15));
                g.angle = rd_f64(r, p + 19);
                g.repeats = static_cast<int>(r.u32(p + 27));
            }
            p += cl;
        }
        while (r.ok(p, 4) && p + r.u32(p) <= b.end) {
            const size_t cl = r.u32(p);
            if (cl == 12) g.colors.push_back({{r.u8(p + 4), r.u8(p + 5), r.u8(p + 6), 255}, static_cast<float>(r.u16(p + 8)), static_cast<float>(r.u16(p + 10))});
            else if (cl == 9) g.opacities.push_back({static_cast<float>(r.u8(p + 4)), static_cast<float>(r.u16(p + 5)), static_cast<float>(r.u16(p + 7))});
            else if (cl < 4) break;
            p += cl;
        }
        if (g.colors.empty()) g.colors = {{{0, 0, 0, 255}, 0, 50}, {{255, 255, 255, 255}, 100, 50}};
        if (g.opacities.empty()) g.opacities = {{100, 0, 50}, {100, 100, 50}};
    }
    return true;
}

// UTF-8 helpers for the text shape's character elements.
void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
}

std::vector<uint32_t> decode_utf8(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp; int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += n;
    }
    return out;
}

// Finds a font file for a family name and style flags; falls back to a
// common sans face so text from files without our fonts still shows.
std::string font_file_for(const std::string& family, bool bold, bool italic) {
    static const std::vector<text::FontInfo> fonts = text::list_fonts();
    auto lower = [](std::string v) { for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return v; };
    const std::string want = lower(family);
    auto style_ok = [&](const text::FontInfo& f) {
        const std::string st = lower(f.style);
        return (st.find("bold") != std::string::npos) == bold && (st.find("italic") != std::string::npos || st.find("oblique") != std::string::npos) == italic;
    };
    for (const auto& f : fonts) if (lower(f.family) == want && style_ok(f)) return f.path;
    for (const auto& f : fonts) if (lower(f.family) == want) return f.path;
    static const char* const fallbacks[] = {"DejaVu Sans", "Liberation Sans", "Arial", "Noto Sans", "Helvetica"};
    for (const char* fb : fallbacks) for (const auto& f : fonts) if (f.family == fb && style_ok(f)) return f.path;
    for (const char* fb : fallbacks) for (const auto& f : fonts) if (f.family == fb) return f.path;
    return fonts.empty() ? "" : fonts.front().path;
}

// Text Vector Shape Definition (spec section 5): attributes (alignment,
// insert point, 3x3 matrix, flow), then character-style and character
// elements. The file carries no outlines; they are laid out again from the font.
bool read_text_shape(const Reader& r, size_t p, size_t end, vec::Object& o) {
    if (!r.ok(p, 4)) return false;
    const size_t ta = r.u32(p);
    if (ta < 94 || p + ta > end) return false;
    vec::TextInfo t;
    t.align = std::clamp<int>(r.u8(p + 4), 0, 2);
    const int32_t ix = r.i32(p + 5), iy = r.i32(p + 9);
    double m[9];
    for (int k = 0; k < 9; ++k) m[k] = rd_f64(r, p + 13 + static_cast<size_t>(k) * 8);
    p += ta;
    if (!r.ok(p, 8)) return false;
    const size_t td = r.u32(p);
    const uint32_t count = r.u32(p + 4);
    p += td;
    bool bold = false, italic = false, stroked = false, filled = true;
    float stroke_width = 1.0f;
    int styles = 0;
    for (uint32_t i = 0; i < count && r.ok(p, 6); ++i) {
        const size_t ea = r.u32(p);
        const uint16_t type = r.u16(p + 4);
        p += ea;
        if (!r.ok(p, 4)) break;
        const size_t cl = r.u32(p);
        if (type == 1) {  // character
            if (cl >= 8) append_utf8(t.text, r.u32(p + 4));
            p += cl;
        } else if (type == 2) {  // character style
            size_t q = p + 4;
            if (!r.ok(q, 2)) break;
            const uint16_t nlen = r.u16(q);
            if (!r.ok(q + 2, nlen)) break;
            std::string name(reinterpret_cast<const char*>(r.p + q + 2), nlen);
            q += 2 + nlen;
            if (r.ok(q, 53)) {
                const uint32_t flags = r.u32(q), weight = r.u32(q + 4);
                const int32_t size = r.i32(q + 12);
                const uint8_t aa = r.u8(q + 16), justify = r.u8(q + 17);
                const uint8_t stk = r.u8(q + 43), fil = r.u8(q + 44);
                const double sw = rd_f64(r, q + 46);
                if (styles++ == 0) {
                    t.font_family = name;
                    t.size = static_cast<float>(std::max(1, size));
                    t.align = std::clamp<int>(justify, 0, 2);
                    t.antialias = aa != 0 || (flags & 0x10) != 0;
                    italic = (flags & 1) != 0;
                    bold = weight >= 600;
                    stroked = stk != 0; filled = fil != 0;
                    stroke_width = static_cast<float>(sw);
                }
            }
            p += cl;
            // Paint styles (stroke, fill) and the line style follow the fixed fields.
            int seen = 0;
            while (r.ok(p, 10) && std::memcmp(r.p + p, "~BK\0", 4) == 0) {
                const uint16_t id = r.u16(p + 4);
                const size_t len = r.u32(p + 6);
                const Block inner{id, p + 10, p + 10 + len};
                if (inner.end > end) break;
                if (id == kPaintStyleBlock && styles == 1) { read_paint_style(r, inner, seen == 0 ? o.stroke : o.fill); ++seen; }
                p = inner.end;
            }
        } else {
            p += cl;
        }
    }
    if (!stroked) o.stroke.kind = vec::PaintStyle::Kind::None;
    if (!filled) o.fill.kind = vec::PaintStyle::Kind::None;
    o.stroke_width = stroke_width;
    o.antialias = t.antialias;
    o.is_text = true;
    // Lay the text out again with the nearest font we have, then apply the
    // deformation matrix. Verified against the original: x' = m0 x + m1 y +
    // m2, y' = m3 x + m4 y + m5 in image coordinates (about the origin).
    t.font_path = font_file_for(t.font_family, bold, italic);
    if (!t.font_path.empty()) {
        if (auto font = text::Font::load(t.font_path)) {
            o.paths = vec::text_outline_paths(t, *font, &t.baseline);
            o.text = t;   // x, y at 0: translate() moves the insert point along with the outlines
            o.translate(static_cast<float>(ix), static_cast<float>(iy) - t.baseline);
            const bool identity = std::abs(m[0] - 1) < 1e-6 && std::abs(m[1]) < 1e-6 && std::abs(m[2]) < 1e-6 && std::abs(m[3]) < 1e-6 && std::abs(m[4] - 1) < 1e-6 && std::abs(m[5]) < 1e-6;
            if (!identity)
                o.transform(static_cast<float>(m[0]), static_cast<float>(m[1]), static_cast<float>(m[3]), static_cast<float>(m[4]), static_cast<float>(m[2]), static_cast<float>(m[5]));
            return true;
        }
    }
    o.text = t;
    return true;
}

bool read_shape(const Reader& r, const Block& sb, vec::Object& o) {
    size_t p = sb.start;
    if (!r.ok(p, 6)) return false;
    const size_t c0 = r.u32(p);
    const uint16_t nlen = r.u16(p + 4);
    if (!r.ok(p + 6, nlen + 14u)) return false;
    o.name.assign(reinterpret_cast<const char*>(r.p + p + 6), nlen);
    size_t q = p + 6 + nlen;
    o.file_type = r.u16(q); o.file_a = r.u32(q + 2); o.file_flags = r.u32(q + 6); o.file_c = r.u32(q + 10);
    p += c0;
    if (o.file_type == 5) {  // group: only a member count follows
        o.is_group = true;
        if (r.ok(p, 8) && r.u32(p) == 8) o.group_count = r.u32(p + 4);
        return true;
    }
    if (o.file_type == 1) return read_text_shape(r, p, sb.end, o);
    if (!r.ok(p, 4)) return false;
    const size_t c1 = r.u32(p);
    bool stroke_on = true, fill_on = true;
    if (c1 >= 4 && r.ok(p, c1)) {
        o.attr_raw.assign(r.p + p + 4, r.p + p + c1);
        if (o.attr_raw.size() >= 56) {
            stroke_on = o.attr_raw[0] != 0;
            fill_on = o.attr_raw[1] != 0;
            o.antialias = o.attr_raw[2] != 0;
            double d; std::memcpy(&d, o.attr_raw.data() + 3, 8); o.stroke_width = static_cast<float>(d);
            std::memcpy(&d, o.attr_raw.data() + 48, 8); o.miter = static_cast<float>(d);
            // The two records read as the first and last line caps: the
            // default file (+Solid) carries the same 7.21 sizes as these.
            auto f64at = [&](size_t off) { double v; std::memcpy(&v, o.attr_raw.data() + off, 8); return static_cast<float>(v); };
            o.line.first_cap = o.attr_raw[11]; o.line.first_w = f64at(13); o.line.first_h = f64at(21);
            o.line.last_cap = o.attr_raw[29]; o.line.last_w = f64at(31); o.line.last_h = f64at(39);
        }
    }
    p += c1;
    int styles_seen = 0;
    while (p < sb.end) {
        if (r.ok(p, 4) && std::memcmp(r.p + p, "~BK\0", 4) == 0) {
            if (!r.ok(p + 4, 6)) break;
            const uint16_t id = r.u16(p + 4);
            const size_t len = r.u32(p + 6);
            const Block inner{id, p + 10, p + 10 + len};
            if (inner.end > sb.end) break;
            if (id == kPaintStyleBlock) { read_paint_style(r, inner, styles_seen == 0 ? o.stroke : o.fill); ++styles_seen; }
            else if (id == kLineStyleBlock && r.ok(inner.start, 4)) {
                const size_t cl = r.u32(inner.start);
                if (cl >= 4 && inner.start + cl <= inner.end) {
                    o.linestyle_raw.assign(r.p + inner.start + 4, r.p + inner.start + cl);
                    if (o.linestyle_raw.size() >= 40) {
                        const uint8_t* d = o.linestyle_raw.data();
                        auto f64at = [&](size_t off) { double v; std::memcpy(&v, d + off, 8); return static_cast<float>(v); };
                        // u16 cap, f64 w, f64 h, u16 cap, f64 w, f64 h, u8 on, 4 bytes.
                        // (Not the u32 framing of .PspStyledLine files: the original
                        // hangs on a block that puts the 1.0 sizes at the wrong offsets.)
                        o.line.seg_start_cap = static_cast<uint32_t>(d[0] | (d[1] << 8)); o.line.seg_start_w = f64at(2); o.line.seg_start_h = f64at(10);
                        o.line.seg_end_cap = static_cast<uint32_t>(d[18] | (d[19] << 8)); o.line.seg_end_w = f64at(20); o.line.seg_end_h = f64at(28);
                        o.line.seg_caps_on = d[36];
                    }
                }
            }
            p = inner.end;
            continue;
        }
        if (!r.ok(p, 4)) break;
        const size_t cl = r.u32(p);
        if (cl == 8) {  // path with node count
            const uint32_t n = r.u32(p + 4);
            p += 8;
            // One node list per shape; a node with bit 0 of its first flag
            // byte starts a new subpath, bit 7 of the second closes one.
            vec::Path path;
            auto flush = [&]() {
                if (path.nodes.empty()) return;
                path.closed = (path.nodes.back().flags[1] & 0x80) != 0;
                o.paths.push_back(std::move(path));
                path = vec::Path{};
            };
            for (uint32_t i = 0; i < n && r.ok(p, 55) && r.u32(p) == 55; ++i) {
                vec::Node nd;
                nd.x = rd_f64(r, p + 4); nd.y = rd_f64(r, p + 12);
                nd.in_x = rd_f64(r, p + 20); nd.in_y = rd_f64(r, p + 28);
                nd.out_x = rd_f64(r, p + 36); nd.out_y = rd_f64(r, p + 44);
                nd.flags[0] = r.u8(p + 52); nd.flags[1] = r.u8(p + 53); nd.flags[2] = r.u8(p + 54);
                if (nd.flags[0] & 1) flush();
                path.nodes.push_back(nd);
                p += 55;
            }
            flush();
            continue;
        }
        if (cl < 4 || p + cl > sb.end) break;
        p += cl;
    }
    if (!stroke_on) o.stroke.kind = vec::PaintStyle::Kind::None;
    if (!fill_on) o.fill.kind = vec::PaintStyle::Kind::None;
    return !o.paths.empty() || o.is_text || o.is_group;
}

}  // namespace

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
    if (type == kLayerAdjustment) {
        Layer& L = doc.add_layer(name);
        L.type = LayerType::Adjustment;
        L.depth = depth;
        L.opacity = opacity / 255.0f;
        L.blend = map_blend(blend);
        L.visible = visible != 0;
        size_t bitmap_at = lb.start + chunk;
        for (const Block& b : blocks(r, lb.start + chunk, lb.end))
            if (b.id == kAdjustmentExtBlock) { read_adjustment(r, b, L.adjustment); bitmap_at = b.end; }
        // The adjustment bitmap (an 8-bit mask over the layer rect) limits where it applies.
        if (r.ok(bitmap_at, 8) && bitmap_at + 8 <= lb.end) {
            const size_t bchunk = r.u32(bitmap_at);
            const int sw = saved_mask[2] - saved_mask[0], sh = saved_mask[3] - saved_mask[1];
            const int ox = mask_rect[0] + saved_mask[0], oy = mask_rect[1] + saved_mask[1];
            for (const Block& b : blocks(r, bitmap_at + bchunk, lb.end)) {
                if (b.id != kChannelBlock || !r.ok(b.start, 16) || r.u16(b.start + 12) != kDibAdjustment) continue;
                const size_t cchunk = r.u32(b.start), clen = r.u32(b.start + 4);
                std::vector<uint8_t> tile;
                if (sw <= 0 || sh <= 0 || !r.ok(b.start + cchunk, clen)) continue;
                if (!decompress(hdr.compression, r.p + b.start + cchunk, clen, static_cast<size_t>(sw) * sh, tile, err)) return false;
                Mask m(doc.width(), doc.height(), 0);
                bool all = true;
                for (int y = 0; y < m.height(); ++y)
                    for (int x = 0; x < m.width(); ++x) {
                        const int tx = x - ox, ty = y - oy;
                        if (tx >= 0 && ty >= 0 && tx < sw && ty < sh) m.at(x, y) = tile[static_cast<size_t>(ty) * sw + tx];
                        if (m.at(x, y) != 255) all = false;
                    }
                if (!all) L.mask = std::move(m);   // a full-white bitmap is "no mask"
                break;
            }
        }
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
    if (type == kLayerVector) {
        Layer& L = doc.add_layer(name);
        L.type = LayerType::Vector;
        L.depth = depth;
        L.opacity = opacity / 255.0f;
        L.blend = map_blend(blend);
        L.visible = visible != 0;
        L.expanded = false;   // the palette lists objects only on request
        L.pixels = Image(doc.width(), doc.height(), {0, 0, 0, 0});
        for (const Block& vb : blocks(r, lb.start + chunk, lb.end)) {
            if (vb.id != kVectorExtBlock || !r.ok(vb.start, 8)) continue;
            const size_t vchunk = r.u32(vb.start);
            for (const Block& sb : blocks(r, vb.start + vchunk, vb.end)) {
                if (sb.id != kShapeBlock) continue;
                vec::Object o;
                if (read_shape(r, sb, o)) L.objects.push_back(std::move(o));
                else if (warnings) warnings->push_back("Skipped an unreadable shape in vector layer \"" + name + "\"");
            }
        }
        doc.rasterize_vector_layer(doc.layer_count() - 1);
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
    Image16 tile16;
    if (!read_channels(r, subs, hdr.compression, hdr.depth, pal, hdr.grayscale, sw, sh, tile, err, hdr.depth == 48 ? &tile16 : nullptr)) return false;
    if (hdr.depth == 48 && !tile16.empty()) {
        Image16 full(doc.width(), doc.height());
        for (int y = 0; y < sh; ++y) {
            const int dy = oy + y;
            if (dy < 0 || dy >= full.height()) continue;
            const int x0 = std::max(0, -ox), x1 = std::min(sw, full.width() - ox);
            if (x1 <= x0) continue;
            std::memcpy(full.data() + (static_cast<size_t>(dy) * full.width() + ox + x0) * 4, tile16.data() + (static_cast<size_t>(y) * sw + x0) * 4, static_cast<size_t>(x1 - x0) * 8);
        }
        L.deep = std::make_shared<const Image16>(std::move(full));
    }
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

// --- Firn stash --------------------------------------------------------------
// Things the original's format has no place for (our filter layers) ride in
// the creator block's description field as JSON. The original shows the
// text in its image information and otherwise ignores it; the layers
// themselves are written as empty placeholders it can read.

std::string creator_description(const Reader& r, const Block& b) {
    size_t o = b.start;
    while (o + 10 <= b.end) {
        if (std::memcmp(r.p + o, "~FL\0", 4) != 0) break;
        const uint16_t id = r.u16(o + 4);
        const size_t len = r.u32(o + 6);
        if (!r.ok(o + 10, len) || o + 10 + len > b.end) break;
        if (id == 5) return std::string(reinterpret_cast<const char*>(r.p + o + 10), len);
        o += 10 + len;
    }
    return {};
}

// The stash names a layer by index and by name. The index is only trusted
// when the name at it agrees: another program (or an older Firn) may have
// reordered or removed layers since, and restoring a filter or a style onto
// the wrong layer is worse than dropping it.
int stash_layer(const Document& doc, const json::Value& entry, std::vector<std::string>* warnings) {
    const int idx = static_cast<int>(entry.get("layer").as_number(-1));
    const std::string name = entry.get("name").as_string();
    const bool in_range = idx >= 0 && idx < static_cast<int>(doc.layer_count());
    if (in_range && (name.empty() || doc.layer(idx).name == name)) return idx;
    int found = -1;
    for (size_t i = 0; i < doc.layer_count(); ++i) {
        if (doc.layer(i).name != name) continue;
        if (found >= 0) { found = -2; break; }   // more than one candidate
        found = static_cast<int>(i);
    }
    if (found >= 0) return found;
    if (warnings) warnings->push_back("Layer \"" + name + "\" is not where the file said, so its Firn-only settings were dropped");
    return -1;
}

void apply_firn_stash(const Reader& r, const Block& creator, Document& doc, std::vector<std::string>* warnings) {
    const std::string text = creator_description(r, creator);
    if (text.empty() || text[0] != '{') return;
    json::Value v;
    if (!json::parse(text, v) || !v.find("firn")) return;
    const json::Value& filters = v.get("filters");
    for (size_t i = 0; i < filters.size(); ++i) {
        const json::Value& f = filters[i];
        const int idx = stash_layer(doc, f, warnings);
        if (idx < 0 || !doc.layer(idx).is_raster()) continue;
        Layer& L = doc.layer(idx);
        L.type = LayerType::Adjustment;
        L.pixels = Image();
        L.deep.reset();
        L.background = false;
        Adjustment& a = L.adjustment;
        a.kind = static_cast<Adjustment::Kind>(static_cast<int>(f.get("kind").as_number(100)));
        a.blur_radius = static_cast<float>(f.get("blur_radius").as_number(a.blur_radius));
        a.average_radius = static_cast<int>(f.get("average_radius").as_number(a.average_radius));
        a.unsharp_radius = static_cast<float>(f.get("unsharp_radius").as_number(a.unsharp_radius));
        a.unsharp_strength = static_cast<int>(f.get("unsharp_strength").as_number(a.unsharp_strength));
        a.unsharp_clipping = static_cast<int>(f.get("unsharp_clipping").as_number(a.unsharp_clipping));
    }
    const json::Value& styles = v.get("styles");
    for (size_t i = 0; i < styles.size(); ++i) {
        const json::Value& e = styles[i];
        const int idx = stash_layer(doc, e, warnings);
        if (idx < 0) continue;
        doc.layer(idx).style = LayerStyle::from_json(e.get("style"));
    }
    doc.touch();
}

std::string firn_stash(const Document& doc) {
    json::Value filters = json::Value::array();
    for (size_t i = 0; i < doc.layer_count(); ++i) {
        const Layer& L = doc.layer(i);
        if (!L.is_adjustment() || !L.adjustment.is_filter()) continue;
        const Adjustment& a = L.adjustment;
        json::Value f = json::Value::object();
        f.set("layer", json::Value::number(static_cast<double>(i)));
        f.set("name", json::Value::string(L.name));
        f.set("kind", json::Value::number(static_cast<int>(a.kind)));
        f.set("blur_radius", json::Value::number(a.blur_radius));
        f.set("average_radius", json::Value::number(a.average_radius));
        f.set("unsharp_radius", json::Value::number(a.unsharp_radius));
        f.set("unsharp_strength", json::Value::number(a.unsharp_strength));
        f.set("unsharp_clipping", json::Value::number(a.unsharp_clipping));
        filters.push(std::move(f));
    }
    json::Value styles = json::Value::array();
    for (size_t i = 0; i < doc.layer_count(); ++i) {
        const Layer& L = doc.layer(i);
        if (!L.style.any()) continue;
        json::Value e = json::Value::object();
        e.set("layer", json::Value::number(static_cast<double>(i)));
        e.set("name", json::Value::string(L.name));
        e.set("style", L.style.to_json());
        styles.push(std::move(e));
    }
    if (filters.size() == 0 && styles.size() == 0) return {};
    json::Value root = json::Value::object();
    root.set("firn", json::Value::number(1));
    if (filters.size()) root.set("filters", std::move(filters));
    if (styles.size()) root.set("styles", std::move(styles));
    return json::dump(root);
}

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
    const Block* alpha_bank = nullptr;
    const Block* creator = nullptr;
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
        } else if (b.id == kAlphaBankBlock) {
            alpha_bank = &b;
        } else if (b.id == kCreatorBlock) {
            creator = &b;
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
        if (creator) apply_firn_stash(r, *creator, *doc, warnings);
    }
    if (doc->layer_count() == 0) {
        if (composite_bank && read_composite(r, *composite_bank, hdr, have_palette ? &pal : nullptr, *doc, e)) {
            if (warnings) warnings->push_back("No raster layers; loaded the flattened composite instead");
        } else {
            return fail(e.empty() ? "no readable layers" : e);
        }
    }
    // Alpha bank: saved selections. Chunk {6, count u16}; each channel block
    // has a chunk {name, rect, saved rect}, a bitmap chunk and one channel of
    // DIB type 4 over the saved rect (relative to the rect).
    if (alpha_bank && r.ok(alpha_bank->start, 6)) {
        const size_t chunk = r.u32(alpha_bank->start);
        for (const Block& ab : blocks(r, alpha_bank->start + chunk, alpha_bank->end)) {
            if (ab.id != kAlphaChannelBlock || !r.ok(ab.start, 6)) continue;
            const size_t achunk = r.u32(ab.start);
            const uint16_t nlen = r.u16(ab.start + 4);
            if (!r.ok(ab.start + 6, nlen + 32u)) continue;
            Document::AlphaChannel ch;
            ch.name.assign(reinterpret_cast<const char*>(r.p + ab.start + 6), nlen);
            size_t o = ab.start + 6 + nlen;
            const int32_t rect[4] = {r.i32(o), r.i32(o + 4), r.i32(o + 8), r.i32(o + 12)};
            const int32_t saved[4] = {r.i32(o + 16), r.i32(o + 20), r.i32(o + 24), r.i32(o + 28)};
            const size_t bo = ab.start + achunk;
            if (!r.ok(bo, 8)) continue;
            const size_t bchunk = r.u32(bo);
            const int sw = saved[2] - saved[0], sh = saved[3] - saved[1];
            const int ox = rect[0] + saved[0], oy = rect[1] + saved[1];
            ch.mask = Mask(hdr.width, hdr.height, 0);
            for (const Block& cb : blocks(r, bo + bchunk, ab.end)) {
                if (cb.id != kChannelBlock || !r.ok(cb.start, 16) || r.u16(cb.start + 12) != kDibAlphaMask) continue;
                const size_t cchunk = r.u32(cb.start), clen = r.u32(cb.start + 4);
                std::vector<uint8_t> tile;
                if (sw <= 0 || sh <= 0 || !r.ok(cb.start + cchunk, clen)) break;
                if (!decompress(hdr.compression, r.p + cb.start + cchunk, clen, static_cast<size_t>(sw) * sh, tile, e)) break;
                for (int y = 0; y < sh; ++y)
                    for (int x = 0; x < sw; ++x) {
                        const int dx = ox + x, dy = oy + y;
                        if (dx >= 0 && dy >= 0 && dx < hdr.width && dy < hdr.height) ch.mask.at(dx, dy) = tile[static_cast<size_t>(y) * sw + x];
                    }
                break;
            }
            doc->alpha_channels().push_back(std::move(ch));
        }
    }
    doc->set_active_layer(std::clamp(hdr.active_layer, 0, static_cast<int>(doc->layer_count()) - 1));
    return doc;
}

std::vector<vec::Object> load_preset_shapes(const std::string& path, std::string* err) {
    std::vector<vec::Object> out;
    auto doc = load_psp(path, err, nullptr);
    if (!doc) return out;
    for (size_t i = 0; i < doc->layer_count(); ++i)
        if (doc->layer(i).is_vector()) for (const vec::Object& o : doc->layer(i).objects) out.push_back(o);
    return out;
}

// Photoshop .grd version 3: big-endian; "8BGR", version u16, count u16, then
// per gradient a Pascal name, color stops (location 0..4096, midpoint, model,
// four u16 components) and opacity stops (location, midpoint, opacity).
std::vector<vec::Gradient> load_gradients(const std::string& path, std::string* err) {
    std::vector<vec::Gradient> out;
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot open " + path; return out; }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto be16 = [&](size_t o) { return static_cast<uint16_t>((d[o] << 8) | d[o + 1]); };
    auto be32 = [&](size_t o) { return (static_cast<uint32_t>(d[o]) << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3]; };
    if (d.size() < 8 || std::memcmp(d.data(), "8BGR", 4) != 0) { if (err) *err = "not a gradient file"; return out; }
    const int count = be16(6);
    size_t p = 8;
    for (int gi = 0; gi < count && p < d.size(); ++gi) {
        vec::Gradient g;
        const uint8_t nlen = d[p];
        if (p + 1 + nlen > d.size()) break;
        g.name.assign(reinterpret_cast<const char*>(d.data() + p + 1), nlen);
        p += 1 + nlen;
        // Some files pad the Pascal name to an even length, others do not:
        // take the reading whose stop count and first location make sense.
        auto plausible = [&](size_t q) { return q + 6 <= d.size() && be16(q) >= 1 && be16(q) <= 64 && be32(q + 2) <= 4096; };
        if (!plausible(p) && plausible(p + 1)) ++p;
        if (p + 2 > d.size()) break;
        const int nc = be16(p); p += 2;
        g.colors.clear();
        // Color stop (20 bytes): location 0..4096, midpoint, color model, four
        // u16 components, and a trailing u16 (stop type).
        for (int i = 0; i < nc && p + 20 <= d.size(); ++i) {
            const float loc = be32(p) / 4096.0f * 100.0f;
            const float mid = static_cast<float>(be32(p + 4));
            const uint16_t model = be16(p + 8);
            Color c{static_cast<uint8_t>(be16(p + 10) >> 8), static_cast<uint8_t>(be16(p + 12) >> 8), static_cast<uint8_t>(be16(p + 14) >> 8), 255};
            if (model == 3) c = {static_cast<uint8_t>(be16(p + 10) >> 8), static_cast<uint8_t>(be16(p + 10) >> 8), static_cast<uint8_t>(be16(p + 10) >> 8), 255};  // grayscale
            g.colors.push_back({c, loc, std::clamp(mid, 1.0f, 99.0f)});
            p += 20;
        }
        if (p + 2 > d.size()) break;
        const int no = be16(p); p += 2;
        g.opacities.clear();
        for (int i = 0; i < no && p + 10 <= d.size(); ++i) {
            g.opacities.push_back({be16(p + 8) / 255.0f * 100.0f, be32(p) / 4096.0f * 100.0f, static_cast<float>(be32(p + 4))});
            p += 10;
        }
        if (g.colors.empty()) continue;
        if (g.opacities.empty()) g.opacities = {{100, 0, 50}, {100, 100, 50}};
        out.push_back(std::move(g));
    }
    return out;
}

bool save_gradients(const std::vector<vec::Gradient>& gradients, const std::string& path, std::string* err) {
    std::vector<uint8_t> d;
    auto be16 = [&](unsigned v) { d.push_back(static_cast<uint8_t>(v >> 8)); d.push_back(static_cast<uint8_t>(v)); };
    auto be32 = [&](uint32_t v) { for (int k = 3; k >= 0; --k) d.push_back(static_cast<uint8_t>(v >> (8 * k))); };
    d.insert(d.end(), {'8', 'B', 'G', 'R'});
    be16(3);
    be16(static_cast<unsigned>(gradients.size()));
    for (const vec::Gradient& g : gradients) {
        const std::string name = g.name.substr(0, 255);
        d.push_back(static_cast<uint8_t>(name.size()));
        d.insert(d.end(), name.begin(), name.end());
        if ((1 + name.size()) % 2) d.push_back(0);  // Pascal string padded to an even length
        be16(static_cast<unsigned>(g.colors.size()));
        for (const vec::GradientStop& st : g.colors) {
            be32(static_cast<uint32_t>(std::lround(std::clamp(st.pos, 0.0f, 100.0f) / 100.0f * 4096.0f)));
            be32(static_cast<uint32_t>(std::lround(std::clamp(st.mid, 1.0f, 99.0f))));
            be16(0);  // RGB
            be16(st.color.r << 8); be16(st.color.g << 8); be16(st.color.b << 8); be16(0);
            be16(0);  // user stop
        }
        be16(static_cast<unsigned>(g.opacities.size()));
        for (const vec::OpacityStop& st : g.opacities) {
            be32(static_cast<uint32_t>(std::lround(std::clamp(st.pos, 0.0f, 100.0f) / 100.0f * 4096.0f)));
            be32(static_cast<uint32_t>(std::lround(std::clamp(st.mid, 1.0f, 99.0f))));
            be16(static_cast<unsigned>(std::lround(std::clamp(st.opacity, 0.0f, 100.0f) / 100.0f * 255.0f)));
        }
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot write " + path; return false; }
    f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
    return static_cast<bool>(f);
}

// Styled line file (docs/FORMAT.md): optional magic 01 51 45 57, then
// {first cap u32, last cap u32, first w f64, first h f64, last w f64,
// last h f64, miter f64, segment count u32, segment lengths u32 each,
// u32, u32, segment start cap {u32, f64, f64}, segment end cap {u32, f64,
// f64}, u32 segment caps on}.
std::optional<vec::LineStyle> load_styled_line(const std::string& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot open " + path; return std::nullopt; }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t o = 0;
    if (d.size() >= 4 && std::memcmp(d.data(), "\x01QEW", 4) == 0) o = 4;
    if (d.size() < o + 52) { if (err) *err = "not a styled line file"; return std::nullopt; }
    auto f64at = [&](size_t p) { double v; std::memcpy(&v, d.data() + p, 8); return static_cast<float>(v); };
    auto u32at = [&](size_t p) { uint32_t v; std::memcpy(&v, d.data() + p, 4); return v; };
    vec::LineStyle l;
    l.first_cap = u32at(o); l.last_cap = u32at(o + 4);
    l.first_w = f64at(o + 8); l.first_h = f64at(o + 16); l.last_w = f64at(o + 24); l.last_h = f64at(o + 32);
    l.miter = f64at(o + 40);
    const uint32_t n = u32at(o + 48);
    size_t p = o + 52;
    for (uint32_t i = 0; i < n && p + 4 <= d.size(); ++i, p += 4) l.dashes.push_back(static_cast<float>(u32at(p)));
    if (p + 52 <= d.size()) {
        l.flag_a = u32at(p); l.flag_b = u32at(p + 4); p += 8;
        l.seg_start_cap = u32at(p); l.seg_start_w = f64at(p + 4); l.seg_start_h = f64at(p + 12); p += 20;
        l.seg_end_cap = u32at(p); l.seg_end_w = f64at(p + 4); l.seg_end_h = f64at(p + 12); p += 20;
        l.seg_caps_on = u32at(p);
    }
    std::string base = path;
    if (const size_t sl = base.find_last_of("/\\"); sl != std::string::npos) base = base.substr(sl + 1);
    if (const size_t dot = base.rfind('.'); dot != std::string::npos) base = base.substr(0, dot);
    l.name = base;
    return l;
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

bool is_psd_extension(const std::string& path) {
    const auto dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == "psd" || ext == "psb";
}

std::unique_ptr<Document> load_document(const std::string& path, std::string* err, std::vector<std::string>* warnings) {
    if (is_ora_extension(path)) return load_ora(path, err, warnings);
    if (is_psp_extension(path)) return load_psp(path, err, warnings);
    if (is_psd_extension(path)) return load_psd(path, err, warnings);
    if (auto deep = load16(path, nullptr)) {
        auto doc = std::make_unique<Document>(deep->width(), deep->height());
        Layer& bg = doc->add_layer("Background");
        bg.background = true;
        bg.set_deep(std::move(*deep));
        doc->set_icc(read_icc(path));
        doc->set_metadata(read_metadata(path));
        return doc;
    }
    auto img = load(path, err);
    if (!img) return nullptr;
    auto doc = std::make_unique<Document>(img->width(), img->height());
    Layer& bg = doc->add_layer("Background");
    bg.background = true;
    bg.pixels = std::move(*img);
    doc->set_icc(read_icc(path));
    doc->set_metadata(read_metadata(path));
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
    // Each channel compresses on its own thread; the blocks are appended in order.
    std::future<std::vector<uint8_t>> jobs[4];
    for (int c = 0; c < 3; ++c) jobs[c] = std::async(std::launch::async, [&, c] { return channel_block(dib_image, static_cast<uint16_t>(c + 1), planes[c], padded * 3); });
    if (with_alpha) jobs[3] = std::async(std::launch::async, [&] { return channel_block(dib_trans, 0, planes[3], padded); });
    for (int c = 0; c < 3; ++c) w.bytes(jobs[c].get());
    if (with_alpha) w.bytes(jobs[3].get());
    return w.out;
}

// 48-bit variant: every channel, the transparency mask included, holds
// little-endian 16-bit samples (the layout GIMP's reader expects; the
// original itself predates 48-bit files, see docs/FORMAT.md).
std::vector<uint8_t> bitmap_and_channels16(const Image16& tile, bool with_alpha, uint16_t dib_image, uint16_t dib_trans) {
    const size_t npx = static_cast<size_t>(tile.width()) * tile.height();
    std::vector<uint8_t> planes[4];
    for (auto& p : planes) p.resize(npx * 2);
    const uint16_t* s = tile.data();
    for (size_t i = 0; i < npx; ++i)
        for (int c = 0; c < 4; ++c) { planes[c][i * 2] = static_cast<uint8_t>(s[i * 4 + c] & 255); planes[c][i * 2 + 1] = static_cast<uint8_t>(s[i * 4 + c] >> 8); }
    Writer w;
    w.u32(8);
    w.u16(with_alpha ? 2 : 1);
    w.u16(with_alpha ? 4 : 3);
    const size_t padded = (static_cast<size_t>(tile.width()) * 2 + 3) / 4 * 4 * tile.height();
    std::future<std::vector<uint8_t>> jobs[4];
    for (int c = 0; c < 3; ++c) jobs[c] = std::async(std::launch::async, [&, c] { return channel_block(dib_image, static_cast<uint16_t>(c + 1), planes[c], padded * 3); });
    if (with_alpha) jobs[3] = std::async(std::launch::async, [&] { return channel_block(dib_trans, 0, planes[3], padded); });
    for (int c = 0; c < 3; ++c) w.bytes(jobs[c].get());
    if (with_alpha) w.bytes(jobs[3].get());
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


// Vector shape writers (see the reader above for the layout).
namespace {


std::vector<uint8_t> write_paint_style(const vec::PaintStyle& s) {
    Writer w;
    w.u32(6); w.u16(static_cast<int>(s.kind));
    if (s.kind == vec::PaintStyle::Kind::Solid) {
        w.u32(12); w.u8(s.color.r); w.u8(s.color.g); w.u8(s.color.b); w.u8(0); w.u32(0xffffffffu);
    } else if (s.kind == vec::PaintStyle::Kind::Gradient) {
        const vec::Gradient& g = s.gradient;
        w.u32(35); w.u16(static_cast<int>(g.style)); w.u32(0xffffffffu); w.u8(g.invert ? 1 : 0);
        w.u32(static_cast<uint32_t>(g.center_x)); w.u32(static_cast<uint32_t>(g.center_y));
        w.f64(g.angle); w.u32(static_cast<uint32_t>(g.repeats));
        w.u16(static_cast<int>(g.colors.size())); w.u16(static_cast<int>(g.opacities.size()));
        for (const vec::GradientStop& st : g.colors) { w.u32(12); w.u8(st.color.r); w.u8(st.color.g); w.u8(st.color.b); w.u8(0); w.u16(static_cast<int>(st.pos)); w.u16(static_cast<int>(st.mid)); }
        for (const vec::OpacityStop& st : g.opacities) { w.u32(9); w.u8(static_cast<int>(st.opacity)); w.u16(static_cast<int>(st.pos)); w.u16(static_cast<int>(st.mid)); }
    }
    Writer b;
    b.block(kPaintStyleBlock, w.out);
    return b.out;
}

// The 41-byte line style payload: {u16 cap, f64 w, f64 h} x2, u8 x5 (segments).
std::vector<uint8_t> write_line_style(const vec::LineStyle& l, const std::vector<uint8_t>& raw) {
    Writer w;
    if (raw.size() == 41) w.bytes(raw);
    else {
        w.u16(static_cast<int>(l.seg_start_cap)); w.f64(l.seg_start_w); w.f64(l.seg_start_h);
        w.u16(static_cast<int>(l.seg_end_cap)); w.f64(l.seg_end_w); w.f64(l.seg_end_h);
        w.u8(static_cast<uint8_t>(l.seg_caps_on)); w.u32(0);
    }
    Writer c;
    c.u32(static_cast<uint32_t>(w.out.size() + 4));
    c.bytes(w.out);
    Writer b;
    b.block(kLineStyleBlock, c.out);
    return b.out;
}

std::vector<uint8_t> write_shape(const vec::Object& o) {
    Writer w;
    const std::string name = o.name.substr(0, 255);
    w.u32(static_cast<uint32_t>(4 + 2 + name.size() + 14));
    w.u16(static_cast<int>(name.size()));
    w.bytes(reinterpret_cast<const uint8_t*>(name.data()), name.size());
    w.u16(o.is_group ? 5 : o.file_type); w.u32(o.file_a); w.u32(o.file_flags); w.u32(o.file_c);
    if (o.is_group) {
        w.u32(8); w.u32(o.group_count);
        Writer gb;
        gb.block(kShapeBlock, w.out);
        return gb.out;
    }
    if (o.is_text && !o.text.text.empty()) {
        // Text Vector Shape: the original lays the text out again from the
        // font name, so it stays editable there and here. Rotation goes into
        // the deformation matrix, about the insert point.
        const size_t type_at = static_cast<size_t>(4 + 2 + name.size());
        w.out[type_at] = 1; w.out[type_at + 1] = 0;  // shape type keVSTText
        const vec::TextInfo& t = o.text;
        w.u32(94); w.u8(static_cast<uint8_t>(std::clamp(t.align, 0, 2)));
        w.i32(static_cast<int32_t>(std::lround(t.x))); w.i32(static_cast<int32_t>(std::lround(t.y + t.baseline)));
        // Rotation about the insert point: x' = m0 x + m1 y + m2, y' = m3 x + m4 y + m5.
        const double rad = t.rotation * 3.14159265358979 / 180.0, rc = std::cos(rad), rs = std::sin(rad);
        const double ix = std::lround(t.x), iy = std::lround(t.y + t.baseline);
        const double matrix[9] = {rc, -rs, ix - (rc * ix - rs * iy), rs, rc, iy - (rs * ix + rc * iy), 0, 0, 1};
        for (double v : matrix) w.f64(v);
        w.u8(0); w.f64(0.0);
        const std::vector<uint32_t> chars = decode_utf8(t.text);
        w.u32(8); w.u32(static_cast<uint32_t>(chars.size() + 1));
        // Character style element first, then one element per character.
        w.u32(6); w.u16(2);
        std::string family = t.font_family, style;
        const size_t sep = family.find("  ");
        if (sep != std::string::npos) { style = family.substr(sep + 2); family = family.substr(0, sep); }
        for (char& ch : style) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const bool bold = style.find("bold") != std::string::npos, italic = style.find("italic") != std::string::npos || style.find("oblique") != std::string::npos;
        if (family.size() > 255) family.resize(255);
        Writer cs;
        cs.u16(static_cast<int>(family.size())); cs.bytes(reinterpret_cast<const uint8_t*>(family.data()), family.size());
        cs.u32((t.antialias ? 0x10u : 0u) | (italic ? 1u : 0u));
        cs.u32(bold ? 700 : 400); cs.i32(0); cs.i32(static_cast<int32_t>(std::lround(t.size)));
        cs.u8(t.antialias ? 2 : 0); cs.u8(static_cast<uint8_t>(std::clamp(t.align, 0, 2))); cs.u8(1);
        cs.f64(0.0); cs.f64(0.0); cs.f64(0.0);
        cs.u8(o.stroke.enabled() ? 1 : 0); cs.u8(o.fill.enabled() ? 1 : 0); cs.u8(0);
        cs.f64(o.stroke_width);
        cs.u8(0); cs.u8(0); cs.f64(1.0); cs.f64(1.0);
        cs.u8(0); cs.u8(0); cs.f64(1.0); cs.f64(1.0);
        cs.u8(0); cs.f64(o.miter);
        w.u32(static_cast<uint32_t>(4 + cs.out.size())); w.bytes(cs.out);
        w.bytes(write_paint_style(o.stroke));
        w.bytes(write_paint_style(o.fill));
        w.bytes(write_line_style(o.line, o.linestyle_raw));
        for (uint32_t cp : chars) { w.u32(6); w.u16(1); w.u32(8); w.u32(cp); }
        Writer b;
        b.block(kShapeBlock, w.out);
        return b.out;
    }
    // Attribute chunk: reuse the file's bytes where we have them, patching
    // the fields we understand (stroke/fill on, antialias, width, miter).
    std::vector<uint8_t> attr = o.attr_raw;
    if (attr.size() != 56) {
        Writer a;
        a.u8(0); a.u8(0); a.u8(0); a.f64(1.0);
        a.u8(1); a.u8(0); a.f64(1.0); a.f64(1.0);
        a.u8(1); a.u8(0); a.f64(1.0); a.f64(1.0);
        a.u8(0); a.f64(10.0);
        attr = a.out;
    }
    attr[0] = o.stroke.enabled() ? 1 : 0;
    attr[1] = o.fill.enabled() ? 1 : 0;
    attr[2] = o.antialias ? 1 : 0;
    const double wd = o.stroke_width, mt = o.miter;
    std::memcpy(attr.data() + 3, &wd, 8);
    std::memcpy(attr.data() + 48, &mt, 8);
    auto put = [&](size_t off, double v) { std::memcpy(attr.data() + off, &v, 8); };
    attr[11] = static_cast<uint8_t>(o.line.first_cap); put(13, o.line.first_w); put(21, o.line.first_h);
    attr[29] = static_cast<uint8_t>(o.line.last_cap); put(31, o.line.last_w); put(39, o.line.last_h);
    w.u32(60); w.bytes(attr);
    w.bytes(write_paint_style(o.stroke));
    w.bytes(write_paint_style(o.fill));
    w.bytes(write_line_style(o.line, o.linestyle_raw));
    // All subpaths go into one node list (the original reads a single one);
    // the start and close flags delimit them.
    size_t total = 0;
    for (const vec::Path& p : o.paths) total += p.nodes.size();
    w.u32(8); w.u32(static_cast<uint32_t>(total));
    for (const vec::Path& p : o.paths) {
        for (size_t i = 0; i < p.nodes.size(); ++i) {
            const vec::Node& n = p.nodes[i];
            w.u32(55);
            w.f64(n.x); w.f64(n.y); w.f64(n.in_x); w.f64(n.in_y); w.f64(n.out_x); w.f64(n.out_y);
            uint8_t f0 = n.flags[0], f1 = n.flags[1];
            if (i == 0) f0 |= 1; else f0 &= static_cast<uint8_t>(~1);
            if (i + 1 == p.nodes.size()) { if (p.closed) f1 |= 0x80; else f1 &= static_cast<uint8_t>(~0x80); }
            else f1 &= static_cast<uint8_t>(~0x80);
            w.u8(f0); w.u8(f1); w.u8(n.flags[2]);
        }
    }
    Writer b;
    b.block(kShapeBlock, w.out);
    return b.out;
}

std::vector<uint8_t> vector_layer_payload(const Layer& L) {
    Writer ext;
    ext.u32(8); ext.u32(static_cast<uint32_t>(L.objects.size()));
    // Shape ids are unique within a layer in the original's files; objects
    // made here all start at 1, so renumber in order.
    uint32_t next_id = 1;
    for (const vec::Object& o : L.objects) {
        vec::Object copy = o;
        copy.file_flags = next_id++;
        ext.bytes(write_shape(copy));
    }
    Writer b;
    b.block(kVectorExtBlock, ext.out);
    return b.out;
}

}  // namespace



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

std::vector<uint8_t> adjustment_definition(const Adjustment& a) {
    Writer w;
    switch (a.kind) {
        case Adjustment::Kind::Levels:
            w.u32(4 + 32 + 64);
            for (int c = 0; c < 4; ++c) w.f64(a.levels[c].gamma);
            for (int c = 0; c < 4; ++c) w.i32(a.levels[c].in_high);
            for (int c = 0; c < 4; ++c) w.i32(a.levels[c].in_low);
            for (int c = 0; c < 4; ++c) w.i32(a.levels[c].out_high);
            for (int c = 0; c < 4; ++c) w.i32(a.levels[c].out_low);
            break;
        case Adjustment::Kind::Curves:
            for (int c = 0; c < 4; ++c) {
                const auto& pts = a.curves[c];
                w.u32(4 + 3 + 36 + 256);
                w.u8(0);
                w.u16(static_cast<int>(std::min<size_t>(pts.size(), 18)));
                for (size_t i = 0; i < 18; ++i) {
                    if (i < pts.size()) { w.u8(static_cast<uint8_t>(std::clamp(pts[i].first, 0.0f, 255.0f))); w.u8(static_cast<uint8_t>(std::clamp(pts[i].second, 0.0f, 255.0f))); }
                    else { w.u8(0); w.u8(0); }
                }
                const adjust::Lut lut = adjust::curve_lut(pts);
                for (int i = 0; i < 256; ++i) w.u8(lut[i]);
            }
            break;
        case Adjustment::Kind::BrightnessContrast: w.u32(12); w.i32(a.brightness); w.i32(a.contrast); break;
        case Adjustment::Kind::ColorBalance:
            w.u32(5 + 36); w.u8(a.color_balance.preserve_luminosity ? 1 : 0);
            for (int i = 0; i < 3; ++i) w.i32(a.color_balance.highlights[i]);
            for (int i = 0; i < 3; ++i) w.i32(a.color_balance.midtones[i]);
            for (int i = 0; i < 3; ++i) w.i32(a.color_balance.shadows[i]);
            break;
        case Adjustment::Kind::HSL:
            w.u32(5 + 24 + 6 * 7 * 4); w.u8(a.colorize ? 1 : 0);
            w.i32(a.hue); w.i32(a.saturation); w.i32(a.lightness);
            w.i32(a.colorize_hue); w.i32(a.colorize_saturation); w.i32(0);
            for (int rng = 0; rng < 6; ++rng) for (int k = 0; k < 7; ++k) w.i32(a.hsl_ranges[rng][k]);
            break;
        case Adjustment::Kind::ChannelMixer:
            w.u32(5 + 48); w.u8(a.mixer.monochrome ? 1 : 0);
            for (int row = 0; row < 3; ++row) {
                const int out = 2 - row;
                for (int i = 0; i < 3; ++i) w.i32(static_cast<int32_t>(a.mixer.mix[out][i]));
                w.i32(static_cast<int32_t>(a.mixer.constant[out]));
            }
            break;
        case Adjustment::Kind::Threshold: w.u32(8); w.i32(a.threshold); break;
        case Adjustment::Kind::Posterize: w.u32(8); w.i32(a.posterize); break;
        default: w.u32(4); break;
    }
    return w.out;
}

std::vector<uint8_t> adjustment_block(const Layer& L, int doc_w, int doc_h) {
    const int32_t full[4] = {0, 0, doc_w, doc_h};
    Writer payload;
    // The adjustment bitmap plays the user-mask role: its rects go in the
    // mask rect fields (the original hangs when they sit in the image rects).
    payload.bytes(layer_info(L.name, kLayerAdjustment, kZeroRect, kZeroRect, L.opacity, L.blend, L.visible, full, full, false));
    Writer ext;
    ext.u32(6); ext.u16(static_cast<int>(L.adjustment.kind));
    ext.bytes(adjustment_definition(L.adjustment));
    payload.block(kAdjustmentExtBlock, ext.out);
    payload.u32(8); payload.u16(1); payload.u16(1);
    std::vector<uint8_t> plane;
    if (L.has_mask() && L.mask_enabled) plane.assign(L.mask.data(), L.mask.data() + L.mask.size());
    else plane.assign(static_cast<size_t>(doc_w) * doc_h, 255);
    payload.bytes(channel_block(kDibAdjustment, 0, plane, (static_cast<size_t>(doc_w) + 3) / 4 * 4 * doc_h));
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

std::vector<uint8_t> layer_block(const Layer& L, int doc_w, int doc_h, bool deep_file) {
    const int32_t rect[4] = {0, 0, doc_w, doc_h};
    // Only a Background layer omits the transparency channel; the original
    // writes one for every other layer even when it is fully opaque, and the
    // reader relies on that to tell them apart.
    const bool with_alpha = !L.background;
    raster::Rect saved = with_alpha ? content_bounds(L.pixels) : raster::Rect{0, 0, doc_w, doc_h};
    // A fully transparent layer still gets a real (1 x 1) tile with channels:
    // the original never finishes reading a layer block without channel data.
    if (saved.empty() && !L.pixels.empty()) saved = {0, 0, 1, 1};
    const int32_t saved_rect[4] = {saved.x0, saved.y0, saved.x1, saved.y1};
    Writer payload;
    payload.bytes(layer_info(L.name, kLayerRaster, rect, saved_rect, L.opacity, L.blend, L.visible, kZeroRect, kZeroRect, false));
    if (!saved.empty() && deep_file) {
        Image16 tile = raster16::crop(L.is_deep() ? *L.deep : to_image16(L.pixels), saved);
        if (!with_alpha) { uint16_t* p = tile.data(); for (size_t i = 3; i < tile.size(); i += 4) p[i] = 65535; }
        payload.bytes(bitmap_and_channels16(tile, with_alpha, kDibImage, kDibTransMask));
    } else if (!saved.empty()) {
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

bool save_styled_line(const vec::LineStyle& l, const std::string& path, std::string* err) {
    Writer w;
    w.u8(0x01); w.u8('Q'); w.u8('E'); w.u8('W');
    w.u32(l.first_cap); w.u32(l.last_cap);
    w.f64(l.first_w); w.f64(l.first_h); w.f64(l.last_w); w.f64(l.last_h);
    w.f64(l.miter);
    w.u32(static_cast<uint32_t>(l.dashes.size()));
    for (float v : l.dashes) w.u32(static_cast<uint32_t>(std::max(0.0f, v) + 0.5f));
    w.u32(l.flag_a); w.u32(l.flag_b);
    w.u32(l.seg_start_cap); w.f64(l.seg_start_w); w.f64(l.seg_start_h);
    w.u32(l.seg_end_cap); w.f64(l.seg_end_w); w.f64(l.seg_end_h);
    w.u32(l.seg_caps_on);
    std::ofstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot write " + path; return false; }
    f.write(reinterpret_cast<const char*>(w.out.data()), static_cast<std::streamsize>(w.out.size()));
    return static_cast<bool>(f);
}

std::vector<uint8_t> save_psp_to_memory(const Document& doc) {
    Writer w;
    w.bytes(reinterpret_cast<const uint8_t*>(kSignature), sizeof(kSignature) - 1);
    while (w.out.size() < 32) w.u8(0);
    // Version 6.0 (the original's own format) for 24-bit documents; 48-bit
    // documents are a version 8 feature and are labeled as such.
    const bool deep_file = doc.bit_depth() == 16;
    w.u16(deep_file ? 8 : 6); w.u16(0);

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
    bool has_groups = false, has_masks = false, has_vectors = false, has_adjustments = false;
    // Blocks are built concurrently (each raster layer compresses on its own
    // threads) and appended in stack order afterwards.
    std::vector<std::future<std::vector<uint8_t>>> jobs;
    auto emit = [&](std::function<std::vector<uint8_t>()> fn) { jobs.push_back(std::async(std::launch::async, std::move(fn))); };
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
                { const Layer* Lp = &L; emit([Lp, members] { return group_block(*Lp, members); }); }
                ++block_count; has_groups = true;
                ++i;
                continue;
            }
            if (L.is_adjustment() && L.adjustment.is_filter()) {
                // Our filter layer: an empty raster placeholder the original
                // opens, with the parameters in the stash. A mask rides along
                // the same way a masked raster layer's does, wrapped in a
                // group, so it survives the round trip here too.
                Layer ph;
                ph.name = L.name; ph.visible = L.visible; ph.opacity = L.opacity; ph.blend = L.blend;
                ph.pixels = Image(doc.width(), doc.height(), {0, 0, 0, 0});
                const int dw = doc.width(), dh = doc.height();
                if (L.has_mask()) {
                    Layer g = ph;
                    g.type = LayerType::Group;
                    { const Layer gc = g; emit([gc] { return group_block(gc, 2); }); }
                    Layer plain = ph;
                    plain.opacity = 1.0f; plain.blend = BlendMode::Normal; plain.visible = true;
                    { const Layer pc = plain; emit([pc, dw, dh, deep_file] { return layer_block(pc, dw, dh, deep_file); }); }
                    { const Layer* Lp = &L; emit([Lp, dw, dh] { return mask_block(Lp->name, Lp->mask, Lp->mask_enabled, dw, dh); }); }
                    block_count += 3; has_groups = true; has_masks = true;
                } else {
                    emit([ph, dw, dh, deep_file] { return layer_block(ph, dw, dh, deep_file); });
                    ++block_count;
                }
                ++i;
                continue;
            }
            if (L.is_adjustment()) {
                { const Layer* Lp = &L; const int dw = doc.width(), dh = doc.height(); emit([Lp, dw, dh] { return adjustment_block(*Lp, dw, dh); }); }
                ++block_count; has_adjustments = true;
                ++i;
                continue;
            }
            if (L.is_vector()) {
                Writer payload;
                payload.bytes(layer_info(L.name, kLayerVector, kZeroRect, kZeroRect, L.opacity, L.blend, L.visible, kZeroRect, kZeroRect, false));
                payload.bytes(vector_layer_payload(L));
                payload.u32(8); payload.u16(0); payload.u16(0);
                Writer vb;
                vb.block(kLayerBlock, payload.out);
                { std::vector<uint8_t> bytes = vb.out; emit([bytes] { return bytes; }); }
                ++block_count; has_vectors = true;
                ++i;
                continue;
            }
            if (L.has_mask()) {
                Layer g = L;
                g.type = LayerType::Group;
                g.mask = Mask();
                { const Layer gc = g; emit([gc] { return group_block(gc, 2); }); }
                Layer plain = L;
                plain.opacity = 1.0f; plain.blend = BlendMode::Normal; plain.visible = true;
                { const Layer pc = plain; const int dw = doc.width(), dh = doc.height(); emit([pc, dw, dh, deep_file] { return layer_block(pc, dw, dh, deep_file); }); }
                { const Layer* Lp = &L; const int dw = doc.width(), dh = doc.height(); emit([Lp, dw, dh] { return mask_block(Lp->name, Lp->mask, Lp->mask_enabled, dw, dh); }); }
                block_count += 3; has_groups = true; has_masks = true;
            } else {
                { const Layer* Lp = &L; const int dw = doc.width(), dh = doc.height(); emit([Lp, dw, dh, deep_file] { return layer_block(*Lp, dw, dh, deep_file); }); }
                ++block_count;
            }
            ++i;
            // Emit the mask layer of any group that has just closed at this index.
            for (int g = doc.parent_group(i - 1); g >= 0; g = doc.parent_group(g)) {
                if (doc.group_end(g) != i) break;
                if (doc.layer(g).has_mask()) {
                    { const Layer* Gp = &doc.layer(g); const int dw = doc.width(), dh = doc.height(); emit([Gp, dw, dh] { return mask_block(Gp->name, Gp->mask, Gp->mask_enabled, dw, dh); }); }
                    ++block_count; has_masks = true;
                }
            }
        }
    }
    for (auto& j : jobs) bank.bytes(j.get());
    if (has_vectors) contents |= 0x00000002u; // vector layers
    if (has_adjustments) contents |= 0x00000004u; // adjustment layers
    if (has_groups) contents |= 0x00000008u;  // group layers
    if (has_masks) contents |= 0x00000010u;   // mask layers
    if (!doc.alpha_channels().empty()) contents |= 0x80000000u;  // alpha channels

    Writer img;
    img.u32(46); img.i32(doc.width()); img.i32(doc.height()); img.f64(72.0); img.u8(1);
    img.u16(kCompLz77); img.u16(deep_file ? 48 : 24); img.u16(1); img.u32(16777216); img.u8(0);
    img.u32(static_cast<uint32_t>(doc.width()) * doc.height() * (deep_file ? 6 : 3));  // sum of the layer bitmaps
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
    const std::string stash = firn_stash(doc);
    if (!stash.empty()) { Writer d; d.bytes(reinterpret_cast<const uint8_t*>(stash.data()), stash.size()); creator.field(5, d.out); }  // description
    w.block(kCreatorBlock, creator.out);

    w.bytes(composite_bank(flat));

    w.block(kLayerStartBlock, bank.out);

    if (!doc.alpha_channels().empty()) {
        Writer abank;
        abank.u32(6); abank.u16(static_cast<int>(doc.alpha_channels().size()));
        for (const Document::AlphaChannel& ch : doc.alpha_channels()) {
            Writer payload;
            const std::string name = ch.name.substr(0, 255);
            const uint32_t chunk_len = 4 + 2 + static_cast<uint32_t>(name.size()) + 32;
            payload.u32(chunk_len);
            payload.u16(static_cast<int>(name.size()));
            payload.bytes(reinterpret_cast<const uint8_t*>(name.data()), name.size());
            payload.i32(0); payload.i32(0); payload.i32(doc.width()); payload.i32(doc.height());
            payload.i32(0); payload.i32(0); payload.i32(doc.width()); payload.i32(doc.height());
            payload.u32(8); payload.u16(1); payload.u16(1);
            std::vector<uint8_t> plane(ch.mask.data(), ch.mask.data() + ch.mask.size());
            payload.bytes(channel_block(kDibAlphaMask, 0, plane, (static_cast<size_t>(doc.width()) + 3) / 4 * 4 * doc.height()));
            abank.block(kAlphaChannelBlock, payload.out);
        }
        w.block(kAlphaBankBlock, abank.out);
    }

    return w.out;
}

std::vector<uint8_t> vector_objects_to_bytes(const std::vector<vec::Object>& objects) {
    Writer ext;
    ext.u32(8); ext.u32(static_cast<uint32_t>(objects.size()));
    uint32_t next_id = 1;
    for (const vec::Object& o : objects) {
        vec::Object copy = o;
        copy.file_flags = next_id++;
        ext.bytes(write_shape(copy));
    }
    Writer b;
    b.block(kVectorExtBlock, ext.out);
    return b.out;
}

bool vector_objects_from_bytes(const uint8_t* data, size_t size, std::vector<vec::Object>& out) {
    const Reader r{data, size};
    bool any = false;
    for (const Block& vb : blocks(r, 0, size)) {
        if (vb.id != kVectorExtBlock || !r.ok(vb.start, 8)) continue;
        any = true;
        const size_t vchunk = r.u32(vb.start);
        for (const Block& sb : blocks(r, vb.start + vchunk, vb.end)) {
            if (sb.id != kShapeBlock) continue;
            vec::Object o;
            if (read_shape(r, sb, o)) out.push_back(std::move(o));
        }
    }
    return any;
}

std::vector<uint8_t> adjustment_to_bytes(const Adjustment& a) {
    Writer ext;
    ext.u32(6); ext.u16(static_cast<int>(a.kind));
    ext.bytes(adjustment_definition(a));
    return ext.out;
}

bool adjustment_from_bytes(const uint8_t* data, size_t size, Adjustment& out) {
    if (size < 6) return false;
    const Reader r{data, size};
    read_adjustment(r, Block{kAdjustmentExtBlock, 0, size}, out);
    return true;
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
    if (is_ora_extension(path)) return save_ora(doc, path, err);
    if (is_psp_extension(path)) return save_psp(doc, path, err);
    if (doc.bit_depth() == 16) {
        std::string ext = path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.') + 1);
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == "png")
            return save_png16(doc.composite16(), path, err) && embed_icc(path, doc.icc(), err) && embed_metadata(path, doc.metadata(), err);
    }
    return save(doc.composite(), path, err, jpeg_quality) && embed_icc(path, doc.icc(), err) && embed_metadata(path, doc.metadata(), err);
}

}  // namespace firn::io
