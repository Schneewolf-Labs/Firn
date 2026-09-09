#pragma once
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "firn/image.h"

// Colour adjustments, named after the original's commands. All operate in
// place on straight-alpha RGBA8 and leave alpha alone.
namespace firn::adjust {

struct HSL { float h, s, l; };  // h in [0,360), s and l in [0,1]
HSL rgb_to_hsl(uint8_t r, uint8_t g, uint8_t b);
void hsl_to_rgb(HSL c, uint8_t* r, uint8_t* g, uint8_t* b);

using Lut = std::array<uint8_t, 256>;
void apply_lut(Image& img, const Lut& lut);                                  // same LUT on R, G, B
void apply_luts(Image& img, const Lut& r, const Lut& g, const Lut& b);

// Colorize: every pixel takes the given hue and saturation, keeping lightness.
void colorize(Image& img, int hue /*0..359*/, int saturation /*0..255*/);

// Hue/Saturation/Lightness: hue shift in degrees, saturation and lightness
// as percentages in -100..100 (positive moves towards full / white).
void hsl_adjust(Image& img, int hue, int saturation, int lightness);

// Levels per the classic dialog: input black/gamma/white, output black/white.
Lut levels_lut(int in_low, float gamma, int in_high, int out_low, int out_high);
Lut gamma_lut(float gamma);
Lut threshold_lut(int value);           // < value -> 0, else 255
void greyscale_then_threshold(Image& img, int value);  // the Threshold command: on luma
Lut posterize_lut(int levels);          // 2..255
Lut solarize_lut(int threshold);        // invert above threshold
Lut brightness_contrast_lut(int brightness, int contrast);

// Monotone cubic curve through sorted control points (x, y in 0..255).
Lut curve_lut(const std::vector<std::pair<float, float>>& points);

// Channel mixer: out[c] = sum(mix[c][i] * in[i]) + constant[c], in percent.
// Monochrome uses row 0 for all three outputs.
struct ChannelMix {
    float mix[3][3] = {{100, 0, 0}, {0, 100, 0}, {0, 0, 100}};
    float constant[3] = {0, 0, 0};
    bool monochrome = false;
};
void channel_mixer(Image& img, const ChannelMix& m);

// Color Balance: per tonal range, cyan-red / magenta-green / yellow-blue in
// -100..100. Weights follow the classic shadows/midtones/highlights curves.
struct ColorBalance {
    int shadows[3] = {0, 0, 0}, midtones[3] = {0, 0, 0}, highlights[3] = {0, 0, 0};
    bool preserve_luminosity = true;
};
void color_balance(Image& img, const ColorBalance& cb);

// Sepia Toning: `amount` 0..100 blends from the original towards a sepia tint.
void sepia(Image& img, int amount);

// Hue Map: ten 36-degree bands starting at red; each band's hue is shifted
// by `shift[band]` degrees (-180..180), interpolated between band centres.
// Saturation and lightness shifts apply to every pixel (-100..100).
struct HueMap {
    int shift[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int saturation = 0, lightness = 0;
};
void hue_map(Image& img, const HueMap& m);

// Histogram helpers and auto adjustments.
std::array<int, 256> histogram_luma(const Image& img);
void histogram_stretch(Image& img);                       // per-channel min/max to full range
void histogram_equalize(Image& img);                      // luma equalisation
void auto_contrast(Image& img, float clip_percent = 0.5f);  // stretch luma ignoring outliers

}  // namespace firn::adjust
