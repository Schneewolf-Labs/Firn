#pragma once
#include <memory>
#include <string>
#include <vector>

#include "psp9/document.h"

namespace psp9 {

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
// layer before executing. Simple and correct; optimise with dirty rects later.
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

}  // namespace psp9
