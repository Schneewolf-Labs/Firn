// Text tool dialog: previews on a temporary layer, commits as a new layer.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "firn/mask.h"
#include "firn/raster.h"

#include "App.h"
#include "ui/TextDialogState.h"
#include "firn/commands.h"
#include "firn/vector.h"
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

vec::TextInfo current_text_info(const App& app) {
    vec::TextInfo t;
    t.text = app.text_dialog_state->text_buf;
    t.font_path = app.text_font ? app.text_font->info().path : "";
    t.font_family = app.fonts.empty() ? "" : app.fonts[app.font_index].family + "  " + app.fonts[app.font_index].style;
    t.size = app.text_size;
    t.align = app.text_dialog_state->text_align;
    t.rotation = app.text_dialog_state->text_angle;
    t.antialias = app.text_antialias;
    return t;
}

// Vector mode: the text is an object on the vector layer, restyled from the
// materials (fill = background, stroke = foreground when the width is > 0).
void render_vector_preview(App& app) {
    if (!app.doc || app.text_dialog_state->text_vec_layer < 0 || app.text_dialog_state->text_vec_index < 0 || !app.text_font) return;
    auto& objs = app.doc->layer(app.text_dialog_state->text_vec_layer).objects;
    if (app.text_dialog_state->text_vec_index >= static_cast<int>(objs.size())) return;
    vec::Object& o = objs[app.text_dialog_state->text_vec_index];
    app.place_text_object(o, current_text_info(app), static_cast<float>(app.text_x), static_cast<float>(app.text_y));
    o.fill = app.material_style(false);
    o.stroke = app.text_stroke > 0.0f ? app.material_style(true) : vec::PaintStyle{};
    o.stroke_width = app.text_stroke;
    o.antialias = app.text_antialias;
    std::string name = app.text_dialog_state->text_buf;
    o.name = name.substr(0, name.find('\n')).substr(0, 32);
    o.selected = true;
    app.doc->rasterize_vector_layer(app.text_dialog_state->text_vec_layer);
}

void render_preview(App& app) {
    if (app.text_dialog_state->text_vec_layer >= 0) { render_vector_preview(app); return; }
    if (!app.doc || app.text_dialog_state->text_temp_layer < 0 || !app.text_font) return;
    Layer& L = app.doc->layer(app.text_dialog_state->text_temp_layer);
    L.pixels = Image(app.doc->width(), app.doc->height(), {0, 0, 0, 0});
    if (!app.text_dialog_state->text_buf[0]) { app.doc->touch(); return; }
    auto c8 = [](float f) { return static_cast<uint8_t>(f * 255.0f + 0.5f); };
    // The original fills text with the background material.
    const Color fill{c8(app.bg_color[0]), c8(app.bg_color[1]), c8(app.bg_color[2]), c8(app.bg_color[3])};
    text::Font::Layout lay;
    Image glyphs = app.text_font->render(app.text_dialog_state->text_buf, app.text_size, fill, app.text_antialias,
                                         static_cast<text::Font::Align>(app.text_dialog_state->text_align), 1.0f, 0.0f, &lay);
    // Stroke: the foreground material painted where a dilated glyph mask
    // extends beyond the glyphs, underneath the fill.
    if (app.text_stroke > 0.0f) {
        const int pad = static_cast<int>(std::ceil(app.text_stroke)) + 1;
        Image padded(glyphs.width() + 2 * pad, glyphs.height() + 2 * pad, {0, 0, 0, 0});
        for (int y = 0; y < glyphs.height(); ++y)
            std::memcpy(padded.data() + (static_cast<size_t>(y + pad) * padded.width() + pad) * 4,
                        glyphs.data() + static_cast<size_t>(y) * glyphs.width() * 4, static_cast<size_t>(glyphs.width()) * 4);
        Mask m(padded.width(), padded.height());
        for (size_t i = 0; i < m.size(); ++i) m.data()[i] = padded.data()[i * 4 + 3];
        Mask ring = m;
        mask::expand(ring, static_cast<int>(std::lround(app.text_stroke)));
        const Color stroke{c8(app.fg_color[0]), c8(app.fg_color[1]), c8(app.fg_color[2]), c8(app.fg_color[3])};
        Image out(padded.width(), padded.height(), {0, 0, 0, 0});
        raster::paint_mask(out, ring, stroke);
        for (int y = 0; y < out.height(); ++y)
            for (int x = 0; x < out.width(); ++x) raster::blend_over(out, x, y, padded.get(x, y), 1.0f);
        glyphs = std::move(out);
        app.text_dialog_state->text_x_offset = -pad; app.text_dialog_state->text_y_offset = -pad;
    } else {
        app.text_dialog_state->text_x_offset = app.text_dialog_state->text_y_offset = 0;
    }
    if (app.text_dialog_state->text_angle != 0.0f) {
        const int ow = glyphs.width(), oh = glyphs.height();
        glyphs = raster::rotate(glyphs, app.text_dialog_state->text_angle);
        app.text_dialog_state->text_x_offset -= (glyphs.width() - ow) / 2;
        app.text_dialog_state->text_y_offset -= (glyphs.height() - oh) / 2;
    }
    for (int y = 0; y < glyphs.height(); ++y) {
        const int dy = app.text_y + app.text_dialog_state->text_y_offset + y;
        if (dy < 0 || dy >= L.pixels.height()) continue;
        for (int x = 0; x < glyphs.width(); ++x) {
            const int dx = app.text_x + app.text_dialog_state->text_x_offset + x;
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
            if (text_font && (create_as_vector || text_edit_object >= 0)) {
                text_dialog_state->text_vec_layer = vector_layer_for_edit(true);
                text_dialog_state->text_temp_layer = -1;
                if (text_dialog_state->text_vec_layer >= 0) {
                    auto& objs = doc->layer(text_dialog_state->text_vec_layer).objects;
                    text_vec_before = objs;
                    for (vec::Object& o : objs) o.selected = false;
                    if (text_edit_object >= 0 && text_edit_object < static_cast<int>(objs.size()) && objs[text_edit_object].is_text) {
                        // Re-edit: load the object's settings into the dialog.
                        const vec::Object& o = objs[text_edit_object];
                        std::snprintf(text_dialog_state->text_buf, sizeof(text_dialog_state->text_buf), "%s", o.text.text.c_str());
                        text_size = o.text.size; text_dialog_state->text_align = o.text.align; text_dialog_state->text_angle = o.text.rotation; text_antialias = o.text.antialias;
                        text_stroke = o.stroke.enabled() ? o.stroke_width : 0.0f;
                        for (size_t i = 0; i < fonts.size(); ++i) if (fonts[i].path == o.text.font_path) { font_index = static_cast<int>(i); text_font = text::Font::load(fonts[i].path); }
                        float bx0, by0, bx1, by1;
                        if (vec::outline_bounds(o, &bx0, &by0, &bx1, &by1)) { text_x = static_cast<int>(bx0); text_y = static_cast<int>(by0); }
                        text_dialog_state->text_vec_index = text_edit_object;
                    } else {
                        objs.push_back(vec::Object{});
                        text_dialog_state->text_vec_index = static_cast<int>(objs.size()) - 1;
                    }
                    render_preview(*this);
                    ImGui::OpenPopup("Text Entry");
                }
            } else if (text_font) {
                text_dialog_state->text_vec_layer = -1;
                text_dialog_state->text_prev_active = active_layer();
                doc->add_layer("Text");
                text_dialog_state->text_temp_layer = static_cast<int>(doc->layer_count()) - 1;
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
    changed |= ImGui::InputTextMultiline("##text", text_dialog_state->text_buf, sizeof(text_dialog_state->text_buf), ImVec2(320, 80));
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
    changed |= ImGui::Combo("Align", &text_dialog_state->text_align, "Left\0Center\0Right\0");
    changed |= ImGui::Checkbox("Anti-alias", &text_antialias);
    ImGui::SameLine();
    changed |= ImGui::ColorEdit4("Fill (background material)", bg_color, ImGuiColorEditFlags_NoInputs);
    if (text_dialog_state->text_vec_layer < 0) { ImGui::SameLine(); changed |= ImGui::Checkbox("Create as vector", &create_as_vector); }
    else { ImGui::SameLine(); ImGui::TextDisabled("(vector object)"); }
    ImGui::SetNextItemWidth(140);
    changed |= ImGui::SliderFloat("Stroke width", &text_stroke, 0.0f, 50.0f, "%.0f px");
    ImGui::SameLine();
    changed |= ImGui::ColorEdit4("Stroke (foreground)", fg_color, ImGuiColorEditFlags_NoInputs);
    ImGui::SetNextItemWidth(140);
    changed |= ImGui::SliderFloat("Rotation", &text_dialog_state->text_angle, -180.0f, 180.0f, "%.0f deg");
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
    if ((ok || cancel) && text_dialog_state->text_vec_layer >= 0) {
        if (doc && text_dialog_state->text_vec_layer < static_cast<int>(doc->layer_count())) {
            if (ok && text_dialog_state->text_buf[0]) objects_changed(text_edit_object >= 0 ? "Edit Text" : "Text", text_vec_before);
            else { doc->layer(text_dialog_state->text_vec_layer).objects = text_vec_before; doc->rasterize_vector_layer(text_dialog_state->text_vec_layer); }
        }
        text_dialog_state->text_vec_layer = -1; text_dialog_state->text_vec_index = -1; text_edit_object = -1;
        ImGui::CloseCurrentPopup();
    } else if (ok || cancel) {
        Image pixels;
        if (doc && text_dialog_state->text_temp_layer >= 0) {
            pixels = doc->layer(text_dialog_state->text_temp_layer).pixels;
            doc->remove_layer(text_dialog_state->text_temp_layer);
            doc->set_active_layer(text_dialog_state->text_prev_active);
        }
        text_dialog_state->text_temp_layer = -1;
        if (ok && doc && text_dialog_state->text_buf[0]) {
            std::string name = text_dialog_state->text_buf;
            name = name.substr(0, name.find('\n')).substr(0, 32);
            run(std::make_unique<PasteLayerCommand>(name, std::move(pixels)));
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
