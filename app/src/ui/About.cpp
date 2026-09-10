// Help > About: version, commit, build facts, and the libraries in use.
#include <cstdio>
#include <string>

#include <SDL.h>
#include <SDL_opengl.h>

#include "App.h"
#include "Config.h"
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
    ImGui::TextDisabled("A Schneewolf Labs project");
    ImGui::EndGroup();
    ImGui::Separator();
    SDL_version linked;
    SDL_GetVersion(&linked);
    ImGui::Text("Dear ImGui %s (docking), SDL %d.%d.%d, %s", IMGUI_VERSION, linked.major, linked.minor, linked.patch, platform());
    ImGui::TextUnformatted(about_gl.c_str());
    ImGui::Text("Settings: %s", Config::directory().c_str());
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

// Color themes. "Firn" is the dark theme tinted toward the icon's violet.
void App::apply_theme(const std::string& name) {
    ImGuiStyle& st = ImGui::GetStyle();
    if (name == "light") { ImGui::StyleColorsLight(); return; }
    if (name == "classic") { ImGui::StyleColorsClassic(); return; }
    ImGui::StyleColorsDark();
    if (name != "firn") return;
    ImVec4* c = st.Colors;
    const ImVec4 violet(0.42f, 0.30f, 0.86f, 1.0f), violet_dim(0.30f, 0.22f, 0.60f, 1.0f), violet_bright(0.55f, 0.42f, 0.98f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.09f, 0.13f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.09f, 0.13f, 0.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.08f, 0.12f, 0.96f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.13f, 0.11f, 0.18f, 1.0f);
    c[ImGuiCol_TitleBg] = ImVec4(0.11f, 0.09f, 0.16f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.24f, 0.18f, 0.42f, 1.0f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.11f, 0.09f, 0.16f, 0.8f);
    c[ImGuiCol_FrameBg] = ImVec4(0.19f, 0.16f, 0.27f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.27f, 0.22f, 0.40f, 1.0f);
    c[ImGuiCol_FrameBgActive] = violet_dim;
    c[ImGuiCol_Header] = violet_dim;
    c[ImGuiCol_HeaderHovered] = violet;
    c[ImGuiCol_HeaderActive] = violet_bright;
    c[ImGuiCol_Button] = ImVec4(0.26f, 0.20f, 0.44f, 1.0f);
    c[ImGuiCol_ButtonHovered] = violet;
    c[ImGuiCol_ButtonActive] = violet_bright;
    c[ImGuiCol_CheckMark] = violet_bright;
    c[ImGuiCol_SliderGrab] = violet;
    c[ImGuiCol_SliderGrabActive] = violet_bright;
    c[ImGuiCol_Tab] = ImVec4(0.18f, 0.15f, 0.27f, 1.0f);
    c[ImGuiCol_TabHovered] = violet;
    c[ImGuiCol_TabSelected] = violet_dim;
    c[ImGuiCol_TabDimmed] = ImVec4(0.12f, 0.11f, 0.17f, 1.0f);
    c[ImGuiCol_TabDimmedSelected] = ImVec4(0.22f, 0.17f, 0.36f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.26f, 0.42f, 1.0f);
    c[ImGuiCol_SeparatorHovered] = violet;
    c[ImGuiCol_SeparatorActive] = violet_bright;
    c[ImGuiCol_ResizeGrip] = ImVec4(0.42f, 0.30f, 0.86f, 0.3f);
    c[ImGuiCol_ResizeGripHovered] = violet;
    c[ImGuiCol_ResizeGripActive] = violet_bright;
    c[ImGuiCol_DockingPreview] = ImVec4(0.55f, 0.42f, 0.98f, 0.6f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.25f, 0.45f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = violet;
    c[ImGuiCol_ScrollbarGrabActive] = violet_bright;
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.42f, 0.30f, 0.86f, 0.4f);
    c[ImGuiCol_NavCursor] = violet_bright;
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.18f, 0.15f, 0.27f, 1.0f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.30f, 0.26f, 0.42f, 1.0f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.22f, 0.19f, 0.32f, 1.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);
    st.FrameRounding = 3.0f;
    st.TabRounding = 3.0f;
    st.GrabRounding = 3.0f;
}
