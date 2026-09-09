// Text tool dialog: previews on a temporary layer, commits as a new layer.
#include <algorithm>
#include <cstring>

#include "App.h"
#include "firn/commands.h"
#include "imgui.h"

using namespace firn;

void App::ensure_fonts() {
    if (fonts_loaded) return;
    fonts = text::list_fonts();
    fonts_loaded = true;
    // Prefer a common sans face as the default.
    for (size_t i = 0; i < fonts.size(); ++i)
        if ((fonts[i].family == "DejaVu Sans" || fonts[i].family == "Arial" || fonts[i].family == "Liberation Sans") && fonts[i].style == "Regular") { font_index = static_cast<int>(i); break; }
}

namespace {

void render_preview(App& app) {
    if (!app.doc || app.text_temp_layer < 0 || !app.text_font) return;
    Layer& L = app.doc->layer(app.text_temp_layer);
    L.pixels = Image(app.doc->width(), app.doc->height(), {0, 0, 0, 0});
    if (!app.text_buf[0]) { app.doc->touch(); return; }
    auto c8 = [](float f) { return static_cast<uint8_t>(f * 255.0f + 0.5f); };
    // The original fills text with the background material.
    const Color fill{c8(app.bg_color[0]), c8(app.bg_color[1]), c8(app.bg_color[2]), c8(app.bg_color[3])};
    text::Font::Layout lay;
    Image glyphs = app.text_font->render(app.text_buf, app.text_size, fill, app.text_antialias,
                                         static_cast<text::Font::Align>(app.text_align), 1.0f, 0.0f, &lay);
    for (int y = 0; y < glyphs.height(); ++y) {
        const int dy = app.text_y + y;
        if (dy < 0 || dy >= L.pixels.height()) continue;
        for (int x = 0; x < glyphs.width(); ++x) {
            const int dx = app.text_x + x;
            if (dx < 0 || dx >= L.pixels.width()) continue;
            std::memcpy(L.pixels.data() + (static_cast<size_t>(dy) * L.pixels.width() + dx) * 4,
                        glyphs.data() + (static_cast<size_t>(y) * glyphs.width() + x) * 4, 4);
        }
    }
    app.doc->touch();
}

}  // namespace

void App::draw_text_dialog() {
    if (show_text_dialog) {
        show_text_dialog = false;
        if (doc) {
            ensure_fonts();
            if (!fonts.empty()) {
                font_index = std::clamp(font_index, 0, static_cast<int>(fonts.size()) - 1);
                if (!text_font || text_font->info().path != fonts[font_index].path) text_font = text::Font::load(fonts[font_index].path);
            }
            if (text_font) {
                text_prev_active = active_layer();
                doc->add_layer("Text");
                text_temp_layer = static_cast<int>(doc->layer_count()) - 1;
                render_preview(*this);
                ImGui::OpenPopup("Text Entry");
            } else {
                status = "Text: no TrueType fonts found (set FIRN_FONT_DIRS).";
            }
        }
    }
    if (!ImGui::BeginPopupModal("Text Entry", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    bool changed = false;
    ImGui::SetNextItemWidth(320);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    changed |= ImGui::InputTextMultiline("##text", text_buf, sizeof(text_buf), ImVec2(320, 80));
    ImGui::SetNextItemWidth(320);
    const std::string current = fonts.empty() ? "" : fonts[font_index].family + "  " + fonts[font_index].style;
    if (ImGui::BeginCombo("Font", current.c_str())) {
        for (size_t i = 0; i < fonts.size(); ++i) {
            const std::string label = fonts[i].family + "  " + fonts[i].style;
            if (ImGui::Selectable(label.c_str(), static_cast<int>(i) == font_index)) {
                font_index = static_cast<int>(i);
                text_font = text::Font::load(fonts[i].path);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(140);
    changed |= ImGui::SliderFloat("Size", &text_size, 4.0f, 500.0f, "%.0f px", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    changed |= ImGui::Combo("Align", &text_align, "Left\0Centre\0Right\0");
    changed |= ImGui::Checkbox("Anti-alias", &text_antialias);
    ImGui::SameLine();
    changed |= ImGui::ColorEdit4("Fill (background material)", bg_color, ImGuiColorEditFlags_NoInputs);
    ImGui::SetNextItemWidth(100);
    changed |= ImGui::InputInt("X", &text_x);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    changed |= ImGui::InputInt("Y", &text_y);
    if (changed) render_preview(*this);

    ImGui::Separator();
    const bool ok = ImGui::Button("OK", ImVec2(80, 0));
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (ok || cancel) {
        Image pixels;
        if (doc && text_temp_layer >= 0) {
            pixels = doc->layer(text_temp_layer).pixels;
            doc->remove_layer(text_temp_layer);
            doc->set_active_layer(text_prev_active);
        }
        text_temp_layer = -1;
        if (ok && doc && text_buf[0]) {
            std::string name = text_buf;
            name = name.substr(0, name.find('\n')).substr(0, 32);
            run(std::make_unique<PasteLayerCommand>(name, std::move(pixels)));
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
