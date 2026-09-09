#include <algorithm>
#include <memory>

#include "App.h"
#include "firn/adjust.h"
#include "firn/io_psp.h"
#include "firn/effects.h"
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
        if (ImGui::BeginMenu("Recent Files", !config.recent_files.empty())) {
            for (size_t i = 0; i < config.recent_files.size(); ++i) {
                const std::string& r = config.recent_files[i];
                if (ImGui::MenuItem(r.c_str())) { open_document(r); break; }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Close", "Ctrl+W", false, has_doc)) close_document(current_doc);
        if (ImGui::MenuItem("Close All", nullptr, false, has_doc)) { for (int i = static_cast<int>(docs.size()) - 1; i >= 0; --i) if (!document_modified(i)) close_document(i); if (!docs.empty()) close_document(0); }
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, has_doc)) save();
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, has_doc)) request_save_as();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) request_quit();
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
        ImGui::MenuItem("Rulers", nullptr, &show_rulers);
        ImGui::MenuItem("Grid", nullptr, &show_grid);
        ImGui::SetNextItemWidth(100);
        ImGui::InputInt("Grid spacing", &grid_spacing);
        grid_spacing = std::clamp(grid_spacing, 1, 1000);
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
            if (ImGui::MenuItem("Color Balance...")) open_adjust = Adj::ColorBalance;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Hue and Saturation", has_layer)) {
            if (ImGui::MenuItem("Colorize...")) open_adjust = Adj::Colorize;
            if (ImGui::MenuItem("Hue Map...")) open_adjust = Adj::HueMap;
            if (ImGui::MenuItem("Hue/Saturation/Lightness...")) open_adjust = Adj::HSL;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Add/Remove Noise", has_layer)) {
            if (ImGui::MenuItem("Add Noise...")) open_adjust = Adj::AddNoise;
            if (ImGui::MenuItem("Median Filter...")) open_adjust = Adj::Median;
            if (ImGui::MenuItem("Despeckle")) run(std::make_unique<AdjustCommand>(layer, "Despeckle", [](Image& i) { effects::median(i, 1); }));
            if (ImGui::MenuItem("Erode")) run(std::make_unique<AdjustCommand>(layer, "Erode", effects::erode));
            if (ImGui::MenuItem("Dilate")) run(std::make_unique<AdjustCommand>(layer, "Dilate", effects::dilate));
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Blur", has_layer)) {
            if (ImGui::MenuItem("Average...")) open_adjust = Adj::Average;
            if (ImGui::MenuItem("Blur More")) run(std::make_unique<AdjustCommand>(layer, "Blur More", effects::blur_more));
            if (ImGui::MenuItem("Gaussian Blur...")) open_adjust = Adj::Gaussian;
            if (ImGui::MenuItem("Motion Blur...")) open_adjust = Adj::MotionBlur;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Sharpness", has_layer)) {
            if (ImGui::MenuItem("Sharpen")) run(std::make_unique<AdjustCommand>(layer, "Sharpen", effects::sharpen));
            if (ImGui::MenuItem("Sharpen More")) run(std::make_unique<AdjustCommand>(layer, "Sharpen More", effects::sharpen_more));
            if (ImGui::MenuItem("Unsharp Mask...")) open_adjust = Adj::UnsharpMask;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Softness", has_layer)) {
            if (ImGui::MenuItem("Soften")) run(std::make_unique<AdjustCommand>(layer, "Soften", effects::soften));
            if (ImGui::MenuItem("Soften More")) run(std::make_unique<AdjustCommand>(layer, "Soften More", effects::soften_more));
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
        if (ImGui::BeginMenu("3D Effects", has_layer)) {
            if (ImGui::MenuItem("Buttonize...")) open_adjust = Adj::Buttonize;
            if (ImGui::MenuItem("Cutout...")) open_adjust = Adj::Cutout;
            if (ImGui::MenuItem("Drop Shadow...")) open_adjust = Adj::DropShadow;
            if (ImGui::MenuItem("Inner Bevel...")) open_adjust = Adj::InnerBevel;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Distortion Effects", has_layer)) {
            if (ImGui::MenuItem("Pinch / Punch...")) open_adjust = Adj::Pinch;
            if (ImGui::MenuItem("Twirl...")) open_adjust = Adj::Twirl;
            if (ImGui::MenuItem("Wave...")) open_adjust = Adj::Wave;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Artistic Effects", has_layer)) {
            if (ImGui::MenuItem("Posterize...")) open_adjust = Adj::Posterize;
            if (ImGui::MenuItem("Sepia Toning...")) open_adjust = Adj::Sepia;
            if (ImGui::MenuItem("Solarize...")) open_adjust = Adj::Solarize;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edge Effects", has_layer)) {
            if (ImGui::MenuItem("Enhance")) run(std::make_unique<AdjustCommand>(layer, "Enhance Edges", effects::enhance_edges));
            if (ImGui::MenuItem("Enhance More")) run(std::make_unique<AdjustCommand>(layer, "Enhance Edges More", effects::enhance_edges_more));
            if (ImGui::MenuItem("Find All")) run(std::make_unique<AdjustCommand>(layer, "Find Edges", effects::find_edges));
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Texture Effects", has_layer)) {
            if (ImGui::MenuItem("Emboss")) run(std::make_unique<AdjustCommand>(layer, "Emboss", effects::emboss));
            if (ImGui::MenuItem("Mosaic...")) open_adjust = Adj::Mosaic;
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
    if (ImGui::BeginMenu("Window")) {
        for (int i = 0; i < static_cast<int>(docs.size()); ++i) {
            const std::string label = document_title(i) + (document_modified(i) ? "*" : "");
            if (ImGui::MenuItem(label.c_str(), nullptr, i == current_doc)) activate_document(i);
        }
        if (docs.empty()) ImGui::MenuItem("(no images open)", nullptr, false, false);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void App::draw_dialogs() {
    draw_adjust_dialogs();
    draw_text_dialog();

    if (file_dialog.draw()) {
        if (file_op == PendingFileOp::Open) open_document(file_dialog.path());
        else if (file_op == PendingFileOp::SaveAs) save_document(file_dialog.path());
        file_op = PendingFileOp::None;
    }

    if (show_new_dialog) { ImGui::OpenPopup("New Image"); show_new_dialog = false; }
    if (pending_close >= 0 && !ImGui::IsPopupOpen("Unsaved Changes")) ImGui::OpenPopup("Unsaved Changes");

    if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const int idx = pending_close;
        if (idx < 0 || idx >= static_cast<int>(docs.size())) { pending_close = -1; ImGui::CloseCurrentPopup(); }
        else {
            ImGui::Text("Save changes to \"%s\" before closing?", document_title(idx).c_str());
            if (ImGui::Button("Save", ImVec2(90, 0))) {
                activate_document(idx);
                pending_close = -1;
                ImGui::CloseCurrentPopup();
                // Saving may need a dialog; close afterwards only if it succeeded in place.
                if (!doc_path.empty() && io::is_psp_extension(doc_path) ? save_document(doc_path) : false) close_document(idx, true);
                else { request_save_as(); pending_quit = false; }
            }
            ImGui::SameLine();
            if (ImGui::Button("Don't Save", ImVec2(90, 0))) {
                pending_close = -1;
                ImGui::CloseCurrentPopup();
                close_document(idx, true);
                if (pending_quit) request_quit();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                pending_close = -1;
                pending_quit = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    static const char* kSelDialogs[] = {nullptr, "Expand Selection", "Contract Selection", "Feather Selection"};
    if (show_sel_dialog) { ImGui::OpenPopup(kSelDialogs[show_sel_dialog]); show_sel_dialog = 0; }
    if (show_layer_props_dialog) { ImGui::OpenPopup("Layer Properties"); show_layer_props_dialog = false; }
    if (show_resize_dialog) { ImGui::OpenPopup("Resize"); show_resize_dialog = false; }
    if (show_canvas_dialog) { ImGui::OpenPopup("Canvas Size"); show_canvas_dialog = false; }
    if (show_rotate_dialog) { ImGui::OpenPopup("Free Rotate"); show_rotate_dialog = false; }

    auto escape = [] { if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup(); };
    // Enter accepts unless a multi-line field has the keyboard.
    auto enter = [] { return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false); };

    if (ImGui::BeginPopupModal("New Image", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        escape();
        ImGui::InputInt("Width", &new_w);
        ImGui::InputInt("Height", &new_h);
        if (new_w < 1) new_w = 1;
        if (new_h < 1) new_h = 1;
        if (ImGui::Button("OK") || enter()) { new_document(new_w, new_h); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    for (int which = 1; which <= 3; ++which) {
        if (!ImGui::BeginPopupModal(kSelDialogs[which], nullptr, ImGuiWindowFlags_AlwaysAutoResize)) continue;
        escape();
        ImGui::SliderInt("Pixels", &sel_modify_px, 1, 100);
        if (ImGui::Button("OK") || enter()) {
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
        if (ImGui::Button("OK") || enter()) {
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
        if (ImGui::Button("OK") || enter()) {
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
        if (ImGui::Button("OK") || enter()) {
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
        if (ImGui::Button("OK") || enter()) { rotate(rotate_cw ? rotate_degrees : -rotate_degrees); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
