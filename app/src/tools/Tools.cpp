#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "App.h"
#include "firn/adjust.h"
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

// --- Brush family ------------------------------------------------------
// One class covers every tool that paints a stroke: what differs is the
// stroke mode and which colour / filter / source feeds it.

class BrushTool : public Tool {
public:
    enum class Kind { Paint, Eraser, Airbrush, Clone, LightenDarken, Saturation, Hue, ColorReplacer };
    explicit BrushTool(Kind k) : kind_(k) {}

    const char* name() const override {
        switch (kind_) {
            case Kind::Paint: return "Paint Brush";
            case Kind::Eraser: return "Eraser";
            case Kind::Airbrush: return "Airbrush";
            case Kind::Clone: return "Clone Brush";
            case Kind::LightenDarken: return "Lighten/Darken";
            case Kind::Saturation: return "Saturation Up/Down";
            case Kind::Hue: return "Hue Up/Down";
            default: return "Color Replacer";
        }
    }
    const char* shortcut() const override {
        switch (kind_) {
            case Kind::Paint: return "B";
            case Kind::Eraser: return "X";
            case Kind::Airbrush: return "P";
            case Kind::Clone: return "C";
            case Kind::LightenDarken: return "N";
            case Kind::ColorReplacer: return "Q";
            default: return nullptr;
        }
    }

    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!app.doc || app.active_layer() < 0) return;
        // Clone: right-click sets the source point.
        if (kind_ == Kind::Clone && b == ImGuiMouseButton_Right) {
            src_x_ = in.img_x; src_y_ = in.img_y; has_src_ = true; first_stroke_ = true;
            return;
        }
        Layer& L = app.doc->layer(app.active_layer());
        raster::Brush brush = app.brush;
        brush.accumulate = kind_ == Kind::Airbrush;
        Color color = to_color(b == ImGuiMouseButton_Left ? app.fg_color : app.bg_color);
        raster::StrokeMode mode = raster::StrokeMode::Paint;
        std::function<Color(Color)> filter;
        const float amount = app.retouch_amount / 100.0f;
        const bool primary = b == ImGuiMouseButton_Left;

        switch (kind_) {
            case Kind::Eraser:
                if (L.background) color = to_color(primary ? app.bg_color : app.fg_color);
                else mode = raster::StrokeMode::Erase;
                break;
            case Kind::Clone:
                if (!has_src_) { app.status = "Clone Brush: right-click to set the source point first."; return; }
                mode = raster::StrokeMode::Clone;
                if (!app.clone_aligned || first_stroke_) {
                    off_x_ = static_cast<int>(std::floor(src_x_ - in.img_x));
                    off_y_ = static_cast<int>(std::floor(src_y_ - in.img_y));
                    first_stroke_ = false;
                }
                clone_src_ = app.clone_sample_merged ? app.doc->composite() : L.pixels;
                break;
            case Kind::LightenDarken:
                mode = raster::StrokeMode::Filter;
                filter = [amount, primary](Color c) {
                    adjust::HSL h = adjust::rgb_to_hsl(c.r, c.g, c.b);
                    h.l = primary ? h.l + (1 - h.l) * amount : h.l * (1 - amount);
                    adjust::hsl_to_rgb(h, &c.r, &c.g, &c.b);
                    return c;
                };
                break;
            case Kind::Saturation:
                mode = raster::StrokeMode::Filter;
                filter = [amount, primary](Color c) {
                    adjust::HSL h = adjust::rgb_to_hsl(c.r, c.g, c.b);
                    h.s = primary ? h.s + (1 - h.s) * amount : h.s * (1 - amount);
                    adjust::hsl_to_rgb(h, &c.r, &c.g, &c.b);
                    return c;
                };
                break;
            case Kind::Hue:
                mode = raster::StrokeMode::Filter;
                filter = [amount, primary](Color c) {
                    adjust::HSL h = adjust::rgb_to_hsl(c.r, c.g, c.b);
                    h.h += (primary ? 1 : -1) * amount * 180.0f;
                    adjust::hsl_to_rgb(h, &c.r, &c.g, &c.b);
                    return c;
                };
                break;
            case Kind::ColorReplacer: {
                // Replaces the background colour with the foreground (right button: the reverse).
                mode = raster::StrokeMode::Filter;
                const Color from = to_color(primary ? app.bg_color : app.fg_color);
                const Color to = to_color(primary ? app.fg_color : app.bg_color);
                const int tol = app.replacer_tolerance;
                filter = [from, to, tol](Color c) {
                    const int d = std::max({std::abs(c.r - from.r), std::abs(c.g - from.g), std::abs(c.b - from.b)});
                    return d <= tol ? Color{to.r, to.g, to.b, c.a} : c;
                };
                break;
            }
            default: break;
        }
        layer_ = app.active_layer();
        stroke_ = std::make_unique<raster::Stroke>(L.pixels, brush, color, mode, &app.doc->selection());
        if (mode == raster::StrokeMode::Clone) stroke_->set_clone_source(&clone_src_, off_x_, off_y_);
        if (filter) stroke_->set_filter(std::move(filter));
        last_x_ = in.img_x; last_y_ = in.img_y;
        stroke_->add_point(in.img_x, in.img_y);
        flush(app);
    }
    void on_drag(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!stroke_) return;
        stroke_->add_point(in.img_x, in.img_y);
        last_x_ = in.img_x; last_y_ = in.img_y;
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
        // Airbrush keeps spraying while the button is held, even at rest.
        if (kind_ == Kind::Airbrush && stroke_ && app.doc) {
            stroke_->stamp_at(last_x_, last_y_);
            flush(app);
        }
        const float r = app.brush.size * 0.5f * in.zoom;
        in.dl->AddCircle(in.screen, r, IM_COL32(0, 0, 0, 200), 0, 1.0f);
        in.dl->AddCircle(in.screen, r + 1.0f, IM_COL32(255, 255, 255, 160), 0, 1.0f);
        if (kind_ == Kind::Clone && has_src_) {
            const float sx = stroke_ ? in.img_x + off_x_ : src_x_, sy = stroke_ ? in.img_y + off_y_ : src_y_;
            const ImVec2 c(in.origin.x + sx * in.zoom, in.origin.y + sy * in.zoom);
            in.dl->AddLine(ImVec2(c.x - 6, c.y), ImVec2(c.x + 6, c.y), IM_COL32(255, 255, 255, 255));
            in.dl->AddLine(ImVec2(c.x, c.y - 6), ImVec2(c.x, c.y + 6), IM_COL32(255, 255, 255, 255));
            in.dl->AddCircle(c, r, IM_COL32(255, 255, 255, 120), 0, 1.0f);
        }
    }
    void draw_options(App& app) override {
        ImGui::SetNextItemWidth(140);
        ImGui::SliderFloat("Size", &app.brush.size, 1.0f, 500.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        float hard = app.brush.hardness * 100.0f;
        if (ImGui::SliderFloat("Hardness", &hard, 0.0f, 100.0f, "%.0f")) app.brush.hardness = hard / 100.0f;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        float op = app.brush.opacity * 100.0f;
        if (ImGui::SliderFloat("Opacity", &op, 1.0f, 100.0f, "%.0f")) app.brush.opacity = op / 100.0f;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        float step = app.brush.step * 100.0f;
        if (ImGui::SliderFloat("Step", &step, 1.0f, 200.0f, "%.0f")) app.brush.step = step / 100.0f;
        switch (kind_) {
            case Kind::Airbrush: {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                float flow = app.brush.flow * 100.0f;
                if (ImGui::SliderFloat("Rate", &flow, 1.0f, 100.0f, "%.0f")) app.brush.flow = flow / 100.0f;
                break;
            }
            case Kind::Clone:
                ImGui::SameLine();
                ImGui::Checkbox("Aligned", &app.clone_aligned);
                ImGui::SameLine();
                ImGui::Checkbox("Sample merged", &app.clone_sample_merged);
                ImGui::SameLine();
                ImGui::TextDisabled(has_src_ ? "Right-click to move the source." : "Right-click to set the source.");
                break;
            case Kind::LightenDarken: case Kind::Saturation: case Kind::Hue:
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                ImGui::SliderInt("Amount", &app.retouch_amount, 1, 100, "%d%%");
                ImGui::SameLine();
                ImGui::TextDisabled(kind_ == Kind::LightenDarken ? "Left lightens, right darkens." :
                                    kind_ == Kind::Saturation ? "Left saturates, right desaturates." : "Left shifts hue up, right down.");
                break;
            case Kind::ColorReplacer:
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                ImGui::SliderInt("Tolerance", &app.replacer_tolerance, 0, 200);
                ImGui::SameLine();
                ImGui::TextDisabled("Paints foreground over background colour.");
                break;
            default: break;
        }
    }

private:
    void flush(App& app) {
        Layer& L = app.doc->layer(layer_);
        if (!stroke_->render(L.pixels).empty()) app.doc->touch();
    }
    Kind kind_;
    size_t layer_ = 0;
    std::unique_ptr<raster::Stroke> stroke_;
    float last_x_ = 0, last_y_ = 0;
    // Clone state
    bool has_src_ = false, first_stroke_ = true;
    float src_x_ = 0, src_y_ = 0;
    int off_x_ = 0, off_y_ = 0;
    Image clone_src_;
};

// --- Move --------------------------------------------------------------
// Left drag moves the active layer's pixels; right drag moves the selection
// marquee, as the original's Mover does.

class MoveTool : public Tool {
public:
    const char* name() const override { return "Move"; }
    const char* shortcut() const override { return "M"; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!app.doc || app.active_layer() < 0) return;
        button_ = b;
        x0_ = in.img_x; y0_ = in.img_y;
        layer_ = app.active_layer();
        if (b == ImGuiMouseButton_Left) before_ = app.doc->layer(layer_).pixels;
        else sel_before_ = app.doc->selection();
        active_ = true;
    }
    void on_drag(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!active_) return;
        dx_ = static_cast<int>(std::lround(in.img_x - x0_));
        dy_ = static_cast<int>(std::lround(in.img_y - y0_));
        if (button_ == ImGuiMouseButton_Left) {
            Layer& L = app.doc->layer(layer_);
            L.pixels = raster::shifted(before_, dx_, dy_);
            if (L.background) {
                const Color fill = app.background_fill();
                uint8_t* p = L.pixels.data();
                for (size_t i = 0; i < L.pixels.size_bytes(); i += 4)
                    if (p[i + 3] == 0) { p[i] = fill.r; p[i + 1] = fill.g; p[i + 2] = fill.b; p[i + 3] = 255; }
            }
            app.doc->touch();
        } else if (!sel_before_.empty()) {
            Mask m(sel_before_.width(), sel_before_.height());
            for (int y = 0; y < m.height(); ++y)
                for (int x = 0; x < m.width(); ++x) {
                    const int sx = x - dx_, sy = y - dy_;
                    if (sx >= 0 && sy >= 0 && sx < m.width() && sy < m.height()) m.at(x, y) = sel_before_.at(sx, sy);
                }
            app.doc->set_selection(std::move(m));
        }
    }
    void on_release(App& app, const ToolInput&, ImGuiMouseButton) override {
        if (!active_) return;
        active_ = false;
        if (dx_ == 0 && dy_ == 0) return;
        if (button_ == ImGuiMouseButton_Left)
            app.commit(std::make_unique<LayerSnapshotCommand>(layer_, "Move", before_, app.doc->layer(layer_).pixels));
        else if (!sel_before_.empty()) {
            Mask moved = app.doc->selection();
            app.doc->set_selection(sel_before_);
            app.set_selection("Move Selection", std::move(moved));
        }
        dx_ = dy_ = 0;
    }
    void cancel(App& app) override {
        if (active_ && app.doc && layer_ < app.doc->layer_count()) {
            if (button_ == ImGuiMouseButton_Left) { app.doc->layer(layer_).pixels = before_; app.doc->touch(); }
            else app.doc->set_selection(sel_before_);
        }
        active_ = false;
        dx_ = dy_ = 0;
    }
    void draw_options(App&) override {
        ImGui::TextUnformatted("Left drag moves the layer, right drag moves the selection marquee.");
    }

private:
    bool active_ = false;
    ImGuiMouseButton button_ = ImGuiMouseButton_Left;
    float x0_ = 0, y0_ = 0;
    int dx_ = 0, dy_ = 0;
    size_t layer_ = 0;
    Image before_;
    Mask sel_before_;
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

// --- Crop --------------------------------------------------------------

class CropTool : public Tool {
public:
    const char* name() const override { return "Crop"; }
    const char* shortcut() const override { return "R"; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!app.doc) return;
        x0_ = in.img_x; y0_ = in.img_y;
        dragging_ = true;
        update(app, in);
    }
    void on_drag(App& app, const ToolInput& in, ImGuiMouseButton) override { if (dragging_) update(app, in); }
    void on_release(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!dragging_) return;
        update(app, in);
        dragging_ = false;
        if (app.crop_rect.x1 - app.crop_rect.x0 < 1 || app.crop_rect.y1 - app.crop_rect.y0 < 1) app.crop_rect = {};
    }
    void cancel(App& app) override { dragging_ = false; app.crop_rect = {}; }
    void draw_overlay(App& app, const ToolInput& in) override {
        const raster::Rect& r = app.crop_rect;
        if (r.empty()) return;
        const ImVec2 a(in.origin.x + r.x0 * in.zoom, in.origin.y + r.y0 * in.zoom);
        const ImVec2 b(in.origin.x + r.x1 * in.zoom, in.origin.y + r.y1 * in.zoom);
        // Darken everything outside the crop rect.
        const ImVec2 big0(in.origin.x - 100000.0f, in.origin.y - 100000.0f), big1(in.origin.x + 100000.0f, in.origin.y + 100000.0f);
        const ImU32 shade = IM_COL32(0, 0, 0, 110);
        in.dl->AddRectFilled(big0, ImVec2(big1.x, a.y), shade);
        in.dl->AddRectFilled(ImVec2(big0.x, b.y), big1, shade);
        in.dl->AddRectFilled(ImVec2(big0.x, a.y), ImVec2(a.x, b.y), shade);
        in.dl->AddRectFilled(ImVec2(b.x, a.y), ImVec2(big1.x, b.y), shade);
        in.dl->AddRect(a, b, IM_COL32(255, 255, 255, 255));
        in.dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), IM_COL32(0, 0, 0, 255));
        // Enter applies while the tool is active.
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) app.crop_to(app.crop_rect);
    }
    void draw_options(App& app) override {
        const raster::Rect& r = app.crop_rect;
        if (r.empty()) ImGui::TextUnformatted("Drag a rectangle, then press Enter or Apply.");
        else ImGui::Text("%d, %d  %d x %d", r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
        ImGui::SameLine();
        if (ImGui::SmallButton("Apply") && !r.empty()) app.crop_to(r);
        ImGui::SameLine();
        if (ImGui::SmallButton("Selection") && app.doc && app.doc->has_selection()) app.crop_rect = app.doc->selection().bounds();
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) app.crop_rect = {};
    }

private:
    void update(App& app, const ToolInput& in) {
        const float x1 = in.img_x, y1 = in.img_y;
        raster::Rect r{static_cast<int>(std::floor(std::min(x0_, x1))), static_cast<int>(std::floor(std::min(y0_, y1))),
                       static_cast<int>(std::ceil(std::max(x0_, x1))), static_cast<int>(std::ceil(std::max(y0_, y1)))};
        app.crop_rect = r.clipped(app.doc->width(), app.doc->height());
    }
    bool dragging_ = false;
    float x0_ = 0, y0_ = 0;
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
    t.push_back(std::make_unique<MoveTool>());
    t.push_back(std::make_unique<CropTool>());
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Paint));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Airbrush));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Eraser));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Clone));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::LightenDarken));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Saturation));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Hue));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::ColorReplacer));
    t.push_back(std::make_unique<FloodFillTool>());
    return t;
}
