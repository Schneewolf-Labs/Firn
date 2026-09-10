// Help > About: version, commit, build facts, and the libraries in use.
#include <cstdio>
#include <string>

#include <SDL.h>
#include <SDL_opengl.h>

#include "App.h"
#include "Config.h"
#include "Tablet.h"
#include "Version.h"
#include "firn/io.h"
#include "imgui.h"

extern const unsigned char kFirnIconPng[];
extern const size_t kFirnIconPng_size;

namespace {

const char* compiler() {
#if defined(__clang__)
    return "clang " __clang_version__;
#elif defined(__GNUC__)
    return "gcc " __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC";
#else
    return "unknown compiler";
#endif
}

const char* platform() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "unknown platform";
#endif
}

}  // namespace

void App::draw_about_dialog() {
    if (show_about_dialog) {
        show_about_dialog = false;
        ImGui::OpenPopup("About Firn");
        if (!about_tex) {
            if (auto icon = firn::io::load_memory(kFirnIconPng, kFirnIconPng_size)) {
                glGenTextures(1, &about_tex);
                glBindTexture(GL_TEXTURE_2D, about_tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, icon->width(), icon->height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, icon->data());
            }
        }
        if (about_gl.empty()) {
            const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
            const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
            about_gl = std::string(renderer ? renderer : "?") + "\nOpenGL " + (version ? version : "?");
        }
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("About Firn", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (about_tex) { ImGui::Image((ImTextureID)(intptr_t)about_tex, ImVec2(96, 96)); ImGui::SameLine(); }
    ImGui::BeginGroup();
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted("Firn");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Text("Version %s", kFirnVersion);
    ImGui::Text("Commit %s%s%s", kFirnCommit, kFirnBranch[0] ? " on " : "", kFirnBranch);
    ImGui::Text("Built %s with %s", kFirnBuildDate, compiler());
    ImGui::TextDisabled("A Schneewolf Labs project, Apache License 2.0");
    ImGui::EndGroup();
    ImGui::Separator();
    SDL_version linked;
    SDL_GetVersion(&linked);
    ImGui::Text("Dear ImGui %s (docking), SDL %d.%d.%d, %s", IMGUI_VERSION, linked.major, linked.minor, linked.patch, platform());
    ImGui::TextUnformatted(about_gl.c_str());
    ImGui::Text("Settings: %s", Config::directory().c_str());
    ImGui::Text("Pen input: %s%s", tablet::backend(), pen.present ? " (pen seen)" : "");
    if (doc) ImGui::Text("Open images: %zu", docs.size());
    ImGui::Separator();
    ImGui::TextUnformatted("github.com/Schneewolf-Labs/Firn");
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText("https://github.com/Schneewolf-Labs/Firn");
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy build info")) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "Firn %s, commit %s, built %s with %s, ImGui %s, SDL %d.%d.%d, %s\n%s", kFirnVersion, kFirnCommit, kFirnBuildDate, compiler(), IMGUI_VERSION, linked.major, linked.minor, linked.patch, platform(), about_gl.c_str());
        ImGui::SetClipboardText(buf);
    }
    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Help > Keyboard Shortcuts: the bindings in one place.
void App::draw_shortcuts_dialog() {
    if (show_shortcuts_dialog) { ImGui::OpenPopup("Keyboard Shortcuts"); show_shortcuts_dialog = false; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(980, 620), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Keyboard Shortcuts", nullptr, ImGuiWindowFlags_NoScrollbar)) return;
    struct Row { const char* keys; const char* what; };
    static const Row file[] = {{"Ctrl+N", "New image"}, {"Ctrl+O", "Open"}, {"Ctrl+S", "Save"}, {"Ctrl+Shift+S", "Save As"}, {"Ctrl+P", "Print"}, {"Ctrl+W", "Close image"}};
    static const Row edit[] = {{"Ctrl+Z", "Undo"}, {"Ctrl+Y or Ctrl+Shift+Z", "Redo"}, {"Ctrl+C", "Copy"}, {"Ctrl+X", "Cut"}, {"Ctrl+V", "Paste as new image"}, {"Ctrl+L", "Paste as new layer"},
                                  {"Ctrl+Shift+C", "Copy merged"}, {"Ctrl+Shift+L", "Paste into the selection"},
                                  {"Ctrl+Shift+Y", "Repeat the last effect"}, {"Delete", "Clear the selection"}};
    static const Row view[] = {{"+ / -", "Zoom in / out"}, {"Mouse wheel", "Zoom about the cursor"}, {"Ctrl+0", "Fit to window"}, {"Ctrl+Alt+0", "Actual size"}, {"Space + drag, middle drag", "Pan"}, {"Shift+I", "Image information"}, {"Ctrl+Shift+M", "Hide / show the marquee"}};
    static const Row sel[] = {{"Ctrl+A", "Select all"}, {"Ctrl+D", "Select none"}, {"Ctrl+Shift+I", "Invert selection"}, {"Shift / Ctrl while selecting", "Add to / remove from the selection"}, {"Ctrl+Shift+R", "Crop to selection"}, {"Ctrl+F", "Float"}, {"Ctrl+Shift+F", "Defloat"}, {"Enter, double-click", "Close a point-to-point selection"}, {"Backspace", "Remove the last point"}, {"Escape", "Cancel the gesture"}};
    static const Row img[] = {{"Ctrl+I", "Negative image"}, {"[ / ]", "Brush size down / up"}};
    auto table = [&](const char* title, const Row* rows, size_t n) {
        ImGui::SeparatorText(title);
        if (ImGui::BeginTable(title, 2, ImGuiTableFlags_SizingFixedFit)) {
            for (size_t i = 0; i < n; ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(rows[i].keys);
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", rows[i].what);
            }
            ImGui::EndTable();
        }
    };
    const float footer = ImGui::GetFrameHeightWithSpacing() + 8;
    if (ImGui::BeginChild("rows", ImVec2(0, -footer))) {
        ImGui::Columns(2, nullptr, false);
        table("File", file, sizeof(file) / sizeof(file[0]));
        table("Edit", edit, sizeof(edit) / sizeof(edit[0]));
        table("View", view, sizeof(view) / sizeof(view[0]));
        ImGui::NextColumn();
        table("Selections", sel, sizeof(sel) / sizeof(sel[0]));
        table("Image and brushes", img, sizeof(img) / sizeof(img[0]));
        ImGui::SeparatorText("Tools");
        if (ImGui::BeginTable("tools", 2, ImGuiTableFlags_SizingFixedFit)) {
            for (const auto& t : tools) {
                if (!t->shortcut()) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(t->shortcut());
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", t->name());
            }
            ImGui::EndTable();
        }
        ImGui::Columns(1);
    }
    ImGui::EndChild();
    if (ImGui::Button("Close", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
