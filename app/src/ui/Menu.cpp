#include <memory>

#include "App.h"
#include "firn/mask.h"
#include "imgui.h"

using namespace firn;

bool blend_combo(const char* label, BlendMode& mode);  // Palettes.cpp

// Menu structure follows the original's: File, Edit, View, Image, Effects, Adjust,
// Layers, Objects, Selections, Window, Help. Most entries are placeholders
// until the corresponding commands exist.
void App::draw_menu() {
    const bool has_doc = doc != nullptr;
    const int layer = active_layer();
    const bool has_layer = has_doc && layer >= 0;

    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New...", "Ctrl+N")) show_new_dialog = true;
        if (ImGui::MenuItem("Open...", "Ctrl+O")) request_open();
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, has_doc)) save();
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, has_doc)) request_save_as();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, has_doc && history.can_undo())) undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, has_doc && history.can_redo())) redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, has_layer)) cut();
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, has_layer)) copy();
        if (ImGui::MenuItem("Paste As New Image", "Ctrl+V", false, !clipboard.empty())) paste_as_new_image();
        if (ImGui::MenuItem("Paste As New Layer", "Ctrl+L", false, has_doc && !clipboard.empty())) paste_as_new_layer();
        if (ImGui::MenuItem("Clear", "Delete", false, has_layer)) clear_selection();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Fit to Window", nullptr, false, has_doc)) fit_requested = true;
        if (ImGui::MenuItem("Actual Size", nullptr, false, has_doc)) { zoom = 1.0f; pan_x = pan_y = 0.0f; }
        ImGui::Separator();
        ImGui::MenuItem("ImGui Demo", nullptr, &show_imgui_demo);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Image")) {
        if (ImGui::MenuItem("Flip", nullptr, false, has_doc)) run(std::make_unique<FlipCommand>());
        if (ImGui::MenuItem("Mirror", nullptr, false, has_doc)) run(std::make_unique<MirrorCommand>());
        ImGui::Separator();
        if (ImGui::MenuItem("Greyscale", nullptr, false, has_layer)) run(std::make_unique<GreyscaleCommand>(layer));
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Adjust")) {
        if (ImGui::BeginMenu("Brightness and Contrast")) {
            if (ImGui::MenuItem("Brightness/Contrast...", nullptr, false, has_layer)) show_bc_dialog = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Blur")) {
            if (ImGui::MenuItem("Average...", nullptr, false, has_layer)) show_blur_dialog = true;
            if (ImGui::MenuItem("Gaussian Blur...", nullptr, false, has_layer)) { show_blur_dialog = true; blur_gaussian = true; }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Negative Image", "Ctrl+I", false, has_layer))
            run(std::make_unique<InvertCommand>(layer));
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Effects")) {
        ImGui::MenuItem("(none yet)", nullptr, false, false);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Selections")) {
        const bool has_sel = has_doc && doc->has_selection();
        if (ImGui::MenuItem("Select All", "Ctrl+A", false, has_doc)) select_all();
        if (ImGui::MenuItem("Select None", "Ctrl+D", false, has_sel)) select_none();
        if (ImGui::MenuItem("Invert", "Ctrl+Shift+I", false, has_doc)) select_invert();
        ImGui::Separator();
        if (ImGui::BeginMenu("Modify", has_sel)) {
            if (ImGui::MenuItem("Expand...")) show_sel_dialog = 1;
            if (ImGui::MenuItem("Contract...")) show_sel_dialog = 2;
            if (ImGui::MenuItem("Feather...")) show_sel_dialog = 3;
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Layers")) {
        const int n = has_doc ? static_cast<int>(doc->layer_count()) : 0;
        const bool is_bg = has_layer && doc->layer(layer).background;
        if (ImGui::MenuItem("New Raster Layer", nullptr, false, has_doc)) layer_new();
        if (ImGui::MenuItem("Duplicate", nullptr, false, has_layer)) layer_duplicate();
        if (ImGui::MenuItem("Delete", nullptr, false, has_layer && n > 1)) layer_delete();
        if (ImGui::MenuItem("Properties...", nullptr, false, has_layer)) open_layer_properties();
        ImGui::Separator();
        if (ImGui::BeginMenu("Arrange", has_layer)) {
            if (ImGui::MenuItem("Bring to Top", nullptr, false, layer < n - 1)) layer_arrange(n);
            if (ImGui::MenuItem("Move Up", nullptr, false, layer < n - 1)) layer_arrange(+1);
            if (ImGui::MenuItem("Move Down", nullptr, false, layer > 0)) layer_arrange(-1);
            if (ImGui::MenuItem("Send to Bottom", nullptr, false, layer > 0)) layer_arrange(-n);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Merge", has_layer)) {
            if (ImGui::MenuItem("Merge Down", nullptr, false, layer > 0)) layer_merge(0);
            if (ImGui::MenuItem("Merge Visible", nullptr, false, n > 1)) layer_merge(1);
            if (ImGui::MenuItem("Merge All (Flatten)", nullptr, false, n > 1)) layer_merge(2);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Promote Background Layer", nullptr, false, is_bg)) layer_promote_background();
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void App::draw_dialogs() {
    if (show_new_dialog) { ImGui::OpenPopup("New Image"); show_new_dialog = false; }
    if (file_dialog.draw()) {
        if (file_op == PendingFileOp::Open) open_document(file_dialog.path());
        else if (file_op == PendingFileOp::SaveAs) save_document(file_dialog.path());
        file_op = PendingFileOp::None;
    }
    if (show_blur_dialog) { ImGui::OpenPopup(blur_gaussian ? "Gaussian Blur" : "Average"); show_blur_dialog = false; }
    if (show_bc_dialog) { ImGui::OpenPopup("Brightness/Contrast"); show_bc_dialog = false; }
    static const char* kSelDialogs[] = {nullptr, "Expand Selection", "Contract Selection", "Feather Selection"};
    if (show_sel_dialog) { ImGui::OpenPopup(kSelDialogs[show_sel_dialog]); show_sel_dialog = 0; }
    if (show_layer_props_dialog) { ImGui::OpenPopup("Layer Properties"); show_layer_props_dialog = false; }

    if (ImGui::BeginPopupModal("Layer Properties", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        LayerProps& p = layer_props_edit;
        char name[256];
        std::snprintf(name, sizeof(name), "%s", p.name.c_str());
        if (ImGui::InputText("Name", name, sizeof(name))) p.name = name;
        ImGui::Checkbox("Layer is visible", &p.visible);
        float op = p.opacity * 100.0f;
        if (ImGui::SliderFloat("Opacity", &op, 0.0f, 100.0f, "%.0f%%")) p.opacity = op / 100.0f;
        ImGui::SetNextItemWidth(160);
        blend_combo("Blend mode", p.blend);
        if (ImGui::Button("OK")) {
            if (doc && active_layer() >= 0) layer_set_props(doc->props(active_layer()), p);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("New Image", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::InputInt("Width", &new_w);
        ImGui::InputInt("Height", &new_h);
        if (new_w < 1) new_w = 1;
        if (new_h < 1) new_h = 1;
        if (ImGui::Button("OK")) { new_document(new_w, new_h); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Average", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        int r = static_cast<int>(blur_radius);
        if (ImGui::SliderInt("Radius", &r, 1, 50)) blur_radius = static_cast<float>(r);
        if (ImGui::Button("OK")) {
            if (active_layer() >= 0) run(std::make_unique<BoxBlurCommand>(active_layer(), r));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Gaussian Blur", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::SliderFloat("Radius", &blur_radius, 0.1f, 100.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
        if (ImGui::Button("OK")) {
            if (active_layer() >= 0) run(std::make_unique<GaussianBlurCommand>(active_layer(), blur_radius));
            blur_gaussian = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { blur_gaussian = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Brightness/Contrast", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::SliderInt("Brightness", &bc_brightness, -255, 255);
        ImGui::SliderInt("Contrast", &bc_contrast, -100, 100);
        if (ImGui::Button("OK")) {
            if (active_layer() >= 0) run(std::make_unique<BrightnessContrastCommand>(active_layer(), bc_brightness, bc_contrast));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    for (int which = 1; which <= 3; ++which) {
        if (!ImGui::BeginPopupModal(kSelDialogs[which], nullptr, ImGuiWindowFlags_AlwaysAutoResize)) continue;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::SliderInt("Pixels", &sel_modify_px, 1, 100);
        if (ImGui::Button("OK")) {
            if (doc && doc->has_selection()) {
                Mask m = doc->selection();
                if (which == 1) mask::expand(m, sel_modify_px);
                else if (which == 2) mask::contract(m, sel_modify_px);
                else mask::feather(m, static_cast<float>(sel_modify_px));
                set_selection(kSelDialogs[which], std::move(m));
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
