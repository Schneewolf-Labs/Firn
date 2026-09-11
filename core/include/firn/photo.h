#pragma once
#include <cstdint>

#include "firn/image.h"

// Photo fixes, named after the original's Adjust menu commands. All operate
// in place on straight-alpha RGBA8 and leave alpha alone.
namespace firn::photo {

// Automatic Color Balance: gray-world correction blended by `strength`
// (0..100), then a warm/cool shift towards `temperature` kelvin (6500 =
// none; lower is warmer).
// Automatic Color Balance. `temperature` in kelvin tilts red against blue
// (6500 is neutral). `remove_cast` additionally neutralizes an overall cast
// by the gray-world rule; the original leaves it off by default
// (RemoveColorCast 0 in its factory preset) because it flattens the colors
// of a picture that is not actually miscast.
void auto_color_balance(Image& img, int strength, int temperature = 6500, bool remove_cast = false);
// Automatic Contrast Enhancement: bias 0 lighter / 1 neutral / 2 darker,
// strength 0 normal / 1 mild, appearance 0 flat / 1 natural / 2 bold.
void auto_contrast_enhance(Image& img, int bias, int strength, int appearance);
// Automatic Saturation Enhancement: bias 0 less / 1 normal / 2 more
// colorful, strength 0 weak / 1 normal / 2 strong; skin tones are protected.
// Automatic Saturation Enhancement. `skin_tones` holds back on skin hues;
// the original's factory preset has it off (Skintones 0).
void auto_saturation(Image& img, int bias, int strength, bool skin_tones = false);
// One Step Photo Fix: the three automatic enhancements, clarify, then edge preservation.
void one_step_photo_fix(Image& img);
// Clarify: local contrast on luma, strength 1..5.
void clarify(Image& img, int strength);
// Black and White Points: maps source black/white colors to destination ones per channel.
void black_white_points(Image& img, Color src_black, Color src_white, Color dst_black, Color dst_white);
// Histogram Adjustment: low/high clip in percent (0..50), gamma 0.1..7,
// midtone expand (positive) or compress (negative) -100..100, on luminance
// (channel 0) or one of red/green/blue (1..3).
void histogram_adjust(Image& img, float low_percent, float high_percent, float gamma, int midtones, int channel);
// Salt and Pepper Filter: speck size 3..9 (odd), sensitivity 1..30; aggressive replaces harder.
void salt_and_pepper(Image& img, int speck_size, int sensitivity, bool include_smaller, bool aggressive);
// JPEG Artifact Removal: strength 0 low .. 3 maximum, crispness 0..100.
void jpeg_artifact_removal(Image& img, int strength, int crispness);
// Fill Flash lifts shadows; Backlighting darkens highlights. Strength 0..100.
void fill_flash(Image& img, int strength);
void backlighting(Image& img, int strength);
// Chromatic Aberration Removal: radial scale of the red and blue channels
// in pixels at the image corners (-20..20).
void chromatic_aberration(Image& img, float red_shift, float blue_shift);
// Edge Preserving Smooth: averages within flat areas while leaving edges
// alone. `smoothing` 1..100 widens both the reach and the tolerance.
void edge_preserving_smooth(Image& img, int smoothing);
// Digital Camera Noise Removal: edge-preserving smoothing. strength 0..100,
// blend (how much of the result to keep) 0..100, sharpening 0..100.
void noise_removal(Image& img, int strength, int blend, int sharpening);

}  // namespace firn::photo
