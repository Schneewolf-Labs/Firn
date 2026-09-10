#include "Theme.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "Config.h"

namespace {

std::string color_key(int i) {
    std::string k = "color.";
    k += ImGui::GetStyleColorName(i);
    return k;
}

int color_index(const std::string& key) {
    if (key.rfind("color.", 0) != 0) return -1;
    const std::string name = key.substr(6);
    for (int i = 0; i < ImGuiCol_COUNT; ++i) if (name == ImGui::GetStyleColorName(i)) return i;
    return -1;
}

ImVec4 parse_color(const std::string& v, ImVec4 def) {
    float r, g, b, a = 1.0f;
    const int n = std::sscanf(v.c_str(), "%f,%f,%f,%f", &r, &g, &b, &a);
    if (n < 3) return def;
    return ImVec4(r, g, b, a);
}

ImVec2 parse_vec2(const std::string& v, ImVec2 def) {
    float x, y;
    if (std::sscanf(v.c_str(), "%f,%f", &x, &y) != 2) return def;
    return ImVec2(x, y);
}

Theme from_imgui(const char* name, void (*fn)(ImGuiStyle*)) {
    ImGuiStyle st;
    fn(&st);
    Theme t;
    t.name = name;
    t.builtin = true;
    t.font_path = "builtin";
    for (int i = 0; i < ImGuiCol_COUNT; ++i) t.colors[i] = st.Colors[i];
    t.window_rounding = st.WindowRounding; t.frame_rounding = st.FrameRounding; t.tab_rounding = st.TabRounding; t.grab_rounding = st.GrabRounding; t.popup_rounding = st.PopupRounding;
    t.window_border = st.WindowBorderSize; t.frame_border = st.FrameBorderSize; t.scrollbar_size = st.ScrollbarSize;
    t.window_padding = st.WindowPadding; t.frame_padding = st.FramePadding; t.item_spacing = st.ItemSpacing;
    return t;
}

}  // namespace

void Theme::apply_style() const {
    ImGuiStyle& st = ImGui::GetStyle();
    for (int i = 0; i < ImGuiCol_COUNT; ++i) st.Colors[i] = colors[i];
    st.WindowRounding = window_rounding; st.FrameRounding = frame_rounding; st.TabRounding = tab_rounding; st.GrabRounding = grab_rounding; st.PopupRounding = popup_rounding;
    st.WindowBorderSize = window_border; st.FrameBorderSize = frame_border; st.ScrollbarSize = scrollbar_size;
    st.WindowPadding = window_padding; st.FramePadding = frame_padding; st.ItemSpacing = item_spacing;
}

void Theme::capture_style() {
    const ImGuiStyle& st = ImGui::GetStyle();
    for (int i = 0; i < ImGuiCol_COUNT; ++i) colors[i] = st.Colors[i];
    window_rounding = st.WindowRounding; frame_rounding = st.FrameRounding; tab_rounding = st.TabRounding; grab_rounding = st.GrabRounding; popup_rounding = st.PopupRounding;
    window_border = st.WindowBorderSize; frame_border = st.FrameBorderSize; scrollbar_size = st.ScrollbarSize;
    window_padding = st.WindowPadding; frame_padding = st.FramePadding; item_spacing = st.ItemSpacing;
}

bool Theme::save(const std::string& file, std::string* err) const {
    std::ofstream f(file);
    if (!f) { if (err) *err = "cannot write " + file; return false; }
    f << "# Firn theme\n";
    f << "name=" << name << "\n";
    f << "font_path=" << font_path << "\n";
    f << "font_size=" << font_size << "\n";
    f << "window_rounding=" << window_rounding << "\nframe_rounding=" << frame_rounding << "\ntab_rounding=" << tab_rounding << "\ngrab_rounding=" << grab_rounding << "\npopup_rounding=" << popup_rounding << "\n";
    f << "window_border=" << window_border << "\nframe_border=" << frame_border << "\nscrollbar_size=" << scrollbar_size << "\n";
    f << "window_padding=" << window_padding.x << "," << window_padding.y << "\nframe_padding=" << frame_padding.x << "," << frame_padding.y << "\nitem_spacing=" << item_spacing.x << "," << item_spacing.y << "\n";
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%.4f,%.4f,%.4f,%.4f", colors[i].x, colors[i].y, colors[i].z, colors[i].w);
        f << color_key(i) << "=" << buf << "\n";
    }
    return static_cast<bool>(f);
}

std::optional<Theme> Theme::load(const std::string& file, std::string* err) {
    std::ifstream f(file);
    if (!f) { if (err) *err = "cannot open " + file; return std::nullopt; }
    Theme t = builtins().front();  // unknown keys keep the default theme's values
    t.builtin = false;
    t.path = file;
    t.name = std::filesystem::path(file).stem().string();
    std::string line;
    bool any = false;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        any = true;
        if (key == "name") { if (!value.empty()) t.name = value; }
        else if (key == "font_path") t.font_path = value;
        else if (key == "font_size") t.font_size = std::max(6.0f, static_cast<float>(std::atof(value.c_str())));
        else if (key == "window_rounding") t.window_rounding = static_cast<float>(std::atof(value.c_str()));
        else if (key == "frame_rounding") t.frame_rounding = static_cast<float>(std::atof(value.c_str()));
        else if (key == "tab_rounding") t.tab_rounding = static_cast<float>(std::atof(value.c_str()));
        else if (key == "grab_rounding") t.grab_rounding = static_cast<float>(std::atof(value.c_str()));
        else if (key == "popup_rounding") t.popup_rounding = static_cast<float>(std::atof(value.c_str()));
        else if (key == "window_border") t.window_border = static_cast<float>(std::atof(value.c_str()));
        else if (key == "frame_border") t.frame_border = static_cast<float>(std::atof(value.c_str()));
        else if (key == "scrollbar_size") t.scrollbar_size = static_cast<float>(std::atof(value.c_str()));
        else if (key == "window_padding") t.window_padding = parse_vec2(value, t.window_padding);
        else if (key == "frame_padding") t.frame_padding = parse_vec2(value, t.frame_padding);
        else if (key == "item_spacing") t.item_spacing = parse_vec2(value, t.item_spacing);
        else { const int ci = color_index(key); if (ci >= 0) t.colors[ci] = parse_color(value, t.colors[ci]); }
    }
    if (!any) { if (err) *err = "not a theme file"; return std::nullopt; }
    return t;
}

std::string Theme::user_dir() {
    const std::string dir = Config::directory() + "/themes";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::vector<Theme> Theme::builtins() {
    std::vector<Theme> out;
    // Firn: the dark theme tinted toward the icon's violet.
    Theme firn = from_imgui("Firn", ImGui::StyleColorsDark);
    ImVec4* c = firn.colors;
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
    firn.frame_rounding = 3.0f; firn.tab_rounding = 3.0f; firn.grab_rounding = 3.0f;
    out.push_back(firn);
    out.push_back(from_imgui("Dark", ImGui::StyleColorsDark));
    out.push_back(from_imgui("Light", ImGui::StyleColorsLight));
    out.push_back(from_imgui("Classic", ImGui::StyleColorsClassic));
    // Slate: the neutral gray of the original's workspace.
    Theme slate = from_imgui("Slate", ImGui::StyleColorsDark);
    ImVec4* s = slate.colors;
    const ImVec4 accent(0.26f, 0.53f, 0.86f, 1.0f), accent_hi(0.36f, 0.63f, 0.96f, 1.0f);
    s[ImGuiCol_WindowBg] = ImVec4(0.16f, 0.16f, 0.17f, 1.0f);
    s[ImGuiCol_MenuBarBg] = ImVec4(0.20f, 0.20f, 0.21f, 1.0f);
    s[ImGuiCol_TitleBgActive] = ImVec4(0.24f, 0.24f, 0.26f, 1.0f);
    s[ImGuiCol_FrameBg] = ImVec4(0.24f, 0.24f, 0.26f, 1.0f);
    s[ImGuiCol_FrameBgHovered] = ImVec4(0.30f, 0.30f, 0.33f, 1.0f);
    s[ImGuiCol_FrameBgActive] = ImVec4(0.35f, 0.35f, 0.38f, 1.0f);
    s[ImGuiCol_Header] = ImVec4(0.30f, 0.30f, 0.33f, 1.0f);
    s[ImGuiCol_HeaderHovered] = accent; s[ImGuiCol_HeaderActive] = accent_hi;
    s[ImGuiCol_Button] = ImVec4(0.30f, 0.30f, 0.33f, 1.0f);
    s[ImGuiCol_ButtonHovered] = accent; s[ImGuiCol_ButtonActive] = accent_hi;
    s[ImGuiCol_CheckMark] = accent_hi; s[ImGuiCol_SliderGrab] = accent; s[ImGuiCol_SliderGrabActive] = accent_hi;
    s[ImGuiCol_Tab] = ImVec4(0.22f, 0.22f, 0.24f, 1.0f); s[ImGuiCol_TabHovered] = accent; s[ImGuiCol_TabSelected] = ImVec4(0.30f, 0.30f, 0.33f, 1.0f);
    s[ImGuiCol_TabDimmed] = ImVec4(0.18f, 0.18f, 0.19f, 1.0f); s[ImGuiCol_TabDimmedSelected] = ImVec4(0.26f, 0.26f, 0.28f, 1.0f);
    slate.frame_rounding = 2.0f;
    out.push_back(slate);
    return out;
}
