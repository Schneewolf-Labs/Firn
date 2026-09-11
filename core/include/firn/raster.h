#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "firn/image.h"

// Low-level pixel operations shared by tools and commands. Everything here
// works on a single straight-alpha RGBA8 Image and knows nothing about
// layers or undo.
namespace firn {
class Mask;
}

namespace firn::raster {

struct Rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // half-open [x0,x1) x [y0,y1)
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    Rect clipped(int w, int h) const;
    Rect united(const Rect& o) const;
};

// A custom brush tip: coverage 0..1 per pixel, scaled to the brush size.
struct BrushTip {
    int width = 0, height = 0;
    std::vector<float> coverage;
    // Builds a tip from an image: darker pixels cover more (white = nothing),
    // weighted by alpha, as the original's brush files are stored.
    static std::shared_ptr<const BrushTip> from_image(const Image& img);
    // Builds a texture: lighter pixels let more paint through.
    static std::shared_ptr<const BrushTip> texture_from_image(const Image& img);
};

struct Brush {
    float size = 16.0f;      // diameter in pixels
    float hardness = 0.5f;   // 0 = fully soft falloff, 1 = hard edge (1px AA)
    float opacity = 1.0f;    // 0..1, applied per stroke, not per stamp
    float step = 0.25f;      // stamp spacing as a fraction of size
    bool accumulate = false; // airbrush: coverage builds up per stamp by `flow`
    float flow = 0.1f;
    bool square = false;     // square stamp instead of round
    std::shared_ptr<const BrushTip> tip;  // custom tip; overrides round/square and hardness
    // Paper texture: a tiled coverage map (light = more paint) mixed into every
    // stamp by `texture_strength` (0 = ignore texture, 1 = full).
    std::shared_ptr<const BrushTip> texture;
    float texture_strength = 0.5f;
};

// Paint: color. Erase: clear alpha. Clone: pixels from a source image at an
// offset. Filter: a per-pixel function of the existing pixel (retouch tools).
enum class StrokeMode { Paint, Erase, Clone, Filter, Heal };

// Symmetry painting: every stamp is repeated mirrored across a vertical
// axis (Horizontal), a horizontal axis (Vertical) or both through
// (cx, cy), or rotated `count` times about that point (Rotational;
// Kaleidoscope adds a mirrored copy of each rotation).
struct Symmetry {
    enum class Mode { None, Horizontal, Vertical, Both, Rotational, Kaleidoscope };
    Mode mode = Mode::None;
    float cx = 0.0f, cy = 0.0f;
    int count = 6;
    // Every point a stamp at (x, y) lands on, the original first.
    std::vector<std::pair<float, float>> points(float x, float y) const;
};

// A brush stroke in progress. Same semantics as the original: opacity is per stroke, so
// overlapping stamps do not build up. Coverage accumulates as max() into a
// mask and the result is base composited with color*mask*opacity.
class Stroke {
public:
    // `clip` (optional, must outlive the stroke) limits painting to a selection.
    Stroke(const Image& base, Brush brush, Color color, StrokeMode mode, const Mask* clip = nullptr);

    // Add a point (image coordinates, sub-pixel ok). Stamps are placed along
    // the segment from the previous point at the brush spacing. `pressure`
    // (0..1, from a pen) scales the stamps per set_pressure_response.
    void add_point(float x, float y, float pressure = 1.0f);
    // One stamp regardless of spacing (airbrush ticks while the mouse rests).
    void stamp_at(float x, float y, float pressure = 1.0f) { apply_pressure(pressure); stamp(x, y); }
    // What pen pressure drives: the stamp size, the coverage, both, or nothing.
    void set_pressure_response(bool size, bool opacity) { pressure_size_ = size; pressure_alpha_ = opacity; }
    // Repeats every stamp per `s` (see Symmetry). Coverage still maxes, so
    // copies meeting at the axis do not build up.
    void set_symmetry(const Symmetry& s) { symmetry_ = s; }

    // Clone source: `src` must outlive the stroke; a destination pixel (x, y)
    // takes src(x + ox, y + oy). Heal uses the same source but blends its
    // texture into the target's colors (a seamless clone per stamp).
    void set_clone_source(const Image* src, int ox, int oy) { clone_ = src; clone_ox_ = ox; clone_oy_ = oy; }
    void set_filter(std::function<Color(Color)> f) { filter_ = std::move(f); }
    // Filter with access to the untouched base image around the pixel.
    void set_area_filter(std::function<Color(const Image&, int, int)> f) { area_filter_ = std::move(f); }

    // Write everything touched since the last render into `dst`, which must
    // be a copy of `base` (or the previous render target). Returns the rect
    // that was updated.
    Rect render(Image& dst);

    const Image& base() const { return base_; }

private:
    void stamp(float cx, float cy);      // fans out per symmetry_
    void stamp_one(float cx, float cy);
    void stamp_tip(float cx, float cy);
    Symmetry symmetry_;
    Image base_;
    Brush brush_;
    Color color_;
    StrokeMode mode_;
    const Mask* clip_;
    const Image* clone_ = nullptr;
    int clone_ox_ = 0, clone_oy_ = 0;
    std::function<Color(Color)> filter_;
    std::function<Color(const Image&, int, int)> area_filter_;
    std::vector<float> mask_;
    Rect pending_;
    bool has_last_ = false;
    float last_x_ = 0, last_y_ = 0, last_pressure_ = 1.0f;
    float carry_ = 0;  // distance left over from the previous segment
    bool pressure_size_ = false, pressure_alpha_ = false;
    float size_scale_ = 1.0f, alpha_scale_ = 1.0f;   // from the current pressure
    void apply_pressure(float p);
    Image heal_;                 // healed pixels per stamp box (Heal mode)
    void heal_box(const Rect& box);
};

// Seamless clone: fills `dst` inside `region` (coverage > 0) with `src`'s
// texture bent to match `dst`'s colors at the region's edge, by solving
// Laplace's equation for the difference image (the classic heal). `src`
// is sampled at (x + ox, y + oy). Only pixels within `box` are touched.
void heal(Image& dst, const Image& src, int ox, int oy, const std::vector<float>& region, const Rect& box);

// Turns `color` into transparency: pixels at the color become clear, others
// keep the part of their color the reference cannot explain, unpremultiplied.
// Between the thresholds (0..1 on the extracted alpha) the result ramps.
void color_to_alpha(Image& img, Color color, float transparency_threshold = 0.0f, float opacity_threshold = 1.0f);

// 4-connected flood fill from (x,y). Pixels whose max channel difference to
// the seed is <= tolerance are filled with `color` composited at `opacity`.
// Returns the bounding rect of changed pixels.
Rect flood_fill(Image& img, int x, int y, Color color, int tolerance, float opacity = 1.0f,
                const Mask* clip = nullptr);

// dst = lerp(before, dst, mask/255): keeps `before` where the mask is 0. Used
// to confine whole-layer commands to the selection.
void apply_through_mask(Image& dst, const Image& before, const Mask& mask);

// Composites `color` over `dst` with coverage from `shape` (and `clip`, if any).
void paint_mask(Image& dst, const Mask& shape, Color color, const Mask* clip = nullptr);

// Composite `c` over the pixel at (x,y) with extra coverage in 0..1.
void blend_over(Image& img, int x, int y, Color c, float coverage);

// Whole-image operations used by commands.
void grayscale(Image& img);
void brightness_contrast(Image& img, int brightness, int contrast);  // -255..255, -100..100
void gaussian_blur(Image& img, float radius);
void box_blur(Image& img, int radius);  // the original's "Average"
void flip_vertical(Image& img);
void mirror_horizontal(Image& img);

}  // namespace firn::raster

// --- Geometry -----------------------------------------------------------
namespace firn::raster {

// EdgeDirected enlarges along edges rather than across them (directional
// cubic convolution), which keeps diagonals clean where Bicubic softens or
// staircases them. It only applies when enlarging; shrinking falls back to
// Bicubic, which is already an area average.
// Smart picks per resize, the way the original's "Smart size" does:
// Lanczos for a reduction or a modest enlargement, which keeps the most
// detail on a photograph, and the edge-directed filter past a doubling,
// where staircasing starts to show instead.
// Lanczos is the sharpest of the separable filters and the usual choice for
// photographic reduction, at the cost of slight ringing on hard edges;
// Mitchell is the soft, ringing-free one.
enum class Filter { Nearest, Bilinear, Bicubic, EdgeDirected, Smart, Lanczos, Mitchell };

// Resamples to (w, h). Works in premultiplied alpha; when shrinking, the
// filter support widens so every source pixel contributes (area average).
Image resample(const Image& src, int w, int h, Filter filter);
// The enlargement behind Filter::EdgeDirected, also callable directly.
Image resample_edge_directed(const Image& src, int w, int h);

// Copies the rect (clipped to the image; outside is transparent).
// Selections > Matting. `remove_matte` undoes a composite against a solid
// color on semi-transparent pixels (black or white halos); `defringe`
// replaces the color of edge pixels within `width` px of opaque ones with
// their nearest opaque neighbor's color.
void remove_matte(Image& img, Color matte);
void defringe(Image& img, int width);
Image crop(const Image& src, Rect r);
// Same size, content moved by (dx, dy); uncovered area is transparent.
Image shifted(const Image& src, int dx, int dy);

// Exact rotations. Positive quarter turns are clockwise.
Image rotate_quarter(const Image& src, int quarter_turns);

// Rotates by `degrees` clockwise about the center, expanding the canvas to
// fit; uncovered area is transparent. Bilinear, premultiplied.
Image rotate(const Image& src, float degrees);

// Size of the canvas rotate() produces for a given source size.
void rotated_size(int w, int h, float degrees, int* out_w, int* out_h);

// Colors and palettes (Image > Count Colors, Decrease Color Depth, Palette).
size_t count_colors(const Image& img);                                        // distinct RGB among opaque pixels
std::vector<Color> median_cut_palette(const Image& img, int colors);          // 2..256 entries
void apply_palette(Image& img, const std::vector<Color>& palette, bool dither);   // nearest color, optional error diffusion
void to_monochrome(Image& img, bool dither);                                  // black and white
// Channel splitting and combining: mode 0 RGB, 1 HSL, 2 CMYK (four planes).
std::vector<Image> split_channels(const Image& img, int mode);
Image combine_channels(const std::vector<Image>& planes, int mode);
// Image Arithmetic: per-channel (a op b) / divisor + bias, clipped or wrapped.
enum class ArithOp { Add, Subtract, Multiply, Difference, Lightest, Darkest, Average, And, Or, Xor };
Image arithmetic(const Image& a, const Image& b, ArithOp op, float divisor, int bias, bool clip, int channel /* 0 all, 1 r, 2 g, 3 b */);

// Projective warps for the Deform, Straighten and Perspective tools.
// A homography H maps source (x, y, 1) to destination; points are
// (TL, TR, BR, BL) corner lists.
struct Quad { float x[4] = {0, 0, 0, 0}, y[4] = {0, 0, 0, 0}; };
bool homography(const Quad& from, const Quad& to, float H[9]);       // false when degenerate
bool invert3(const float H[9], float out[9]);
void apply_homography(const float H[9], float x, float y, float* ox, float* oy);
// Warps `src` by H into a w x h image (premultiplied bilinear, transparent outside).
Image warp(const Image& src, const float H[9], int w, int h);
// Bounds of the pixels with alpha > 0; the full image when it is all transparent.
Rect content_bounds(const Image& img);

// Mesh Warp: the (cols+1) x (rows+1) grid of nodes lists destination
// positions of the regular source grid over the image, row-major.
Image mesh_warp(const Image& src, int cols, int rows, const std::vector<std::pair<float, float>>& nodes);
// Displacement warp for the Warp Brush: out(x, y) = src(x + dx, y + dy).
Image displace(const Image& src, const std::vector<float>& dx, const std::vector<float>& dy);
// Scratch Remover: fills the strip of `width` along the line from pixels
// sampled just outside both edges.
void scratch_fill(Image& img, float x0, float y0, float x1, float y1, float width);

// Same resample for 8-bit masks (selection follows the geometry).
void resample_mask(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh);

}  // namespace firn::raster
