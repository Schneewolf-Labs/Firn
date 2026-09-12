#pragma once
#include <algorithm>
#include <cstdint>

#include "firn/json.h"

namespace firn {

// Layer blend modes, in the original's menu order. Values are stable
// because they are stored in files.
enum class BlendMode : uint8_t {
    Normal = 0, Darken, Lighten, Hue, Saturation, Color, Luminance, Multiply, Screen,
    Dissolve, Overlay, HardLight, SoftLight, Difference, Dodge, Burn, Exclusion,
    Count
};

const char* blend_mode_name(BlendMode m);

// Blend ranges: a layer can be limited to the tones it covers, or to the
// tones underneath it, without painting a mask. Each range has four stops
// in 0..255. A value at or below `low0` is hidden, ramps in to fully shown
// by `low1`, stays shown to `high1`, and ramps back out to hidden at
// `high0`. Leaving the two stops of an end together gives a hard edge;
// moving them apart gives a soft one.
struct BlendRange {
    uint8_t low0 = 0, low1 = 0, high1 = 255, high0 = 255;
    bool identity() const { return low0 == 0 && low1 == 0 && high1 == 255 && high0 == 255; }
    bool operator==(const BlendRange&) const = default;
    // How much of a pixel shows at value `v`, 0 to 1.
    float factor(int v) const {
        if (v < low0 || v > high0) return 0.0f;
        float f = 1.0f;
        if (v < low1 && low1 > low0) f = static_cast<float>(v - low0) / static_cast<float>(low1 - low0);
        if (v > high1 && high0 > high1) f = std::min(f, static_cast<float>(high0 - v) / static_cast<float>(high0 - high1));
        return f;
    }
};

struct BlendRanges {
    // Which value the stops are read from: the pixel's lightness, or one
    // channel of it.
    enum class Channel : uint8_t { Gray = 0, Red, Green, Blue };
    Channel channel = Channel::Gray;
    BlendRange source;   // the layer's own pixels
    BlendRange under;    // what is composited below it
    bool identity() const { return source.identity() && under.identity(); }
    bool operator==(const BlendRanges&) const = default;
    // The channel value of a straight-alpha RGBA8 pixel.
    int value_of(const uint8_t* px) const {
        switch (channel) {
            case Channel::Red: return px[0];
            case Channel::Green: return px[1];
            case Channel::Blue: return px[2];
            default: break;
        }
        return (px[0] * 77 + px[1] * 151 + px[2] * 28) >> 8;
    }
    // How much of the layer's pixel shows, given it and what is under it.
    float factor(const uint8_t* src, const uint8_t* dst) const {
        float f = 1.0f;
        if (!source.identity()) f *= source.factor(value_of(src));
        if (f > 0.0f && !under.identity()) f *= under.factor(value_of(dst));
        return f;
    }
};

const char* blend_channel_name(BlendRanges::Channel c);
json::Value blend_ranges_json(const BlendRanges& b);
BlendRanges blend_ranges_from_json(const json::Value& v);

namespace blend {

// Composite one straight-alpha RGBA8 source pixel over a destination pixel
// in place. `opacity` is the layer opacity in 0..1; `hash` is a per-pixel
// pseudo-random value used by Dissolve.
void pixel(uint8_t* dst, const uint8_t* src, float opacity, BlendMode mode, uint32_t hash);

// Small, fast, decorrelated hash of a pixel position for Dissolve.
inline uint32_t position_hash(int x, int y) {
    uint32_t h = static_cast<uint32_t>(x) * 0x9E3779B1u ^ static_cast<uint32_t>(y) * 0x85EBCA77u;
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return h;
}

}  // namespace blend
}  // namespace firn
