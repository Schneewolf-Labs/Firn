#include <algorithm>
#include <cmath>
#include <memory>

#include "App.h"
#include "firn/commands.h"
#include "firn/raster.h"
#include "tools/Tool.h"

using namespace firn;

namespace {

Color to_color(const float* f) {
    auto c = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return {c(f[0]), c(f[1]), c(f[2]), c(f[3])};
}

// --- Pan ---------------------------------------------------------------

class PanTool : public Tool {
public:
    const char* name() const override { return "Pan"; }
    const char* shortcut() const override { return "A"; }
    bool pans_with_left_drag() const override { return true; }
    void draw_options(App&) override { ImGui::TextUnformatted("Drag to pan. Wheel to zoom."); }
};

// --- Zoom --------------------------------------------------------------

class ZoomTool : public Tool {
public:
    const char* name() const override { return "Zoom"; }
    const char* shortcut() const override { return "Z"; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        app.zoom_about(in.screen, b == ImGuiMouseButton_Left ? 1.25f : 1.0f / 1.25f);
    }
    void draw_options(App& app) override {
        ImGui::TextUnformatted("Left click zoom in, right click zoom out.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Fit")) app.fit_requested = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("100%")) { app.zoom = 1.0f; app.pan_x = app.pan_y = 0.0f; }
    }
};

// --- Dropper -----------------------------------------------------------

class DropperTool : public Tool {
public:
    const char* name() const override { return "Dropper"; }
    const char* shortcut() const override { return "E"; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override { pick(app, in, b); }
    void on_drag(App& app, const ToolInput& in, ImGuiMouseButton b) override { pick(app, in, b); }
    void draw_options(App&) override {
        ImGui::TextUnformatted("Left click sets foreground, right click sets background. Samples the merged image.");
    }

private:
    void pick(App& app, const ToolInput& in, ImGuiMouseButton b) {
        if (!in.inside || !app.doc) return;
        const int x = static_cast<int>(std::floor(in.img_x)), y = static_cast<int>(std::floor(in.img_y));
        const Color c = app.doc->composite().get(x, y);
        float* dst = b == ImGuiMouseButton_Left ? app.fg_color : app.bg_color;
        dst[0] = c.r / 255.0f; dst[1] = c.g / 255.0f; dst[2] = c.b / 255.0f; dst[3] = c.a / 255.0f;
    }
};

// --- Paint Brush / Eraser ----------------------------------------------

class BrushTool : public Tool {
public:
    explicit BrushTool(bool eraser) : eraser_(eraser) {}
    const char* name() const override { return eraser_ ? "Eraser" : "Paint Brush"; }
    const char* shortcut() const override { return eraser_ ? "X" : "B"; }

    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!app.doc || app.active_layer() < 0) return;
        Layer& L = app.doc->layer(app.active_layer());
        raster::Brush brush = app.brush;
        // Left paints foreground, right paints background. The eraser
        // on a Background layer paints the background colour instead.
        Color color = to_color(b == ImGuiMouseButton_Left ? app.fg_color : app.bg_color);
        raster::StrokeMode mode = raster::StrokeMode::Paint;
        if (eraser_) {
            if (L.background) color = to_color(b == ImGuiMouseButton_Left ? app.bg_color : app.fg_color);
            else mode = raster::StrokeMode::Erase;
        }
        layer_ = app.active_layer();
        stroke_ = std::make_unique<raster::Stroke>(L.pixels, brush, color, mode);
        stroke_->add_point(in.img_x, in.img_y);
        flush(app);
    }
    void on_drag(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!stroke_) return;
        stroke_->add_point(in.img_x, in.img_y);
        flush(app);
    }
    void on_release(App& app, const ToolInput&, ImGuiMouseButton) override {
        if (!stroke_ || !app.doc) return;
        Layer& L = app.doc->layer(layer_);
        app.commit(std::make_unique<LayerSnapshotCommand>(layer_, name(), stroke_->base(), L.pixels));
        stroke_.reset();
    }
    void cancel(App& app) override {
        if (stroke_ && app.doc && layer_ < app.doc->layer_count()) {
            app.doc->layer(layer_).pixels = stroke_->base();
            app.doc->touch();
        }
        stroke_.reset();
    }

    void draw_overlay(App& app, const ToolInput& in) override {
        const float r = app.brush.size * 0.5f * in.zoom;
        in.dl->AddCircle(in.screen, r, IM_COL32(0, 0, 0, 200), 0, 1.0f);
        in.dl->AddCircle(in.screen, r + 1.0f, IM_COL32(255, 255, 255, 160), 0, 1.0f);
    }
    void draw_options(App& app) override {
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("Size", &app.brush.size, 1.0f, 500.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        float hard = app.brush.hardness * 100.0f;
        if (ImGui::SliderFloat("Hardness", &hard, 0.0f, 100.0f, "%.0f")) app.brush.hardness = hard / 100.0f;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        float op = app.brush.opacity * 100.0f;
        if (ImGui::SliderFloat("Opacity", &op, 1.0f, 100.0f, "%.0f")) app.brush.opacity = op / 100.0f;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        float step = app.brush.step * 100.0f;
        if (ImGui::SliderFloat("Step", &step, 1.0f, 200.0f, "%.0f")) app.brush.step = step / 100.0f;
    }

private:
    void flush(App& app) {
        Layer& L = app.doc->layer(layer_);
        if (!stroke_->render(L.pixels).empty()) app.doc->touch();
    }
    bool eraser_;
    size_t layer_ = 0;
    std::unique_ptr<raster::Stroke> stroke_;
};

// --- Flood Fill --------------------------------------------------------

class FloodFillTool : public Tool {
public:
    const char* name() const override { return "Flood Fill"; }
    const char* shortcut() const override { return "F"; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!in.inside || !app.doc || app.active_layer() < 0) return;
        const size_t layer = app.active_layer();
        Layer& L = app.doc->layer(layer);
        Image before = L.pixels;
        const Color color = to_color(b == ImGuiMouseButton_Left ? app.fg_color : app.bg_color);
        const raster::Rect changed = raster::flood_fill(L.pixels, static_cast<int>(std::floor(in.img_x)),
                                                        static_cast<int>(std::floor(in.img_y)), color,
                                                        app.fill_tolerance, app.fill_opacity);
        if (changed.empty()) return;
        app.doc->touch();
        app.commit(std::make_unique<LayerSnapshotCommand>(layer, name(), std::move(before), L.pixels));
    }
    void draw_options(App& app) override {
        ImGui::SetNextItemWidth(140);
        ImGui::SliderInt("Tolerance", &app.fill_tolerance, 0, 200);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        float op = app.fill_opacity * 100.0f;
        if (ImGui::SliderFloat("Opacity", &op, 1.0f, 100.0f, "%.0f")) app.fill_opacity = op / 100.0f;
    }
};

}  // namespace

std::vector<std::unique_ptr<Tool>> make_default_tools() {
    std::vector<std::unique_ptr<Tool>> t;
    t.push_back(std::make_unique<PanTool>());
    t.push_back(std::make_unique<ZoomTool>());
    t.push_back(std::make_unique<DropperTool>());
    t.push_back(std::make_unique<BrushTool>(false));
    t.push_back(std::make_unique<BrushTool>(true));
    t.push_back(std::make_unique<FloodFillTool>());
    return t;
}
