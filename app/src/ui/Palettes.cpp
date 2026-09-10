#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "App.h"
#include "Icons.h"
#include "MaterialDialog.h"
#include "firn/vector.h"
#include "imgui.h"

using namespace firn;

// The dockable palettes: Layers, History, Materials, Tool Options, Overview.
// Tools palette is a vertical strip like the original's Tools toolbar.
static void draw_tools(App& app) {
    ImGui::Begin("Tools");
    const char* last_cat = nullptr;
    for (size_t i = 0; i < app.tools.size(); ++i) {
        const Tool& t = *app.tools[i];
        if (!last_cat || std::strcmp(last_cat, t.category()) != 0) {
            last_cat = t.category();
            ImGui::SeparatorText(last_cat);
        }
        char label[64], id[16];
        if (t.shortcut()) std::snprintf(label, sizeof(label), "%s (%s)", t.name(), t.shortcut());
        else std::snprintf(label, sizeof(label), "%s", t.name());
        std::snprintf(id, sizeof(id), "##tool%zu", i);
        // Icon at the left, drawn over the selectable's rect; the label follows it.
        const float row = ImGui::GetTextLineHeight() + 4.0f;
        if (ImGui::Selectable(id, app.tool_index == static_cast<int>(i), 0, ImVec2(0, row))) app.select_tool(static_cast<int>(i));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", label);
        const ImVec2 r0 = ImGui::GetItemRectMin();
        const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
        draw_tool_icon(ImGui::GetWindowDrawList(), t.name(), ImVec2(r0.x + 3.0f, r0.y + 2.0f), row - 4.0f, col);
        ImGui::GetWindowDrawList()->AddText(ImVec2(r0.x + row + 6.0f, r0.y + 2.0f), col, label);
    }
    ImGui::End();
}

static void draw_tool_options(App& app) {
    ImGui::Begin("Tool Options");
    ImGui::TextUnformatted(app.tool().name());
    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();
    app.tool().draw_options(app);
    ImGui::End();
}


static void draw_materials(App& app) {
    ImGui::Begin("Materials");
    draw_materials_header(app);
    if (app.material_view != 2) {
        if (!app.recent_colors.empty()) {
            ImGui::SeparatorText("Recent");
            const float sz = 16.0f;
            for (size_t i = 0; i < app.recent_colors.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                const Color& c = app.recent_colors[i];
                const ImVec4 col(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f);
                if (ImGui::ColorButton("##rc", col, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha, ImVec2(sz, sz))) { app.fg_color[0] = col.x; app.fg_color[1] = col.y; app.fg_color[2] = col.z; app.fg_material.kind = 0; }
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { app.bg_color[0] = col.x; app.bg_color[1] = col.y; app.bg_color[2] = col.z; app.bg_material.kind = 0; }
                ImGui::PopID();
                if (i + 1 < app.recent_colors.size() && (i + 1) % 8 != 0) ImGui::SameLine(0, 2);
            }
        }
        ImGui::End();
        return;
    }
    // Swatches: left click sets the foreground, right click the background;
    // + adds the foreground, right click on a swatch with Ctrl removes it.
    app.ensure_swatches();
    ImGui::SeparatorText("Swatches");
    auto swatch_row = [&](std::vector<Color>& list, const char* id, bool removable) {
        const float sz = 16.0f;
        const int per_row = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (sz + 4)));
        for (size_t i = 0; i < list.size(); ++i) {
            ImGui::PushID(id); ImGui::PushID(static_cast<int>(i));
            const Color& c = list[i];
            ImVec4 col(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f);
            if (ImGui::ColorButton("##sw", col, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha, ImVec2(sz, sz))) {
                app.fg_color[0] = col.x; app.fg_color[1] = col.y; app.fg_color[2] = col.z; app.fg_material.kind = 0;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%d, %d, %d", c.r, c.g, c.b);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    if (removable && ImGui::GetIO().KeyCtrl) { list.erase(list.begin() + static_cast<long>(i)); app.save_swatches(); ImGui::PopID(); ImGui::PopID(); break; }
                    app.bg_color[0] = col.x; app.bg_color[1] = col.y; app.bg_color[2] = col.z; app.bg_material.kind = 0;
                }
            }
            ImGui::PopID(); ImGui::PopID();
            if (static_cast<int>(i % per_row) != per_row - 1 && i + 1 < list.size()) ImGui::SameLine();
        }
    };
    swatch_row(app.swatches, "sw", true);
    if (ImGui::SmallButton("+")) { app.swatches.push_back({static_cast<uint8_t>(app.fg_color[0] * 255 + 0.5f), static_cast<uint8_t>(app.fg_color[1] * 255 + 0.5f), static_cast<uint8_t>(app.fg_color[2] * 255 + 0.5f), 255}); app.save_swatches(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add the foreground color (Ctrl+right-click a swatch removes it)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Load...")) app.request_load_swatches();
    ImGui::SameLine();
    if (ImGui::SmallButton("Save...")) app.request_save_swatches();
    if (!app.recent_colors.empty()) {
        ImGui::SeparatorText("Recent");
        swatch_row(app.recent_colors, "rc", false);
    }
    ImGui::Separator();
    if (app.doc && app.active_layer() >= 0) {
        if (ImGui::Button("Fill layer with foreground")) {
            auto c = [&](float f) { return static_cast<uint8_t>(f * 255.0f + 0.5f); };
            app.run(std::make_unique<FillCommand>(
                app.active_layer(),
                Color{c(app.fg_color[0]), c(app.fg_color[1]), c(app.fg_color[2]), c(app.fg_color[3])}));
        }
    }
    ImGui::End();
}

// Blend mode combo over the BlendMode enum; returns true when changed.
bool blend_combo(const char* label, BlendMode& mode) {
    bool changed = false;
    if (ImGui::BeginCombo(label, blend_mode_name(mode))) {
        for (int i = 0; i < static_cast<int>(BlendMode::Count); ++i) {
            const auto m = static_cast<BlendMode>(i);
            if (ImGui::Selectable(blend_mode_name(m), m == mode)) { mode = m; changed = true; }
        }
        ImGui::EndCombo();
    }
    return changed;
}

static void draw_layers(App& app) {
    ImGui::Begin("Layers");
    if (!app.doc) { ImGui::TextDisabled("No image"); ImGui::End(); return; }
    Document& doc = *app.doc;
    const int active = app.active_layer();

    if (ImGui::SmallButton("New")) app.layer_new();
    ImGui::SameLine();
    if (ImGui::SmallButton("Dup")) app.layer_duplicate();
    ImGui::SameLine();
    if (ImGui::SmallButton("Del")) app.layer_delete();
    ImGui::SameLine();
    if (ImGui::SmallButton("Up")) app.layer_arrange(+1);
    ImGui::SameLine();
    if (ImGui::SmallButton("Down")) app.layer_arrange(-1);
    ImGui::SameLine();
    if (ImGui::SmallButton("Merge Down")) app.layer_merge(0);

    // Active layer controls: blend mode and opacity. The slider previews
    // live and commits one Layer Properties entry when released.
    if (active >= 0) {
        Layer& L = doc.layer(active);
        ImGui::SetNextItemWidth(130);
        BlendMode m = L.blend;
        if (blend_combo("##blend", m)) {
            LayerProps after = doc.props(active);
            after.blend = m;
            app.layer_set_props(doc.props(active), after);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        float op = L.opacity * 100.0f;
        if (ImGui::SliderFloat("##opacity", &op, 0.0f, 100.0f, "Opacity %.0f%%")) {
            L.opacity = op / 100.0f;
            doc.touch();
        }
        if (ImGui::IsItemActivated()) app.layer_props_before = doc.props(active);
        if (ImGui::IsItemDeactivatedAfterEdit()) app.layer_set_props(app.layer_props_before, doc.props(active));
    }
    ImGui::Separator();

    // Top of stack first, like every layer palette ever. Members of a
    // collapsed group are skipped (they sit above their group header).
    std::vector<bool> hidden(doc.layer_count(), false);
    for (size_t g = 0; g < doc.layer_count(); ++g)
        if (doc.layer(g).type == LayerType::Group && !doc.layer(g).expanded)
            for (size_t j = g + 1; j < doc.group_end(g); ++j) hidden[j] = true;
    for (int i = static_cast<int>(doc.layer_count()) - 1; i >= 0; --i) {
        Layer& L = doc.layer(i);
        if (hidden[i]) continue;
        ImGui::PushID(i);
        ImGui::Indent(L.depth * 14.0f);
        bool vis = L.visible;
        if (ImGui::Checkbox("##vis", &vis)) {
            LayerProps before = doc.props(i), after = before;
            after.visible = vis;
            doc.set_active_layer(i);
            app.layer_set_props(before, after);
        }
        ImGui::SameLine();
        if (L.type == LayerType::Group) {
            if (ImGui::ArrowButton("##exp", L.expanded ? ImGuiDir_Up : ImGuiDir_Right)) L.expanded = !L.expanded;
            ImGui::SameLine();
        }
        if (L.is_vector()) {
            if (ImGui::ArrowButton("##exp", L.expanded ? ImGuiDir_Down : ImGuiDir_Right)) L.expanded = !L.expanded;
            ImGui::SameLine();
        }
        char label[192];
        std::snprintf(label, sizeof(label), "%s%s%s%s", L.type == LayerType::Group ? "[Group] " : L.is_vector() ? "[Vector] " : L.is_adjustment() ? "[Adjust] " : "", L.name.c_str(),
                      L.blend != BlendMode::Normal ? "  [" : "", L.blend != BlendMode::Normal ? blend_mode_name(L.blend) : "");
        if (L.blend != BlendMode::Normal) std::strncat(label, "]", sizeof(label) - std::strlen(label) - 1);
        // Size the selectable to its label so the controls after it stay clickable.
        if (ImGui::Selectable(label, active == i, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(ImGui::CalcTextSize(label).x + 8.0f, 0))) {
            doc.set_active_layer(i);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (L.is_adjustment()) app.open_adjustment_dialog(i, false);
                else app.open_layer_properties();
            }
        }
        // Right-click: the Layers menu for this layer.
        if (ImGui::BeginPopupContextItem("layer_context")) {
            if (ImGui::IsWindowAppearing()) doc.set_active_layer(i);
            app.draw_layer_menu_items();
            ImGui::EndPopup();
            if (!app.doc || i >= static_cast<int>(app.doc->layer_count())) { ImGui::Unindent(L.depth * 14.0f); ImGui::PopID(); break; }
        }
        if (L.has_mask()) {
            ImGui::SameLine();
            bool on = L.mask_enabled;
            if (ImGui::Checkbox("Mask", &on)) { doc.set_active_layer(i); app.layer_set_mask(on ? "Enable Mask" : "Disable Mask", L.mask, on); }
            ImGui::SameLine();
            const bool editing = app.mask_edit && static_cast<int>(app.mask_proxy_layer) == i;
            if (editing) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            if (ImGui::SmallButton("Edit")) { doc.set_active_layer(i); app.set_mask_edit(!editing); }
            if (editing) ImGui::PopStyleColor();
        }
        if (L.background) { ImGui::SameLine(); ImGui::TextDisabled("(background)"); }
        else if (L.floating) { ImGui::SameLine(); ImGui::TextDisabled("(floating)"); }
        else if (L.opacity < 1.0f) { ImGui::SameLine(); ImGui::TextDisabled("%.0f%%", L.opacity * 100.0f); }
        // Objects of an expanded vector layer, top-most first; clicking
        // selects the object (Shift adds), the checkbox hides it.
        if (L.is_vector() && L.expanded) {
            ImGui::Indent(20.0f);
            std::vector<vec::Object>& objs = L.objects;
            int depth = 0;
            std::vector<size_t> group_ends;
            for (int oi = static_cast<int>(objs.size()) - 1; oi >= 0; --oi) {
                vec::Object& o = objs[oi];
                ImGui::PushID(oi);
                const int g = vec::group_of(objs, oi);
                depth = 0;
                for (int gg = g; gg >= 0; gg = vec::group_of(objs, static_cast<size_t>(gg))) ++depth;
                ImGui::Indent(depth * 12.0f);
                bool ovis = o.visible;
                if (ImGui::Checkbox("##ovis", &ovis)) {
                    std::vector<vec::Object> before = objs;
                    o.visible = ovis;
                    doc.set_active_layer(i);
                    app.objects_changed(ovis ? "Show Object" : "Hide Object", std::move(before));
                }
                ImGui::SameLine();
                char olabel[160];
                std::snprintf(olabel, sizeof(olabel), "%s%s", o.is_group ? "[Group] " : o.is_text ? "[Text] " : "", o.name.c_str());
                if (ImGui::Selectable(olabel, o.selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(ImGui::CalcTextSize(olabel).x + 8.0f, 0))) {
                    doc.set_active_layer(i);
                    app.select_objects({static_cast<size_t>(oi)}, ImGui::GetIO().KeyShift);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) app.open_vector_properties();
                }
                ImGui::Unindent(depth * 12.0f);
                ImGui::PopID();
            }
            if (objs.empty()) ImGui::TextDisabled("(no objects)");
            ImGui::Unindent(20.0f);
        }
        ImGui::Unindent(L.depth * 14.0f);
        ImGui::PopID();
    }
    ImGui::End();
}

static void draw_history(App& app) {
    ImGui::Begin("History");
    if (!app.doc) { ImGui::TextDisabled("No image"); ImGui::End(); return; }
    const CommandStack& h = app.history;
    if (ImGui::Selectable("Original", h.cursor() == 0)) {
        while (app.history.can_undo()) app.undo();
    }
    for (size_t i = 0; i < h.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const bool applied = i < h.cursor();
        if (!applied) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        if (ImGui::Selectable(h.at(i).name().c_str(), i + 1 == h.cursor())) {
            // Click to jump to that point in history.
            while (app.history.cursor() > i + 1) app.undo();
            while (app.history.cursor() < i + 1) app.redo();
        }
        if (!applied) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::End();
}

static void draw_overview(App& app) {
    ImGui::Begin("Overview");
    if (app.doc && app.canvas_tex) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float aspect = static_cast<float>(app.doc->width()) / app.doc->height();
        ImVec2 sz(avail.x, avail.x / aspect);
        if (sz.y > avail.y) sz = ImVec2(avail.y * aspect, avail.y);
        ImGui::Image((ImTextureID)(intptr_t)app.canvas_tex, sz);
        ImGui::Text("%d x %d", app.doc->width(), app.doc->height());
    }
    if (!app.status.empty()) ImGui::TextWrapped("%s", app.status.c_str());
    ImGui::End();
}

void App::draw_palettes() {
    draw_tools(*this);
    draw_tool_options(*this);
    draw_materials(*this);
    draw_layers(*this);
    draw_history(*this);
    draw_overview(*this);
}
