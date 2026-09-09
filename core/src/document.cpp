#include "firn/document.h"

#include <algorithm>
#include <cassert>

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

Image Document::composite() const {
    return layers_.empty() ? Image(width_, height_) : composite_range(0, layers_.size() - 1);
}

Image Document::composite_range(size_t from, size_t to) const {
    Image out(width_, height_, {0, 0, 0, 0});
    uint8_t* dst = out.data();
    for (size_t li = from; li <= to && li < layers_.size(); ++li) {
        const Layer& L = *layers_[li];
        if (!L.visible || L.opacity <= 0.0f) continue;
        const uint8_t* src = L.pixels.data();
        const float lo = std::clamp(L.opacity, 0.0f, 1.0f);
        for (int y = 0; y < height_; ++y) {
            for (int x = 0; x < width_; ++x) {
                const size_t i = (static_cast<size_t>(y) * width_ + x) * 4;
                blend::pixel(dst + i, src + i, lo, L.blend,
                             L.blend == BlendMode::Dissolve ? blend::position_hash(x, y) : 0u);
            }
        }
    }
    return out;
}

}  // namespace firn
