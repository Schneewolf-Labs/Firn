// Photoshop PSD import: 8-bit and 16-bit RGB or grayscale, layers with
// opacity, blend mode, visibility, masks and groups, raw or RLE (PackBits)
// channels. Layers arrive bottom to top, like ours; a group is stored as
// a closing divider below its members and the group record above them.
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

std::unique_ptr<Document> load_psd(const std::string& path, std::string* err, std::vector<std::string>* warnings) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return std::unique_ptr<Document>(); };
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("cannot open " + path);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
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

}  // namespace firn::io
