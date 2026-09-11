#include "firn/photo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "firn/adjust.h"
#include "firn/effects.h"
#include "firn/parallel.h"
#include "firn/raster.h"

namespace firn::photo {

namespace {

inline uint8_t clamp8(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f); }
inline float luma(const uint8_t* p) { return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]; }

// Blends `img` towards `other` by t (0..1), keeping alpha.
void blend_towards(Image& img, const Image& other, float t) {
    uint8_t* d = img.data();
    const uint8_t* s = other.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        if (!d[i + 3]) continue;
        for (int c = 0; c < 3; ++c) d[i + c] = clamp8(d[i + c] + (s[i + c] - d[i + c]) * t);
    }
}

std::array<int, 256> histogram_channel(const Image& img, int channel) {
    std::array<int, 256> h{};
    const uint8_t* p = img.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        if (!p[i + 3]) continue;
        const int v = channel == 0 ? static_cast<int>(luma(p + i) + 0.5f) : p[i + channel - 1];
        ++h[static_cast<size_t>(std::clamp(v, 0, 255))];
    }
    return h;
}

// Levels-style LUT from clip percentages and gamma over a histogram.
adjust::Lut clip_lut(const std::array<int, 256>& h, float low_percent, float high_percent, float gamma) {
    long total = 0;
    for (int v : h) total += v;
    int lo = 0, hi = 255;
    long acc = 0;
    for (int i = 0; i < 256; ++i) { acc += h[static_cast<size_t>(i)]; if (acc > total * low_percent / 100.0f) { lo = i; break; } }
    acc = 0;
    for (int i = 255; i >= 0; --i) { acc += h[static_cast<size_t>(i)]; if (acc > total * high_percent / 100.0f) { hi = i; break; } }
    if (hi <= lo) { lo = std::max(0, lo - 1); hi = std::min(255, lo + 1); }
    // The stretched range stops a few levels short of each end, and what
    // falls outside the clip points is compressed into those few levels
    // rather than collapsed onto 0 or 255. Without this toe and shoulder an
    // auto contrast turns every shadow into the same flat black, which also
    // costs the pixel its color once the luma LUT scales the channels.
    const int toe = 6;
    adjust::Lut lut = adjust::levels_lut(lo, gamma, hi, toe, 255 - toe);
    if (lo > 0)
        for (int i = 0; i <= lo; ++i) lut[static_cast<size_t>(i)] = clamp8(static_cast<float>(i) * toe / lo);
    if (hi < 255)
        for (int i = hi; i < 256; ++i) lut[static_cast<size_t>(i)] = clamp8(255 - toe + static_cast<float>(i - hi) * toe / (255 - hi));
    return lut;
}

// Applies a luma LUT by scaling each pixel's channels by new/old luma.
void apply_luma_lut(Image& img, const adjust::Lut& lut) {
    uint8_t* p = img.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        if (!p[i + 3]) continue;
        const float y = luma(p + i);
        if (y <= 0.0f) continue;
        const float ny = lut[static_cast<size_t>(std::clamp(static_cast<int>(y + 0.5f), 0, 255))];
        const float k = ny / y;
        for (int c = 0; c < 3; ++c) p[i + c] = clamp8(p[i + c] * k);
    }
}

}  // namespace

void auto_color_balance(Image& img, int strength, int temperature, bool remove_cast) {
    const uint8_t* p = img.data();
    double sum[3] = {0, 0, 0};
    long n = 0;
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        if (!p[i + 3]) continue;
        for (int c = 0; c < 3; ++c) sum[c] += p[i + c];
        ++n;
    }
    if (n == 0) return;
    const double mean[3] = {sum[0] / n, sum[1] / n, sum[2] / n};
    const double gray = (mean[0] + mean[1] + mean[2]) / 3.0;
    float gain[3] = {1.0f, 1.0f, 1.0f};
    // Gray-world cast removal, only when asked for: it pulls the channel
    // means together, which also drains real color from a picture that has
    // no cast to remove.
    if (remove_cast)
        for (int c = 0; c < 3; ++c) gain[c] = static_cast<float>(mean[c] > 1 ? gray / mean[c] : 1.0);
    // Temperature: a mild red/blue tilt around 6500 K.
    const float t = std::clamp((temperature - 6500) / 6500.0f, -1.0f, 1.0f);
    gain[0] *= 1.0f - 0.25f * t;
    gain[2] *= 1.0f + 0.25f * t;
    const float s = std::clamp(strength, 0, 100) / 100.0f;
    uint8_t* d = img.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        if (!d[i + 3]) continue;
        for (int c = 0; c < 3; ++c) d[i + c] = clamp8(d[i + c] * (1.0f + (gain[c] - 1.0f) * s));
    }
}

void auto_contrast_enhance(Image& img, int bias, int strength, int appearance) {
    const auto h = histogram_channel(img, 0);
    const float clip = appearance == 0 ? 0.1f : appearance == 1 ? 0.5f : 1.5f;
    const float gamma = bias == 0 ? 0.85f : bias == 2 ? 1.15f : 1.0f;
    adjust::Lut lut = clip_lut(h, clip, clip, gamma);
    if (strength == 1) for (int i = 0; i < 256; ++i) lut[static_cast<size_t>(i)] = clamp8(i + (lut[static_cast<size_t>(i)] - i) * 0.5f);
    apply_luma_lut(img, lut);
}

void auto_saturation(Image& img, int bias, int strength, bool skin_tones) {
    const float base = bias == 0 ? 0.9f : bias == 2 ? 1.3f : 1.1f;
    const float amount = strength == 0 ? 0.5f : strength == 2 ? 1.5f : 1.0f;
    const float factor = 1.0f + (base - 1.0f) * amount;
    uint8_t* p = img.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        if (!p[i + 3]) continue;
        const float y = luma(p + i);
        float f = factor;
        if (skin_tones) {
            const adjust::HSL hsl = adjust::rgb_to_hsl(p[i], p[i + 1], p[i + 2]);
            if (hsl.h >= 5 && hsl.h <= 45 && hsl.s > 0.15f && hsl.s < 0.75f) f = 1.0f + (factor - 1.0f) * 0.3f;
        }
        for (int c = 0; c < 3; ++c) p[i + c] = clamp8(y + (p[i + c] - y) * f);
    }
}

// The original's own factory presets give the settings each step runs with:
// AutoColorBalance strength 30 / 6500 K / RemoveColorCast 0,
// AutoContrastEnhancement bias 1 strength 0 appearance 1, Clarify strength 2,
// AutoSaturationEnhancement bias 1 strength 1 Skintones 0.
void one_step_photo_fix(Image& img) {
    auto_color_balance(img, 30, 6500, false);
    auto_contrast_enhance(img, 1, 0, 1);
    clarify(img, 2);
    auto_saturation(img, 1, 1, false);
}

void clarify(Image& img, int strength) {
    strength = std::clamp(strength, 1, 5);
    Image blurred = img;
    raster::gaussian_blur(blurred, 8.0f + strength * 4.0f);
    uint8_t* d = img.data();
    const uint8_t* b = blurred.data();
    const float k = 0.15f * strength;
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        if (!d[i + 3]) continue;
        const float y = luma(d + i), yb = luma(b + i);
        float delta = (y - yb) * k;
        // The local contrast is added to every channel, not multiplied in:
        // scaling by ny/y sent a dark pixel in a bright neighborhood to pure
        // black, which blocked up the shadows of any contrasty photo. The
        // room left at each end also limits the push, so nothing is driven
        // into 0 or 255 just because its surroundings are far away.
        const float room = delta > 0.0f ? 255.0f - std::max({d[i], d[i + 1], d[i + 2]})
                                        : static_cast<float>(std::min({d[i], d[i + 1], d[i + 2]}));
        // Approach the room left at that end instead of running past it, so
        // a small push is untouched and a large one lands just short of the
        // limit. Local contrast then never invents a pure black or white.
        if (room > 0.5f) delta = std::copysign(room * (1.0f - std::exp(-std::abs(delta) / room)), delta);
        else delta = 0.0f;
        for (int c = 0; c < 3; ++c) d[i + c] = clamp8(d[i + c] + delta);
    }
}

void black_white_points(Image& img, Color src_black, Color src_white, Color dst_black, Color dst_white) {
    adjust::Lut lut[3];
    const int sb[3] = {src_black.r, src_black.g, src_black.b}, sw[3] = {src_white.r, src_white.g, src_white.b};
    const int db[3] = {dst_black.r, dst_black.g, dst_black.b}, dw[3] = {dst_white.r, dst_white.g, dst_white.b};
    for (int c = 0; c < 3; ++c) {
        const int span = std::max(1, sw[c] - sb[c]);
        for (int i = 0; i < 256; ++i) lut[c][static_cast<size_t>(i)] = clamp8(db[c] + (i - sb[c]) * static_cast<float>(dw[c] - db[c]) / span);
    }
    adjust::apply_luts(img, lut[0], lut[1], lut[2]);
}

void histogram_adjust(Image& img, float low_percent, float high_percent, float gamma, int midtones, int channel) {
    const auto h = histogram_channel(img, std::clamp(channel, 0, 3));
    adjust::Lut lut = clip_lut(h, std::clamp(low_percent, 0.0f, 50.0f), std::clamp(high_percent, 0.0f, 50.0f), std::clamp(gamma, 0.1f, 7.0f));
    if (midtones != 0) {
        // Expand (positive) pushes values away from mid-gray, compress pulls them in.
        const float m = std::clamp(midtones, -100, 100) / 100.0f;
        for (int i = 0; i < 256; ++i) {
            const float v = lut[static_cast<size_t>(i)] / 255.0f - 0.5f;
            const float e = m >= 0 ? std::copysign(std::pow(std::abs(v) * 2.0f, 1.0f - 0.6f * m) * 0.5f, v) : std::copysign(std::pow(std::abs(v) * 2.0f, 1.0f - 0.6f * m) * 0.5f, v);
            lut[static_cast<size_t>(i)] = clamp8((e + 0.5f) * 255.0f);
        }
    }
    if (channel == 0) apply_luma_lut(img, lut);
    else {
        adjust::Lut id;
        for (int i = 0; i < 256; ++i) id[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
        adjust::apply_luts(img, channel == 1 ? lut : id, channel == 2 ? lut : id, channel == 3 ? lut : id);
    }
}

void salt_and_pepper(Image& img, int speck_size, int sensitivity, bool include_smaller, bool aggressive) {
    speck_size = std::clamp(speck_size | 1, 3, 9);
    const int w = img.width(), h = img.height();
    const int start = include_smaller ? 3 : speck_size;
    for (int size = start; size <= speck_size; size += 2) {
        const int r = size / 2;
        Image ref = img;
        effects::median(ref, r);
        const float thr = (31 - std::clamp(sensitivity, 1, 30)) * (aggressive ? 4.0f : 8.0f);
        uint8_t* d = img.data();
        const uint8_t* m = ref.data();
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = (static_cast<size_t>(y) * w + x) * 4;
                if (!d[i + 3]) continue;
                const float diff = std::abs(luma(d + i) - luma(m + i));
                if (diff > thr) { d[i] = m[i]; d[i + 1] = m[i + 1]; d[i + 2] = m[i + 2]; }
            }
    }
}

// Bilateral smoothing: each pixel averages neighbors within `radius` whose
// luma is close to its own (Gaussian in color with `color_sigma`), so flat
// areas smooth and edges stay. `amount` blends the result in.
static void smooth_flat(Image& img, float radius, float color_sigma, float amount) {
    const int r = std::max(1, static_cast<int>(std::ceil(radius)));
    const int w = img.width(), h = img.height();
    const Image src = img;
    const uint8_t* s = src.data();
    uint8_t* d = img.data();
    std::vector<float> spatial(static_cast<size_t>(2 * r + 1) * (2 * r + 1));
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) spatial[static_cast<size_t>((dy + r) * (2 * r + 1) + dx + r)] = std::exp(-(dx * dx + dy * dy) / (2.0f * radius * radius));
    const float inv2s = 1.0f / (2.0f * color_sigma * color_sigma);
    parallel::rows(h, static_cast<size_t>(w) * (2 * r + 1) * (2 * r + 1), [&](int ry0, int ry1) {
    for (int y = ry0; y < ry1; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            if (!s[i + 3]) continue;
            const float l0 = luma(s + i);
            float acc[3] = {0, 0, 0}, wsum = 0;
            for (int dy = -r; dy <= r; ++dy) {
                const int py = y + dy;
                if (py < 0 || py >= h) continue;
                for (int dx = -r; dx <= r; ++dx) {
                    const int px = x + dx;
                    if (px < 0 || px >= w) continue;
                    const uint8_t* q = s + (static_cast<size_t>(py) * w + px) * 4;
                    if (!q[3]) continue;
                    const float dl = luma(q) - l0;
                    const float wt = spatial[static_cast<size_t>((dy + r) * (2 * r + 1) + dx + r)] * std::exp(-dl * dl * inv2s);
                    for (int c = 0; c < 3; ++c) acc[c] += q[c] * wt;
                    wsum += wt;
                }
            }
            if (wsum <= 0.0f) continue;
            for (int c = 0; c < 3; ++c) d[i + c] = clamp8(d[i + c] + (acc[c] / wsum - d[i + c]) * amount);
        }
    });
}

void edge_preserving_smooth(Image& img, int smoothing) {
    const int s = std::clamp(smoothing, 1, 100);
    // A wider reach and a looser luma tolerance as the setting rises; the
    // tolerance is what keeps edges, so it grows more slowly than the radius.
    smooth_flat(img, 1.0f + s / 25.0f, 2.0f + s * 0.30f, 1.0f);
}

void jpeg_artifact_removal(Image& img, int strength, int crispness) {
    strength = std::clamp(strength, 0, 3);
    const float radius = 1.0f + strength * 0.75f;
    const float sigma = 8.0f + strength * 6.0f;
    smooth_flat(img, radius, sigma, 1.0f);
    if (crispness > 0) effects::unsharp_mask(img, 1.0f, std::clamp(crispness, 0, 100), 0);
}

void fill_flash(Image& img, int strength) {
    const float s = std::clamp(strength, 0, 100) / 100.0f;
    adjust::Lut lut;
    for (int i = 0; i < 256; ++i) {
        const float v = i / 255.0f;
        const float lifted = std::pow(v, 1.0f / (1.0f + 1.5f * s));
        lut[static_cast<size_t>(i)] = clamp8((v + (lifted - v) * (1.0f - v)) * 255.0f);
    }
    apply_luma_lut(img, lut);
}

void backlighting(Image& img, int strength) {
    const float s = std::clamp(strength, 0, 100) / 100.0f;
    adjust::Lut lut;
    for (int i = 0; i < 256; ++i) {
        const float v = i / 255.0f;
        const float lowered = std::pow(v, 1.0f + 1.5f * s);
        lut[static_cast<size_t>(i)] = clamp8((v + (lowered - v) * v) * 255.0f);
    }
    apply_luma_lut(img, lut);
}

void chromatic_aberration(Image& img, float red_shift, float blue_shift) {
    const int w = img.width(), h = img.height();
    if (w < 2 || h < 2) return;
    const Image src = img;
    const float cx = w * 0.5f, cy = h * 0.5f;
    const float corner = std::hypot(cx, cy);
    auto sample = [&](int c, float x, float y) {
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const float fx = x - x0, fy = y - y0;
        float v = 0;
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const int px = std::clamp(x0 + i, 0, w - 1), py = std::clamp(y0 + j, 0, h - 1);
                v += src.data()[(static_cast<size_t>(py) * w + px) * 4 + c] * (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            }
        return v;
    };
    uint8_t* d = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            if (!d[i + 3]) continue;
            const float dx = x - cx, dy = y - cy;
            const float r = std::hypot(dx, dy) / corner;   // 0 at center, 1 at corners
            const float kr = 1.0f - red_shift * r / corner, kb = 1.0f - blue_shift * r / corner;
            d[i] = clamp8(sample(0, cx + dx * kr, cy + dy * kr));
            d[i + 2] = clamp8(sample(2, cx + dx * kb, cy + dy * kb));
        }
}

void noise_removal(Image& img, int strength, int blend, int sharpening) {
    const float s = std::clamp(strength, 0, 100) / 100.0f;
    Image work = img;
    // Bilateral-style: smooth flat regions progressively harder.
    smooth_flat(work, 1.0f + 2.0f * s, 6.0f + 20.0f * s, 1.0f);
    if (s > 0.5f) smooth_flat(work, 2.0f + 2.0f * s, 6.0f + 20.0f * s, (s - 0.5f) * 2.0f);
    blend_towards(img, work, std::clamp(blend, 0, 100) / 100.0f);
    if (sharpening > 0) effects::unsharp_mask(img, 1.0f, std::clamp(sharpening, 0, 100), 0);
}

}  // namespace firn::photo
