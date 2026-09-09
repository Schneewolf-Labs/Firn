#pragma once
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
};

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

    // For the history panel.
    size_t size() const { return done_.size(); }
    size_t cursor() const { return cursor_; }
    const Command& at(size_t i) const { return *done_.at(i); }

private:
    std::vector<std::unique_ptr<Command>> done_;
    size_t cursor_ = 0;  // entries [0, cursor_) are applied
};

// --- Concrete commands -------------------------------------------------

// Base for commands that rewrite a single layer's pixels. Snapshots the
// layer before executing and confines the result to the document's selection.
// Simple and correct; optimise with dirty rects later.
class LayerPixelCommand : public Command {
public:
    explicit LayerPixelCommand(size_t layer) : layer_(layer) {}
    void execute(Document& doc) override;
    void undo(Document& doc) override;

protected:
    virtual void apply(Image& img) = 0;
    size_t layer_;
    Image before_;
};

class InvertCommand : public LayerPixelCommand {
public:
    using LayerPixelCommand::LayerPixelCommand;
    std::string name() const override { return "Invert"; }
protected:
    void apply(Image& img) override;
};

class FillCommand : public LayerPixelCommand {
public:
    FillCommand(size_t layer, Color c) : LayerPixelCommand(layer), color_(c) {}
    std::string name() const override { return "Fill"; }
protected:
    void apply(Image& img) override;
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

class GreyscaleCommand : public LayerPixelCommand {
public:
    using LayerPixelCommand::LayerPixelCommand;
    std::string name() const override { return "Greyscale"; }
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

// Edit > Clear: fills the selection (or the whole layer) with a colour.
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
};

// Inserts a new raster layer holding `pixels` (document-sized) above the
// active layer. Used by Paste As New Layer.
class PasteLayerCommand : public Command {
public:
    PasteLayerCommand(std::string layer_name, Image pixels)
        : layer_name_(std::move(layer_name)), pixels_(std::move(pixels)) {}
    std::string name() const override { return "Paste As New Layer"; }
    void execute(Document& doc) override;
    void undo(Document& doc) override;
private:
    std::string layer_name_;
    Image pixels_;
    size_t index_ = 0;
    int prev_active_ = -1;
};

// Records an edit made live by a tool (brush stroke, flood fill). The tool
// snapshots the layer before it starts and hands both images over on commit.
class LayerSnapshotCommand : public Command {
public:
    LayerSnapshotCommand(size_t layer, std::string name, Image before, Image after)
        : layer_(layer), name_(std::move(name)), before_(std::move(before)), after_(std::move(after)) {}
    std::string name() const override { return name_; }
    void execute(Document& doc) override;
    void undo(Document& doc) override;
private:
    size_t layer_;
    std::string name_;
    Image before_, after_;
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

class AddLayerCommand : public Command {
public:
    explicit AddLayerCommand(std::string name) : name_(std::move(name)) {}
    std::string name() const override { return "Add Layer"; }
    void execute(Document& doc) override;
    void undo(Document& doc) override;
private:
    std::string name_;
    size_t index_ = 0;
};

class LayerPropertiesCommand : public Command {
public:
    // `before` is explicit because palettes preview the change live before committing.
    LayerPropertiesCommand(size_t index, LayerProps before, LayerProps after)
        : index_(index), before_(std::move(before)), after_(std::move(after)) {}
    std::string name() const override { return "Layer Properties"; }
    void execute(Document& doc) override { doc.set_props(index_, after_); }
    void undo(Document& doc) override { doc.set_props(index_, before_); }
private:
    size_t index_;
    LayerProps before_, after_;
};

class DuplicateLayerCommand : public Command {
public:
    explicit DuplicateLayerCommand(size_t index) : index_(index) {}
    std::string name() const override { return "Duplicate Layer"; }
    void execute(Document& doc) override;
    void undo(Document& doc) override;
private:
    size_t index_;
};

class ArrangeLayerCommand : public Command {
public:
    ArrangeLayerCommand(size_t from, size_t to) : from_(from), to_(to) {}
    std::string name() const override { return "Arrange Layer"; }
    void execute(Document& doc) override { doc.move_layer(from_, to_); }
    void undo(Document& doc) override { doc.move_layer(to_, from_); }
private:
    size_t from_, to_;
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
class RotateCommand : public GeometryCommand {
public:
    RotateCommand(float degrees, Color fill) : degrees_(degrees), fill_(fill) {}
    std::string name() const override { return "Rotate"; }
protected:
    void transform(const Document::State& in, Document::State& out) override;
    float degrees_;
    Color fill_;
};

class RemoveLayerCommand : public Command {
public:
    explicit RemoveLayerCommand(size_t index) : index_(index) {}
    std::string name() const override { return "Remove Layer"; }
    void execute(Document& doc) override;
    void undo(Document& doc) override;
private:
    size_t index_;
    std::unique_ptr<Layer> removed_;
};

}  // namespace firn
