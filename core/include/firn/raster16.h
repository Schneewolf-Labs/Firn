#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "firn/adjust.h"
#include "firn/image.h"
#include "firn/mask.h"
#include "firn/raster.h"

// The subset of raster operations that run at 16 bits per channel. Values
// in callbacks are 0..1 floats; alpha is left alone unless stated.
namespace firn::raster16 {

using Lut16 = std::vector<uint16_t>;                     // 65536 entries
Lut16 lut_from(const std::function<float(float)>& fn);   // fn on 0..1
Lut16 lut_from8(const adjust::Lut& lut8);                // interpolates an 8-bit LUT
void apply_lut(Image16& img, const Lut16& lut);
void apply_luts(Image16& img, const Lut16& r, const Lut16& g, const Lut16& b);
// Per-pixel RGB mapping in floats.
void map_rgb(Image16& img, const std::function<void(float&, float&, float&)>& fn);

// Adjustments.
void brightness_contrast(Image16& img, int brightness, int contrast);
void levels(Image16& img, int in_low, float gamma, int in_high, int out_low, int out_high);
void gamma(Image16& img, float r, float g, float b);
void curves(Image16& img, const std::vector<std::pair<float, float>>& points);
void invert(Image16& img);
void threshold(Image16& img, int value);
void posterize(Image16& img, int levels);
void grayscale(Image16& img);
void hsl_adjust(Image16& img, int hue, int saturation, int lightness);
void colorize(Image16& img, int hue, int saturation /*0..255*/);
void color_balance(Image16& img, const adjust::ColorBalance& cb);
void channel_mixer(Image16& img, const adjust::ChannelMix& m);

// Geometry and painting.
void fill(Image16& img, Color c);
void gaussian_blur(Image16& img, float radius);
Image16 crop(const Image16& src, raster::Rect r);          // outside = transparent
Image16 resample(const Image16& src, int w, int h, raster::Filter filter);
Image16 rotate_quarter(const Image16& src, int quarter_turns);
Image16 rotate(const Image16& src, float degrees);          // same canvas growth as raster::rotate
void flip_vertical(Image16& img);
void mirror_horizontal(Image16& img);
Image16 shifted(const Image16& src, int dx, int dy);
// Keeps `before` where the mask is clear (selection clipping).
void apply_through_mask(Image16& dst, const Image16& before, const Mask& mask);
void restore_clear_pixels(Image16& dst, const Image16& before);
// Composite of 16-bit layers with Normal blending (export); other blend
// modes and non-raster layers fall back to their 8-bit pixels.
}  // namespace firn::raster16
