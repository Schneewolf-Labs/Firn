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
    return {width_, height_, clone_layers(), active_, selection_, alpha_};
}

void Document::restore(const State& s) {
    width_ = s.width;
    height_ = s.height;
    replace_layers(s.layers, s.active);
    set_selection(s.selection);
    alpha_ = s.alpha;
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

// Blends rows [y0, y1) x [x0, x1) of `src` (document-sized, straight alpha)
// onto `dst` with a layer's opacity, blend mode and optional mask. Normal
// mode at full opacity without a mask takes an integer fast path.
void blend_rows(Image& dst, const Image& src, const Layer& L, int w, const raster::Rect& r) {
    const float lo = std::clamp(L.opacity, 0.0f, 1.0f);
    const bool masked = L.has_mask() && L.mask_enabled;
    const bool fast = !masked && L.blend == BlendMode::Normal && lo >= 1.0f;
    uint8_t* d = dst.data();
    const uint8_t* s = src.data();
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            if (fast) {
                const int sa = s[i + 3];
                if (sa == 0) continue;
                if (sa == 255) { std::memcpy(d + i, s + i, 4); continue; }
                const int da = d[i + 3];
                const int oa = sa + da * (255 - sa) / 255;
                if (oa == 0) continue;
                for (int c = 0; c < 3; ++c)
                    d[i + c] = static_cast<uint8_t>((s[i + c] * sa + d[i + c] * da * (255 - sa) / 255) / oa);
                d[i + 3] = static_cast<uint8_t>(oa);
                continue;
            }
            if (masked) {
                const uint8_t m = L.mask.at(x, y);
                if (m == 0) continue;
                uint8_t px[4] = {s[i], s[i + 1], s[i + 2], static_cast<uint8_t>(s[i + 3] * m / 255)};
                blend::pixel(d + i, px, lo, L.blend, L.blend == BlendMode::Dissolve ? blend::position_hash(x, y) : 0u);
            } else {
                blend::pixel(d + i, s + i, lo, L.blend, L.blend == BlendMode::Dissolve ? blend::position_hash(x, y) : 0u);
            }
        }
}

// Splits the rect into row bands and blends them on worker threads.
void blend_layer(Image& dst, const Image& src, const Layer& L, int w, const raster::Rect& r) {
    const int rows = r.y1 - r.y0;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const int bands = static_cast<int>(std::min<size_t>(hw, static_cast<size_t>(std::max(1, rows * (r.x1 - r.x0) / 65536))));
    if (bands <= 1) { blend_rows(dst, src, L, w, r); return; }
    std::vector<std::thread> pool;
    for (int b = 0; b < bands; ++b) {
        raster::Rect band = r;
        band.y0 = r.y0 + rows * b / bands;
        band.y1 = r.y0 + rows * (b + 1) / bands;
        pool.emplace_back([&, band] { blend_rows(dst, src, L, w, band); });
    }
    for (auto& t : pool) t.join();
}

}  // namespace

void Document::composite_into(Image& dst, const raster::Rect& rect) const {
    const raster::Rect r = rect.clipped(width_, height_);
    if (r.empty()) return;
    // Clear the region, then blend the top-level stack into it.
    for (int y = r.y0; y < r.y1; ++y)
        std::memset(dst.data() + (static_cast<size_t>(y) * width_ + r.x0) * 4, 0, static_cast<size_t>(r.x1 - r.x0) * 4);
    if (layers_.empty()) return;
    composite_region(dst, 0, layers_.size() - 1, r);
}

void Document::composite_region(Image& out, size_t from, size_t to, const raster::Rect& r) const {
    size_t li = from;
    while (li <= to && li < layers_.size()) {
        const Layer& L = *layers_[li];
        if (L.type == LayerType::Group) {
            const size_t end = group_end(li);
            if (L.visible && L.opacity > 0.0f && end > li + 1) {
                Image inner(width_, height_, {0, 0, 0, 0});
                composite_region(inner, li + 1, std::min(end - 1, to), r);
                blend_layer(out, inner, L, width_, r);
            }
            li = end;
            continue;
        }
        if (L.visible && L.opacity > 0.0f && !L.pixels.empty()) blend_layer(out, L.pixels, L, width_, r);
        ++li;
    }
}

Image Document::composite_range(size_t from, size_t to) const {
    Image out(width_, height_, {0, 0, 0, 0});
    composite_region(out, from, to, {0, 0, width_, height_});
    return out;
}

}  // namespace firn
