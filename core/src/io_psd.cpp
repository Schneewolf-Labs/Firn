// Photoshop PSD import: 8-bit and 16-bit RGB or grayscale, layers with
// opacity, blend mode, visibility, masks and groups, raw or RLE (PackBits)
// channels. Layers arrive bottom to top, like ours; a group is stored as
// a closing divider below its members and the group record above them.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

#include "firn/document.h"
#include "firn/io_psp.h"

namespace firn::io {

namespace {

struct Cursor {
    const std::vector<uint8_t>& d;
    size_t p = 0;
    bool ok(size_t n) const { return p + n <= d.size(); }
    uint8_t u8() { return ok(1) ? d[p++] : 0; }
    uint16_t u16() { const uint16_t v = ok(2) ? static_cast<uint16_t>((d[p] << 8) | d[p + 1]) : 0; p += 2; return v; }
    int16_t i16() { return static_cast<int16_t>(u16()); }
    uint32_t u32() { const uint32_t v = ok(4) ? (static_cast<uint32_t>(d[p]) << 24) | (d[p + 1] << 16) | (d[p + 2] << 8) | d[p + 3] : 0; p += 4; return v; }
    int32_t i32() { return static_cast<int32_t>(u32()); }
};

BlendMode blend_from_key(const char* k) {
    struct M { const char* key; BlendMode mode; };
    static const M table[] = {{"norm", BlendMode::Normal}, {"dark", BlendMode::Darken}, {"lite", BlendMode::Lighten}, {"hue ", BlendMode::Hue},
                              {"sat ", BlendMode::Saturation}, {"colr", BlendMode::Color}, {"lum ", BlendMode::Luminance}, {"mul ", BlendMode::Multiply},
                              {"scrn", BlendMode::Screen}, {"diss", BlendMode::Dissolve}, {"over", BlendMode::Overlay}, {"hLit", BlendMode::HardLight},
                              {"sLit", BlendMode::SoftLight}, {"diff", BlendMode::Difference}, {"div ", BlendMode::Dodge}, {"idiv", BlendMode::Burn},
                              {"smud", BlendMode::Exclusion}, {"pass", BlendMode::Normal}};
    for (const M& m : table) if (std::memcmp(k, m.key, 4) == 0) return m.mode;
    return BlendMode::Normal;
}

// Decodes one channel of `w` x `h` samples of `bytes_per_sample` bytes.
bool read_channel(Cursor& c, int w, int h, int bps, bool psb, std::vector<uint8_t>& out) {
    const uint16_t comp = c.u16();
    const size_t row = static_cast<size_t>(w) * bps;
    out.assign(row * h, 0);
    if (w <= 0 || h <= 0) return true;
    if (comp == 0) {
        if (!c.ok(row * h)) return false;
        std::memcpy(out.data(), c.d.data() + c.p, row * h);
        c.p += row * h;
        return true;
    }
    if (comp != 1) return false;  // zip variants are rare; the layer is skipped
    std::vector<size_t> lens(static_cast<size_t>(h));
    for (int y = 0; y < h; ++y) lens[y] = psb ? c.u32() : c.u16();
    for (int y = 0; y < h; ++y) {
        const size_t end = c.p + lens[y];
        if (end > c.d.size()) return false;
        uint8_t* dst = out.data() + static_cast<size_t>(y) * row;
        size_t o = 0;
        while (c.p < end && o < row) {
            const int8_t n = static_cast<int8_t>(c.d[c.p++]);
            if (n >= 0) {
                const size_t cnt = std::min<size_t>(static_cast<size_t>(n) + 1, row - o);
                if (c.p + cnt > end) return false;
                std::memcpy(dst + o, c.d.data() + c.p, cnt);
                c.p += static_cast<size_t>(n) + 1; o += cnt;
            } else if (n != -128) {
                const size_t cnt = std::min<size_t>(static_cast<size_t>(1 - n), row - o);
                if (c.p >= end) return false;
                std::memset(dst + o, c.d[c.p++], cnt);
                o += cnt;
            }
        }
        c.p = end;
    }
    return true;
}

// Places a channel (8-bit after conversion) into a document-sized plane.
void place(std::vector<uint8_t>& plane, int doc_w, int doc_h, const std::vector<uint8_t>& ch, int x0, int y0, int w, int h, int bps) {
    for (int y = 0; y < h; ++y) {
        const int dy = y0 + y;
        if (dy < 0 || dy >= doc_h) continue;
        for (int x = 0; x < w; ++x) {
            const int dx = x0 + x;
            if (dx < 0 || dx >= doc_w) continue;
            const size_t si = (static_cast<size_t>(y) * w + x) * bps;
            plane[static_cast<size_t>(dy) * doc_w + dx] = ch[si];  // big-endian: the high byte comes first
        }
    }
}

struct PsdLayer {
    std::string name;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::vector<std::pair<int16_t, size_t>> channels;  // id, byte length
    BlendMode blend = BlendMode::Normal;
    float opacity = 1.0f;
    bool visible = true;
    int mask_x0 = 0, mask_y0 = 0, mask_x1 = 0, mask_y1 = 0;
    uint8_t mask_default = 255;
    bool has_mask = false;
    int section = 0;  // lsct: 1/2 group (open/closed), 3 divider
};

}  // namespace

std::unique_ptr<Document> load_psd_from_memory(const uint8_t* data, size_t size, std::string* err, std::vector<std::string>* warnings) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return std::unique_ptr<Document>(); };
    const std::vector<uint8_t> d(data, data + size);
    Cursor c{d};
    if (d.size() < 26 || std::memcmp(d.data(), "8BPS", 4) != 0) return fail("not a Photoshop file");
    c.p = 4;
    const uint16_t version = c.u16();
    const bool psb = version == 2;
    if (version != 1 && version != 2) return fail("unsupported PSD version");
    c.p += 6;
    const int channels = c.u16();
    const int height = static_cast<int>(c.u32()), width = static_cast<int>(c.u32());
    const int depth = c.u16(), mode = c.u16();
    if (width <= 0 || height <= 0 || width > 65536 || height > 65536) return fail("bad image size");
    if (depth != 8 && depth != 16) return fail("only 8 and 16 bits per channel are supported");
    if (mode != 3 && mode != 1) return fail(mode == 4 ? "CMYK files are not supported: convert to RGB first" : "only RGB and grayscale files are supported");
    const int bps = depth / 8;
    const int color_channels = mode == 3 ? 3 : 1;
    // Color mode data and image resources: skipped.
    c.p += c.u32();
    c.p += c.u32();
    // Layer and mask information.
    size_t lm_len;
    if (psb) { c.u32(); lm_len = c.u32(); } else lm_len = c.u32();
    const size_t lm_end = c.p + lm_len;
    if (lm_end > d.size()) return fail("truncated layer section");

    auto doc = std::make_unique<Document>(width, height);
    std::vector<PsdLayer> layers;
    size_t data_at = 0;
    if (lm_len > 0) {
        if (psb) c.u32();
        const size_t li_len = c.u32();
        const size_t li_end = c.p + li_len;
        int count = c.i16();
        if (count < 0) count = -count;  // first alpha is the merged transparency
        for (int i = 0; i < count && c.ok(18); ++i) {
            PsdLayer L;
            L.y0 = c.i32(); L.x0 = c.i32(); L.y1 = c.i32(); L.x1 = c.i32();
            const int nch = c.u16();
            for (int k = 0; k < nch; ++k) {
                const int16_t id = c.i16();
                size_t len;
                if (psb) { c.u32(); len = c.u32(); } else len = c.u32();
                L.channels.emplace_back(id, len);
            }
            if (!c.ok(16) || std::memcmp(d.data() + c.p, "8BIM", 4) != 0) return fail("bad layer record");
            c.p += 4;
            char key[4]; std::memcpy(key, d.data() + c.p, 4); c.p += 4;
            L.blend = blend_from_key(key);
            L.opacity = c.u8() / 255.0f;
            c.u8();  // clipping
            const uint8_t flags = c.u8();
            L.visible = (flags & 2) == 0;
            c.u8();
            const size_t extra = c.u32();
            const size_t extra_end = c.p + extra;
            // Mask data.
            const size_t mlen = c.u32();
            if (mlen >= 20) {
                const size_t mend = c.p + mlen;
                L.mask_y0 = c.i32(); L.mask_x0 = c.i32(); L.mask_y1 = c.i32(); L.mask_x1 = c.i32();
                L.mask_default = c.u8();
                L.has_mask = true;
                c.p = mend;
            } else c.p += mlen;
            // Blending ranges.
            c.p += c.u32();
            // Pascal name padded to 4.
            const uint8_t nlen = c.u8();
            L.name.assign(reinterpret_cast<const char*>(d.data() + c.p), std::min<size_t>(nlen, d.size() - c.p));
            c.p += ((nlen + 1 + 3) / 4) * 4 - 1;
            // Additional info: unicode name and section dividers.
            while (c.p + 12 <= extra_end) {
                if (std::memcmp(d.data() + c.p, "8BIM", 4) != 0 && std::memcmp(d.data() + c.p, "8B64", 4) != 0) break;
                c.p += 4;
                char k2[4]; std::memcpy(k2, d.data() + c.p, 4); c.p += 4;
                size_t alen = c.u32();
                const size_t aend = c.p + alen;
                if (std::memcmp(k2, "luni", 4) == 0) {
                    const uint32_t n = c.u32();
                    std::string uni;
                    for (uint32_t k = 0; k < n && c.ok(2); ++k) {
                        const uint16_t cp = c.u16();
                        if (cp < 0x80) uni += static_cast<char>(cp);
                        else if (cp < 0x800) { uni += static_cast<char>(0xC0 | (cp >> 6)); uni += static_cast<char>(0x80 | (cp & 0x3F)); }
                        else { uni += static_cast<char>(0xE0 | (cp >> 12)); uni += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); uni += static_cast<char>(0x80 | (cp & 0x3F)); }
                    }
                    if (!uni.empty()) L.name = uni;
                } else if (std::memcmp(k2, "lsct", 4) == 0) {
                    L.section = static_cast<int>(c.u32());
                }
                c.p = aend + (alen & 1);
            }
            c.p = extra_end;
            layers.push_back(std::move(L));
        }
        data_at = c.p;
        (void)li_end;
    }

    // Channel image data follows the records, in the same order.
    c.p = data_at;
    std::vector<int> depth_of(layers.size(), 0);
    std::vector<int> group_open;  // indices (into `layers`) of dividers awaiting their group record
    for (size_t i = 0; i < layers.size(); ++i) {
        PsdLayer& L = layers[i];
        const int lw = L.x1 - L.x0, lh = L.y1 - L.y0;
        std::vector<uint8_t> r(static_cast<size_t>(width) * height, 0), g = r, b = r, a(r.size(), 255), m;
        bool got_color = false, bad = false;
        for (const auto& [id, len] : L.channels) {
            const size_t start = c.p;
            std::vector<uint8_t> ch;
            const bool is_mask = id == -2;
            const int cw = is_mask ? L.mask_x1 - L.mask_x0 : lw, chh = is_mask ? L.mask_y1 - L.mask_y0 : lh;
            if (!read_channel(c, cw, chh, bps, psb, ch)) { bad = true; }
            c.p = start + len;
            if (bad) continue;
            if (id == 0) { place(r, width, height, ch, L.x0, L.y0, lw, lh, bps); got_color = true; }
            else if (id == 1 && mode == 3) place(g, width, height, ch, L.x0, L.y0, lw, lh, bps);
            else if (id == 2 && mode == 3) place(b, width, height, ch, L.x0, L.y0, lw, lh, bps);
            else if (id == -1) { std::fill(a.begin(), a.end(), 0); place(a, width, height, ch, L.x0, L.y0, lw, lh, bps); }
            else if (id == -2 && L.has_mask) { m.assign(r.size(), L.mask_default); place(m, width, height, ch, L.mask_x0, L.mask_y0, cw, chh, bps); }
        }
        if (bad && warnings) warnings->push_back("Layer \"" + L.name + "\": unsupported channel compression, left empty");
        if (L.section == 3) { group_open.push_back(static_cast<int>(doc->layer_count())); continue; }  // divider: the group starts here
        Layer* out;
        if (L.section == 1 || L.section == 2) {
            // The group record closes the run its divider opened.
            const int at = group_open.empty() ? static_cast<int>(doc->layer_count()) : group_open.back();
            if (!group_open.empty()) group_open.pop_back();
            out = &doc->add_layer(L.name, at);
            out->type = LayerType::Group;
            out->pixels = Image();
            out->expanded = L.section == 1;
        } else {
            out = &doc->add_layer(L.name);
            if (mode == 1) { g = r; b = r; }
            uint8_t* px = out->pixels.data();
            for (size_t k = 0; k < r.size(); ++k) { px[k * 4] = r[k]; px[k * 4 + 1] = g[k]; px[k * 4 + 2] = b[k]; px[k * 4 + 3] = a[k]; }
            // Outside the layer's rectangle the layer is transparent.
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    if (x < L.x0 || x >= L.x1 || y < L.y0 || y >= L.y1) px[(static_cast<size_t>(y) * width + x) * 4 + 3] = 0;
            (void)got_color;
        }
        out->opacity = L.opacity;
        out->blend = L.blend;
        out->visible = L.visible;
        out->depth = static_cast<int>(group_open.size());
        if (!m.empty()) { out->mask = Mask(width, height); std::memcpy(out->mask.data(), m.data(), m.size()); }
    }
    // Members added while a group was open already got their depth from the divider count.
    if (doc->layer_count() == 0) {
        // No layers: the merged image (channels planar, same compression scheme for all).
        c.p = lm_end;
        const uint16_t comp = c.u16();
        c.p -= 2;
        Layer& bg = doc->add_layer("Background");
        bg.background = true;
        std::vector<std::vector<uint8_t>> planes;
        if (comp == 1) {
            // RLE: row lengths for every channel come first, then the rows.
            const size_t total_rows = static_cast<size_t>(channels) * height;
            c.u16();
            std::vector<size_t> lens(total_rows);
            for (size_t k = 0; k < total_rows; ++k) lens[k] = psb ? c.u32() : c.u16();
            for (int ch = 0; ch < channels; ++ch) {
                std::vector<uint8_t> plane(static_cast<size_t>(width) * height * bps, 0);
                for (int y = 0; y < height; ++y) {
                    const size_t end = c.p + lens[static_cast<size_t>(ch) * height + y];
                    uint8_t* dst = plane.data() + static_cast<size_t>(y) * width * bps;
                    const size_t row = static_cast<size_t>(width) * bps;
                    size_t o = 0;
                    while (c.p < end && o < row && end <= d.size()) {
                        const int8_t n = static_cast<int8_t>(d[c.p++]);
                        if (n >= 0) { const size_t cnt = std::min<size_t>(static_cast<size_t>(n) + 1, row - o); std::memcpy(dst + o, d.data() + c.p, std::min(cnt, d.size() - c.p)); c.p += static_cast<size_t>(n) + 1; o += cnt; }
                        else if (n != -128) { const size_t cnt = std::min<size_t>(static_cast<size_t>(1 - n), row - o); std::memset(dst + o, d[c.p++], cnt); o += cnt; }
                    }
                    c.p = end;
                }
                planes.push_back(std::move(plane));
            }
        } else {
            c.u16();
            const size_t plane_bytes = static_cast<size_t>(width) * height * bps;
            for (int ch = 0; ch < channels && c.ok(plane_bytes); ++ch) { planes.emplace_back(d.begin() + static_cast<long>(c.p), d.begin() + static_cast<long>(c.p + plane_bytes)); c.p += plane_bytes; }
        }
        uint8_t* px = bg.pixels.data();
        for (size_t k = 0; k < static_cast<size_t>(width) * height; ++k) {
            for (int ch = 0; ch < 3; ++ch) {
                const int src = mode == 3 ? ch : 0;
                px[k * 4 + ch] = src < static_cast<int>(planes.size()) ? planes[src][k * bps] : 0;
            }
            const int alpha_ch = color_channels;
            px[k * 4 + 3] = alpha_ch < static_cast<int>(planes.size()) ? planes[alpha_ch][k * bps] : 255;
        }
    } else {
        // The bottom layer is the Background when it is opaque and fills the canvas.
        Layer& bottom = doc->layer(0);
        if (bottom.type == LayerType::Raster && bottom.depth == 0) {
            bool opaque = true;
            for (size_t k = 3; k < bottom.pixels.size_bytes() && opaque; k += 4) opaque = bottom.pixels.data()[k] == 255;
            bottom.background = opaque;
        }
    }
    if (depth == 16 && warnings) warnings->push_back("16-bit PSD: read at 8 bits per channel");
    doc->set_active_layer(static_cast<int>(doc->layer_count()) - 1);
    return doc;
}

std::unique_ptr<Document> load_psd(const std::string& path, std::string* err, std::vector<std::string>* warnings) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot open " + path; return nullptr; }
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return load_psd_from_memory(d.data(), d.size(), err, warnings);
}


// --- Writing ----------------------------------------------------------------
// The layer stack, then the flattened composite every reader falls back on.
// 8-bit RGB only: a 16-bit document is written at 8 bits with a warning,
// which is what the reader does in the other direction.

namespace {

struct Out {
    std::vector<uint8_t> d;
    void u8(uint8_t v) { d.push_back(v); }
    void u16(uint16_t v) { d.push_back(static_cast<uint8_t>(v >> 8)); d.push_back(static_cast<uint8_t>(v)); }
    void i16(int16_t v) { u16(static_cast<uint16_t>(v)); }
    void u32(uint32_t v) { d.push_back(static_cast<uint8_t>(v >> 24)); d.push_back(static_cast<uint8_t>(v >> 16)); d.push_back(static_cast<uint8_t>(v >> 8)); d.push_back(static_cast<uint8_t>(v)); }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void bytes(const void* p, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(p); d.insert(d.end(), b, b + n); }
    void key(const char* k) { bytes(k, 4); }
    void patch32(size_t at, uint32_t v) { d[at] = static_cast<uint8_t>(v >> 24); d[at + 1] = static_cast<uint8_t>(v >> 16); d[at + 2] = static_cast<uint8_t>(v >> 8); d[at + 3] = static_cast<uint8_t>(v); }
};

const char* blend_key_of(BlendMode m) {
    switch (m) {
        case BlendMode::Darken: return "dark";
        case BlendMode::Lighten: return "lite";
        case BlendMode::Hue: return "hue ";
        case BlendMode::Saturation: return "sat ";
        case BlendMode::Color: return "colr";
        case BlendMode::Luminance: return "lum ";
        case BlendMode::Multiply: return "mul ";
        case BlendMode::Screen: return "scrn";
        case BlendMode::Dissolve: return "diss";
        case BlendMode::Overlay: return "over";
        case BlendMode::HardLight: return "hLit";
        case BlendMode::SoftLight: return "sLit";
        case BlendMode::Difference: return "diff";
        case BlendMode::Dodge: return "div ";
        case BlendMode::Burn: return "idiv";
        case BlendMode::Exclusion: return "smud";
        default: return "norm";
    }
}

// PackBits, the run encoding PSD calls compression 1. A run of three or more
// equal bytes is worth encoding; anything else goes in a literal run. Never
// crosses a row, because the row lengths are stored separately.
std::vector<uint8_t> pack_bits(const uint8_t* row, size_t n) {
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < n) {
        size_t run = 1;
        while (i + run < n && run < 128 && row[i + run] == row[i]) ++run;
        if (run >= 3) {
            out.push_back(static_cast<uint8_t>(static_cast<int8_t>(1 - static_cast<int>(run))));
            out.push_back(row[i]);
            i += run;
            continue;
        }
        // A literal run ends where three equal bytes begin.
        size_t lit = 0;
        while (i + lit < n && lit < 128) {
            if (i + lit + 2 < n && row[i + lit] == row[i + lit + 1] && row[i + lit] == row[i + lit + 2]) break;
            ++lit;
        }
        if (lit == 0) lit = 1;
        out.push_back(static_cast<uint8_t>(lit - 1));
        out.insert(out.end(), row + i, row + i + lit);
        i += lit;
    }
    return out;
}

// One channel as PSD stores it: the compression word, the per-row compressed
// lengths, then the rows.
std::vector<uint8_t> encode_channel(const std::vector<uint8_t>& plane, int w, int h) {
    Out o;
    o.u16(1);   // PackBits
    if (w <= 0 || h <= 0) return o.d;
    std::vector<std::vector<uint8_t>> rows(static_cast<size_t>(h));
    for (int y = 0; y < h; ++y) rows[static_cast<size_t>(y)] = pack_bits(plane.data() + static_cast<size_t>(y) * w, static_cast<size_t>(w));
    for (const auto& r : rows) o.u16(static_cast<uint16_t>(r.size()));
    for (const auto& r : rows) o.bytes(r.data(), r.size());
    return o.d;
}

void pascal_name(Out& o, const std::string& name) {
    // A Pascal string whose own length, the count byte included, is a
    // multiple of four. Padding to the buffer's absolute offset instead is
    // the same thing only when the field happens to start aligned, and is
    // why the first files this wrote were rejected as corrupt.
    const std::string n = name.substr(0, 255);
    const size_t begin = o.d.size();
    o.u8(static_cast<uint8_t>(n.size()));
    o.bytes(n.data(), n.size());
    while (((o.d.size() - begin) % 4) != 0) o.u8(0);
}

void unicode_name(Out& o, const std::string& name) {
    // The 'luni' block, which is the name readers actually prefer. UTF-8 in,
    // UTF-16BE out; anything outside the basic plane becomes a replacement,
    // since a layer name is not worth surrogate bookkeeping.
    std::vector<uint16_t> utf16;
    for (size_t i = 0; i < name.size();) {
        const uint8_t c = static_cast<uint8_t>(name[i]);
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0xF0) { cp = c & 0x07u; len = 4; }
        else if (c >= 0xE0) { cp = c & 0x0Fu; len = 3; }
        else if (c >= 0xC0) { cp = c & 0x1Fu; len = 2; }
        if (i + len > name.size()) { utf16.push_back('?'); break; }
        for (size_t k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<uint8_t>(name[i + k]) & 0x3Fu);
        utf16.push_back(cp > 0xFFFF ? 0xFFFD : static_cast<uint16_t>(cp));
        i += len;
    }
    o.key("8BIM");
    o.key("luni");
    const size_t len_at = o.d.size();
    o.u32(0);
    o.u32(static_cast<uint32_t>(utf16.size()));
    for (const uint16_t u : utf16) o.u16(u);
    // The declared length is the data alone; the pad that keeps the next
    // block even sits outside it, which is where a reader expects to skip it.
    const uint32_t len = static_cast<uint32_t>(o.d.size() - len_at - 4);
    o.patch32(len_at, len);
    if (len & 1) o.u8(0);
}

void section_divider(Out& o, int kind) {
    o.key("8BIM");
    o.key("lsct");
    o.u32(4);
    o.u32(static_cast<uint32_t>(kind));
}

// What the writer emits, flattened out of the document's tree: PSD wants a
// group's members between an opening divider below them and the group record
// above, which is the reverse of the order Firn keeps.
struct OutLayer {
    std::string name;
    const Layer* src = nullptr;   // null for a divider
    int section = 0;              // 0 raster, 1 group record, 3 divider
};

void flatten_stack(const Document& doc, size_t begin, size_t end, std::vector<OutLayer>& out) {
    for (size_t i = begin; i < end;) {
        const Layer& L = doc.layer(i);
        if (L.type == LayerType::Group) {
            const size_t stop = doc.group_end(i);
            out.push_back({"</Layer group>", nullptr, 3});
            flatten_stack(doc, i + 1, stop, out);
            out.push_back({L.name, &L, L.expanded ? 1 : 2});
            i = stop;
            continue;
        }
        out.push_back({L.name, &L, 0});
        ++i;
    }
}

}  // namespace

std::vector<uint8_t> save_psd_to_memory(const Document& doc, std::vector<std::string>* warnings) {
    const int w = doc.width(), h = doc.height();
    const Image flat = doc.composite();
    Out o;
    o.key("8BPS");
    o.u16(1);                       // version 1 (PSB is version 2 and 64-bit lengths)
    for (int i = 0; i < 6; ++i) o.u8(0);
    o.u16(4);                       // the merged image is RGBA
    o.u32(static_cast<uint32_t>(h));
    o.u32(static_cast<uint32_t>(w));
    o.u16(8);
    o.u16(3);                       // RGB
    o.u32(0);                       // colour mode data

    // Image resources: the ICC profile, when the document carries one.
    const size_t res_len_at = o.d.size();
    o.u32(0);
    if (!doc.icc().empty()) {
        o.key("8BIM");
        o.u16(0x040F);              // ICC profile
        o.u16(0);                   // empty Pascal name, padded to even
        o.u32(static_cast<uint32_t>(doc.icc().size()));
        o.bytes(doc.icc().data(), doc.icc().size());
        if (doc.icc().size() & 1) o.u8(0);
    }
    o.patch32(res_len_at, static_cast<uint32_t>(o.d.size() - res_len_at - 4));

    std::vector<OutLayer> stack;
    flatten_stack(doc, 0, doc.layer_count(), stack);
    if (warnings) {
        for (const OutLayer& e : stack)
            if (e.src && (e.src->is_adjustment() || e.src->type == LayerType::Adjustment))
                warnings->push_back("Layer \"" + e.name + "\": Photoshop cannot hold this adjustment, written as an empty layer");
        if (doc.bit_depth() == 16) warnings->push_back("16-bit document: written at 8 bits per channel");
    }

    // Every layer's channels, encoded up front so the record can carry their
    // lengths, which PSD stores before the data itself.
    struct Encoded { std::vector<std::vector<uint8_t>> channels; bool has_mask = false; };
    std::vector<Encoded> encoded(stack.size());
    for (size_t i = 0; i < stack.size(); ++i) {
        const Layer* L = stack[i].src;
        const bool pixels = stack[i].section == 0 && L && L->type != LayerType::Group && !L->pixels.empty();
        // A group record and a divider own no pixels, so their rectangle is
        // empty and their channels have to be encoded empty to match it. A
        // reader trusts the rectangle, not the stored length, when it walks
        // the rows: writing full-size channels behind an empty rectangle is
        // what made Photoshop and GIMP call the first version corrupt.
        const int cw = pixels ? w : 0, chh = pixels ? h : 0;
        const size_t n = static_cast<size_t>(w) * h;
        std::vector<uint8_t> r, g, b, a;
        if (pixels) {
            r.assign(n, 0); g.assign(n, 0); b.assign(n, 0); a.assign(n, 0);
            const uint8_t* px = L->pixels.data();
            for (size_t k = 0; k < n; ++k) { r[k] = px[k * 4]; g[k] = px[k * 4 + 1]; b[k] = px[k * 4 + 2]; a[k] = px[k * 4 + 3]; }
        }
        // Channel order in the record is alpha first, then red, green, blue.
        encoded[i].channels.push_back(encode_channel(a, cw, chh));
        encoded[i].channels.push_back(encode_channel(r, cw, chh));
        encoded[i].channels.push_back(encode_channel(g, cw, chh));
        encoded[i].channels.push_back(encode_channel(b, cw, chh));
        if (pixels && L->has_mask()) {
            std::vector<uint8_t> m(n, 255);
            std::memcpy(m.data(), L->mask.data(), std::min(n, L->mask.size()));
            encoded[i].channels.push_back(encode_channel(m, w, h));
            encoded[i].has_mask = true;
        }
    }

    // Layer and mask information.
    const size_t lm_len_at = o.d.size();
    o.u32(0);
    const size_t li_len_at = o.d.size();
    o.u32(0);
    // A negative count promises the merged image carries real transparency.
    o.i16(static_cast<int16_t>(-static_cast<int>(stack.size())));
    for (size_t i = 0; i < stack.size(); ++i) {
        const OutLayer& e = stack[i];
        const Layer* L = e.src;
        // A raster layer is written at full canvas size, which costs almost
        // nothing once its transparent rows run-length away.
        const bool empty_rect = encoded[i].channels[0].size() <= 2;
        o.i32(0); o.i32(0);
        o.i32(empty_rect ? 0 : h); o.i32(empty_rect ? 0 : w);
        o.u16(static_cast<uint16_t>(encoded[i].channels.size()));
        static const int16_t kIds[] = {-1, 0, 1, 2};
        for (size_t k = 0; k < encoded[i].channels.size(); ++k) {
            o.i16(k < 4 ? kIds[k] : static_cast<int16_t>(-2));
            o.u32(static_cast<uint32_t>(encoded[i].channels[k].size()));
        }
        o.key("8BIM");
        o.key(L ? blend_key_of(L->blend) : "norm");
        o.u8(L ? static_cast<uint8_t>(std::lround(std::clamp(L->opacity, 0.0f, 1.0f) * 255.0f)) : 255);
        o.u8(L && L->clipped ? 1 : 0);
        o.u8(static_cast<uint8_t>(L && !L->visible ? 2 : 0));
        o.u8(0);
        const size_t extra_at = o.d.size();
        o.u32(0);
        // Layer mask data: the rectangle plus the default outside it.
        if (encoded[i].has_mask) {
            o.u32(20);
            o.i32(0); o.i32(0); o.i32(h); o.i32(w);
            o.u8(0);      // colour outside the mask rectangle
            o.u8(0);      // flags
            o.u16(0);     // padding to the promised 20 bytes
        } else {
            o.u32(0);
        }
        o.u32(0);         // blending ranges
        pascal_name(o, e.name);
        unicode_name(o, e.name);
        if (e.section != 0) section_divider(o, e.section);
        o.patch32(extra_at, static_cast<uint32_t>(o.d.size() - extra_at - 4));
    }
    // Channel image data, in record order.
    for (const Encoded& e : encoded)
        for (const std::vector<uint8_t>& ch : e.channels) o.bytes(ch.data(), ch.size());
    if ((o.d.size() - li_len_at - 4) & 1) o.u8(0);    // the layer info block is padded to even
    o.patch32(li_len_at, static_cast<uint32_t>(o.d.size() - li_len_at - 4));
    o.u32(0);                                         // global layer mask info
    o.patch32(lm_len_at, static_cast<uint32_t>(o.d.size() - lm_len_at - 4));

    // The merged image: what a reader that ignores layers shows.
    {
        const size_t n = static_cast<size_t>(w) * h;
        std::vector<std::vector<uint8_t>> planes(4, std::vector<uint8_t>(n));
        const uint8_t* px = flat.data();
        for (size_t k = 0; k < n; ++k)
            for (int ch = 0; ch < 4; ++ch) planes[static_cast<size_t>(ch)][k] = px[k * 4 + ch];
        o.u16(1);   // PackBits
        std::vector<std::vector<uint8_t>> rows;
        rows.reserve(static_cast<size_t>(h) * 4);
        for (const auto& plane : planes)
            for (int y = 0; y < h; ++y) rows.push_back(pack_bits(plane.data() + static_cast<size_t>(y) * w, static_cast<size_t>(w)));
        for (const auto& r : rows) o.u16(static_cast<uint16_t>(r.size()));
        for (const auto& r : rows) o.bytes(r.data(), r.size());
    }
    return o.d;
}

bool save_psd(const Document& doc, const std::string& path, std::string* err, std::vector<std::string>* warnings) {
    const std::vector<uint8_t> d = save_psd_to_memory(doc, warnings);
    std::ofstream f(path, std::ios::binary);
    if (!f || !f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()))) {
        if (err) *err = "cannot write " + path;
        return false;
    }
    return true;
}

}  // namespace firn::io
