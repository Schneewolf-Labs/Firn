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

Image Document::composite() const {
    Image out(width_, height_, {0, 0, 0, 0});
    uint8_t* dst = out.data();
    const size_t n = static_cast<size_t>(width_) * height_;

    for (const auto& lp : layers_) {
        const Layer& L = *lp;
        if (!L.visible || L.opacity <= 0.0f) continue;
        const uint8_t* src = L.pixels.data();
        const float lo = std::clamp(L.opacity, 0.0f, 1.0f);

        // Normal blend, straight alpha, "over" operator.
        for (size_t i = 0; i < n; ++i) {
            const uint8_t* s = src + i * 4;
            uint8_t* d = dst + i * 4;
            const float sa = (s[3] / 255.0f) * lo;
            if (sa <= 0.0f) continue;
            const float da = d[3] / 255.0f;
            const float oa = sa + da * (1.0f - sa);
            if (oa <= 0.0f) continue;
            for (int c = 0; c < 3; ++c) {
                const float sc = s[c] / 255.0f;
                const float dc = d[c] / 255.0f;
                const float oc = (sc * sa + dc * da * (1.0f - sa)) / oa;
                d[c] = static_cast<uint8_t>(oc * 255.0f + 0.5f);
            }
            d[3] = static_cast<uint8_t>(oa * 255.0f + 0.5f);
        }
    }
    return out;
}

}  // namespace firn
