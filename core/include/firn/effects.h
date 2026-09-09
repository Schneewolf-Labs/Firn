#pragma once
#include <cstdint>

#include "firn/image.h"

// Spatial effects, named after the original's Effects and Adjust menu
// entries. In place on straight-alpha RGBA8; colour work is done in
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
// Pinch (strength > 0) pulls towards the centre, Punch (< 0) pushes out. -100..100.
void pinch(Image& img, int strength);
// Twirl about the centre by `degrees` at the middle, falling off to the edge.
void twirl(Image& img, float degrees);

// Ripple: concentric waves from the centre. Spherize: bulge (positive) or
// dish (negative) inside the largest inscribed circle. Lens: barrel
// (positive) or pincushion (negative) distortion, -100..100.
void ripple(Image& img, float amplitude, float wavelength);
void spherize(Image& img, int strength);
void lens_distortion(Image& img, int strength);

// Halftone: dots on a grid of `cell` px sized by luminance, ink over paper.
void halftone(Image& img, int cell, float angle_degrees, Color ink, Color paper);
// Chrome: greyscale run through a repeated brightness ramp (`bands`).
void chrome(Image& img, int bands, float brightness);

// --- 3D ---------------------------------------------------------------
// Buttonize: bevelled border of `width` px in `color` at `opacity`;
// `transparent_edge` lightens/darkens the image instead of painting colour.
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

}  // namespace firn::effects
