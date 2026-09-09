// Vector Properties dialog, the materials editor (gradient / pattern
// materials in the Materials palette), and the small option widgets shared
// by the shape tools.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "App.h"
#include "firn/commands.h"
#include "firn/io.h"
#include "firn/io_psp.h"
#include "imgui.h"

using namespace firn;

namespace {

std::shared_ptr<const Image> load_pattern(const std::string& path) {
    std::string err;
    if (io::is_psp_extension(path)) {
        std::vector<std::string> warnings;
        auto d = io::load_document(path, &err, &warnings);
        if (!d) return nullptr;
        return std::make_shared<Image>(d->composite());
    }
    auto img = io::load(path, &err);
    if (!img) return nullptr;
    return std::make_shared<Image>(std::move(*img));
}

// A strip of the gradient's colors over white, as a preview.
void gradient_strip(const vec::Gradient& g, float width, float height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const int n = std::max(8, static_cast<int>(width / 3));
    for (int i = 0; i < n; ++i) {
        float t = static_cast<float>(i) / (n - 1);
        if (g.repeats > 0) t = std::fmod(t * (g.repeats + 1), 1.0f);
        if (g.invert) t = 1.0f - t;
        const Color c = g.at(t);
        const float a = c.a / 255.0f;
        const ImU32 col = IM_COL32(static_cast<int>(c.r * a + 255 * (1 - a)), static_cast<int>(c.g * a + 255 * (1 - a)), static_cast<int>(c.b * a + 255 * (1 - a)), 255);
        dl->AddRectFilled(ImVec2(p.x + width * i / n, p.y), ImVec2(p.x + width * (i + 1) / n + 1, p.y + height), col);
    }
    dl->AddRect(p, ImVec2(p.x + width, p.y + height), IM_COL32(0, 0, 0, 255));
    ImGui::Dummy(ImVec2(width, height));
}

bool gradient_library_combo(App& app, const char* label, int& index, vec::Gradient& g, const vec::Gradient* fallback) {
    app.ensure_gradients();
    bool changed = false;
    const char* current = index >= 0 && index < static_cast<int>(app.gradient_library.size()) ? app.gradient_library[index].name.c_str()
                          : fallback ? fallback->name.c_str() : "Foreground-Background";
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo(label, current)) {
        if (fallback && ImGui::Selectable(fallback->name.c_str(), index < 0)) { index = -1; g = *fallback; changed = true; }
        for (size_t i = 0; i < app.gradient_library.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(app.gradient_library[i].name.c_str(), index == static_cast<int>(i))) {
                index = static_cast<int>(i);
                const vec::Gradient keep = g;
                g = app.gradient_library[i];
                g.style = keep.style; g.angle = keep.angle; g.repeats = keep.repeats; g.invert = keep.invert; g.center_x = keep.center_x; g.center_y = keep.center_y;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool pattern_library_combo(App& app, const char* label, int& index, std::shared_ptr<const Image>& pattern) {
    app.ensure_patterns();
    bool changed = false;
    const char* current = index >= 0 && index < static_cast<int>(app.pattern_library.size()) ? app.pattern_library[index].name.c_str() : "(none)";
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo(label, current)) {
        for (size_t i = 0; i < app.pattern_library.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(app.pattern_library[i].name.c_str(), index == static_cast<int>(i))) {
                index = static_cast<int>(i);
                pattern = load_pattern(app.pattern_library[i].path);
                if (!pattern) app.status = "Could not load pattern " + app.pattern_library[i].path;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Editor for one paint style (stroke or fill) of an object.
bool paint_style_editor(App& app, const char* id, vec::PaintStyle& st, int& gradient_index, int& pattern_index) {
    ImGui::PushID(id);
    bool changed = false;
    int kind = static_cast<int>(st.kind);
    ImGui::SetNextItemWidth(110);
    if (ImGui::Combo("##kind", &kind, "None\0Color\0Gradient\0Pattern\0")) { st.kind = static_cast<vec::PaintStyle::Kind>(kind); changed = true; }
    if (st.kind == vec::PaintStyle::Kind::Solid) {
        ImGui::SameLine();
        float c[4] = {st.color.r / 255.0f, st.color.g / 255.0f, st.color.b / 255.0f, st.color.a / 255.0f};
        if (ImGui::ColorEdit4("##color", c, ImGuiColorEditFlags_NoInputs)) {
            auto q = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
            st.color = {q(c[0]), q(c[1]), q(c[2]), q(c[3])};
            changed = true;
        }
    } else if (st.kind == vec::PaintStyle::Kind::Gradient) {
        ImGui::SameLine();
        changed |= gradient_library_combo(app, "##grad", gradient_index, st.gradient, nullptr);
        ImGui::Indent();
        gradient_strip(st.gradient, 200, 14);
        int style = static_cast<int>(st.gradient.style);
        ImGui::SetNextItemWidth(110);
        if (ImGui::Combo("Style", &style, "Linear\0Rectangular\0Sunburst\0Radial\0")) { st.gradient.style = static_cast<vec::GradientStyle>(style); changed = true; }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        changed |= ImGui::SliderFloat("Angle", &st.gradient.angle, 0.0f, 360.0f, "%.0f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        changed |= ImGui::SliderInt("Repeats", &st.gradient.repeats, 0, 20);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Invert", &st.gradient.invert);
        ImGui::SetNextItemWidth(100);
        changed |= ImGui::SliderFloat("Center X", &st.gradient.center_x, 0.0f, 100.0f, "%.0f%%");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        changed |= ImGui::SliderFloat("Center Y", &st.gradient.center_y, 0.0f, 100.0f, "%.0f%%");
        ImGui::Unindent();
    } else if (st.kind == vec::PaintStyle::Kind::Pattern) {
        ImGui::SameLine();
        changed |= pattern_library_combo(app, "##pat", pattern_index, st.pattern);
        ImGui::Indent();
        ImGui::SetNextItemWidth(100);
        changed |= ImGui::SliderFloat("Scale", &st.pattern_scale, 0.1f, 4.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        changed |= ImGui::SliderFloat("Angle", &st.pattern_angle, 0.0f, 360.0f, "%.0f");
        ImGui::Unindent();
    }
    ImGui::PopID();
    return changed;
}

}  // namespace

// --- Materials palette ------------------------------------------------------------

void draw_material_editor(App& app, bool foreground) {
    App::Material& m = foreground ? app.fg_material : app.bg_material;
    ImGui::PushID(foreground ? "fg" : "bg");
    ImGui::SetNextItemWidth(90);
    ImGui::Combo("##kind", &m.kind, "Color\0Gradient\0Pattern\0");
    ImGui::SameLine();
    if (m.kind == 0) {
        ImGui::ColorEdit4(foreground ? "Foreground" : "Background", foreground ? app.fg_color : app.bg_color, ImGuiColorEditFlags_NoInputs);
    } else if (m.kind == 1) {
        const vec::PaintStyle fallback_style = app.material_style(foreground);
        vec::Gradient def = fallback_style.gradient;
        def.name = "Foreground-Background";
        gradient_library_combo(app, "##grad", m.gradient_index, m.gradient, &def);
        ImGui::Indent();
        gradient_strip(app.material_style(foreground).gradient, 200, 12);
        ImGui::SetNextItemWidth(100);
        ImGui::Combo("Style", &m.gradient_style, "Linear\0Rectangular\0Sunburst\0Radial\0");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::SliderFloat("Angle", &m.gradient_angle, 0.0f, 360.0f, "%.0f");
        ImGui::SetNextItemWidth(90);
        ImGui::SliderInt("Repeats", &m.gradient_repeats, 0, 20);
        ImGui::SameLine();
        ImGui::Checkbox("Invert", &m.gradient_invert);
        ImGui::Unindent();
    } else {
        pattern_library_combo(app, "##pat", m.pattern_index, m.pattern);
        ImGui::Indent();
        ImGui::SetNextItemWidth(90);
        ImGui::SliderFloat("Scale", &m.pattern_scale, 0.1f, 4.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::SliderFloat("Angle", &m.pattern_angle, 0.0f, 360.0f, "%.0f");
        ImGui::Unindent();
    }
    ImGui::PopID();
}

// --- Tool option widgets ------------------------------------------------------------

void App::draw_line_style_combo() {
    ensure_line_styles();
    const char* current = line_index >= 0 && line_index < static_cast<int>(line_library.size()) ? line_library[line_index].line.name.c_str() : "Solid";
    ImGui::SetNextItemWidth(150);
    if (ImGui::BeginCombo("Line style", current)) {
        if (ImGui::Selectable("Solid", line_index < 0)) line_index = -1;
        for (size_t i = 0; i < line_library.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(line_library[i].line.name.c_str(), line_index == static_cast<int>(i))) line_index = static_cast<int>(i);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}

void App::draw_create_as_vector() {
    ImGui::Checkbox("Create as vector", &create_as_vector);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Make an editable object on a vector layer instead of painting pixels.");
}

// --- Vector Properties dialog ------------------------------------------------------------

void App::open_vector_properties() {
    const int layer = vector_layer_for_edit(false);
    if (layer < 0) return;
    const auto& objs = doc->layer(layer).objects;
    vector_props_index = -1;
    for (size_t i = 0; i < objs.size(); ++i) if (objs[i].selected && !objs[i].is_group) { vector_props_index = static_cast<int>(i); break; }
    if (vector_props_index < 0) return;
    vector_props_layer = layer;
    vector_props_before = objs;
    vector_props_edit = objs[vector_props_index];
    show_vector_props_dialog = true;
}

void App::open_text_edit() {
    const int layer = vector_layer_for_edit(false);
    if (layer < 0) return;
    const auto& objs = doc->layer(layer).objects;
    for (size_t i = 0; i < objs.size(); ++i)
        if (objs[i].selected && objs[i].is_text) {
            text_edit_object = static_cast<int>(i);
            show_text_dialog = true;
            return;
        }
}

void App::draw_vector_dialogs() {
    if (show_vector_props_dialog) { ImGui::OpenPopup("Vector Properties"); show_vector_props_dialog = false; }
    if (!ImGui::BeginPopupModal("Vector Properties", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (!doc || vector_props_layer < 0 || vector_props_layer >= static_cast<int>(doc->layer_count()) || !doc->layer(vector_props_layer).is_vector()) {
        ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return;
    }
    vec::Object& e = vector_props_edit;
    static int stroke_grad = -1, fill_grad = -1, stroke_pat = -1, fill_pat = -1;
    bool changed = false;
    char name[256];
    std::snprintf(name, sizeof(name), "%s", e.name.c_str());
    ImGui::SetNextItemWidth(260);
    if (ImGui::InputText("Name", name, sizeof(name))) { e.name = name; changed = true; }
    changed |= ImGui::Checkbox("Visible", &e.visible);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Anti-alias", &e.antialias);
    ImGui::SeparatorText("Stroke");
    changed |= paint_style_editor(*this, "stroke", e.stroke, stroke_grad, stroke_pat);
    ImGui::SetNextItemWidth(120);
    changed |= ImGui::SliderFloat("Width", &e.stroke_width, 0.0f, 100.0f, "%.1f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    changed |= ImGui::SliderFloat("Miter limit", &e.miter, 1.0f, 20.0f, "%.0f");
    ensure_line_styles();
    {
        const char* current = e.line.dashed() || e.line.first_cap || e.line.last_cap ? (e.line.name.empty() ? "(custom)" : e.line.name.c_str()) : "Solid";
        ImGui::SetNextItemWidth(160);
        if (ImGui::BeginCombo("Line style", current)) {
            if (ImGui::Selectable("Solid", false)) { e.line = vec::LineStyle{}; changed = true; }
            for (size_t i = 0; i < line_library.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(line_library[i].line.name.c_str(), e.line.name == line_library[i].line.name)) { e.line = line_library[i].line; changed = true; }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SeparatorText("Fill");
    changed |= paint_style_editor(*this, "fill", e.fill, fill_grad, fill_pat);
    bool text_changed = false;
    if (e.is_text) {
        ImGui::SeparatorText("Text");
        char buf[2048];
        std::snprintf(buf, sizeof(buf), "%s", e.text.text.c_str());
        if (ImGui::InputTextMultiline("##text", buf, sizeof(buf), ImVec2(300, 60))) { e.text.text = buf; text_changed = true; }
        ensure_fonts();
        ImGui::SetNextItemWidth(300);
        if (ImGui::BeginCombo("Font", e.text.font_family.c_str())) {
            for (size_t i = 0; i < fonts.size(); ++i) {
                const std::string label = fonts[i].family + "  " + fonts[i].style;
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(label.c_str(), fonts[i].path == e.text.font_path)) { e.text.font_path = fonts[i].path; e.text.font_family = label; text_changed = true; }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(140);
        text_changed |= ImGui::SliderFloat("Size", &e.text.size, 4.0f, 500.0f, "%.0f px", ImGuiSliderFlags_Logarithmic);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        text_changed |= ImGui::Combo("Align", &e.text.align, "Left\0Center\0Right\0");
        ImGui::SetNextItemWidth(140);
        text_changed |= ImGui::SliderFloat("Rotation", &e.text.rotation, -180.0f, 180.0f, "%.0f deg");
    }
    if (changed || text_changed) {
        auto& objs = doc->layer(vector_props_layer).objects;
        for (size_t i = 0; i < objs.size() && i < vector_props_before.size(); ++i) {
            if (!vector_props_before[i].selected || vector_props_before[i].is_group) continue;
            vec::Object& o = objs[i];
            o.visible = e.visible; o.antialias = e.antialias;
            o.stroke = e.stroke; o.fill = e.fill; o.stroke_width = e.stroke_width; o.miter = e.miter; o.line = e.line;
            if (static_cast<int>(i) == vector_props_index) o.name = e.name;
            if (text_changed && o.is_text) {
                float bx0, by0, bx1, by1;
                vec::outline_bounds(vector_props_before[i], &bx0, &by0, &bx1, &by1);
                vec::TextInfo t = e.text;
                if (static_cast<int>(i) != vector_props_index) { t = o.text; t.text = e.text.text; t.size = e.text.size; t.align = e.text.align; t.rotation = e.text.rotation; t.font_path = e.text.font_path; t.font_family = e.text.font_family; }
                place_text_object(o, t, 0, 0);
                float nx0, ny0, nx1, ny1;
                if (vec::outline_bounds(o, &nx0, &ny0, &nx1, &ny1)) o.translate(bx0 - nx0, by0 - ny0);
                o.selected = true;
            }
        }
        doc->rasterize_vector_layer(vector_props_layer);
    }
    ImGui::Separator();
    const bool ok = ImGui::Button("OK", ImVec2(80, 0));
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (ok) {
        objects_changed("Vector Properties", vector_props_before);
        ImGui::CloseCurrentPopup();
    } else if (cancel) {
        doc->layer(vector_props_layer).objects = vector_props_before;
        doc->rasterize_vector_layer(vector_props_layer);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
