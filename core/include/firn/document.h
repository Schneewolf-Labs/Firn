#pragma once
#include <memory>
#include <string>
#include <vector>

#include "firn/blend.h"
#include "firn/image.h"
#include "firn/mask.h"

namespace firn {

enum class LayerType : uint8_t { Raster, Group };

struct Layer {
    std::string name;
    LayerType type = LayerType::Raster;
    // Groups: a group layer sits below its members in the stack; its members
    // are the run of layers immediately above it with a greater depth. A
    // group composites its members together, then blends that result with
    // its own opacity, blend mode and mask.
    int depth = 0;
    bool expanded = true;   // palette state only
    bool visible = true;
    // A Background layer has no transparency: the eraser paints the
    // background color on it instead of clearing alpha.
    bool background = false;
    float opacity = 1.0f;  // 0..1
    BlendMode blend = BlendMode::Normal;
    // Optional document-sized mask (0 hides, 255 shows); empty = none.
    Mask mask;
    bool mask_enabled = true;
    Image pixels;           // empty for groups
    bool is_raster() const { return type == LayerType::Raster; }
    bool has_mask() const { return !mask.empty(); }
};

// The undoable subset of Layer, for LayerPropertiesCommand.
struct LayerProps {
    std::string name;
    bool visible = true;
    float opacity = 1.0f;
    BlendMode blend = BlendMode::Normal;
    bool operator==(const LayerProps&) const = default;
};

// The single source of truth. The UI and scripts both read this and
// mutate it only through Commands (see commands.h).
class Document {
public:
    // Saved selections ("alpha channels" in the original): named masks that
    // travel with the file.
    struct AlphaChannel { std::string name; Mask mask; };

    Document(int width, int height);

    int width() const { return width_; }
    int height() const { return height_; }

    size_t layer_count() const { return layers_.size(); }
    Layer& layer(size_t i) { return *layers_.at(i); }
    const Layer& layer(size_t i) const { return *layers_.at(i); }

    int active_layer() const { return active_; }
    void set_active_layer(int i);

    // Layer ordering: index 0 is the bottom of the stack.
    Layer& add_layer(std::string name, int at = -1);
    // Group helpers: one past the last member (descendants included), and
    // the index of the group a layer belongs to (-1 at top level).
    size_t group_end(size_t group_index) const;
    int parent_group(size_t index) const;
    std::unique_ptr<Layer> remove_layer(size_t i);
    void insert_layer(std::unique_ptr<Layer> layer, size_t at);
    void move_layer(size_t from, size_t to);

    // Whole-stack snapshot/restore, for commands that restructure many layers.
    std::vector<Layer> clone_layers() const;
    void replace_layers(const std::vector<Layer>& layers, int active);

    // Everything at once, for commands that change the canvas size. Layers
    // must already be (width x height).
    struct State {
        int width = 0, height = 0;
        std::vector<Layer> layers;
        int active = -1;
        Mask selection;
        std::vector<AlphaChannel> alpha;
    };
    State snapshot() const;
    void restore(const State& s);

    // Flatten visible layers with their blend modes into one image.
    Image composite() const;
    // Flatten layers [from, to] (inclusive, bottom to top) honoring
    // visibility, groups and masks. Members of a group must be included
    // with their group for the group's opacity and mask to apply.
    Image composite_range(size_t from, size_t to) const;

    LayerProps props(size_t i) const;
    void set_props(size_t i, const LayerProps& p);

    std::vector<AlphaChannel>& alpha_channels() { return alpha_; }
    const std::vector<AlphaChannel>& alpha_channels() const { return alpha_; }

    // Selection: an empty mask means none. Commands and tools clip to it.
    const Mask& selection() const { return selection_; }
    bool has_selection() const { return !selection_.empty(); }
    void set_selection(Mask m);
    uint64_t selection_revision() const { return selection_revision_; }

    // Bumped on every mutation; the UI uses it to know when to re-upload.
    uint64_t revision() const { return revision_; }
    void touch() { ++revision_; dirty_ = {0, 0, width_, height_}; }
    // Mutation confined to a rect: lets the display recomposite only that area.
    void touch(const raster::Rect& r) { ++revision_; dirty_ = dirty_.empty() ? r.clipped(width_, height_) : dirty_.united(r.clipped(width_, height_)); }
    // Area changed since the last take_dirty(); the whole image after touch().
    raster::Rect take_dirty() { raster::Rect r = dirty_; dirty_ = {}; return r; }

    // Composites only `r` of the document into `dst` (document-sized).
    void composite_into(Image& dst, const raster::Rect& r) const;

private:
    void composite_region(Image& out, size_t from, size_t to, const raster::Rect& r) const;
    int width_;
    int height_;
    std::vector<std::unique_ptr<Layer>> layers_;
    int active_ = -1;
    uint64_t revision_ = 0;
    Mask selection_;
    uint64_t selection_revision_ = 0;
    raster::Rect dirty_;
    std::vector<AlphaChannel> alpha_;
};

}  // namespace firn
