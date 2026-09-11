#pragma once
#include <memory>
#include <string>
#include <vector>

#include "firn/blend.h"
#include "firn/image.h"
#include "firn/mask.h"
#include "firn/metadata.h"
#include "firn/adjustment.h"
#include "firn/layerstyle.h"
#include "firn/vector.h"

namespace firn {

enum class LayerType : uint8_t { Raster, Group, Vector, Adjustment };

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
    bool floating = false;    // a floating selection (Selections > Float); not saved
    float opacity = 1.0f;  // 0..1
    BlendMode blend = BlendMode::Normal;
    // Optional document-sized mask (0 hides, 255 shows); empty = none.
    Mask mask;
    bool mask_enabled = true;
    Image pixels;           // empty for groups; the rendered cache for vector layers
    // 16-bit layers keep their true pixels here; `pixels` is derived from it
    // for display. Shared between snapshots, so never modify in place:
    // replace it with a new Image16 (see set_deep).
    std::shared_ptr<const Image16> deep;
    bool is_deep() const { return deep != nullptr; }
    void set_deep(Image16 img) { pixels = to_image8(img); deep = std::make_shared<const Image16>(std::move(img)); }
    std::vector<vec::Object> objects;  // vector layers only
    Adjustment adjustment;             // adjustment layers only; its mask limits where it applies
    LayerStyle style;                  // raster and vector layers: effects rendered at composite time
    bool is_raster() const { return type == LayerType::Raster; }
    bool is_vector() const { return type == LayerType::Vector; }
    bool is_adjustment() const { return type == LayerType::Adjustment; }
    bool has_mask() const { return !mask.empty(); }
};

// Painting assistants: guides for the brushes, kept with the image the way
// ruler guides are. A vanishing point makes strokes run toward it, a
// parallel ruler gives them its direction, a ruler holds them on its line.
struct Assistant {
    enum class Kind { VanishingPoint, Parallel, Ruler };
    Kind kind = Kind::VanishingPoint;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // image coords; the point is (x0, y0)
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
        std::vector<uint8_t> icc;
        meta::Metadata metadata;
    };
    // Embedded ICC profile (empty = untagged, treated as sRGB).
    const std::vector<uint8_t>& icc() const { return icc_; }
    void set_icc(std::vector<uint8_t> bytes) { icc_ = std::move(bytes); ++revision_; }
    // Exif and text metadata carried from the file it was loaded from.
    const meta::Metadata& metadata() const { return metadata_; }
    meta::Metadata& metadata() { return metadata_; }
    void set_metadata(meta::Metadata md) { metadata_ = std::move(md); ++revision_; }
    State snapshot() const;
    void restore(const State& s);

    // Flatten visible layers with their blend modes into one image.
    Image composite() const;
    // 16-bit flatten: exact when every visible layer is a plain Normal raster
    // layer without a mask; otherwise the 8-bit composite widened.
    Image16 composite16() const;
    // Flatten layers [from, to] (inclusive, bottom to top) honoring
    // visibility, groups and masks. Members of a group must be included
    // with their group for the group's opacity and mask to apply.
    Image composite_range(size_t from, size_t to) const;

    LayerProps props(size_t i) const;
    void set_props(size_t i, const LayerProps& p);

    // Re-renders a vector layer's objects into its pixel cache.
    void rasterize_vector_layer(size_t i);
    // Bits per channel: 16 when any raster layer carries deep data.
    int bit_depth() const;
    void set_bit_depth(int bits);   // 16 promotes every raster layer, 8 drops the deep data

    // View aids that belong to the image rather than the window: ruler
    // guides (image-space y and x positions) and painting assistants. Not
    // part of the undo state; the project format saves them.
    std::vector<float>& guides_h() { return guides_h_; }
    const std::vector<float>& guides_h() const { return guides_h_; }
    std::vector<float>& guides_v() { return guides_v_; }
    const std::vector<float>& guides_v() const { return guides_v_; }
    std::vector<Assistant>& assistants() { return assistants_; }
    const std::vector<Assistant>& assistants() const { return assistants_; }

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
    void touch(const raster::Rect& r);
    // Pixels a filter layer spreads an edit by (the sum over visible filter layers).
    int filter_reach() const;
    // Area changed since the last take_dirty(); the whole image after touch().
    raster::Rect take_dirty() { raster::Rect r = dirty_; dirty_ = {}; return r; }

    // Composites only `r` of the document into `dst` (document-sized).
    void composite_into(Image& dst, const raster::Rect& r) const;

private:
    // Composites layers [from, to] over `r` (document coordinates) into
    // `out`, whose pixel (0, 0) sits at document (ox, oy).
    void composite_region(Image& out, int ox, int oy, size_t from, size_t to, const raster::Rect& r) const;
    void apply_filter_layer(Image& out, int ox, int oy, size_t from, size_t li, const raster::Rect& r) const;
    int width_;
    int height_;
    std::vector<std::unique_ptr<Layer>> layers_;
    int active_ = -1;
    uint64_t revision_ = 0;
    Mask selection_;
    uint64_t selection_revision_ = 0;
    raster::Rect dirty_;
    std::vector<AlphaChannel> alpha_;
    std::vector<float> guides_h_, guides_v_;
    std::vector<Assistant> assistants_;
    std::vector<uint8_t> icc_;
    meta::Metadata metadata_;
};

}  // namespace firn
