#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "firn/document.h"

namespace firn {

// Every mutation of a Document goes through a Command so that undo/redo,
// the history panel, and scripting all see the same thing.
class Command {
public:
    virtual ~Command() = default;
    virtual std::string name() const = 0;
    virtual void execute(Document& doc) = 0;
    virtual void undo(Document& doc) = 0;
    // Approximate bytes of pixel data this entry keeps alive, for the undo budget.
    virtual size_t memory_bytes() const { return 0; }
};

// Bytes held by a document snapshot (layers, deep data, masks, selection).
size_t state_bytes(const Document::State& s);

class CommandStack {
public:
    void run(Document& doc, std::unique_ptr<Command> cmd);
    // Record a command whose effect has already been applied to the document
    // (interactive tools paint live, then commit). Only undo/redo call into it.
    void push_applied(std::unique_ptr<Command> cmd);
    bool can_undo() const { return cursor_ > 0; }
    bool can_redo() const { return cursor_ < done_.size(); }
    void undo(Document& doc);
    void redo(Document& doc);
    void clear();
    // Oldest entries are dropped beyond this many (0 = unlimited), or once
    // the entries' pixel snapshots exceed the byte budget (0 = unlimited).
    void set_limit(size_t n) { limit_ = n; trim(); }
    size_t limit() const { return limit_; }
    void set_memory_limit(size_t bytes) { memory_limit_ = bytes; trim(); }
    size_t memory_bytes() const;

    // For the history panel.
    size_t size() const { return done_.size(); }
    size_t cursor() const { return cursor_; }
    // Identifies content, unlike cursor(), which can repeat after branching or trimming.
    uint64_t state_id() const { return states_.at(cursor_); }
    const Command& at(size_t i) const { return *done_.at(i); }

private:
    void trim();
    std::vector<std::unique_ptr<Command>> done_;
    std::vector<uint64_t> states_{0}; // one identity for each undo boundary
    uint64_t next_state_ = 1;
    size_t cursor_ = 0;  // entries [0, cursor_) are applied
    size_t limit_ = 0;
    size_t memory_limit_ = 0;
};

// --- Concrete commands -------------------------------------------------

// Base for commands that rewrite a single layer's pixels. Snapshots the
// layer before executing and confines the result to the document's selection.
// Simple and correct; optimize with dirty rects later.
class LayerPixelCommand : public Command {
public:
    explicit LayerPixelCommand(size_t layer) : layer_(layer) {}
    void execute(Document& doc) override;
    void undo(Document& doc) override;

protected:
    virtual void apply(Image& img) = 0;
    // 16-bit layers: return true after applying to the deep copy; false
    // (the default) drops the layer to 8 bits first, undoably.
    virtual bool apply16(Image16&) { return false; }
    size_t layer_;
    Image before_;
    std::shared_ptr<const Image16> before_deep_;
public:
    size_t memory_bytes() const override { return before_.size_bytes() + (before_deep_ ? before_deep_->size() * 2 : 0); }
};

class InvertCommand : public LayerPixelCommand {
public:
    using LayerPixelCommand::LayerPixelCommand;
    std::string name() const override { return "Invert"; }
protected:
    bool apply16(Image16& img) override;
    void apply(Image& img) override;
};

class FillCommand : public LayerPixelCommand {
public:
    FillCommand(size_t layer, Color c) : LayerPixelCommand(layer), color_(c) {}
    std::string name() const override { return "Fill"; }
protected:
    void apply(Image& img) override;
    bool apply16(Image16& img) override;
    Color color_;
};

class BoxBlurCommand : public LayerPixelCommand {
public:
    BoxBlurCommand(size_t layer, int radius) : LayerPixelCommand(layer), radius_(radius) {}
    std::string name() const override { return "Box Blur"; }
protected:
    void apply(Image& img) override;
    int radius_;
};

// Any in-place pixel function as an undoable, selection-clipped command.
class AdjustCommand : public LayerPixelCommand {
public:
    AdjustCommand(size_t layer, std::string name, std::function<void(Image&)> fn, std::function<void(Image16&)> fn16 = nullptr)
        : LayerPixelCommand(layer), name_(std::move(name)), fn_(std::move(fn)), fn16_(std::move(fn16)) {}
    std::string name() const override { return name_; }
protected:
    void apply(Image& img) override { fn_(img); }
    bool apply16(Image16& img) override { if (!fn16_) return false; fn16_(img); return true; }
    std::string name_;
    std::function<void(Image&)> fn_;
    std::function<void(Image16&)> fn16_;
};

class GrayscaleCommand : public LayerPixelCommand {
public:
    using LayerPixelCommand::LayerPixelCommand;
    std::string name() const override { return "Grayscale"; }
protected:
    void apply(Image& img) override;
};

class BrightnessContrastCommand : public LayerPixelCommand {
public:
    BrightnessContrastCommand(size_t layer, int brightness, int contrast)
        : LayerPixelCommand(layer), brightness_(brightness), contrast_(contrast) {}
    std::string name() const override { return "Brightness/Contrast"; }
protected:
    void apply(Image& img) override;
    int brightness_, contrast_;
};

class GaussianBlurCommand : public LayerPixelCommand {
public:
    GaussianBlurCommand(size_t layer, float radius) : LayerPixelCommand(layer), radius_(radius) {}
    std::string name() const override { return "Gaussian Blur"; }
protected:
    void apply(Image& img) override;
    float radius_;
};

// Edit > Clear: fills the selection (or the whole layer) with a color.
class ClearCommand : public LayerPixelCommand {
public:
    ClearCommand(size_t layer, Color c) : LayerPixelCommand(layer), color_(c) {}
    std::string name() const override { return "Clear"; }
protected:
    void apply(Image& img) override { img.fill(color_); }
    Color color_;
};

// Any change to the selection mask. Named after the gesture that made it so
// the History palette reads like the original's.
class SelectionCommand : public Command {
public:
    SelectionCommand(std::string name, Mask after) : name_(std::move(name)), after_(std::move(after)) {}
    std::string name() const override { return name_; }
    void execute(Document& doc) override;
    void undo(Document& doc) override;
private:
    std::string name_;
    Mask before_, after_;
public:
    size_t memory_bytes() const override { return before_.size() + after_.size(); }
};

// Saved selections. They are part of the document and of its undo state, so
// adding or removing one has to go through a command like every other
// mutation: without this, saving a selection did not even mark the file
// modified, and deleting them all could not be undone.
class AlphaChannelCommand : public Command {
public:
    AlphaChannelCommand(std::string name, std::vector<Document::AlphaChannel> after)
        : name_(std::move(name)), after_(std::move(after)) {}
    std::string name() const override { return name_; }
    void execute(Document& doc) override;
    void undo(Document& doc) override;
    size_t memory_bytes() const override {
        size_t n = 0;
        for (const auto& c : before_) n += c.mask.size();
        for (const auto& c : after_) n += c.mask.size();
        return n;
    }
private:
    std::string name_;
    std::vector<Document::AlphaChannel> before_, after_;
};

// Inserts a new raster layer holding `pixels` (document-sized) above the
// active layer. Used by Paste As New Layer.
class PasteLayerCommand : public Command {
public:
    // `history` names the step when the layer did not arrive from the
    // clipboard: a generated layer in the History palette should say what
    // made it, not "Paste As New Layer".
    PasteLayerCommand(std::string layer_name, Image pixels, bool floating = false, std::string history = {})
        : layer_name_(std::move(layer_name)), pixels_(std::move(pixels)), floating_(floating), history_(std::move(history)) {}
    std::string name() const override { return !history_.empty() ? history_ : floating_ ? "Float" : "Paste As New Layer"; }
    void execute(Document& doc) override;
    void undo(Document& doc) override;
    size_t memory_bytes() const override { return pixels_.size_bytes(); }
private:
    std::string layer_name_;
    Image pixels_;
    bool floating_ = false;
    std::string history_;
    size_t index_ = 0;
    int prev_active_ = -1;
};

// Records an edit made live by a tool (brush stroke, flood fill). The tool
// snapshots the layer before it starts and hands both images over on commit.
// Tools paint at 8 bits: the layer's deep data is dropped, undoably.
class LayerSnapshotCommand : public Command {
public:
    // Keeps only the rectangle where `before` and `after` differ.
    LayerSnapshotCommand(size_t layer, std::string name, Image before, Image after);
    std::string name() const override { return name_; }
    void execute(Document& doc) override;
    void undo(Document& doc) override;
    // For edits already applied live: remembers and drops the layer's deep data.
    void capture_deep(Document& doc);
    size_t memory_bytes() const override { return before_.size_bytes() + after_.size_bytes() + (before_deep_ ? before_deep_->size() * 2 : 0); }
    const raster::Rect& rect() const { return rect_; }
private:
    size_t layer_;
    std::string name_;
    raster::Rect rect_;          // the changed area; before_/after_ are crops of it
    Image before_, after_;
    std::shared_ptr<const Image16> before_deep_;
};

// Whole-document geometry that keeps the canvas size. These are involutions,
// so undo just re-applies.
class FlipCommand : public Command {
public:
    std::string name() const override { return "Flip"; }
    void execute(Document& doc) override;
    void undo(Document& doc) override { execute(doc); }
};

class MirrorCommand : public Command {
public:
    std::string name() const override { return "Mirror"; }
    void execute(Document& doc) override;
    void undo(Document& doc) override { execute(doc); }
};

// Adds an empty raster layer directly above the active layer (inside its
// group), or on top of the stack when nothing is active.
class AddLayerCommand : public Command {
public:
    explicit AddLayerCommand(std::string name) : name_(std::move(name)) {}
    std::string name() const override { return "Add Layer"; }
    void execute(Document& doc) override;
    void undo(Document& doc) override;
private:
    std::string name_;
    size_t index_ = 0;
    int prev_active_ = -1;
};

// Wraps the active layer (or a group block) in a new group.
class NewLayerGroupCommand : public Command {
public:
    explicit NewLayerGroupCommand(size_t index) : index_(index) {}
    std::string name() const override { return "New Layer Group"; }
    void execute(Document& doc) override;
    void undo(Document& doc) override { doc.restore(before_); }
private:
    size_t index_;
    Document::State before_;
public:
    size_t memory_bytes() const override { return state_bytes(before_); }
private:
};

// Removes a group layer, promoting its members one level.
class UngroupCommand : public Command {
public:
    explicit UngroupCommand(size_t index) : index_(index) {}
    std::string name() const override { return "Ungroup Layers"; }
    void execute(Document& doc) override;
    void undo(Document& doc) override { doc.restore(before_); }
private:
    size_t index_;
    Document::State before_;
public:
    size_t memory_bytes() const override { return state_bytes(before_); }
private:
};

// Any change to a vector layer's objects: snapshots the object list.
class VectorEditCommand : public Command {
public:
    VectorEditCommand(size_t layer, std::string name, std::vector<vec::Object> after)
        : layer_(layer), name_(std::move(name)), after_(std::move(after)) {}
    // For edits already applied live: records both states, nothing is re-run.
    VectorEditCommand(size_t layer, std::string name, std::vector<vec::Object> before, std::vector<vec::Object> after)
        : layer_(layer), name_(std::move(name)), before_(std::move(before)), after_(std::move(after)), has_before_(true) {}
    std::string name() const override { return name_; }
    void execute(Document& doc) override;
    void undo(Document& doc) override;
private:
    size_t layer_;
    std::string name_;
    std::vector<vec::Object> before_, after_;
    bool has_before_ = false;
};

// Adds an empty vector layer above the active layer.
class AddVectorLayerCommand : public Command {
public:
    explicit AddVectorLayerCommand(std::string name) : name_(std::move(name)) {}
    std::string name() const override { return "New Vector Layer"; }
    void execute(Document& doc) override;
    void undo(Document& doc) override;
private:
    std::string name_;
    size_t index_ = 0;
    int prev_active_ = -1;
};

// Adds an adjustment layer above the active layer.
class AddAdjustmentLayerCommand : public Command {
public:
    AddAdjustmentLayerCommand(std::string name, Adjustment adj) : name_(std::move(name)), adj_(std::move(adj)) {}
    std::string name() const override { return "New Adjustment Layer"; }
    void execute(Document& doc) override;
    void undo(Document& doc) override;
    size_t index() const { return index_; }
private:
    std::string name_;
    Adjustment adj_;
    size_t index_ = 0;
    int prev_active_ = -1;
};

// Changes an adjustment layer's parameters (already applied live or not).
class SetAdjustmentCommand : public Command {
public:
    SetAdjustmentCommand(size_t layer, Adjustment before, Adjustment after) : layer_(layer), before_(std::move(before)), after_(std::move(after)) {}
    std::string name() const override { return std::string(Adjustment::kind_name(after_.kind)) + " Layer"; }
    void execute(Document& doc) override;
    void undo(Document& doc) override;
private:
    size_t layer_;
    Adjustment before_, after_;
};

// Turns a vector layer into a raster layer holding its rendering.
// Layers > Layer Styles: replaces a layer's style.
class SetLayerStyleCommand : public Command {
public:
    SetLayerStyleCommand(size_t layer, LayerStyle before, LayerStyle after) : layer_(layer), before_(before), after_(after) {}
    std::string name() const override { return "Layer Styles"; }
    void execute(Document& doc) override { if (layer_ < doc.layer_count()) { doc.layer(layer_).style = after_; doc.touch(); } }
    void undo(Document& doc) override { if (layer_ < doc.layer_count()) { doc.layer(layer_).style = before_; doc.touch(); } }
private:
    size_t layer_;
    LayerStyle before_, after_;
};

class ConvertToRasterCommand : public Command {
public:
    explicit ConvertToRasterCommand(size_t index) : index_(index) {}
    std::string name() const override { return "Convert to Raster Layer"; }
    void execute(Document& doc) override;
    void undo(Document& doc) override { doc.restore(before_); }
private:
    size_t index_;
    Document::State before_;
public:
    size_t memory_bytes() const override { return state_bytes(before_); }
private:
};

// Sets (or clears, with an empty mask) a layer's mask.
class SetMaskCommand : public Command {
public:
    SetMaskCommand(size_t index, std::string name, Mask mask, bool enabled = true)
        : index_(index), name_(std::move(name)), after_(std::move(mask)), enabled_(enabled) {}
    std::string name() const override { return name_; }
    void execute(Document& doc) override;
    void undo(Document& doc) override;
private:
    size_t index_;
    std::string name_;
    Mask before_, after_;
    bool before_enabled_ = true, enabled_;
};

class LayerPropertiesCommand : public Command {
public:
    // `before` is explicit because palettes preview the change live before committing.
    LayerPropertiesCommand(size_t index, LayerProps before, LayerProps after)
        : index_(index), before_(std::move(before)), after_(std::move(after)) {}
    // Say which property changed, so a run of these in the History palette
    // can be told apart. "Layer Properties" twelve times over tells nobody
    // which one was the visibility flick and which was the rename.
    std::string name() const override {
        if (before_.name != after_.name) return "Rename to " + after_.name;
        if (before_.visible != after_.visible) return (after_.visible ? "Show " : "Hide ") + after_.name;
        if (before_.opacity != after_.opacity) return "Opacity " + std::to_string(static_cast<int>(after_.opacity * 100.0f + 0.5f)) + "%";
        if (before_.blend != after_.blend) return std::string("Blend: ") + blend_mode_name(after_.blend);
        if (before_.clipped != after_.clipped) return after_.clipped ? "Create Clipping Mask" : "Release Clipping Mask";
        if (before_.pass_through != after_.pass_through) return after_.pass_through ? "Pass Through On" : "Pass Through Off";
        if (before_.lock_alpha != after_.lock_alpha) return after_.lock_alpha ? "Lock Transparency" : "Unlock Transparency";
        return "Layer Properties";
    }
    void execute(Document& doc) override { doc.set_props(index_, after_); }
    void undo(Document& doc) override { doc.set_props(index_, before_); }
private:
    size_t index_;
    LayerProps before_, after_;
};

// Duplicates a layer or a whole group block directly above the original.
class DuplicateLayerCommand : public Command {
public:
    explicit DuplicateLayerCommand(size_t index) : index_(index) {}
    std::string name() const override { return "Duplicate Layer"; }
    void execute(Document& doc) override;
    void undo(Document& doc) override { doc.restore(before_); }
private:
    size_t index_;
    Document::State before_;
public:
    size_t memory_bytes() const override { return state_bytes(before_); }
private:
};

// Moves a layer (or group block) past its neighboring sibling: +1 up
// towards the top, -1 down; large steps go to the end of the siblings.
// Any layer-stack restructuring expressed as a function on the document;
// undo restores the whole state.
class StateEditCommand : public Command {
public:
    StateEditCommand(std::string name, std::function<void(Document&)> fn) : name_(std::move(name)), fn_(std::move(fn)) {}
    std::string name() const override { return name_; }
    void execute(Document& doc) override { before_ = doc.snapshot(); fn_(doc); doc.touch(); }
    void undo(Document& doc) override { doc.restore(before_); }
private:
    std::string name_;
    std::function<void(Document&)> fn_;
    Document::State before_;
public:
    size_t memory_bytes() const override { return state_bytes(before_); }
private:
};

// Edits the image's Exif and text metadata. Cheap to undo: it holds two
// metadata lists rather than a snapshot of every layer.
class MetadataCommand : public Command {
public:
    MetadataCommand(std::string name, meta::Metadata after) : name_(std::move(name)), after_(std::move(after)) {}
    std::string name() const override { return name_; }
    void execute(Document& doc) override { before_ = doc.metadata(); doc.set_metadata(after_); }
    void undo(Document& doc) override { doc.set_metadata(before_); }
    size_t memory_bytes() const override {
        size_t n = sizeof(*this);
        for (const meta::Metadata* m : {&before_, &after_})
            for (const meta::Entry& e : m->entries) n += e.value.size() + e.key.size() + sizeof(meta::Entry);
        return n;
    }
private:
    std::string name_;
    meta::Metadata before_, after_;
};

// Several commands as one history entry: executed in order, undone in reverse.
class CompoundCommand : public Command {
public:
    CompoundCommand(std::string name, std::vector<std::unique_ptr<Command>> parts) : name_(std::move(name)), parts_(std::move(parts)) {}
    std::string name() const override { return name_; }
    void execute(Document& doc) override { for (auto& c : parts_) c->execute(doc); }
    void undo(Document& doc) override { for (auto it = parts_.rbegin(); it != parts_.rend(); ++it) (*it)->undo(doc); }
    size_t memory_bytes() const override { size_t n = 0; for (const auto& c : parts_) n += c->memory_bytes(); return n; }
private:
    std::string name_;
    std::vector<std::unique_ptr<Command>> parts_;
};

// Drag and drop in the Layers palette: moves the layer at `index` (with its
// members, when it is a group) so it sits directly above the stack position
// `before`, at depth `depth`. Refuses to move a group into itself.
class MoveLayerCommand : public Command {
public:
    MoveLayerCommand(size_t index, size_t before, int depth) : index_(index), before_pos_(before), depth_(depth) {}
    std::string name() const override { return "Move Layer"; }
    void execute(Document& doc) override;
    void undo(Document& doc) override { doc.restore(before_); }
    size_t memory_bytes() const override { return state_bytes(before_); }
private:
    size_t index_, before_pos_;
    int depth_;
    Document::State before_;
};

class ArrangeLayerCommand : public Command {
public:
    ArrangeLayerCommand(size_t index, int steps) : index_(index), steps_(steps) {}
    std::string name() const override { return "Arrange Layer"; }
    void execute(Document& doc) override;
    void undo(Document& doc) override { doc.restore(before_); }
private:
    size_t index_;
    int steps_;
    Document::State before_;
public:
    size_t memory_bytes() const override { return state_bytes(before_); }
private:
};

class PromoteBackgroundCommand : public Command {
public:
    explicit PromoteBackgroundCommand(size_t index) : index_(index) {}
    std::string name() const override { return "Promote Background Layer"; }
    void execute(Document& doc) override;
    void undo(Document& doc) override;
private:
    size_t index_;
    std::string old_name_;
};

// Merge Down / Merge Visible / Merge All (Flatten). Snapshots the whole
// stack for undo; merges are rare enough that the memory is acceptable.
class MergeLayersCommand : public Command {
public:
    enum class Kind { Down, Visible, All };
    MergeLayersCommand(Kind kind, size_t index = 0) : kind_(kind), index_(index) {}
    std::string name() const override;
    void execute(Document& doc) override;
    void undo(Document& doc) override { doc.replace_layers(before_, before_active_); }
private:
    Kind kind_;
    size_t index_;
    std::vector<Layer> before_;
    int before_active_ = -1;
};

// Base for commands that change the canvas size: snapshots the whole
// document for undo and rebuilds every layer (and the selection) through
// transform().
class GeometryCommand : public Command {
public:
    void execute(Document& doc) override;
    void undo(Document& doc) override { doc.restore(before_); }
protected:
    // Produces the new state from the old one. Layers in `out` must all be
    // out.width x out.height.
    virtual void transform(const Document::State& in, Document::State& out) = 0;
    Document::State before_;
public:
    size_t memory_bytes() const override { return state_bytes(before_); }
private:
};

class CropCommand : public GeometryCommand {
public:
    explicit CropCommand(raster::Rect r) : rect_(r) {}
    std::string name() const override { return "Crop"; }
protected:
    void transform(const Document::State& in, Document::State& out) override;
    raster::Rect rect_;
};

class ResizeCommand : public GeometryCommand {
public:
    ResizeCommand(int w, int h, raster::Filter f) : w_(w), h_(h), filter_(f) {}
    std::string name() const override { return "Resize"; }
protected:
    void transform(const Document::State& in, Document::State& out) override;
    int w_, h_;
    raster::Filter filter_;
};

// New canvas of (w, h); the old content is placed at (offset_x, offset_y).
// Background layers are padded with `fill`, others with transparency.
class CanvasSizeCommand : public GeometryCommand {
public:
    CanvasSizeCommand(int w, int h, int offset_x, int offset_y, Color fill)
        : w_(w), h_(h), ox_(offset_x), oy_(offset_y), fill_(fill) {}
    std::string name() const override { return "Canvas Size"; }
protected:
    void transform(const Document::State& in, Document::State& out) override;
    int w_, h_, ox_, oy_;
    Color fill_;
};

// Clockwise degrees. Multiples of 90 are exact; anything else expands the
// canvas and fills Background layers with `fill`.
// Warps every raster layer (or one) by a homography, keeping the canvas
// size, optionally cropping afterwards. Straighten and Perspective
// Correction use it.
class WarpLayersCommand : public GeometryCommand {
public:
    WarpLayersCommand(std::string name, const float H[9], int layer /* -1 = all */, raster::Rect crop, Color fill)
        : name_(std::move(name)), layer_(layer), crop_(crop), fill_(fill) { for (int i = 0; i < 9; ++i) H_[i] = H[i]; }
    std::string name() const override { return name_; }
protected:
    void transform(const Document::State& in, Document::State& out) override;
    std::string name_;
    float H_[9];
    int layer_;
    raster::Rect crop_;
    Color fill_;
};

class RotateCommand : public GeometryCommand {
public:
    RotateCommand(float degrees, Color fill) : degrees_(degrees), fill_(fill) {}
    std::string name() const override { return "Rotate"; }
protected:
    void transform(const Document::State& in, Document::State& out) override;
    float degrees_;
    Color fill_;
};

// Removes a layer, or a group with all its members.
class RemoveLayerCommand : public Command {
public:
    explicit RemoveLayerCommand(size_t index) : index_(index) {}
    std::string name() const override { return "Remove Layer"; }
    void execute(Document& doc) override;
    void undo(Document& doc) override { doc.restore(before_); }
private:
    size_t index_;
    Document::State before_;
public:
    size_t memory_bytes() const override { return state_bytes(before_); }
private:
};

}  // namespace firn
