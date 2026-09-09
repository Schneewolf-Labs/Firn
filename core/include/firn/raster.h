#pragma once
#include <cstdint>
#include <functional>
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

struct Brush {
    float size = 16.0f;      // diameter in pixels
    float hardness = 0.5f;   // 0 = fully soft falloff, 1 = hard edge (1px AA)
    float opacity = 1.0f;    // 0..1, applied per stroke, not per stamp
    float step = 0.25f;      // stamp spacing as a fraction of size
    bool accumulate = false; // airbrush: coverage builds up per stamp by `flow`
    float flow = 0.1f;
    bool square = false;     // square stamp instead of round
};

// Paint: color. Erase: clear alpha. Clone: pixels from a source image at an
// offset. Filter: a per-pixel function of the existing pixel (retouch tools).
enum class StrokeMode { Paint, Erase, Clone, Filter };

// A brush stroke in progress. Same semantics as the original: opacity is per stroke, so
// overlapping stamps do not build up. Coverage accumulates as max() into a
// mask and the result is base composited with color*mask*opacity.
class Stroke {
public:
    // `clip` (optional, must outlive the stroke) limits painting to a selection.
    Stroke(const Image& base, Brush brush, Color color, StrokeMode mode, const Mask* clip = nullptr);

    // Add a point (image coordinates, sub-pixel ok). Stamps are placed along
    // the segment from the previous point at the brush spacing.
    void add_point(float x, float y);
    // One stamp regardless of spacing (airbrush ticks while the mouse rests).
    void stamp_at(float x, float y) { stamp(x, y); }

    // Clone source: `src` must outlive the stroke; a destination pixel (x, y)
    // takes src(x + ox, y + oy).
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
    void stamp(float cx, float cy);
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
    float last_x_ = 0, last_y_ = 0;
    float carry_ = 0;  // distance left over from the previous segment
};

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

enum class Filter { Nearest, Bilinear, Bicubic };

// Resamples to (w, h). Works in premultiplied alpha; when shrinking, the
// filter support widens so every source pixel contributes (area average).
Image resample(const Image& src, int w, int h, Filter filter);

// Copies the rect (clipped to the image; outside is transparent).
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

// Same resample for 8-bit masks (selection follows the geometry).
void resample_mask(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh);

}  // namespace firn::raster
