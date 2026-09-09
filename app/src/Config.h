#pragma once
#include <string>
#include <vector>

// Persistent user settings: a small key=value file under the config
// directory (XDG_CONFIG_HOME/firn or ~/.config/firn, %APPDATA%/firn on
// Windows). The ImGui layout lives next to it.
struct Config {
    std::vector<std::string> recent_files;  // most recent first, max 10
    int window_w = 1400, window_h = 900;
    std::string last_directory;
    bool show_rulers = true, show_grid = false;
    int grid_spacing = 10;

    static std::string directory();          // created on demand
    static std::string layout_path();        // imgui.ini location
    void load();
    void save() const;
    void touch_recent(const std::string& path);
};
