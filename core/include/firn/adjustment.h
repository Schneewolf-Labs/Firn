#pragma once
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "firn/adjust.h"
#include "firn/image.h"
#include "firn/json.h"
#include "firn/vector.h"

// Adjustment layers: a color operation applied to everything composited
// below the layer (within its group), through the layer's mask and opacity.
// The kinds and their parameters follow the original's adjustment layer
// definitions in its file format (docs/FORMAT.md).
namespace firn {

struct Adjustment {
    enum class Kind : uint16_t {
        None = 0, Levels = 1, Curves = 2, BrightnessContrast = 3, ColorBalance = 4,
        HSL = 5, ChannelMixer = 6, Invert = 7, Threshold = 8, Posterize = 9,
        // Colour adjustments the original does not have (50..99). Like the
        // filters below they are written to the native container as empty
        // placeholder layers plus the Firn stash.
        GradientMap = 50,
        // Filter layers (ours, not the original's): spatial effects applied
        // live to everything below. Saved in the native format as empty
        // placeholder layers plus a stash the original ignores.
        GaussianBlur = 100, Average = 101, UnsharpMask = 102
    };
    Kind kind = Kind::BrightnessContrast;

    // Brightness/Contrast (-255..255, -100..100)
    int brightness = 0, contrast = 0;
    // Levels: master, red, green, blue (only the master is applied when the
    // channels are at their defaults).
    struct Levels {
        float gamma = 1.0f;
        int in_low = 0, in_high = 255, out_low = 0, out_high = 255;
        bool operator==(const Levels&) const = default;
    };
    std::array<Levels, 4> levels{};
    // Curves: RGB, red, green, blue as control points (x, y in 0..255).
    std::array<std::vector<std::pair<float, float>>, 4> curves{{{{0.0f, 0.0f}, {255.0f, 255.0f}}, {{0.0f, 0.0f}, {255.0f, 255.0f}},
                                                              {{0.0f, 0.0f}, {255.0f, 255.0f}}, {{0.0f, 0.0f}, {255.0f, 255.0f}}}};
    // HSL: hue shift in degrees, saturation and lightness in -100..100.
    int hue = 0, saturation = 0, lightness = 0;
    bool colorize = false;
    int colorize_hue = 0, colorize_saturation = 100;
    // The original's per-color-range values (hue, saturation, lightness, four
    // range degrees) for red, yellow, green, cyan, blue, magenta; carried
    // through files, only the master values are applied.
    std::array<std::array<int32_t, 7>, 6> hsl_ranges{};
    adjust::ColorBalance color_balance;
    adjust::ChannelMix mixer;
    int threshold = 128;
    int posterize = 6;
    // Gradient Map: the pixel's lightness picks a colour along the gradient.
    vec::Gradient gradient;
    int gradient_index = -1;      // into the app's gradient library, -1 = its own
    // Filter layers.
    float blur_radius = 5.0f;
    int average_radius = 2;
    float unsharp_radius = 2.0f;
    int unsharp_strength = 100, unsharp_clipping = 0;

    bool is_filter() const { return static_cast<uint16_t>(kind) >= 100; }
    // Kinds the original has no equivalent for, which the native container
    // stores as a placeholder layer plus the stash rather than as one of
    // its own adjustment blocks.
    bool is_firn_only() const { return static_cast<uint16_t>(kind) >= 50; }
    // How far (pixels) a filter spreads what lies below; 0 for color kinds.
    int reach() const;

    void apply(Image& img) const;   // in place; color kinds leave alpha alone
    static const char* kind_name(Kind k);
    bool operator==(const Adjustment&) const;

    // Every field, for the project format. The native container stores only
    // the active kind's parameters, the way the original does; this keeps
    // the ones a person set and then switched away from.
    json::Value to_json() const;
    static Adjustment from_json(const json::Value& v);
};

}  // namespace firn
