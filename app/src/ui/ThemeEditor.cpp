// Theme switching, the font atlas rebuild, and the Theme Editor window
// (Preferences > Edit Themes...): colors, shape, text, save / import / export.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "App.h"
#include "firn/text.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "ui/Theme.h"

namespace fs = std::filesystem;

void App::ensure_themes() {
    if (themes_loaded) return;
    themes_loaded = true;
    themes = Theme::builtins();
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(Theme::user_dir(), fs::directory_options::skip_permission_denied, ec)) {
        if (!de.is_regular_file(ec)) continue;
        std::string ext = de.path().extension().string();
        for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ext != std::string(".") + Theme::extension()) continue;
        if (auto t = Theme::load(de.path().string())) themes.push_back(std::move(*t));
    }
}

const Theme* App::find_theme(const std::string& name) const {
    for (const Theme& t : themes) if (t.name == name) return &t;
    return nullptr;
}

// Applies a theme by name: style now, font at the start of the next frame.
void App::apply_theme(const std::string& name) {
    ensure_themes();
    const Theme* t = find_theme(name);
    if (!t) t = &themes.front();
    apply_theme_values(*t);
}

void App::apply_theme_values(const Theme& t) {
    t.apply_style();
    if (t.font_path != font_current_path || std::abs(t.font_size - font_current_size) > 0.01f) {
        font_pending_path = t.font_path;
        font_pending_size = t.font_size;
        font_pending = true;
    }
}

// Default UI face when a theme asks for text at a size but names no font.
std::string App::default_ui_font() {
    ensure_fonts();
    static const char* const prefs[] = {"DejaVu Sans", "Liberation Sans", "Noto Sans", "Arial", "Segoe UI", "Ubuntu", "Cantarell"};
    for (const char* fam : prefs)
        for (const auto& f : fonts) if (f.family == fam && (f.style == "Regular" || f.style == "Book")) return f.path;
    for (const auto& f : fonts) if (f.style == "Regular") return f.path;
    return fonts.empty() ? "" : fonts.front().path;
}

// Runs between frames (before NewFrame): rebuilds the font atlas for a new
// font or size. An empty path means the built-in 13 px bitmap font, scaled.
void App::apply_pending_font() {
    if (!font_pending) return;
    font_pending = false;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    std::string path = font_pending_path;
    if (path.empty()) path = default_ui_font();
    ImFont* loaded = nullptr;
    if (path != "builtin" && !path.empty() && fs::exists(path)) loaded = io.Fonts->AddFontFromFileTTF(path.c_str(), std::max(6.0f, font_pending_size));
    if (!loaded) {
        io.Fonts->AddFontDefault();
        io.FontGlobalScale = std::abs(font_pending_size - 13.0f) < 0.01f ? 1.0f : font_pending_size / 13.0f;
    } else {
        io.FontGlobalScale = 1.0f;
    }
    io.Fonts->Build();
    ImGui_ImplOpenGL3_DestroyFontsTexture();  // NewFrame creates the texture again
    font_current_path = font_pending_path;
    font_current_size = font_pending_size;
}

void App::open_theme_editor() {
    ensure_themes();
    const Theme* t = find_theme(config.theme);
    theme_edit = t ? *t : themes.front();
    theme_edit_from = theme_edit.name;
    std::snprintf(theme_name_buf, sizeof(theme_name_buf), "%s", theme_edit.name.c_str());
    theme_editor_before = theme_edit;
    theme_editor_before.capture_style();
    theme_editor_before.font_path = font_current_path;
    theme_editor_before.font_size = font_current_size;
    show_theme_editor = true;
}

bool App::save_theme(Theme t, const std::string& name, std::string* err) {
    t.name = name;
    t.builtin = false;
    std::string safe = name;
    for (char& ch : safe) if (ch == '/' || ch == '\\' || ch == ':') ch = '_';
    t.path = Theme::user_dir() + "/" + safe + "." + Theme::extension();
    if (!t.save(t.path, err)) return false;
    ensure_themes();
    bool replaced = false;
    for (Theme& existing : themes) if (!existing.builtin && existing.name == name) { existing = t; replaced = true; }
    if (!replaced) themes.push_back(t);
    return true;
}

void App::import_theme(const std::string& path) {
    std::string err;
    auto t = Theme::load(path, &err);
    if (!t) { status = "Import theme: " + err; return; }
    if (save_theme(*t, t->name, &err)) {
        config.theme = t->name;
        config.save();
        apply_theme(t->name);
        status = "Imported theme " + t->name;
    } else status = "Import theme: " + err;
}

void App::export_theme(const std::string& path) {
    std::string err;
    Theme t = show_theme_editor ? theme_edit : (find_theme(config.theme) ? *find_theme(config.theme) : themes.front());
    if (t.save(path, &err)) status = "Exported theme to " + path;
    else status = "Export theme: " + err;
}

void App::draw_theme_editor() {
    if (show_theme_editor && !ImGui::IsPopupOpen("Theme Editor")) ImGui::OpenPopup("Theme Editor");
    if (!show_theme_editor) return;
    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Theme Editor", nullptr, ImGuiWindowFlags_NoScrollbar)) return;
    Theme& t = theme_edit;
    bool changed = false;

    // Top row: which theme is being edited, and its name.
    ensure_themes();
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo("Theme", theme_edit_from.c_str())) {
        for (const Theme& th : themes) {
            if (ImGui::Selectable((th.name + (th.builtin ? "  (built-in)" : "")).c_str(), th.name == theme_edit_from)) {
                theme_edit = th;
                theme_edit_from = th.name;
                std::snprintf(theme_name_buf, sizeof(theme_name_buf), "%s", th.name.c_str());
                apply_theme_values(theme_edit);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("Name", theme_name_buf, sizeof(theme_name_buf));
    ImGui::SameLine();
    const Theme* source = find_theme(theme_edit_from);
    if (ImGui::Button("Save") && theme_name_buf[0]) {
        std::string err;
        const std::string name = theme_name_buf;
        if (source && source->builtin && name == source->name) status = "Built-in themes cannot be overwritten: give it a new name.";
        else if (save_theme(t, name, &err)) { theme_edit_from = name; config.theme = name; config.save(); status = "Saved theme " + name; }
        else status = err;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save to %s", Theme::user_dir().c_str());
    ImGui::SameLine();
    if (ImGui::Button("Delete") && source && !source->builtin) {
        std::error_code ec;
        fs::remove(source->path, ec);
        const std::string gone = source->name;
        themes.erase(std::remove_if(themes.begin(), themes.end(), [&](const Theme& th) { return !th.builtin && th.name == gone; }), themes.end());
        theme_edit = themes.front(); theme_edit_from = theme_edit.name;
        std::snprintf(theme_name_buf, sizeof(theme_name_buf), "%s", theme_edit.name.c_str());
        config.theme = theme_edit.name; config.save();
        apply_theme_values(theme_edit);
    }
    ImGui::SameLine();
    if (ImGui::Button("Import...")) { file_op = PendingFileOp::ImportTheme; file_dialog.open(FileDialog::Mode::Open, "Import Theme", {Theme::extension()}, config.last_directory); }
    ImGui::SameLine();
    if (ImGui::Button("Export...")) { file_op = PendingFileOp::ExportTheme; file_dialog.open(FileDialog::Mode::Save, "Export Theme", {Theme::extension()}, std::string(theme_name_buf) + "." + Theme::extension()); }
    ImGui::SameLine();
    if (ImGui::Button("Revert") && source) { theme_edit = *source; apply_theme_values(theme_edit); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back to the theme as stored");

    ImGui::Separator();
    if (ImGui::BeginTabBar("##themetabs")) {
        if (ImGui::BeginTabItem("Text")) {
            ensure_fonts();
            const char* current = t.font_path == "builtin" ? "Built-in (bitmap, 13 px)" : t.font_path.empty() ? "Default sans (system)" : nullptr;
            std::string label;
            if (!current) {
                for (const auto& f : fonts) if (f.path == t.font_path) label = f.family + "  " + f.style;
                if (label.empty()) label = t.font_path;
                current = label.c_str();
            }
            ImGui::SetNextItemWidth(320);
            if (ImGui::BeginCombo("Font", current)) {
                if (ImGui::Selectable("Built-in (bitmap, 13 px)", t.font_path == "builtin")) { t.font_path = "builtin"; t.font_size = 13.0f; changed = true; }
                if (ImGui::Selectable("Default sans (system)", t.font_path.empty())) { t.font_path.clear(); changed = true; }
                ImGui::Separator();
                for (size_t i = 0; i < fonts.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    const std::string l = fonts[i].family + "  " + fonts[i].style;
                    if (ImGui::Selectable(l.c_str(), fonts[i].path == t.font_path)) { t.font_path = fonts[i].path; changed = true; }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(320);
            if (ImGui::SliderFloat("Text size", &t.font_size, 8.0f, 32.0f, "%.0f px")) {
                changed = true;
                if (t.font_path == "builtin" && std::abs(t.font_size - 13.0f) > 0.01f) t.font_path.clear();  // sizes need a scalable face
            }
            ImGui::TextDisabled("The built-in bitmap font only comes in 13 px; any other size switches to a TrueType face.");
            ImGui::TextDisabled("The font rebuilds when you release the slider.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Shape")) {
            ImGui::SetNextItemWidth(220);
            changed |= ImGui::SliderFloat("Window rounding", &t.window_rounding, 0.0f, 12.0f, "%.0f");
            ImGui::SetNextItemWidth(220);
            changed |= ImGui::SliderFloat("Frame rounding", &t.frame_rounding, 0.0f, 12.0f, "%.0f");
            ImGui::SetNextItemWidth(220);
            changed |= ImGui::SliderFloat("Tab rounding", &t.tab_rounding, 0.0f, 12.0f, "%.0f");
            ImGui::SetNextItemWidth(220);
            changed |= ImGui::SliderFloat("Slider grab rounding", &t.grab_rounding, 0.0f, 12.0f, "%.0f");
            ImGui::SetNextItemWidth(220);
            changed |= ImGui::SliderFloat("Popup rounding", &t.popup_rounding, 0.0f, 12.0f, "%.0f");
            ImGui::SetNextItemWidth(220);
            changed |= ImGui::SliderFloat("Window border", &t.window_border, 0.0f, 2.0f, "%.0f");
            ImGui::SetNextItemWidth(220);
            changed |= ImGui::SliderFloat("Frame border", &t.frame_border, 0.0f, 2.0f, "%.0f");
            ImGui::SetNextItemWidth(220);
            changed |= ImGui::SliderFloat("Scrollbar size", &t.scrollbar_size, 6.0f, 24.0f, "%.0f");
            ImGui::SetNextItemWidth(220);
            changed |= ImGui::SliderFloat2("Window padding", &t.window_padding.x, 0.0f, 20.0f, "%.0f");
            ImGui::SetNextItemWidth(220);
            changed |= ImGui::SliderFloat2("Frame padding", &t.frame_padding.x, 0.0f, 12.0f, "%.0f");
            ImGui::SetNextItemWidth(220);
            changed |= ImGui::SliderFloat2("Item spacing", &t.item_spacing.x, 0.0f, 16.0f, "%.0f");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Colors")) {
            static char filter[64] = {};
            ImGui::SetNextItemWidth(200);
            ImGui::InputTextWithHint("##filter", "Filter", filter, sizeof(filter));
            ImGui::SameLine();
            if (ImGui::SmallButton("Lighten all")) { for (auto& c : t.colors) { c.x = std::min(1.0f, c.x * 1.1f + 0.02f); c.y = std::min(1.0f, c.y * 1.1f + 0.02f); c.z = std::min(1.0f, c.z * 1.1f + 0.02f); } changed = true; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Darken all")) { for (auto& c : t.colors) { c.x *= 0.9f; c.y *= 0.9f; c.z *= 0.9f; } changed = true; }
            ImGui::SameLine();
            ImGui::TextDisabled("Text, backgrounds, frames, buttons, headers, tabs, scrollbars, tables...");
            const float footer = ImGui::GetFrameHeightWithSpacing() + 8;
            if (ImGui::BeginChild("colors", ImVec2(0, -footer), ImGuiChildFlags_Borders)) {
                std::string want = filter;
                for (char& ch : want) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                for (int i = 0; i < ImGuiCol_COUNT; ++i) {
                    std::string name = ImGui::GetStyleColorName(i), low = name;
                    for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    if (!want.empty() && low.find(want) == std::string::npos) continue;
                    ImGui::PushID(i);
                    ImGui::SetNextItemWidth(300);
                    if (ImGui::ColorEdit4("##c", &t.colors[i].x, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) changed = true;
                    ImGui::SameLine();
                    ImGui::TextUnformatted(name.c_str());
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (changed) {
        // Colors and shape preview immediately; the font waits for the slider release.
        t.apply_style();
        if (!ImGui::IsAnyItemActive() && (t.font_path != font_current_path || std::abs(t.font_size - font_current_size) > 0.01f)) apply_theme_values(t);
    } else if (!ImGui::IsAnyItemActive() && (t.font_path != font_current_path || std::abs(t.font_size - font_current_size) > 0.01f)) {
        apply_theme_values(t);
    }

    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(110, 0))) {
        // Keep what is on screen; an unsaved edit stays until the next theme switch.
        show_theme_editor = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        apply_theme_values(theme_editor_before);
        show_theme_editor = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Save writes the theme to your themes folder and makes it the current one.");
    ImGui::EndPopup();
}
