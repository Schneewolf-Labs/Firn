#include "firn/adjust.h"

#include <algorithm>
#include <cmath>

namespace firn::adjust {

namespace {
uint8_t clamp8(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f); }
int luma(const uint8_t* p) { return (p[0] * 299 + p[1] * 587 + p[2] * 114 + 500) / 1000; }
}  // namespace

HSL rgb_to_hsl(uint8_t r8, uint8_t g8, uint8_t b8) {
    const float r = r8 / 255.0f, g = g8 / 255.0f, b = b8 / 255.0f;
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    const float l = (mx + mn) * 0.5f;
    if (mx == mn) return {0.0f, 0.0f, l};
    const float d = mx - mn;
    const float s = l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    float h;
    if (mx == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g) h = (b - r) / d + 2.0f;
    else h = (r - g) / d + 4.0f;
    return {h * 60.0f, s, l};
}

void hsl_to_rgb(HSL c, uint8_t* r, uint8_t* g, uint8_t* b) {
    const float s = std::clamp(c.s, 0.0f, 1.0f), l = std::clamp(c.l, 0.0f, 1.0f);
    if (s <= 0.0f) { *r = *g = *b = clamp8(l * 255.0f); return; }
    float h = std::fmod(c.h, 360.0f);
    if (h < 0) h += 360.0f;
    h /= 360.0f;
    const float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
    const float p = 2 * l - q;
    auto hue = [&](float t) {
        if (t < 0) t += 1;
        if (t > 1) t -= 1;
        if (t < 1.0f / 6) return p + (q - p) * 6 * t;
        if (t < 0.5f) return q;
        if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
        return p;
    };
    *r = clamp8(hue(h + 1.0f / 3) * 255.0f);
    *g = clamp8(hue(h) * 255.0f);
    *b = clamp8(hue(h - 1.0f / 3) * 255.0f);
}

void apply_lut(Image& img, const Lut& lut) { apply_luts(img, lut, lut, lut); }

void apply_luts(Image& img, const Lut& r, const Lut& g, const Lut& b) {
    uint8_t* p = img.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        p[i] = r[p[i]];
        p[i + 1] = g[p[i + 1]];
        p[i + 2] = b[p[i + 2]];
    }
}

void colorize(Image& img, int hue, int saturation) {
    uint8_t* p = img.data();
    const float s = std::clamp(saturation, 0, 255) / 255.0f;
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        HSL c = rgb_to_hsl(p[i], p[i + 1], p[i + 2]);
        c.h = static_cast<float>(hue);
        c.s = s;
        hsl_to_rgb(c, p + i, p + i + 1, p + i + 2);
    }
}

void hsl_adjust(Image& img, int hue, int saturation, int lightness) {
    uint8_t* p = img.data();
    const float sat = std::clamp(saturation, -100, 100) / 100.0f;
    const float lig = std::clamp(lightness, -100, 100) / 100.0f;
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        HSL c = rgb_to_hsl(p[i], p[i + 1], p[i + 2]);
        c.h += hue;
        c.s = sat >= 0 ? c.s + (1 - c.s) * sat : c.s * (1 + sat);
        c.l = lig >= 0 ? c.l + (1 - c.l) * lig : c.l * (1 + lig);
        hsl_to_rgb(c, p + i, p + i + 1, p + i + 2);
    }
}

Lut levels_lut(int in_low, float gamma, int in_high, int out_low, int out_high) {
    Lut lut;
    in_low = std::clamp(in_low, 0, 255);
    in_high = std::clamp(in_high, 0, 255);
    if (in_high <= in_low) in_high = in_low + 1;
    gamma = std::max(gamma, 0.01f);
    for (int i = 0; i < 256; ++i) {
        float v = std::clamp((i - in_low) / static_cast<float>(in_high - in_low), 0.0f, 1.0f);
        v = std::pow(v, 1.0f / gamma);
        lut[i] = clamp8(out_low + v * (out_high - out_low));
    }
    return lut;
}

Lut gamma_lut(float gamma) { return levels_lut(0, gamma, 255, 0, 255); }

Lut threshold_lut(int value) {
    Lut lut;
    for (int i = 0; i < 256; ++i) lut[i] = i < value ? 0 : 255;
    return lut;
}

void greyscale_then_threshold(Image& img, int value) {
    uint8_t* p = img.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        const uint8_t v = luma(p + i) < value ? 0 : 255;
        p[i] = p[i + 1] = p[i + 2] = v;
    }
}

Lut posterize_lut(int levels) {
    Lut lut;
    levels = std::clamp(levels, 2, 255);
    for (int i = 0; i < 256; ++i) {
        const int step = i * levels / 256;
        lut[i] = clamp8(step * 255.0f / (levels - 1));
    }
    return lut;
}

Lut solarize_lut(int threshold) {
    Lut lut;
    for (int i = 0; i < 256; ++i) lut[i] = i > threshold ? static_cast<uint8_t>(255 - i) : static_cast<uint8_t>(i);
    return lut;
}

Lut brightness_contrast_lut(int brightness, int contrast) {
    const float c = std::clamp(contrast, -100, 100) / 100.0f;
    const float k = c >= 0.0f ? 1.0f / std::max(1.0f - c, 0.01f) : 1.0f + c;
    Lut lut;
    for (int i = 0; i < 256; ++i) lut[i] = clamp8((i - 128) * k + 128 + brightness);
    return lut;
}

// Fritsch-Carlson monotone cubic interpolation: no overshoot between points.
Lut curve_lut(const std::vector<std::pair<float, float>>& in) {
    std::vector<std::pair<float, float>> pts = in;
    std::sort(pts.begin(), pts.end());
    if (pts.empty()) pts = {{0, 0}, {255, 255}};
    if (pts.size() == 1) pts.push_back({pts[0].first + 1, pts[0].second});
    const size_t n = pts.size();
    std::vector<float> d(n - 1), m(n);
    for (size_t i = 0; i + 1 < n; ++i) {
        const float dx = std::max(pts[i + 1].first - pts[i].first, 1e-3f);
        d[i] = (pts[i + 1].second - pts[i].second) / dx;
    }
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (size_t i = 1; i + 1 < n; ++i) m[i] = (d[i - 1] * d[i] <= 0) ? 0.0f : (d[i - 1] + d[i]) * 0.5f;
    for (size_t i = 0; i + 1 < n; ++i) {
        if (d[i] == 0) { m[i] = m[i + 1] = 0; continue; }
        const float a = m[i] / d[i], b = m[i + 1] / d[i];
        const float h = std::hypot(a, b);
        if (h > 3.0f) { m[i] = 3.0f * a / h * d[i]; m[i + 1] = 3.0f * b / h * d[i]; }
    }
    Lut lut;
    for (int x = 0; x < 256; ++x) {
        float y;
        if (x <= pts.front().first) y = pts.front().second;
        else if (x >= pts.back().first) y = pts.back().second;
        else {
            size_t i = 0;
            while (i + 2 < n && pts[i + 1].first <= x) ++i;
            const float h = std::max(pts[i + 1].first - pts[i].first, 1e-3f);
            const float t = (x - pts[i].first) / h;
            const float t2 = t * t, t3 = t2 * t;
            y = (2 * t3 - 3 * t2 + 1) * pts[i].second + (t3 - 2 * t2 + t) * h * m[i] +
                (-2 * t3 + 3 * t2) * pts[i + 1].second + (t3 - t2) * h * m[i + 1];
        }
        lut[x] = clamp8(y);
    }
    return lut;
}

void channel_mixer(Image& img, const ChannelMix& mx) {
    uint8_t* p = img.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        const float in[3] = {static_cast<float>(p[i]), static_cast<float>(p[i + 1]), static_cast<float>(p[i + 2])};
        uint8_t out[3];
        for (int c = 0; c < 3; ++c) {
            const int row = mx.monochrome ? 0 : c;
            float v = mx.constant[row] * 2.55f;
            for (int k = 0; k < 3; ++k) v += mx.mix[row][k] / 100.0f * in[k];
            out[c] = clamp8(v);
        }
        p[i] = out[0]; p[i + 1] = out[1]; p[i + 2] = out[2];
    }
}

void color_balance(Image& img, const ColorBalance& cb) {
    // Transfer weights per input level (GIMP's classic tables).
    float shadows_w[256], midtones_w[256], highlights_w[256];
    for (int i = 0; i < 256; ++i) {
        shadows_w[i] = 1.075f - 1.0f / (i / 16.0f + 1.0f);
        midtones_w[i] = 0.667f * (1.0f - ((i - 127.0f) / 127.0f) * ((i - 127.0f) / 127.0f));
        highlights_w[i] = 1.075f - 1.0f / ((255 - i) / 16.0f + 1.0f);
    }
    Lut luts[3];
    for (int c = 0; c < 3; ++c) {
        for (int i = 0; i < 256; ++i) {
            float v = static_cast<float>(i);
            // Positive values push towards the channel; negative pull away.
            v += cb.shadows[c] * (cb.shadows[c] > 0 ? shadows_w[i] : highlights_w[i]);
            v += cb.midtones[c] * midtones_w[i];
            v += cb.highlights[c] * (cb.highlights[c] > 0 ? highlights_w[i] : shadows_w[i]);
            luts[c][i] = clamp8(v);
        }
    }
    uint8_t* p = img.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        const uint8_t r = luts[0][p[i]], g = luts[1][p[i + 1]], b = luts[2][p[i + 2]];
        if (cb.preserve_luminosity) {
            HSL before = rgb_to_hsl(p[i], p[i + 1], p[i + 2]);
            HSL after = rgb_to_hsl(r, g, b);
            after.l = before.l;
            hsl_to_rgb(after, p + i, p + i + 1, p + i + 2);
        } else {
            p[i] = r; p[i + 1] = g; p[i + 2] = b;
        }
    }
}

void sepia(Image& img, int amount) {
    const float t = std::clamp(amount, 0, 100) / 100.0f;
    uint8_t* p = img.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        const float y = luma(p + i);
        const float sr = std::min(255.0f, y * 1.15f), sg = y * 0.95f, sb = y * 0.72f;
        p[i] = clamp8(p[i] + (sr - p[i]) * t);
        p[i + 1] = clamp8(p[i + 1] + (sg - p[i + 1]) * t);
        p[i + 2] = clamp8(p[i + 2] + (sb - p[i + 2]) * t);
    }
}

void hue_map(Image& img, const HueMap& m) {
    uint8_t* p = img.data();
    const float sat = std::clamp(m.saturation, -100, 100) / 100.0f;
    const float lig = std::clamp(m.lightness, -100, 100) / 100.0f;
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        HSL c = rgb_to_hsl(p[i], p[i + 1], p[i + 2]);
        if (c.s > 0.0f) {
            // Interpolate the shift between the two nearest band centres (0, 36, 72, ...).
            const float pos = c.h / 36.0f;
            const int b0 = static_cast<int>(std::floor(pos)) % 10, b1 = (b0 + 1) % 10;
            const float t = pos - std::floor(pos);
            c.h += m.shift[b0] * (1 - t) + m.shift[b1] * t;
        }
        c.s = sat >= 0 ? c.s + (1 - c.s) * sat : c.s * (1 + sat);
        c.l = lig >= 0 ? c.l + (1 - c.l) * lig : c.l * (1 + lig);
        hsl_to_rgb(c, p + i, p + i + 1, p + i + 2);
    }
}

std::array<int, 256> histogram_luma(const Image& img) {
    std::array<int, 256> h{};
    const uint8_t* p = img.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4)
        if (p[i + 3]) ++h[luma(p + i)];
    return h;
}

void histogram_stretch(Image& img) {
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    const uint8_t* p = img.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        if (!p[i + 3]) continue;
        for (int c = 0; c < 3; ++c) { lo[c] = std::min<int>(lo[c], p[i + c]); hi[c] = std::max<int>(hi[c], p[i + c]); }
    }
    Lut luts[3];
    for (int c = 0; c < 3; ++c) luts[c] = levels_lut(lo[c], 1.0f, hi[c] > lo[c] ? hi[c] : lo[c] + 1, 0, 255);
    apply_luts(img, luts[0], luts[1], luts[2]);
}

void histogram_equalize(Image& img) {
    const std::array<int, 256> h = histogram_luma(img);
    long total = 0;
    for (int v : h) total += v;
    if (total == 0) return;
    Lut lut;
    long acc = 0;
    for (int i = 0; i < 256; ++i) {
        acc += h[i];
        lut[i] = clamp8(acc * 255.0f / total);
    }
    // Remap luma, scaling RGB to keep hue.
    uint8_t* p = img.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        const int y = luma(p + i);
        if (y == 0) continue;
        const float k = lut[y] / static_cast<float>(y);
        for (int c = 0; c < 3; ++c) p[i + c] = clamp8(p[i + c] * k);
    }
}

void auto_contrast(Image& img, float clip_percent) {
    const std::array<int, 256> h = histogram_luma(img);
    long total = 0;
    for (int v : h) total += v;
    if (total == 0) return;
    const long clip = static_cast<long>(total * clip_percent / 100.0f);
    int lo = 0, hi = 255;
    long acc = 0;
    while (lo < 255 && acc + h[lo] <= clip) acc += h[lo++];
    acc = 0;
    while (hi > lo && acc + h[hi] <= clip) acc += h[hi--];
    apply_lut(img, levels_lut(lo, 1.0f, hi, 0, 255));
}

}  // namespace firn::adjust
