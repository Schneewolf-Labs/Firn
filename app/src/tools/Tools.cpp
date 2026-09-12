#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include "App.h"
#include "tools/ToolState.h"
#include "firn/adjust.h"
#include "firn/commands.h"
#include "firn/io_psp.h"
#include "firn/mask.h"
#include "firn/raster.h"
#include "firn/vector.h"
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
    const char* category() const override { return "View"; }
    const char* name() const override { return "Pan"; }
    const char* shortcut() const override { return "A"; }
    bool pans_with_left_drag() const override { return true; }
    void draw_options(App&) override { ImGui::TextUnformatted("Drag to pan. Wheel to zoom."); }
};

// --- Zoom --------------------------------------------------------------

class ZoomTool : public Tool {
public:
    const char* category() const override { return "View"; }
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
    const char* category() const override { return "Color"; }
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
// stroke mode and which color / filter / source feeds it.

class BrushTool : public Tool {
public:
    enum class Kind { Paint, Eraser, Airbrush, Clone, LightenDarken, DodgeBurn, Saturation, Hue, ColorReplacer, Soften, Sharpen, Heal };
    const char* category() const override {
        switch (kind_) {
            case Kind::Paint: case Kind::Airbrush: return "Paint";
            case Kind::Eraser: return "Erase";
            case Kind::Clone: case Kind::Heal: case Kind::ColorReplacer: return "Clone and Replace";
            default: return "Retouch";
        }
    }
    explicit BrushTool(Kind k) : kind_(k) {}

    const char* name() const override {
        switch (kind_) {
            case Kind::Paint: return "Paint Brush";
            case Kind::Eraser: return "Eraser";
            case Kind::Airbrush: return "Airbrush";
            case Kind::Clone: return "Clone Brush";
            case Kind::Heal: return "Heal Brush";
            case Kind::LightenDarken: return "Lighten/Darken";
            case Kind::DodgeBurn: return "Dodge/Burn";
            case Kind::Soften: return "Soften Brush";
            case Kind::Sharpen: return "Sharpen Brush";
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
        if (!app.active_is_raster()) { if (app.doc && app.active_layer() >= 0) app.status = "Select a raster layer to paint on."; return; }
        if (app.symmetry_place) {   // Tool Options "Place": this click sets the symmetry center.
            app.symmetry_x = in.img_x; app.symmetry_y = in.img_y; app.symmetry_place = false;
            return;
        }
        // Clone: right-click sets the source point.
        if ((kind_ == Kind::Clone || kind_ == Kind::Heal) && b == ImGuiMouseButton_Right) {
            src_x_ = in.img_x; src_y_ = in.img_y; has_src_ = true; first_stroke_ = true;
            return;
        }
        Layer& L = app.doc->layer(app.active_layer());
        raster::Brush brush = app.brush;
        brush.accumulate = kind_ == Kind::Airbrush;
        Color color = to_color(b == ImGuiMouseButton_Left ? app.fg_color : app.bg_color);
        raster::StrokeMode mode = raster::StrokeMode::Paint;
        std::function<Color(Color)> filter;
        std::function<Color(const Image&, int, int)> area_filter;
        const float amount = app.tool_state->retouch_amount / 100.0f;
        const bool primary = b == ImGuiMouseButton_Left;

        switch (kind_) {
            case Kind::Eraser:
                if (L.background) color = to_color(primary ? app.bg_color : app.fg_color);
                else mode = raster::StrokeMode::Erase;
                break;
            case Kind::Clone: case Kind::Heal:
                if (!has_src_) { app.status = std::string(name()) + ": right-click to set the source point first."; return; }
                mode = kind_ == Kind::Heal ? raster::StrokeMode::Heal : raster::StrokeMode::Clone;
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
            case Kind::DodgeBurn:
                mode = raster::StrokeMode::Filter;
                filter = [amount, primary](Color c) {
                    const float k = primary ? 1.0f + amount : 1.0f - amount;
                    auto m = [&](uint8_t v) { return static_cast<uint8_t>(std::clamp(v * k, 0.0f, 255.0f) + 0.5f); };
                    return Color{m(c.r), m(c.g), m(c.b), c.a};
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
            case Kind::Soften:
            case Kind::Sharpen: {
                mode = raster::StrokeMode::Filter;
                const bool soften = kind_ == Kind::Soften;
                area_filter = [amount, soften](const Image& im, int x, int y) {
                    float acc[3] = {0, 0, 0};
                    int n = 0;
                    for (int j = -1; j <= 1; ++j)
                        for (int i = -1; i <= 1; ++i) {
                            const int px = std::clamp(x + i, 0, im.width() - 1), py = std::clamp(y + j, 0, im.height() - 1);
                            const Color s = im.get(px, py);
                            acc[0] += s.r; acc[1] += s.g; acc[2] += s.b; ++n;
                        }
                    const Color c = im.get(x, y);
                    auto mix = [&](uint8_t v, float avg) {
                        // Soften moves towards the neighborhood mean; sharpen away from it.
                        const float out = soften ? v + (avg - v) * amount : v + (v - avg) * amount * 2.0f;
                        return static_cast<uint8_t>(std::clamp(out, 0.0f, 255.0f) + 0.5f);
                    };
                    return Color{mix(c.r, acc[0] / n), mix(c.g, acc[1] / n), mix(c.b, acc[2] / n), c.a};
                };
                break;
            }
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
                // Replaces the background color with the foreground (right button: the reverse).
                mode = raster::StrokeMode::Filter;
                const Color from = to_color(primary ? app.bg_color : app.fg_color);
                const Color to = to_color(primary ? app.fg_color : app.bg_color);
                const int tol = app.tool_state->replacer_tolerance;
                filter = [from, to, tol](Color c) {
                    const int d = std::max({std::abs(c.r - from.r), std::abs(c.g - from.g), std::abs(c.b - from.b)});
                    return d <= tol ? Color{to.r, to.g, to.b, c.a} : c;
                };
                break;
            }
            default: break;
        }
        layer_ = app.active_layer();
        stroke_ = std::make_unique<raster::Stroke>(app.paint_pixels(layer_), brush, color, mode, app.paint_clip(static_cast<int>(layer_)));
        if (mode == raster::StrokeMode::Clone || mode == raster::StrokeMode::Heal) stroke_->set_clone_source(&clone_src_, off_x_, off_y_);
        if (filter) stroke_->set_filter(std::move(filter));
        if (area_filter) stroke_->set_area_filter(std::move(area_filter));
        stroke_->set_pressure_response(app.pen_size, app.pen_opacity);
        stroke_->set_symmetry(app.symmetry());
        // Assistants: the stroke follows the nearest one from where it starts.
        // Either the assistant the user picked, or the nearest to where the
        // stroke starts. Two vanishing points need the pick: "nearest" has
        // no useful meaning once both of them cover the picture.
        assist_ = -1;
        if (app.assistant_snap) {
            const int chosen = app.assistant_choice;
            assist_ = chosen >= 0 && chosen < static_cast<int>(app.assistants().size()) ? chosen : app.nearest_assistant(in.img_x, in.img_y);
        }
        float sx = in.img_x, sy = in.img_y;
        if (assist_ >= 0 && app.assistants()[assist_].kind == Assistant::Kind::Ruler) app.assist_point(assist_, sx, sy, sx, sy);
        assist_sx_ = sx; assist_sy_ = sy;
        smooth_x_ = sx; smooth_y_ = sy; history_.clear();
        last_x_ = sx; last_y_ = sy; last_pressure_ = in.pressure;
        stroke_->add_point(sx, sy, in.pressure);
        flush(app);
    }
    void on_drag(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!stroke_) return;
        float x = in.img_x, y = in.img_y;
        if (assist_ >= 0) app.assist_point(assist_, assist_sx_, assist_sy_, x, y);
        smooth_point(app, in, x, y);
        stroke_->add_point(x, y, in.pressure);
        last_x_ = x; last_y_ = y; last_pressure_ = in.pressure;
        flush(app);
    }
    void on_release(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!stroke_ || !app.doc) return;
        // Smoothing lags behind the cursor; finish the line to where the pen lifted.
        float ex = in.img_x, ey = in.img_y;
        if (assist_ >= 0) app.assist_point(assist_, assist_sx_, assist_sy_, ex, ey);
        if (app.smooth_mode != 0 && (std::abs(ex - last_x_) > 0.5f || std::abs(ey - last_y_) > 0.5f)) {
            stroke_->add_point(ex, ey, in.pressure);
            flush(app);
        }
        app.commit_pixels(layer_, name(), stroke_->base(), app.paint_pixels(layer_));
        stroke_.reset();
    }
    // Stroke smoothing, in the spirit of the usual painting programs:
    // Basic averages the last few points, Weighted follows the cursor with
    // inertia, Stabilizer drags the brush behind the cursor on a string.
    void smooth_point(const App& app, const ToolInput& in, float& x, float& y) {
        const float amount = std::clamp(app.smooth_amount, 0.0f, 100.0f);
        if (app.smooth_mode == 0 || amount <= 0.0f) { smooth_x_ = x; smooth_y_ = y; return; }
        if (app.smooth_mode == 1) {
            history_.emplace_back(x, y);
            const size_t n = static_cast<size_t>(2 + amount / 8.0f);
            while (history_.size() > n) history_.erase(history_.begin());
            float sx = 0, sy = 0;
            for (const auto& pt : history_) { sx += pt.first; sy += pt.second; }
            x = sx / history_.size(); y = sy / history_.size();
        } else if (app.smooth_mode == 2) {
            const float k = 1.0f - 0.95f * amount / 100.0f;   // follow fraction per event
            smooth_x_ += (x - smooth_x_) * k; smooth_y_ += (y - smooth_y_) * k;
            x = smooth_x_; y = smooth_y_;
        } else {
            // String length in screen pixels, so it feels the same at any zoom.
            const float radius = amount * 2.0f / std::max(in.zoom, 0.01f);
            const float dx = x - smooth_x_, dy = y - smooth_y_, dist = std::sqrt(dx * dx + dy * dy);
            if (dist > radius) { const float f = (dist - radius) / dist; smooth_x_ += dx * f; smooth_y_ += dy * f; }
            x = smooth_x_; y = smooth_y_;
        }
    }
    void cancel(App& app) override {
        if (stroke_ && app.doc && layer_ < app.doc->layer_count()) {
            app.paint_pixels(layer_) = stroke_->base();
            app.paint_touched(layer_);
        }
        stroke_.reset();
    }

    void draw_overlay(App& app, const ToolInput& in) override {
        // Airbrush keeps spraying while the button is held, even at rest.
        if (kind_ == Kind::Airbrush && stroke_ && app.doc) {
            stroke_->stamp_at(last_x_, last_y_, last_pressure_);
            flush(app);
        }
        const float r = app.brush.size * 0.5f * in.zoom;
        if (app.brush.tip) {
            const float scale = app.brush.size / std::max(app.brush.tip->width, app.brush.tip->height);
            const float hw = app.brush.tip->width * scale * 0.5f * in.zoom, hh = app.brush.tip->height * scale * 0.5f * in.zoom;
            in.dl->AddRect(ImVec2(in.screen.x - hw, in.screen.y - hh), ImVec2(in.screen.x + hw, in.screen.y + hh), IM_COL32(255, 255, 255, 160));
        } else if (app.brush.square) {
            in.dl->AddRect(ImVec2(in.screen.x - r, in.screen.y - r), ImVec2(in.screen.x + r, in.screen.y + r), IM_COL32(0, 0, 0, 200));
            in.dl->AddRect(ImVec2(in.screen.x - r - 1, in.screen.y - r - 1), ImVec2(in.screen.x + r + 1, in.screen.y + r + 1), IM_COL32(255, 255, 255, 160));
        } else {
            in.dl->AddCircle(in.screen, r, IM_COL32(0, 0, 0, 200), 0, 1.0f);
            in.dl->AddCircle(in.screen, r + 1.0f, IM_COL32(255, 255, 255, 160), 0, 1.0f);
        }
        if ((kind_ == Kind::Clone || kind_ == Kind::Heal) && has_src_) {
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
        if (app.pen.present) {
            ImGui::SameLine();
            ImGui::TextDisabled("Pen %d%%%s", static_cast<int>(app.pen.pressure * 100 + 0.5f), app.pen.eraser ? " (eraser)" : "");
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Pressure:");
        ImGui::SameLine();
        if (ImGui::Checkbox("Size##pen", &app.pen_size)) { app.config.pen_size = app.pen_size; app.config.save(); }
        ImGui::SameLine();
        if (ImGui::Checkbox("Opacity##pen", &app.pen_opacity)) { app.config.pen_opacity = app.pen_opacity; app.config.save(); }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        if (ImGui::Combo("Smoothing", &app.smooth_mode, "None\0Basic\0Weighted\0Stabilizer\0")) { app.config.smooth_mode = app.smooth_mode; app.config.save(); }
        if (app.smooth_mode != 0) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            if (ImGui::SliderFloat("##smooth", &app.smooth_amount, 1.0f, 100.0f, "%.0f")) { app.config.smooth_amount = app.smooth_amount; app.config.save(); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Basic: points averaged. Weighted: the brush follows with inertia. Stabilizer: the brush trails the cursor on a string this long.");
        }
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
        // Second row: shape, custom tip, tool-specific extras.
        ImGui::SetNextItemWidth(80);
        int shape = app.brush.square ? 1 : 0;
        if (ImGui::Combo("Shape", &shape, "Round\0Square\0")) app.brush.square = shape == 1;
        ImGui::SameLine();
        app.ensure_brush_tips();
        ImGui::SetNextItemWidth(140);
        const char* tip_label = app.brush_tip_index >= 0 ? app.brush_tips[app.brush_tip_index].name.c_str() : "(shape)";
        if (ImGui::BeginCombo("Tip", tip_label)) {
            if (ImGui::Selectable("(shape)", app.brush_tip_index < 0)) app.select_brush_tip(-1);
            for (size_t i = 0; i < app.brush_tips.size(); ++i)
                if (ImGui::Selectable(app.brush_tips[i].name.c_str(), static_cast<int>(i) == app.brush_tip_index)) app.select_brush_tip(static_cast<int>(i));
            ImGui::EndCombo();
        }
        if (app.doc && app.doc->has_selection()) { ImGui::SameLine(); if (ImGui::SmallButton("Tip from selection")) app.brush_tip_from_selection(); }
        ImGui::SameLine();
        app.ensure_textures();
        ImGui::SetNextItemWidth(140);
        const char* tex_label = app.texture_index >= 0 ? app.textures[app.texture_index].name.c_str() : "(none)";
        if (ImGui::BeginCombo("Texture", tex_label)) {
            if (ImGui::Selectable("(none)", app.texture_index < 0)) app.select_texture(-1);
            for (size_t i = 0; i < app.textures.size(); ++i)
                if (ImGui::Selectable(app.textures[i].name.c_str(), static_cast<int>(i) == app.texture_index)) app.select_texture(static_cast<int>(i));
            ImGui::EndCombo();
        }
        if (app.brush.texture) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            float ts = app.brush.texture_strength * 100.0f;
            if (ImGui::SliderFloat("Strength", &ts, 0.0f, 100.0f, "%.0f%%")) app.brush.texture_strength = ts / 100.0f;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::Combo("Symmetry", &app.symmetry_mode, "None\0Horizontal\0Vertical\0Both\0Rotational\0Kaleidoscope\0");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every stamp is repeated across the axes through the center point (Place, or the image center).");
        if (app.symmetry_mode != 0) {
            if (app.symmetry_mode >= 4) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90);
                ImGui::SliderInt("##symcount", &app.symmetry_count, 2, 32, "%d copies");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(app.symmetry_place ? "Click the image..." : "Place")) app.symmetry_place = !app.symmetry_place;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The next click on the image sets the symmetry center.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Center")) { app.symmetry_x = app.symmetry_y = -1.0f; app.symmetry_place = false; }
        }
        switch (kind_) {
            case Kind::Airbrush: {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                float flow = app.brush.flow * 100.0f;
                if (ImGui::SliderFloat("Rate", &flow, 1.0f, 100.0f, "%.0f")) app.brush.flow = flow / 100.0f;
                break;
            }
            case Kind::Clone: case Kind::Heal:
                ImGui::SameLine();
                ImGui::Checkbox("Aligned", &app.clone_aligned);
                ImGui::SameLine();
                ImGui::Checkbox("Sample merged", &app.clone_sample_merged);
                ImGui::SameLine();
                ImGui::TextDisabled(has_src_ ? "Right-click to move the source." : "Right-click to set the source.");
                break;
            case Kind::LightenDarken: case Kind::DodgeBurn: case Kind::Saturation: case Kind::Hue: case Kind::Soften: case Kind::Sharpen:
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                ImGui::SliderInt("Amount", &app.tool_state->retouch_amount, 1, 100, "%d%%");
                ImGui::SameLine();
                ImGui::TextDisabled(kind_ == Kind::LightenDarken ? "Left lightens, right darkens." :
                                    kind_ == Kind::DodgeBurn ? "Left dodges (lightens), right burns." :
                                    kind_ == Kind::Saturation ? "Left saturates, right desaturates." :
                                    kind_ == Kind::Hue ? "Left shifts hue up, right down." : "Either button.");
                break;
            case Kind::ColorReplacer:
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                ImGui::SliderInt("Tolerance", &app.tool_state->replacer_tolerance, 0, 200);
                ImGui::SameLine();
                ImGui::TextDisabled("Paints foreground over background color.");
                break;
            default: break;
        }
    }

private:
    void flush(App& app) {
        const raster::Rect r = stroke_->render(app.paint_pixels(layer_));
        if (!r.empty()) app.paint_touched(layer_, &r);
    }
    Kind kind_;
    size_t layer_ = 0;
    std::unique_ptr<raster::Stroke> stroke_;
    float last_x_ = 0, last_y_ = 0, last_pressure_ = 1.0f;
    float smooth_x_ = 0, smooth_y_ = 0;
    std::vector<std::pair<float, float>> history_;
    int assist_ = -1;                       // assistant this stroke follows
    float assist_sx_ = 0, assist_sy_ = 0;   // where it started
    // Clone state
    bool has_src_ = false, first_stroke_ = true;
    float src_x_ = 0, src_y_ = 0;
    int off_x_ = 0, off_y_ = 0;
    Image clone_src_;
};

// --- Assistant -----------------------------------------------------------
// Places painting assistants: click for a vanishing point, drag for a
// parallel ruler or a ruler; drag a handle to move one, right-click to
// remove it. The brushes follow them while View > Snap to Assistants is on.

const char* kind_name(firn::Assistant::Kind k) {
    return k == firn::Assistant::Kind::VanishingPoint ? "vanishing point" : k == firn::Assistant::Kind::Parallel ? "parallel ruler" : "ruler";
}

class AssistantTool : public Tool {
public:
    const char* category() const override { return "View"; }
    const char* name() const override { return "Assistant"; }
    bool overlay_always() const override { return true; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!app.doc) return;
        const float tol = 8.0f / std::max(in.zoom, 0.01f);
        int hit = -1, handle = 0;
        for (size_t i = 0; i < app.assistants().size(); ++i) {
            const Assistant& a = app.assistants()[i];
            if (std::hypot(in.img_x - a.x0, in.img_y - a.y0) <= tol) { hit = static_cast<int>(i); handle = 0; }
            else if (a.kind != Assistant::Kind::VanishingPoint && std::hypot(in.img_x - a.x1, in.img_y - a.y1) <= tol) { hit = static_cast<int>(i); handle = 1; }
        }
        if (b == ImGuiMouseButton_Right) {
            if (hit >= 0) app.assistants().erase(app.assistants().begin() + hit);
            return;
        }
        if (hit >= 0) { drag_ = hit; handle_ = handle; return; }
        Assistant a;
        a.kind = static_cast<Assistant::Kind>(std::clamp(app.assistant_kind, 0, 2));
        a.x0 = a.x1 = in.img_x; a.y0 = a.y1 = in.img_y;
        app.assistants().push_back(a);
        drag_ = static_cast<int>(app.assistants().size()) - 1;
        handle_ = a.kind == Assistant::Kind::VanishingPoint ? 0 : 1;
        fresh_ = true;
    }
    void on_drag(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (drag_ < 0 || drag_ >= static_cast<int>(app.assistants().size())) return;
        Assistant& a = app.assistants()[drag_];
        if (handle_ == 0) {
            if (a.kind == Assistant::Kind::VanishingPoint) { a.x0 = in.img_x; a.y0 = in.img_y; }
            else { a.x0 = in.img_x; a.y0 = in.img_y; }
        } else { a.x1 = in.img_x; a.y1 = in.img_y; }
    }
    void on_release(App& app, const ToolInput&, ImGuiMouseButton) override {
        // A ruler needs two distinct ends; a click alone makes none.
        if (fresh_ && drag_ >= 0 && drag_ < static_cast<int>(app.assistants().size())) {
            Assistant& a = app.assistants()[drag_];
            if (a.kind != Assistant::Kind::VanishingPoint && std::hypot(a.x1 - a.x0, a.y1 - a.y0) < 2.0f) app.assistants().erase(app.assistants().begin() + drag_);
        }
        drag_ = -1; fresh_ = false;
    }
    void cancel(App&) override { drag_ = -1; fresh_ = false; }
    void draw_options(App& app) override {
        ImGui::SetNextItemWidth(140);
        ImGui::Combo("Kind", &app.assistant_kind, "Vanishing Point\0Parallel Ruler\0Ruler\0");
        ImGui::SameLine();
        ImGui::Checkbox("Snap brushes", &app.assistant_snap);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        {
            const auto& list = app.assistants();
            const int chosen = app.assistant_choice;
            char label[64];
            if (chosen < 0 || chosen >= static_cast<int>(list.size())) std::snprintf(label, sizeof(label), "Nearest");
            else std::snprintf(label, sizeof(label), "%d: %s", chosen + 1, kind_name(list[static_cast<size_t>(chosen)].kind));
            if (ImGui::BeginCombo("Follow", label)) {
                if (ImGui::Selectable("Nearest", chosen < 0)) app.assistant_choice = -1;
                for (size_t i = 0; i < list.size(); ++i) {
                    char item[64];
                    std::snprintf(item, sizeof(item), "%zu: %s", i + 1, kind_name(list[i].kind));
                    if (ImGui::Selectable(item, chosen == static_cast<int>(i))) app.assistant_choice = static_cast<int>(i);
                }
                ImGui::EndCombo();
            }
        }
        ImGui::SameLine();
        ImGui::Checkbox("Show", &app.show_assistants);
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) app.assistants().clear();
        ImGui::SameLine();
        ImGui::TextDisabled(app.assistant_kind == 0 ? "Click to place a vanishing point; drag a point to move it; right-click removes it."
                                                    : "Drag to lay a ruler; drag an end to move it; right-click an end removes it.");
    }

private:
    int drag_ = -1, handle_ = 0;
    bool fresh_ = false;
};

// --- Smudge / Push -----------------------------------------------------
// Carries the pixels under the brush along the stroke: each stamp blends
// the previous stamp's pixels over the current ones (Smudge), or copies
// them without fading (Push, right button).

// Color Smudge (the painting programs' color smudge engine) is the same
// tool carrying paint as well: every stamp tints the carried pixels toward
// the foreground color by the color rate, so strokes lay down color that
// blends with what they pass over. Dulling carries one averaged color
// instead of the patch (smearing).
class SmudgeTool : public Tool {
public:
    explicit SmudgeTool(bool color = false) : color_(color) {}
    const char* category() const override { return color_ ? "Paint" : "Retouch"; }
    const char* name() const override { return color_ ? "Color Smudge" : "Smudge"; }
    const char* shortcut() const override { return color_ ? nullptr : "U"; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!app.active_is_raster()) return;
        layer_ = app.active_layer();
        before_ = app.paint_pixels(layer_);
        push_ = !color_ && b == ImGuiMouseButton_Right;
        paint_ = to_color(b == ImGuiMouseButton_Right ? app.bg_color : app.fg_color);
        grab(app, in.img_x, in.img_y);
        last_x_ = in.img_x; last_y_ = in.img_y;
        active_ = true;
    }
    void on_drag(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!active_) return;
        const float spacing = std::max(app.brush.size * 0.15f, 1.0f);
        const float dx = in.img_x - last_x_, dy = in.img_y - last_y_;
        const float len = std::hypot(dx, dy);
        for (float t = spacing; t <= len; t += spacing) stamp(app, last_x_ + dx * t / len, last_y_ + dy * t / len);
        if (len >= spacing) { last_x_ = in.img_x; last_y_ = in.img_y; }
        app.paint_touched(layer_);
    }
    void on_release(App& app, const ToolInput&, ImGuiMouseButton) override {
        if (!active_) return;
        active_ = false;
        app.commit_pixels(layer_, color_ ? "Color Smudge" : push_ ? "Push" : "Smudge", before_, app.paint_pixels(layer_));
    }
    void cancel(App& app) override {
        if (active_ && app.doc && layer_ < app.doc->layer_count()) { app.paint_pixels(layer_) = before_; app.paint_touched(layer_); }
        active_ = false;
    }
    void draw_overlay(App& app, const ToolInput& in) override {
        const float r = app.brush.size * 0.5f * in.zoom;
        in.dl->AddCircle(in.screen, r, IM_COL32(0, 0, 0, 200), 0, 1.0f);
        in.dl->AddCircle(in.screen, r + 1.0f, IM_COL32(255, 255, 255, 160), 0, 1.0f);
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
        if (color_) {
            float op = app.brush.opacity * 100.0f;
            if (ImGui::SliderFloat("Opacity", &op, 1.0f, 100.0f, "%.0f")) app.brush.opacity = op / 100.0f;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            ImGui::SliderInt("Length", &app.csmudge_length, 0, 100, "%d%%");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("How far the carried color is dragged before it fades to what lies under the brush.");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            ImGui::SliderInt("Color rate", &app.csmudge_rate, 0, 100, "%d%%");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("How much foreground color each stamp adds to what the brush carries. 0 smudges only.");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            ImGui::Combo("Mode", &app.csmudge_mode, "Smearing\0Dulling\0");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Smearing drags the pixels under the brush along; Dulling drags their average color.");
            ImGui::SameLine();
            ImGui::TextDisabled("Left paints the foreground color, right the background.");
        } else {
            ImGui::SliderInt("Amount", &app.tool_state->retouch_amount, 1, 100, "%d%%");
            ImGui::SameLine();
            ImGui::TextDisabled("Left smudges, right pushes.");
        }
    }

private:
    float coverage(App& app, float dx, float dy) const {
        const float r = std::max(app.brush.size * 0.5f, 0.5f), inner = r * app.brush.hardness;
        const float d = std::hypot(dx, dy);
        if (d <= inner) return 1.0f;
        if (app.brush.hardness >= 1.0f || r - inner < 1.0f) return std::clamp(r + 0.5f - d, 0.0f, 1.0f);
        return std::clamp((r - d) / (r - inner), 0.0f, 1.0f);
    }
    void grab(App& app, float cx, float cy) {
        const Image& px = app.paint_pixels(layer_);
        const int r = static_cast<int>(std::ceil(app.brush.size * 0.5f)) + 1;
        rad_ = r;
        buf_.assign(static_cast<size_t>(2 * r + 1) * (2 * r + 1) * 4, 0);
        const int ox = static_cast<int>(std::floor(cx)) - r, oy = static_cast<int>(std::floor(cy)) - r;
        for (int y = 0; y <= 2 * r; ++y)
            for (int x = 0; x <= 2 * r; ++x) {
                const int sx = ox + x, sy = oy + y;
                if (sx < 0 || sy < 0 || sx >= px.width() || sy >= px.height()) continue;
                std::memcpy(&buf_[(static_cast<size_t>(y) * (2 * r + 1) + x) * 4], px.data() + (static_cast<size_t>(sy) * px.width() + sx) * 4, 4);
            }
        if (color_ && app.csmudge_mode == 1) dull();
    }
    // Dulling: the patch becomes its alpha-weighted mean color.
    void dull() {
        float acc[4] = {0, 0, 0, 0}; float n = 0;
        for (size_t i = 0; i < buf_.size(); i += 4) { const float a = buf_[i + 3] / 255.0f; acc[0] += buf_[i] * a; acc[1] += buf_[i + 1] * a; acc[2] += buf_[i + 2] * a; acc[3] += a; n += 1; }
        const uint8_t mean[4] = {static_cast<uint8_t>(acc[3] > 0 ? acc[0] / acc[3] + 0.5f : 0), static_cast<uint8_t>(acc[3] > 0 ? acc[1] / acc[3] + 0.5f : 0), static_cast<uint8_t>(acc[3] > 0 ? acc[2] / acc[3] + 0.5f : 0), static_cast<uint8_t>(n > 0 ? acc[3] / n * 255.0f + 0.5f : 0)};
        for (size_t i = 0; i < buf_.size(); i += 4) std::memcpy(&buf_[i], mean, 4);
    }
    // Color Smudge stamp: the carried patch tinted by the paint color is the
    // dab; it goes down at the brush opacity, then what the brush carries on
    // is that dab faded toward the fresh pixels underneath by the length.
    void color_stamp(App& app, float cx, float cy) {
        Image& px = app.paint_pixels(layer_);
        const int r = rad_, W = 2 * r + 1;
        const int ox = static_cast<int>(std::floor(cx)) - r, oy = static_cast<int>(std::floor(cy)) - r;
        const Mask& clip = app.doc->selection();
        const float rate = std::clamp(app.csmudge_rate / 100.0f, 0.0f, 1.0f);
        const float length = std::clamp(app.csmudge_length / 100.0f, 0.0f, 1.0f);
        const float opacity = std::clamp(app.brush.opacity, 0.0f, 1.0f);
        const uint8_t pc[3] = {paint_.r, paint_.g, paint_.b};
        // What lies under the brush before this dab: the color the stroke picks up.
        std::vector<uint8_t> carried = std::move(buf_);
        grab(app, cx, cy);
        std::vector<uint8_t> fresh = std::move(buf_);
        buf_ = std::move(carried);
        std::vector<uint8_t> dab(buf_.size());
        for (size_t i = 0; i < buf_.size(); i += 4) {
            const float a = buf_[i + 3] / 255.0f;
            const float k = rate + (1.0f - rate) * (1.0f - a);   // where nothing is carried, the paint color fills in
            for (int c = 0; c < 3; ++c) dab[i + c] = static_cast<uint8_t>(buf_[i + c] + (pc[c] - buf_[i + c]) * k + 0.5f);
            dab[i + 3] = static_cast<uint8_t>(buf_[i + 3] + (255 - buf_[i + 3]) * rate + 0.5f);
        }
        for (int y = 0; y < W; ++y)
            for (int x = 0; x < W; ++x) {
                const int dx = ox + x, dy = oy + y;
                if (dx < 0 || dy < 0 || dx >= px.width() || dy >= px.height()) continue;
                float cov = coverage(app, (dx + 0.5f) - cx, (dy + 0.5f) - cy) * opacity;
                if (!clip.empty()) cov *= clip.at(dx, dy) / 255.0f;
                if (cov <= 0.0f) continue;
                const uint8_t* d = &dab[(static_cast<size_t>(y) * W + x) * 4];
                raster::blend_over(px, dx, dy, Color{d[0], d[1], d[2], d[3]}, cov);
            }
        // Carry on: the dab fading toward what was underneath, by the length.
        for (size_t i = 0; i < buf_.size(); ++i) buf_[i] = static_cast<uint8_t>(fresh[i] + (dab[i] - fresh[i]) * length + 0.5f);
    }
    void stamp(App& app, float cx, float cy) {
        if (color_) { color_stamp(app, cx, cy); return; }
        Image& px = app.paint_pixels(layer_);
        const int r = rad_, W = 2 * r + 1;
        const int ox = static_cast<int>(std::floor(cx)) - r, oy = static_cast<int>(std::floor(cy)) - r;
        const float strength = push_ ? 1.0f : app.tool_state->retouch_amount / 100.0f;
        const Mask& clip = app.doc->selection();
        for (int y = 0; y < W; ++y)
            for (int x = 0; x < W; ++x) {
                const int dx = ox + x, dy = oy + y;
                if (dx < 0 || dy < 0 || dx >= px.width() || dy >= px.height()) continue;
                float cov = coverage(app, (dx + 0.5f) - cx, (dy + 0.5f) - cy) * strength;
                if (!clip.empty()) cov *= clip.at(dx, dy) / 255.0f;
                if (cov <= 0.0f) continue;
                uint8_t* d = px.data() + (static_cast<size_t>(dy) * px.width() + dx) * 4;
                const uint8_t* s = &buf_[(static_cast<size_t>(y) * W + x) * 4];
                for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(d[c] + (s[c] - d[c]) * cov + 0.5f);
            }
        grab(app, cx, cy);
    }
    bool color_ = false, active_ = false, push_ = false;
    Color paint_{0, 0, 0, 255};
    size_t layer_ = 0;
    Image before_;
    std::vector<uint8_t> buf_;
    int rad_ = 0;
    float last_x_ = 0, last_y_ = 0;
};

// --- Red-eye Removal ---------------------------------------------------

class RedEyeTool : public Tool {
public:
    const char* category() const override { return "Retouch"; }
    const char* name() const override { return "Red-eye Removal"; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!in.inside || !app.active_is_raster()) return;
        const size_t layer = app.active_layer();
        Image& target = app.paint_pixels(layer);
        Image before = target;
        adjust::red_eye(target, in.img_x, in.img_y, app.brush.size * 0.5f, app.tool_state->redeye_strength);
        const int r = static_cast<int>(app.brush.size * 0.5f) + 2;
        const raster::Rect rect{static_cast<int>(in.img_x) - r, static_cast<int>(in.img_y) - r, static_cast<int>(in.img_x) + r, static_cast<int>(in.img_y) + r};
        app.paint_touched(layer, &rect);
        app.commit_pixels(layer, name(), std::move(before), target);
    }
    void draw_overlay(App& app, const ToolInput& in) override {
        const float r = app.brush.size * 0.5f * in.zoom;
        in.dl->AddCircle(in.screen, r, IM_COL32(255, 60, 60, 220), 0, 1.5f);
    }
    void draw_options(App& app) override {
        ImGui::SetNextItemWidth(140);
        ImGui::SliderFloat("Size", &app.brush.size, 2.0f, 200.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        float st = app.tool_state->redeye_strength * 100.0f;
        if (ImGui::SliderFloat("Strength", &st, 10.0f, 100.0f, "%.0f%%")) app.tool_state->redeye_strength = st / 100.0f;
        ImGui::SameLine();
        ImGui::TextDisabled("Click on the pupil.");
    }
};

// --- Move --------------------------------------------------------------
// Left drag moves the active layer's pixels; right drag moves the selection
// marquee, as the original's Mover does.

class MoveTool : public Tool {
public:
    const char* category() const override { return "Edit"; }
    bool wants_snap() const override { return true; }
    const char* name() const override { return "Move"; }
    const char* shortcut() const override { return "M"; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!app.doc || app.active_layer() < 0) return;
        if (b == ImGuiMouseButton_Left && !app.active_is_raster()) return;
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
    const char* category() const override { return "Fill"; }
    const char* name() const override { return "Flood Fill"; }
    const char* shortcut() const override { return "F"; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!in.inside || !app.active_is_raster()) return;
        const size_t layer = app.active_layer();
        Image& target = app.paint_pixels(layer);
        Image before = target;
        const bool fg = b == ImGuiMouseButton_Left;
        const App::Material& mat = fg ? app.fg_material : app.bg_material;
        raster::Rect changed;
        if (mat.kind == 0) {
            const Color color = to_color(fg ? app.fg_color : app.bg_color);
            changed = raster::flood_fill(target, static_cast<int>(std::floor(in.img_x)),
                                         static_cast<int>(std::floor(in.img_y)), color,
                                         app.tool_state->fill_tolerance, app.tool_state->fill_opacity, app.paint_clip(app.active_layer()));
        } else {
            // Gradient or pattern: fill the matching region through the material.
            Mask region = mask::magic_wand(target, static_cast<int>(std::floor(in.img_x)), static_cast<int>(std::floor(in.img_y)), app.tool_state->fill_tolerance, true);
            if (app.doc->has_selection()) mask::combine(region, app.doc->selection(), mask::Combine::Intersect);
            int rx0 = target.width(), ry0 = target.height(), rx1 = -1, ry1 = -1;
            std::vector<uint8_t> cov(region.data(), region.data() + region.size());
            for (int y = 0; y < region.height(); ++y)
                for (int x = 0; x < region.width(); ++x)
                    if (region.at(x, y)) { rx0 = std::min(rx0, x); ry0 = std::min(ry0, y); rx1 = std::max(rx1, x); ry1 = std::max(ry1, y); }
            if (rx1 < rx0) return;
            if (app.tool_state->fill_opacity < 1.0f) for (uint8_t& c : cov) c = static_cast<uint8_t>(c * app.tool_state->fill_opacity + 0.5f);
            vec::paint(target, cov, target.width(), target.height(), app.material_style(fg), static_cast<float>(rx0), static_cast<float>(ry0), static_cast<float>(rx1 + 1), static_cast<float>(ry1 + 1));
            changed = raster::Rect{rx0, ry0, rx1 + 1, ry1 + 1};
        }
        if (changed.empty()) return;
        app.paint_touched(layer, &changed);
        app.commit_pixels(layer, name(), std::move(before), target);
    }
    void draw_options(App& app) override {
        ImGui::SetNextItemWidth(140);
        ImGui::SliderInt("Tolerance", &app.tool_state->fill_tolerance, 0, 200);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        float op = app.tool_state->fill_opacity * 100.0f;
        if (ImGui::SliderFloat("Opacity", &op, 1.0f, 100.0f, "%.0f")) app.tool_state->fill_opacity = op / 100.0f;
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

// Selection tool shapes, in the original's order.
static const char* const kSelectionShapes = "Rectangle\0Square\0Rounded Rectangle\0Rounded Square\0Ellipse\0Circle\0Triangle\0Pentagon\0Hexagon\0Octagon\0Star\0Arrow\0";

// The mask for a selection shape dragged from (x0, y0) to (x1, y1).
Mask selection_shape(int shape, int w, int h, float x0, float y0, float x1, float y1, bool aa) {
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, rx = std::abs(x1 - x0) * 0.5f, ry = std::abs(y1 - y0) * 0.5f;
    const float lx = std::min(x0, x1), ly = std::min(y0, y1), hx = std::max(x0, x1), hy = std::max(y0, y1);
    switch (shape) {
        case 2: case 3: return mask::rounded_rectangle(w, h, lx, ly, hx, hy, std::min(rx, ry) * 0.3f, aa);
        case 4: case 5: return mask::ellipse(w, h, cx, cy, rx, ry, aa);
        case 6: return mask::regular_polygon(w, h, cx, cy, rx, ry, 3, 0.0f, aa);
        case 7: return mask::regular_polygon(w, h, cx, cy, rx, ry, 5, 0.0f, aa);
        case 8: return mask::regular_polygon(w, h, cx, cy, rx, ry, 6, 0.0f, aa);
        case 9: return mask::regular_polygon(w, h, cx, cy, rx, ry, 8, 22.5f, aa);
        case 10: return mask::star(w, h, cx, cy, rx, ry, 5, 0.45f, 0.0f, aa);
        case 11: return mask::polygon(w, h, {{lx, cy - ry * 0.4f}, {cx, cy - ry * 0.4f}, {cx, ly}, {hx, cy}, {cx, hy}, {cx, cy + ry * 0.4f}, {lx, cy + ry * 0.4f}}, aa);
        default: return mask::rectangle(w, h, lx, ly, hx, hy, aa);
    }
}

class SelectionTool : public Tool {
public:
    const char* category() const override { return "Selection"; }
    bool wants_snap() const override { return true; }
    const char* name() const override { return "Selection"; }
    const char* shortcut() const override { return "S"; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!app.doc) return;
        dragging_ = true;
        x0_ = x1_ = in.img_x;
        y0_ = y1_ = in.img_y;
        mode_ = gesture_mode(app);
    }
    // Square / circle shapes keep the drag square.
    void constrain(App& app, float& x1, float& y1) const {
        if (app.sel_shape == 1 || app.sel_shape == 3 || app.sel_shape == 5) {
            const float d = std::max(std::abs(x1 - x0_), std::abs(y1 - y0_));
            x1 = x0_ + (x1 >= x0_ ? d : -d);
            y1 = y0_ + (y1 >= y0_ ? d : -d);
        }
    }
    void on_drag(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!dragging_) return;
        x1_ = in.img_x;
        y1_ = in.img_y;
        constrain(app, x1_, y1_);
    }
    void on_release(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!dragging_ || !app.doc) return;
        dragging_ = false;
        x1_ = in.img_x; y1_ = in.img_y;
        constrain(app, x1_, y1_);
        const int w = app.doc->width(), h = app.doc->height();
        if (std::abs(x1_ - x0_) < 1.0f || std::abs(y1_ - y0_) < 1.0f) {
            // A click without a drag deselects, like the original.
            app.select_none();
            return;
        }
        Mask shape = selection_shape(app.sel_shape, w, h, x0_, y0_, x1_, y1_, app.sel_antialias);
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
        if (app.sel_shape != 4 && app.sel_shape != 5) {
            in.dl->AddRect(a, b, IM_COL32(0, 0, 0, 255));
            in.dl->AddRect(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 1), IM_COL32(255, 255, 255, 255));
        } else {
            const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            in.dl->AddEllipse(c, ImVec2(std::abs(b.x - a.x) * 0.5f, std::abs(b.y - a.y) * 0.5f), IM_COL32(0, 0, 0, 255), 0.0f, 0, 1.0f);
            in.dl->AddEllipse(c, ImVec2(std::abs(b.x - a.x) * 0.5f - 1, std::abs(b.y - a.y) * 0.5f - 1), IM_COL32(255, 255, 255, 255), 0.0f, 0, 1.0f);
        }
    }
    void draw_options(App& app) override {
        ImGui::SetNextItemWidth(150);
        ImGui::Combo("Shape", &app.sel_shape, kSelectionShapes);
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
    const char* category() const override { return "Selection"; }
    const char* name() const override { return "Freehand Selection"; }
    const char* shortcut() const override { return "L"; }
    bool overlay_always() const override { return polygon_open_; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!app.doc) return;
        const int type = app.sel_freehand_type;
        if (type == 1 || type == 2) {
            // Point to Point / Smart Edge: a click adds a vertex; a double
            // click, Enter, or a click on the first point closes.
            if (!polygon_open_) {
                pts_.clear(); vertices_.clear();
                polygon_open_ = true;
                mode_ = gesture_mode(app);
                if (type == 2) edges_ = mask::edge_map(app.doc->composite());
                vertices_.emplace_back(in.img_x, in.img_y);
                pts_ = vertices_;
                return;
            }
            const bool near_first = std::hypot(in.img_x - vertices_.front().first, in.img_y - vertices_.front().second) * in.zoom < 8.0f && vertices_.size() > 2;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || near_first) { close_polygon(app); return; }
            add_vertex(app, in.img_x, in.img_y);
            return;
        }
        pts_.clear();
        mode_ = gesture_mode(app);
        if (type == 3) edges_ = mask::edge_map(app.doc->composite());
        pts_.push_back(seek(app, in.img_x, in.img_y));
        dragging_ = true;
    }
    void on_drag(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (polygon_open_ || pts_.empty()) return;
        const auto p = seek(app, in.img_x, in.img_y);
        const auto& l = pts_.back();
        if (std::abs(l.first - p.first) >= 0.5f || std::abs(l.second - p.second) >= 0.5f) pts_.push_back(p);
    }
    void on_release(App& app, const ToolInput&, ImGuiMouseButton) override {
        if (!dragging_) return;  // releases of point-to-point clicks are not gestures
        dragging_ = false;
        if (!app.doc) { pts_.clear(); return; }
        finish(app, app.sel_freehand_type == 3 ? "Edge Seeker Selection" : "Freehand Selection");
    }
    void cancel(App&) override { pts_.clear(); vertices_.clear(); polygon_open_ = false; dragging_ = false; }
    void draw_overlay(App& app, const ToolInput& in) override {
        if (polygon_open_) {
            // Keys while the polygon is open: Enter closes, Backspace drops the last vertex.
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) { close_polygon(app); return; }
            if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) && vertices_.size() > 1) { vertices_.pop_back(); rebuild_points(app); }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { cancel(app); return; }
            // Rubber band from the last vertex to the cursor.
            if (app.sel_freehand_type == 2 && app.doc) {
                if (std::abs(in.img_x - live_target_.first) > 0.5f || std::abs(in.img_y - live_target_.second) > 0.5f) {
                    live_target_ = {in.img_x, in.img_y};
                    live_ = mask::edge_path(edges_, app.doc->width(), app.doc->height(), vertices_.back(), live_target_);
                }
            } else {
                live_ = {vertices_.back(), {in.img_x, in.img_y}};
            }
        }
        auto line = [&](const std::pair<float, float>& a, const std::pair<float, float>& b, ImU32 col, float th) {
            in.dl->AddLine(ImVec2(in.origin.x + a.first * in.zoom, in.origin.y + a.second * in.zoom), ImVec2(in.origin.x + b.first * in.zoom, in.origin.y + b.second * in.zoom), col, th);
        };
        for (size_t i = 0; i + 1 < pts_.size(); ++i) { line(pts_[i], pts_[i + 1], IM_COL32(0, 0, 0, 255), 3.0f); line(pts_[i], pts_[i + 1], IM_COL32(255, 255, 255, 255), 1.0f); }
        if (polygon_open_) {
            for (size_t i = 0; i + 1 < live_.size(); ++i) line(live_[i], live_[i + 1], IM_COL32(255, 255, 0, 220), 1.0f);
            for (const auto& v : vertices_) in.dl->AddRectFilled(ImVec2(in.origin.x + v.first * in.zoom - 3, in.origin.y + v.second * in.zoom - 3), ImVec2(in.origin.x + v.first * in.zoom + 3, in.origin.y + v.second * in.zoom + 3), IM_COL32(255, 255, 255, 255));
        }
    }
    void draw_options(App& app) override {
        ImGui::SetNextItemWidth(130);
        if (ImGui::Combo("Type", &app.sel_freehand_type, "Freehand\0Point to Point\0Smart Edge\0Edge Seeker\0")) cancel(app);
        ImGui::SameLine();
        if (app.sel_freehand_type == 3) { ImGui::SetNextItemWidth(90); ImGui::SliderInt("Range", &app.sel_range, 1, 50); ImGui::SameLine(); }
        ImGui::SetNextItemWidth(90);
        ImGui::SliderInt("Smoothing", &app.sel_smoothing, 0, 100);
        ImGui::SameLine();
        draw_selection_common(app);
        if (app.sel_freehand_type == 1 || app.sel_freehand_type == 2) { ImGui::SameLine(); ImGui::TextDisabled("Click to add points; double-click or Enter closes, Backspace removes"); }
    }

private:
    std::pair<float, float> seek(const App& app, float x, float y) const {
        if (app.sel_freehand_type == 3 && app.doc && !edges_.empty()) return mask::seek_edge(edges_, app.doc->width(), app.doc->height(), x, y, app.sel_range);
        return {x, y};
    }
    void add_vertex(App& app, float x, float y) {
        vertices_.emplace_back(x, y);
        rebuild_points(app);
    }
    // Points along the polygon: the vertices, or edge-hugging paths between them.
    void rebuild_points(App& app) {
        pts_.clear();
        if (app.sel_freehand_type == 2 && app.doc) {
            for (size_t i = 0; i < vertices_.size(); ++i) {
                if (i == 0) { pts_.push_back(vertices_[0]); continue; }
                auto seg = mask::edge_path(edges_, app.doc->width(), app.doc->height(), vertices_[i - 1], vertices_[i]);
                pts_.insert(pts_.end(), seg.begin() + (seg.empty() ? 0 : 1), seg.end());
            }
        } else {
            pts_ = vertices_;
        }
    }
    void close_polygon(App& app) {
        if (!app.doc) { cancel(app); return; }
        if (app.sel_freehand_type == 2 && vertices_.size() > 1) {
            auto seg = mask::edge_path(edges_, app.doc->width(), app.doc->height(), vertices_.back(), vertices_.front());
            pts_.insert(pts_.end(), seg.begin() + 1, seg.end());
        }
        polygon_open_ = false;
        vertices_.clear(); live_.clear();
        finish(app, app.sel_freehand_type == 2 ? "Smart Edge Selection" : "Point to Point Selection");
    }
    void finish(App& app, const char* label) {
        if (pts_.size() < 3) { pts_.clear(); app.select_none(); return; }
        std::vector<std::pair<float, float>> outline = app.sel_smoothing > 0 ? mask::smooth_polygon(pts_, app.sel_smoothing, true) : pts_;
        Mask shape = mask::polygon(app.doc->width(), app.doc->height(), outline, app.sel_antialias);
        pts_.clear();
        const int saved = app.sel_mode;
        app.sel_mode = mode_;
        app.apply_selection_gesture(label, std::move(shape));
        app.sel_mode = saved;
    }
    std::vector<std::pair<float, float>> pts_, vertices_, live_;
    std::pair<float, float> live_target_{-1.0f, -1.0f};
    std::vector<float> edges_;
    bool polygon_open_ = false;
    bool dragging_ = false;
    int mode_ = 0;
};

class MagicWandTool : public Tool {
public:
    const char* category() const override { return "Selection"; }
    const char* name() const override { return "Magic Wand"; }
    const char* shortcut() const override { return "W"; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!in.inside || !app.doc || app.active_layer() < 0) return;
        const int x = static_cast<int>(std::floor(in.img_x)), y = static_cast<int>(std::floor(in.img_y));
        Mask shape = app.wand_sample_merged
            ? mask::magic_wand(app.doc->composite(), x, y, app.tool_state->wand_tolerance, app.wand_contiguous)
            : mask::magic_wand(app.doc->layer(app.active_layer()).pixels, x, y, app.tool_state->wand_tolerance, app.wand_contiguous);
        const int saved = app.sel_mode;
        app.sel_mode = gesture_mode(app);
        app.apply_selection_gesture("Magic Wand", std::move(shape));
        app.sel_mode = saved;
    }
    void draw_options(App& app) override {
        ImGui::SetNextItemWidth(110);
        ImGui::SliderInt("Tolerance", &app.tool_state->wand_tolerance, 0, 200);
        ImGui::SameLine();
        ImGui::Checkbox("Contiguous", &app.wand_contiguous);
        ImGui::SameLine();
        ImGui::Checkbox("Sample merged", &app.wand_sample_merged);
        ImGui::SameLine();
        draw_selection_common(app);
    }
};

// --- Foreground Select --------------------------------------------------
// Scribble on the object (left button) and, if needed, on the background
// (right button); after every stroke the selection is recomputed from the
// marks (mask::foreground_select). A selection made beforehand acts as the
// rough outline: everything outside it is background.

class ForegroundSelectTool : public Tool {
public:
    const char* category() const override { return "Selection"; }
    const char* name() const override { return "Foreground Select"; }
    bool overlay_always() const override { return !strokes_.empty(); }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!in.inside || !app.doc || app.active_layer() < 0) return;
        sync(app);
        if (strokes_.empty()) region_ = app.doc->selection();
        strokes_.push_back({b == ImGuiMouseButton_Right, static_cast<float>(app.fgsel_size), {{in.img_x, in.img_y}}});
        drawing_ = true;
    }
    void on_drag(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!drawing_ || !app.doc) return;
        strokes_.back().pts.emplace_back(in.img_x, in.img_y);
    }
    void on_release(App& app, const ToolInput&, ImGuiMouseButton) override {
        if (!drawing_ || !app.doc) return;
        drawing_ = false;
        rasterize(strokes_.back());
        compute(app);
    }
    void cancel(App&) override { drawing_ = false; }
    void draw_overlay(App& app, const ToolInput& in) override {
        if (app.doc.get() == doc_) for (const MarkStroke& s : strokes_) {
            const ImU32 col = s.background ? IM_COL32(255, 70, 70, 150) : IM_COL32(60, 220, 90, 150);
            const float th = std::max(s.size * in.zoom, 2.0f);
            std::vector<ImVec2> pts;
            pts.reserve(s.pts.size());
            for (const auto& p : s.pts) pts.emplace_back(in.origin.x + p.first * in.zoom, in.origin.y + p.second * in.zoom);
            if (pts.size() == 1) in.dl->AddCircleFilled(pts[0], th * 0.5f, col);
            else in.dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, ImDrawFlags_RoundCornersAll, th);
        }
        if (in.inside) {
            const float r = app.fgsel_size * 0.5f * in.zoom;
            in.dl->AddCircle(in.screen, r, IM_COL32(0, 0, 0, 200), 0, 1.0f);
            in.dl->AddCircle(in.screen, r + 1.0f, IM_COL32(255, 255, 255, 160), 0, 1.0f);
        }
    }
    void draw_options(App& app) override {
        ImGui::SetNextItemWidth(110);
        ImGui::SliderInt("Size", &app.fgsel_size, 1, 200);
        ImGui::SameLine();
        ImGui::Checkbox("Sample merged", &app.fgsel_merged);
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear marks")) { strokes_.clear(); fg_ = Mask(); bg_ = Mask(); }
        ImGui::SameLine();
        ImGui::TextDisabled("Left: mark the object. Right: mark background. Select first for a rough outline.");
    }

private:
    struct MarkStroke { bool background; float size; std::vector<std::pair<float, float>> pts; };
    // Marks belong to one image size; a different image starts over.
    void sync(const App& app) {
        if (app.doc.get() != doc_ || fg_.width() != app.doc->width() || fg_.height() != app.doc->height()) {
            doc_ = app.doc.get();
            fg_ = Mask(app.doc->width(), app.doc->height(), 0);
            bg_ = Mask(app.doc->width(), app.doc->height(), 0);
            strokes_.clear();
            region_ = Mask();
        }
    }
    void rasterize(const MarkStroke& s) {
        Mask line = mask::polyline(fg_.width(), fg_.height(), s.pts, s.size, false);
        mask::combine(s.background ? bg_ : fg_, line, mask::Combine::Add);
    }
    void compute(App& app) {
        const Image& src = app.fgsel_merged ? app.doc->composite() : app.doc->layer(app.active_layer()).pixels;
        Mask m = mask::foreground_select(src, fg_, bg_, region_);
        app.set_selection("Foreground Select", std::move(m));
    }
    std::vector<MarkStroke> strokes_;
    Mask fg_, bg_, region_;
    const Document* doc_ = nullptr;   // the image the marks belong to
    bool drawing_ = false;
};

// --- Crop --------------------------------------------------------------

class CropTool : public Tool {
public:
    const char* category() const override { return "Edit"; }
    bool wants_snap() const override { return true; }
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

// --- Picture Tube ------------------------------------------------------
// Stamps cells of a tube sheet along the stroke. Placement: random jitter
// or continuous spacing; selection: random, incremental or angular (by the
// direction of motion), as in the original.

class PictureTubeTool : public Tool {
public:
    const char* category() const override { return "Fill"; }
    const char* name() const override { return "Picture Tube"; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton) override {
        app.ensure_tubes();
        if (!app.active_is_raster() || app.tube_image.empty()) { if (app.tube_image.empty()) app.status = "No picture tubes found (put .PspTube files in " + Config::directory() + "/tubes)."; return; }
        layer_ = app.active_layer();
        before_ = app.paint_pixels(layer_);
        active_ = true;
        last_x_ = in.img_x; last_y_ = in.img_y;
        carry_ = 0.0f;
        stamp(app, in.img_x, in.img_y, 0.0f);
    }
    void on_drag(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!active_) return;
        const float step = std::max(1.0f, (app.tool_state->tube_step_override > 0 ? app.tool_state->tube_step_override : std::max(1, app.tube_info.step)) * app.tool_state->tube_scale);
        const float dx = in.img_x - last_x_, dy = in.img_y - last_y_;
        const float len = std::hypot(dx, dy);
        if (len <= 0.0f) return;
        const float angle = std::atan2(-dy, dx);
        float t = step - carry_;
        while (t <= len) {
            stamp(app, last_x_ + dx * t / len, last_y_ + dy * t / len, angle);
            t += step;
        }
        carry_ = len - (t - step);
        last_x_ = in.img_x; last_y_ = in.img_y;
    }
    void on_release(App& app, const ToolInput&, ImGuiMouseButton) override {
        if (!active_) return;
        active_ = false;
        app.commit_pixels(layer_, name(), before_, app.paint_pixels(layer_));
    }
    void cancel(App& app) override {
        if (active_ && app.doc && layer_ < app.doc->layer_count()) { app.paint_pixels(layer_) = before_; app.paint_touched(layer_); }
        active_ = false;
    }
    void draw_overlay(App& app, const ToolInput& in) override {
        if (app.tube_image.empty()) return;
        const float cw = app.tube_image.width() / static_cast<float>(app.tube_info.columns) * app.tool_state->tube_scale * in.zoom;
        const float ch = app.tube_image.height() / static_cast<float>(app.tube_info.rows) * app.tool_state->tube_scale * in.zoom;
        in.dl->AddRect(ImVec2(in.screen.x - cw * 0.5f, in.screen.y - ch * 0.5f), ImVec2(in.screen.x + cw * 0.5f, in.screen.y + ch * 0.5f), IM_COL32(255, 255, 255, 160));
    }
    void draw_options(App& app) override {
        app.ensure_tubes();
        ImGui::SetNextItemWidth(200);
        const char* current = app.tube_index >= 0 ? app.tubes[app.tube_index].name.c_str() : "(none)";
        if (ImGui::BeginCombo("Tube", current)) {
            for (size_t i = 0; i < app.tubes.size(); ++i)
                if (ImGui::Selectable(app.tubes[i].name.c_str(), static_cast<int>(i) == app.tube_index)) app.load_tube(static_cast<int>(i));
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        float pct = app.tool_state->tube_scale * 100.0f;
        if (ImGui::SliderFloat("Scale", &pct, 10.0f, 250.0f, "%.0f%%")) app.tool_state->tube_scale = pct / 100.0f;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputInt("Step", &app.tool_state->tube_step_override);
        app.tool_state->tube_step_override = std::max(0, app.tool_state->tube_step_override);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::Combo("Placement", &app.tool_state->tube_placement, "As tube\0Random\0Continuous\0");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::Combo("Selection", &app.tool_state->tube_selection, "As tube\0Random\0Incremental\0Angular\0");
        if (app.tube_index >= 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("%d cells, step %d", app.tube_info.total, app.tube_info.step);
        }
    }

private:
    void stamp(App& app, float cx, float cy, float angle) {
        const io::TubeInfo& t = app.tube_info;
        const int placement = app.tool_state->tube_placement ? app.tool_state->tube_placement : t.placement;
        const int selection = app.tool_state->tube_selection ? app.tool_state->tube_selection : t.selection;
        // Random placement jitters the position by up to a quarter step.
        if (placement == 1) {
            const float step = std::max(1.0f, (app.tool_state->tube_step_override > 0 ? app.tool_state->tube_step_override : std::max(1, t.step)) * app.tool_state->tube_scale);
            cx += (rnd() - 0.5f) * step * 0.5f;
            cy += (rnd() - 0.5f) * step * 0.5f;
        }
        int cell;
        if (selection == 2) cell = next_cell_++ % t.total;
        else if (selection == 3) { float a = angle; if (a < 0) a += 6.2831853f; cell = static_cast<int>(a / 6.2831853f * t.total) % t.total; }
        else cell = static_cast<int>(rnd() * t.total) % t.total;
        const int cw = app.tube_image.width() / t.columns, ch = app.tube_image.height() / t.rows;
        const int col = cell % t.columns, row = cell / t.columns;
        Image tile = raster::crop(app.tube_image, {col * cw, row * ch, (col + 1) * cw, (row + 1) * ch});
        const int ow = std::max(1, static_cast<int>(cw * app.tool_state->tube_scale + 0.5f)), oh = std::max(1, static_cast<int>(ch * app.tool_state->tube_scale + 0.5f));
        if (ow != cw || oh != ch) tile = raster::resample(tile, ow, oh, raster::Filter::Bilinear);
        Image& target = app.paint_pixels(layer_);
        const int ox = static_cast<int>(std::floor(cx)) - ow / 2, oy = static_cast<int>(std::floor(cy)) - oh / 2;
        const Mask& clip = app.doc->selection();
        for (int y = 0; y < oh; ++y)
            for (int x = 0; x < ow; ++x) {
                const int dx = ox + x, dy = oy + y;
                if (dx < 0 || dy < 0 || dx >= target.width() || dy >= target.height()) continue;
                const Color c = tile.get(x, y);
                if (c.a == 0) continue;
                const float cov = clip.empty() ? 1.0f : clip.at(dx, dy) / 255.0f;
                raster::blend_over(target, dx, dy, c, cov);
            }
        const raster::Rect r{ox, oy, ox + ow, oy + oh};
        app.paint_touched(layer_, &r);
    }
    float rnd() { seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5; return (seed_ & 0xFFFFFF) / 16777216.0f; }
    bool active_ = false;
    size_t layer_ = 0;
    Image before_;
    float last_x_ = 0, last_y_ = 0, carry_ = 0;
    int next_cell_ = 0;
    uint32_t seed_ = 0x9E3779B9u;
};

// --- Text --------------------------------------------------------------

class TextTool : public Tool {
public:
    const char* category() const override { return "Text and Shapes"; }
    bool wants_snap() const override { return true; }
    const char* name() const override { return "Text"; }
    const char* shortcut() const override { return "T"; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!app.doc || b != ImGuiMouseButton_Left) return;
        app.text_x = static_cast<int>(std::floor(in.img_x));
        app.text_y = static_cast<int>(std::floor(in.img_y));
        app.text_edit_object = -1;
        app.show_text_dialog = true;
    }
    void draw_overlay(App&, const ToolInput& in) override {
        in.dl->AddLine(ImVec2(in.screen.x, in.screen.y - 8), ImVec2(in.screen.x, in.screen.y + 8), IM_COL32(255, 255, 255, 220));
        in.dl->AddLine(ImVec2(in.screen.x - 8, in.screen.y), ImVec2(in.screen.x + 8, in.screen.y), IM_COL32(255, 255, 255, 220));
    }
    void draw_options(App& app) override {
        app.draw_create_as_vector();
        ImGui::SameLine();
        ImGui::TextUnformatted("Click where the text's top-left corner should go.");
    }
};

// --- Line and Preset Shapes --------------------------------------------
// Both preview on a snapshot of the layer while dragging and commit one
// history entry. Stroke uses the foreground material, fill the background.

class ShapeToolBase : public Tool {
public:
    const char* category() const override { return "Text and Shapes"; }
    bool wants_snap() const override { return true; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!app.doc) return;
        button_ = b;
        vector_ = app.create_as_vector;
        if (vector_) {
            layer_ = app.vector_layer_for_edit(true);
            if (layer_ < 0) return;
            before_objects_ = app.doc->layer(layer_).objects;
        } else {
            if (!app.active_is_raster()) return;
            layer_ = app.active_layer();
            before_ = app.paint_pixels(layer_);
        }
        x0_ = x1_ = in.img_x; y0_ = y1_ = in.img_y;
        active_ = true;
    }
    void on_drag(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!active_) return;
        x1_ = in.img_x; y1_ = in.img_y;
        std::vector<vec::Object> objs = build(app);
        if (vector_) {
            auto& L = app.doc->layer(layer_).objects;
            L = before_objects_;
            for (vec::Object& o : L) o.selected = false;
            for (vec::Object& o : objs) { o.selected = true; L.push_back(o); }
            app.doc->rasterize_vector_layer(layer_);
            return;
        }
        // Raster: render the objects, then composite through the selection.
        Image work = before_;
        Image shape(work.width(), work.height(), {0, 0, 0, 0});
        vec::rasterize(objs, shape);
        for (int y = 0; y < work.height(); ++y)
            for (int x = 0; x < work.width(); ++x) {
                const Color c = shape.get(x, y);
                if (c.a) raster::blend_over(work, x, y, c, 1.0f);
            }
        if (app.doc->has_selection()) raster::apply_through_mask(work, before_, app.doc->selection());
        app.paint_pixels(layer_) = std::move(work);
        app.paint_touched(layer_);
    }
    void on_release(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!active_) return;
        on_drag(app, in, ImGuiMouseButton_Left);
        active_ = false;
        if (vector_) app.objects_changed(name(), before_objects_);
        else app.commit_pixels(layer_, name(), before_, app.paint_pixels(layer_));
    }
    void cancel(App& app) override {
        if (active_ && app.doc && layer_ < static_cast<int>(app.doc->layer_count())) {
            if (vector_) { app.doc->layer(layer_).objects = before_objects_; app.doc->rasterize_vector_layer(layer_); }
            else { app.paint_pixels(layer_) = before_; app.paint_touched(layer_); }
        }
        active_ = false;
    }

protected:
    // The objects for the current drag, styled from the materials.
    virtual std::vector<vec::Object> build(App& app) = 0;
    bool active_ = false, vector_ = false;
    int layer_ = 0;
    ImGuiMouseButton button_ = ImGuiMouseButton_Left;
    Image before_;
    std::vector<vec::Object> before_objects_;
    float x0_ = 0, y0_ = 0, x1_ = 0, y1_ = 0;
};

class LineTool : public ShapeToolBase {
public:
    const char* name() const override { return "Line"; }
    const char* shortcut() const override { return "V"; }
    void draw_options(App& app) override {
        ImGui::SetNextItemWidth(120);
        ImGui::SliderFloat("Width", &app.line_width, 1.0f, 100.0f, "%.0f");
        ImGui::SameLine();
        ImGui::Checkbox("Anti-alias", &app.shape_antialias);
        ImGui::SameLine();
        app.draw_line_style_combo();
        ImGui::SameLine();
        app.draw_create_as_vector();
        ImGui::SameLine();
        ImGui::TextDisabled("Left draws with the foreground material, right with the background.");
    }

protected:
    std::vector<vec::Object> build(App& app) override {
        vec::Object o = vec::make_polygon({{x0_, y0_}, {x1_, y1_}}, false);
        app.apply_object_style(o, true, false, button_);
        return {o};
    }
};

class PresetShapeTool : public ShapeToolBase {
public:
    const char* name() const override { return "Preset Shape"; }
    const char* shortcut() const override { return "I"; }
    void draw_options(App& app) override {
        app.ensure_shape_library();
        ImGui::SetNextItemWidth(170);
        static const char* builtin[] = {"Rectangle", "Rounded Rectangle", "Ellipse", "Triangle", "Polygon", "Star"};
        const char* current = app.shape_library_index >= 0 && app.shape_library_index < static_cast<int>(app.shape_library.size())
                                  ? app.shape_library[app.shape_library_index].name.c_str() : builtin[std::clamp(app.shape_kind, 0, 5)];
        if (ImGui::BeginCombo("Shape", current)) {
            for (int i = 0; i < 6; ++i)
                if (ImGui::Selectable(builtin[i], app.shape_library_index < 0 && app.shape_kind == i)) { app.shape_kind = i; app.shape_library_index = -1; }
            if (!app.shape_library.empty()) ImGui::Separator();
            for (size_t i = 0; i < app.shape_library.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(app.shape_library[i].name.c_str(), app.shape_library_index == static_cast<int>(i))) app.shape_library_index = static_cast<int>(i);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (app.shape_library_index < 0) {
            if (app.shape_kind == 1) { ImGui::SameLine(); ImGui::SetNextItemWidth(90); ImGui::SliderFloat("Radius", &app.shape_radius, 1.0f, 200.0f, "%.0f"); }
            if (app.shape_kind == 4) { ImGui::SameLine(); ImGui::SetNextItemWidth(90); ImGui::SliderInt("Sides", &app.shape_sides, 3, 24); }
            if (app.shape_kind == 5) {
                ImGui::SameLine(); ImGui::SetNextItemWidth(90); ImGui::SliderInt("Points", &app.star_points, 3, 24);
                ImGui::SameLine(); ImGui::SetNextItemWidth(90); ImGui::SliderFloat("Inner", &app.star_inner, 0.1f, 1.0f, "%.2f");
            }
        } else {
            ImGui::SameLine();
            ImGui::Checkbox("Retain style", &app.shape_retain_style);
        }
        ImGui::SameLine();
        ImGui::Checkbox("Stroke", &app.shape_stroke);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::SliderFloat("Width", &app.line_width, 1.0f, 100.0f, "%.0f");
        ImGui::SameLine();
        ImGui::Checkbox("Fill", &app.shape_fill);
        ImGui::SameLine();
        ImGui::Checkbox("Anti-alias", &app.shape_antialias);
        ImGui::SameLine();
        app.draw_line_style_combo();
        ImGui::SameLine();
        app.draw_create_as_vector();
        ImGui::SameLine();
        ImGui::TextDisabled("Stroke: foreground material. Fill: background. Right button swaps them.");
    }

protected:
    std::vector<vec::Object> build(App& app) override {
        std::vector<vec::Object> objs = app.shape_objects(x0_, y0_, x1_, y1_);
        const bool retain = app.shape_library_index >= 0 && app.shape_retain_style;
        for (vec::Object& o : objs) {
            if (o.is_group) continue;
            if (retain) { o.antialias = app.shape_antialias; continue; }
            app.apply_object_style(o, app.shape_stroke, app.shape_fill, button_);
        }
        return objs;
    }
};

}  // namespace

std::vector<std::unique_ptr<Tool>> make_default_tools() {
    std::vector<std::unique_ptr<Tool>> t;
    t.push_back(std::make_unique<PanTool>());
    t.push_back(std::make_unique<ZoomTool>());
    t.push_back(std::make_unique<AssistantTool>());
    t.push_back(std::make_unique<MoveTool>());
    t.push_back(std::make_unique<CropTool>());
    auto warp = make_warp_tools();   // 0 Warp Brush, 1 Mesh Warp, 2 Scratch Remover, 3 Object Remover
    for (auto& d : make_deform_tools()) t.push_back(std::move(d));
    t.push_back(std::move(warp[1]));
    t.push_back(std::make_unique<SelectionTool>());
    t.push_back(std::make_unique<FreehandTool>());
    t.push_back(std::make_unique<MagicWandTool>());
    t.push_back(std::make_unique<ForegroundSelectTool>());
    t.push_back(std::make_unique<DropperTool>());
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Paint));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Airbrush));
    t.push_back(std::make_unique<SmudgeTool>(true));
    t.push_back(std::move(warp[0]));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::LightenDarken));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::DodgeBurn));
    t.push_back(std::make_unique<SmudgeTool>());
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Soften));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Sharpen));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Saturation));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Hue));
    t.push_back(std::make_unique<RedEyeTool>());
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Clone));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Heal));
    t.push_back(std::move(warp[2]));
    t.push_back(std::move(warp[3]));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::ColorReplacer));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Eraser));
    t.push_back(std::make_unique<FloodFillTool>());
    t.push_back(std::make_unique<PictureTubeTool>());
    t.push_back(std::make_unique<TextTool>());
    t.push_back(std::make_unique<LineTool>());
    t.push_back(std::make_unique<PresetShapeTool>());
    for (auto& v : make_vector_tools()) t.push_back(std::move(v));
    return t;
}
