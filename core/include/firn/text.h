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

private:
    Font() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    FontInfo info_;
};

}  // namespace firn::text
