#include <algorithm>
#include <cmath>

#include "App.h"
#include "imgui.h"

// The image window. Draws the composite texture with zoom/pan, a checkerboard
// behind it for transparency, handles wheel-zoom and middle/space-drag pan,
// and routes everything else to the active tool.
void App::draw_canvas() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Image", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    const ImVec2 view_pos = ImGui::GetCursorScreenPos();
    const ImVec2 view_size = ImGui::GetContentRegionAvail();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (!doc || !canvas_tex || view_size.x <= 0 || view_size.y <= 0) {
        dl->AddRectFilled(view_pos, ImVec2(view_pos.x + view_size.x, view_pos.y + view_size.y), IM_COL32(60, 60, 60, 255));
        ImGui::End();
        return;
    }

    const float img_w = static_cast<float>(doc->width());
    const float img_h = static_cast<float>(doc->height());
    canvas_centre = ImVec2(view_pos.x + view_size.x * 0.5f, view_pos.y + view_size.y * 0.5f);

    // Defer the fit until the window has been laid out; on the first frame the
    // dock node has not been sized yet and the view is a few pixels wide.
    if (fit_requested && view_size.x > 64.0f && view_size.y > 64.0f) {
        zoom = std::min(view_size.x / img_w, view_size.y / img_h) * 0.95f;
        zoom = std::clamp(zoom, 0.01f, 64.0f);
        pan_x = pan_y = 0.0f;
        fit_requested = false;
    }

    // Input: the whole view is one invisible button so we get hover/drag state.
    ImGui::InvisibleButton("canvas", view_size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                               ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImGuiIO& io = ImGui::GetIO();

    if (hovered && io.MouseWheel != 0.0f) zoom_about(io.MousePos, std::pow(1.15f, io.MouseWheel));

    const bool space = ImGui::IsKeyDown(ImGuiKey_Space);
    const bool pan_drag = ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                          ((tool().pans_with_left_drag() || space) && ImGui::IsMouseDragging(ImGuiMouseButton_Left));
    if (active && pan_drag) {
        pan_x += io.MouseDelta.x;
        pan_y += io.MouseDelta.y;
    }

    // Image rect in screen space.
    const float dw = img_w * zoom, dh = img_h * zoom;
    const ImVec2 p0(canvas_centre.x + pan_x - dw * 0.5f, canvas_centre.y + pan_y - dh * 0.5f);
    const ImVec2 p1(p0.x + dw, p0.y + dh);

    // Tool dispatch. One gesture = one button held from press to release.
    ToolInput in;
    in.screen = io.MousePos;
    in.origin = p0;
    in.zoom = zoom;
    in.img_x = (io.MousePos.x - p0.x) / zoom;
    in.img_y = (io.MousePos.y - p0.y) / zoom;
    in.inside = in.img_x >= 0 && in.img_y >= 0 && in.img_x < img_w && in.img_y < img_h;
    in.dl = dl;

    const bool tool_takes_left = !tool().pans_with_left_drag() && !space;
    if (active_button < 0 && hovered) {
        if (tool_takes_left && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) active_button = ImGuiMouseButton_Left;
        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) active_button = ImGuiMouseButton_Right;
        if (active_button >= 0) tool().on_press(*this, in, static_cast<ImGuiMouseButton>(active_button));
    } else if (active_button >= 0) {
        const auto b = static_cast<ImGuiMouseButton>(active_button);
        if (ImGui::IsMouseDown(b)) {
            if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) tool().on_drag(*this, in, b);
        } else {
            tool().on_release(*this, in, b);
            active_button = -1;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && active_button >= 0) {
        tool().cancel(*this);
        active_button = -1;
    }

    dl->PushClipRect(view_pos, ImVec2(view_pos.x + view_size.x, view_pos.y + view_size.y), true);
    dl->AddRectFilled(view_pos, ImVec2(view_pos.x + view_size.x, view_pos.y + view_size.y), IM_COL32(60, 60, 60, 255));

    // Checkerboard for transparency.
    {
        const float cell = 12.0f;
        const float x0 = std::max(p0.x, view_pos.x), y0 = std::max(p0.y, view_pos.y);
        const float x1 = std::min(p1.x, view_pos.x + view_size.x), y1 = std::min(p1.y, view_pos.y + view_size.y);
        int row = 0;
        for (float y = y0 - std::fmod(y0 - p0.y, cell); y < y1; y += cell, ++row) {
            int col = row;
            for (float x = x0 - std::fmod(x0 - p0.x, cell); x < x1; x += cell, ++col) {
                ImU32 c = (col % 2 == 0) ? IM_COL32(200, 200, 200, 255) : IM_COL32(150, 150, 150, 255);
                dl->AddRectFilled(ImVec2(std::max(x, x0), std::max(y, y0)),
                                  ImVec2(std::min(x + cell, x1), std::min(y + cell, y1)), c);
            }
        }
    }

    dl->AddImage((ImTextureID)(intptr_t)canvas_tex, p0, p1);
    dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), IM_COL32(0, 0, 0, 255));
    if (hovered || active_button >= 0) tool().draw_overlay(*this, in);

    // Marching ants along the selection boundary. Each unit edge is one
    // segment; colour alternates along the outline and cycles with time.
    sync_ants();
    if (!ants.empty()) {
        const int phase = static_cast<int>(ImGui::GetTime() * 10.0);
        const float vx0 = view_pos.x, vy0 = view_pos.y, vx1 = view_pos.x + view_size.x, vy1 = view_pos.y + view_size.y;
        for (const Edge& e : ants) {
            const float sx = p0.x + e.x * zoom, sy = p0.y + e.y * zoom;
            const float ex = e.horizontal ? sx + zoom : sx, ey = e.horizontal ? sy : sy + zoom;
            if (std::max(sx, ex) < vx0 || std::min(sx, ex) > vx1 || std::max(sy, ey) < vy0 || std::min(sy, ey) > vy1) continue;
            const ImU32 col = (((e.x + e.y + phase) / 4) & 1) ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255);
            dl->AddLine(ImVec2(sx, sy), ImVec2(ex, ey), col, 1.0f);
        }
    }
    dl->PopClipRect();

    // Status line at the bottom of the canvas window.
    if (hovered) {
        const int ix = static_cast<int>(std::floor(in.img_x));
        const int iy = static_cast<int>(std::floor(in.img_y));
        char buf[128];
        if (in.inside)
            std::snprintf(buf, sizeof(buf), "(%d, %d)  %d%%", ix, iy, static_cast<int>(zoom * 100));
        else
            std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(zoom * 100));
        dl->AddText(ImVec2(view_pos.x + 6, view_pos.y + view_size.y - 20), IM_COL32(255, 255, 255, 220), buf);
    }
    ImGui::End();
}
