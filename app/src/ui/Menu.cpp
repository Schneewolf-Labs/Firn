#include <memory>

#include "App.h"
#include "imgui.h"

using namespace psp9;

// Menu structure follows PSP9's: File, Edit, View, Image, Effects, Adjust,
// Layers, Objects, Selections, Window, Help. Most entries are placeholders
// until the corresponding commands exist.
void App::draw_menu() {
    const bool has_doc = doc != nullptr;
    const int layer = active_layer();
    const bool has_layer = has_doc && layer >= 0;

    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New...", "Ctrl+N")) show_new_dialog = true;
        if (ImGui::MenuItem("Open...", "Ctrl+O")) show_open_dialog = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Save As PNG...", "Ctrl+S", false, has_doc)) show_save_dialog = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, has_doc && history.can_undo())) undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, has_doc && history.can_redo())) redo();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Fit to Window", nullptr, false, has_doc)) fit_requested = true;
        if (ImGui::MenuItem("Actual Size", nullptr, false, has_doc)) { zoom = 1.0f; pan_x = pan_y = 0.0f; }
        ImGui::Separator();
        ImGui::MenuItem("ImGui Demo", nullptr, &show_imgui_demo);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Adjust")) {
        if (ImGui::MenuItem("Negative Image", "Ctrl+I", false, has_layer))
            run(std::make_unique<InvertCommand>(layer));
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Effects")) {
        if (ImGui::MenuItem("Blur...", nullptr, false, has_layer)) show_blur_dialog = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Layers")) {
        if (ImGui::MenuItem("New Raster Layer", nullptr, false, has_doc))
            run(std::make_unique<AddLayerCommand>("Raster " + std::to_string(doc->layer_count())));
        if (ImGui::MenuItem("Delete", nullptr, false, has_layer))
            run(std::make_unique<RemoveLayerCommand>(layer));
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void App::draw_dialogs() {
    if (show_new_dialog) { ImGui::OpenPopup("New Image"); show_new_dialog = false; }
    if (show_open_dialog) { ImGui::OpenPopup("Open Image"); show_open_dialog = false; }
    if (show_save_dialog) {
        std::snprintf(path_buf, sizeof(path_buf), "%s", doc_path.c_str());
        ImGui::OpenPopup("Save As PNG");
        show_save_dialog = false;
    }
    if (show_blur_dialog) { ImGui::OpenPopup("Blur"); show_blur_dialog = false; }

    if (ImGui::BeginPopupModal("New Image", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputInt("Width", &new_w);
        ImGui::InputInt("Height", &new_h);
        if (new_w < 1) new_w = 1;
        if (new_h < 1) new_h = 1;
        if (ImGui::Button("OK")) { new_document(new_w, new_h); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    // Text-entry file dialogs are a stopgap; a real file browser comes later.
    if (ImGui::BeginPopupModal("Open Image", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(500);
        bool enter = ImGui::InputText("Path", path_buf, sizeof(path_buf), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Open") || enter) { open_document(path_buf); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Save As PNG", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(500);
        bool enter = ImGui::InputText("Path", path_buf, sizeof(path_buf), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Save") || enter) { save_document_png(path_buf); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Blur", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SliderInt("Radius", &blur_radius, 1, 50);
        if (ImGui::Button("OK")) {
            if (active_layer() >= 0) run(std::make_unique<BoxBlurCommand>(active_layer(), blur_radius));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
