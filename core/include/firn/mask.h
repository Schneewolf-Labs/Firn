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

// Selections > Modify, as the original names them.
void feather_inside(Mask& m, float radius);    // soften only within the selection
void feather_outside(Mask& m, float radius);   // soften only outward
void unfeather(Mask& m);                        // hard edge at 50%
void smooth(Mask& m, int amount, bool preserve_corners);   // rounds jagged edges
void shape_antialias(Mask& m, bool inside, bool outside);  // one-pixel soft edge
// Drops selected islands up to `speck_size` pixels and fills unselected holes up to `hole_size`.
void remove_specks_and_holes(Mask& m, int speck_size, int hole_size);
// Every pixel within `tolerance` (max channel difference) of `color`; beyond
// it the selection fades over `softness` more levels.
Mask select_color_range(const Image& img, Color color, int tolerance, int softness);
// Pixels anywhere in the image that are within `tolerance` of a color found
// under the current selection (the original's Select Similar).
Mask select_similar(const Image& img, const Mask& selection, int tolerance);

// Foreground extraction from scribbles: `fg` and `bg` mark known foreground
// and background pixels (nonzero); everything outside `region` (when not
// empty) is background too, and so is the image's outer edge when nothing
// marks any background. Unknown pixels are classified by color likelihood
// (k-means color models of the marks) regularized by geodesic distance to
// the marks, so a region is claimed by the nearest mark it can reach without
// crossing a color edge. Large images are solved at about 1.5 MP and the
// result scaled back up. Returns a selection mask with a soft edge.
Mask foreground_select(const Image& img, const Mask& fg, const Mask& bg, const Mask& region);

// Edge helpers for the freehand selection's Smart Edge and Edge Seeker modes.
// `edge_map` is a Sobel magnitude of the luma, 0..1, one float per pixel.
std::vector<float> edge_map(const Image& img);
// The strongest edge within `range` pixels of (x, y), or (x, y) itself when nothing is stronger.
std::pair<float, float> seek_edge(const std::vector<float>& edges, int w, int h, float x, float y, int range);
// Lowest-cost path from a to b hugging strong edges (8-connected, in a corridor around the segment).
std::vector<std::pair<float, float>> edge_path(const std::vector<float>& edges, int w, int h, std::pair<float, float> a, std::pair<float, float> b);
// Moving-average smoothing of a polygon outline; `amount` 0..100 sets the window.
std::vector<std::pair<float, float>> smooth_polygon(const std::vector<std::pair<float, float>>& pts, int amount, bool closed);

}  // namespace mask
}  // namespace firn
