#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include "App.h"
#include "firn/adjust.h"
#include "firn/commands.h"
#include "firn/io_psp.h"
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
    enum class Kind { Paint, Eraser, Airbrush, Clone, LightenDarken, DodgeBurn, Saturation, Hue, ColorReplacer, Soften, Sharpen };
    const char* category() const override {
        switch (kind_) {
            case Kind::Paint: case Kind::Airbrush: return "Paint";
            case Kind::Eraser: return "Erase";
            case Kind::Clone: case Kind::ColorReplacer: return "Clone and Replace";
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
        std::function<Color(const Image&, int, int)> area_filter;
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
        stroke_ = std::make_unique<raster::Stroke>(app.paint_pixels(layer_), brush, color, mode, &app.doc->selection());
        if (mode == raster::StrokeMode::Clone) stroke_->set_clone_source(&clone_src_, off_x_, off_y_);
        if (filter) stroke_->set_filter(std::move(filter));
        if (area_filter) stroke_->set_area_filter(std::move(area_filter));
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
        app.commit_pixels(layer_, name(), stroke_->base(), app.paint_pixels(layer_));
        stroke_.reset();
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
            stroke_->stamp_at(last_x_, last_y_);
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
            case Kind::LightenDarken: case Kind::DodgeBurn: case Kind::Saturation: case Kind::Hue: case Kind::Soften: case Kind::Sharpen:
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                ImGui::SliderInt("Amount", &app.retouch_amount, 1, 100, "%d%%");
                ImGui::SameLine();
                ImGui::TextDisabled(kind_ == Kind::LightenDarken ? "Left lightens, right darkens." :
                                    kind_ == Kind::DodgeBurn ? "Left dodges (lightens), right burns." :
                                    kind_ == Kind::Saturation ? "Left saturates, right desaturates." :
                                    kind_ == Kind::Hue ? "Left shifts hue up, right down." : "Either button.");
                break;
            case Kind::ColorReplacer:
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                ImGui::SliderInt("Tolerance", &app.replacer_tolerance, 0, 200);
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
    float last_x_ = 0, last_y_ = 0;
    // Clone state
    bool has_src_ = false, first_stroke_ = true;
    float src_x_ = 0, src_y_ = 0;
    int off_x_ = 0, off_y_ = 0;
    Image clone_src_;
};

// --- Smudge / Push -----------------------------------------------------
// Carries the pixels under the brush along the stroke: each stamp blends
// the previous stamp's pixels over the current ones (Smudge), or copies
// them without fading (Push, right button).

class SmudgeTool : public Tool {
public:
    const char* category() const override { return "Retouch"; }
    const char* name() const override { return "Smudge"; }
    const char* shortcut() const override { return "U"; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!app.active_is_raster()) return;
        layer_ = app.active_layer();
        before_ = app.paint_pixels(layer_);
        push_ = b == ImGuiMouseButton_Right;
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
        app.commit_pixels(layer_, push_ ? "Push" : "Smudge", before_, app.paint_pixels(layer_));
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
        ImGui::SliderInt("Amount", &app.retouch_amount, 1, 100, "%d%%");
        ImGui::SameLine();
        ImGui::TextDisabled("Left smudges, right pushes.");
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
    }
    void stamp(App& app, float cx, float cy) {
        Image& px = app.paint_pixels(layer_);
        const int r = rad_, W = 2 * r + 1;
        const int ox = static_cast<int>(std::floor(cx)) - r, oy = static_cast<int>(std::floor(cy)) - r;
        const float strength = push_ ? 1.0f : app.retouch_amount / 100.0f;
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
    bool active_ = false, push_ = false;
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
        adjust::red_eye(target, in.img_x, in.img_y, app.brush.size * 0.5f, app.redeye_strength);
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
        float st = app.redeye_strength * 100.0f;
        if (ImGui::SliderFloat("Strength", &st, 10.0f, 100.0f, "%.0f%%")) app.redeye_strength = st / 100.0f;
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
        const Color color = to_color(b == ImGuiMouseButton_Left ? app.fg_color : app.bg_color);
        const raster::Rect changed = raster::flood_fill(target, static_cast<int>(std::floor(in.img_x)),
                                                        static_cast<int>(std::floor(in.img_y)), color,
                                                        app.fill_tolerance, app.fill_opacity, &app.doc->selection());
        if (changed.empty()) return;
        app.paint_touched(layer, &changed);
        app.commit_pixels(layer, name(), std::move(before), target);
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
    const char* category() const override { return "Selection"; }
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
    const char* category() const override { return "Selection"; }
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
        if (!app.active_is_raster() || app.tube_image.empty()) { if (app.tube_image.empty()) app.status = "No picture tubes found (put .PspTube files in ~/.config/firn/tubes)."; return; }
        layer_ = app.active_layer();
        before_ = app.paint_pixels(layer_);
        active_ = true;
        last_x_ = in.img_x; last_y_ = in.img_y;
        carry_ = 0.0f;
        stamp(app, in.img_x, in.img_y, 0.0f);
    }
    void on_drag(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!active_) return;
        const float step = std::max(1.0f, (app.tube_step_override > 0 ? app.tube_step_override : std::max(1, app.tube_info.step)) * app.tube_scale);
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
        const float cw = app.tube_image.width() / static_cast<float>(app.tube_info.columns) * app.tube_scale * in.zoom;
        const float ch = app.tube_image.height() / static_cast<float>(app.tube_info.rows) * app.tube_scale * in.zoom;
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
        float pct = app.tube_scale * 100.0f;
        if (ImGui::SliderFloat("Scale", &pct, 10.0f, 250.0f, "%.0f%%")) app.tube_scale = pct / 100.0f;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputInt("Step", &app.tube_step_override);
        app.tube_step_override = std::max(0, app.tube_step_override);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::Combo("Placement", &app.tube_placement, "As tube\0Random\0Continuous\0");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::Combo("Selection", &app.tube_selection, "As tube\0Random\0Incremental\0Angular\0");
        if (app.tube_index >= 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("%d cells, step %d", app.tube_info.total, app.tube_info.step);
        }
    }

private:
    void stamp(App& app, float cx, float cy, float angle) {
        const io::TubeInfo& t = app.tube_info;
        const int placement = app.tube_placement ? app.tube_placement : t.placement;
        const int selection = app.tube_selection ? app.tube_selection : t.selection;
        // Random placement jitters the position by up to a quarter step.
        if (placement == 1) {
            const float step = std::max(1.0f, (app.tube_step_override > 0 ? app.tube_step_override : std::max(1, t.step)) * app.tube_scale);
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
        const int ow = std::max(1, static_cast<int>(cw * app.tube_scale + 0.5f)), oh = std::max(1, static_cast<int>(ch * app.tube_scale + 0.5f));
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
        app.show_text_dialog = true;
    }
    void draw_overlay(App&, const ToolInput& in) override {
        in.dl->AddLine(ImVec2(in.screen.x, in.screen.y - 8), ImVec2(in.screen.x, in.screen.y + 8), IM_COL32(255, 255, 255, 220));
        in.dl->AddLine(ImVec2(in.screen.x - 8, in.screen.y), ImVec2(in.screen.x + 8, in.screen.y), IM_COL32(255, 255, 255, 220));
    }
    void draw_options(App&) override { ImGui::TextUnformatted("Click where the text's top-left corner should go."); }
};

// --- Line and Preset Shapes --------------------------------------------
// Both preview on a snapshot of the layer while dragging and commit one
// history entry. Stroke uses the foreground material, fill the background.

class ShapeToolBase : public Tool {
public:
    const char* category() const override { return "Text and Shapes"; }
    bool wants_snap() const override { return true; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!app.active_is_raster()) return;
        layer_ = app.active_layer();
        before_ = app.paint_pixels(layer_);
        x0_ = x1_ = in.img_x; y0_ = y1_ = in.img_y;
        active_ = true;
    }
    void on_drag(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!active_) return;
        x1_ = in.img_x; y1_ = in.img_y;
        Image work = before_;
        draw(app, work);
        app.paint_pixels(layer_) = std::move(work);
        app.paint_touched(layer_);
    }
    void on_release(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!active_) return;
        on_drag(app, in, ImGuiMouseButton_Left);
        active_ = false;
        app.commit_pixels(layer_, name(), before_, app.paint_pixels(layer_));
    }
    void cancel(App& app) override {
        if (active_ && app.doc && layer_ < app.doc->layer_count()) { app.paint_pixels(layer_) = before_; app.paint_touched(layer_); }
        active_ = false;
    }

protected:
    virtual void draw(App& app, Image& img) = 0;
    bool active_ = false;
    size_t layer_ = 0;
    Image before_;
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
        ImGui::TextDisabled("Left draws with the foreground, right with the background.");
    }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override { button_ = b; ShapeToolBase::on_press(app, in, b); }

protected:
    void draw(App& app, Image& img) override {
        const Mask m = mask::polyline(img.width(), img.height(), {{x0_, y0_}, {x1_, y1_}}, app.line_width, app.shape_antialias);
        raster::paint_mask(img, m, to_color(button_ == ImGuiMouseButton_Left ? app.fg_color : app.bg_color), &app.doc->selection());
    }
    ImGuiMouseButton button_ = ImGuiMouseButton_Left;
};

class PresetShapeTool : public ShapeToolBase {
public:
    const char* name() const override { return "Preset Shape"; }
    const char* shortcut() const override { return "I"; }
    void draw_options(App& app) override {
        ImGui::SetNextItemWidth(150);
        ImGui::Combo("Shape", &app.shape_kind, "Rectangle\0Rounded Rectangle\0Ellipse\0Triangle\0Polygon\0Star\0");
        if (app.shape_kind == 1) { ImGui::SameLine(); ImGui::SetNextItemWidth(90); ImGui::SliderFloat("Radius", &app.shape_radius, 1.0f, 200.0f, "%.0f"); }
        if (app.shape_kind == 4) { ImGui::SameLine(); ImGui::SetNextItemWidth(90); ImGui::SliderInt("Sides", &app.shape_sides, 3, 24); }
        if (app.shape_kind == 5) {
            ImGui::SameLine(); ImGui::SetNextItemWidth(90); ImGui::SliderInt("Points", &app.star_points, 3, 24);
            ImGui::SameLine(); ImGui::SetNextItemWidth(90); ImGui::SliderFloat("Inner", &app.star_inner, 0.1f, 1.0f, "%.2f");
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
        ImGui::TextDisabled("Stroke: foreground. Fill: background.");
    }

protected:
    void draw(App& app, Image& img) override {
        const int w = img.width(), h = img.height();
        const float lx = std::min(x0_, x1_), rx = std::max(x0_, x1_), ty = std::min(y0_, y1_), by = std::max(y0_, y1_);
        const float sw = app.shape_stroke ? app.line_width : 0.0f;
        const float cx = (lx + rx) * 0.5f, cy = (ty + by) * 0.5f;
        auto shape = [&](float inset) {
            const float hx = (rx - lx) * 0.5f - inset, hy = (by - ty) * 0.5f - inset;
            switch (app.shape_kind) {
                case 1: return mask::rounded_rectangle(w, h, lx + inset, ty + inset, rx - inset, by - inset, std::max(0.0f, app.shape_radius - inset), app.shape_antialias);
                case 2: return mask::ellipse(w, h, cx, cy, hx, hy, app.shape_antialias);
                case 3: return mask::regular_polygon(w, h, cx, cy, hx, hy, 3, 0.0f, app.shape_antialias);
                case 4: return mask::regular_polygon(w, h, cx, cy, hx, hy, app.shape_sides, 0.0f, app.shape_antialias);
                case 5: return mask::star(w, h, cx, cy, hx, hy, app.star_points, app.star_inner, 0.0f, app.shape_antialias);
                default: return mask::rectangle(w, h, lx + inset, ty + inset, rx - inset, by - inset, app.shape_antialias);
            }
        };
        const Mask* clip = &app.doc->selection();
        if (app.shape_fill) raster::paint_mask(img, shape(sw), to_color(app.bg_color), clip);
        if (app.shape_stroke && sw > 0.0f) {
            Mask ring = shape(0.0f);
            mask::combine(ring, shape(sw), mask::Combine::Subtract);
            raster::paint_mask(img, ring, to_color(app.fg_color), clip);
        }
    }
};

}  // namespace

std::vector<std::unique_ptr<Tool>> make_default_tools() {
    std::vector<std::unique_ptr<Tool>> t;
    t.push_back(std::make_unique<PanTool>());
    t.push_back(std::make_unique<ZoomTool>());
    t.push_back(std::make_unique<MoveTool>());
    t.push_back(std::make_unique<CropTool>());
    t.push_back(std::make_unique<SelectionTool>());
    t.push_back(std::make_unique<FreehandTool>());
    t.push_back(std::make_unique<MagicWandTool>());
    t.push_back(std::make_unique<DropperTool>());
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Paint));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Airbrush));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::LightenDarken));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::DodgeBurn));
    t.push_back(std::make_unique<SmudgeTool>());
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Soften));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Sharpen));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Saturation));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Hue));
    t.push_back(std::make_unique<RedEyeTool>());
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Clone));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::ColorReplacer));
    t.push_back(std::make_unique<BrushTool>(BrushTool::Kind::Eraser));
    t.push_back(std::make_unique<FloodFillTool>());
    t.push_back(std::make_unique<PictureTubeTool>());
    t.push_back(std::make_unique<TextTool>());
    t.push_back(std::make_unique<LineTool>());
    t.push_back(std::make_unique<PresetShapeTool>());
    return t;
}
