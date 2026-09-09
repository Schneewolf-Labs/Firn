// Image menu extras: Add Borders, Picture Frame, Count Colors, Decrease
// Color Depth, Split/Combine Channel, Arithmetic, and palette files.
#include <algorithm>
#include <cstdio>
#include <memory>

#include "App.h"
#include "firn/commands.h"
#include "firn/io.h"
#include "firn/io_psp.h"
#include "firn/raster.h"
#include "imgui.h"

using namespace firn;

Document* App::document_at(int index) {
    if (index < 0 || index >= static_cast<int>(docs.size())) return nullptr;
    return index == current_doc ? doc.get() : docs[static_cast<size_t>(index)].doc.get();
}

void App::image_count_colors() {
    if (!doc) return;
    const size_t n = raster::count_colors(doc->composite());
    status = "Count Colors Used: " + std::to_string(n) + " unique colors in the image";
}

void App::image_decrease_depth(int colors, bool dither) {
    if (!doc || !active_is_raster()) return;
    const std::string name = colors <= 2 ? "Decrease Color Depth (2 Colors)" : "Decrease Color Depth (" + std::to_string(colors) + " Colors)";
    run(std::make_unique<AdjustCommand>(active_layer(), name, [colors, dither](Image& img) {
        if (colors <= 2) raster::to_monochrome(img, dither);
        else raster::apply_palette(img, raster::median_cut_palette(img, colors), dither);
    }));
}

void App::image_split_channels(int mode) {
    if (!doc) return;
    static const char* names[3][4] = {{"Red", "Green", "Blue", ""}, {"Hue", "Saturation", "Lightness", ""}, {"Cyan", "Magenta", "Yellow", "Black"}};
    const std::string base = doc_title.substr(0, doc_title.find_last_of('.'));
    const auto planes = raster::split_channels(doc->composite(), mode);
    for (size_t i = 0; i < planes.size(); ++i) {
        auto d = std::make_unique<Document>(planes[i].width(), planes[i].height());
        Layer& L = d->add_layer("Background");
        L.pixels = planes[i];
        L.background = true;
        add_document(std::move(d), "");
        doc_title = names[std::clamp(mode, 0, 2)][i] + std::string(" - ") + base;
    }
}

void App::request_load_palette() {
    if (!doc) return;
    file_op = PendingFileOp::LoadPalette;
    file_dialog.open(FileDialog::Mode::Open, "Load Palette", {"psppalette", "pal"}, "");
}

void App::request_save_palette() {
    if (!doc) return;
    file_op = PendingFileOp::SavePalette;
    file_dialog.open(FileDialog::Mode::Save, "Save Palette", {"psppalette", "pal"}, "palette.PspPalette");
}

void App::load_palette(const std::string& path) {
    if (!doc || !active_is_raster()) return;
    std::string err;
    std::vector<Color> pal = io::load_palette(path, &err);
    if (pal.empty()) { status = "Load Palette: " + err; return; }
    const bool dither = depth_dither;
    run(std::make_unique<AdjustCommand>(active_layer(), "Load Palette", [pal, dither](Image& img) { raster::apply_palette(img, pal, dither); }));
}

void App::save_palette(const std::string& path) {
    if (!doc) return;
    const Image flat = doc->composite();
    std::vector<Color> pal = raster::median_cut_palette(flat, std::clamp(static_cast<int>(raster::count_colors(flat)), 2, 256));
    std::string err;
    if (io::save_palette(pal, path, &err)) status = "Saved palette " + path + " (" + std::to_string(pal.size()) + " colors)";
    else status = "Save Palette: " + err;
}

namespace {

bool doc_combo(App& app, const char* label, int& index) {
    const std::string current = index >= 0 && index < static_cast<int>(app.docs.size()) ? app.document_title(index) : "(none)";
    ImGui::SetNextItemWidth(220);
    bool changed = false;
    if (ImGui::BeginCombo(label, current.c_str())) {
        for (int i = 0; i < static_cast<int>(app.docs.size()); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(app.document_title(i).c_str(), i == index)) { index = i; changed = true; }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

}  // namespace

void App::draw_image_dialogs() {
    if (show_borders_dialog) { ImGui::OpenPopup("Add Borders"); show_borders_dialog = false; }
    if (show_frame_dialog) { ImGui::OpenPopup("Picture Frame"); show_frame_dialog = false; ensure_frames(); }
    if (show_depth_dialog) { ImGui::OpenPopup("Decrease Color Depth"); show_depth_dialog = false; }
    if (show_combine_dialog) { ImGui::OpenPopup("Combine Channel"); show_combine_dialog = false; }
    if (show_arith_dialog) { ImGui::OpenPopup("Image Arithmetic"); show_arith_dialog = false; }

    if (ImGui::BeginPopupModal("Add Borders", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Checkbox("Symmetric", &border_symmetric);
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Top", &border_t) && border_symmetric) border_b = border_l = border_r = border_t;
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Bottom", &border_b) && border_symmetric) border_t = border_l = border_r = border_b;
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Left", &border_l) && border_symmetric) border_t = border_b = border_r = border_l;
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Right", &border_r) && border_symmetric) border_t = border_b = border_l = border_r;
        for (int* v : {&border_t, &border_b, &border_l, &border_r}) *v = std::clamp(*v, 0, 10000);
        ImGui::ColorEdit4("Color (background material)", bg_color, ImGuiColorEditFlags_NoInputs);
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
            if (doc) run(std::make_unique<CanvasSizeCommand>(doc->width() + border_l + border_r, doc->height() + border_t + border_b, border_l, border_t, background_fill()));
            fit_requested = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Picture Frame", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const char* current = frame_index >= 0 && frame_index < static_cast<int>(frame_library.size()) ? frame_library[static_cast<size_t>(frame_index)].name.c_str() : "(no frames found)";
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("Frame", current)) {
            for (size_t i = 0; i < frame_library.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(frame_library[i].name.c_str(), static_cast<int>(i) == frame_index)) frame_index = static_cast<int>(i);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("From ~/.config/firn/frames or FIRN_FRAME_DIRS (.PspFrame files).");
        if (ImGui::RadioButton("Frame inside of the image", frame_inside)) frame_inside = true;
        if (ImGui::RadioButton("Frame outside of the image", !frame_inside)) frame_inside = false;
        ImGui::Checkbox("Flip", &frame_flip); ImGui::SameLine(); ImGui::Checkbox("Mirror", &frame_mirror);
        ImGui::Separator();
        const bool ok = ImGui::Button("OK", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        if (ok && doc && frame_index >= 0 && frame_index < static_cast<int>(frame_library.size())) {
            std::string err; std::vector<std::string> warnings;
            auto fd = io::load_document(frame_library[static_cast<size_t>(frame_index)].path, &err, &warnings);
            if (!fd) status = "Picture Frame: " + err;
            else {
                Image frame = fd->composite();
                if (frame_flip) raster::flip_vertical(frame);
                if (frame_mirror) raster::mirror_horizontal(frame);
                const int W = doc->width(), H = doc->height();
                doc->set_active_layer(static_cast<int>(doc->layer_count()) - 1);   // frames go on top
                if (frame_inside) {
                    run(std::make_unique<PasteLayerCommand>("Picture Frame", raster::resample(frame, W, H, raster::Filter::Bicubic)));
                } else {
                    // Scale the frame so its transparent interior wraps the image, growing the canvas.
                    int ix0 = frame.width(), iy0 = frame.height(), ix1 = -1, iy1 = -1;
                    for (int y = 0; y < frame.height(); ++y)
                        for (int x = 0; x < frame.width(); ++x)
                            if (frame.get(x, y).a < 128) { ix0 = std::min(ix0, x); ix1 = std::max(ix1, x); iy0 = std::min(iy0, y); iy1 = std::max(iy1, y); }
                    if (ix1 <= ix0 || iy1 <= iy0) { ix0 = frame.width() / 8; iy0 = frame.height() / 8; ix1 = frame.width() - ix0; iy1 = frame.height() - iy0; }
                    const float sx = static_cast<float>(W) / (ix1 - ix0), sy = static_cast<float>(H) / (iy1 - iy0);
                    const int fw = std::max(1, static_cast<int>(frame.width() * sx)), fh = std::max(1, static_cast<int>(frame.height() * sy));
                    const int offx = static_cast<int>(ix0 * sx), offy = static_cast<int>(iy0 * sy);
                    run(std::make_unique<CanvasSizeCommand>(fw, fh, offx, offy, background_fill()));
                    doc->set_active_layer(static_cast<int>(doc->layer_count()) - 1);
                    run(std::make_unique<PasteLayerCommand>("Picture Frame", raster::resample(frame, fw, fh, raster::Filter::Bicubic)));
                    fit_requested = true;
                }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Decrease Color Depth", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%d colors", depth_colors);
        ImGui::Checkbox("Error diffusion dithering", &depth_dither);
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) { image_decrease_depth(depth_colors, depth_dither); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Combine Channel", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(160);
        ImGui::Combo("Mode", &combine_mode, "RGB\0HSL\0CMYK\0");
        static const char* labels[3][4] = {{"Red", "Green", "Blue", ""}, {"Hue", "Saturation", "Lightness", ""}, {"Cyan", "Magenta", "Yellow", "Black"}};
        const int n = combine_mode == 2 ? 4 : 3;
        for (int i = 0; i < n; ++i) doc_combo(*this, labels[combine_mode][i], combine_src[i]);
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
            std::vector<Image> planes;
            for (int i = 0; i < n; ++i) if (Document* d = document_at(combine_src[i])) planes.push_back(d->composite());
            Image out = static_cast<int>(planes.size()) == n ? raster::combine_channels(planes, combine_mode) : Image();
            if (out.empty()) status = "Combine Channel: the source images must all be the same size.";
            else {
                auto d = std::make_unique<Document>(out.width(), out.height());
                Layer& L = d->add_layer("Background");
                L.pixels = std::move(out);
                L.background = true;
                add_document(std::move(d), "");
                doc_title = "Combined";
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Image Arithmetic", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        doc_combo(*this, "Image #1", arith_a);
        doc_combo(*this, "Image #2", arith_b);
        ImGui::SetNextItemWidth(160);
        ImGui::Combo("Function", &arith_op, "Add\0Subtract\0Multiply\0Difference\0Lightest\0Darkest\0Average\0AND\0OR\0XOR\0");
        ImGui::SetNextItemWidth(160);
        ImGui::Combo("Channel", &arith_channel, "All channels\0Red\0Green\0Blue\0");
        ImGui::SetNextItemWidth(160);
        ImGui::InputFloat("Divisor", &arith_divisor, 0.5f, 1.0f, "%.1f");
        ImGui::SetNextItemWidth(160);
        ImGui::InputInt("Bias", &arith_bias);
        ImGui::Checkbox("Clip color values", &arith_clip);
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
            Document* a = document_at(arith_a);
            Document* b = document_at(arith_b);
            if (a && b) {
                Image out = raster::arithmetic(a->composite(), b->composite(), static_cast<raster::ArithOp>(arith_op), arith_divisor, arith_bias, arith_clip, arith_channel);
                auto d = std::make_unique<Document>(out.width(), out.height());
                Layer& L = d->add_layer("Background");
                L.pixels = std::move(out);
                L.background = true;
                add_document(std::move(d), "");
                doc_title = "Arithmetic";
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
