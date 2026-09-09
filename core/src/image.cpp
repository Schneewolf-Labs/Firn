#include "firn/image.h"

#include <cassert>

namespace firn {

Image::Image(int width, int height, Color fill)
    : width_(width), height_(height), pixels_(static_cast<size_t>(width) * height * 4) {
    assert(width >= 0 && height >= 0);
    this->fill(fill);
}

Color Image::get(int x, int y) const {
    assert(x >= 0 && x < width_ && y >= 0 && y < height_);
    const uint8_t* p = &pixels_[(static_cast<size_t>(y) * width_ + x) * 4];
    return {p[0], p[1], p[2], p[3]};
}

void Image::set(int x, int y, Color c) {
    assert(x >= 0 && x < width_ && y >= 0 && y < height_);
    uint8_t* p = &pixels_[(static_cast<size_t>(y) * width_ + x) * 4];
    p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a;
}

void Image::fill(Color c) {
    for (size_t i = 0; i < pixels_.size(); i += 4) {
        pixels_[i + 0] = c.r;
        pixels_[i + 1] = c.g;
        pixels_[i + 2] = c.b;
        pixels_[i + 3] = c.a;
    }
}

}  // namespace firn
