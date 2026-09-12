#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "firn/image.h"
#include "firn/json.h"
#include "firn/mask.h"

// Vector objects, modeled on what the original stores in its vector layers:
// paths of Bezier nodes with stroke and fill paint styles. Unknown bytes
// from files are carried verbatim so round trips stay faithful.
namespace firn::text { class Font; }

namespace firn::vec {

struct Node {
    float x = 0, y = 0;          // anchor
    float in_x = 0, in_y = 0;    // incoming control point (absolute)
    float out_x = 0, out_y = 0;  // outgoing control point (absolute)
    // The original's three flag bytes: [0] bit 0 = first node of a path,
    // [1] bit 0x80 = last node of a closed path, other bits = node type.
    uint8_t flags[3] = {0, 0, 0};
    bool is_corner() const { return in_x == x && in_y == y && out_x == x && out_y == y; }
};

struct Path {
    std::vector<Node> nodes;
    bool closed = true;
};

struct GradientStop { Color color; float pos = 0; float mid = 50; };       // pos/mid in percent
struct OpacityStop { float opacity = 100; float pos = 0; float mid = 50; };

// Gradient styles as the original numbers them in files (its dialog order).
enum class GradientStyle : uint16_t { Linear = 0, Rectangular = 1, Sunburst = 2, Radial = 3 };

struct Gradient {
    std::string name;
    std::vector<GradientStop> colors{{{0, 0, 0, 255}, 0, 50}, {{255, 255, 255, 255}, 100, 50}};
    std::vector<OpacityStop> opacities{{100, 0, 50}, {100, 100, 50}};
    GradientStyle style = GradientStyle::Linear;
    float angle = 0;               // degrees
    float center_x = 50, center_y = 50;  // percent of the object's bounds
    int repeats = 0;
    bool invert = false;
    Color at(float t) const;       // t in 0..1 (repeats/invert already applied by caller)
    // The color this gradient paints at a point inside the box, with its
    // style, angle, center, repeats and invert applied. Used for painting
    // and for the previews in the material dialog.
    Color at_point(float px, float py, float bx0, float by0, float bx1, float by1) const;
};

// A gradient as JSON, for the places that store one outside a paint style
// (an adjustment layer's gradient map, and anything else that follows).
json::Value gradient_json(const Gradient& g);
Gradient gradient_from_json(const json::Value& v);

struct PaintStyle {
    enum class Kind : uint16_t { None = 0, Solid = 1, Gradient = 2, Pattern = 3 };
    Kind kind = Kind::None;
    Color color{0, 0, 0, 255};
    Gradient gradient;
    std::shared_ptr<const Image> pattern;  // tiled
    float pattern_scale = 1.0f;
    float pattern_angle = 0.0f;
    // Texture: a tiled image whose lightness scales the coverage (white lets
    // all the paint through, black none), like the original's material texture.
    std::shared_ptr<const Image> texture;
    float texture_scale = 1.0f;
    float texture_angle = 0.0f;
    float texture_strength = 1.0f;  // 0 = ignore the texture, 1 = full effect
    bool enabled() const { return kind != Kind::None; }
};

struct LineStyle {
    // Caps as the original numbers them in .PspStyledLine files: 0 none,
    // 1 round, 2 square, 3 arrow (narrow), 4 arrow (wide), 7 fleur-de-lis,
    // 12 ball; sizes are multiples of the stroke width.
    uint32_t first_cap = 0, last_cap = 0;
    float first_w = 1, first_h = 1, last_w = 1, last_h = 1;
    float miter = 2;                // miter limit (unverified name)
    std::vector<float> dashes;      // alternating dash/gap lengths in stroke widths; empty = solid
    // Segment caps (the LINESTYLE block of a shape): type, width, height.
    uint32_t seg_start_cap = 0, seg_end_cap = 0;
    float seg_start_w = 1, seg_start_h = 1, seg_end_w = 1, seg_end_h = 1;
    uint32_t flag_a = 0, flag_b = 0, seg_caps_on = 0;   // carried through
    std::string name;
    bool dashed() const { return dashes.size() >= 2; }
};

struct TextInfo {
    std::string text;
    std::string font_path, font_family;
    float size = 48;
    int align = 0;                  // 0 left, 1 center, 2 right
    float rotation = 0;             // degrees
    bool antialias = true;
    float x = 0, y = 0;             // top-left of the laid-out block, image space
    float baseline = 0;             // first baseline below the block's top (set by text_outline_paths)
};

struct Object {
    std::string name;
    std::vector<Path> paths;
    PaintStyle stroke;              // first paint style block in files
    PaintStyle fill;                // second
    float stroke_width = 1.0f;
    float miter = 10.0f;            // miter limit
    LineStyle line;
    bool antialias = true;
    bool visible = true;
    bool selected = false;          // editor state, not saved
    bool is_text = false;
    TextInfo text;
    // Vector groups: a group object (type 5 in files) precedes its members.
    bool is_group = false;
    uint32_t group_count = 0;
    // Raw bytes carried from files for exact round trips (empty when created here).
    uint16_t file_type = 2;
    uint32_t file_a = 5, file_flags = 1, file_c = 0;
    std::vector<uint8_t> attr_raw, linestyle_raw;

    void bounds(float* x0, float* y0, float* x1, float* y1) const;  // of anchors and handles
    void translate(float dx, float dy);
    void transform(float a, float b, float c, float d, float tx, float ty);  // affine [a b; c d] + t
};

// Bounds of the flattened outline (anchors and curves, not handles); false when empty.
bool outline_bounds(const Object& o, float* x0, float* y0, float* x1, float* y1);
// True when (x, y) is inside a filled object or within `tolerance` of its outline.
bool hit_test(const Object& o, float x, float y, float tolerance);
// One past the last member of the group starting at index i (i + 1 for plain objects).
size_t group_end(const std::vector<Object>& objects, size_t i);
// Index of the group object containing i, or -1 when it is top level.
int group_of(const std::vector<Object>& objects, size_t i);

// Strokes a flattened path into a coverage mask, applying dashes and caps.
void stroke_polyline(Mask& acc, const std::vector<std::pair<float, float>>& pts, bool closed, float width, const LineStyle& line, bool antialias);

// Flattens a path into a polyline (image coordinates).
std::vector<std::pair<float, float>> flatten(const Path& p, float tolerance = 0.25f);

// Draws the objects onto `dst` (document-sized, composited over existing pixels).
void rasterize(const std::vector<Object>& objects, Image& dst);

// Renders a paint style through a coverage mask over the object's bounds.
void paint(Image& dst, const std::vector<uint8_t>& coverage, int w, int h, const PaintStyle& style,
           float bx0, float by0, float bx1, float by1);
// Glyph outlines of `t` laid out with the block's top-left at (0, 0), as
// closed paths (fills baseline). The app and the native reader share it.
std::vector<Path> text_outline_paths(const TextInfo& t, const text::Font& font, float* baseline = nullptr, std::vector<int>* glyph_ids = nullptr);
// Coverage multiplier of a style's texture at a pixel (1 when the style has none).
float texture_factor(const PaintStyle& style, float x, float y, float ox, float oy);

// Builders for the tools.
Object make_rectangle(float x0, float y0, float x1, float y1);
Object make_rounded_rectangle(float x0, float y0, float x1, float y1, float radius);
Object make_ellipse(float cx, float cy, float rx, float ry);
Object make_polygon(const std::vector<std::pair<float, float>>& pts, bool closed);

}  // namespace firn::vec
