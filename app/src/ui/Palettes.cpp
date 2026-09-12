#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "App.h"
#include "ui/ImGuiMenuBuilder.h"
#include "ui/PaletteState.h"
#include "Icons.h"
#include "MaterialDialog.h"
#include "firn/vector.h"
#include "imgui.h"
#include "imgui_internal.h"

using namespace firn;

// The dockable palettes: Layers, History, Materials, Tool Options, Overview.
// Tools palette is a vertical strip like the original's Tools toolbar.
static void draw_tools(App& app) {
    ImGui::Begin("Tools");
    auto& filter = app.palette_state->tool_filter;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##tool_search", "Search tools...", filter.InputBuf, sizeof(filter.InputBuf))) filter.Build();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Search names, categories, or shortcuts. Use a comma for alternatives.");
    if (filter.IsActive() && ImGui::SmallButton("Clear search")) filter.Clear();
    ImGui::BeginChild("Tool list");
    auto row = [&](size_t i) {
        const Tool& t = *app.tools[i];
        std::string label = t.name();
        if (t.shortcut()) label += std::string(" (") + t.shortcut() + ")";
        const float icon = ImGui::GetTextLineHeight();
        const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x - icon - 10.0f);
        const float height = ImGui::CalcTextSize(label.c_str(), nullptr, false, width).y + 6.0f;
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable("##tool", app.tool_index == static_cast<int>(i), 0, ImVec2(0, height))) app.select_tool(static_cast<int>(i));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s / %s", t.category(), label.c_str());
        const ImVec2 pos = ImGui::GetItemRectMin();
        auto* dl = ImGui::GetWindowDrawList();
        const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
        draw_tool_icon(dl, t.name(), ImVec2(pos.x + 2, pos.y + 3), icon, color);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(pos.x + icon + 8, pos.y + 3), color, label.c_str(), nullptr, width);
        ImGui::PopID();
    };
    if (filter.IsActive()) {
        int matches = 0;
        for (size_t i = 0; i < app.tools.size(); ++i) {
            const auto& t = *app.tools[i];
            const std::string text = std::string(t.name()) + " " + t.category() + " " + (t.shortcut() ? t.shortcut() : "");
            if (filter.PassFilter(text.c_str())) { row(i); ++matches; }
        }
        if (!matches) ImGui::TextWrapped("No matching tools. Try a name such as brush, selection, or shape.");
    } else {
        ImGui::SeparatorText("Common tools");
        const char* common[] = {"Paint Brush", "Eraser", "Dropper", "Flood Fill", "Selection", "Move", "Crop", "Text", "Pan", "Zoom"};
        for (const char* name : common)
            for (size_t i = 0; i < app.tools.size(); ++i)
                if (std::strcmp(name, app.tools[i]->name()) == 0) { row(i); break; }
        ImGui::Spacing();
        ImGui::SeparatorText("All tools");
        // Collect categories once, without relying on tools being contiguous.
        std::vector<std::string> categories;
        for (const auto& t : app.tools)
            if (std::find(categories.begin(), categories.end(), t->category()) == categories.end()) categories.push_back(t->category());
        ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        for (const auto& category : categories) {
            if (ImGui::CollapsingHeader(category.c_str())) {
                ImGui::PushID(category.c_str());
                for (size_t i = 0; i < app.tools.size(); ++i)
                    if (category == app.tools[i]->category()) row(i);
                ImGui::PopID();
            }
        }
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::End();
}

static void draw_tool_options(App& app) {
    ImGui::Begin("Tool Options", nullptr, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(app.tool().name());
    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();
    app.tool().draw_options(app);
    const float content_height = ImGui::GetCursorPosY() + ImGui::GetScrollY() + ImGui::GetStyle().WindowPadding.y + (ImGui::GetCurrentWindow()->ScrollbarX ? ImGui::GetStyle().ScrollbarSize : 0);
    if (ImGui::BeginPopupContextWindow("Options sizing", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::Checkbox("Fit height when options change", &app.config.fit_tool_options)) {
            app.config.save();
            app.palette_state->options_content_height = 0;
        }
        ImGui::EndPopup();
    }
    // Only resize a single options pane above the central canvas. Custom
    // side docks, tab groups and floating palettes retain their dimensions.
    ImGuiDockNode* node = ImGui::GetWindowDockNode();
    if (app.config.fit_tool_options && node && node->Windows.Size == 1 && node->ParentNode &&
        node->ParentNode->SplitAxis == ImGuiAxis_Y && node->ParentNode->ChildNodes[0] == node &&
        node->ParentNode->ChildNodes[1]->HasCentralNodeChild &&
        std::abs(content_height - app.palette_state->options_content_height) > 1.0f) {
        const float height = std::clamp(content_height, ImGui::GetFrameHeight() * 2, std::max(ImGui::GetFrameHeight() * 2, node->ParentNode->Size.y * 0.35f));
        node->SizeRef.y = height;
        app.palette_state->options_content_height = content_height;
    }
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
    bool first_button = true;
    auto layer_button = [&](const char* label) {
        const float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
        if (!first_button && ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - ImGui::GetItemRectMax().x > width + ImGui::GetStyle().ItemSpacing.x) ImGui::SameLine();
        first_button = false;
        return ImGui::SmallButton(label);
    };
    if (layer_button("New")) app.layer_new();
    if (layer_button("Duplicate")) app.layer_duplicate();
    if (layer_button("Delete")) app.layer_delete();
    if (layer_button("Up")) app.layer_arrange(+1);
    if (layer_button("Down")) app.layer_arrange(-1);
    if (layer_button("Merge Down")) app.layer_merge(0);

    // Toolbar actions can change both the stack and the active index.
    const int active = app.active_layer();

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
        if (ImGui::IsItemActivated()) app.palette_state->layer_props_before = doc.props(active);
        if (ImGui::IsItemDeactivatedAfterEdit()) app.layer_set_props(app.palette_state->layer_props_before, doc.props(active));
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
        // A clipped layer shows only where the one below it does; the marker
        // points at the layer it is clipped to.
        if (L.lock_alpha) {
            ImGui::TextDisabled("[T]");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Transparency locked");
            ImGui::SameLine();
        }
        if (L.clipped) {
            ImGui::TextDisabled("|_");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clipped to the layer below");
            ImGui::SameLine();
        }
        char label[192];
        std::snprintf(label, sizeof(label), "%s%s%s%s", L.type == LayerType::Group ? "[Group] " : L.is_vector() ? "[Vector] " : L.is_adjustment() ? (L.adjustment.is_filter() ? "[Filter] " : "[Adjust] ") : L.style.any() ? "[fx] " : "", L.name.c_str(),
                      L.blend != BlendMode::Normal ? "  [" : "", L.blend != BlendMode::Normal ? blend_mode_name(L.blend) : "");
        if (L.blend != BlendMode::Normal) std::strncat(label, "]", sizeof(label) - std::strlen(label) - 1);
        // Double-clicking the name renames it in place; the rest of the
        // layer's settings are behind Properties in the context menu.
        if (app.palette_state->rename_layer == i) {
            ImGui::SetNextItemWidth(std::max(120.0f, ImGui::CalcTextSize(label).x + 20.0f));
            if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
            const bool done = ImGui::InputText("##rename", app.palette_state->rename_buf, sizeof(app.palette_state->rename_buf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if (done || ImGui::IsItemDeactivated()) {
                if (done && app.palette_state->rename_buf[0] && L.name != app.palette_state->rename_buf) {
                    LayerProps before = doc.props(i), after = before;
                    after.name = app.palette_state->rename_buf;
                    doc.set_active_layer(i);
                    app.layer_set_props(before, after);
                }
                app.palette_state->rename_layer = -1;
            }
        } else if (ImGui::Selectable(label, active == i, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(ImGui::CalcTextSize(label).x + 8.0f, 0))) {
            doc.set_active_layer(i);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                app.palette_state->rename_layer = i;
                std::snprintf(app.palette_state->rename_buf, sizeof(app.palette_state->rename_buf), "%s", L.name.c_str());
            }
        }
        // Drag a row onto another to restack it (into and out of groups).
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
            ImGui::SetDragDropPayload("FIRN_LAYER", &i, sizeof(int));
            ImGui::TextUnformatted(L.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FIRN_LAYER")) {
                const int from = *static_cast<const int*>(payload->Data);
                const int row_depth = L.depth;
                app.layer_move_onto(from, i);
                ImGui::EndDragDropTarget();
                ImGui::Unindent(row_depth * 14.0f);
                ImGui::PopID();
                break;   // the stack changed under us
            }
            ImGui::EndDragDropTarget();
        }
        // Right-click: the Layers menu for this layer.
        if (ImGui::BeginPopupContextItem("layer_context")) {
            if (ImGui::IsWindowAppearing()) doc.set_active_layer(i);
            const int row_depth = L.depth;
            const uint64_t revision = doc.revision();
            ImGuiMenuBuilder builder;
            app.draw_layer_menu_items(builder);
            ImGui::EndPopup();
            builder.dispatch();
            // A menu action may replace the stack even when its count stays
            // the same. L and the visibility map then belong to the old stack.
            if (app.doc.get() != &doc || doc.revision() != revision) { ImGui::Unindent(row_depth * 14.0f); ImGui::PopID(); break; }
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
