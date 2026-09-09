#pragma once
#include <cstdint>
#include <utility>
#include <vector>

#include "firn/image.h"
#include "firn/raster.h"

namespace firn {

// An 8-bit coverage mask the size of the document. Used for selections
// (0 = unselected, 255 = fully selected, in between = feathered/antialiased).
// An empty Mask (width 0) means "no selection": everything is editable.
class Mask {
public:
    Mask() = default;
    Mask(int width, int height, uint8_t fill = 0);

    int width() const { return width_; }
    int height() const { return height_; }
    bool empty() const { return width_ == 0 || height_ == 0; }

    uint8_t at(int x, int y) const { return data_[static_cast<size_t>(y) * width_ + x]; }
    uint8_t& at(int x, int y) { return data_[static_cast<size_t>(y) * width_ + x]; }
    uint8_t* data() { return data_.data(); }
    const uint8_t* data() const { return data_.data(); }
    size_t size() const { return data_.size(); }

    // Bounding rect of non-zero pixels; empty if nothing is selected.
    raster::Rect bounds() const;
    bool any() const { return !bounds().empty(); }

private:
    int width_ = 0, height_ = 0;
    std::vector<uint8_t> data_;
};

namespace mask {

enum class Combine { Replace, Add, Subtract, Intersect };

// Shape rasterizers. Feather is a blur radius in pixels applied to the edge.
Mask rectangle(int w, int h, float x0, float y0, float x1, float y1, bool antialias);
Mask ellipse(int w, int h, float cx, float cy, float rx, float ry, bool antialias);
Mask polygon(int w, int h, const std::vector<std::pair<float, float>>& pts, bool antialias);
// Several closed polygons filled together with the even-odd rule (holes).
Mask polygons(int w, int h, const std::vector<std::vector<std::pair<float, float>>>& polys, bool antialias);
// Rounded rectangle with corner radius; regular polygon and star centered at
// (cx, cy) with radii (rx, ry), first vertex pointing up, rotated by degrees.
Mask rounded_rectangle(int w, int h, float x0, float y0, float x1, float y1, float radius, bool antialias);
Mask regular_polygon(int w, int h, float cx, float cy, float rx, float ry, int sides, float rotation_degrees, bool antialias);
Mask star(int w, int h, float cx, float cy, float rx, float ry, int points, float inner_ratio, float rotation_degrees, bool antialias);
// A polyline stroked with the given width (round joins and caps).
Mask polyline(int w, int h, const std::vector<std::pair<float, float>>& pts, float width, bool antialias);

// Pixels reachable from (x,y) whose max channel difference to the seed is
// <= tolerance (contiguous), or every such pixel in the image (!contiguous).
Mask magic_wand(const Image& img, int x, int y, int tolerance, bool contiguous);

void combine(Mask& dst, const Mask& src, Combine mode);
void invert(Mask& m);
void feather(Mask& m, float radius);
void expand(Mask& m, int pixels);    // grow by a circular structuring element
void contract(Mask& m, int pixels);  // shrink likewise

}  // namespace mask
}  // namespace firn
