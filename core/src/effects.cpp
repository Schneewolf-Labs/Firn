#include "firn/effects.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "firn/raster.h"

namespace firn::effects {

namespace {

uint8_t clamp8(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f); }

// Generic RGB neighbourhood pass with edge clamping. `fn(x, y, out)` gets
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

}  // namespace

void convolve3(Image& img, const float kernel[9], float divisor, float bias) {
    if (divisor == 0.0f) divisor = 1.0f;
    rgb_pass(img, [&](int x, int y, auto at, float* out) {
        for (int c = 0; c < 3; ++c) {
            float acc = 0.0f;
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i) acc += at(x + i, y + j)[c] * kernel[(j + 1) * 3 + (i + 1)];
            out[c] = acc / divisor + bias;
        }
    });
}

void sharpen(Image& img) {
    static const float k[9] = {0, -1, 0, -1, 5, -1, 0, -1, 0};
    convolve3(img, k);
}

void sharpen_more(Image& img) {
    static const float k[9] = {-1, -1, -1, -1, 9, -1, -1, -1, -1};
    convolve3(img, k);
}

void blur_more(Image& img) {
    static const float k[9] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
    convolve3(img, k, 9.0f);
}

void soften(Image& img) {
    static const float k[9] = {1, 1, 1, 1, 4, 1, 1, 1, 1};
    convolve3(img, k, 12.0f);
}

void soften_more(Image& img) {
    static const float k[9] = {1, 2, 1, 2, 4, 2, 1, 2, 1};
    convolve3(img, k, 16.0f);
}

void unsharp_mask(Image& img, float radius, int strength, int clipping) {
    Image blurred = img;
    raster::gaussian_blur(blurred, radius);
    const float amount = strength / 100.0f;
    uint8_t* p = img.data();
    const uint8_t* b = blurred.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4)
        for (int c = 0; c < 3; ++c) {
            const int diff = p[i + c] - b[i + c];
            if (std::abs(diff) < clipping) continue;
            p[i + c] = clamp8(p[i + c] + diff * amount);
        }
}

void median(Image& img, int radius) {
    radius = std::max(1, radius);
    std::vector<uint8_t> win;
    rgb_pass(img, [&](int x, int y, auto at, float* out) {
        for (int c = 0; c < 3; ++c) {
            win.clear();
            for (int j = -radius; j <= radius; ++j)
                for (int i = -radius; i <= radius; ++i) win.push_back(at(x + i, y + j)[c]);
            std::nth_element(win.begin(), win.begin() + win.size() / 2, win.end());
            out[c] = win[win.size() / 2];
        }
    });
}

void motion_blur(Image& img, float angle_degrees, int strength) {
    const int n = std::max(1, strength);
    const float rad = angle_degrees * 3.14159265f / 180.0f;
    const float dx = std::cos(rad), dy = -std::sin(rad);  // screen space: angle 0 = right, 90 = up
    // Premultiplied so transparent pixels don't darken the streak.
    const Image src = img;
    const int w = img.width(), h = img.height();
    uint8_t* d = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < n; ++k) {
                const int sx = std::clamp(static_cast<int>(std::lround(x + dx * k)), 0, w - 1);
                const int sy = std::clamp(static_cast<int>(std::lround(y + dy * k)), 0, h - 1);
                const uint8_t* s = src.data() + (static_cast<size_t>(sy) * w + sx) * 4;
                const float a = s[3] / 255.0f;
                acc[0] += s[0] * a; acc[1] += s[1] * a; acc[2] += s[2] * a; acc[3] += s[3];
            }
            uint8_t* p = d + (static_cast<size_t>(y) * w + x) * 4;
            const float a = acc[3] / n;
            for (int c = 0; c < 3; ++c) p[c] = a > 0 ? clamp8(acc[c] / n / (a / 255.0f)) : 0;
            p[3] = clamp8(a);
        }
}

void mosaic(Image& img, int bw, int bh) {
    bw = std::max(1, bw); bh = std::max(1, bh);
    const int w = img.width(), h = img.height();
    uint8_t* d = img.data();
    for (int by = 0; by < h; by += bh)
        for (int bx = 0; bx < w; bx += bw) {
            const int x1 = std::min(w, bx + bw), y1 = std::min(h, by + bh);
            long sum[4] = {0, 0, 0, 0};
            int n = 0;
            for (int y = by; y < y1; ++y)
                for (int x = bx; x < x1; ++x, ++n)
                    for (int c = 0; c < 4; ++c) sum[c] += d[(static_cast<size_t>(y) * w + x) * 4 + c];
            for (int y = by; y < y1; ++y)
                for (int x = bx; x < x1; ++x)
                    for (int c = 0; c < 4; ++c) d[(static_cast<size_t>(y) * w + x) * 4 + c] = static_cast<uint8_t>((sum[c] + n / 2) / n);
        }
}

void add_noise(Image& img, int percent, bool gaussian, bool monochrome, uint32_t seed) {
    uint32_t state = seed ? seed : 1;
    auto rnd = [&]() {  // xorshift32 -> [-1, 1]
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return (state & 0xFFFFFF) / static_cast<float>(0x7FFFFF) - 1.0f;
    };
    auto sample = [&]() {
        if (!gaussian) return rnd();
        float s = 0;  // approximately normal, clipped to [-1, 1]
        for (int i = 0; i < 4; ++i) s += rnd();
        return std::clamp(s * 0.5f, -1.0f, 1.0f);
    };
    const float amp = std::clamp(percent, 0, 100) * 2.55f;
    uint8_t* p = img.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        if (monochrome) {
            const float n = sample() * amp;
            for (int c = 0; c < 3; ++c) p[i + c] = clamp8(p[i + c] + n);
        } else {
            for (int c = 0; c < 3; ++c) p[i + c] = clamp8(p[i + c] + sample() * amp);
        }
    }
}

void find_edges(Image& img) {
    rgb_pass(img, [&](int x, int y, auto at, float* out) {
        for (int c = 0; c < 3; ++c) {
            const float gx = -at(x - 1, y - 1)[c] - 2 * at(x - 1, y)[c] - at(x - 1, y + 1)[c] + at(x + 1, y - 1)[c] + 2 * at(x + 1, y)[c] + at(x + 1, y + 1)[c];
            const float gy = -at(x - 1, y - 1)[c] - 2 * at(x, y - 1)[c] - at(x + 1, y - 1)[c] + at(x - 1, y + 1)[c] + 2 * at(x, y + 1)[c] + at(x + 1, y + 1)[c];
            out[c] = std::sqrt(gx * gx + gy * gy) * 0.5f;
        }
    });
}

void enhance_edges(Image& img) {
    static const float k[9] = {0, -1, 0, -1, 6, -1, 0, -1, 0};
    convolve3(img, k, 2.0f);
}

void enhance_edges_more(Image& img) {
    static const float k[9] = {-1, -1, -1, -1, 10, -1, -1, -1, -1};
    convolve3(img, k, 2.0f);
}

void emboss(Image& img) {
    static const float k[9] = {-2, -1, 0, -1, 0, 1, 0, 1, 2};
    convolve3(img, k, 1.0f, 128.0f);
    // The original's emboss is grey.
    uint8_t* p = img.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        const uint8_t g = static_cast<uint8_t>((p[i] * 299 + p[i + 1] * 587 + p[i + 2] * 114 + 500) / 1000);
        p[i] = p[i + 1] = p[i + 2] = g;
    }
}

void erode(Image& img) {
    rgb_pass(img, [&](int x, int y, auto at, float* out) {
        for (int c = 0; c < 3; ++c) {
            int m = 255;
            for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) m = std::min<int>(m, at(x + i, y + j)[c]);
            out[c] = static_cast<float>(m);
        }
    });
}

void dilate(Image& img) {
    rgb_pass(img, [&](int x, int y, auto at, float* out) {
        for (int c = 0; c < 3; ++c) {
            int m = 0;
            for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) m = std::max<int>(m, at(x + i, y + j)[c]);
            out[c] = static_cast<float>(m);
        }
    });
}

void drop_shadow(Image& img, int ox, int oy, float opacity, float blur, Color color) {
    const int w = img.width(), h = img.height();
    // Shadow image: the alpha mask, offset, coloured.
    Image shadow(w, h, {color.r, color.g, color.b, 0});
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int sx = x - ox, sy = y - oy;
            if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
            const uint8_t a = img.data()[(static_cast<size_t>(sy) * w + sx) * 4 + 3];
            shadow.data()[(static_cast<size_t>(y) * w + x) * 4 + 3] = static_cast<uint8_t>(a * std::clamp(opacity, 0.0f, 1.0f) + 0.5f);
        }
    if (blur > 0.0f) raster::gaussian_blur(shadow, blur);
    // Original pixels over the shadow.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t* s = img.data() + (static_cast<size_t>(y) * w + x) * 4;
            raster::blend_over(shadow, x, y, {s[0], s[1], s[2], s[3]}, 1.0f);
        }
    img = std::move(shadow);
}

}  // namespace firn::effects
