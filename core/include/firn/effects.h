#pragma once
#include <cstdint>

#include "firn/image.h"

// Spatial effects, named after the original's Effects and Adjust menu
// entries. In place on straight-alpha RGBA8; color work is done in
// premultiplied space where transparency matters.
namespace firn::effects {

// Convolves RGB with a 3x3 kernel (row-major), divided by `divisor`, plus `bias`.
void convolve3(Image& img, const float kernel[9], float divisor = 1.0f, float bias = 0.0f);

void sharpen(Image& img);
void sharpen_more(Image& img);
void unsharp_mask(Image& img, float radius, int strength /*1..500 %*/, int clipping /*0..100*/);
void blur_more(Image& img);
void soften(Image& img);
void soften_more(Image& img);

void median(Image& img, int radius);                       // aperture = 2r+1
void motion_blur(Image& img, float angle_degrees, int strength /*1..100 px*/);
void mosaic(Image& img, int block_w, int block_h);

void add_noise(Image& img, int percent /*0..100*/, bool gaussian, bool monochrome, uint32_t seed = 1);

void find_edges(Image& img);      // Sobel magnitude on luma, kept per channel
void enhance_edges(Image& img);
void enhance_edges_more(Image& img);
void emboss(Image& img);
void erode(Image& img);           // 3x3 minimum
void dilate(Image& img);          // 3x3 maximum

// Composites a blurred, offset copy of the alpha mask in `color` beneath the
// existing pixels. Meant for layers with transparency.
void drop_shadow(Image& img, int offset_x, int offset_y, float opacity, float blur, Color color);

// --- Distortion (inverse-mapped, bilinear, premultiplied) --------------
// Wave: horizontal displacement varies with y, vertical with x.
void wave(Image& img, float h_amplitude, float h_wavelength, float v_amplitude, float v_wavelength);
// Pinch (strength > 0) pulls towards the center, Punch (< 0) pushes out. -100..100.
void pinch(Image& img, int strength);
// Twirl about the center by `degrees` at the middle, falling off to the edge.
void twirl(Image& img, float degrees);

// Ripple: concentric waves from the center. Spherize: bulge (positive) or
// dish (negative) inside the largest inscribed circle. Lens: barrel
// (positive) or pincushion (negative) distortion, -100..100.
void ripple(Image& img, float amplitude, float wavelength);
void spherize(Image& img, int strength);
void lens_distortion(Image& img, int strength);

// Kaleidoscope: mirrors a wedge of `petals` around the center, rotated by
// `angle` degrees. Sunburst: a light source at (fx, fy) in 0..1 with rays.
void kaleidoscope(Image& img, int petals, float angle_degrees, float radius_percent);
void sunburst(Image& img, float fx, float fy, float brightness, int rays, float ray_brightness, Color color, uint32_t seed = 7);

// Halftone: dots on a grid of `cell` px sized by luminance, ink over paper.
void halftone(Image& img, int cell, float angle_degrees, Color ink, Color paper);
// Chrome: grayscale run through a repeated brightness ramp (`bands`).
void chrome(Image& img, int bands, float brightness);

// --- 3D ---------------------------------------------------------------
// Buttonize: bevelled border of `width` px in `color` at `opacity`;
// `transparent_edge` lightens/darkens the image instead of painting color.
void buttonize(Image& img, int width, float opacity, Color color, bool transparent_edge);
// Inner Bevel on the region where `region` (document-sized mask, 0..255,
// or the alpha channel when null) is set: shades by a height ramp of
// `width` px lit from `angle` degrees.
void inner_bevel(Image& img, const uint8_t* region, int width, float angle_degrees, float depth, float ambient);
// Outer Bevel: raises a rim of `width` px around the region (alpha when
// `region` is null) in `color`, lit from `angle`; adds pixels outside.
void outer_bevel(Image& img, const uint8_t* region, int width, float angle_degrees, float depth, Color color);
// Cutout: a shadow cast into the region by its edge (the inverse of Drop Shadow).
void cutout(Image& img, const uint8_t* region, int offset_x, int offset_y, float opacity, float blur, Color color);


// --- Geometric, distortion, reflection and image effects (effects_geo.cpp) ---
// Edge handling: 0 wrap, 1 repeat, 2 fill color, 3 transparent.
struct Edge { int mode = 1; Color fill{0, 0, 0, 255}; };
void curlicues(Image& img, int columns, int rows, int radius, int strength);
// Displacement from `map` (stretched over the image): luma offsets both axes
// (one-dimensional) or red/green offset x/y; intensity in percent of size.
void displacement_map(Image& img, const Image& map, float intensity, bool two_d, float blur, Edge edge);
void polar_coordinates(Image& img, bool rect_to_polar, Edge edge);
void spiky_halo(Image& img, float radius_percent, int spikes, float offset_percent, int bend);
void warp(Image& img, float cx_percent, float cy_percent, float size_percent, int strength);
void wind(Image& img, bool from_left, int strength);
void circle(Image& img, Edge edge);
void cylinder(Image& img, bool vertical, int strength);
void pentagon(Image& img, Edge edge);
void perspective(Image& img, bool vertical, int distortion, Edge edge);
void skew(Image& img, bool vertical, int angle, Edge edge);
void feedback(Image& img, int opacity, int intensity, float cx_percent, float cy_percent, bool elliptical);
void rotating_mirror(Image& img, float angle, float cx_percent, float cy_percent, Edge edge);
void pattern(Image& img, float angle, float cx_percent, float cy_percent, float scale_percent, int rotation);
void offset(Image& img, int dx, int dy, Edge edge);
void seamless_tiling(Image& img, int method /*0 edge, 1 corner, 2 mirror*/, int direction /*0 both, 1 horizontal, 2 vertical*/, int transition);
void page_curl(Image& img, int corner /*0 top-left .. 3 bottom-right*/, float width_percent, float height_percent, int radius, Color back, Color fill, bool transparent_fill);

// --- Artistic, illumination, texture and art media effects (effects_art.cpp) ---
void aged_newspaper(Image& img, int amount /*1..100*/);
void balls_and_bubbles(Image& img, int count, int min_size, int max_size, int opacity, bool bubbles, Color color, uint32_t seed);
void colored_edges(Image& img, int luminance, int blur, Color color);
void colored_foil(Image& img, int blur, int detail, Color color, float angle);
void contours(Image& img, int luminance, int blur, int detail, Color color);
void enamel(Image& img, int blur, int detail, int density, float angle, Color color);
void glowing_edges(Image& img, int intensity, int sharpness);
void hot_wax(Image& img, Color wax);
void magnifying_lens(Image& img, float cx_percent, float cy_percent, float size_percent, int refraction, int shading);
void neon_glow(Image& img, int detail, int opacity);
void topography(Image& img, int width, int density, float angle, Color color);
// Lights: up to five, each {x %, y %, direction degrees, cone degrees, intensity, color}.
struct Light { bool on = false; float x = 50, y = 50, direction = 270, cone = 60; int intensity = 50; Color color{255, 255, 255, 255}; };
void lights(Image& img, const Light* lights, int count, int darkness);
void blinds(Image& img, int width, int opacity, bool horizontal, bool light_from_left, Color color);
void leather(Image& img, bool rough, int color_amount, float angle, int blur, int transparency, Color color, uint32_t seed);
void fur(Image& img, int blur, int density, int length, int transparency, uint32_t seed);
void mosaic_antique(Image& img, int columns, int rows, int symmetric, int diffusion, int grout_width, int grout_transparency);
void mosaic_glass(Image& img, int columns, int rows, int curvature, int edge_width, int grout_transparency);
void polished_stone(Image& img, int blur, int detail, float angle, int color_amount, Color color);
void sandstone(Image& img, int blur, int detail, float angle, Color color, uint32_t seed);
void sculpture(Image& img, int smoothness, int depth, float angle, Color color);
void soft_plastic(Image& img, int blur, int detail, int density, float angle, Color color);
void straw_wall(Image& img, int blur, int detail, int density, float angle, Color color, uint32_t seed);
void texture(Image& img, const Image& bump, int size_percent, int smoothness, int depth, float angle, Color color);
void tiles(Image& img, int shape /*0 square, 1 hexagon, 2 triangle*/, int size, int border, int smoothness, int depth, float angle, Color color);
void weave(Image& img, int gap, int width, int opacity, Color gap_color, Color weave_color, bool fill_gaps);
void black_pencil(Image& img, int detail, int opacity);
void brush_strokes(Image& img, int length, int density, int width, int opacity, uint32_t seed);
void charcoal(Image& img, int detail, int opacity);
void colored_chalk(Image& img, int detail, int opacity);
void colored_pencil(Image& img, int detail, int opacity);
void pencil(Image& img, int luminance, int blur, Color color);
void user_defined_filter(Image& img, const float kernel[25], float divisor, float bias);
}  // namespace firn::effects
