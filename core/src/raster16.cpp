#include "firn/raster16.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace firn::raster16 {

namespace {
inline uint16_t q16(float v) { return static_cast<uint16_t>(std::clamp(v, 0.0f, 1.0f) * 65535.0f + 0.5f); }
inline float f16(uint16_t v) { return v / 65535.0f; }
}  // namespace

Lut16 lut_from(const std::function<float(float)>& fn) {
    Lut16 lut(65536);
    for (int i = 0; i < 65536; ++i) lut[static_cast<size_t>(i)] = q16(fn(i / 65535.0f));
    return lut;
}

Lut16 lut_from8(const adjust::Lut& lut8) {
    return lut_from([&](float v) {
        const float x = v * 255.0f;
        const int i = std::min(254, static_cast<int>(x));
        const float t = x - i;
        return (lut8[static_cast<size_t>(i)] * (1 - t) + lut8[static_cast<size_t>(i + 1)] * t) / 255.0f;
    });
}

void apply_lut(Image16& img, const Lut16& lut) {
    uint16_t* p = img.data();
    for (size_t i = 0; i < img.size(); i += 4) { p[i] = lut[p[i]]; p[i + 1] = lut[p[i + 1]]; p[i + 2] = lut[p[i + 2]]; }
}

void apply_luts(Image16& img, const Lut16& r, const Lut16& g, const Lut16& b) {
    uint16_t* p = img.data();
    for (size_t i = 0; i < img.size(); i += 4) { p[i] = r[p[i]]; p[i + 1] = g[p[i + 1]]; p[i + 2] = b[p[i + 2]]; }
}

void map_rgb(Image16& img, const std::function<void(float&, float&, float&)>& fn) {
    uint16_t* p = img.data();
    for (size_t i = 0; i < img.size(); i += 4) {
        float r = f16(p[i]), g = f16(p[i + 1]), b = f16(p[i + 2]);
        fn(r, g, b);
        p[i] = q16(r); p[i + 1] = q16(g); p[i + 2] = q16(b);
    }
}

void brightness_contrast(Image16& img, int brightness, int contrast) {
    const float b = brightness / 255.0f, c = contrast / 100.0f;
    const float k = c >= 0 ? 1.0f / std::max(0.01f, 1.0f - c) : 1.0f + c;
    apply_lut(img, lut_from([&](float v) { return (v + b - 0.5f) * k + 0.5f; }));
}

void levels(Image16& img, int in_low, float gamma_v, int in_high, int out_low, int out_high) {
    const float lo = in_low / 255.0f, hi = std::max(in_high / 255.0f, lo + 1e-4f), olo = out_low / 255.0f, ohi = out_high / 255.0f;
    apply_lut(img, lut_from([&](float v) { const float t = std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f); return olo + std::pow(t, 1.0f / std::max(gamma_v, 0.01f)) * (ohi - olo); }));
}

void gamma(Image16& img, float r, float g, float b) {
    apply_luts(img, lut_from([&](float v) { return std::pow(v, 1.0f / std::max(r, 0.01f)); }), lut_from([&](float v) { return std::pow(v, 1.0f / std::max(g, 0.01f)); }), lut_from([&](float v) { return std::pow(v, 1.0f / std::max(b, 0.01f)); }));
}

void curves(Image16& img, const std::vector<std::pair<float, float>>& points) { apply_lut(img, lut_from8(adjust::curve_lut(points))); }
void invert(Image16& img) { apply_lut(img, lut_from([](float v) { return 1.0f - v; })); }

void threshold(Image16& img, int value) {
    const float t = value / 255.0f;
    map_rgb(img, [&](float& r, float& g, float& b) { const float l = 0.299f * r + 0.587f * g + 0.114f * b; r = g = b = l >= t ? 1.0f : 0.0f; });
}

void posterize(Image16& img, int levels_n) {
    const int n = std::clamp(levels_n, 2, 255);
    apply_lut(img, lut_from([&](float v) { return std::floor(v * (n - 1) + 0.5f) / (n - 1); }));
}

void grayscale(Image16& img) {
    map_rgb(img, [](float& r, float& g, float& b) { const float l = 0.299f * r + 0.587f * g + 0.114f * b; r = g = b = l; });
}

namespace {
struct HSLf { float h, s, l; };
HSLf rgb_to_hsl(float r, float g, float b) {
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    HSLf o{0, 0, (mx + mn) * 0.5f};
    const float d = mx - mn;
    if (d < 1e-6f) return o;
    o.s = o.l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    if (mx == r) o.h = std::fmod((g - b) / d + 6.0f, 6.0f);
    else if (mx == g) o.h = (b - r) / d + 2.0f;
    else o.h = (r - g) / d + 4.0f;
    o.h *= 60.0f;
    return o;
}
void hsl_to_rgb(HSLf c, float& r, float& g, float& b) {
    auto f = [&](float n) { const float k = std::fmod(n + c.h / 30.0f, 12.0f); const float a = c.s * std::min(c.l, 1 - c.l); return c.l - a * std::max(-1.0f, std::min({k - 3.0f, 9.0f - k, 1.0f})); };
    r = f(0); g = f(8); b = f(4);
}
}  // namespace

void hsl_adjust(Image16& img, int hue, int saturation, int lightness) {
    const float sh = static_cast<float>(hue), sat = saturation / 100.0f, lig = lightness / 100.0f;
    map_rgb(img, [&](float& r, float& g, float& b) {
        HSLf c = rgb_to_hsl(r, g, b);
        c.h = std::fmod(c.h + sh + 360.0f, 360.0f);
        c.s = sat >= 0 ? c.s + (1.0f - c.s) * sat : c.s * (1.0f + sat);
        c.l = lig >= 0 ? c.l + (1.0f - c.l) * lig : c.l * (1.0f + lig);
        hsl_to_rgb(c, r, g, b);
    });
}

void colorize(Image16& img, int hue, int saturation) {
    const float h = static_cast<float>(hue), s = saturation / 255.0f;
    map_rgb(img, [&](float& r, float& g, float& b) { const float l = rgb_to_hsl(r, g, b).l; hsl_to_rgb({h, s, l}, r, g, b); });
}

void color_balance(Image16& img, const adjust::ColorBalance& cb) {
    map_rgb(img, [&](float& r, float& g, float& b) {
        const float l = 0.299f * r + 0.587f * g + 0.114f * b;
        const float ws = std::clamp(1.0f - l * 2.0f, 0.0f, 1.0f), wh = std::clamp(l * 2.0f - 1.0f, 0.0f, 1.0f), wm = 1.0f - ws - wh;
        float v[3] = {r, g, b};
        for (int c = 0; c < 3; ++c) v[c] += (cb.shadows[c] * ws + cb.midtones[c] * wm + cb.highlights[c] * wh) / 100.0f * 0.5f;
        if (cb.preserve_luminosity) { const float nl = 0.299f * v[0] + 0.587f * v[1] + 0.114f * v[2]; const float d = l - nl; for (float& x : v) x += d; }
        r = v[0]; g = v[1]; b = v[2];
    });
}

void channel_mixer(Image16& img, const adjust::ChannelMix& m) {
    map_rgb(img, [&](float& r, float& g, float& b) {
        const float in[3] = {r, g, b};
        float out[3];
        for (int c = 0; c < 3; ++c) {
            const int row = m.monochrome ? 0 : c;
            out[c] = (m.mix[row][0] * in[0] + m.mix[row][1] * in[1] + m.mix[row][2] * in[2] + m.constant[row]) / 100.0f;
        }
        r = out[0]; g = out[1]; b = out[2];
    });
}

void fill(Image16& img, Color c) {
    uint16_t* p = img.data();
    for (size_t i = 0; i < img.size(); i += 4) { p[i] = static_cast<uint16_t>(c.r * 257); p[i + 1] = static_cast<uint16_t>(c.g * 257); p[i + 2] = static_cast<uint16_t>(c.b * 257); p[i + 3] = static_cast<uint16_t>(c.a * 257); }
}

namespace {
// Float planes (premultiplied) for resampling.
struct Planes {
    int w, h;
    std::vector<float> v;   // w*h*4 premultiplied
    explicit Planes(const Image16& img) : w(img.width()), h(img.height()), v(static_cast<size_t>(img.width()) * img.height() * 4) {
        const uint16_t* s = img.data();
        for (size_t i = 0; i < v.size(); i += 4) { const float a = f16(s[i + 3]); v[i] = f16(s[i]) * a; v[i + 1] = f16(s[i + 1]) * a; v[i + 2] = f16(s[i + 2]) * a; v[i + 3] = a; }
    }
    Planes(int w_, int h_) : w(w_), h(h_), v(static_cast<size_t>(w_) * h_ * 4, 0.0f) {}
    Image16 to_image() const {
        Image16 out(w, h);
        uint16_t* d = out.data();
        for (size_t i = 0; i < v.size(); i += 4) {
            const float a = std::clamp(v[i + 3], 0.0f, 1.0f);
            for (int c = 0; c < 3; ++c) d[i + c] = a > 0 ? q16(v[i + c] / a) : 0;
            d[i + 3] = q16(a);
        }
        return out;
    }
    void sample_bilinear(float x, float y, float* out) const {
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const float fx = x - x0, fy = y - y0;
        for (int c = 0; c < 4; ++c) out[c] = 0;
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const int px = x0 + i, py = y0 + j;
                if (px < 0 || py < 0 || px >= w || py >= h) continue;
                const float wt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                const float* s = v.data() + (static_cast<size_t>(py) * w + px) * 4;
                for (int c = 0; c < 4; ++c) out[c] += s[c] * wt;
            }
    }
};
float cubic(float t) { t = std::abs(t); return t < 1 ? 1.5f * t * t * t - 2.5f * t * t + 1 : t < 2 ? -0.5f * t * t * t + 2.5f * t * t - 4 * t + 2 : 0.0f; }
}  // namespace

void gaussian_blur(Image16& img, float radius) {
    if (radius <= 0.0f) return;
    Planes p(img);
    const int r = std::max(1, static_cast<int>(std::ceil(radius * 3)));
    std::vector<float> k(static_cast<size_t>(2 * r + 1));
    float sum = 0;
    for (int i = -r; i <= r; ++i) { k[static_cast<size_t>(i + r)] = std::exp(-(i * i) / (2.0f * radius * radius)); sum += k[static_cast<size_t>(i + r)]; }
    for (float& x : k) x /= sum;
    Planes tmp(p.w, p.h);
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x)
            for (int c = 0; c < 4; ++c) {
                float acc = 0;
                for (int i = -r; i <= r; ++i) acc += p.v[(static_cast<size_t>(y) * p.w + std::clamp(x + i, 0, p.w - 1)) * 4 + c] * k[static_cast<size_t>(i + r)];
                tmp.v[(static_cast<size_t>(y) * p.w + x) * 4 + c] = acc;
            }
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x)
            for (int c = 0; c < 4; ++c) {
                float acc = 0;
                for (int i = -r; i <= r; ++i) acc += tmp.v[(static_cast<size_t>(std::clamp(y + i, 0, p.h - 1)) * p.w + x) * 4 + c] * k[static_cast<size_t>(i + r)];
                p.v[(static_cast<size_t>(y) * p.w + x) * 4 + c] = acc;
            }
    img = p.to_image();
}

Image16 crop(const Image16& src, raster::Rect r) {
    Image16 out(r.x1 - r.x0, r.y1 - r.y0);
    for (int y = r.y0; y < r.y1; ++y) {
        if (y < 0 || y >= src.height()) continue;
        const int x0 = std::max(r.x0, 0), x1 = std::min(r.x1, src.width());
        if (x1 <= x0) continue;
        std::memcpy(out.data() + (static_cast<size_t>(y - r.y0) * out.width() + (x0 - r.x0)) * 4, src.data() + (static_cast<size_t>(y) * src.width() + x0) * 4, static_cast<size_t>(x1 - x0) * 8);
    }
    return out;
}

Image16 resample(const Image16& src, int w, int h, raster::Filter filter) {
    if (w == src.width() && h == src.height()) return src;
    const Planes p(src);
    Planes out(w, h);
    const float sx = static_cast<float>(src.width()) / w, sy = static_cast<float>(src.height()) / h;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float fx = (x + 0.5f) * sx - 0.5f, fy = (y + 0.5f) * sy - 0.5f;
            float* d = out.v.data() + (static_cast<size_t>(y) * w + x) * 4;
            if (filter == raster::Filter::Nearest) {
                const int px = std::clamp(static_cast<int>(std::floor(fx + 0.5f)), 0, src.width() - 1), py = std::clamp(static_cast<int>(std::floor(fy + 0.5f)), 0, src.height() - 1);
                std::memcpy(d, p.v.data() + (static_cast<size_t>(py) * src.width() + px) * 4, 16);
            } else if (filter == raster::Filter::Bilinear || sx < 1.0f || sy < 1.0f) {
                p.sample_bilinear(fx, fy, d);
                if (filter != raster::Filter::Bilinear && (sx < 1.0f || sy < 1.0f)) {
                    // Bicubic upsampling.
                    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
                    float acc[4] = {0, 0, 0, 0}, wsum = 0;
                    for (int j = -1; j <= 2; ++j)
                        for (int i = -1; i <= 2; ++i) {
                            const int px = std::clamp(x0 + i, 0, src.width() - 1), py = std::clamp(y0 + j, 0, src.height() - 1);
                            const float wt = cubic(fx - (x0 + i)) * cubic(fy - (y0 + j));
                            const float* s = p.v.data() + (static_cast<size_t>(py) * src.width() + px) * 4;
                            for (int c = 0; c < 4; ++c) acc[c] += s[c] * wt;
                            wsum += wt;
                        }
                    if (wsum > 0) for (int c = 0; c < 4; ++c) d[c] = acc[c] / wsum;
                }
            } else {
                // Box average when shrinking.
                const int x0 = std::max(0, static_cast<int>(x * sx)), x1 = std::min(src.width(), std::max(x0 + 1, static_cast<int>((x + 1) * sx)));
                const int y0 = std::max(0, static_cast<int>(y * sy)), y1 = std::min(src.height(), std::max(y0 + 1, static_cast<int>((y + 1) * sy)));
                float acc[4] = {0, 0, 0, 0}; int n = 0;
                for (int py = y0; py < y1; ++py) for (int px = x0; px < x1; ++px) { const float* s = p.v.data() + (static_cast<size_t>(py) * src.width() + px) * 4; for (int c = 0; c < 4; ++c) acc[c] += s[c]; ++n; }
                for (int c = 0; c < 4; ++c) d[c] = n ? acc[c] / n : 0;
            }
        }
    return out.to_image();
}

Image16 rotate_quarter(const Image16& src, int quarter_turns) {
    const int q = ((quarter_turns % 4) + 4) % 4;
    if (q == 0) return src;
    const int sw = src.width(), sh = src.height();
    const int w = (q % 2) ? sh : sw, h = (q % 2) ? sw : sh;
    Image16 out(w, h);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            int dx, dy;
            if (q == 1) { dx = sh - 1 - y; dy = x; } else if (q == 2) { dx = sw - 1 - x; dy = sh - 1 - y; } else { dx = y; dy = sw - 1 - x; }
            std::memcpy(out.data() + (static_cast<size_t>(dy) * w + dx) * 4, src.data() + (static_cast<size_t>(y) * sw + x) * 4, 8);
        }
    return out;
}

Image16 rotate(const Image16& src, float degrees) {
    int w, h;
    raster::rotated_size(src.width(), src.height(), degrees, &w, &h);
    const Planes p(src);
    Planes out(w, h);
    const float rad = degrees * 3.14159265f / 180.0f, cs = std::cos(rad), sn = std::sin(rad);
    const float cx = src.width() * 0.5f, cy = src.height() * 0.5f, ox = w * 0.5f, oy = h * 0.5f;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float dx = (x + 0.5f) - ox, dy = (y + 0.5f) - oy;
            p.sample_bilinear(cx + dx * cs + dy * sn - 0.5f, cy - dx * sn + dy * cs - 0.5f, out.v.data() + (static_cast<size_t>(y) * w + x) * 4);
        }
    return out.to_image();
}

void flip_vertical(Image16& img) {
    const size_t row = static_cast<size_t>(img.width()) * 4;
    std::vector<uint16_t> tmp(row);
    for (int y = 0; y < img.height() / 2; ++y) {
        uint16_t* a = img.data() + y * row; uint16_t* b = img.data() + (img.height() - 1 - y) * row;
        std::memcpy(tmp.data(), a, row * 2); std::memcpy(a, b, row * 2); std::memcpy(b, tmp.data(), row * 2);
    }
}

void mirror_horizontal(Image16& img) {
    for (int y = 0; y < img.height(); ++y) {
        uint16_t* r = img.data() + static_cast<size_t>(y) * img.width() * 4;
        for (int x = 0; x < img.width() / 2; ++x) for (int c = 0; c < 4; ++c) std::swap(r[x * 4 + c], r[(img.width() - 1 - x) * 4 + c]);
    }
}

Image16 shifted(const Image16& src, int dx, int dy) {
    Image16 out(src.width(), src.height());
    for (int y = 0; y < src.height(); ++y) {
        const int sy = y - dy;
        if (sy < 0 || sy >= src.height()) continue;
        for (int x = 0; x < src.width(); ++x) {
            const int sx = x - dx;
            if (sx < 0 || sx >= src.width()) continue;
            std::memcpy(out.data() + (static_cast<size_t>(y) * src.width() + x) * 4, src.data() + (static_cast<size_t>(sy) * src.width() + sx) * 4, 8);
        }
    }
    return out;
}

void apply_through_mask(Image16& dst, const Image16& before, const Mask& mask) {
    if (mask.empty()) return;
    uint16_t* d = dst.data();
    const uint16_t* b = before.data();
    for (size_t i = 0; i < mask.size() && i * 4 + 3 < dst.size(); ++i) {
        const int m = mask.data()[i];
        if (m == 255) continue;
        for (int c = 0; c < 4; ++c) d[i * 4 + c] = static_cast<uint16_t>((d[i * 4 + c] * m + b[i * 4 + c] * (255 - m)) / 255);
    }
}

}  // namespace firn::raster16
