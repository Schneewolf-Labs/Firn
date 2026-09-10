#include "firn/vector.h"
#include "firn/text.h"

#include <algorithm>
#include <cmath>

#include "firn/mask.h"
#include "firn/raster.h"

namespace firn::vec {

namespace {
uint8_t c8(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f); }
}  // namespace

Color Gradient::at(float t) const {
    t = std::clamp(t, 0.0f, 1.0f) * 100.0f;
    Color out{0, 0, 0, 255};
    if (colors.empty()) return out;
    if (colors.size() == 1 || t <= colors.front().pos) out = colors.front().color;
    else if (t >= colors.back().pos) out = colors.back().color;
    else {
        for (size_t i = 0; i + 1 < colors.size(); ++i) {
            const GradientStop& a = colors[i];
            const GradientStop& b = colors[i + 1];
            if (t < a.pos || t > b.pos) continue;
            float f = (b.pos > a.pos) ? (t - a.pos) / (b.pos - a.pos) : 0.0f;
            // Midpoint skews the blend like the original: mid% of the span is where the 50% mix sits.
            const float m = std::clamp(a.mid, 1.0f, 99.0f) / 100.0f;
            f = f <= m ? 0.5f * f / m : 0.5f + 0.5f * (f - m) / (1.0f - m);
            out = {c8(a.color.r + (b.color.r - a.color.r) * f), c8(a.color.g + (b.color.g - a.color.g) * f),
                   c8(a.color.b + (b.color.b - a.color.b) * f), 255};
            break;
        }
    }
    float alpha = 100.0f;
    if (!opacities.empty()) {
        if (opacities.size() == 1 || t <= opacities.front().pos) alpha = opacities.front().opacity;
        else if (t >= opacities.back().pos) alpha = opacities.back().opacity;
        else
            for (size_t i = 0; i + 1 < opacities.size(); ++i) {
                const OpacityStop& a = opacities[i];
                const OpacityStop& b = opacities[i + 1];
                if (t < a.pos || t > b.pos) continue;
                const float f = (b.pos > a.pos) ? (t - a.pos) / (b.pos - a.pos) : 0.0f;
                alpha = a.opacity + (b.opacity - a.opacity) * f;
                break;
            }
    }
    out.a = c8(alpha * 2.55f);
    return out;
}

void Object::bounds(float* x0, float* y0, float* x1, float* y1) const {
    *x0 = *y0 = 1e9f; *x1 = *y1 = -1e9f;
    for (const Path& p : paths)
        for (const Node& n : p.nodes) {
            for (float x : {n.x, n.in_x, n.out_x}) { *x0 = std::min(*x0, x); *x1 = std::max(*x1, x); }
            for (float y : {n.y, n.in_y, n.out_y}) { *y0 = std::min(*y0, y); *y1 = std::max(*y1, y); }
        }
    if (*x1 < *x0) { *x0 = *y0 = *x1 = *y1 = 0; }
}

void Object::translate(float dx, float dy) { transform(1, 0, 0, 1, dx, dy); }

void Object::transform(float a, float b, float c, float d, float tx, float ty) {
    auto ap = [&](float& x, float& y) { const float nx = a * x + b * y + tx, ny = c * x + d * y + ty; x = nx; y = ny; };
    for (Path& p : paths)
        for (Node& n : p.nodes) { ap(n.x, n.y); ap(n.in_x, n.in_y); ap(n.out_x, n.out_y); }
    if (is_text) {
        // The insert point (baseline start) follows the outlines, and a
        // rotation part becomes the text's rotation, so "lay out at (x, y)
        // and rotate about the insert point" reproduces the paths.
        float ix = text.x, iy = text.y + text.baseline;
        ap(ix, iy);
        text.x = ix; text.y = iy - text.baseline;
        const float rot = std::atan2(c, a) * 180.0f / 3.14159265f;
        if (std::abs(rot) > 1e-4f) text.rotation += rot;
    }
}

std::vector<std::pair<float, float>> flatten(const Path& p, float tolerance) {
    std::vector<std::pair<float, float>> out;
    const size_t n = p.nodes.size();
    if (n == 0) return out;
    const size_t segs = p.closed ? n : n - 1;
    out.emplace_back(p.nodes[0].x, p.nodes[0].y);
    for (size_t i = 0; i < segs; ++i) {
        const Node& a = p.nodes[i];
        const Node& b = p.nodes[(i + 1) % n];
        if (a.out_x == a.x && a.out_y == a.y && b.in_x == b.x && b.in_y == b.y) {
            out.emplace_back(b.x, b.y);
            continue;
        }
        // Subdivide the cubic by an estimate of its length over the tolerance.
        const float len = std::hypot(a.out_x - a.x, a.out_y - a.y) + std::hypot(b.in_x - a.out_x, b.in_y - a.out_y) + std::hypot(b.x - b.in_x, b.y - b.in_y);
        const int steps = std::clamp(static_cast<int>(std::sqrt(len / tolerance)) + 1, 2, 200);
        for (int s = 1; s <= steps; ++s) {
            const float t = static_cast<float>(s) / steps, u = 1 - t;
            const float x = u * u * u * a.x + 3 * u * u * t * a.out_x + 3 * u * t * t * b.in_x + t * t * t * b.x;
            const float y = u * u * u * a.y + 3 * u * u * t * a.out_y + 3 * u * t * t * b.in_y + t * t * t * b.y;
            out.emplace_back(x, y);
        }
    }
    return out;
}

// Gradient parameter for a pixel relative to the object's bounds.
static float gradient_t(const Gradient& g, float px, float py, float bx0, float by0, float bx1, float by1) {
    const float w = std::max(bx1 - bx0, 1.0f), h = std::max(by1 - by0, 1.0f);
    const float u = (px - bx0) / w, v = (py - by0) / h;
    const float cx = g.center_x / 100.0f, cy = g.center_y / 100.0f;
    float t;
    switch (g.style) {
        case GradientStyle::Radial:
            t = std::hypot((u - cx) * w, (v - cy) * h) / (0.5f * std::hypot(w, h));
            break;
        case GradientStyle::Rectangular: {
            const float rad = g.angle * 3.14159265f / 180.0f;
            const float dx = (u - cx) * w, dy = (v - cy) * h;
            const float rx = dx * std::cos(rad) + dy * std::sin(rad), ry = -dx * std::sin(rad) + dy * std::cos(rad);
            t = std::max(std::abs(rx) / (0.5f * w), std::abs(ry) / (0.5f * h));
            break;
        }
        case GradientStyle::Sunburst:
            t = std::hypot((u - cx) * w, (v - cy) * h) / std::max(std::hypot(w, h) * 0.5f, 1.0f);
            break;
        default: {
            // Linear, verified against the original's stored composites: the
            // pixel offset from the bounds' center is projected onto the
            // gradient axis and divided by the projected extent of the box,
            // so the gradient always spans the whole object.
            const float rad = g.angle * 3.14159265f / 180.0f;
            const float sn = std::sin(rad), cs = std::cos(rad);
            const float dx = (u - 0.5f) * w, dy = (v - 0.5f) * h;
            const float extent = std::max(std::abs(w * sn) + std::abs(h * cs), 1.0f);
            t = (dx * sn - dy * cs) / extent + 0.5f;
            break;
        }
    }
    if (g.repeats > 0) t = std::fmod(std::max(t, 0.0f) * (g.repeats + 1), 1.0f);
    if (g.invert) t = 1.0f - t;
    return t;
}

std::vector<Path> text_outline_paths(const TextInfo& t, const text::Font& font, float* baseline, std::vector<int>* glyph_ids) {
    std::vector<Path> out;
    std::vector<int> ids;
    text::Font::Layout lay;
    const auto contours = font.outlines(t.text, t.size, static_cast<text::Font::Align>(t.align), 1.0f, 0.0f, &lay, &ids);
    if (baseline) *baseline = static_cast<float>(lay.baseline);
    for (size_t c = 0; c < contours.size(); ++c) {
        Path p;
        p.closed = true;
        for (const auto& pt : contours[c]) {
            Node n;
            n.x = pt.x; n.y = pt.y; n.in_x = pt.in_x; n.in_y = pt.in_y; n.out_x = pt.out_x; n.out_y = pt.out_y;
            n.flags[1] = 0x40;
            p.nodes.push_back(n);
        }
        if (p.nodes.size() < 2) continue;
        p.nodes.front().flags[0] = 1;
        p.nodes.back().flags[1] |= 0x80;
        out.push_back(std::move(p));
        if (glyph_ids) glyph_ids->push_back(c < ids.size() ? ids[c] : -1);
    }
    return out;
}

// Tiled sample of `tile` at (x, y) relative to (ox, oy), scaled and rotated.
static Color tile_sample(const Image& tile, float x, float y, float ox, float oy, float scale, float angle_deg) {
    const float s = std::max(scale, 0.01f);
    const float rad = angle_deg * 3.14159265f / 180.0f;
    const float rx = (x - ox) * std::cos(rad) + (y - oy) * std::sin(rad), ry = -(x - ox) * std::sin(rad) + (y - oy) * std::cos(rad);
    const int pw = tile.width(), ph = tile.height();
    const int px = ((static_cast<int>(std::floor(rx / s)) % pw) + pw) % pw, py = ((static_cast<int>(std::floor(ry / s)) % ph) + ph) % ph;
    return tile.get(px, py);
}

// Coverage factor of a style's texture at (x, y): lightness weighted by alpha.
float texture_factor(const PaintStyle& style, float x, float y, float ox, float oy) {
    if (!style.texture || style.texture->empty() || style.texture_strength <= 0.0f) return 1.0f;
    const Color t = tile_sample(*style.texture, x, y, ox, oy, style.texture_scale, style.texture_angle);
    const float lum = (0.299f * t.r + 0.587f * t.g + 0.114f * t.b) / 255.0f * (t.a / 255.0f);
    return 1.0f - std::clamp(style.texture_strength, 0.0f, 1.0f) * (1.0f - lum);
}

void paint(Image& dst, const std::vector<uint8_t>& cov, int w, int h, const PaintStyle& style, float bx0, float by0, float bx1, float by1) {
    if (!style.enabled()) return;
    const bool textured = style.texture && !style.texture->empty() && style.texture_strength > 0.0f;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t c = cov[static_cast<size_t>(y) * w + x];
            if (!c) continue;
            Color col = style.color;
            if (style.kind == PaintStyle::Kind::Gradient) {
                col = style.gradient.at(gradient_t(style.gradient, x + 0.5f, y + 0.5f, bx0, by0, bx1, by1));
            } else if (style.kind == PaintStyle::Kind::Pattern && style.pattern && !style.pattern->empty()) {
                const float s = std::max(style.pattern_scale, 0.01f);
                const float rad = style.pattern_angle * 3.14159265f / 180.0f;
                const float rx = (x - bx0) * std::cos(rad) + (y - by0) * std::sin(rad), ry = -(x - bx0) * std::sin(rad) + (y - by0) * std::cos(rad);
                const int pw = style.pattern->width(), ph = style.pattern->height();
                const int px = ((static_cast<int>(std::floor(rx / s)) % pw) + pw) % pw, py = ((static_cast<int>(std::floor(ry / s)) % ph) + ph) % ph;
                col = style.pattern->get(px, py);
            }
            float a = c / 255.0f;
            if (textured) a *= texture_factor(style, static_cast<float>(x), static_cast<float>(y), bx0, by0);
            if (a > 0.0f) raster::blend_over(dst, x, y, col, a);
        }
}

// --- Strokes: dashes and caps ---------------------------------------------

namespace {
using Pt = std::pair<float, float>;

// Cap outline at (x, y) pointing along (dx, dy); sizes are in stroke widths.
std::vector<Pt> cap_polygon(uint32_t type, float x, float y, float dx, float dy, float cw, float ch, float width) {
    std::vector<Pt> out;
    const float len = std::hypot(dx, dy);
    if (len <= 0.0f || type == 0) return out;
    dx /= len; dy /= len;
    const float nx = -dy, ny = dx;
    const float hw = std::max(cw * width, 1.0f), hh = std::max(ch * width, 1.0f);
    auto at = [&](float a, float b) { return Pt{x + dx * a + nx * b, y + dy * a + ny * b}; };
    switch (type) {
        case 1: case 12: {  // round / ball
            const float r = (type == 12 ? std::max(hw, hh) : hw) * 0.5f;
            const float cx = type == 12 ? x + dx * r : x, cy = type == 12 ? y + dy * r : y;
            for (int i = 0; i < 24; ++i) {
                const float a = static_cast<float>(i) / 24 * 6.2831853f;
                out.emplace_back(cx + std::cos(a) * r, cy + std::sin(a) * r);
            }
            break;
        }
        case 2:  // square
            out = {at(0, -hw * 0.5f), at(hh, -hw * 0.5f), at(hh, hw * 0.5f), at(0, hw * 0.5f)};
            break;
        case 4:  // wide arrow with a notched base
            out = {at(hh, 0), at(-hh * 0.25f, -hw * 0.5f), at(0, 0), at(-hh * 0.25f, hw * 0.5f)};
            break;
        case 7: {  // fleur-de-lis stand-in: a diamond
            out = {at(hh, 0), at(hh * 0.5f, -hw * 0.5f), at(0, 0), at(hh * 0.5f, hw * 0.5f)};
            break;
        }
        default:  // 3 and unknown types: triangle
            out = {at(hh, 0), at(0, -hw * 0.5f), at(0, hw * 0.5f)};
            break;
    }
    return out;
}

void add_cap(Mask& acc, uint32_t type, const Pt& p, const Pt& toward_line, float cw, float ch, float width, bool aa) {
    // Caps point away from the line.
    std::vector<Pt> poly = cap_polygon(type, p.first, p.second, p.first - toward_line.first, p.second - toward_line.second, cw, ch, width);
    if (poly.size() < 3) return;
    Mask m = mask::polygon(acc.width(), acc.height(), poly, aa);
    mask::combine(acc, m, mask::Combine::Add);
}

// Splits a polyline into dashes; lengths are multiples of the stroke width.
std::vector<std::vector<Pt>> dash_polyline(const std::vector<Pt>& pts, const std::vector<float>& pattern, float width) {
    std::vector<std::vector<Pt>> out;
    if (pts.size() < 2) return out;
    std::vector<float> pat;
    for (float v : pattern) pat.push_back(std::max(v * width, 0.5f));
    if (pat.size() % 2) pat.insert(pat.end(), pat.begin(), pat.end());  // odd counts repeat as dash/gap swaps
    size_t pi = 0;
    float remain = pat[0];
    bool on = true;
    std::vector<Pt> cur{pts[0]};
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        Pt a = pts[i];
        const Pt b = pts[i + 1];
        float seg = std::hypot(b.first - a.first, b.second - a.second);
        while (seg > 0.0f) {
            if (remain >= seg) {
                remain -= seg;
                if (on) cur.push_back(b);
                seg = 0.0f;
                break;
            }
            const float t = remain / seg;
            const Pt m{a.first + (b.first - a.first) * t, a.second + (b.second - a.second) * t};
            if (on) { cur.push_back(m); out.push_back(cur); cur.clear(); }
            else cur = {m};
            on = !on;
            seg -= remain;
            a = m;
            pi = (pi + 1) % pat.size();
            remain = pat[pi];
        }
    }
    if (on && cur.size() >= 2) out.push_back(cur);
    return out;
}
}  // namespace

void stroke_polyline(Mask& acc, const std::vector<std::pair<float, float>>& pts, bool closed, float width, const LineStyle& line, bool aa) {
    if (pts.size() < 2) return;
    const int w = acc.width(), h = acc.height();
    if (!line.dashed()) {
        Mask m = mask::polyline(w, h, pts, width, aa);
        mask::combine(acc, m, mask::Combine::Add);
    } else {
        for (const auto& piece : dash_polyline(pts, line.dashes, width)) {
            Mask m = mask::polyline(w, h, piece, width, aa);
            mask::combine(acc, m, mask::Combine::Add);
            if (line.seg_caps_on && piece.size() >= 2) {
                add_cap(acc, line.seg_start_cap, piece.front(), piece[1], line.seg_start_w, line.seg_start_h, width, aa);
                add_cap(acc, line.seg_end_cap, piece.back(), piece[piece.size() - 2], line.seg_end_w, line.seg_end_h, width, aa);
            }
        }
    }
    if (!closed) {
        add_cap(acc, line.first_cap, pts.front(), pts[1], line.first_w, line.first_h, width, aa);
        add_cap(acc, line.last_cap, pts.back(), pts[pts.size() - 2], line.last_w, line.last_h, width, aa);
    }
}

// Objects are stored bottom-first: the last object in the list is drawn on
// top (the original's layer palette lists them in reverse).
void rasterize(const std::vector<Object>& objects, Image& dst) {
    const int w = dst.width(), h = dst.height();
    for (size_t idx = 0; idx < objects.size(); ++idx) {
        const Object& o = objects[idx];
        if (!o.visible || o.paths.empty()) continue;
        float bx0, by0, bx1, by1;
        o.bounds(&bx0, &by0, &bx1, &by1);
        // Fill: even-odd over all subpaths.
        if (o.fill.enabled()) {
            std::vector<std::vector<std::pair<float, float>>> polys;
            for (const Path& p : o.paths) if (p.nodes.size() >= 2) polys.push_back(flatten(p));
            if (!polys.empty()) {
                Mask m = mask::polygons(w, h, polys, o.antialias);
                std::vector<uint8_t> cov(m.data(), m.data() + m.size());
                paint(dst, cov, w, h, o.fill, bx0, by0, bx1, by1);
            }
        }
        if (o.stroke.enabled() && o.stroke_width > 0.0f) {
            Mask acc(w, h);
            for (const Path& p : o.paths) {
                if (p.nodes.size() < 2) continue;
                std::vector<std::pair<float, float>> pts = flatten(p);
                if (p.closed && !pts.empty()) pts.push_back(pts.front());
                stroke_polyline(acc, pts, p.closed, o.stroke_width, o.line, o.antialias);
            }
            std::vector<uint8_t> cov(acc.data(), acc.data() + acc.size());
            paint(dst, cov, w, h, o.stroke, bx0, by0, bx1, by1);
        }
    }
}

// --- Queries ---------------------------------------------------------------

bool outline_bounds(const Object& o, float* x0, float* y0, float* x1, float* y1) {
    bool any = false;
    for (const Path& p : o.paths) {
        for (const auto& pt : flatten(p)) {
            if (!any) { *x0 = *x1 = pt.first; *y0 = *y1 = pt.second; any = true; }
            *x0 = std::min(*x0, pt.first); *x1 = std::max(*x1, pt.first);
            *y0 = std::min(*y0, pt.second); *y1 = std::max(*y1, pt.second);
        }
    }
    return any;
}

bool hit_test(const Object& o, float x, float y, float tolerance) {
    float bx0, by0, bx1, by1;
    if (!outline_bounds(o, &bx0, &by0, &bx1, &by1)) return false;
    const float tol = std::max(tolerance, o.stroke.enabled() ? o.stroke_width * 0.5f : 0.0f);
    if (x < bx0 - tol || x > bx1 + tol || y < by0 - tol || y > by1 + tol) return false;
    bool inside = false;
    for (const Path& p : o.paths) {
        const auto pts = flatten(p);
        const size_t n = pts.size();
        if (n < 2) continue;
        // Distance to the outline.
        const size_t segs = p.closed ? n : n - 1;
        for (size_t i = 0; i < segs; ++i) {
            const auto& a = pts[i]; const auto& b = pts[(i + 1) % n];
            const float dx = b.first - a.first, dy = b.second - a.second;
            const float len2 = dx * dx + dy * dy;
            float t = len2 > 0 ? ((x - a.first) * dx + (y - a.second) * dy) / len2 : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            const float px = a.first + dx * t, py = a.second + dy * t;
            if (std::hypot(x - px, y - py) <= tol) return true;
        }
        // Even-odd inside test (fills are drawn even-odd too).
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            const auto& a = pts[i]; const auto& b = pts[j];
            if ((a.second > y) != (b.second > y) && x < (b.first - a.first) * (y - a.second) / (b.second - a.second) + a.first) inside = !inside;
        }
    }
    return inside && o.fill.enabled();
}

size_t group_end(const std::vector<Object>& objects, size_t i) {
    if (i >= objects.size()) return objects.size();
    if (!objects[i].is_group) return i + 1;
    size_t p = i + 1;
    for (uint32_t k = 0; k < objects[i].group_count && p < objects.size(); ++k) p = group_end(objects, p);
    return p;
}

int group_of(const std::vector<Object>& objects, size_t i) {
    int best = -1;  // innermost: the last group that starts before i and spans it
    for (size_t g = 0; g < i; ++g)
        if (objects[g].is_group && group_end(objects, g) > i) best = static_cast<int>(g);
    return best;
}

// --- Builders ------------------------------------------------------------

namespace {
Node corner(float x, float y) { Node n; n.x = n.in_x = n.out_x = x; n.y = n.in_y = n.out_y = y; n.flags[1] = 0x40; return n; }
Object finish(Object o) {
    for (Path& p : o.paths) {
        if (p.nodes.empty()) continue;
        p.nodes.front().flags[0] |= 1;
        if (p.closed) p.nodes.back().flags[1] |= 0x80;
    }
    return o;
}
}  // namespace

Object make_rectangle(float x0, float y0, float x1, float y1) {
    Object o;
    o.name = "Rectangle";
    Path p;
    p.nodes = {corner(x0, y1), corner(x0, y0), corner(x1, y0), corner(x1, y1)};
    o.paths.push_back(p);
    return finish(o);
}

Object make_rounded_rectangle(float x0, float y0, float x1, float y1, float r) {
    r = std::clamp(r, 0.0f, std::min(x1 - x0, y1 - y0) * 0.5f);
    if (r <= 0.0f) return make_rectangle(x0, y0, x1, y1);
    const float k = 0.5523f * r;  // circular arc approximation
    Object o;
    o.name = "Rounded rectangle";
    Path p;
    auto node = [](float x, float y, float ix, float iy, float ox, float oy) { Node n; n.x = x; n.y = y; n.in_x = ix; n.in_y = iy; n.out_x = ox; n.out_y = oy; return n; };
    p.nodes = {
        node(x0 + r, y0, x0 + r - k, y0, x0 + r, y0), node(x1 - r, y0, x1 - r, y0, x1 - r + k, y0),
        node(x1, y0 + r, x1, y0 + r - k, x1, y0 + r), node(x1, y1 - r, x1, y1 - r, x1, y1 - r + k),
        node(x1 - r, y1, x1 - r + k, y1, x1 - r, y1), node(x0 + r, y1, x0 + r, y1, x0 + r - k, y1),
        node(x0, y1 - r, x0, y1 - r + k, x0, y1 - r), node(x0, y0 + r, x0, y0 + r, x0, y0 + r - k)};
    o.paths.push_back(p);
    return finish(o);
}

Object make_ellipse(float cx, float cy, float rx, float ry) {
    const float kx = 0.5523f * rx, ky = 0.5523f * ry;
    Object o;
    o.name = "Ellipse";
    Path p;
    auto node = [](float x, float y, float ix, float iy, float ox, float oy) { Node n; n.x = x; n.y = y; n.in_x = ix; n.in_y = iy; n.out_x = ox; n.out_y = oy; return n; };
    p.nodes = {node(cx, cy - ry, cx - kx, cy - ry, cx + kx, cy - ry), node(cx + rx, cy, cx + rx, cy - ky, cx + rx, cy + ky),
               node(cx, cy + ry, cx + kx, cy + ry, cx - kx, cy + ry), node(cx - rx, cy, cx - rx, cy + ky, cx - rx, cy - ky)};
    o.paths.push_back(p);
    return finish(o);
}

Object make_polygon(const std::vector<std::pair<float, float>>& pts, bool closed) {
    Object o;
    o.name = closed ? "Polygon" : "Line";
    Path p;
    p.closed = closed;
    for (const auto& pt : pts) p.nodes.push_back(corner(pt.first, pt.second));
    o.paths.push_back(p);
    return finish(o);
}

}  // namespace firn::vec
