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


Image16::Image16(int width, int height, uint16_t r, uint16_t g, uint16_t b, uint16_t a)
    : width_(width), height_(height), pixels_(static_cast<size_t>(width) * height * 4) {
    for (size_t i = 0; i < pixels_.size(); i += 4) { pixels_[i] = r; pixels_[i + 1] = g; pixels_[i + 2] = b; pixels_[i + 3] = a; }
}

Image to_image8(const Image16& deep) {
    Image out(deep.width(), deep.height());
    const uint16_t* s = deep.data();
    uint8_t* d = out.data();
    for (size_t i = 0; i < deep.size(); ++i) d[i] = static_cast<uint8_t>((s[i] + 128) / 257);
    return out;
}

Image16 to_image16(const Image& img) {
    Image16 out(img.width(), img.height());
    const uint8_t* s = img.data();
    uint16_t* d = out.data();
    for (size_t i = 0; i < out.size(); ++i) d[i] = static_cast<uint16_t>(s[i] * 257);
    return out;
}

}  // namespace firn
