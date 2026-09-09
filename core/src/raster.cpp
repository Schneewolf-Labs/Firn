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
            m = brush_.accumulate ? std::min(1.0f, m + cov * brush_.flow) : std::max(m, cov);
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
            } else if (mode_ == StrokeMode::Clone) {
                std::memcpy(d, b, 4);
                const int sx = x + clone_ox_, sy = y + clone_oy_;
                if (clone_ && sx >= 0 && sy >= 0 && sx < clone_->width() && sy < clone_->height())
                    blend_over(dst, x, y, clone_->get(sx, sy), m);
            } else if (mode_ == StrokeMode::Filter) {
                const Color before{b[0], b[1], b[2], b[3]};
                const Color after = filter_ ? filter_(before) : before;
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

// Separable box blur on all four channels. Straight-alpha blur is not
// colour-correct at transparent edges; premultiplied comes later.
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
    std::vector<float> weights;   // count taps per output, normalised
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
        const float centre = (i + 0.5f) * scale;
        int lo = static_cast<int>(std::floor(centre - support)), hi = static_cast<int>(std::ceil(centre + support));
        lo = std::max(lo, 0); hi = std::min(hi, src_n - 1);
        if (hi < lo) { lo = hi = std::clamp(static_cast<int>(centre), 0, src_n - 1); }
        float sum = 0.0f;
        w.clear();
        for (int j = lo; j <= hi; ++j) {
            const float x = ((j + 0.5f) - centre) / blur;
            float k;
            if (filter == Filter::Bicubic) k = catmull_rom(x);
            else if (filter == Filter::Bilinear) k = triangle(x);
            else k = std::abs(x) < 0.5f ? 1.0f : 0.0f;
            w.push_back(k);
            sum += k;
        }
        if (sum == 0.0f) {  // nearest: pick the closest tap
            int best = std::clamp(static_cast<int>(centre), lo, hi);
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
    // Go through an Image so the same filter code applies (grey in RGB, opaque alpha).
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

}  // namespace firn::raster
