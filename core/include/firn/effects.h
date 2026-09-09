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

}  // namespace firn::effects
