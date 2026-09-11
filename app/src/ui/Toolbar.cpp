// Standard toolbar (file, undo/redo, zoom) and the status bar along the
// bottom: tool hint, cursor position, zoom, image size.
#include <cstdio>

#include "App.h"
#include "MaterialDialog.h"
#include "imgui.h"

void App::draw_toolbar() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, toolbar_height));
    // Fixed chrome must not inherit the scaled minimum size of dockable windows.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 3));
    ImGui::Begin("##toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    const bool has_doc = doc != nullptr;
    auto button = [&](const char* label, bool enabled, const char* tip) {
        ImGui::BeginDisabled(!enabled);
        const bool hit = ImGui::SmallButton(label);
        ImGui::EndDisabled();
        if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
        ImGui::SameLine();
        return hit;
    };
    if (button("New", true, "New image (Ctrl+N)")) show_new_dialog = true;
    if (button("Open", true, "Open (Ctrl+O)")) request_open();
    if (button("Save", has_doc, "Save (Ctrl+S)")) save();
    ImGui::TextDisabled("|"); ImGui::SameLine();
    if (button("Undo", has_doc && history.can_undo(), "Undo (Ctrl+Z)")) undo();
    if (button("Redo", has_doc && history.can_redo(), "Redo (Ctrl+Y)")) redo();
    ImGui::TextDisabled("|"); ImGui::SameLine();
    if (button("-", has_doc, "Zoom out (-)")) zoom_about(canvas_center, 0.8f);
    if (button("+", has_doc, "Zoom in (+)")) zoom_about(canvas_center, 1.25f);
    if (button("Fit", has_doc, "Fit to window (Ctrl+0)")) fit_requested = true;
    if (button("1:1", has_doc, "Actual size (Ctrl+Alt+0)")) { zoom = 1.0f; pan_x = pan_y = 0.0f; }
    if (has_doc) { ImGui::Text("%d%%", static_cast<int>(zoom * 100 + 0.5f)); ImGui::SameLine(); }
    ImGui::TextDisabled("|"); ImGui::SameLine();
    const float mb = ImGui::GetFrameHeight();
    material_box(*this, true, ImVec2(mb, mb));
    ImGui::SameLine();
    material_box(*this, false, ImVec2(mb, mb));
    ImGui::End();
    ImGui::PopStyleVar(4);
}

void App::draw_status_bar() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - status_height));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, status_height));
    // Fixed chrome must not inherit the scaled minimum size of dockable windows.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 2));
    ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    // Left: the last status message (first line only); right: cursor and image facts.
    std::string first = status.substr(0, status.find('\n'));
    ImGui::TextUnformatted(first.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", status.c_str());
    if (doc) {
        char right[160];
        if (cursor_inside)
            std::snprintf(right, sizeof(right), "(%d, %d)   %d%%   %d x %d x %d   %zu layer(s)", cursor_x, cursor_y, static_cast<int>(zoom * 100 + 0.5f), doc->width(), doc->height(), doc->bit_depth() * 3, doc->layer_count());
        else
            std::snprintf(right, sizeof(right), "%d%%   %d x %d x %d   %zu layer(s)", static_cast<int>(zoom * 100 + 0.5f), doc->width(), doc->height(), doc->bit_depth() * 3, doc->layer_count());
        const float w = ImGui::CalcTextSize(right).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - w - 12.0f);
        ImGui::TextUnformatted(right);
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
}
