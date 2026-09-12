#include "firn/document.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <thread>

namespace firn {

Document::Document(int width, int height) : width_(width), height_(height) {}

void Document::set_active_layer(int i) {
    if (i < -1 || i >= static_cast<int>(layers_.size())) return;
    active_ = i;
}

Layer& Document::add_layer(std::string name, int at) {
    auto layer = std::make_unique<Layer>();
    layer->name = std::move(name);
    layer->pixels = Image(width_, height_);
    size_t idx = (at < 0 || at > static_cast<int>(layers_.size())) ? layers_.size() : at;
    insert_layer(std::move(layer), idx);
    return *layers_[idx];
}

void Document::insert_layer(std::unique_ptr<Layer> layer, size_t at) {
    at = std::min(at, layers_.size());
    layers_.insert(layers_.begin() + at, std::move(layer));
    active_ = static_cast<int>(at);
    touch();
}

std::unique_ptr<Layer> Document::remove_layer(size_t i) {
    assert(i < layers_.size());
    auto out = std::move(layers_[i]);
    layers_.erase(layers_.begin() + i);
    if (layers_.empty()) active_ = -1;
    else if (active_ >= static_cast<int>(layers_.size())) active_ = static_cast<int>(layers_.size()) - 1;
    touch();
    return out;
}

void Document::set_selection(Mask m) {
    // Normalise "selected nothing" to "no selection".
    if (!m.empty() && !m.any()) m = Mask();
    selection_ = std::move(m);
    ++selection_revision_;
}

void Document::move_layer(size_t from, size_t to) {
    if (from >= layers_.size() || to >= layers_.size() || from == to) return;
    auto l = std::move(layers_[from]);
    layers_.erase(layers_.begin() + from);
    layers_.insert(layers_.begin() + to, std::move(l));
    active_ = static_cast<int>(to);
    touch();
}

std::vector<Layer> Document::clone_layers() const {
    std::vector<Layer> out;
    out.reserve(layers_.size());
    for (const auto& l : layers_) out.push_back(*l);
    return out;
}

void Document::replace_layers(const std::vector<Layer>& layers, int active) {
    layers_.clear();
    for (const Layer& l : layers) layers_.push_back(std::make_unique<Layer>(l));
    active_ = layers_.empty() ? -1 : std::clamp(active, 0, static_cast<int>(layers_.size()) - 1);
    touch();
}

Document::State Document::snapshot() const {
    return {width_, height_, clone_layers(), active_, selection_, alpha_, icc_, metadata_};
}

void Document::restore(const State& s) {
    width_ = s.width;
    height_ = s.height;
    replace_layers(s.layers, s.active);
    set_selection(s.selection);
    alpha_ = s.alpha;
    icc_ = s.icc;
    metadata_ = s.metadata;
}

Image16 Document::composite16() const {
    bool simple = true;
    for (const auto& L : layers_) if (L->visible && (!L->is_raster() || L->blend != BlendMode::Normal || L->has_mask() || L->depth > 0)) simple = false;
    if (!simple) return to_image16(composite());
    Image16 out(width_, height_);
    for (const auto& L : layers_) {
        if (!L->visible || L->opacity <= 0.0f || L->pixels.empty()) continue;
        const Image16 src = L->is_deep() ? *L->deep : to_image16(L->pixels);
        const float op = L->opacity;
        uint16_t* d = out.data();
        const uint16_t* s = src.data();
        for (size_t i = 0; i < out.size(); i += 4) {
            const float sa = s[i + 3] / 65535.0f * op, da = d[i + 3] / 65535.0f;
            const float oa = sa + da * (1.0f - sa);
            if (oa <= 0.0f) continue;
            for (int c = 0; c < 3; ++c) d[i + c] = static_cast<uint16_t>(std::clamp((s[i + c] * sa + d[i + c] * da * (1.0f - sa)) / oa, 0.0f, 65535.0f) + 0.5f);
            d[i + 3] = static_cast<uint16_t>(oa * 65535.0f + 0.5f);
        }
    }
    return out;
}

int Document::bit_depth() const {
    for (const auto& L : layers_) if (L->is_deep()) return 16;
    return 8;
}

void Document::set_bit_depth(int bits) {
    for (auto& L : layers_) {
        if (!L->is_raster()) continue;
        if (bits == 16 && !L->is_deep()) L->deep = std::make_shared<const Image16>(to_image16(L->pixels));
        else if (bits != 16) L->deep.reset();
    }
    touch();
}

void Document::rasterize_vector_layer(size_t i) {
    Layer& L = layer(i);
    if (!L.is_vector()) return;
    L.pixels = Image(width_, height_, {0, 0, 0, 0});
    vec::rasterize(L.objects, L.pixels);
    touch();
}

LayerProps Document::props(size_t i) const {
    const Layer& L = layer(i);
    return {L.name, L.visible, L.opacity, L.blend, L.clipped, L.pass_through, L.lock_alpha, L.ranges};
}

void Document::set_props(size_t i, const LayerProps& p) {
    Layer& L = layer(i);
    L.name = p.name;
    L.visible = p.visible;
    L.opacity = p.opacity;
    L.blend = p.blend;
    L.clipped = p.clipped;
    L.pass_through = p.pass_through;
    L.lock_alpha = p.lock_alpha;
    L.ranges = p.ranges;
    touch();
}

size_t Document::group_end(size_t g) const {
    if (g >= layers_.size()) return layers_.size();
    const int d = layers_[g]->depth;
    size_t i = g + 1;
    while (i < layers_.size() && layers_[i]->depth > d) ++i;
    return i;
}

int Document::parent_group(size_t index) const {
    if (index >= layers_.size()) return -1;
    const int d = layers_[index]->depth;
    if (d == 0) return -1;
    for (size_t i = index; i-- > 0;)
        if (layers_[i]->type == LayerType::Group && layers_[i]->depth == d - 1) return static_cast<int>(i);
    return -1;
}

Image Document::composite() const {
    return layers_.empty() ? Image(width_, height_) : composite_range(0, layers_.size() - 1);
}

namespace {

// Blends rows [y0, y1) x [x0, x1) (document coordinates) of `src`, whose
// pixel (0, 0) sits at document (sox, soy), onto `dst`, whose pixel (0, 0)
// sits at (dox, doy), with a layer's opacity, blend mode and optional mask.
// Normal mode at full opacity without a mask takes an integer fast path.
void blend_rows(Image& dst, int dox, int doy, const Image& src, int sox, int soy, const Layer& L, const raster::Rect& r) {
    const float lo = std::clamp(L.opacity, 0.0f, 1.0f);
    const bool masked = L.has_mask() && L.mask_enabled;
    const bool ranged = !L.ranges.identity();
    const bool fast = !masked && !ranged && L.blend == BlendMode::Normal && lo >= 1.0f;
    uint8_t* d = dst.data();
    const uint8_t* s = src.data();
    const int dw = dst.width(), sw = src.width();
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) {
            const size_t di = (static_cast<size_t>(y - doy) * dw + (x - dox)) * 4;
            const size_t si = (static_cast<size_t>(y - soy) * sw + (x - sox)) * 4;
            if (fast) {
                const int sa = s[si + 3];
                if (sa == 0) continue;
                if (sa == 255) { std::memcpy(d + di, s + si, 4); continue; }
                const int da = d[di + 3];
                const int oa = sa + da * (255 - sa) / 255;
                if (oa == 0) continue;
                for (int c = 0; c < 3; ++c)
                    d[di + c] = static_cast<uint8_t>((s[si + c] * sa + d[di + c] * da * (255 - sa) / 255) / oa);
                d[di + 3] = static_cast<uint8_t>(oa);
                continue;
            }
            // The mask and the blend ranges both scale the source alpha
            // before the blend, so they compose the way two masks would.
            float cover = 1.0f;
            if (masked) {
                const uint8_t m = L.mask.at(x, y);
                if (m == 0) continue;
                cover = m / 255.0f;
            }
            if (ranged) {
                cover *= L.ranges.factor(s + si, d + di);
                if (cover <= 0.0f) continue;
            }
            if (cover < 1.0f) {
                uint8_t px[4] = {s[si], s[si + 1], s[si + 2], static_cast<uint8_t>(s[si + 3] * cover + 0.5f)};
                blend::pixel(d + di, px, lo, L.blend, L.blend == BlendMode::Dissolve ? blend::position_hash(x, y) : 0u);
            } else {
                blend::pixel(d + di, s + si, lo, L.blend, L.blend == BlendMode::Dissolve ? blend::position_hash(x, y) : 0u);
            }
        }
}

void blend_layer(Image& dst, int dox, int doy, const Image& src, int sox, int soy, const Layer& L, const raster::Rect& r) {
    const int rows = r.y1 - r.y0;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const int bands = static_cast<int>(std::min<size_t>(hw, static_cast<size_t>(std::max(1, rows * (r.x1 - r.x0) / 65536))));
    if (bands <= 1) { blend_rows(dst, dox, doy, src, sox, soy, L, r); return; }
    std::vector<std::thread> pool;
    for (int b = 0; b < bands; ++b) {
        raster::Rect band = r;
        band.y0 = r.y0 + rows * b / bands;
        band.y1 = r.y0 + rows * (b + 1) / bands;
        pool.emplace_back([&, band] { blend_rows(dst, dox, doy, src, sox, soy, L, band); });
    }
    for (auto& t : pool) t.join();
}

// An adjustment layer transforms what is below it; opacity and its mask
// blend the result back over the untouched pixels. Alpha is kept.
void apply_adjustment_layer(Image& out, int ox, int oy, const Layer& L, const raster::Rect& r) {
    const int rw = r.x1 - r.x0, rh = r.y1 - r.y0;
    if (rw <= 0 || rh <= 0) return;
    const int w = out.width();
    Image region(rw, rh);
    for (int y = 0; y < rh; ++y)
        std::memcpy(region.data() + static_cast<size_t>(y) * rw * 4, out.data() + (static_cast<size_t>(y + r.y0 - oy) * w + (r.x0 - ox)) * 4, static_cast<size_t>(rw) * 4);
    L.adjustment.apply(region);
    const bool masked = L.has_mask() && L.mask_enabled;
    for (int y = 0; y < rh; ++y) {
        uint8_t* d = out.data() + (static_cast<size_t>(y + r.y0 - oy) * w + (r.x0 - ox)) * 4;
        const uint8_t* s = region.data() + static_cast<size_t>(y) * rw * 4;
        for (int x = 0; x < rw; ++x, d += 4, s += 4) {
            float f = L.opacity;
            if (masked) f *= L.mask.at(x + r.x0, y + r.y0) / 255.0f;
            if (f <= 0.0f || d[3] == 0) continue;
            for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>(d[c] + (s[c] - d[c]) * f + 0.5f);
        }
    }
}

}  // namespace

// A filter layer needs what lies below it a little beyond `r` (its reach),
// so that part of the stack is composited again over the padded rect and
// filtered there; only `r` is written back, blended by opacity and mask.
void Document::apply_filter_layer(Image& out, int ox, int oy, size_t from, size_t li, const raster::Rect& r, bool source_is_out) const {
    const Layer& L = *layers_[li];
    const int reach = L.adjustment.reach();
    const raster::Rect rp = raster::Rect{r.x0 - reach, r.y0 - reach, r.x1 + reach, r.y1 + reach}.clipped(width_, height_);
    if (rp.empty()) return;
    Image below(rp.x1 - rp.x0, rp.y1 - rp.y0, {0, 0, 0, 0});
    if (source_is_out) {
        for (int y = rp.y0; y < rp.y1; ++y)
            for (int x = rp.x0; x < rp.x1; ++x) {
                const int sx = x - ox, sy = y - oy;
                if (sx >= 0 && sy >= 0 && sx < out.width() && sy < out.height())
                    below.set(x - rp.x0, y - rp.y0, out.get(sx, sy));
            }
    } else if (li > from) {
        composite_region(below, rp.x0, rp.y0, from, li - 1, rp);
    }
    L.adjustment.apply(below);
    const bool masked = L.has_mask() && L.mask_enabled;
    const int w = out.width(), bw = below.width();
    for (int y = r.y0; y < r.y1; ++y) {
        uint8_t* d = out.data() + (static_cast<size_t>(y - oy) * w + (r.x0 - ox)) * 4;
        const uint8_t* s = below.data() + (static_cast<size_t>(y - rp.y0) * bw + (r.x0 - rp.x0)) * 4;
        for (int x = r.x0; x < r.x1; ++x, d += 4, s += 4) {
            float f = L.opacity;
            if (masked) f *= L.mask.at(x, y) / 255.0f;
            if (f <= 0.0f) continue;
            for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(d[c] + (s[c] - d[c]) * f + 0.5f);
        }
    }
}

int Document::filter_reach() const {
    int reach = 0;
    for (const auto& L : layers_) {
        if (!L->visible) continue;
        if (L->is_adjustment() && L->adjustment.is_filter()) reach += L->adjustment.reach();
        if (L->style.any()) reach = std::max(reach, L->style.reach());
    }
    return reach;
}

void Document::touch(const raster::Rect& rect) {
    ++revision_;
    const int g = filter_reach();
    const raster::Rect r = raster::Rect{rect.x0 - g, rect.y0 - g, rect.x1 + g, rect.y1 + g}.clipped(width_, height_);
    dirty_ = dirty_.empty() ? r : dirty_.united(r);
}

void Document::composite_into(Image& dst, const raster::Rect& rect) const {
    const raster::Rect r = rect.clipped(width_, height_);
    if (r.empty()) return;
    // Clear the region, then blend the top-level stack into it.
    for (int y = r.y0; y < r.y1; ++y)
        std::memset(dst.data() + (static_cast<size_t>(y) * width_ + r.x0) * 4, 0, static_cast<size_t>(r.x1 - r.x0) * 4);
    if (layers_.empty()) return;
    composite_region(dst, 0, 0, 0, layers_.size() - 1, r);
}

// Draws a clipping unit: the base layer at `base`, the layers clipped to it
// in [own_end, ce), and nothing else. The clipped layers are composited onto
// the base's own pixels and then held to the base's alpha, so they show only
// where the base does. The unit blends into `out` with the base's opacity,
// blend mode, mask and style, exactly as the base alone would have.
void Document::composite_clip_unit(Image& out, int ox, int oy, size_t base, size_t own_end, size_t ce, const raster::Rect& r) const {
    const Layer& B = *layers_[base];
    if (!B.visible || B.opacity <= 0.0f) return;
    const int reach = B.style.any() ? B.style.reach() : 0;
    const raster::Rect rp = raster::Rect{r.x0 - reach, r.y0 - reach, r.x1 + reach, r.y1 + reach}.clipped(width_, height_);
    if (rp.empty()) return;

    // The base's own pixels, at full strength: its opacity and blend mode
    // belong to the finished unit, not to what the clipped layers land on.
    Image unit(rp.x1 - rp.x0, rp.y1 - rp.y0, {0, 0, 0, 0});
    if (B.type == LayerType::Group) {
        if (own_end > base + 1) composite_region(unit, rp.x0, rp.y0, base + 1, own_end - 1, rp);
    } else if (!B.pixels.empty()) {
        for (int y = rp.y0; y < rp.y1; ++y)
            for (int x = rp.x0; x < rp.x1; ++x) unit.set(x - rp.x0, y - rp.y0, B.pixels.get(x, y));
    }
    // The base's mask limits the unit's shape too, so the clipped layers stop
    // where the mask hides the base rather than at its raw alpha.
    if (B.has_mask() && B.mask_enabled) {
        for (int y = rp.y0; y < rp.y1; ++y)
            for (int x = rp.x0; x < rp.x1; ++x) {
                Color c = unit.get(x - rp.x0, y - rp.y0);
                c.a = static_cast<uint8_t>(c.a * B.mask.at(x, y) / 255);
                unit.set(x - rp.x0, y - rp.y0, c);
            }
    }

    // Remember the shape, draw the clipped layers onto it, then restore it.
    std::vector<uint8_t> shape(static_cast<size_t>(unit.width()) * unit.height());
    for (size_t i = 0; i < shape.size(); ++i) shape[i] = unit.data()[i * 4 + 3];
    if (ce > own_end) composite_region(unit, rp.x0, rp.y0, own_end, ce - 1, rp, {false, true});
    for (size_t i = 0; i < shape.size(); ++i) unit.data()[i * 4 + 3] = shape[i];

    // The mask is already in the shape; blending it again would square it.
    Layer as_base;
    as_base.opacity = B.opacity;
    as_base.blend = B.blend;
    as_base.visible = true;
    if (reach || B.style.any()) {
        const Image styled = render_layer_style(unit, rp.x0, rp.y0, B.style, r);
        blend_layer(out, ox, oy, styled, r.x0, r.y0, as_base, r);
    } else {
        blend_layer(out, ox, oy, unit, rp.x0, rp.y0, as_base, r);
    }
}

// Draws a pass-through group: its members composite straight onto what is
// already in `out`, so an adjustment or filter layer inside the group reaches
// the whole image below it rather than only its siblings. The group's own
// opacity and mask then say how much of that change survives, by mixing the
// changed image back over the original. A pass-through group has no shape of
// its own, so its blend mode and layer style do not apply.
void Document::composite_pass_through(Image& out, int ox, int oy, size_t group, size_t end, const raster::Rect& r) const {
    const Layer& G = *layers_[group];
    if (!G.visible || G.opacity <= 0.0f || end <= group + 1 || r.empty()) return;

    // Work on a copy of the image so far, so the members can read and change
    // what is below the group.
    Image changed(r.x1 - r.x0, r.y1 - r.y0, {0, 0, 0, 0});
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) {
            const int sx = x - ox, sy = y - oy;
            if (sx >= 0 && sy >= 0 && sx < out.width() && sy < out.height())
                changed.set(x - r.x0, y - r.y0, out.get(sx, sy));
        }
    composite_region(changed, r.x0, r.y0, group + 1, end - 1, r, {true, true});

    const bool masked = G.has_mask() && G.mask_enabled;
    const float go = std::clamp(G.opacity, 0.0f, 1.0f);
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) {
            float f = go;
            if (masked) f *= G.mask.at(x, y) / 255.0f;
            if (f <= 0.0f) continue;
            const int dx = x - ox, dy = y - oy;
            if (dx < 0 || dy < 0 || dx >= out.width() || dy >= out.height()) continue;
            uint8_t* d = out.data() + (static_cast<size_t>(dy) * out.width() + dx) * 4;
            const uint8_t* c = changed.data() + (static_cast<size_t>(y - r.y0) * changed.width() + (x - r.x0)) * 4;
            if (f >= 1.0f) { std::memcpy(d, c, 4); continue; }
            for (int k = 0; k < 4; ++k) d[k] = static_cast<uint8_t>(d[k] + (c[k] - d[k]) * f + 0.5f);
        }
}

size_t Document::clip_end(size_t base) const {
    if (base >= layers_.size()) return base;
    const Layer& B = *layers_[base];
    size_t i = B.type == LayerType::Group ? group_end(base) : base + 1;
    while (i < layers_.size() && layers_[i]->clipped && layers_[i]->depth == B.depth)
        i = layers_[i]->type == LayerType::Group ? group_end(i) : i + 1;
    return i;
}

void Document::composite_region(Image& out, int ox, int oy, size_t from, size_t to, const raster::Rect& r, CompositeOpts opts) const {
    size_t li = from;
    while (li <= to && li < layers_.size()) {
        const Layer& L = *layers_[li];
        // A layer with clipped layers above it forms one unit: they are drawn
        // onto its pixels, held to its alpha, and the result blends with the
        // layer's own opacity, blend mode and mask.
        if (opts.top_clips && !L.clipped) {
            const size_t ce = clip_end(li);
            const size_t own_end = L.type == LayerType::Group ? group_end(li) : li + 1;
            if (ce > own_end && ce <= to + 1) {
                composite_clip_unit(out, ox, oy, li, own_end, ce, r);
                li = ce;
                continue;
            }
        }
        if (L.type == LayerType::Group) {
            const size_t end = group_end(li);
            if (L.pass_through) {
                composite_pass_through(out, ox, oy, li, end, r);
                li = end;
                continue;
            }
            if (L.visible && L.opacity > 0.0f && end > li + 1) {
                // A styled group needs its members beyond `r` as well, so the
                // style has the shape its shadow and glow grow from.
                const int reach = L.style.any() ? L.style.reach() : 0;
                const raster::Rect rp = raster::Rect{r.x0 - reach, r.y0 - reach, r.x1 + reach, r.y1 + reach}.clipped(width_, height_);
                Image inner(rp.x1 - rp.x0, rp.y1 - rp.y0, {0, 0, 0, 0});
                composite_region(inner, rp.x0, rp.y0, li + 1, std::min(end - 1, to), rp);
                if (reach || L.style.any()) {
                    const Image styled = render_layer_style(inner, rp.x0, rp.y0, L.style, r);
                    blend_layer(out, ox, oy, styled, r.x0, r.y0, L, r);
                } else {
                    blend_layer(out, ox, oy, inner, rp.x0, rp.y0, L, r);
                }
            }
            li = end;
            continue;
        }
        if (L.type == LayerType::Adjustment) {
            if (L.visible && L.opacity > 0.0f) {
                if (L.adjustment.is_filter()) apply_filter_layer(out, ox, oy, from, li, r, opts.filters_from_out);
                else apply_adjustment_layer(out, ox, oy, L, r);
            }
            ++li;
            continue;
        }
        if (L.visible && L.opacity > 0.0f && !L.pixels.empty()) {
            if (L.style.any()) {
                const Image styled = render_layer_style(L.pixels, 0, 0, L.style, r);
                blend_layer(out, ox, oy, styled, r.x0, r.y0, L, r);
            } else {
                blend_layer(out, ox, oy, L.pixels, 0, 0, L, r);
            }
        }
        ++li;
    }
}

Image Document::composite_range(size_t from, size_t to) const {
    Image out(width_, height_, {0, 0, 0, 0});
    composite_region(out, 0, 0, from, to, {0, 0, width_, height_});
    return out;
}

}  // namespace firn
