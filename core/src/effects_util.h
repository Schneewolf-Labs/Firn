#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "firn/image.h"

// Helpers shared by the effects sources (internal to core).
namespace firn::effects::detail {

inline uint8_t clamp8(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f); }

// Generic RGB neighborhood pass with edge clamping. `fn(x, y, out)` gets
// a sampler over the *source* copy and writes three channel values.
template <class Fn>
void rgb_pass(Image& img, Fn fn) {
    const Image src = img;
    const int w = img.width(), h = img.height();
    auto at = [&](int x, int y) { return src.data() + (static_cast<size_t>(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)) * 4; };
    uint8_t* d = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float out[3];
            fn(x, y, at, out);
            uint8_t* p = d + (static_cast<size_t>(y) * w + x) * 4;
            for (int c = 0; c < 3; ++c) p[c] = clamp8(out[c]);
        }
}


// Rebuilds img by sampling src at map(x, y) -> (sx, sy) with premultiplied
// bilinear filtering; samples outside the image are transparent.
template <class Map>
void remap(Image& img, Map map) {
    const Image src = img;
    const int w = img.width(), h = img.height();
    uint8_t* d = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float sx, sy;
            map(x + 0.5f, y + 0.5f, sx, sy);
            sx -= 0.5f; sy -= 0.5f;
            const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
            const float fx = sx - x0, fy = sy - y0;
            float acc[4] = {0, 0, 0, 0};
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i) {
                    const int px = std::clamp(x0 + i, 0, w - 1), py = std::clamp(y0 + j, 0, h - 1);
                    const float wt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                    const uint8_t* s = src.data() + (static_cast<size_t>(py) * w + px) * 4;
                    const float a = s[3] / 255.0f;
                    acc[0] += s[0] * a * wt; acc[1] += s[1] * a * wt; acc[2] += s[2] * a * wt; acc[3] += s[3] * wt;
                }
            uint8_t* p = d + (static_cast<size_t>(y) * w + x) * 4;
            const float a = std::clamp(acc[3], 0.0f, 255.0f);
            for (int c = 0; c < 3; ++c) p[c] = a > 0 ? clamp8(acc[c] / (a / 255.0f)) : 0;
            p[3] = clamp8(a);
        }
}


// Edge handling for geometric effects: 0 wrap, 1 repeat (clamp), 2 fill color, 3 transparent.
struct EdgeMode { int mode = 1; Color fill{0, 0, 0, 255}; };

// remap() with edge handling instead of clamping.
template <class Map>
void remap_edges(Image& img, const EdgeMode& edge, Map map) {
    const Image src = img;
    const int w = img.width(), h = img.height();
    uint8_t* d = img.data();
    auto fetch = [&](int px, int py, const uint8_t*& s, bool& valid) {
        valid = true;
        if (edge.mode == 0) { px = ((px % w) + w) % w; py = ((py % h) + h) % h; }
        else if (edge.mode == 1) { px = std::clamp(px, 0, w - 1); py = std::clamp(py, 0, h - 1); }
        else if (px < 0 || py < 0 || px >= w || py >= h) { valid = false; return; }
        s = src.data() + (static_cast<size_t>(py) * w + px) * 4;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float sx, sy;
            map(x + 0.5f, y + 0.5f, sx, sy);
            sx -= 0.5f; sy -= 0.5f;
            const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
            const float fx = sx - x0, fy = sy - y0;
            float acc[4] = {0, 0, 0, 0};
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i) {
                    const float wt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                    const uint8_t* s = nullptr; bool valid;
                    fetch(x0 + i, y0 + j, s, valid);
                    if (!valid) {
                        if (edge.mode == 2) { const float a = edge.fill.a / 255.0f; acc[0] += edge.fill.r * a * wt; acc[1] += edge.fill.g * a * wt; acc[2] += edge.fill.b * a * wt; acc[3] += edge.fill.a * wt; }
                        continue;
                    }
                    const float a = s[3] / 255.0f;
                    acc[0] += s[0] * a * wt; acc[1] += s[1] * a * wt; acc[2] += s[2] * a * wt; acc[3] += s[3] * wt;
                }
            uint8_t* p = d + (static_cast<size_t>(y) * w + x) * 4;
            const float a = std::clamp(acc[3], 0.0f, 255.0f);
            for (int c = 0; c < 3; ++c) p[c] = a > 0 ? clamp8(acc[c] / (a / 255.0f)) : 0;
            p[3] = clamp8(a);
        }
}

inline float luma_of(const uint8_t* p) { return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]; }

}  // namespace firn::effects::detail
