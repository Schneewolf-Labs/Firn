#include "firn/text.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb/stb_truetype.h"

namespace fs = std::filesystem;

namespace firn::text {

struct Font::Impl {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
};

namespace {

// Decodes one UTF-8 code point; advances `i`.
uint32_t next_codepoint(const std::string& s, size_t& i) {
    const unsigned char c = s[i++];
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = c & (0x3F >> extra);
    while (extra-- > 0 && i < s.size()) cp = (cp << 6) | (s[i++] & 0x3F);
    return cp;
}

// Name table strings are UTF-16BE for the platform ids we ask for.
std::string name_string(const stbtt_fontinfo& f, int name_id) {
    int len = 0;
    const char* p = stbtt_GetFontNameString(&f, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, name_id);
    std::string out;
    if (p) {
        for (int i = 0; i + 1 < len; i += 2) {
            const uint16_t u = static_cast<uint16_t>((static_cast<unsigned char>(p[i]) << 8) | static_cast<unsigned char>(p[i + 1]));
            if (u < 0x80) out.push_back(static_cast<char>(u));
            else if (u < 0x800) { out.push_back(static_cast<char>(0xC0 | (u >> 6))); out.push_back(static_cast<char>(0x80 | (u & 0x3F))); }
            else { out.push_back(static_cast<char>(0xE0 | (u >> 12))); out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (u & 0x3F))); }
        }
        return out;
    }
    p = stbtt_GetFontNameString(&f, &len, STBTT_PLATFORM_ID_MAC, STBTT_MAC_EID_ROMAN, STBTT_MAC_LANG_ENGLISH, name_id);
    if (p) out.assign(p, len);
    return out;
}

}  // namespace

std::vector<FontInfo> list_fonts() {
    std::vector<fs::path> dirs;
    if (const char* extra = std::getenv("FIRN_FONT_DIRS")) {
        std::string s = extra;
        size_t start = 0;
        while (start <= s.size()) {
            const size_t end = s.find(':', start);
            dirs.emplace_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    if (const char* home = std::getenv("HOME")) {
        dirs.emplace_back(fs::path(home) / ".fonts");
        dirs.emplace_back(fs::path(home) / ".local/share/fonts");
        dirs.emplace_back(fs::path(home) / "Library/Fonts");        // macOS
    }
    dirs.emplace_back("/usr/share/fonts");
    dirs.emplace_back("/usr/local/share/fonts");
    dirs.emplace_back("/System/Library/Fonts");                     // macOS
    dirs.emplace_back("/System/Library/Fonts/Supplemental");
    dirs.emplace_back("/Library/Fonts");
    dirs.emplace_back("C:/Windows/Fonts");

    std::vector<FontInfo> out;
    for (const fs::path& d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) continue;
        for (const auto& de : fs::recursive_directory_iterator(d, fs::directory_options::skip_permission_denied, ec)) {
            if (!de.is_regular_file(ec)) continue;
            std::string ext = de.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
            if (ext != ".ttf" && ext != ".otf") continue;
            // Read only the head of the file for the name table; stb needs the
            // whole thing, but font files are small enough to load briefly.
            auto f = Font::load(de.path().string());
            if (f) out.push_back(f->info());
        }
    }
    std::sort(out.begin(), out.end(), [](const FontInfo& a, const FontInfo& b) {
        return a.family != b.family ? a.family < b.family : a.style < b.style;
    });
    return out;
}

Font::~Font() = default;

std::shared_ptr<Font> Font::load(const std::string& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot open " + path; return nullptr; }
    auto impl = std::make_unique<Impl>();
    impl->data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    const int off = stbtt_GetFontOffsetForIndex(impl->data.data(), 0);
    if (off < 0 || !stbtt_InitFont(&impl->info, impl->data.data(), off)) {
        if (err) *err = "not a usable TrueType/OpenType font: " + path;
        return nullptr;
    }
    std::shared_ptr<Font> font(new Font());
    font->info_.path = path;
    font->info_.family = name_string(impl->info, 1);
    font->info_.style = name_string(impl->info, 2);
    if (font->info_.family.empty()) font->info_.family = fs::path(path).stem().string();
    if (font->info_.style.empty()) font->info_.style = "Regular";
    font->impl_ = std::move(impl);
    return font;
}

Image Font::render(const std::string& utf8, float px, Color color, bool antialias, Align align,
                   float line_spacing, float kerning, Layout* layout) const {
    const stbtt_fontinfo& f = impl_->info;
    const float scale = stbtt_ScaleForPixelHeight(&f, std::max(px, 1.0f));
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&f, &ascent, &descent, &line_gap);
    const float line_h = (ascent - descent + line_gap) * scale * line_spacing;

    // Split lines, measure each.
    std::vector<std::string> lines;
    {
        size_t start = 0;
        while (true) {
            const size_t nl = utf8.find('\n', start);
            lines.push_back(utf8.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }
    struct Glyph { int index; float x; };
    std::vector<std::vector<Glyph>> placed(lines.size());
    std::vector<float> widths(lines.size(), 0.0f);
    for (size_t li = 0; li < lines.size(); ++li) {
        float x = 0.0f;
        int prev = 0;
        size_t i = 0;
        while (i < lines[li].size()) {
            const uint32_t cp = next_codepoint(lines[li], i);
            const int g = stbtt_FindGlyphIndex(&f, static_cast<int>(cp));
            if (prev) x += stbtt_GetGlyphKernAdvance(&f, prev, g) * scale;
            placed[li].push_back({g, x});
            int adv, lsb;
            stbtt_GetGlyphHMetrics(&f, g, &adv, &lsb);
            x += adv * scale + kerning;
            prev = g;
        }
        widths[li] = x;
    }
    const float block_w = *std::max_element(widths.begin(), widths.end());
    const int W = std::max(1, static_cast<int>(std::ceil(block_w)) + 2);
    const int H = std::max(1, static_cast<int>(std::ceil(line_h * lines.size() + (-descent) * scale)) + 2);
    Image out(W, H, {color.r, color.g, color.b, 0});
    std::vector<unsigned char> glyph_bitmap;

    for (size_t li = 0; li < lines.size(); ++li) {
        const float base_y = 1.0f + ascent * scale + li * line_h;
        const float indent = align == Align::Left ? 0.0f : align == Align::Center ? (block_w - widths[li]) * 0.5f : block_w - widths[li];
        for (const Glyph& g : placed[li]) {
            const float gx = 1.0f + indent + g.x;
            const float sub_x = antialias ? gx - std::floor(gx) : 0.0f;
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBoxSubpixel(&f, g.index, scale, scale, sub_x, 0.0f, &x0, &y0, &x1, &y1);
            const int gw = x1 - x0, gh = y1 - y0;
            if (gw <= 0 || gh <= 0) continue;
            glyph_bitmap.assign(static_cast<size_t>(gw) * gh, 0);
            stbtt_MakeGlyphBitmapSubpixel(&f, glyph_bitmap.data(), gw, gh, gw, scale, scale, sub_x, 0.0f, g.index);
            const int ox = static_cast<int>(std::floor(gx)) + x0, oy = static_cast<int>(std::floor(base_y)) + y0;
            for (int y = 0; y < gh; ++y) {
                const int py = oy + y;
                if (py < 0 || py >= H) continue;
                for (int x = 0; x < gw; ++x) {
                    const int px_ = ox + x;
                    if (px_ < 0 || px_ >= W) continue;
                    unsigned char a = glyph_bitmap[static_cast<size_t>(y) * gw + x];
                    if (!antialias) a = a >= 128 ? 255 : 0;
                    uint8_t& dst = out.data()[(static_cast<size_t>(py) * W + px_) * 4 + 3];
                    dst = std::max(dst, a);
                }
            }
        }
    }
    // Scale alpha by the color's alpha.
    if (color.a != 255)
        for (size_t i = 3; i < out.size_bytes(); i += 4) out.data()[i] = static_cast<uint8_t>(out.data()[i] * color.a / 255);
    if (layout) { layout->width = W; layout->height = H; layout->baseline = static_cast<int>(1.0f + ascent * scale); }
    return out;
}


std::vector<Font::Contour> Font::outlines(const std::string& utf8, float px, Align align,
                                          float line_spacing, float kerning, Layout* layout, std::vector<int>* glyph_ids) const {
    const stbtt_fontinfo& f = impl_->info;
    const float scale = stbtt_ScaleForPixelHeight(&f, std::max(px, 1.0f));
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&f, &ascent, &descent, &line_gap);
    const float line_h = (ascent - descent + line_gap) * scale * line_spacing;
    std::vector<std::string> lines;
    {
        size_t start = 0;
        while (true) {
            const size_t nl = utf8.find('\n', start);
            lines.push_back(utf8.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }
    struct Glyph { int index; float x; };
    std::vector<std::vector<Glyph>> placed(lines.size());
    std::vector<float> widths(lines.size(), 0.0f);
    for (size_t li = 0; li < lines.size(); ++li) {
        float x = 0.0f;
        int prev = 0;
        size_t i = 0;
        while (i < lines[li].size()) {
            const uint32_t cp = next_codepoint(lines[li], i);
            const int g = stbtt_FindGlyphIndex(&f, static_cast<int>(cp));
            if (prev) x += stbtt_GetGlyphKernAdvance(&f, prev, g) * scale;
            placed[li].push_back({g, x});
            int adv, lsb;
            stbtt_GetGlyphHMetrics(&f, g, &adv, &lsb);
            x += adv * scale + kerning;
            prev = g;
        }
        widths[li] = x;
    }
    const float block_w = *std::max_element(widths.begin(), widths.end());
    std::vector<Contour> out;
    int glyph_no = 0;
    for (size_t li = 0; li < lines.size(); ++li) {
        const float base_y = 1.0f + ascent * scale + li * line_h;
        const float indent = align == Align::Left ? 0.0f : align == Align::Center ? (block_w - widths[li]) * 0.5f : block_w - widths[li];
        for (const Glyph& g : placed[li]) {
            const float gx = 1.0f + indent + g.x;
            const int this_glyph = glyph_no++;
            stbtt_vertex* verts = nullptr;
            const int n = stbtt_GetGlyphShape(&f, g.index, &verts);
            Contour cur;
            auto map_x = [&](short v) { return gx + v * scale; };
            auto map_y = [&](short v) { return base_y - v * scale; };
            auto flush = [&]() { if (cur.size() >= 2) { out.push_back(cur); if (glyph_ids) glyph_ids->push_back(this_glyph); } cur.clear(); };
            for (int i = 0; i < n; ++i) {
                const stbtt_vertex& v = verts[i];
                const float x = map_x(v.x), y = map_y(v.y);
                if (v.type == STBTT_vmove) {
                    flush();
                    cur.push_back({x, y, x, y, x, y});
                } else if (cur.empty()) {
                    cur.push_back({x, y, x, y, x, y});
                } else if (v.type == STBTT_vline) {
                    cur.push_back({x, y, x, y, x, y});
                } else if (v.type == STBTT_vcurve) {
                    OutlinePoint& a = cur.back();
                    const float cx = map_x(v.cx), cy = map_y(v.cy);
                    a.out_x = a.x + 2.0f / 3.0f * (cx - a.x); a.out_y = a.y + 2.0f / 3.0f * (cy - a.y);
                    cur.push_back({x, y, x + 2.0f / 3.0f * (cx - x), y + 2.0f / 3.0f * (cy - y), x, y});
                } else if (v.type == STBTT_vcubic) {
                    OutlinePoint& a = cur.back();
                    a.out_x = map_x(v.cx); a.out_y = map_y(v.cy);
                    cur.push_back({x, y, map_x(v.cx1), map_y(v.cy1), x, y});
                }
            }
            flush();
            stbtt_FreeShape(&f, verts);
        }
    }
    // Contours end on their start point; drop that duplicate so closing the
    // path does not add a zero-length segment (keep its incoming handle).
    for (Contour& c : out) {
        if (c.size() >= 2 && std::abs(c.front().x - c.back().x) < 1e-3f && std::abs(c.front().y - c.back().y) < 1e-3f) {
            c.front().in_x = c.back().in_x; c.front().in_y = c.back().in_y;
            c.pop_back();
        }
    }
    if (layout) {
        layout->width = std::max(1, static_cast<int>(std::ceil(block_w)) + 2);
        layout->height = std::max(1, static_cast<int>(std::ceil(line_h * lines.size() + (-descent) * scale)) + 2);
        layout->baseline = static_cast<int>(1.0f + ascent * scale);
    }
    return out;
}

}  // namespace firn::text
