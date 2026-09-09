// Warp Brush, Mesh Warp, Scratch Remover and Object Remover. The warp tools
// keep the layer's pixels from the press and re-render from them while the
// gesture (or mesh session) lasts, so nothing degrades with repeated
// resampling; one LayerSnapshotCommand records the result.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>

#include "App.h"
#include "firn/commands.h"
#include "firn/mask.h"
#include "firn/raster.h"
#include "tools/Tool.h"

using namespace firn;

namespace {

ImVec2 to_screen(const ToolInput& in, float x, float y) { return ImVec2(in.origin.x + x * in.zoom, in.origin.y + y * in.zoom); }

// --- Warp Brush --------------------------------------------------------------------

class WarpBrushTool : public Tool {
public:
    const char* category() const override { return "Paint"; }
    const char* name() const override { return "Warp Brush"; }
    bool wants_snap() const override { return false; }

    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!app.active_is_raster()) { app.status = "Warp Brush: select a raster layer."; return; }
        layer_ = app.active_layer();
        before_ = app.paint_pixels(layer_);
        const size_t n = static_cast<size_t>(before_.width()) * before_.height();
        dx_.assign(n, 0.0f); dy_.assign(n, 0.0f);
        last_x_ = in.img_x; last_y_ = in.img_y;
        reverse_ = b == ImGuiMouseButton_Right;
        active_ = true;
        stamp(app, in.img_x, in.img_y, 0.0f, 0.0f);
    }
    void on_drag(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!active_) return;
        const float dx = in.img_x - last_x_, dy = in.img_y - last_y_;
        const float dist = std::hypot(dx, dy);
        const float step = std::max(1.0f, app.brush.size * 0.15f);
        const int n = std::max(1, static_cast<int>(dist / step));
        for (int i = 1; i <= n; ++i) {
            const float t = static_cast<float>(i) / n;
            stamp(app, last_x_ + dx * t, last_y_ + dy * t, dx / n, dy / n);
        }
        last_x_ = in.img_x; last_y_ = in.img_y;
        render(app);
    }
    void on_release(App& app, const ToolInput&, ImGuiMouseButton) override {
        if (!active_) return;
        active_ = false;
        render(app);
        app.commit_pixels(layer_, "Warp Brush", before_, app.paint_pixels(layer_));
    }
    void cancel(App& app) override {
        if (active_ && app.doc && layer_ < app.doc->layer_count()) { app.paint_pixels(layer_) = before_; app.paint_touched(layer_); }
        active_ = false;
    }
    void draw_overlay(App& app, const ToolInput& in) override {
        in.dl->AddCircle(in.screen, app.brush.size * 0.5f * in.zoom, IM_COL32(255, 255, 255, 200));
        in.dl->AddCircle(in.screen, app.brush.size * 0.5f * in.zoom + 1, IM_COL32(0, 0, 0, 120));
    }
    void draw_options(App& app) override {
        ImGui::SetNextItemWidth(130);
        ImGui::Combo("Mode", &app.warp_mode, "Push\0Expand\0Contract\0Twirl right\0Twirl left\0Noise\0Iron out\0");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::SliderFloat("Size", &app.brush.size, 4.0f, 500.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::SliderFloat("Hardness", &app.brush.hardness, 0.0f, 1.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::SliderInt("Strength", &app.warp_strength, 1, 100);
        ImGui::SameLine();
        ImGui::TextDisabled("Drag to warp; the right button reverses expand/contract/twirl. Iron out removes warping from this stroke.");
    }

private:
    void stamp(App& app, float cx, float cy, float mx, float my) {
        const int w = before_.width(), h = before_.height();
        const float r = std::max(2.0f, app.brush.size * 0.5f);
        const float k = app.warp_strength / 100.0f;
        const int x0 = std::max(0, static_cast<int>(cx - r)), x1 = std::min(w - 1, static_cast<int>(cx + r));
        const int y0 = std::max(0, static_cast<int>(cy - r)), y1 = std::min(h - 1, static_cast<int>(cy + r));
        const float hard = std::clamp(app.brush.hardness, 0.0f, 0.99f);
        const int mode = app.warp_mode;
        const float sign = reverse_ ? -1.0f : 1.0f;
        std::uniform_real_distribution<float> uni(-1.0f, 1.0f);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float px = x - cx, py = y - cy;
                const float d = std::hypot(px, py) / r;
                if (d >= 1.0f) continue;
                // Soft falloff: 1 inside the hard core, cosine to 0 at the edge.
                const float f = d <= hard ? 1.0f : 0.5f * (1.0f + std::cos((d - hard) / (1.0f - hard) * 3.14159265f));
                const size_t i = static_cast<size_t>(y) * w + x;
                switch (mode) {
                    case 0: dx_[i] -= mx * f * k * 2.0f; dy_[i] -= my * f * k * 2.0f; break;                     // push: pixels follow the drag
                    case 1: dx_[i] -= px * f * k * 0.15f * sign; dy_[i] -= py * f * k * 0.15f * sign; break;  // expand: sample nearer the center
                    case 2: dx_[i] += px * f * k * 0.15f * sign; dy_[i] += py * f * k * 0.15f * sign; break;  // contract
                    case 3: case 4: {
                        const float a = (mode == 3 ? 1.0f : -1.0f) * sign * f * k * 0.12f;
                        const float c = std::cos(a), s = std::sin(a);
                        const float rx = px * c - py * s, ry = px * s + py * c;
                        dx_[i] += (rx - px); dy_[i] += (ry - py);
                        break;
                    }
                    case 5: dx_[i] += uni(rng_) * f * k * 4.0f; dy_[i] += uni(rng_) * f * k * 4.0f; break;
                    default: dx_[i] *= 1.0f - f * k * 0.5f; dy_[i] *= 1.0f - f * k * 0.5f; break;           // iron out
                }
            }
    }
    void render(App& app) {
        app.paint_pixels(layer_) = raster::displace(before_, dx_, dy_);
        app.paint_touched(layer_);
    }
    bool active_ = false, reverse_ = false;
    size_t layer_ = 0;
    float last_x_ = 0, last_y_ = 0;
    Image before_;
    std::vector<float> dx_, dy_;
    std::mt19937 rng_{12345};
};

// --- Mesh Warp ----------------------------------------------------------------------

class MeshWarpTool : public Tool {
public:
    const char* category() const override { return "Edit"; }
    const char* name() const override { return "Mesh Warp"; }
    bool wants_snap() const override { return false; }
    bool overlay_always() const override { return true; }

    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!app.doc || b != ImGuiMouseButton_Left) return;
        if (!session_ && !begin(app)) return;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { apply(app); return; }
        // Nearest node within reach.
        drag_ = -1;
        float best = 10.0f / in.zoom;
        for (size_t i = 0; i < nodes_.size(); ++i) {
            const float d = std::hypot(nodes_[i].first - in.img_x, nodes_[i].second - in.img_y);
            if (d < best) { best = d; drag_ = static_cast<int>(i); }
        }
    }
    void on_drag(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!session_ || drag_ < 0) return;
        nodes_[static_cast<size_t>(drag_)] = {in.img_x, in.img_y};
        render(app);
    }
    void on_release(App&, const ToolInput&, ImGuiMouseButton) override { drag_ = -1; }
    void cancel(App& app) override {
        if (session_ && app.doc && layer_ < app.doc->layer_count()) { app.paint_pixels(layer_) = before_; app.paint_touched(layer_); }
        session_ = false;
        drag_ = -1;
    }
    void draw_overlay(App& app, const ToolInput& in) override {
        if (!session_) return;
        const int cols = cols_, rows = rows_;
        for (int r = 0; r <= rows; ++r)
            for (int c = 0; c <= cols; ++c) {
                const auto& n = nodes_[static_cast<size_t>(r * (cols + 1) + c)];
                if (c < cols) { const auto& m = nodes_[static_cast<size_t>(r * (cols + 1) + c + 1)]; in.dl->AddLine(to_screen(in, n.first, n.second), to_screen(in, m.first, m.second), IM_COL32(0, 160, 255, 200)); }
                if (r < rows) { const auto& m = nodes_[static_cast<size_t>((r + 1) * (cols + 1) + c)]; in.dl->AddLine(to_screen(in, n.first, n.second), to_screen(in, m.first, m.second), IM_COL32(0, 160, 255, 200)); }
                const ImVec2 p = to_screen(in, n.first, n.second);
                in.dl->AddRectFilled(ImVec2(p.x - 3, p.y - 3), ImVec2(p.x + 3, p.y + 3), IM_COL32(255, 255, 255, 255));
                in.dl->AddRect(ImVec2(p.x - 3, p.y - 3), ImVec2(p.x + 3, p.y + 3), IM_COL32(0, 0, 0, 255));
            }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) apply(app);
    }
    void draw_options(App& app) override {
        ImGui::SetNextItemWidth(100);
        if (ImGui::SliderInt("Mesh horizontal", &app.mesh_cols, 2, 20) && session_) reset_nodes(app);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        if (ImGui::SliderInt("Mesh vertical", &app.mesh_rows, 2, 20) && session_) reset_nodes(app);
        ImGui::SameLine();
        if (!session_ && app.active_is_raster() && ImGui::SmallButton("Start")) begin(app);
        if (session_) {
            if (ImGui::SmallButton("Apply")) apply(app);
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) { reset_nodes(app); }
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel")) cancel(app);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Drag mesh nodes; Enter, Apply, or a double-click keeps the result.");
    }

private:
    bool begin(App& app) {
        if (!app.active_is_raster()) { app.status = "Mesh Warp: select a raster layer."; return false; }
        layer_ = app.active_layer();
        before_ = app.paint_pixels(layer_);
        session_ = true;
        reset_nodes(app);
        return true;
    }
    void reset_nodes(App& app) {
        cols_ = std::clamp(app.mesh_cols, 2, 40); rows_ = std::clamp(app.mesh_rows, 2, 40);
        nodes_.clear();
        for (int r = 0; r <= rows_; ++r)
            for (int c = 0; c <= cols_; ++c) nodes_.emplace_back(static_cast<float>(before_.width()) * c / cols_, static_cast<float>(before_.height()) * r / rows_);
        if (session_) render(app);
    }
    void render(App& app) {
        app.paint_pixels(layer_) = raster::mesh_warp(before_, cols_, rows_, nodes_);
        app.paint_touched(layer_);
    }
    void apply(App& app) {
        if (!session_) return;
        session_ = false;
        render(app);
        app.commit_pixels(layer_, "Mesh Warp", before_, app.paint_pixels(layer_));
    }
    bool session_ = false;
    int drag_ = -1, cols_ = 4, rows_ = 4;
    size_t layer_ = 0;
    Image before_;
    std::vector<std::pair<float, float>> nodes_;
};

// --- Scratch Remover ------------------------------------------------------------------

class ScratchRemoverTool : public Tool {
public:
    const char* category() const override { return "Clone and Replace"; }
    const char* name() const override { return "Scratch Remover"; }
    bool wants_snap() const override { return false; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!app.active_is_raster() || b != ImGuiMouseButton_Left) return;
        x0_ = x1_ = in.img_x; y0_ = y1_ = in.img_y;
        active_ = true;
    }
    void on_drag(App&, const ToolInput& in, ImGuiMouseButton) override { if (active_) { x1_ = in.img_x; y1_ = in.img_y; } }
    void on_release(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!active_) return;
        active_ = false;
        x1_ = in.img_x; y1_ = in.img_y;
        const size_t layer = app.active_layer();
        Image before = app.paint_pixels(layer);
        Image& target = app.paint_pixels(layer);
        raster::scratch_fill(target, x0_, y0_, x1_, y1_, static_cast<float>(app.scratch_width));
        if (app.doc->has_selection()) raster::apply_through_mask(target, before, app.doc->selection());
        app.paint_touched(layer);
        app.commit_pixels(layer, "Scratch Remover", std::move(before), target);
    }
    void cancel(App&) override { active_ = false; }
    void draw_overlay(App& app, const ToolInput& in) override {
        if (!active_) return;
        const float hw = app.scratch_width * 0.5f * in.zoom;
        const ImVec2 a = to_screen(in, x0_, y0_), b = to_screen(in, x1_, y1_);
        float nx = -(b.y - a.y), ny = b.x - a.x;
        const float l = std::hypot(nx, ny);
        if (l > 0) { nx = nx / l * hw; ny = ny / l * hw; }
        const ImVec2 q[4] = {ImVec2(a.x + nx, a.y + ny), ImVec2(b.x + nx, b.y + ny), ImVec2(b.x - nx, b.y - ny), ImVec2(a.x - nx, a.y - ny)};
        in.dl->AddPolyline(q, 4, IM_COL32(255, 255, 255, 220), ImDrawFlags_Closed, 1.0f);
    }
    void draw_options(App& app) override {
        ImGui::SetNextItemWidth(120);
        ImGui::SliderInt("Width", &app.scratch_width, 2, 200);
        ImGui::SameLine();
        ImGui::TextDisabled("Drag a box along the scratch; it is filled from the pixels beside it.");
    }

private:
    bool active_ = false;
    float x0_ = 0, y0_ = 0, x1_ = 0, y1_ = 0;
};

// --- Object Remover ----------------------------------------------------------------------

class ObjectRemoverTool : public Tool {
public:
    const char* category() const override { return "Clone and Replace"; }
    const char* name() const override { return "Object Remover"; }
    bool wants_snap() const override { return true; }
    bool overlay_always() const override { return true; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!app.doc || b != ImGuiMouseButton_Left) return;
        x0_ = x1_ = in.img_x; y0_ = y1_ = in.img_y;
        dragging_ = true; has_src_ = true;
    }
    void on_drag(App&, const ToolInput& in, ImGuiMouseButton) override { if (dragging_) { x1_ = in.img_x; y1_ = in.img_y; } }
    void on_release(App&, const ToolInput&, ImGuiMouseButton) override { dragging_ = false; }
    void cancel(App&) override { dragging_ = false; has_src_ = false; }
    void draw_overlay(App& app, const ToolInput& in) override {
        if (!has_src_) return;
        const ImVec2 a = to_screen(in, std::min(x0_, x1_), std::min(y0_, y1_)), b = to_screen(in, std::max(x0_, x1_), std::max(y0_, y1_));
        in.dl->AddRect(a, b, IM_COL32(0, 255, 0, 220));
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) apply(app);
    }
    void draw_options(App& app) override {
        ImGui::SetNextItemWidth(110);
        ImGui::SliderInt("Feather", &app.remover_feather, 0, 50);
        ImGui::SameLine();
        ImGui::BeginDisabled(!has_src_ || !app.doc || !app.doc->has_selection());
        if (ImGui::SmallButton("Apply")) apply(app);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Select the object first, then drag a source rectangle of matching background; Apply or Enter covers the selection with it.");
    }

private:
    void apply(App& app) {
        if (!has_src_ || !app.doc || !app.doc->has_selection() || !app.active_is_raster()) { app.status = "Object Remover: make a selection around the object first."; return; }
        has_src_ = false;
        const size_t layer = app.active_layer();
        Image before = app.paint_pixels(layer);
        Image& target = app.paint_pixels(layer);
        const Mask& sel = app.doc->selection();
        // Selection bounds and the source rectangle; the source tiles to cover the bounds.
        int bx0 = sel.width(), by0 = sel.height(), bx1 = -1, by1 = -1;
        for (int y = 0; y < sel.height(); ++y) for (int x = 0; x < sel.width(); ++x) if (sel.at(x, y)) { bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); by0 = std::min(by0, y); by1 = std::max(by1, y); }
        if (bx1 < bx0) return;
        const int sx0 = static_cast<int>(std::min(x0_, x1_)), sy0 = static_cast<int>(std::min(y0_, y1_));
        const int sw = std::max(1, static_cast<int>(std::abs(x1_ - x0_))), sh = std::max(1, static_cast<int>(std::abs(y1_ - y0_)));
        Mask soft = sel;
        if (app.remover_feather > 0) mask::feather(soft, static_cast<float>(app.remover_feather));
        for (int y = std::max(0, by0 - app.remover_feather); y <= std::min(target.height() - 1, by1 + app.remover_feather); ++y)
            for (int x = std::max(0, bx0 - app.remover_feather); x <= std::min(target.width() - 1, bx1 + app.remover_feather); ++x) {
                const float t = soft.at(x, y) / 255.0f;
                if (t <= 0.0f) continue;
                const int ux = sx0 + ((x - bx0) % sw + sw) % sw, uy = sy0 + ((y - by0) % sh + sh) % sh;
                if (ux < 0 || uy < 0 || ux >= before.width() || uy >= before.height()) continue;
                const Color s = before.get(ux, uy);
                raster::blend_over(target, x, y, s, t);
            }
        app.paint_touched(layer);
        app.commit_pixels(layer, "Object Remover", std::move(before), target);
    }
    bool dragging_ = false, has_src_ = false;
    float x0_ = 0, y0_ = 0, x1_ = 0, y1_ = 0;
};

}  // namespace

std::vector<std::unique_ptr<Tool>> make_warp_tools() {
    std::vector<std::unique_ptr<Tool>> t;
    t.push_back(std::make_unique<WarpBrushTool>());
    t.push_back(std::make_unique<MeshWarpTool>());
    t.push_back(std::make_unique<ScratchRemoverTool>());
    t.push_back(std::make_unique<ObjectRemoverTool>());
    return t;
}
