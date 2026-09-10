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
    return {width_, height_, clone_layers(), active_, selection_, alpha_, icc_};
}

void Document::restore(const State& s) {
    width_ = s.width;
    height_ = s.height;
    replace_layers(s.layers, s.active);
    set_selection(s.selection);
    alpha_ = s.alpha;
    icc_ = s.icc;
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
    return {L.name, L.visible, L.opacity, L.blend};
}

void Document::set_props(size_t i, const LayerProps& p) {
    Layer& L = layer(i);
    L.name = p.name;
    L.visible = p.visible;
    L.opacity = p.opacity;
    L.blend = p.blend;
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
    const bool fast = !masked && L.blend == BlendMode::Normal && lo >= 1.0f;
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
            if (masked) {
                const uint8_t m = L.mask.at(x, y);
                if (m == 0) continue;
                uint8_t px[4] = {s[si], s[si + 1], s[si + 2], static_cast<uint8_t>(s[si + 3] * m / 255)};
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
void Document::apply_filter_layer(Image& out, int ox, int oy, size_t from, size_t li, const raster::Rect& r) const {
    const Layer& L = *layers_[li];
    const int reach = L.adjustment.reach();
    const raster::Rect rp = raster::Rect{r.x0 - reach, r.y0 - reach, r.x1 + reach, r.y1 + reach}.clipped(width_, height_);
    if (rp.empty()) return;
    Image below(rp.x1 - rp.x0, rp.y1 - rp.y0, {0, 0, 0, 0});
    if (li > from) composite_region(below, rp.x0, rp.y0, from, li - 1, rp);
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
    for (const auto& L : layers_) if (L->visible && L->is_adjustment() && L->adjustment.is_filter()) reach += L->adjustment.reach();
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

void Document::composite_region(Image& out, int ox, int oy, size_t from, size_t to, const raster::Rect& r) const {
    size_t li = from;
    while (li <= to && li < layers_.size()) {
        const Layer& L = *layers_[li];
        if (L.type == LayerType::Group) {
            const size_t end = group_end(li);
            if (L.visible && L.opacity > 0.0f && end > li + 1) {
                Image inner(r.x1 - r.x0, r.y1 - r.y0, {0, 0, 0, 0});
                composite_region(inner, r.x0, r.y0, li + 1, std::min(end - 1, to), r);
                blend_layer(out, ox, oy, inner, r.x0, r.y0, L, r);
            }
            li = end;
            continue;
        }
        if (L.type == LayerType::Adjustment) {
            if (L.visible && L.opacity > 0.0f) {
                if (L.adjustment.is_filter()) apply_filter_layer(out, ox, oy, from, li, r);
                else apply_adjustment_layer(out, ox, oy, L, r);
            }
            ++li;
            continue;
        }
        if (L.visible && L.opacity > 0.0f && !L.pixels.empty()) blend_layer(out, ox, oy, L.pixels, 0, 0, L, r);
        ++li;
    }
}

Image Document::composite_range(size_t from, size_t to) const {
    Image out(width_, height_, {0, 0, 0, 0});
    composite_region(out, 0, 0, from, to, {0, 0, width_, height_});
    return out;
}

}  // namespace firn
