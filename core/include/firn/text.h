#pragma once
#include <memory>
#include <string>
#include <vector>

#include "firn/image.h"

// Text rasterization through stb_truetype. A Font wraps one .ttf/.otf file.
namespace firn::text {

struct FontInfo {
    std::string path;
    std::string family;   // e.g. "DejaVu Sans"
    std::string style;    // e.g. "Bold Italic"
};

// Scans the usual font directories (and FIRN_FONT_DIRS, colon-separated).
std::vector<FontInfo> list_fonts();

class Font {
public:
    static std::shared_ptr<Font> load(const std::string& path, std::string* err = nullptr);
    ~Font();
    const FontInfo& info() const { return info_; }

    struct Layout {
        int width = 0, height = 0;   // pixel bounds of the rendered block
        int baseline = 0;            // y of the first baseline within the block
    };
    enum class Align { Left, Center, Right };

    // Renders UTF-8 text ('\n' separated lines) at `px` pixels tall into an
    // RGBA image of the block's size, in `color`, antialiased or not.
    Image render(const std::string& utf8, float px, Color color, bool antialias, Align align,
                 float line_spacing = 1.0f, float kerning = 0.0f, Layout* layout = nullptr) const;

    // Glyph outlines as closed cubic contours, laid out like render() with
    // the block's top-left at (0, 0). Quadratic segments are raised to cubics.
    struct OutlinePoint { float x, y, in_x, in_y, out_x, out_y; };
    using Contour = std::vector<OutlinePoint>;
    std::vector<Contour> outlines(const std::string& utf8, float px, Align align,
                                  float line_spacing = 1.0f, float kerning = 0.0f, Layout* layout = nullptr,
                                  std::vector<int>* glyph_ids = nullptr) const;   // per contour: index of its glyph in the text

private:
    Font() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    FontInfo info_;
};

}  // namespace firn::text
