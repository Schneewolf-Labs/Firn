// Deform, Straighten and Perspective Correction tools. Deform warps the
// active raster layer (or just the selected pixels) live by a homography
// from its content box to a dragged quad (move, scale, rotate about a
// movable pivot, skew, perspective, numeric entry, flips) and commits on
// Apply; Straighten rotates by a drawn line; Perspective Correction maps a
// dragged quad back to a rectangle. The latter two act on all layers by
// default through WarpLayersCommand.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "App.h"
#include "firn/commands.h"
#include "firn/mask.h"
#include "firn/raster.h"
#include "tools/Tool.h"

using namespace firn;

namespace {

constexpr float kHandle = 4.0f;
constexpr float kRotateGap = 22.0f;

ImVec2 to_screen(const ToolInput& in, float x, float y) { return ImVec2(in.origin.x + x * in.zoom, in.origin.y + y * in.zoom); }

void quad_from_rect(const raster::Rect& r, raster::Quad& q) {
    q.x[0] = static_cast<float>(r.x0); q.y[0] = static_cast<float>(r.y0);
    q.x[1] = static_cast<float>(r.x1); q.y[1] = static_cast<float>(r.y0);
    q.x[2] = static_cast<float>(r.x1); q.y[2] = static_cast<float>(r.y1);
    q.x[3] = static_cast<float>(r.x0); q.y[3] = static_cast<float>(r.y1);
}

void quad_center(const raster::Quad& q, float* cx, float* cy) {
    *cx = (q.x[0] + q.x[1] + q.x[2] + q.x[3]) * 0.25f;
    *cy = (q.y[0] + q.y[1] + q.y[2] + q.y[3]) * 0.25f;
}

// Handle points: 0..3 corners, 4..7 edge midpoints (top, right, bottom, left), 8 rotate knob.
std::vector<ImVec2> handle_points(const ToolInput& in, const raster::Quad& q) {
    std::vector<ImVec2> h;
    for (int i = 0; i < 4; ++i) h.push_back(to_screen(in, q.x[i], q.y[i]));
    for (int i = 0; i < 4; ++i) { const int j = (i + 1) % 4; h.push_back(to_screen(in, (q.x[i] + q.x[j]) * 0.5f, (q.y[i] + q.y[j]) * 0.5f)); }
    // Knob above the top edge, along its outward normal.
    const ImVec2 top = h[4];
    float nx = -(q.y[1] - q.y[0]), ny = q.x[1] - q.x[0];
    const float len = std::hypot(nx, ny);
    if (len > 0) { nx /= len; ny /= len; }
    h.push_back(ImVec2(top.x + nx * kRotateGap, top.y + ny * kRotateGap));
    return h;
}

void draw_quad(const ToolInput& in, const raster::Quad& q, bool handles, bool knob) {
    const auto h = handle_points(in, q);
    for (int i = 0; i < 4; ++i) {
        in.dl->AddLine(h[i], h[(i + 1) % 4], IM_COL32(0, 0, 0, 255), 3.0f);
        in.dl->AddLine(h[i], h[(i + 1) % 4], IM_COL32(255, 255, 255, 220), 1.0f);
    }
    if (!handles) return;
    for (int i = 0; i < 8; ++i) {
        in.dl->AddRectFilled(ImVec2(h[i].x - kHandle, h[i].y - kHandle), ImVec2(h[i].x + kHandle, h[i].y + kHandle), IM_COL32(255, 255, 255, 255));
        in.dl->AddRect(ImVec2(h[i].x - kHandle, h[i].y - kHandle), ImVec2(h[i].x + kHandle, h[i].y + kHandle), IM_COL32(0, 0, 0, 255));
    }
    if (knob) {
        in.dl->AddLine(h[4], h[8], IM_COL32(0, 0, 0, 255));
        in.dl->AddCircleFilled(h[8], kHandle + 1, IM_COL32(255, 255, 255, 255));
        in.dl->AddCircle(h[8], kHandle + 1, IM_COL32(0, 0, 0, 255));
    }
}

int hit_handle(const ToolInput& in, const raster::Quad& q, bool knob) {
    const auto h = handle_points(in, q);
    for (int i = knob ? 8 : 7; i >= 0; --i)
        if (std::abs(in.screen.x - h[i].x) <= kHandle + 2 && std::abs(in.screen.y - h[i].y) <= kHandle + 2) return i;
    return -1;
}

bool inside_quad(const raster::Quad& q, float x, float y) {
    bool inside = false;
    for (int i = 0, j = 3; i < 4; j = i++)
        if ((q.y[i] > y) != (q.y[j] > y) && x < (q.x[j] - q.x[i]) * (y - q.y[i]) / (q.y[j] - q.y[i]) + q.x[i]) inside = !inside;
    return inside;
}

// --- Deform ------------------------------------------------------------------

class DeformTool : public Tool {
public:
    const char* category() const override { return "Edit"; }
    const char* name() const override { return "Deform"; }
    const char* shortcut() const override { return "K"; }
    bool wants_snap() const override { return true; }
    bool overlay_always() const override { return true; }

    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!app.doc || b != ImGuiMouseButton_Left) return;
        if (!session_ && !begin(app)) return;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { apply(app); return; }
        const ImGuiIO& io = ImGui::GetIO();
        x0_ = in.img_x; y0_ = in.img_y;
        start_ = quad_;
        const int h = hit_handle(in, quad_, true);
        const ImVec2 pv = to_screen(in, pivot_x_, pivot_y_);
        if (std::abs(in.screen.x - pv.x) <= kHandle + 3 && std::abs(in.screen.y - pv.y) <= kHandle + 3) mode_ = Mode::Pivot;
        else if (h == 8) mode_ = Mode::Rotate;
        else if (h >= 0 && h < 4) { handle_ = h; mode_ = io.KeyCtrl ? Mode::Perspective : Mode::ScaleCorner; }
        else if (h >= 4) { handle_ = h - 4; mode_ = io.KeyShift ? Mode::Skew : Mode::ScaleEdge; }
        else if (inside_quad(quad_, in.img_x, in.img_y)) mode_ = Mode::Move;
        else mode_ = Mode::Rotate;   // outside the box: rotate about the pivot
        start_pivot_x_ = pivot_x_; start_pivot_y_ = pivot_y_;
    }

    void on_drag(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (mode_ == Mode::None || !session_) return;
        const float dx = in.img_x - x0_, dy = in.img_y - y0_;
        const ImGuiIO& io = ImGui::GetIO();
        quad_ = start_;
        const float cx = start_pivot_x_, cy = start_pivot_y_;
        switch (mode_) {
            case Mode::Pivot: pivot_x_ = start_pivot_x_ + dx; pivot_y_ = start_pivot_y_ + dy; return;
            case Mode::Move: for (int i = 0; i < 4; ++i) { quad_.x[i] += dx; quad_.y[i] += dy; } pivot_x_ = cx + dx; pivot_y_ = cy + dy; break;
            case Mode::Perspective: quad_.x[handle_] += dx; quad_.y[handle_] += dy; break;
            case Mode::ScaleCorner: {
                // Scale about the opposite corner (Alt: about the center), in the quad's own axes.
                const int opp = (handle_ + 2) % 4;
                float ax = start_.x[opp], ay = start_.y[opp];
                if (io.KeyAlt) quad_center(start_, &ax, &ay);
                const float hx = start_.x[handle_] - ax, hy = start_.y[handle_] - ay;
                const float nx = start_.x[handle_] + dx - ax, ny = start_.y[handle_] + dy - ay;
                // Express in the frame of the two edges leaving the anchor.
                const int e1 = (opp + 1) % 4, e2 = (opp + 3) % 4;
                float ux = start_.x[e1] - ax, uy = start_.y[e1] - ay, vx = start_.x[e2] - ax, vy = start_.y[e2] - ay;
                const float det = ux * vy - uy * vx;
                if (std::abs(det) < 1e-6f) break;
                auto coords = [&](float px, float py, float* s, float* t) { *s = (px * vy - py * vx) / det; *t = (ux * py - uy * px) / det; };
                float hs, ht, ns, nt;
                coords(hx, hy, &hs, &ht); coords(nx, ny, &ns, &nt);
                float sx = std::abs(hs) > 1e-6f ? ns / hs : 1.0f, sy = std::abs(ht) > 1e-6f ? nt / ht : 1.0f;
                if (io.KeyShift) { const float s = std::max(std::abs(sx), std::abs(sy)); sx = std::copysign(s, sx); sy = std::copysign(s, sy); }
                for (int i = 0; i < 4; ++i) {
                    float s, t;
                    coords(start_.x[i] - ax, start_.y[i] - ay, &s, &t);
                    quad_.x[i] = ax + ux * s * sx + vx * t * sy;
                    quad_.y[i] = ay + uy * s * sx + vy * t * sy;
                }
                break;
            }
            case Mode::ScaleEdge: case Mode::Skew: {
                // Edge handle_ runs from corner a to corner b; the opposite edge stays.
                const int a = handle_, b2 = (handle_ + 1) % 4;
                float ex = start_.x[b2] - start_.x[a], ey = start_.y[b2] - start_.y[a];
                const float el = std::hypot(ex, ey);
                if (el < 1e-6f) break;
                ex /= el; ey /= el;
                if (mode_ == Mode::Skew) {
                    const float along = dx * ex + dy * ey;
                    quad_.x[a] += ex * along; quad_.y[a] += ey * along; quad_.x[b2] += ex * along; quad_.y[b2] += ey * along;
                } else {
                    const float nx = -ey, ny = ex;   // outward normal of this edge
                    const float out = dx * nx + dy * ny;
                    quad_.x[a] += nx * out; quad_.y[a] += ny * out; quad_.x[b2] += nx * out; quad_.y[b2] += ny * out;
                }
                break;
            }
            case Mode::Rotate: {
                float ang = std::atan2(in.img_y - cy, in.img_x - cx) - std::atan2(y0_ - cy, x0_ - cx);
                if (io.KeyShift) ang = std::round(ang / (3.14159265f / 12)) * (3.14159265f / 12);
                const float c = std::cos(ang), s = std::sin(ang);
                for (int i = 0; i < 4; ++i) {
                    const float px = start_.x[i] - cx, py = start_.y[i] - cy;
                    quad_.x[i] = cx + px * c - py * s;
                    quad_.y[i] = cy + px * s + py * c;
                }
                break;
            }
            default: break;
        }
        render(app);
    }

    void on_release(App&, const ToolInput&, ImGuiMouseButton) override { mode_ = Mode::None; }

    void cancel(App& app) override {
        if (session_ && app.doc && layer_ < static_cast<int>(app.doc->layer_count())) {
            app.doc->layer(layer_).pixels = before_;
            app.doc->touch();
        }
        session_ = false;
        mode_ = Mode::None;
    }

    void draw_overlay(App& app, const ToolInput& in) override {
        if (!session_) return;
        draw_quad(in, quad_, true, true);
        const ImVec2 pv = to_screen(in, pivot_x_, pivot_y_);
        in.dl->AddCircle(pv, kHandle + 3, IM_COL32(0, 0, 0, 255), 0, 3.0f);
        in.dl->AddCircle(pv, kHandle + 3, IM_COL32(255, 255, 255, 255), 0, 1.0f);
        in.dl->AddLine(ImVec2(pv.x - kHandle - 6, pv.y), ImVec2(pv.x + kHandle + 6, pv.y), IM_COL32(255, 255, 255, 200));
        in.dl->AddLine(ImVec2(pv.x, pv.y - kHandle - 6), ImVec2(pv.x, pv.y + kHandle + 6), IM_COL32(255, 255, 255, 200));
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) apply(app);
    }

    void draw_options(App& app) override {
        if (!session_ && app.doc && app.active_is_raster() && ImGui::SmallButton("Start")) begin(app);
        if (session_) {
            const bool changed = quad_changed();
            ImGui::BeginDisabled(!changed);
            if (ImGui::SmallButton("Apply")) apply(app);
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) { quad_ = original_; render(app); }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel")) cancel(app);
            ImGui::SameLine();
            if (ImGui::SmallButton("Flip H")) { std::swap(quad_.x[0], quad_.x[1]); std::swap(quad_.y[0], quad_.y[1]); std::swap(quad_.x[2], quad_.x[3]); std::swap(quad_.y[2], quad_.y[3]); render(app); }
            ImGui::SameLine();
            if (ImGui::SmallButton("Flip V")) { std::swap(quad_.x[0], quad_.x[3]); std::swap(quad_.y[0], quad_.y[3]); std::swap(quad_.x[1], quad_.x[2]); std::swap(quad_.y[1], quad_.y[2]); render(app); }
            ImGui::SameLine();
            if (ImGui::SmallButton("90 CCW")) { rotate_by(-90.0f); render(app); }
            ImGui::SameLine();
            if (ImGui::SmallButton("90 CW")) { rotate_by(90.0f); render(app); }
            // Numeric entry: each field edits the quad relative to what it is now,
            // so it keeps working after perspective drags.
            float cx, cy;
            quad_center(quad_, &cx, &cy);
            const float w = std::hypot(quad_.x[1] - quad_.x[0], quad_.y[1] - quad_.y[0]);
            const float h = std::hypot(quad_.x[3] - quad_.x[0], quad_.y[3] - quad_.y[0]);
            const float ang = std::atan2(quad_.y[1] - quad_.y[0], quad_.x[1] - quad_.x[0]) * 180.0f / 3.14159265f;
            float nx = cx, ny = cy, nw = w, nh = h, na = ang;
            bool changed_num = false;
            ImGui::SetNextItemWidth(70);
            if (ImGui::DragFloat("X", &nx, 1.0f, -1e5f, 1e5f, "%.0f")) { translate(nx - cx, 0); changed_num = true; }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            if (ImGui::DragFloat("Y", &ny, 1.0f, -1e5f, 1e5f, "%.0f")) { translate(0, ny - cy); changed_num = true; }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            if (ImGui::DragFloat("W", &nw, 1.0f, 1.0f, 1e5f, "%.0f") && w > 0.5f) { scale_along(0, nw / w); changed_num = true; }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            if (ImGui::DragFloat("H", &nh, 1.0f, 1.0f, 1e5f, "%.0f") && h > 0.5f) { scale_along(1, nh / h); changed_num = true; }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            if (ImGui::DragFloat("Angle", &na, 0.5f, -1e5f, 1e5f, "%.1f")) { rotate_by(na - ang); changed_num = true; }
            if (changed_num) render(app);
            ImGui::SameLine();
            ImGui::TextDisabled(has_sel_ ? "Transforming the selection." : "Transforming the whole layer.");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Drag inside to move, outside or the knob to rotate about the pivot (drag the pivot to move it); corners scale (Shift keeps aspect, Alt from the center, Ctrl moves one corner); edges scale (Shift skews). Enter or double-click applies.");
    }

private:
    enum class Mode { None, Move, ScaleCorner, ScaleEdge, Skew, Rotate, Perspective, Pivot };

    bool begin(App& app) {
        if (!app.active_is_raster()) { app.status = "Deform: the active layer is not a raster layer."; return false; }
        layer_ = app.active_layer();
        before_ = app.doc->layer(layer_).pixels;
        has_sel_ = app.doc->has_selection() && app.doc->selection().any();
        if (has_sel_) {
            // The selected pixels become a floating piece; the layer keeps the rest.
            sel_ = app.doc->selection();
            piece_ = before_;
            base_ = before_;
            const bool bg = app.doc->layer(layer_).background;
            const Color fill = app.background_fill();
            for (size_t i = 0; i < sel_.size(); ++i) {
                const float m = sel_.data()[i] / 255.0f;
                uint8_t* p = piece_.data() + i * 4;
                uint8_t* b = base_.data() + i * 4;
                p[3] = static_cast<uint8_t>(p[3] * m + 0.5f);
                if (bg) { b[0] = static_cast<uint8_t>(b[0] + (fill.r - b[0]) * m + 0.5f); b[1] = static_cast<uint8_t>(b[1] + (fill.g - b[1]) * m + 0.5f); b[2] = static_cast<uint8_t>(b[2] + (fill.b - b[2]) * m + 0.5f); }
                else b[3] = static_cast<uint8_t>(b[3] * (1.0f - m) + 0.5f);
            }
            quad_from_rect(sel_.bounds(), original_);
        } else {
            quad_from_rect(raster::content_bounds(before_), original_);
        }
        quad_ = original_;
        quad_center(quad_, &pivot_x_, &pivot_y_);
        session_ = true;
        return true;
    }
    void translate(float dx, float dy) {
        for (int i = 0; i < 4; ++i) { quad_.x[i] += dx; quad_.y[i] += dy; }
        pivot_x_ += dx; pivot_y_ += dy;
    }
    void rotate_by(float degrees) {
        const float rad = degrees * 3.14159265f / 180.0f, c = std::cos(rad), s = std::sin(rad);
        for (int i = 0; i < 4; ++i) {
            const float px = quad_.x[i] - pivot_x_, py = quad_.y[i] - pivot_y_;
            quad_.x[i] = pivot_x_ + px * c - py * s;
            quad_.y[i] = pivot_y_ + px * s + py * c;
        }
    }
    // Scales the quad about the pivot along its top edge (axis 0) or left edge (axis 1).
    void scale_along(int axis, float f) {
        float ux = axis == 0 ? quad_.x[1] - quad_.x[0] : quad_.x[3] - quad_.x[0];
        float uy = axis == 0 ? quad_.y[1] - quad_.y[0] : quad_.y[3] - quad_.y[0];
        const float len = std::hypot(ux, uy);
        if (len < 1e-6f) return;
        ux /= len; uy /= len;
        for (int i = 0; i < 4; ++i) {
            const float px = quad_.x[i] - pivot_x_, py = quad_.y[i] - pivot_y_;
            const float along = px * ux + py * uy;
            quad_.x[i] += ux * along * (f - 1.0f);
            quad_.y[i] += uy * along * (f - 1.0f);
        }
    }
    bool quad_changed() const {
        for (int i = 0; i < 4; ++i) if (quad_.x[i] != original_.x[i] || quad_.y[i] != original_.y[i]) return true;
        return false;
    }
    void render(App& app) {
        if (!session_ || !app.doc || layer_ >= static_cast<int>(app.doc->layer_count())) return;
        float H[9];
        if (!raster::homography(original_, quad_, H)) return;
        Layer& L = app.doc->layer(layer_);
        if (has_sel_) {
            // The warped piece composited over the layer with the selection cut out.
            const Image moved = raster::warp(piece_, H, piece_.width(), piece_.height());
            L.pixels = base_;
            for (int y = 0; y < moved.height(); ++y)
                for (int x = 0; x < moved.width(); ++x) {
                    const Color c = moved.get(x, y);
                    if (c.a) raster::blend_over(L.pixels, x, y, c, 1.0f);
                }
            app.doc->touch();
            return;
        }
        L.pixels = raster::warp(before_, H, before_.width(), before_.height());
        if (L.background) {
            const Color fill = app.background_fill();
            uint8_t* p = L.pixels.data();
            for (size_t i = 0; i < L.pixels.size_bytes(); i += 4)
                if (p[i + 3] == 0) { p[i] = fill.r; p[i + 1] = fill.g; p[i + 2] = fill.b; p[i + 3] = 255; }
        }
        app.doc->touch();
    }
    void apply(App& app) {
        if (!session_) return;
        if (quad_changed() && app.doc && layer_ < static_cast<int>(app.doc->layer_count())) {
            render(app);
            if (has_sel_) {
                // The selection follows the pixels; one history entry for both.
                float H[9];
                raster::homography(original_, quad_, H);
                std::vector<std::unique_ptr<Command>> parts;
                parts.push_back(std::make_unique<LayerSnapshotCommand>(layer_, "Deform", before_, app.doc->layer(layer_).pixels));
                parts.push_back(std::make_unique<SelectionCommand>("Deform", mask::warp(sel_, H)));
                app.run(std::make_unique<CompoundCommand>("Deform", std::move(parts)));
            } else {
                app.commit(std::make_unique<LayerSnapshotCommand>(layer_, "Deform", before_, app.doc->layer(layer_).pixels));
            }
        }
        session_ = false;
        mode_ = Mode::None;
    }

    bool session_ = false, has_sel_ = false;
    Mode mode_ = Mode::None;
    int handle_ = 0, layer_ = 0;
    float x0_ = 0, y0_ = 0;
    float pivot_x_ = 0, pivot_y_ = 0, start_pivot_x_ = 0, start_pivot_y_ = 0;
    raster::Quad original_, quad_, start_;
    Image before_, piece_, base_;   // piece_/base_: the selection and the rest, when transforming a selection
    Mask sel_;
};

// --- Straighten --------------------------------------------------------------------

class StraightenTool : public Tool {
public:
    const char* category() const override { return "Edit"; }
    const char* name() const override { return "Straighten"; }
    bool wants_snap() const override { return true; }
    bool overlay_always() const override { return true; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!app.doc || b != ImGuiMouseButton_Left) return;
        if (has_line_ && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { apply(app); return; }
        x0_ = x1_ = in.img_x; y0_ = y1_ = in.img_y;
        dragging_ = true; has_line_ = true;
    }
    void on_drag(App&, const ToolInput& in, ImGuiMouseButton) override { if (dragging_) { x1_ = in.img_x; y1_ = in.img_y; } }
    void on_release(App&, const ToolInput&, ImGuiMouseButton) override { dragging_ = false; }
    void cancel(App&) override { dragging_ = false; has_line_ = false; }
    void draw_overlay(App& app, const ToolInput& in) override {
        if (!has_line_) return;
        in.dl->AddLine(to_screen(in, x0_, y0_), to_screen(in, x1_, y1_), IM_COL32(0, 0, 0, 255), 3.0f);
        in.dl->AddLine(to_screen(in, x0_, y0_), to_screen(in, x1_, y1_), IM_COL32(255, 255, 0, 255), 1.0f);
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) apply(app);
    }
    void draw_options(App& app) override {
        ImGui::SetNextItemWidth(120);
        ImGui::Combo("Mode", &app.straighten_mode, "Auto\0Make vertical\0Make horizontal\0");
        ImGui::SameLine();
        ImGui::Checkbox("Rotate all layers", &app.straighten_all_layers);
        ImGui::SameLine();
        ImGui::Checkbox("Crop image", &app.straighten_crop);
        ImGui::SameLine();
        ImGui::BeginDisabled(!has_line_);
        if (ImGui::SmallButton("Apply")) apply(app);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (has_line_) ImGui::Text("%.2f deg", angle());
        ImGui::SameLine();
        ImGui::TextDisabled("Drag a line along something that should be straight; Enter, Apply, or a double-click rotates.");
    }

private:
    float angle() const { return std::atan2(y1_ - y0_, x1_ - x0_) * 180.0f / 3.14159265f; }
    void apply(App& app) {
        if (!has_line_ || !app.doc) return;
        has_line_ = false;
        const float a = angle();
        // Rotate so the line becomes horizontal or vertical (whichever is nearer in Auto).
        float target;
        if (app.straighten_mode == 1) target = a >= 0 ? 90.0f : -90.0f;
        else if (app.straighten_mode == 2) target = std::abs(a) <= 90.0f ? 0.0f : (a > 0 ? 180.0f : -180.0f);
        else { const float cands[] = {-180.0f, -90.0f, 0.0f, 90.0f, 180.0f}; target = 0; float best = 1e9f; for (float c : cands) if (std::abs(a - c) < best) { best = std::abs(a - c); target = c; } }
        const float rot = target - a;   // degrees, image coordinates (y down): positive rotates clockwise on screen
        if (std::abs(rot) < 0.01f) return;
        const int w = app.doc->width(), h = app.doc->height();
        const float rad = rot * 3.14159265f / 180.0f, c = std::cos(rad), s = std::sin(rad);
        const float cx = w * 0.5f, cy = h * 0.5f;
        const float H[9] = {c, -s, cx - c * cx + s * cy, s, c, cy - s * cx - c * cy, 0, 0, 1};
        raster::Rect crop;
        if (app.straighten_crop) {
            // Largest axis-aligned rectangle of the same aspect inside the rotated canvas.
            const float ar = std::abs(rad);
            const float sa = std::sin(ar), ca = std::cos(ar);
            const float wr = w * ca + h * sa, hr = w * sa + h * ca;   // rotated bounding size
            (void)wr; (void)hr;
            // Inscribed rectangle for a w x h rectangle rotated by ar (Coleman's formula).
            const bool wide = w >= h;
            const float side_long = wide ? static_cast<float>(w) : static_cast<float>(h), side_short = wide ? static_cast<float>(h) : static_cast<float>(w);
            float cw, ch;
            if (side_short <= 2.0f * sa * ca * side_long || std::abs(sa - ca) < 1e-6f) {
                const float x = 0.5f * side_short;
                cw = wide ? x / sa : x / ca; ch = wide ? x / ca : x / sa;
            } else {
                const float cos2 = ca * ca - sa * sa;
                cw = (w * ca - h * sa) / cos2; ch = (h * ca - w * sa) / cos2;
            }
            crop = {static_cast<int>(std::floor(cx - cw * 0.5f)), static_cast<int>(std::floor(cy - ch * 0.5f)), static_cast<int>(std::ceil(cx + cw * 0.5f)), static_cast<int>(std::ceil(cy + ch * 0.5f))};
        }
        app.run(std::make_unique<WarpLayersCommand>("Straighten", H, app.straighten_all_layers ? -1 : app.active_layer(), crop, app.background_fill()));
    }
    bool dragging_ = false, has_line_ = false;
    float x0_ = 0, y0_ = 0, x1_ = 0, y1_ = 0;
};

// --- Perspective Correction ------------------------------------------------------------

class PerspectiveTool : public Tool {
public:
    const char* category() const override { return "Edit"; }
    const char* name() const override { return "Perspective Correction"; }
    bool wants_snap() const override { return true; }
    bool overlay_always() const override { return true; }
    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!app.doc || b != ImGuiMouseButton_Left) return;
        if (!active_) reset(app);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { apply(app); return; }
        handle_ = hit_handle(in, quad_, false);
        if (handle_ >= 4) handle_ = -1;   // only corners move
        x0_ = in.img_x; y0_ = in.img_y; start_ = quad_;
        moving_ = handle_ < 0 && inside_quad(quad_, in.img_x, in.img_y);
    }
    void on_drag(App&, const ToolInput& in, ImGuiMouseButton) override {
        if (!active_) return;
        const float dx = in.img_x - x0_, dy = in.img_y - y0_;
        quad_ = start_;
        if (handle_ >= 0) { quad_.x[handle_] += dx; quad_.y[handle_] += dy; }
        else if (moving_) for (int i = 0; i < 4; ++i) { quad_.x[i] += dx; quad_.y[i] += dy; }
    }
    void on_release(App&, const ToolInput&, ImGuiMouseButton) override { handle_ = -1; moving_ = false; }
    void cancel(App&) override { active_ = false; }
    void draw_overlay(App& app, const ToolInput& in) override {
        if (!active_) { if (app.doc) reset(app); else return; }
        draw_quad(in, quad_, true, false);
        // Grid lines help judge the correction.
        for (int i = 1; i < 3; ++i) {
            const float t = i / 3.0f;
            in.dl->AddLine(to_screen(in, quad_.x[0] + (quad_.x[1] - quad_.x[0]) * t, quad_.y[0] + (quad_.y[1] - quad_.y[0]) * t), to_screen(in, quad_.x[3] + (quad_.x[2] - quad_.x[3]) * t, quad_.y[3] + (quad_.y[2] - quad_.y[3]) * t), IM_COL32(255, 255, 255, 120));
            in.dl->AddLine(to_screen(in, quad_.x[0] + (quad_.x[3] - quad_.x[0]) * t, quad_.y[0] + (quad_.y[3] - quad_.y[0]) * t), to_screen(in, quad_.x[1] + (quad_.x[2] - quad_.x[1]) * t, quad_.y[1] + (quad_.y[2] - quad_.y[1]) * t), IM_COL32(255, 255, 255, 120));
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) apply(app);
    }
    void draw_options(App& app) override {
        ImGui::Checkbox("All layers", &app.perspective_all_layers);
        ImGui::SameLine();
        ImGui::Checkbox("Crop image", &app.perspective_crop);
        ImGui::SameLine();
        if (ImGui::SmallButton("Apply")) apply(app);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) active_ = false;
        ImGui::SameLine();
        ImGui::TextDisabled("Drag the corners onto something that should be rectangular; Enter, Apply, or a double-click straightens it.");
    }

private:
    void reset(App& app) {
        const int w = app.doc->width(), h = app.doc->height();
        raster::Rect r{w / 8, h / 8, w - w / 8, h - h / 8};
        quad_from_rect(r, quad_);
        active_ = true;
    }
    void apply(App& app) {
        if (!active_ || !app.doc) return;
        active_ = false;
        // Target: the quad's bounding rectangle, keeping its center.
        float minx = quad_.x[0], maxx = quad_.x[0], miny = quad_.y[0], maxy = quad_.y[0];
        for (int i = 1; i < 4; ++i) { minx = std::min(minx, quad_.x[i]); maxx = std::max(maxx, quad_.x[i]); miny = std::min(miny, quad_.y[i]); maxy = std::max(maxy, quad_.y[i]); }
        raster::Quad target;
        raster::Rect rect{static_cast<int>(std::lround(minx)), static_cast<int>(std::lround(miny)), static_cast<int>(std::lround(maxx)), static_cast<int>(std::lround(maxy))};
        if (rect.empty()) return;
        quad_from_rect(rect, target);
        float H[9];
        if (!raster::homography(quad_, target, H)) return;
        app.run(std::make_unique<WarpLayersCommand>("Perspective Correction", H, app.perspective_all_layers ? -1 : app.active_layer(), app.perspective_crop ? rect : raster::Rect{}, app.background_fill()));
        if (app.perspective_crop) app.fit_requested = true;
    }
    bool active_ = false, moving_ = false;
    int handle_ = -1;
    float x0_ = 0, y0_ = 0;
    raster::Quad quad_, start_;
};

}  // namespace

std::vector<std::unique_ptr<Tool>> make_deform_tools() {
    std::vector<std::unique_ptr<Tool>> t;
    t.push_back(std::make_unique<DeformTool>());
    t.push_back(std::make_unique<StraightenTool>());
    t.push_back(std::make_unique<PerspectiveTool>());
    return t;
}
