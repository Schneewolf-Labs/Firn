#pragma once
// A UI theme: every Dear ImGui style color, the shape values that matter
// (rounding, padding, borders), and the text font and size. Built-in
// themes live in code; user themes are small key=value files under
// ~/.config/firn/themes/*.firntheme and can be exported and imported.
#include <optional>
#include <string>
#include <vector>

#include "imgui.h"

struct Theme {
    std::string name;
    bool builtin = false;
    std::string path;                 // file it was loaded from (user themes)
    ImVec4 colors[ImGuiCol_COUNT];
    float window_rounding = 0.0f, frame_rounding = 0.0f, tab_rounding = 4.0f, grab_rounding = 0.0f, popup_rounding = 0.0f;
    float window_border = 1.0f, frame_border = 0.0f, scrollbar_size = 14.0f;
    ImVec2 window_padding{8, 8}, frame_padding{4, 3}, item_spacing{8, 4};
    // Text: "builtin" is Dear ImGui's 13 px bitmap font, an empty path the
    // system's default sans face at `font_size`, otherwise a TrueType file.
    std::string font_path = "builtin";
    float font_size = 13.0f;

    // Pushes the colors and shape values into the live ImGui style (the
    // font is handled by App::apply_pending_font, since it rebuilds the atlas).
    void apply_style() const;
    // Reads the live style back (colors and shape; font untouched).
    void capture_style();

    bool save(const std::string& file, std::string* err = nullptr) const;
    static std::optional<Theme> load(const std::string& file, std::string* err = nullptr);
    static std::vector<Theme> builtins();
    static std::string user_dir();     // ~/.config/firn/themes, created on demand
    static const char* extension() { return "firntheme"; }
};
