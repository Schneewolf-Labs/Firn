#include <algorithm>
#include <memory>

#include "App.h"
#include "firn/adjust.h"
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
        if (ImGui::BeginMenu("Rotate", has_doc)) {
            if (ImGui::MenuItem("Rotate Clockwise 90")) rotate(90.0f);
            if (ImGui::MenuItem("Rotate Counter-clockwise 90")) rotate(-90.0f);
            if (ImGui::MenuItem("Rotate 180")) rotate(180.0f);
            if (ImGui::MenuItem("Free Rotate...")) show_rotate_dialog = true;
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Crop to Selection", "Ctrl+Shift+R", false, has_doc && doc->has_selection())) crop_to_selection();
        if (ImGui::MenuItem("Resize...", nullptr, false, has_doc)) open_resize_dialog();
        if (ImGui::MenuItem("Canvas Size...", nullptr, false, has_doc)) open_canvas_dialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Greyscale", nullptr, false, has_layer)) run(std::make_unique<GreyscaleCommand>(layer));
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Adjust")) {
        if (ImGui::BeginMenu("Brightness and Contrast", has_layer)) {
            if (ImGui::MenuItem("Brightness/Contrast...")) open_adjust = Adj::BrightnessContrast;
            if (ImGui::MenuItem("Curves...")) open_adjust = Adj::Curves;
            if (ImGui::MenuItem("Gamma Correction...")) open_adjust = Adj::Gamma;
            if (ImGui::MenuItem("Histogram Equalize")) run(std::make_unique<AdjustCommand>(layer, "Histogram Equalize", adjust::histogram_equalize));
            if (ImGui::MenuItem("Histogram Stretch")) run(std::make_unique<AdjustCommand>(layer, "Histogram Stretch", adjust::histogram_stretch));
            if (ImGui::MenuItem("Levels...")) open_adjust = Adj::Levels;
            if (ImGui::MenuItem("Threshold...")) open_adjust = Adj::Threshold;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Color Balance", has_layer)) {
            if (ImGui::MenuItem("Channel Mixer...")) open_adjust = Adj::ChannelMixer;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Hue and Saturation", has_layer)) {
            if (ImGui::MenuItem("Colorize...")) open_adjust = Adj::Colorize;
            if (ImGui::MenuItem("Hue/Saturation/Lightness...")) open_adjust = Adj::HSL;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Blur", has_layer)) {
            if (ImGui::MenuItem("Average...")) open_adjust = Adj::Average;
            if (ImGui::MenuItem("Gaussian Blur...")) open_adjust = Adj::Gaussian;
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Automatic Contrast Enhancement", nullptr, false, has_layer))
            run(std::make_unique<AdjustCommand>(layer, "Automatic Contrast Enhancement", [](Image& img) { adjust::auto_contrast(img); }));
        if (ImGui::MenuItem("Negative Image", "Ctrl+I", false, has_layer))
            run(std::make_unique<InvertCommand>(layer));
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Effects")) {
        if (ImGui::BeginMenu("Artistic Effects", has_layer)) {
            if (ImGui::MenuItem("Posterize...")) open_adjust = Adj::Posterize;
            if (ImGui::MenuItem("Solarize...")) open_adjust = Adj::Solarize;
            ImGui::EndMenu();
        }
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
    draw_adjust_dialogs();

    if (file_dialog.draw()) {
        if (file_op == PendingFileOp::Open) open_document(file_dialog.path());
        else if (file_op == PendingFileOp::SaveAs) save_document(file_dialog.path());
        file_op = PendingFileOp::None;
    }

    if (show_new_dialog) { ImGui::OpenPopup("New Image"); show_new_dialog = false; }
    static const char* kSelDialogs[] = {nullptr, "Expand Selection", "Contract Selection", "Feather Selection"};
    if (show_sel_dialog) { ImGui::OpenPopup(kSelDialogs[show_sel_dialog]); show_sel_dialog = 0; }
    if (show_layer_props_dialog) { ImGui::OpenPopup("Layer Properties"); show_layer_props_dialog = false; }
    if (show_resize_dialog) { ImGui::OpenPopup("Resize"); show_resize_dialog = false; }
    if (show_canvas_dialog) { ImGui::OpenPopup("Canvas Size"); show_canvas_dialog = false; }
    if (show_rotate_dialog) { ImGui::OpenPopup("Free Rotate"); show_rotate_dialog = false; }

    auto escape = [] { if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup(); };

    if (ImGui::BeginPopupModal("New Image", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        escape();
        ImGui::InputInt("Width", &new_w);
        ImGui::InputInt("Height", &new_h);
        if (new_w < 1) new_w = 1;
        if (new_h < 1) new_h = 1;
        if (ImGui::Button("OK")) { new_document(new_w, new_h); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    for (int which = 1; which <= 3; ++which) {
        if (!ImGui::BeginPopupModal(kSelDialogs[which], nullptr, ImGuiWindowFlags_AlwaysAutoResize)) continue;
        escape();
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

    if (ImGui::BeginPopupModal("Layer Properties", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        escape();
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

    if (ImGui::BeginPopupModal("Resize", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        escape();
        const float aspect = doc ? static_cast<float>(doc->width()) / doc->height() : 1.0f;
        ImGui::RadioButton("Pixels", &resize_by_percent, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Percent", &resize_by_percent, 1);
        if (resize_by_percent) {
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputFloat("%", &resize_pct, 1.0f, 10.0f, "%.1f") && doc) {
                resize_pct = std::max(resize_pct, 0.1f);
                resize_w = std::max(1, static_cast<int>(doc->width() * resize_pct / 100.0f + 0.5f));
                resize_h = std::max(1, static_cast<int>(doc->height() * resize_pct / 100.0f + 0.5f));
            }
        } else {
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputInt("Width", &resize_w)) {
                resize_w = std::max(1, resize_w);
                if (resize_lock) resize_h = std::max(1, static_cast<int>(resize_w / aspect + 0.5f));
            }
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputInt("Height", &resize_h)) {
                resize_h = std::max(1, resize_h);
                if (resize_lock) resize_w = std::max(1, static_cast<int>(resize_h * aspect + 0.5f));
            }
            ImGui::Checkbox("Lock aspect ratio", &resize_lock);
        }
        ImGui::SetNextItemWidth(160);
        ImGui::Combo("Resample", &resize_filter, "Pixel resize\0Bilinear\0Bicubic\0");
        ImGui::Text("%d x %d  ->  %d x %d", doc ? doc->width() : 0, doc ? doc->height() : 0, resize_w, resize_h);
        if (ImGui::Button("OK")) {
            if (doc && (resize_w != doc->width() || resize_h != doc->height())) {
                tool().cancel(*this);
                run(std::make_unique<ResizeCommand>(resize_w, resize_h, static_cast<raster::Filter>(resize_filter)));
                fit_requested = true;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Canvas Size", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        escape();
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Width", &canvas_w)) canvas_w = std::max(1, canvas_w);
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Height", &canvas_h)) canvas_h = std::max(1, canvas_h);
        ImGui::TextUnformatted("Placement");
        for (int i = 0; i < 9; ++i) {
            if (i % 3) ImGui::SameLine();
            ImGui::PushID(i);
            const bool sel = canvas_anchor == i;
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            if (ImGui::Button(sel ? "o" : " ", ImVec2(28, 28))) canvas_anchor = i;
            if (sel) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::TextDisabled("Background layers are padded with the background colour.");
        if (ImGui::Button("OK")) {
            if (doc && (canvas_w != doc->width() || canvas_h != doc->height())) {
                const int dx = canvas_w - doc->width(), dy = canvas_h - doc->height();
                const int ox = (canvas_anchor % 3) * dx / 2, oy = (canvas_anchor / 3) * dy / 2;
                tool().cancel(*this);
                run(std::make_unique<CanvasSizeCommand>(canvas_w, canvas_h, ox, oy, background_fill()));
                fit_requested = true;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Free Rotate", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        escape();
        ImGui::RadioButton("Right (clockwise)", &rotate_cw, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Left", &rotate_cw, 0);
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("Degrees", &rotate_degrees, 0.0f, 359.99f, "%.2f");
        ImGui::TextDisabled("Uncovered corners take the background colour on Background layers.");
        if (ImGui::Button("OK")) { rotate(rotate_cw ? rotate_degrees : -rotate_degrees); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
