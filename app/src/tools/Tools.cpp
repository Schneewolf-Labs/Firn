#include <algorithm>
#include <cmath>
#include <memory>

#include "App.h"
#include "firn/commands.h"
#include "firn/mask.h"
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
        stroke_ = std::make_unique<raster::Stroke>(L.pixels, brush, color, mode, &app.doc->selection());
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
                                                        app.fill_tolerance, app.fill_opacity, &app.doc->selection());
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

// --- Selection tools ---------------------------------------------------

// Options shared by the shape and freehand tools: mode, feather, antialias.
void draw_selection_common(App& app) {
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("Mode", &app.sel_mode, "Replace\0Add (Shift)\0Remove (Ctrl)\0Intersect\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::SliderFloat("Feather", &app.sel_feather, 0.0f, 200.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    ImGui::Checkbox("Anti-alias", &app.sel_antialias);
}

// Shift/Ctrl held at press time override the mode, as in the original.
int gesture_mode(const App& app) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyShift) return 1;
    if (io.KeyCtrl) return 2;
    return app.sel_mode;
}

class SelectionTool : public Tool {
public:
    const char* name() const override { return "Selection"; }
    const char* shortcut() const override { return "S"; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!app.doc) return;
        dragging_ = true;
        x0_ = x1_ = in.img_x;
        y0_ = y1_ = in.img_y;
        mode_ = gesture_mode(app);
    }
    void on_drag(App&, const ToolInput& in, ImGuiMouseButton) override {
        if (!dragging_) return;
        x1_ = in.img_x;
        y1_ = in.img_y;
    }
    void on_release(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!dragging_ || !app.doc) return;
        dragging_ = false;
        x1_ = in.img_x; y1_ = in.img_y;
        const int w = app.doc->width(), h = app.doc->height();
        if (std::abs(x1_ - x0_) < 1.0f || std::abs(y1_ - y0_) < 1.0f) {
            // A click without a drag deselects, like the original.
            app.select_none();
            return;
        }
        Mask shape = app.sel_shape == 0
            ? mask::rectangle(w, h, x0_, y0_, x1_, y1_, app.sel_antialias)
            : mask::ellipse(w, h, (x0_ + x1_) * 0.5f, (y0_ + y1_) * 0.5f, (x1_ - x0_) * 0.5f, (y1_ - y0_) * 0.5f, app.sel_antialias);
        const int saved = app.sel_mode;
        app.sel_mode = mode_;
        app.apply_selection_gesture("Selection", std::move(shape));
        app.sel_mode = saved;
    }
    void cancel(App&) override { dragging_ = false; }
    void draw_overlay(App& app, const ToolInput& in) override {
        if (!dragging_) return;
        const ImVec2 a(in.origin.x + x0_ * in.zoom, in.origin.y + y0_ * in.zoom);
        const ImVec2 b(in.origin.x + x1_ * in.zoom, in.origin.y + y1_ * in.zoom);
        if (app.sel_shape == 0) {
            in.dl->AddRect(a, b, IM_COL32(0, 0, 0, 255));
            in.dl->AddRect(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 1), IM_COL32(255, 255, 255, 255));
        } else {
            const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            in.dl->AddEllipse(c, ImVec2(std::abs(b.x - a.x) * 0.5f, std::abs(b.y - a.y) * 0.5f), IM_COL32(0, 0, 0, 255), 0.0f, 0, 1.0f);
            in.dl->AddEllipse(c, ImVec2(std::abs(b.x - a.x) * 0.5f - 1, std::abs(b.y - a.y) * 0.5f - 1), IM_COL32(255, 255, 255, 255), 0.0f, 0, 1.0f);
        }
    }
    void draw_options(App& app) override {
        ImGui::SetNextItemWidth(110);
        ImGui::Combo("Shape", &app.sel_shape, "Rectangle\0Ellipse\0");
        ImGui::SameLine();
        draw_selection_common(app);
    }

private:
    bool dragging_ = false;
    float x0_ = 0, y0_ = 0, x1_ = 0, y1_ = 0;
    int mode_ = 0;
};

class FreehandTool : public Tool {
public:
    const char* name() const override { return "Freehand Selection"; }
    const char* shortcut() const override { return "L"; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!app.doc) return;
        pts_.clear();
        pts_.emplace_back(in.img_x, in.img_y);
        mode_ = gesture_mode(app);
    }
    void on_drag(App&, const ToolInput& in, ImGuiMouseButton) override {
        if (pts_.empty()) return;
        const auto& l = pts_.back();
        if (std::abs(l.first - in.img_x) >= 0.5f || std::abs(l.second - in.img_y) >= 0.5f) pts_.emplace_back(in.img_x, in.img_y);
    }
    void on_release(App& app, const ToolInput&, ImGuiMouseButton) override {
        if (!app.doc) { pts_.clear(); return; }
        if (pts_.size() < 3) { pts_.clear(); app.select_none(); return; }
        Mask shape = mask::polygon(app.doc->width(), app.doc->height(), pts_, app.sel_antialias);
        pts_.clear();
        const int saved = app.sel_mode;
        app.sel_mode = mode_;
        app.apply_selection_gesture("Freehand Selection", std::move(shape));
        app.sel_mode = saved;
    }
    void cancel(App&) override { pts_.clear(); }
    void draw_overlay(App&, const ToolInput& in) override {
        if (pts_.size() < 2) return;
        for (size_t i = 0; i + 1 < pts_.size(); ++i) {
            const ImVec2 a(in.origin.x + pts_[i].first * in.zoom, in.origin.y + pts_[i].second * in.zoom);
            const ImVec2 b(in.origin.x + pts_[i + 1].first * in.zoom, in.origin.y + pts_[i + 1].second * in.zoom);
            in.dl->AddLine(a, b, IM_COL32(0, 0, 0, 255), 3.0f);
            in.dl->AddLine(a, b, IM_COL32(255, 255, 255, 255), 1.0f);
        }
    }
    void draw_options(App& app) override { draw_selection_common(app); }

private:
    std::vector<std::pair<float, float>> pts_;
    int mode_ = 0;
};

class MagicWandTool : public Tool {
public:
    const char* name() const override { return "Magic Wand"; }
    const char* shortcut() const override { return "W"; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!in.inside || !app.doc || app.active_layer() < 0) return;
        const int x = static_cast<int>(std::floor(in.img_x)), y = static_cast<int>(std::floor(in.img_y));
        Mask shape = app.wand_sample_merged
            ? mask::magic_wand(app.doc->composite(), x, y, app.wand_tolerance, app.wand_contiguous)
            : mask::magic_wand(app.doc->layer(app.active_layer()).pixels, x, y, app.wand_tolerance, app.wand_contiguous);
        const int saved = app.sel_mode;
        app.sel_mode = gesture_mode(app);
        app.apply_selection_gesture("Magic Wand", std::move(shape));
        app.sel_mode = saved;
    }
    void draw_options(App& app) override {
        ImGui::SetNextItemWidth(110);
        ImGui::SliderInt("Tolerance", &app.wand_tolerance, 0, 200);
        ImGui::SameLine();
        ImGui::Checkbox("Contiguous", &app.wand_contiguous);
        ImGui::SameLine();
        ImGui::Checkbox("Sample merged", &app.wand_sample_merged);
        ImGui::SameLine();
        draw_selection_common(app);
    }
};

}  // namespace

std::vector<std::unique_ptr<Tool>> make_default_tools() {
    std::vector<std::unique_ptr<Tool>> t;
    t.push_back(std::make_unique<PanTool>());
    t.push_back(std::make_unique<ZoomTool>());
    t.push_back(std::make_unique<SelectionTool>());
    t.push_back(std::make_unique<FreehandTool>());
    t.push_back(std::make_unique<MagicWandTool>());
    t.push_back(std::make_unique<DropperTool>());
    t.push_back(std::make_unique<BrushTool>(false));
    t.push_back(std::make_unique<BrushTool>(true));
    t.push_back(std::make_unique<FloodFillTool>());
    return t;
}
