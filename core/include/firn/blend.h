#pragma once
#include <cstdint>

namespace firn {

// Layer blend modes, in the original's menu order. Values are stable
// because they are stored in files.
enum class BlendMode : uint8_t {
    Normal = 0, Darken, Lighten, Hue, Saturation, Color, Luminance, Multiply, Screen,
    Dissolve, Overlay, HardLight, SoftLight, Difference, Dodge, Burn, Exclusion,
    Count
};

const char* blend_mode_name(BlendMode m);

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
