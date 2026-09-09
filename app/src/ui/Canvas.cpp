#include <algorithm>
#include <cmath>

#include "App.h"
#include "imgui.h"

// The image window. Draws the composite texture with zoom/pan, a checkerboard
// behind it for transparency, and handles wheel-zoom and middle/space-drag pan.
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

    // Defer the fit until the window has been laid out; on the first frame the
    // dock node has not been sized yet and the view is a few pixels wide.
    if (fit_requested && view_size.x > 64.0f && view_size.y > 64.0f) {
        zoom = std::min(view_size.x / img_w, view_size.y / img_h) * 0.95f;
        zoom = std::clamp(zoom, 0.01f, 64.0f);
        pan_x = pan_y = 0.0f;
        fit_requested = false;
    }

    // Input: the whole view is one invisible button so we get hover/drag state.
    ImGui::InvisibleButton("canvas", view_size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();

    const ImVec2 centre(view_pos.x + view_size.x * 0.5f, view_pos.y + view_size.y * 0.5f);

    if (hovered && io.MouseWheel != 0.0f) {
        // Zoom about the cursor.
        const float old_zoom = zoom;
        zoom = std::clamp(zoom * std::pow(1.15f, io.MouseWheel), 0.01f, 64.0f);
        const float k = zoom / old_zoom;
        const float mx = io.MousePos.x - centre.x, my = io.MousePos.y - centre.y;
        pan_x = mx - (mx - pan_x) * k;
        pan_y = my - (my - pan_y) * k;
    }
    const bool pan_drag = ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                          (tool == Tool::Pan && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) ||
                          (ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsMouseDragging(ImGuiMouseButton_Left));
    if (ImGui::IsItemActive() && pan_drag) {
        pan_x += io.MouseDelta.x;
        pan_y += io.MouseDelta.y;
    }

    // Image rect in screen space.
    const float dw = img_w * zoom, dh = img_h * zoom;
    const ImVec2 p0(centre.x + pan_x - dw * 0.5f, centre.y + pan_y - dh * 0.5f);
    const ImVec2 p1(p0.x + dw, p0.y + dh);

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
    dl->PopClipRect();

    // Status line at the bottom of the canvas window.
    if (hovered) {
        const int ix = static_cast<int>(std::floor((io.MousePos.x - p0.x) / zoom));
        const int iy = static_cast<int>(std::floor((io.MousePos.y - p0.y) / zoom));
        char buf[128];
        if (ix >= 0 && iy >= 0 && ix < doc->width() && iy < doc->height())
            std::snprintf(buf, sizeof(buf), "(%d, %d)  %d%%", ix, iy, static_cast<int>(zoom * 100));
        else
            std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(zoom * 100));
        dl->AddText(ImVec2(view_pos.x + 6, view_pos.y + view_size.y - 20), IM_COL32(255, 255, 255, 220), buf);
    }
    ImGui::End();
}
