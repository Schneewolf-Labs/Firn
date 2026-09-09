#pragma once
#include <cstdint>
#include <vector>

namespace firn {

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
};

// A straight-alpha RGBA8 raster. Deliberately the simplest possible
// representation; tiling and 16-bit channels can come later behind this API.
class Image {
public:
    Image() = default;
    Image(int width, int height, Color fill = {0, 0, 0, 0});

    int width() const { return width_; }
    int height() const { return height_; }
    bool empty() const { return width_ == 0 || height_ == 0; }
    size_t size_bytes() const { return pixels_.size(); }

    uint8_t* data() { return pixels_.data(); }
    const uint8_t* data() const { return pixels_.data(); }

    Color get(int x, int y) const;
    void set(int x, int y, Color c);
    void fill(Color c);

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<uint8_t> pixels_;  // width*height*4, row-major, top-down
};

}  // namespace firn
