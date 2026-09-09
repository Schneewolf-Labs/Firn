#include <cstdio>
#include <cstring>
#include <memory>

#include "App.h"
#include "imgui.h"

using namespace firn;

// The dockable palettes: Layers, History, Materials, Tool Options, Overview.
// Tools palette is a vertical strip like the original's Tools toolbar.
static void draw_tools(App& app) {
    ImGui::Begin("Tools");
    for (size_t i = 0; i < app.tools.size(); ++i) {
        const Tool& t = *app.tools[i];
        char label[64];
        if (t.shortcut()) std::snprintf(label, sizeof(label), "%s (%s)", t.name(), t.shortcut());
        else std::snprintf(label, sizeof(label), "%s", t.name());
        if (ImGui::Selectable(label, app.tool_index == static_cast<int>(i))) app.select_tool(static_cast<int>(i));
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
    ImGui::ColorEdit4("Foreground", app.fg_color, ImGuiColorEditFlags_NoInputs);
    ImGui::ColorEdit4("Background", app.bg_color, ImGuiColorEditFlags_NoInputs);
    if (ImGui::Button("Swap")) {
        for (int i = 0; i < 4; ++i) std::swap(app.fg_color[i], app.bg_color[i]);
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

    // Top of stack first, like every layer palette ever.
    for (int i = static_cast<int>(doc.layer_count()) - 1; i >= 0; --i) {
        Layer& L = doc.layer(i);
        ImGui::PushID(i);
        bool vis = L.visible;
        if (ImGui::Checkbox("##vis", &vis)) {
            LayerProps before = doc.props(i), after = before;
            after.visible = vis;
            doc.set_active_layer(i);
            app.layer_set_props(before, after);
        }
        ImGui::SameLine();
        char label[160];
        std::snprintf(label, sizeof(label), "%s%s%s", L.name.c_str(),
                      L.blend != BlendMode::Normal ? "  [" : "", L.blend != BlendMode::Normal ? blend_mode_name(L.blend) : "");
        if (L.blend != BlendMode::Normal) std::strcat(label, "]");
        if (ImGui::Selectable(label, active == i, ImGuiSelectableFlags_AllowDoubleClick)) {
            doc.set_active_layer(i);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) app.open_layer_properties();
        }
        if (L.background) { ImGui::SameLine(); ImGui::TextDisabled("(background)"); }
        else if (L.opacity < 1.0f) { ImGui::SameLine(); ImGui::TextDisabled("%.0f%%", L.opacity * 100.0f); }
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
