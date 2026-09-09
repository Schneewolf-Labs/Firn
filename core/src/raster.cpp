#include "firn/raster.h"
#include "firn/adjust.h"

#include "firn/mask.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <cstring>

namespace firn::raster {

// --- Rect --------------------------------------------------------------

Rect Rect::clipped(int w, int h) const {
    return {std::max(x0, 0), std::max(y0, 0), std::min(x1, w), std::min(y1, h)};
}

Rect Rect::united(const Rect& o) const {
    if (empty()) return o;
    if (o.empty()) return *this;
    return {std::min(x0, o.x0), std::min(y0, o.y0), std::max(x1, o.x1), std::max(y1, o.y1)};
}

// --- Pixel blending ----------------------------------------------------

void blend_over(Image& img, int x, int y, Color c, float coverage) {
    uint8_t* d = img.data() + (static_cast<size_t>(y) * img.width() + x) * 4;
    const float sa = (c.a / 255.0f) * std::clamp(coverage, 0.0f, 1.0f);
    if (sa <= 0.0f) return;
    const float da = d[3] / 255.0f;
    const float oa = sa + da * (1.0f - sa);
    const uint8_t src[3] = {c.r, c.g, c.b};
    for (int i = 0; i < 3; ++i) {
        const float oc = (src[i] / 255.0f * sa + d[i] / 255.0f * da * (1.0f - sa)) / oa;
        d[i] = static_cast<uint8_t>(oc * 255.0f + 0.5f);
    }
    d[3] = static_cast<uint8_t>(oa * 255.0f + 0.5f);
}

// --- Stroke ------------------------------------------------------------

Stroke::Stroke(const Image& base, Brush brush, Color color, StrokeMode mode, const Mask* clip)
    : base_(base), brush_(brush), color_(color), mode_(mode), clip_(clip && !clip->empty() ? clip : nullptr),
      mask_(static_cast<size_t>(base.width()) * base.height(), 0.0f) {}

std::shared_ptr<const BrushTip> BrushTip::from_image(const Image& img) {
    auto tip = std::make_shared<BrushTip>();
    tip->width = img.width();
    tip->height = img.height();
    tip->coverage.resize(static_cast<size_t>(img.width()) * img.height());
    for (size_t i = 0; i < tip->coverage.size(); ++i) {
        const uint8_t* p = img.data() + i * 4;
        const float luma = (p[0] * 299 + p[1] * 587 + p[2] * 114) / 255000.0f;
        tip->coverage[i] = (1.0f - luma) * (p[3] / 255.0f);
    }
    return tip;
}

std::shared_ptr<const BrushTip> BrushTip::texture_from_image(const Image& img) {
    auto t = std::make_shared<BrushTip>();
    t->width = img.width();
    t->height = img.height();
    t->coverage.resize(static_cast<size_t>(img.width()) * img.height());
    for (size_t i = 0; i < t->coverage.size(); ++i) {
        const uint8_t* p = img.data() + i * 4;
        t->coverage[i] = (p[0] * 299 + p[1] * 587 + p[2] * 114) / 255000.0f;
    }
    return t;
}

// Texture weight at an image pixel: 1 without a texture.
static inline float texture_weight(const Brush& b, int x, int y) {
    if (!b.texture || b.texture->width <= 0 || b.texture_strength <= 0.0f) return 1.0f;
    const int tx = ((x % b.texture->width) + b.texture->width) % b.texture->width;
    const int ty = ((y % b.texture->height) + b.texture->height) % b.texture->height;
    const float t = b.texture->coverage[static_cast<size_t>(ty) * b.texture->width + tx];
    return 1.0f - b.texture_strength * (1.0f - t);
}

void Stroke::stamp(float cx, float cy) {
    if (brush_.tip && brush_.tip->width > 0) { stamp_tip(cx, cy); return; }
    const float r = std::max(brush_.size * 0.5f, 0.5f);
    const float inner = r * std::clamp(brush_.hardness, 0.0f, 1.0f);
    const int w = base_.width(), h = base_.height();
    Rect box{static_cast<int>(std::floor(cx - r)), static_cast<int>(std::floor(cy - r)),
             static_cast<int>(std::ceil(cx + r)) + 1, static_cast<int>(std::ceil(cy + r)) + 1};
    box = box.clipped(w, h);
    if (box.empty()) return;

    for (int y = box.y0; y < box.y1; ++y) {
        for (int x = box.x0; x < box.x1; ++x) {
            const float dx = (x + 0.5f) - cx, dy = (y + 0.5f) - cy;
            const float d = brush_.square ? std::max(std::abs(dx), std::abs(dy)) : std::sqrt(dx * dx + dy * dy);
            float cov;
            if (d <= inner) cov = 1.0f;
            else if (brush_.hardness >= 1.0f || r - inner < 1.0f) cov = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
            else cov = std::clamp((r - d) / (r - inner), 0.0f, 1.0f);
            if (cov <= 0.0f) continue;
            cov *= texture_weight(brush_, x, y);
            float& m = mask_[static_cast<size_t>(y) * w + x];
            m = brush_.accumulate ? std::min(1.0f, m + cov * brush_.flow) : std::max(m, cov);
        }
    }
    pending_ = pending_.united(box);
}

// Custom tip: the tip image scaled so its longer side equals the brush
// size, sampled bilinearly, centered on (cx, cy).
void Stroke::stamp_tip(float cx, float cy) {
    const BrushTip& tip = *brush_.tip;
    const float scale = std::max(brush_.size, 1.0f) / std::max(tip.width, tip.height);
    const float tw = tip.width * scale, th = tip.height * scale;
    const int w = base_.width(), h = base_.height();
    Rect box{static_cast<int>(std::floor(cx - tw * 0.5f)), static_cast<int>(std::floor(cy - th * 0.5f)),
             static_cast<int>(std::ceil(cx + tw * 0.5f)) + 1, static_cast<int>(std::ceil(cy + th * 0.5f)) + 1};
    box = box.clipped(w, h);
    if (box.empty()) return;
    auto sample = [&](float u, float v) {  // tip coords in pixels
        const int x0 = static_cast<int>(std::floor(u)), y0 = static_cast<int>(std::floor(v));
        const float fx = u - x0, fy = v - y0;
        float acc = 0.0f;
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const int px = x0 + i, py = y0 + j;
                if (px < 0 || py < 0 || px >= tip.width || py >= tip.height) continue;
                acc += tip.coverage[static_cast<size_t>(py) * tip.width + px] * (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            }
        return acc;
    };
    for (int y = box.y0; y < box.y1; ++y)
        for (int x = box.x0; x < box.x1; ++x) {
            const float u = ((x + 0.5f) - (cx - tw * 0.5f)) / scale - 0.5f;
            const float v = ((y + 0.5f) - (cy - th * 0.5f)) / scale - 0.5f;
            const float cov = std::clamp(sample(u, v), 0.0f, 1.0f) * texture_weight(brush_, x, y);
            if (cov <= 0.0f) continue;
            float& m = mask_[static_cast<size_t>(y) * w + x];
            m = brush_.accumulate ? std::min(1.0f, m + cov * brush_.flow) : std::max(m, cov);
        }
    pending_ = pending_.united(box);
}

void Stroke::add_point(float x, float y) {
    if (!has_last_) {
        stamp(x, y);
        has_last_ = true;
        last_x_ = x;
        last_y_ = y;
        return;
    }
    const float spacing = std::max(brush_.size * brush_.step, 0.5f);
    const float dx = x - last_x_, dy = y - last_y_;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0f) return;
    float t = spacing - carry_;
    while (t <= len) {
        stamp(last_x_ + dx * (t / len), last_y_ + dy * (t / len));
        t += spacing;
    }
    carry_ = len - (t - spacing);
    last_x_ = x;
    last_y_ = y;
}

Rect Stroke::render(Image& dst) {
    const Rect r = pending_;
    pending_ = {};
    if (r.empty()) return r;
    const int w = base_.width();
    const float op = std::clamp(brush_.opacity, 0.0f, 1.0f);
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            float m = mask_[i] * op;
            if (clip_) m *= clip_->data()[i] / 255.0f;
            const uint8_t* b = base_.data() + i * 4;
            uint8_t* d = dst.data() + i * 4;
            if (m <= 0.0f) {
                std::memcpy(d, b, 4);
                continue;
            }
            if (mode_ == StrokeMode::Erase) {
                std::memcpy(d, b, 3);
                d[3] = static_cast<uint8_t>(b[3] * (1.0f - m) + 0.5f);
            } else if (mode_ == StrokeMode::Clone) {
                std::memcpy(d, b, 4);
                const int sx = x + clone_ox_, sy = y + clone_oy_;
                if (clone_ && sx >= 0 && sy >= 0 && sx < clone_->width() && sy < clone_->height())
                    blend_over(dst, x, y, clone_->get(sx, sy), m);
            } else if (mode_ == StrokeMode::Filter) {
                const Color before{b[0], b[1], b[2], b[3]};
                const Color after = area_filter_ ? area_filter_(base_, x, y) : filter_ ? filter_(before) : before;
                d[0] = static_cast<uint8_t>(b[0] + (after.r - b[0]) * m + 0.5f);
                d[1] = static_cast<uint8_t>(b[1] + (after.g - b[1]) * m + 0.5f);
                d[2] = static_cast<uint8_t>(b[2] + (after.b - b[2]) * m + 0.5f);
                d[3] = static_cast<uint8_t>(b[3] + (after.a - b[3]) * m + 0.5f);
            } else {
                std::memcpy(d, b, 4);
                blend_over(dst, x, y, color_, m);
            }
        }
    }
    return r;
}

// --- Flood fill --------------------------------------------------------

Rect flood_fill(Image& img, int x, int y, Color color, int tolerance, float opacity, const Mask* clip) {
    const int w = img.width(), h = img.height();
    if (x < 0 || y < 0 || x >= w || y >= h) return {};
    if (clip && clip->empty()) clip = nullptr;
    if (clip && clip->at(x, y) == 0) return {};
    const Color seed = img.get(x, y);
    auto matches = [&](int px, int py) {
        const Color c = img.get(px, py);
        const int d = std::max({std::abs(c.r - seed.r), std::abs(c.g - seed.g),
                                std::abs(c.b - seed.b), std::abs(c.a - seed.a)});
        return d <= tolerance;
    };

    // Two passes: find the region first (so blending doesn't change what
    // "matches" mid-fill), then paint it.
    std::vector<uint8_t> in(static_cast<size_t>(w) * h, 0);
    std::vector<std::pair<int, int>> stack{{x, y}};
    in[static_cast<size_t>(y) * w + x] = 1;
    Rect bounds{x, y, x + 1, y + 1};
    while (!stack.empty()) {
        auto [px, py] = stack.back();
        stack.pop_back();
        // Scanline: walk left and right from (px,py).
        int lx = px;
        while (lx > 0 && !in[static_cast<size_t>(py) * w + lx - 1] && matches(lx - 1, py)) --lx;
        int rx = px;
        while (rx + 1 < w && !in[static_cast<size_t>(py) * w + rx + 1] && matches(rx + 1, py)) ++rx;
        for (int i = lx; i <= rx; ++i) in[static_cast<size_t>(py) * w + i] = 1;
        bounds = bounds.united({lx, py, rx + 1, py + 1});
        for (int ny : {py - 1, py + 1}) {
            if (ny < 0 || ny >= h) continue;
            bool run = false;
            for (int i = lx; i <= rx; ++i) {
                const size_t idx = static_cast<size_t>(ny) * w + i;
                if (!in[idx] && matches(i, ny)) {
                    if (!run) { stack.emplace_back(i, ny); run = true; }
                } else {
                    run = false;
                }
            }
        }
    }
    for (int py = bounds.y0; py < bounds.y1; ++py)
        for (int px = bounds.x0; px < bounds.x1; ++px)
            if (in[static_cast<size_t>(py) * w + px])
                blend_over(img, px, py, color, opacity * (clip ? clip->at(px, py) / 255.0f : 1.0f));
    return bounds;
}

void paint_mask(Image& dst, const Mask& shape, Color color, const Mask* clip) {
    if (clip && clip->empty()) clip = nullptr;
    const raster::Rect b = shape.bounds();
    for (int y = b.y0; y < b.y1; ++y)
        for (int x = b.x0; x < b.x1; ++x) {
            float cov = shape.at(x, y) / 255.0f;
            if (clip) cov *= clip->at(x, y) / 255.0f;
            if (cov > 0.0f) blend_over(dst, x, y, color, cov);
        }
}

void apply_through_mask(Image& dst, const Image& before, const Mask& mask) {
    if (mask.empty()) return;
    const size_t n = static_cast<size_t>(dst.width()) * dst.height();
    uint8_t* d = dst.data();
    const uint8_t* b = before.data();
    const uint8_t* m = mask.data();
    for (size_t i = 0; i < n; ++i) {
        const int k = m[i];
        if (k == 255) continue;
        for (int c = 0; c < 4; ++c) {
            const int v = (d[i * 4 + c] * k + b[i * 4 + c] * (255 - k) + 127) / 255;
            d[i * 4 + c] = static_cast<uint8_t>(v);
        }
    }
}

// --- Whole-image ops ---------------------------------------------------

void grayscale(Image& img) {
    uint8_t* p = img.data();
    const size_t n = img.size_bytes();
    for (size_t i = 0; i < n; i += 4) {
        // Rec.601 luma, matching the original's Grayscale command.
        const int y = (p[i] * 299 + p[i + 1] * 587 + p[i + 2] * 114 + 500) / 1000;
        p[i] = p[i + 1] = p[i + 2] = static_cast<uint8_t>(y);
    }
}

void brightness_contrast(Image& img, int brightness, int contrast) {
    // Contrast scales about mid-gray; brightness is a plain offset.
    const float c = std::clamp(contrast, -100, 100) / 100.0f;
    const float k = c >= 0.0f ? 1.0f / std::max(1.0f - c, 0.01f) : 1.0f + c;
    uint8_t lut[256];
    for (int i = 0; i < 256; ++i) {
        float v = (i - 128) * k + 128 + brightness;
        lut[i] = static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
    }
    uint8_t* p = img.data();
    const size_t n = img.size_bytes();
    for (size_t i = 0; i < n; i += 4) {
        p[i] = lut[p[i]];
        p[i + 1] = lut[p[i + 1]];
        p[i + 2] = lut[p[i + 2]];
    }
}

void gaussian_blur(Image& img, float radius) {
    const int w = img.width(), h = img.height();
    if (radius <= 0.0f || w == 0 || h == 0) return;
    const float sigma = radius;
    const int extent = std::max(1, static_cast<int>(std::ceil(sigma * 3.0f)));
    std::vector<float> kernel(2 * extent + 1);
    float sum = 0.0f;
    for (int i = -extent; i <= extent; ++i) {
        kernel[i + extent] = std::exp(-(i * i) / (2.0f * sigma * sigma));
        sum += kernel[i + extent];
    }
    for (float& k : kernel) k /= sum;

    // Blur premultiplied so transparent pixels don't bleed their color in.
    std::vector<float> pre(static_cast<size_t>(w) * h * 4), tmp(pre.size());
    const uint8_t* src = img.data();
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        const float a = src[i * 4 + 3] / 255.0f;
        pre[i * 4 + 0] = src[i * 4 + 0] * a;
        pre[i * 4 + 1] = src[i * 4 + 1] * a;
        pre[i * 4 + 2] = src[i * 4 + 2] * a;
        pre[i * 4 + 3] = src[i * 4 + 3];
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int k = -extent; k <= extent; ++k) {
                const int xx = std::clamp(x + k, 0, w - 1);
                const float* s = &pre[(static_cast<size_t>(y) * w + xx) * 4];
                const float kw = kernel[k + extent];
                for (int c = 0; c < 4; ++c) acc[c] += s[c] * kw;
            }
            float* d = &tmp[(static_cast<size_t>(y) * w + x) * 4];
            for (int c = 0; c < 4; ++c) d[c] = acc[c];
        }
    }
    uint8_t* out = img.data();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int k = -extent; k <= extent; ++k) {
                const int yy = std::clamp(y + k, 0, h - 1);
                const float* s = &tmp[(static_cast<size_t>(yy) * w + x) * 4];
                const float kw = kernel[k + extent];
                for (int c = 0; c < 4; ++c) acc[c] += s[c] * kw;
            }
            uint8_t* d = out + (static_cast<size_t>(y) * w + x) * 4;
            const float a = acc[3] / 255.0f;
            for (int c = 0; c < 3; ++c)
                d[c] = a > 0.0f ? static_cast<uint8_t>(std::clamp(acc[c] / a, 0.0f, 255.0f) + 0.5f) : 0;
            d[3] = static_cast<uint8_t>(std::clamp(acc[3], 0.0f, 255.0f) + 0.5f);
        }
    }
}

// Separable box blur on all four channels. Straight-alpha blur is not
// color-correct at transparent edges; premultiplied comes later.
void box_blur(Image& img, int radius) {
    const int w = img.width(), h = img.height(), r = std::max(0, radius);
    if (r == 0 || w == 0 || h == 0) return;

    std::vector<uint8_t> tmp(img.size_bytes());
    const uint8_t* src = img.data();
    uint8_t* dst = tmp.data();

    // Horizontal pass: src -> tmp
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int sum[4] = {0, 0, 0, 0}, cnt = 0;
            for (int k = -r; k <= r; ++k) {
                int xx = std::clamp(x + k, 0, w - 1);
                const uint8_t* s = src + (static_cast<size_t>(y) * w + xx) * 4;
                for (int c = 0; c < 4; ++c) sum[c] += s[c];
                ++cnt;
            }
            uint8_t* d = dst + (static_cast<size_t>(y) * w + x) * 4;
            for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(sum[c] / cnt);
        }
    }
    // Vertical pass: tmp -> img
    uint8_t* out = img.data();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int sum[4] = {0, 0, 0, 0}, cnt = 0;
            for (int k = -r; k <= r; ++k) {
                int yy = std::clamp(y + k, 0, h - 1);
                const uint8_t* s = dst + (static_cast<size_t>(yy) * w + x) * 4;
                for (int c = 0; c < 4; ++c) sum[c] += s[c];
                ++cnt;
            }
            uint8_t* d = out + (static_cast<size_t>(y) * w + x) * 4;
            for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(sum[c] / cnt);
        }
    }
}

void flip_vertical(Image& img) {
    const int w = img.width(), h = img.height();
    const size_t row = static_cast<size_t>(w) * 4;
    std::vector<uint8_t> tmp(row);
    for (int y = 0; y < h / 2; ++y) {
        uint8_t* a = img.data() + y * row;
        uint8_t* b = img.data() + (h - 1 - y) * row;
        std::memcpy(tmp.data(), a, row);
        std::memcpy(a, b, row);
        std::memcpy(b, tmp.data(), row);
    }
}

void mirror_horizontal(Image& img) {
    const int w = img.width(), h = img.height();
    for (int y = 0; y < h; ++y) {
        uint8_t* r = img.data() + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w / 2; ++x) {
            uint8_t* a = r + x * 4;
            uint8_t* b = r + (w - 1 - x) * 4;
            for (int c = 0; c < 4; ++c) std::swap(a[c], b[c]);
        }
    }
}

}  // namespace firn::raster

// --- Geometry -----------------------------------------------------------
namespace firn::raster {
namespace {

float catmull_rom(float x) {
    x = std::abs(x);
    if (x < 1.0f) return 1.5f * x * x * x - 2.5f * x * x + 1.0f;
    if (x < 2.0f) return -0.5f * x * x * x + 2.5f * x * x - 4.0f * x + 2.0f;
    return 0.0f;
}
float triangle(float x) { x = std::abs(x); return x < 1.0f ? 1.0f - x : 0.0f; }

// Precomputed per-output-pixel taps for a 1-D pass.
struct Taps {
    std::vector<int> start;       // first source index per output index
    std::vector<int> count;       // number of taps
    std::vector<float> weights;   // count taps per output, normalized
    int max_count = 0;
};

Taps make_taps(int src_n, int dst_n, Filter filter) {
    Taps t;
    const float scale = static_cast<float>(src_n) / dst_n;
    const float blur = std::max(1.0f, scale);  // widen support when shrinking
    const float support = (filter == Filter::Bicubic ? 2.0f : filter == Filter::Bilinear ? 1.0f : 0.5f) * blur;
    t.start.resize(dst_n); t.count.resize(dst_n);
    std::vector<float> w;
    for (int i = 0; i < dst_n; ++i) {
        const float center = (i + 0.5f) * scale;
        int lo = static_cast<int>(std::floor(center - support)), hi = static_cast<int>(std::ceil(center + support));
        lo = std::max(lo, 0); hi = std::min(hi, src_n - 1);
        if (hi < lo) { lo = hi = std::clamp(static_cast<int>(center), 0, src_n - 1); }
        float sum = 0.0f;
        w.clear();
        for (int j = lo; j <= hi; ++j) {
            const float x = ((j + 0.5f) - center) / blur;
            float k;
            if (filter == Filter::Bicubic) k = catmull_rom(x);
            else if (filter == Filter::Bilinear) k = triangle(x);
            else k = std::abs(x) < 0.5f ? 1.0f : 0.0f;
            w.push_back(k);
            sum += k;
        }
        if (sum == 0.0f) {  // nearest: pick the closest tap
            int best = std::clamp(static_cast<int>(center), lo, hi);
            std::fill(w.begin(), w.end(), 0.0f);
            w[best - lo] = 1.0f;
            sum = 1.0f;
        }
        t.start[i] = lo; t.count[i] = hi - lo + 1;
        t.max_count = std::max(t.max_count, t.count[i]);
        for (float v : w) t.weights.push_back(v / sum);
    }
    return t;
}

}  // namespace

Image resample(const Image& src, int w, int h, Filter filter) {
    const int sw = src.width(), sh = src.height();
    if (w <= 0 || h <= 0 || sw == 0 || sh == 0) return Image(std::max(w, 0), std::max(h, 0));
    // Premultiply into float.
    std::vector<float> pre(static_cast<size_t>(sw) * sh * 4);
    for (size_t i = 0; i < static_cast<size_t>(sw) * sh; ++i) {
        const uint8_t* s = src.data() + i * 4;
        const float a = s[3] / 255.0f;
        pre[i * 4 + 0] = s[0] * a; pre[i * 4 + 1] = s[1] * a; pre[i * 4 + 2] = s[2] * a; pre[i * 4 + 3] = s[3];
    }
    // Horizontal pass: sw x sh -> w x sh
    const Taps tx = make_taps(sw, w, filter);
    std::vector<float> mid(static_cast<size_t>(w) * sh * 4, 0.0f);
    for (int y = 0; y < sh; ++y) {
        const float* row = &pre[static_cast<size_t>(y) * sw * 4];
        size_t wi = 0;
        for (int x = 0; x < w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < tx.count[x]; ++k, ++wi) {
                const float* s = row + (tx.start[x] + k) * 4;
                const float wt = tx.weights[wi];
                for (int c = 0; c < 4; ++c) acc[c] += s[c] * wt;
            }
            float* d = &mid[(static_cast<size_t>(y) * w + x) * 4];
            for (int c = 0; c < 4; ++c) d[c] = acc[c];
        }
    }
    // Vertical pass: w x sh -> w x h
    const Taps ty = make_taps(sh, h, filter);
    Image out(w, h);
    size_t wi = 0;
    for (int y = 0; y < h; ++y) {
        const size_t wi0 = wi;
        for (int x = 0; x < w; ++x) {
            wi = wi0;
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < ty.count[y]; ++k, ++wi) {
                const float* s = &mid[(static_cast<size_t>(ty.start[y] + k) * w + x) * 4];
                const float wt = ty.weights[wi];
                for (int c = 0; c < 4; ++c) acc[c] += s[c] * wt;
            }
            uint8_t* d = out.data() + (static_cast<size_t>(y) * w + x) * 4;
            const float a = std::clamp(acc[3], 0.0f, 255.0f);
            for (int c = 0; c < 3; ++c) d[c] = a > 0.0f ? static_cast<uint8_t>(std::clamp(acc[c] / (a / 255.0f), 0.0f, 255.0f) + 0.5f) : 0;
            d[3] = static_cast<uint8_t>(a + 0.5f);
        }
    }
    return out;
}

void resample_mask(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
    // Go through an Image so the same filter code applies (gray in RGB, opaque alpha).
    Image tmp(sw, sh);
    for (size_t i = 0; i < static_cast<size_t>(sw) * sh; ++i) {
        uint8_t* p = tmp.data() + i * 4;
        p[0] = p[1] = p[2] = src[i]; p[3] = 255;
    }
    Image r = resample(tmp, dw, dh, Filter::Bilinear);
    for (size_t i = 0; i < static_cast<size_t>(dw) * dh; ++i) dst[i] = r.data()[i * 4];
}

Image shifted(const Image& src, int dx, int dy) {
    return crop(src, {-dx, -dy, src.width() - dx, src.height() - dy});
}

Image crop(const Image& src, Rect r) {
    const int w = r.x1 - r.x0, h = r.y1 - r.y0;
    if (w <= 0 || h <= 0) return Image();
    Image out(w, h, {0, 0, 0, 0});
    const Rect c = r.clipped(src.width(), src.height());
    for (int y = c.y0; y < c.y1; ++y)
        std::memcpy(out.data() + (static_cast<size_t>(y - r.y0) * w + (c.x0 - r.x0)) * 4,
                    src.data() + (static_cast<size_t>(y) * src.width() + c.x0) * 4, static_cast<size_t>(c.x1 - c.x0) * 4);
    return out;
}

Image rotate_quarter(const Image& src, int quarter_turns) {
    const int q = ((quarter_turns % 4) + 4) % 4;
    const int sw = src.width(), sh = src.height();
    if (q == 0) return src;
    if (q == 2) {
        Image out = src;
        flip_vertical(out);
        mirror_horizontal(out);
        return out;
    }
    Image out(sh, sw);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            const int dx = q == 1 ? sh - 1 - y : y;
            const int dy = q == 1 ? x : sw - 1 - x;
            std::memcpy(out.data() + (static_cast<size_t>(dy) * sh + dx) * 4, src.data() + (static_cast<size_t>(y) * sw + x) * 4, 4);
        }
    return out;
}

void rotated_size(int w, int h, float degrees, int* out_w, int* out_h) {
    const float rad = degrees * 3.14159265358979f / 180.0f;
    const float c = std::abs(std::cos(rad)), s = std::abs(std::sin(rad));
    *out_w = std::max(1, static_cast<int>(std::ceil(w * c + h * s - 1e-3f)));
    *out_h = std::max(1, static_cast<int>(std::ceil(w * s + h * c - 1e-3f)));
}

Image rotate(const Image& src, float degrees) {
    const int sw = src.width(), sh = src.height();
    int w, h;
    rotated_size(sw, sh, degrees, &w, &h);
    const float rad = degrees * 3.14159265358979f / 180.0f;
    const float cs = std::cos(rad), sn = std::sin(rad);
    const float cx = sw * 0.5f, cy = sh * 0.5f, ox = w * 0.5f, oy = h * 0.5f;
    Image out(w, h, {0, 0, 0, 0});
    auto sample = [&](float x, float y, float* rgba) {  // premultiplied bilinear
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const float fx = x - x0, fy = y - y0;
        for (int c = 0; c < 4; ++c) rgba[c] = 0.0f;
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const int px = x0 + i, py = y0 + j;
                if (px < 0 || py < 0 || px >= sw || py >= sh) continue;
                const float wt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                const uint8_t* s = src.data() + (static_cast<size_t>(py) * sw + px) * 4;
                const float a = s[3] / 255.0f;
                rgba[0] += s[0] * a * wt; rgba[1] += s[1] * a * wt; rgba[2] += s[2] * a * wt; rgba[3] += s[3] * wt;
            }
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            // Inverse map: rotate the destination point counter-clockwise back to source space.
            const float dx = (x + 0.5f) - ox, dy = (y + 0.5f) - oy;
            const float sx = cx + dx * cs + dy * sn - 0.5f, sy = cy - dx * sn + dy * cs - 0.5f;
            float v[4];
            sample(sx, sy, v);
            uint8_t* d = out.data() + (static_cast<size_t>(y) * w + x) * 4;
            const float a = std::clamp(v[3], 0.0f, 255.0f);
            for (int c = 0; c < 3; ++c) d[c] = a > 0.0f ? static_cast<uint8_t>(std::clamp(v[c] / (a / 255.0f), 0.0f, 255.0f) + 0.5f) : 0;
            d[3] = static_cast<uint8_t>(a + 0.5f);
        }
    return out;
}


// --- Colors and palettes ---------------------------------------------------------

size_t count_colors(const Image& img) {
    std::vector<uint32_t> seen;
    seen.reserve(4096);
    const uint8_t* p = img.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4) {
        if (p[i + 3] == 0) continue;
        seen.push_back(static_cast<uint32_t>(p[i]) << 16 | static_cast<uint32_t>(p[i + 1]) << 8 | p[i + 2]);
    }
    std::sort(seen.begin(), seen.end());
    return static_cast<size_t>(std::unique(seen.begin(), seen.end()) - seen.begin());
}

std::vector<Color> median_cut_palette(const Image& img, int colors) {
    colors = std::clamp(colors, 2, 256);
    // Histogram at 5 bits per channel keeps the boxes manageable.
    std::vector<uint32_t> counts(32 * 32 * 32, 0);
    const uint8_t* p = img.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4)
        if (p[i + 3]) ++counts[(p[i] >> 3) << 10 | (p[i + 1] >> 3) << 5 | (p[i + 2] >> 3)];
    struct Entry { uint8_t r, g, b; uint32_t n; };
    std::vector<Entry> all;
    for (int i = 0; i < 32 * 32 * 32; ++i)
        if (counts[i]) all.push_back({static_cast<uint8_t>(i >> 10), static_cast<uint8_t>((i >> 5) & 31), static_cast<uint8_t>(i & 31), counts[i]});
    if (all.empty()) return {{0, 0, 0, 255}, {255, 255, 255, 255}};
    struct Box { size_t begin, end; };
    std::vector<Box> boxes{{0, all.size()}};
    while (static_cast<int>(boxes.size()) < colors) {
        // Split the box with the largest extent along its longest axis.
        int best = -1, best_axis = 0, best_range = -1;
        for (size_t bi = 0; bi < boxes.size(); ++bi) {
            const Box& b = boxes[bi];
            if (b.end - b.begin < 2) continue;
            int lo[3] = {32, 32, 32}, hi[3] = {-1, -1, -1};
            for (size_t i = b.begin; i < b.end; ++i) {
                const int v[3] = {all[i].r, all[i].g, all[i].b};
                for (int c = 0; c < 3; ++c) { lo[c] = std::min(lo[c], v[c]); hi[c] = std::max(hi[c], v[c]); }
            }
            for (int c = 0; c < 3; ++c) if (hi[c] - lo[c] > best_range) { best_range = hi[c] - lo[c]; best = static_cast<int>(bi); best_axis = c; }
        }
        if (best < 0) break;
        Box b = boxes[static_cast<size_t>(best)];
        std::sort(all.begin() + static_cast<long>(b.begin), all.begin() + static_cast<long>(b.end), [&](const Entry& x, const Entry& y) {
            return best_axis == 0 ? x.r < y.r : best_axis == 1 ? x.g < y.g : x.b < y.b;
        });
        // Median by pixel count.
        uint64_t total = 0, acc = 0;
        for (size_t i = b.begin; i < b.end; ++i) total += all[i].n;
        size_t mid = b.begin + 1;
        for (size_t i = b.begin; i + 1 < b.end; ++i) { acc += all[i].n; if (acc * 2 >= total) { mid = i + 1; break; } }
        boxes[static_cast<size_t>(best)] = {b.begin, mid};
        boxes.push_back({mid, b.end});
    }
    std::vector<Color> out;
    for (const Box& b : boxes) {
        uint64_t r = 0, g = 0, bl = 0, n = 0;
        for (size_t i = b.begin; i < b.end; ++i) { r += (all[i].r * 8 + 4) * static_cast<uint64_t>(all[i].n); g += (all[i].g * 8 + 4) * static_cast<uint64_t>(all[i].n); bl += (all[i].b * 8 + 4) * static_cast<uint64_t>(all[i].n); n += all[i].n; }
        if (n == 0) continue;
        out.push_back({static_cast<uint8_t>(std::min<uint64_t>(255, r / n)), static_cast<uint8_t>(std::min<uint64_t>(255, g / n)), static_cast<uint8_t>(std::min<uint64_t>(255, bl / n)), 255});
    }
    return out;
}

void apply_palette(Image& img, const std::vector<Color>& palette, bool dither) {
    if (palette.empty()) return;
    const int w = img.width(), h = img.height();
    auto nearest = [&](int r, int g, int b) {
        int best = 0, bd = 1 << 30;
        for (size_t i = 0; i < palette.size(); ++i) {
            const int dr = r - palette[i].r, dg = g - palette[i].g, db = b - palette[i].b;
            const int d = dr * dr * 2 + dg * dg * 4 + db * db * 3;
            if (d < bd) { bd = d; best = static_cast<int>(i); }
        }
        return palette[static_cast<size_t>(best)];
    };
    if (!dither) {
        uint8_t* p = img.data();
        for (size_t i = 0; i < img.size_bytes(); i += 4) {
            if (!p[i + 3]) continue;
            const Color c = nearest(p[i], p[i + 1], p[i + 2]);
            p[i] = c.r; p[i + 1] = c.g; p[i + 2] = c.b;
        }
        return;
    }
    // Floyd-Steinberg error diffusion.
    std::vector<float> err(static_cast<size_t>(w + 2) * 2 * 3, 0.0f);
    auto E = [&](int row, int x, int c) -> float& { return err[(static_cast<size_t>(row) * (w + 2) + x + 1) * 3 + c]; };
    for (int y = 0; y < h; ++y) {
        const int cur = y & 1, nxt = cur ^ 1;
        for (int x = -1; x <= w; ++x) for (int c = 0; c < 3; ++c) E(nxt, x, c) = 0.0f;
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img.data() + (static_cast<size_t>(y) * w + x) * 4;
            if (!p[3]) continue;
            int v[3];
            for (int c = 0; c < 3; ++c) v[c] = std::clamp(static_cast<int>(std::lround(p[c] + E(cur, x, c))), 0, 255);
            const Color q = nearest(v[0], v[1], v[2]);
            const int e[3] = {v[0] - q.r, v[1] - q.g, v[2] - q.b};
            p[0] = q.r; p[1] = q.g; p[2] = q.b;
            for (int c = 0; c < 3; ++c) {
                E(cur, x + 1, c) += e[c] * 7.0f / 16.0f;
                E(nxt, x - 1, c) += e[c] * 3.0f / 16.0f;
                E(nxt, x, c) += e[c] * 5.0f / 16.0f;
                E(nxt, x + 1, c) += e[c] * 1.0f / 16.0f;
            }
        }
    }
}

void to_monochrome(Image& img, bool dither) {
    grayscale(img);
    apply_palette(img, {{0, 0, 0, 255}, {255, 255, 255, 255}}, dither);
}

namespace {
Image gray_plane(int w, int h) { return Image(w, h, {0, 0, 0, 255}); }
void set_gray(Image& img, int x, int y, int v) { uint8_t* p = img.data() + (static_cast<size_t>(y) * img.width() + x) * 4; p[0] = p[1] = p[2] = static_cast<uint8_t>(std::clamp(v, 0, 255)); p[3] = 255; }
int get_gray(const Image& img, int x, int y) { return img.data()[(static_cast<size_t>(y) * img.width() + x) * 4]; }
}  // namespace

std::vector<Image> split_channels(const Image& img, int mode) {
    const int w = img.width(), h = img.height();
    std::vector<Image> out;
    const int n = mode == 2 ? 4 : 3;
    for (int i = 0; i < n; ++i) out.push_back(gray_plane(w, h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const Color c = img.get(x, y);
            if (mode == 0) { set_gray(out[0], x, y, c.r); set_gray(out[1], x, y, c.g); set_gray(out[2], x, y, c.b); }
            else if (mode == 1) {
                const adjust::HSL hsl = adjust::rgb_to_hsl(c.r, c.g, c.b);
                set_gray(out[0], x, y, static_cast<int>(hsl.h / 360.0f * 255.0f + 0.5f));
                set_gray(out[1], x, y, static_cast<int>(hsl.s * 255.0f + 0.5f));
                set_gray(out[2], x, y, static_cast<int>(hsl.l * 255.0f + 0.5f));
            } else {
                const int k = 255 - std::max({c.r, c.g, c.b});
                const int den = 255 - k;
                auto ch = [&](int v) { return den > 0 ? (255 - v - k) * 255 / den : 0; };
                set_gray(out[0], x, y, ch(c.r)); set_gray(out[1], x, y, ch(c.g)); set_gray(out[2], x, y, ch(c.b)); set_gray(out[3], x, y, k);
            }
        }
    return out;
}

Image combine_channels(const std::vector<Image>& planes, int mode) {
    const int n = mode == 2 ? 4 : 3;
    if (static_cast<int>(planes.size()) < n) return Image();
    const int w = planes[0].width(), h = planes[0].height();
    for (int i = 1; i < n; ++i) if (planes[static_cast<size_t>(i)].width() != w || planes[static_cast<size_t>(i)].height() != h) return Image();
    Image out(w, h, {0, 0, 0, 255});
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int a = get_gray(planes[0], x, y), b = get_gray(planes[1], x, y), c = get_gray(planes[2], x, y);
            Color col{0, 0, 0, 255};
            if (mode == 0) col = {static_cast<uint8_t>(a), static_cast<uint8_t>(b), static_cast<uint8_t>(c), 255};
            else if (mode == 1) adjust::hsl_to_rgb({a / 255.0f * 360.0f, b / 255.0f, c / 255.0f}, &col.r, &col.g, &col.b);
            else {
                const int k = get_gray(planes[3], x, y);
                col = {static_cast<uint8_t>((255 - a) * (255 - k) / 255), static_cast<uint8_t>((255 - b) * (255 - k) / 255), static_cast<uint8_t>((255 - c) * (255 - k) / 255), 255};
            }
            out.set(x, y, col);
        }
    return out;
}

Image arithmetic(const Image& a, const Image& b, ArithOp op, float divisor, int bias, bool clip, int channel) {
    const int w = std::max(a.width(), b.width()), h = std::max(a.height(), b.height());
    Image out(w, h, {0, 0, 0, 255});
    if (divisor == 0.0f) divisor = 1.0f;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const Color ca = x < a.width() && y < a.height() ? a.get(x, y) : Color{0, 0, 0, 255};
            const Color cb = x < b.width() && y < b.height() ? b.get(x, y) : Color{0, 0, 0, 255};
            const int va[3] = {ca.r, ca.g, ca.b}, vb[3] = {cb.r, cb.g, cb.b};
            uint8_t res[3];
            for (int c = 0; c < 3; ++c) {
                if (channel != 0 && channel - 1 != c) { res[c] = static_cast<uint8_t>(va[c]); continue; }
                int v;
                switch (op) {
                    case ArithOp::Add: v = va[c] + vb[c]; break;
                    case ArithOp::Subtract: v = va[c] - vb[c]; break;
                    case ArithOp::Multiply: v = va[c] * vb[c] / 255; break;
                    case ArithOp::Difference: v = std::abs(va[c] - vb[c]); break;
                    case ArithOp::Lightest: v = std::max(va[c], vb[c]); break;
                    case ArithOp::Darkest: v = std::min(va[c], vb[c]); break;
                    case ArithOp::Average: v = (va[c] + vb[c]) / 2; break;
                    case ArithOp::And: v = va[c] & vb[c]; break;
                    case ArithOp::Or: v = va[c] | vb[c]; break;
                    default: v = va[c] ^ vb[c]; break;
                }
                v = static_cast<int>(v / divisor) + bias;
                res[c] = static_cast<uint8_t>(clip ? std::clamp(v, 0, 255) : ((v % 256) + 256) % 256);
            }
            out.set(x, y, {res[0], res[1], res[2], 255});
        }
    return out;
}

// --- Projective warps -----------------------------------------------------

// Direct linear transform for four point pairs: solves the 8 unknowns of H
// (h33 = 1) by Gaussian elimination.
bool homography(const Quad& from, const Quad& to, float H[9]) {
    double A[8][9];
    for (int i = 0; i < 4; ++i) {
        const double x = from.x[i], y = from.y[i], u = to.x[i], v = to.y[i];
        double* r0 = A[i * 2];
        double* r1 = A[i * 2 + 1];
        r0[0] = x; r0[1] = y; r0[2] = 1; r0[3] = 0; r0[4] = 0; r0[5] = 0; r0[6] = -u * x; r0[7] = -u * y; r0[8] = u;
        r1[0] = 0; r1[1] = 0; r1[2] = 0; r1[3] = x; r1[4] = y; r1[5] = 1; r1[6] = -v * x; r1[7] = -v * y; r1[8] = v;
    }
    for (int col = 0; col < 8; ++col) {
        int piv = col;
        for (int r = col + 1; r < 8; ++r) if (std::abs(A[r][col]) > std::abs(A[piv][col])) piv = r;
        if (std::abs(A[piv][col]) < 1e-9) return false;
        if (piv != col) for (int k = 0; k < 9; ++k) std::swap(A[piv][k], A[col][k]);
        for (int r = 0; r < 8; ++r) {
            if (r == col) continue;
            const double f = A[r][col] / A[col][col];
            for (int k = col; k < 9; ++k) A[r][k] -= f * A[col][k];
        }
    }
    for (int i = 0; i < 8; ++i) H[i] = static_cast<float>(A[i][8] / A[i][i]);
    H[8] = 1.0f;
    return true;
}

bool invert3(const float H[9], float out[9]) {
    const double a = H[0], b = H[1], c = H[2], d = H[3], e = H[4], f = H[5], g = H[6], h = H[7], i = H[8];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::abs(det) < 1e-12) return false;
    const double inv = 1.0 / det;
    out[0] = static_cast<float>((e * i - f * h) * inv); out[1] = static_cast<float>((c * h - b * i) * inv); out[2] = static_cast<float>((b * f - c * e) * inv);
    out[3] = static_cast<float>((f * g - d * i) * inv); out[4] = static_cast<float>((a * i - c * g) * inv); out[5] = static_cast<float>((c * d - a * f) * inv);
    out[6] = static_cast<float>((d * h - e * g) * inv); out[7] = static_cast<float>((b * g - a * h) * inv); out[8] = static_cast<float>((a * e - b * d) * inv);
    return true;
}

void apply_homography(const float H[9], float x, float y, float* ox, float* oy) {
    const float w = H[6] * x + H[7] * y + H[8];
    const float iw = std::abs(w) > 1e-12f ? 1.0f / w : 0.0f;
    *ox = (H[0] * x + H[1] * y + H[2]) * iw;
    *oy = (H[3] * x + H[4] * y + H[5]) * iw;
}

Image warp(const Image& src, const float H[9], int w, int h) {
    Image out(w, h, {0, 0, 0, 0});
    float inv[9];
    if (!invert3(H, inv)) return out;
    const int sw = src.width(), sh = src.height();
    auto sample = [&](float x, float y, float* rgba) {  // premultiplied bilinear
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const float fx = x - x0, fy = y - y0;
        for (int c = 0; c < 4; ++c) rgba[c] = 0.0f;
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const int px = x0 + i, py = y0 + j;
                if (px < 0 || py < 0 || px >= sw || py >= sh) continue;
                const float wt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                const uint8_t* s = src.data() + (static_cast<size_t>(py) * sw + px) * 4;
                const float a = s[3] / 255.0f;
                rgba[0] += s[0] * a * wt; rgba[1] += s[1] * a * wt; rgba[2] += s[2] * a * wt; rgba[3] += s[3] * wt;
            }
    };
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const int bands = static_cast<int>(std::min<size_t>(hw, static_cast<size_t>(std::max(1, w * h / 65536))));
    auto rows = [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                float sx, sy;
                apply_homography(inv, x + 0.5f, y + 0.5f, &sx, &sy);
                sx -= 0.5f; sy -= 0.5f;
                if (sx < -1 || sy < -1 || sx > sw || sy > sh) continue;
                float v[4];
                sample(sx, sy, v);
                uint8_t* d = out.data() + (static_cast<size_t>(y) * w + x) * 4;
                const float a = std::clamp(v[3], 0.0f, 255.0f);
                for (int c = 0; c < 3; ++c) d[c] = a > 0.0f ? static_cast<uint8_t>(std::clamp(v[c] / (a / 255.0f), 0.0f, 255.0f) + 0.5f) : 0;
                d[3] = static_cast<uint8_t>(a + 0.5f);
            }
    };
    if (bands <= 1) { rows(0, h); return out; }
    std::vector<std::thread> pool;
    for (int b = 0; b < bands; ++b) pool.emplace_back(rows, h * b / bands, h * (b + 1) / bands);
    for (auto& t : pool) t.join();
    return out;
}

Rect content_bounds(const Image& img) {
    const int w = img.width(), h = img.height();
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y) {
        const uint8_t* p = img.data() + static_cast<size_t>(y) * w * 4 + 3;
        for (int x = 0; x < w; ++x, p += 4)
            if (*p) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    }
    if (x1 < x0) return {0, 0, w, h};
    return {x0, y0, x1 + 1, y1 + 1};
}


// --- Mesh and displacement warps -------------------------------------------------

namespace {
struct Sampler {
    const Image& src;
    int w, h;
    explicit Sampler(const Image& s) : src(s), w(s.width()), h(s.height()) {}
    void at(float x, float y, uint8_t* d) const {  // premultiplied bilinear, straight result
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const float fx = x - x0, fy = y - y0;
        float v[4] = {0, 0, 0, 0};
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const int px = std::clamp(x0 + i, 0, w - 1), py = std::clamp(y0 + j, 0, h - 1);
                const float wt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                const uint8_t* s = src.data() + (static_cast<size_t>(py) * w + px) * 4;
                const float a = s[3] / 255.0f;
                v[0] += s[0] * a * wt; v[1] += s[1] * a * wt; v[2] += s[2] * a * wt; v[3] += s[3] * wt;
            }
        const float a = std::clamp(v[3], 0.0f, 255.0f);
        for (int c = 0; c < 3; ++c) d[c] = a > 0.0f ? static_cast<uint8_t>(std::clamp(v[c] / (a / 255.0f), 0.0f, 255.0f) + 0.5f) : 0;
        d[3] = static_cast<uint8_t>(a + 0.5f);
    }
};
}  // namespace

Image mesh_warp(const Image& src, int cols, int rows, const std::vector<std::pair<float, float>>& nodes) {
    const int w = src.width(), h = src.height();
    Image out(w, h, {0, 0, 0, 0});
    if (cols < 1 || rows < 1 || nodes.size() != static_cast<size_t>((cols + 1) * (rows + 1))) return src;
    const Sampler sampler(src);
    auto node = [&](int c, int r) { return nodes[static_cast<size_t>(r * (cols + 1) + c)]; };
    auto src_node = [&](int c, int r) { return std::pair<float, float>{static_cast<float>(w) * c / cols, static_cast<float>(h) * r / rows}; };
    // Each cell splits into two triangles; every destination pixel inside a
    // destination triangle takes its barycentric position in the source one.
    auto tri = [&](std::pair<float, float> d0, std::pair<float, float> d1, std::pair<float, float> d2,
                   std::pair<float, float> s0, std::pair<float, float> s1, std::pair<float, float> s2) {
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({d0.first, d1.first, d2.first}))));
        const int x1 = std::min(w - 1, static_cast<int>(std::ceil(std::max({d0.first, d1.first, d2.first}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({d0.second, d1.second, d2.second}))));
        const int y1 = std::min(h - 1, static_cast<int>(std::ceil(std::max({d0.second, d1.second, d2.second}))));
        const float det = (d1.first - d0.first) * (d2.second - d0.second) - (d2.first - d0.first) * (d1.second - d0.second);
        if (std::abs(det) < 1e-6f) return;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float px = x + 0.5f, py = y + 0.5f;
                const float l1 = ((px - d0.first) * (d2.second - d0.second) - (d2.first - d0.first) * (py - d0.second)) / det;
                const float l2 = ((d1.first - d0.first) * (py - d0.second) - (px - d0.first) * (d1.second - d0.second)) / det;
                const float l0 = 1.0f - l1 - l2;
                const float eps = -0.002f;
                if (l0 < eps || l1 < eps || l2 < eps) continue;
                const float sx = l0 * s0.first + l1 * s1.first + l2 * s2.first - 0.5f;
                const float sy = l0 * s0.second + l1 * s1.second + l2 * s2.second - 0.5f;
                sampler.at(sx, sy, out.data() + (static_cast<size_t>(y) * w + x) * 4);
            }
    };
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            tri(node(c, r), node(c + 1, r), node(c, r + 1), src_node(c, r), src_node(c + 1, r), src_node(c, r + 1));
            tri(node(c + 1, r), node(c + 1, r + 1), node(c, r + 1), src_node(c + 1, r), src_node(c + 1, r + 1), src_node(c, r + 1));
        }
    return out;
}

Image displace(const Image& src, const std::vector<float>& dx, const std::vector<float>& dy) {
    const int w = src.width(), h = src.height();
    Image out(w, h, {0, 0, 0, 0});
    if (dx.size() != static_cast<size_t>(w) * h || dy.size() != dx.size()) return src;
    const Sampler sampler(src);
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const int bands = static_cast<int>(std::min<size_t>(hw, static_cast<size_t>(std::max(1, w * h / 65536))));
    auto rows = [&](int ya, int yb) {
        for (int y = ya; y < yb; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                if (dx[i] == 0.0f && dy[i] == 0.0f) { std::memcpy(out.data() + i * 4, src.data() + i * 4, 4); continue; }
                const float sx = x + dx[i], sy = y + dy[i];
                if (sx < -1 || sy < -1 || sx > w || sy > h) continue;
                sampler.at(sx, sy, out.data() + i * 4);
            }
    };
    if (bands <= 1) { rows(0, h); return out; }
    std::vector<std::thread> pool;
    for (int b = 0; b < bands; ++b) pool.emplace_back(rows, h * b / bands, h * (b + 1) / bands);
    for (auto& t : pool) t.join();
    return out;
}

void scratch_fill(Image& img, float x0, float y0, float x1, float y1, float width) {
    const int w = img.width(), h = img.height();
    float ex = x1 - x0, ey = y1 - y0;
    const float len = std::hypot(ex, ey);
    if (len < 1.0f) return;
    ex /= len; ey /= len;
    const float nx = -ey, ny = ex;
    const float half = std::max(1.0f, width * 0.5f);
    const Image src = img;
    const Sampler sampler(src);
    const int bx0 = std::max(0, static_cast<int>(std::floor(std::min(x0, x1) - half - 1))), bx1 = std::min(w - 1, static_cast<int>(std::ceil(std::max(x0, x1) + half + 1)));
    const int by0 = std::max(0, static_cast<int>(std::floor(std::min(y0, y1) - half - 1))), by1 = std::min(h - 1, static_cast<int>(std::ceil(std::max(y0, y1) + half + 1)));
    for (int y = by0; y <= by1; ++y)
        for (int x = bx0; x <= bx1; ++x) {
            const float px = x + 0.5f - x0, py = y + 0.5f - y0;
            const float along = px * ex + py * ey, across = px * nx + py * ny;
            if (along < 0 || along > len || std::abs(across) > half) continue;
            // Blend the two colors just outside the strip by position across it.
            uint8_t a[4], b[4];
            const float ax = x0 + ex * along + nx * (half + 1.5f), ay = y0 + ey * along + ny * (half + 1.5f);
            const float bx = x0 + ex * along - nx * (half + 1.5f), by = y0 + ey * along - ny * (half + 1.5f);
            sampler.at(ax - 0.5f, ay - 0.5f, a);
            sampler.at(bx - 0.5f, by - 0.5f, b);
            const float t = (across + half) / (2.0f * half);
            uint8_t* d = img.data() + (static_cast<size_t>(y) * w + x) * 4;
            if (!d[3]) continue;
            for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>(b[c] + (a[c] - b[c]) * t + 0.5f);
        }
}

}  // namespace firn::raster
