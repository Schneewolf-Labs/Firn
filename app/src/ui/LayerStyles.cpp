// Layers > Layer Styles: drop shadow, outer and inner glow, stroke and
// bevel on the active raster or vector layer, edited live and committed as
// one SetLayerStyleCommand on OK.
#include <cstdio>

#include "App.h"
#include "firn/commands.h"
#include "imgui.h"

using namespace firn;

namespace {

bool color_edit(const char* label, Color& c) {
    float f[3] = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f};
    ImGui::SetNextItemWidth(160);
    if (!ImGui::ColorEdit3(label, f, ImGuiColorEditFlags_NoInputs)) return false;
    c.r = static_cast<uint8_t>(f[0] * 255.0f + 0.5f); c.g = static_cast<uint8_t>(f[1] * 255.0f + 0.5f); c.b = static_cast<uint8_t>(f[2] * 255.0f + 0.5f);
    return true;
}

bool percent(const char* label, float& v) {
    float p = v * 100.0f;
    ImGui::SetNextItemWidth(160);
    if (!ImGui::SliderFloat(label, &p, 0.0f, 100.0f, "%.0f%%")) return false;
    v = p / 100.0f;
    return true;
}

bool slider(const char* label, float& v, float lo, float hi, const char* fmt = "%.1f") {
    ImGui::SetNextItemWidth(160);
    return ImGui::SliderFloat(label, &v, lo, hi, fmt);
}

}  // namespace

void App::open_layer_styles(int layer) {
    if (!doc || layer < 0 || layer >= static_cast<int>(doc->layer_count())) return;
    const Layer& L = doc->layer(layer);
    if (!L.is_raster() && !L.is_vector()) { status = "Layer styles apply to raster and vector layers."; return; }
    style_layer_index = layer;
    style_before = L.style;
    show_layer_styles_dialog = true;
}

void App::draw_layer_styles_dialog() {
    if (show_layer_styles_dialog) { ImGui::OpenPopup("Layer Styles"); show_layer_styles_dialog = false; }
    if (!ImGui::BeginPopupModal("Layer Styles", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (!doc || style_layer_index < 0 || style_layer_index >= static_cast<int>(doc->layer_count())) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    Layer& L = doc->layer(style_layer_index);
    LayerStyle& s = L.style;
    bool changed = false;
    ImGui::Text("%s", L.name.c_str());
    ImGui::Separator();
    changed |= ImGui::Checkbox("Drop Shadow", &s.drop_shadow);
    if (s.drop_shadow) {
        ImGui::Indent();
        changed |= color_edit("Color##shadow", s.shadow_color);
        changed |= percent("Opacity##shadow", s.shadow_opacity);
        changed |= slider("Offset X##shadow", s.shadow_offset_x, -100.0f, 100.0f, "%.0f");
        changed |= slider("Offset Y##shadow", s.shadow_offset_y, -100.0f, 100.0f, "%.0f");
        changed |= slider("Blur##shadow", s.shadow_blur, 0.0f, 50.0f);
        ImGui::Unindent();
    }
    changed |= ImGui::Checkbox("Outer Glow", &s.outer_glow);
    if (s.outer_glow) {
        ImGui::Indent();
        changed |= color_edit("Color##glow", s.glow_color);
        changed |= percent("Opacity##glow", s.glow_opacity);
        changed |= slider("Size##glow", s.glow_size, 0.5f, 50.0f);
        ImGui::Unindent();
    }
    changed |= ImGui::Checkbox("Inner Glow", &s.inner_glow);
    if (s.inner_glow) {
        ImGui::Indent();
        changed |= color_edit("Color##iglow", s.inner_glow_color);
        changed |= percent("Opacity##iglow", s.inner_glow_opacity);
        changed |= slider("Size##iglow", s.inner_glow_size, 0.5f, 50.0f);
        ImGui::Unindent();
    }
    changed |= ImGui::Checkbox("Stroke", &s.stroke);
    if (s.stroke) {
        ImGui::Indent();
        changed |= color_edit("Color##stroke", s.stroke_color);
        changed |= percent("Opacity##stroke", s.stroke_opacity);
        ImGui::SetNextItemWidth(160);
        changed |= ImGui::SliderInt("Width##stroke", &s.stroke_width, 1, 30);
        ImGui::Unindent();
    }
    changed |= ImGui::Checkbox("Bevel", &s.bevel);
    if (s.bevel) {
        ImGui::Indent();
        changed |= slider("Size##bevel", s.bevel_size, 0.5f, 30.0f);
        changed |= slider("Depth##bevel", s.bevel_depth, 0.1f, 3.0f);
        changed |= slider("Angle##bevel", s.bevel_angle, 0.0f, 360.0f, "%.0f");
        ImGui::Unindent();
    }
    if (changed) doc->touch();
    ImGui::Separator();
    const bool ok = ImGui::Button("OK", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    ImGui::SameLine();
    if (ImGui::Button("Clear All")) { s = LayerStyle(); doc->touch(); }
    if (ok) {
        const LayerStyle after = s;
        if (after != style_before) {
            s = style_before;
            run(std::make_unique<SetLayerStyleCommand>(style_layer_index, style_before, after));
        }
        ImGui::CloseCurrentPopup();
    } else if (cancel) {
        s = style_before;
        doc->touch();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
