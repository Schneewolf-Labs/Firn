#pragma once
#include <memory>
#include <vector>

#include "imgui.h"

struct App;

// Where the mouse is this frame, in both spaces the tools care about.
struct ToolInput {
    float img_x = 0, img_y = 0;    // image pixel coords (sub-pixel)
    ImVec2 screen;                 // mouse position
    ImVec2 origin;                 // screen position of image pixel (0,0)
    float zoom = 1.0f;
    bool inside = false;           // cursor over the image rect
    ImDrawList* dl = nullptr;      // canvas draw list, for overlays
};

// Interactive canvas tools are one class per tool, like the original's tool DLLs: each gets press /
// drag / release from the canvas and draws its own Tool Options. Tools edit
// the active layer live and commit a LayerSnapshotCommand on release so the
// History palette and undo see one entry per gesture.
class Tool {
public:
    virtual ~Tool() = default;
    virtual const char* name() const = 0;
    virtual const char* shortcut() const { return nullptr; }

    virtual void on_press(App&, const ToolInput&, ImGuiMouseButton) {}
    virtual void on_drag(App&, const ToolInput&, ImGuiMouseButton) {}
    virtual void on_release(App&, const ToolInput&, ImGuiMouseButton) {}
    virtual void cancel(App&) {}

    // Drawn over the canvas while hovered (brush outline, etc).
    virtual void draw_overlay(App&, const ToolInput&) {}
    virtual void draw_options(App&) {}

    // Left-drag pans instead of reaching the tool.
    virtual bool pans_with_left_drag() const { return false; }
};

std::vector<std::unique_ptr<Tool>> make_default_tools();
