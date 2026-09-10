#include "firn/layerstyle.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace firn {

namespace {

bool same(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

json::Value color_json(Color c) {
    json::Value v = json::Value::array();
    v.push(json::Value::number(c.r)); v.push(json::Value::number(c.g)); v.push(json::Value::number(c.b)); v.push(json::Value::number(c.a));
    return v;
}
Color color_from(const json::Value& v, Color def) {
    if (v.size() < 3) return def;
    return {static_cast<uint8_t>(v[0].as_number()), static_cast<uint8_t>(v[1].as_number()), static_cast<uint8_t>(v[2].as_number()), static_cast<uint8_t>(v.size() > 3 ? v[3].as_number(255) : 255)};
}

// Alpha (0..1) over a padded rect, blurred with a Gaussian of `radius`
// through the premultiplied image blur (white pixels with this alpha).
std::vector<float> blurred(const std::vector<float>& a, int w, int h, float radius) {
    if (radius <= 0.05f) return a;
    Image tmp(w, h, {255, 255, 255, 0});
    for (size_t i = 0; i < a.size(); ++i) tmp.data()[i * 4 + 3] = static_cast<uint8_t>(std::clamp(a[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    raster::gaussian_blur(tmp, radius);
    std::vector<float> out(a.size());
    for (size_t i = 0; i < a.size(); ++i) out[i] = tmp.data()[i * 4 + 3] / 255.0f;
    return out;
}

// Grows the shape by `radius` pixels (a separable max filter).
std::vector<float> dilated(const std::vector<float>& a, int w, int h, int radius) {
    if (radius <= 0) return a;
    std::vector<float> tmp(a.size()), out(a.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float m = 0.0f;
            for (int k = std::max(0, x - radius); k <= std::min(w - 1, x + radius); ++k) m = std::max(m, a[static_cast<size_t>(y) * w + k]);
            tmp[static_cast<size_t>(y) * w + x] = m;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float m = 0.0f;
            for (int k = std::max(0, y - radius); k <= std::min(h - 1, y + radius); ++k) m = std::max(m, tmp[static_cast<size_t>(k) * w + x]);
            out[static_cast<size_t>(y) * w + x] = m;
        }
    return out;
}

inline void over(float* d, Color c, float cov) {   // straight-alpha "over" on a float RGBA pixel (0..1)
    const float sa = (c.a / 255.0f) * std::clamp(cov, 0.0f, 1.0f);
    if (sa <= 0.0f) return;
    const float da = d[3], oa = sa + da * (1.0f - sa);
    const float src[3] = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f};
    for (int i = 0; i < 3; ++i) d[i] = (src[i] * sa + d[i] * da * (1.0f - sa)) / oa;
    d[3] = oa;
}

}  // namespace

int LayerStyle::reach() const {
    int r = 0;
    if (drop_shadow) r = std::max(r, static_cast<int>(std::ceil(shadow_blur * 3.0f + std::max(std::abs(shadow_offset_x), std::abs(shadow_offset_y)))) + 1);
    if (outer_glow) r = std::max(r, static_cast<int>(std::ceil(glow_size * 3.0f)) + 1);
    if (inner_glow) r = std::max(r, static_cast<int>(std::ceil(inner_glow_size * 3.0f)) + 1);
    if (stroke) r = std::max(r, stroke_width + 1);
    if (bevel) r = std::max(r, static_cast<int>(std::ceil(bevel_size * 3.0f)) + 1);
    return r;
}

bool LayerStyle::operator==(const LayerStyle& o) const {
    return drop_shadow == o.drop_shadow && same(shadow_color, o.shadow_color) && shadow_opacity == o.shadow_opacity && shadow_offset_x == o.shadow_offset_x && shadow_offset_y == o.shadow_offset_y && shadow_blur == o.shadow_blur &&
           outer_glow == o.outer_glow && same(glow_color, o.glow_color) && glow_size == o.glow_size && glow_opacity == o.glow_opacity &&
           inner_glow == o.inner_glow && same(inner_glow_color, o.inner_glow_color) && inner_glow_size == o.inner_glow_size && inner_glow_opacity == o.inner_glow_opacity &&
           stroke == o.stroke && same(stroke_color, o.stroke_color) && stroke_width == o.stroke_width && stroke_opacity == o.stroke_opacity &&
           bevel == o.bevel && bevel_size == o.bevel_size && bevel_depth == o.bevel_depth && bevel_angle == o.bevel_angle;
}

json::Value LayerStyle::to_json() const {
    json::Value v = json::Value::object();
    v.set("drop_shadow", json::Value::boolean(drop_shadow)); v.set("shadow_color", color_json(shadow_color));
    v.set("shadow_opacity", json::Value::number(shadow_opacity)); v.set("shadow_offset_x", json::Value::number(shadow_offset_x)); v.set("shadow_offset_y", json::Value::number(shadow_offset_y)); v.set("shadow_blur", json::Value::number(shadow_blur));
    v.set("outer_glow", json::Value::boolean(outer_glow)); v.set("glow_color", color_json(glow_color)); v.set("glow_size", json::Value::number(glow_size)); v.set("glow_opacity", json::Value::number(glow_opacity));
    v.set("inner_glow", json::Value::boolean(inner_glow)); v.set("inner_glow_color", color_json(inner_glow_color)); v.set("inner_glow_size", json::Value::number(inner_glow_size)); v.set("inner_glow_opacity", json::Value::number(inner_glow_opacity));
    v.set("stroke", json::Value::boolean(stroke)); v.set("stroke_color", color_json(stroke_color)); v.set("stroke_width", json::Value::number(stroke_width)); v.set("stroke_opacity", json::Value::number(stroke_opacity));
    v.set("bevel", json::Value::boolean(bevel)); v.set("bevel_size", json::Value::number(bevel_size)); v.set("bevel_depth", json::Value::number(bevel_depth)); v.set("bevel_angle", json::Value::number(bevel_angle));
    return v;
}

LayerStyle LayerStyle::from_json(const json::Value& v) {
    LayerStyle s;
    auto num = [&](const char* k, float def) { return static_cast<float>(v.get(k).as_number(def)); };
    s.drop_shadow = v.get("drop_shadow").as_bool(); s.shadow_color = color_from(v.get("shadow_color"), s.shadow_color);
    s.shadow_opacity = num("shadow_opacity", s.shadow_opacity); s.shadow_offset_x = num("shadow_offset_x", s.shadow_offset_x); s.shadow_offset_y = num("shadow_offset_y", s.shadow_offset_y); s.shadow_blur = num("shadow_blur", s.shadow_blur);
    s.outer_glow = v.get("outer_glow").as_bool(); s.glow_color = color_from(v.get("glow_color"), s.glow_color); s.glow_size = num("glow_size", s.glow_size); s.glow_opacity = num("glow_opacity", s.glow_opacity);
    s.inner_glow = v.get("inner_glow").as_bool(); s.inner_glow_color = color_from(v.get("inner_glow_color"), s.inner_glow_color); s.inner_glow_size = num("inner_glow_size", s.inner_glow_size); s.inner_glow_opacity = num("inner_glow_opacity", s.inner_glow_opacity);
    s.stroke = v.get("stroke").as_bool(); s.stroke_color = color_from(v.get("stroke_color"), s.stroke_color); s.stroke_width = static_cast<int>(num("stroke_width", static_cast<float>(s.stroke_width))); s.stroke_opacity = num("stroke_opacity", s.stroke_opacity);
    s.bevel = v.get("bevel").as_bool(); s.bevel_size = num("bevel_size", s.bevel_size); s.bevel_depth = num("bevel_depth", s.bevel_depth); s.bevel_angle = num("bevel_angle", s.bevel_angle);
    return s;
}

Image render_layer_style(const Image& src, const LayerStyle& st, const raster::Rect& rect) {
    const raster::Rect r = rect.clipped(src.width(), src.height());
    Image out(std::max(r.x1 - r.x0, 0), std::max(r.y1 - r.y0, 0), {0, 0, 0, 0});
    if (r.empty()) return out;
    // Work over the rect padded by the reach, clipped to the image.
    const int reach = st.reach();
    const raster::Rect rp = raster::Rect{r.x0 - reach, r.y0 - reach, r.x1 + reach, r.y1 + reach}.clipped(src.width(), src.height());
    const int pw = rp.x1 - rp.x0, ph = rp.y1 - rp.y0;
    std::vector<float> A(static_cast<size_t>(pw) * ph);
    for (int y = 0; y < ph; ++y)
        for (int x = 0; x < pw; ++x) A[static_cast<size_t>(y) * pw + x] = src.get(rp.x0 + x, rp.y0 + y).a / 255.0f;

    std::vector<float> shadow, glow, inner, stroked, bevel_blur;
    if (st.drop_shadow) {
        // The shape shifted by the offset, then blurred.
        std::vector<float> shifted(A.size(), 0.0f);
        const int dx = static_cast<int>(std::lround(st.shadow_offset_x)), dy = static_cast<int>(std::lround(st.shadow_offset_y));
        for (int y = 0; y < ph; ++y)
            for (int x = 0; x < pw; ++x) {
                const int sx = x - dx, sy = y - dy;
                if (sx >= 0 && sy >= 0 && sx < pw && sy < ph) shifted[static_cast<size_t>(y) * pw + x] = A[static_cast<size_t>(sy) * pw + sx];
            }
        shadow = blurred(shifted, pw, ph, st.shadow_blur);
    }
    if (st.outer_glow) glow = blurred(A, pw, ph, st.glow_size);
    if (st.inner_glow) {
        std::vector<float> inv(A.size());
        for (size_t i = 0; i < A.size(); ++i) inv[i] = 1.0f - A[i];
        inner = blurred(inv, pw, ph, st.inner_glow_size);
    }
    if (st.stroke) stroked = dilated(A, pw, ph, st.stroke_width);
    if (st.bevel) bevel_blur = blurred(A, pw, ph, st.bevel_size);
    const float lx = std::cos(st.bevel_angle * 3.14159265f / 180.0f), ly = -std::sin(st.bevel_angle * 3.14159265f / 180.0f);

    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) {
            const int px = x - rp.x0, py = y - rp.y0;
            const size_t i = static_cast<size_t>(py) * pw + px;
            const float a = A[i];
            float d[4] = {0, 0, 0, 0};
            if (st.drop_shadow) over(d, st.shadow_color, shadow[i] * st.shadow_opacity);
            if (st.outer_glow) over(d, st.glow_color, glow[i] * (1.0f - a) * st.glow_opacity * 1.5f);
            if (st.stroke) over(d, st.stroke_color, std::max(stroked[i] - a, 0.0f) * st.stroke_opacity);
            over(d, src.get(x, y), 1.0f);
            if (st.inner_glow) over(d, st.inner_glow_color, inner[i] * a * st.inner_glow_opacity * 1.5f);
            if (st.bevel && a > 0.0f) {
                // Lit slope of the blurred shape: highlight facing the light, shadow away from it.
                const float gx = (bevel_blur[std::min<size_t>(i + 1, A.size() - 1)] - bevel_blur[i > 0 ? i - 1 : 0]) * 0.5f;
                const float gy = (bevel_blur[std::min<size_t>(i + pw, A.size() - 1)] - bevel_blur[i >= static_cast<size_t>(pw) ? i - pw : 0]) * 0.5f;
                const float lit = (gx * lx + gy * ly) * st.bevel_size * 2.0f * st.bevel_depth;
                if (lit > 0.0f) over(d, {255, 255, 255, 255}, std::min(lit, 1.0f) * a * 0.9f);
                else if (lit < 0.0f) over(d, {0, 0, 0, 255}, std::min(-lit, 1.0f) * a * 0.9f);
            }
            uint8_t* o = out.data() + (static_cast<size_t>(y - r.y0) * out.width() + (x - r.x0)) * 4;
            for (int c = 0; c < 4; ++c) o[c] = static_cast<uint8_t>(std::clamp(d[c], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    return out;
}

}  // namespace firn
