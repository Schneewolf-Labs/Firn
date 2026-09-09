// Vector tools: the Object Selector (select, move, scale, rotate objects on a
// vector layer) and the Pen (draw point-to-point or freehand paths, edit
// nodes). Both edit the layer's objects live, re-render the layer, and commit
// one VectorEditCommand per gesture.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "App.h"
#include "firn/commands.h"
#include "firn/vector.h"
#include "tools/Tool.h"

using namespace firn;

namespace {

constexpr float kHandle = 4.0f;    // half size in screen px
constexpr float kRotateGap = 22.0f;

ImVec2 to_screen(const ToolInput& in, float x, float y) { return ImVec2(in.origin.x + x * in.zoom, in.origin.y + y * in.zoom); }

// Union of the outline bounds of the selected objects.
bool selection_bounds(const std::vector<vec::Object>& objs, float* x0, float* y0, float* x1, float* y1) {
    bool any = false;
    for (const vec::Object& o : objs) {
        float a, b, c, d;
        if (!o.selected || o.is_group || !vec::outline_bounds(o, &a, &b, &c, &d)) continue;
        if (!any) { *x0 = a; *y0 = b; *x1 = c; *y1 = d; any = true; }
        *x0 = std::min(*x0, a); *y0 = std::min(*y0, b); *x1 = std::max(*x1, c); *y1 = std::max(*y1, d);
    }
    return any;
}

// Handle positions: 0..7 around the box (TL, T, TR, R, BR, B, BL, L), 8 = rotate.
std::vector<ImVec2> handle_points(const ToolInput& in, float x0, float y0, float x1, float y1) {
    const ImVec2 a = to_screen(in, x0, y0), b = to_screen(in, x1, y1);
    const float mx = (a.x + b.x) * 0.5f, my = (a.y + b.y) * 0.5f;
    return {ImVec2(a.x, a.y), ImVec2(mx, a.y), ImVec2(b.x, a.y), ImVec2(b.x, my), ImVec2(b.x, b.y), ImVec2(mx, b.y), ImVec2(a.x, b.y), ImVec2(a.x, my),
            ImVec2(mx, a.y - kRotateGap)};
}

void draw_selection_box(const ToolInput& in, float x0, float y0, float x1, float y1, bool handles) {
    const ImVec2 a = to_screen(in, x0, y0), b = to_screen(in, x1, y1);
    in.dl->AddRect(a, b, IM_COL32(0, 0, 0, 255));
    in.dl->AddRect(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 1), IM_COL32(255, 255, 255, 200));
    if (!handles) return;
    const auto hp = handle_points(in, x0, y0, x1, y1);
    for (size_t i = 0; i < 8; ++i) {
        in.dl->AddRectFilled(ImVec2(hp[i].x - kHandle, hp[i].y - kHandle), ImVec2(hp[i].x + kHandle, hp[i].y + kHandle), IM_COL32(255, 255, 255, 255));
        in.dl->AddRect(ImVec2(hp[i].x - kHandle, hp[i].y - kHandle), ImVec2(hp[i].x + kHandle, hp[i].y + kHandle), IM_COL32(0, 0, 0, 255));
    }
    in.dl->AddLine(hp[1], hp[8], IM_COL32(0, 0, 0, 255));
    in.dl->AddCircleFilled(hp[8], kHandle + 1, IM_COL32(255, 255, 255, 255));
    in.dl->AddCircle(hp[8], kHandle + 1, IM_COL32(0, 0, 0, 255));
}

int hit_handle(const ToolInput& in, float x0, float y0, float x1, float y1) {
    const auto hp = handle_points(in, x0, y0, x1, y1);
    for (int i = 8; i >= 0; --i)
        if (std::abs(in.screen.x - hp[i].x) <= kHandle + 2 && std::abs(in.screen.y - hp[i].y) <= kHandle + 2) return i;
    return -1;
}

// Topmost object under the point on the layer, or -1.
int object_at(const std::vector<vec::Object>& objs, float x, float y, float tol) {
    for (int i = static_cast<int>(objs.size()) - 1; i >= 0; --i)
        if (!objs[i].is_group && objs[i].visible && vec::hit_test(objs[i], x, y, tol)) return i;
    return -1;
}

// --- Object Selector -------------------------------------------------------

class ObjectSelectorTool : public Tool {
public:
    const char* category() const override { return "Vector"; }
    const char* name() const override { return "Object Selector"; }
    const char* shortcut() const override { return "O"; }
    bool wants_snap() const override { return true; }
    bool overlay_always() const override { return true; }

    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!app.doc || b != ImGuiMouseButton_Left) return;
        int layer = app.vector_layer_for_edit(false);
        const float tol = 4.0f / in.zoom;
        const ImGuiIO& io = ImGui::GetIO();
        x0_ = in.img_x; y0_ = in.img_y;
        int hit = -1;
        if (layer >= 0) {
            auto& objs = app.doc->layer(layer).objects;
            float bx0, by0, bx1, by1;
            if (selection_bounds(objs, &bx0, &by0, &bx1, &by1)) {
                const int h = hit_handle(in, bx0, by0, bx1, by1);
                layer_ = layer;
                before_ = objs;
                if (h == 8) { mode_ = Mode::Rotate; box_ = {bx0, by0, bx1, by1}; return; }
                if (h >= 0) { mode_ = Mode::Scale; handle_ = h; box_ = {bx0, by0, bx1, by1}; return; }
            }
            hit = object_at(objs, in.img_x, in.img_y, tol);
        }
        if (hit < 0) {
            // Nothing on the active layer: pick the topmost vector layer with
            // an object under the cursor, as the original does.
            for (int i = static_cast<int>(app.doc->layer_count()) - 1; i >= 0; --i) {
                if (!app.doc->layer(i).is_vector() || i == layer) continue;
                const int h = object_at(app.doc->layer(i).objects, in.img_x, in.img_y, tol);
                if (h >= 0) { if (layer >= 0 && !io.KeyShift) app.object_select_none(); app.doc->set_active_layer(i); layer = i; hit = h; break; }
            }
            if (layer < 0) { app.status = "Object Selector: no vector layer under the cursor."; return; }
        }
        layer_ = layer;
        auto& objs = app.doc->layer(layer_).objects;
        before_ = objs;
        if (hit >= 0) {
            if (!objs[hit].selected) app.select_objects({static_cast<size_t>(hit)}, io.KeyShift);
            else if (io.KeyShift) { for (size_t k = hit; k < vec::group_end(objs, hit); ++k) objs[k].selected = false; mode_ = Mode::None; return; }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { mode_ = Mode::None; app.open_vector_properties(); return; }
            mode_ = Mode::Move;
        } else {
            if (!io.KeyShift) app.object_select_none();
            mode_ = Mode::Marquee;
        }
        x1_ = x0_; y1_ = y0_;
    }

    void on_drag(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (mode_ == Mode::None || !app.doc || layer_ >= static_cast<int>(app.doc->layer_count())) return;
        x1_ = in.img_x; y1_ = in.img_y;
        auto& objs = app.doc->layer(layer_).objects;
        if (mode_ == Mode::Marquee) return;
        const ImGuiIO& io = ImGui::GetIO();
        for (size_t i = 0; i < objs.size() && i < before_.size(); ++i) {
            if (!before_[i].selected) continue;
            objs[i] = before_[i];
            if (mode_ == Mode::Move) {
                objs[i].translate(x1_ - x0_, y1_ - y0_);
            } else if (mode_ == Mode::Scale) {
                // The handle opposite the dragged one stays put.
                float nx0 = box_[0], ny0 = box_[1], nx1 = box_[2], ny1 = box_[3];
                const bool left = handle_ == 0 || handle_ == 6 || handle_ == 7, right = handle_ == 2 || handle_ == 3 || handle_ == 4;
                const bool top = handle_ <= 2, bottom = handle_ >= 4 && handle_ <= 6;
                if (left) nx0 = x1_; if (right) nx1 = x1_;
                if (top) ny0 = y1_; if (bottom) ny1 = y1_;
                float sx = (nx1 - nx0) / std::max(box_[2] - box_[0], 1e-3f), sy = (ny1 - ny0) / std::max(box_[3] - box_[1], 1e-3f);
                if (io.KeyShift && (left || right) && (top || bottom)) { const float s = std::max(std::abs(sx), std::abs(sy)); sx = std::copysign(s, sx); sy = std::copysign(s, sy); }
                const float ax = left ? box_[2] : box_[0], ay = top ? box_[3] : box_[1];   // anchor
                objs[i].transform(sx, 0, 0, sy, ax - ax * sx, ay - ay * sy);
            } else if (mode_ == Mode::Rotate) {
                const float cx = (box_[0] + box_[2]) * 0.5f, cy = (box_[1] + box_[3]) * 0.5f;
                float a = std::atan2(y1_ - cy, x1_ - cx) - std::atan2(y0_ - cy, x0_ - cx);
                if (io.KeyShift) a = std::round(a / (3.14159265f / 12)) * (3.14159265f / 12);
                const float c = std::cos(a), s = std::sin(a);
                objs[i].transform(c, -s, s, c, cx - (c * cx - s * cy), cy - (s * cx + c * cy));
            }
        }
        app.doc->rasterize_vector_layer(layer_);
    }

    void on_release(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (mode_ == Mode::None || !app.doc) return;
        const Mode m = mode_;
        mode_ = Mode::None;
        if (layer_ >= static_cast<int>(app.doc->layer_count())) return;
        auto& objs = app.doc->layer(layer_).objects;
        if (m == Mode::Marquee) {
            const float rx0 = std::min(x0_, in.img_x), rx1 = std::max(x0_, in.img_x), ry0 = std::min(y0_, in.img_y), ry1 = std::max(y0_, in.img_y);
            std::vector<size_t> picks;
            for (size_t i = 0; i < objs.size(); ++i) {
                float a, b, c, d;
                if (objs[i].is_group || !vec::outline_bounds(objs[i], &a, &b, &c, &d)) continue;
                if (a < rx1 && c > rx0 && b < ry1 && d > ry0) picks.push_back(i);
            }
            if (!picks.empty()) app.select_objects(picks, true);
            return;
        }
        if (x1_ == x0_ && y1_ == y0_) { app.doc->rasterize_vector_layer(layer_); return; }
        app.objects_changed(m == Mode::Move ? "Move Objects" : m == Mode::Scale ? "Scale Objects" : "Rotate Objects", before_);
    }

    void cancel(App& app) override {
        if (mode_ != Mode::None && mode_ != Mode::Marquee && app.doc && layer_ < static_cast<int>(app.doc->layer_count())) {
            app.doc->layer(layer_).objects = before_;
            app.doc->rasterize_vector_layer(layer_);
        }
        mode_ = Mode::None;
    }

    void draw_overlay(App& app, const ToolInput& in) override {
        const int layer = app.vector_layer_for_edit(false);
        if (layer < 0) return;
        const auto& objs = app.doc->layer(layer).objects;
        float x0, y0, x1, y1;
        if (selection_bounds(objs, &x0, &y0, &x1, &y1)) draw_selection_box(in, x0, y0, x1, y1, true);
        if (mode_ == Mode::Marquee) {
            const ImVec2 a = to_screen(in, x0_, y0_), b = to_screen(in, x1_, y1_);
            in.dl->AddRect(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), IM_COL32(0, 160, 255, 255));
        }
    }

    void draw_options(App& app) override {
        const size_t n = app.selected_objects().size();
        ImGui::Text("%zu selected", n);
        ImGui::SameLine();
        ImGui::BeginDisabled(n == 0);
        if (ImGui::SmallButton("Properties")) app.open_vector_properties();
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) app.object_delete();
        ImGui::SameLine();
        if (ImGui::SmallButton("Group")) app.object_group();
        ImGui::SameLine();
        if (ImGui::SmallButton("Ungroup")) app.object_ungroup();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Select All")) app.object_select_all();
        ImGui::SameLine();
        ImGui::TextDisabled("Drag to move; handles scale (Shift: keep aspect); the top knob rotates (Shift: 15 degree steps). Double-click for properties.");
    }

private:
    enum class Mode { None, Move, Scale, Rotate, Marquee };
    Mode mode_ = Mode::None;
    int handle_ = -1;
    int layer_ = 0;
    float x0_ = 0, y0_ = 0, x1_ = 0, y1_ = 0;
    std::array<float, 4> box_{};
    std::vector<vec::Object> before_;
};

// --- Pen --------------------------------------------------------------------------

class PenTool : public Tool {
public:
    const char* category() const override { return "Vector"; }
    const char* name() const override { return "Pen"; }
    const char* shortcut() const override { return "D"; }
    bool wants_snap() const override { return true; }
    bool overlay_always() const override { return true; }

    void on_press(App& app, const ToolInput& in, ImGuiMouseButton b) override {
        if (!app.doc || b != ImGuiMouseButton_Left) return;
        const ImGuiIO& io = ImGui::GetIO();
        if (app.pen_mode == 2) { press_edit(app, in, io); return; }
        if (!drawing_) {
            layer_ = app.vector_layer_for_edit(true);
            if (layer_ < 0) return;
            before_ = app.doc->layer(layer_).objects;
            for (vec::Object& o : app.doc->layer(layer_).objects) o.selected = false;
            work_ = vec::Object{};
            work_.name = app.pen_mode == 1 ? "Freehand" : "Drawing";
            work_.paths.push_back(vec::Path{});
            work_.paths[0].closed = false;
            app.apply_object_style(work_, app.shape_stroke, app.shape_fill, b);
            work_.selected = true;
            drawing_ = true;
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !work_.paths[0].nodes.empty()) { finish(app, false); return; }
        add_node(in.img_x, in.img_y);
        dragging_ = true;
        drag_x_ = in.img_x; drag_y_ = in.img_y;
        refresh(app);
    }

    void on_drag(App& app, const ToolInput& in, ImGuiMouseButton) override {
        if (!app.doc) return;
        if (app.pen_mode == 2) { drag_edit(app, in); return; }
        if (!drawing_ || !dragging_) return;
        vec::Path& p = work_.paths[0];
        if (p.nodes.empty()) return;
        if (app.pen_mode == 1) {
            // Freehand: add a corner node every few pixels.
            const vec::Node& last = p.nodes.back();
            if (std::hypot(in.img_x - last.x, in.img_y - last.y) >= 4.0f / std::max(in.zoom, 0.05f)) add_node(in.img_x, in.img_y);
        } else {
            // Point to point: dragging sets a symmetric pair of handles.
            vec::Node& n = p.nodes.back();
            n.out_x = in.img_x; n.out_y = in.img_y;
            n.in_x = 2 * n.x - in.img_x; n.in_y = 2 * n.y - in.img_y;
            n.flags[1] = (n.flags[1] & 0x80) | 0x43;
        }
        refresh(app);
    }

    void on_release(App& app, const ToolInput&, ImGuiMouseButton) override {
        if (app.pen_mode == 2) { release_edit(app); return; }
        dragging_ = false;
        if (drawing_ && app.pen_mode == 1) finish(app, app.pen_close);
    }

    void cancel(App& app) override {
        if (drawing_ && app.doc && layer_ < static_cast<int>(app.doc->layer_count())) {
            app.doc->layer(layer_).objects = before_;
            app.doc->rasterize_vector_layer(layer_);
        }
        drawing_ = dragging_ = false;
        if (edit_active_ && app.doc && layer_ < static_cast<int>(app.doc->layer_count())) {
            app.doc->layer(layer_).objects = before_;
            app.doc->rasterize_vector_layer(layer_);
        }
        edit_active_ = false;
    }

    void draw_overlay(App& app, const ToolInput& in) override {
        if (drawing_) {
            draw_nodes(in, work_, -1, -1);
            // Rubber band from the last node to the cursor.
            const vec::Path& p = work_.paths[0];
            if (!p.nodes.empty() && !dragging_) in.dl->AddLine(to_screen(in, p.nodes.back().x, p.nodes.back().y), in.screen, IM_COL32(0, 160, 255, 160));
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) finish(app, app.pen_close);
            return;
        }
        if (app.pen_mode == 2) {
            const int layer = app.vector_layer_for_edit(false);
            if (layer < 0) return;
            const auto& objs = app.doc->layer(layer).objects;
            for (size_t i = 0; i < objs.size(); ++i)
                if (objs[i].selected && !objs[i].is_group) draw_nodes(in, objs[i], static_cast<int>(i) == edit_object_ ? edit_path_ : -1, static_cast<int>(i) == edit_object_ ? edit_node_ : -1);
            if ((ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) && edit_object_ >= 0) delete_node(app);
        }
    }

    void draw_options(App& app) override {
        ImGui::SetNextItemWidth(170);
        if (ImGui::Combo("Mode", &app.pen_mode, "Draw Point to Point\0Draw Freehand\0Edit Nodes\0")) { if (drawing_) finish(app, app.pen_close); edit_object_ = -1; }
        ImGui::SameLine();
        ImGui::Checkbox("Close path", &app.pen_close);
        ImGui::SameLine();
        ImGui::Checkbox("Stroke", &app.shape_stroke);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::SliderFloat("Width", &app.line_width, 1.0f, 100.0f, "%.0f");
        ImGui::SameLine();
        ImGui::Checkbox("Fill", &app.shape_fill);
        ImGui::SameLine();
        ImGui::Checkbox("Anti-alias", &app.shape_antialias);
        ImGui::SameLine();
        app.draw_line_style_combo();
        if (app.pen_mode == 2) {
            ImGui::SameLine();
            ImGui::BeginDisabled(edit_object_ < 0);
            if (ImGui::SmallButton("Corner")) set_node_type(app, 0);
            ImGui::SameLine();
            if (ImGui::SmallButton("Smooth")) set_node_type(app, 1);
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete Node")) delete_node(app);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("Drag nodes and handles of the selected objects; Delete removes a node.");
        } else if (drawing_) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Apply")) finish(app, app.pen_close);
            ImGui::SameLine();
            ImGui::TextDisabled("Click adds a corner, drag adds a curve node. Enter, Apply, or a double-click finishes.");
        } else {
            ImGui::SameLine();
            ImGui::TextDisabled(app.pen_mode == 1 ? "Drag to draw a freehand path." : "Click to place nodes; drag for curves.");
        }
    }

private:
    // --- drawing ---
    void add_node(float x, float y) {
        vec::Node n;
        n.x = n.in_x = n.out_x = x; n.y = n.in_y = n.out_y = y;
        n.flags[1] = 0x40;
        vec::Path& p = work_.paths[0];
        if (p.nodes.empty()) n.flags[0] = 1;
        p.nodes.push_back(n);
    }
    void refresh(App& app) {
        if (!app.doc || layer_ >= static_cast<int>(app.doc->layer_count())) return;
        auto& objs = app.doc->layer(layer_).objects;
        objs = before_;
        for (vec::Object& o : objs) o.selected = false;
        objs.push_back(work_);
        app.doc->rasterize_vector_layer(layer_);
    }
    void finish(App& app, bool close) {
        if (!drawing_) return;
        drawing_ = dragging_ = false;
        vec::Path& p = work_.paths[0];
        if (app.pen_mode == 1 && p.nodes.size() > 3) simplify(p);
        if (p.nodes.size() < 2 || !app.doc || layer_ >= static_cast<int>(app.doc->layer_count())) {
            if (app.doc && layer_ < static_cast<int>(app.doc->layer_count())) { app.doc->layer(layer_).objects = before_; app.doc->rasterize_vector_layer(layer_); }
            return;
        }
        p.closed = close;
        if (close) p.nodes.back().flags[1] |= 0x80;
        refresh(app);
        app.objects_changed(app.pen_mode == 1 ? "Freehand" : "Draw", before_);
    }
    // Freehand: drop nodes closer than a few pixels to the line between their neighbors.
    static void simplify(vec::Path& p) {
        std::vector<vec::Node> out{p.nodes.front()};
        for (size_t i = 1; i + 1 < p.nodes.size(); ++i) {
            const vec::Node& a = out.back(); const vec::Node& b = p.nodes[i]; const vec::Node& c = p.nodes[i + 1];
            const float dx = c.x - a.x, dy = c.y - a.y;
            const float len = std::hypot(dx, dy);
            const float dist = len > 0 ? std::abs((b.x - a.x) * dy - (b.y - a.y) * dx) / len : 0.0f;
            if (dist > 1.5f || std::hypot(b.x - a.x, b.y - a.y) > 24.0f) out.push_back(b);
        }
        out.push_back(p.nodes.back());
        p.nodes = out;
    }

    // --- node editing ---
    void press_edit(App& app, const ToolInput& in, const ImGuiIO& io) {
        const int layer = app.vector_layer_for_edit(false);
        if (layer < 0) { app.status = "Pen: the active layer is not a vector layer."; return; }
        layer_ = layer;
        auto& objs = app.doc->layer(layer_).objects;
        const float tol = 6.0f / in.zoom;
        // Handles of the selected node first, then any node of a selected object.
        edit_part_ = 0;
        if (edit_object_ >= 0 && edit_object_ < static_cast<int>(objs.size()) && edit_path_ < static_cast<int>(objs[edit_object_].paths.size()) && edit_node_ < static_cast<int>(objs[edit_object_].paths[edit_path_].nodes.size())) {
            const vec::Node& n = objs[edit_object_].paths[edit_path_].nodes[edit_node_];
            if (std::hypot(in.img_x - n.in_x, in.img_y - n.in_y) <= tol && !n.is_corner()) edit_part_ = 1;
            else if (std::hypot(in.img_x - n.out_x, in.img_y - n.out_y) <= tol && !n.is_corner()) edit_part_ = 2;
        }
        if (edit_part_ == 0) {
            edit_object_ = -1;
            for (int i = static_cast<int>(objs.size()) - 1; i >= 0 && edit_object_ < 0; --i) {
                if (!objs[i].selected || objs[i].is_group) continue;
                for (size_t pi = 0; pi < objs[i].paths.size() && edit_object_ < 0; ++pi)
                    for (size_t ni = 0; ni < objs[i].paths[pi].nodes.size(); ++ni) {
                        const vec::Node& n = objs[i].paths[pi].nodes[ni];
                        if (std::hypot(in.img_x - n.x, in.img_y - n.y) <= tol) { edit_object_ = i; edit_path_ = static_cast<int>(pi); edit_node_ = static_cast<int>(ni); break; }
                    }
            }
        }
        if (edit_object_ < 0) {
            // Not on a node: select the object under the cursor (Ctrl inserts a node on it).
            const int hit = object_at(objs, in.img_x, in.img_y, tol);
            if (hit >= 0) {
                app.select_objects({static_cast<size_t>(hit)}, io.KeyShift);
                if (io.KeyCtrl) insert_node(app, hit, in.img_x, in.img_y);
            } else if (!io.KeyShift) app.object_select_none();
            return;
        }
        before_ = objs;
        edit_active_ = true;
        x0_ = in.img_x; y0_ = in.img_y;
    }
    void drag_edit(App& app, const ToolInput& in) {
        if (!edit_active_ || edit_object_ < 0) return;
        auto& objs = app.doc->layer(layer_).objects;
        if (edit_object_ >= static_cast<int>(objs.size())) return;
        vec::Node& n = objs[edit_object_].paths[edit_path_].nodes[edit_node_];
        const vec::Node& b = before_[edit_object_].paths[edit_path_].nodes[edit_node_];
        const float dx = in.img_x - x0_, dy = in.img_y - y0_;
        const ImGuiIO& io = ImGui::GetIO();
        if (edit_part_ == 0) {
            n.x = b.x + dx; n.y = b.y + dy; n.in_x = b.in_x + dx; n.in_y = b.in_y + dy; n.out_x = b.out_x + dx; n.out_y = b.out_y + dy;
        } else if (edit_part_ == 1) {
            n.in_x = b.in_x + dx; n.in_y = b.in_y + dy;
            if (!io.KeyCtrl) { n.out_x = 2 * n.x - n.in_x; n.out_y = 2 * n.y - n.in_y; }   // Ctrl breaks the pair (cusp)
        } else {
            n.out_x = b.out_x + dx; n.out_y = b.out_y + dy;
            if (!io.KeyCtrl) { n.in_x = 2 * n.x - n.out_x; n.in_y = 2 * n.y - n.out_y; }
        }
        app.doc->rasterize_vector_layer(layer_);
    }
    void release_edit(App& app) {
        if (!edit_active_) return;
        edit_active_ = false;
        if (!app.doc || layer_ >= static_cast<int>(app.doc->layer_count())) return;
        // A click that did not move anything is a selection, not an edit.
        const auto& objs = app.doc->layer(layer_).objects;
        if (edit_object_ >= 0 && edit_object_ < static_cast<int>(objs.size()) && edit_object_ < static_cast<int>(before_.size())) {
            const vec::Node& a = objs[edit_object_].paths[edit_path_].nodes[edit_node_];
            const vec::Node& b = before_[edit_object_].paths[edit_path_].nodes[edit_node_];
            if (a.x == b.x && a.y == b.y && a.in_x == b.in_x && a.in_y == b.in_y && a.out_x == b.out_x && a.out_y == b.out_y) return;
        }
        app.objects_changed(edit_part_ == 0 ? "Move Node" : "Adjust Curve", before_);
    }
    void set_node_type(App& app, int type) {
        const int layer = app.vector_layer_for_edit(false);
        if (layer < 0 || edit_object_ < 0) return;
        auto& objs = app.doc->layer(layer).objects;
        if (edit_object_ >= static_cast<int>(objs.size())) return;
        std::vector<vec::Object> before = objs;
        vec::Path& p = objs[edit_object_].paths[edit_path_];
        vec::Node& n = p.nodes[edit_node_];
        if (type == 0) { n.in_x = n.out_x = n.x; n.in_y = n.out_y = n.y; n.flags[1] = (n.flags[1] & 0x80) | 0x40; }
        else {
            // Smooth: handles along the chord between the neighbors, a third of the way each side.
            const size_t cnt = p.nodes.size();
            const vec::Node& prev = p.nodes[(edit_node_ + cnt - 1) % cnt];
            const vec::Node& next = p.nodes[(edit_node_ + 1) % cnt];
            const float dx = (next.x - prev.x) / 6.0f, dy = (next.y - prev.y) / 6.0f;
            n.in_x = n.x - dx; n.in_y = n.y - dy; n.out_x = n.x + dx; n.out_y = n.y + dy;
            n.flags[1] = (n.flags[1] & 0x80) | 0x43;
        }
        app.objects_changed(type == 0 ? "Corner Node" : "Smooth Node", std::move(before));
    }
    void delete_node(App& app) {
        const int layer = app.vector_layer_for_edit(false);
        if (layer < 0 || edit_object_ < 0) return;
        auto& objs = app.doc->layer(layer).objects;
        if (edit_object_ >= static_cast<int>(objs.size())) return;
        std::vector<vec::Object> before = objs;
        vec::Path& p = objs[edit_object_].paths[edit_path_];
        if (edit_node_ >= static_cast<int>(p.nodes.size())) return;
        p.nodes.erase(p.nodes.begin() + edit_node_);
        if (p.nodes.size() < 2) objs[edit_object_].paths.erase(objs[edit_object_].paths.begin() + edit_path_);
        else { p.nodes.front().flags[0] = 1; if (p.closed) p.nodes.back().flags[1] |= 0x80; }
        if (objs[edit_object_].paths.empty()) objs.erase(objs.begin() + edit_object_);
        edit_object_ = -1;
        app.objects_changed("Delete Node", std::move(before));
    }
    void insert_node(App& app, int obj, float x, float y) {
        auto& objs = app.doc->layer(layer_).objects;
        std::vector<vec::Object> before = objs;
        // Insert on the closest flattened segment of the closest path.
        int best_path = -1, best_seg = -1; float best_d = 1e9f;
        for (size_t pi = 0; pi < objs[obj].paths.size(); ++pi) {
            const vec::Path& p = objs[obj].paths[pi];
            const size_t n = p.nodes.size();
            const size_t segs = p.closed ? n : n - 1;
            for (size_t si = 0; si < segs; ++si) {
                vec::Path one; one.closed = false; one.nodes = {p.nodes[si], p.nodes[(si + 1) % n]};
                for (const auto& pt : vec::flatten(one)) {
                    const float d = std::hypot(pt.first - x, pt.second - y);
                    if (d < best_d) { best_d = d; best_path = static_cast<int>(pi); best_seg = static_cast<int>(si); }
                }
            }
        }
        if (best_path < 0) return;
        vec::Node n; n.x = n.in_x = n.out_x = x; n.y = n.in_y = n.out_y = y; n.flags[1] = 0x40;
        vec::Path& p = objs[obj].paths[best_path];
        p.nodes.insert(p.nodes.begin() + best_seg + 1, n);
        edit_object_ = obj; edit_path_ = best_path; edit_node_ = best_seg + 1;
        app.objects_changed("Add Node", std::move(before));
    }
    static void draw_nodes(const ToolInput& in, const vec::Object& o, int sel_path, int sel_node) {
        for (size_t pi = 0; pi < o.paths.size(); ++pi) {
            const vec::Path& p = o.paths[pi];
            const auto pts = vec::flatten(p);
            for (size_t i = 0; i + 1 < pts.size(); ++i) in.dl->AddLine(to_screen(in, pts[i].first, pts[i].second), to_screen(in, pts[i + 1].first, pts[i + 1].second), IM_COL32(0, 160, 255, 200));
            if (p.closed && pts.size() > 2) in.dl->AddLine(to_screen(in, pts.back().first, pts.back().second), to_screen(in, pts.front().first, pts.front().second), IM_COL32(0, 160, 255, 200));
            for (size_t ni = 0; ni < p.nodes.size(); ++ni) {
                const vec::Node& n = p.nodes[ni];
                const ImVec2 c = to_screen(in, n.x, n.y);
                const bool sel = static_cast<int>(pi) == sel_path && static_cast<int>(ni) == sel_node;
                if (sel && !n.is_corner()) {
                    const ImVec2 a = to_screen(in, n.in_x, n.in_y), b = to_screen(in, n.out_x, n.out_y);
                    in.dl->AddLine(a, c, IM_COL32(255, 255, 255, 200)); in.dl->AddLine(c, b, IM_COL32(255, 255, 255, 200));
                    in.dl->AddCircleFilled(a, 3.0f, IM_COL32(255, 200, 0, 255)); in.dl->AddCircleFilled(b, 3.0f, IM_COL32(255, 200, 0, 255));
                }
                in.dl->AddRectFilled(ImVec2(c.x - 3, c.y - 3), ImVec2(c.x + 3, c.y + 3), sel ? IM_COL32(255, 80, 80, 255) : IM_COL32(255, 255, 255, 255));
                in.dl->AddRect(ImVec2(c.x - 3, c.y - 3), ImVec2(c.x + 3, c.y + 3), IM_COL32(0, 0, 0, 255));
            }
        }
    }

    bool drawing_ = false, dragging_ = false;
    int layer_ = 0;
    vec::Object work_;
    std::vector<vec::Object> before_;
    float drag_x_ = 0, drag_y_ = 0, x0_ = 0, y0_ = 0;
    // Edit mode
    bool edit_active_ = false;
    int edit_object_ = -1, edit_path_ = 0, edit_node_ = 0, edit_part_ = 0;   // part: 0 anchor, 1 in handle, 2 out handle
};

}  // namespace

std::vector<std::unique_ptr<Tool>> make_vector_tools() {
    std::vector<std::unique_ptr<Tool>> t;
    t.push_back(std::make_unique<ObjectSelectorTool>());
    t.push_back(std::make_unique<PenTool>());
    return t;
}
