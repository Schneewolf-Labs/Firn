#include "firn/raster.h"

#include "firn/mask.h"

#include <algorithm>
#include <cmath>
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

void Stroke::stamp(float cx, float cy) {
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
            const float d = std::sqrt(dx * dx + dy * dy);
            float cov;
            if (d <= inner) cov = 1.0f;
            else if (brush_.hardness >= 1.0f || r - inner < 1.0f) cov = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
            else cov = std::clamp((r - d) / (r - inner), 0.0f, 1.0f);
            if (cov <= 0.0f) continue;
            float& m = mask_[static_cast<size_t>(y) * w + x];
            m = std::max(m, cov);
        }
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

void greyscale(Image& img) {
    uint8_t* p = img.data();
    const size_t n = img.size_bytes();
    for (size_t i = 0; i < n; i += 4) {
        // Rec.601 luma, matching the original's Greyscale command.
        const int y = (p[i] * 299 + p[i + 1] * 587 + p[i + 2] * 114 + 500) / 1000;
        p[i] = p[i + 1] = p[i + 2] = static_cast<uint8_t>(y);
    }
}

void brightness_contrast(Image& img, int brightness, int contrast) {
    // Contrast scales about mid-grey; brightness is a plain offset.
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

    // Blur premultiplied so transparent pixels don't bleed their colour in.
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
