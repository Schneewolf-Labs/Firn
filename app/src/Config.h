#pragma once
#include <string>
#include <vector>

// Persistent user settings: a small key=value file under the config
// directory (XDG_CONFIG_HOME/firn or ~/.config/firn, %APPDATA%/firn on
// Windows, ~/Library/Application Support/Firn on macOS). The ImGui layout
// lives next to it.
struct Config {
    std::vector<std::string> recent_files;  // most recent first, max 10
    int window_w = 1400, window_h = 900;
    std::string last_directory;
    bool show_rulers = true, show_grid = false;
    int grid_spacing = 10;
    int jpeg_quality = 90;
    int undo_limit = 100;             // history entries kept per document
    int undo_memory_mb = 1024;        // pixel snapshots kept by the history, per document
    int autosave_minutes = 5;         // 0 = off
    float ui_scale = 0.0f;            // 0 = follow the display's scale factor
    bool pen_size = true, pen_opacity = false;   // what pen pressure drives in the brushes
    int smooth_mode = 0;                          // brush smoothing mode
    float smooth_amount = 30.0f;
    int checker_size = 12;            // transparency checkerboard cell, in screen px
    int new_width = 800, new_height = 600;
    bool color_managed_display = true;   // convert tagged images to sRGB for the screen
    bool fit_tool_options = true;     // fit the default options strip when its content changes
    bool image_windows = false;          // one window per image instead of tabs
    // Off unless asked for: a check tells a server someone is running Firn
    // and from which address, which is the user's call to make.
    bool check_updates = false;
    // Where to send generative work. Empty means the feature is off: a
    // request tells a server you are running Firn, so it is opt in like the
    // update check.
    //
    // A server holds one model, so switching model means switching server:
    // the addresses are a named list and `generate_url` is whichever of them
    // is in use. Everything else in the program reads `generate_url` alone
    // and does not care that there is a list behind it.
    struct GenServer { std::string name, url; };
    std::vector<GenServer> generate_servers;
    std::string generate_url;
    // Points `generate_url` at one of the list, and does nothing if the
    // index is not one of them. Returns whether anything changed.
    bool use_generate_server(size_t index);
    int current_generate_server() const;   // -1 when the address is not one of the list
    // A readable default name for an address: the host and port, without
    // the scheme. Also used when a config written before the list is read.
    static std::string server_name_for(const std::string& url);
    // Which of the two stacked right-hand palettes was last showing. ImGui
    // keeps its own dock layout, but not reliably this: with a saved
    // workspace it hands the tab to whichever window drew last, so the
    // choice is kept here and put back on the way in.
    std::string right_palette = "Materials";
    long long last_update_check = 0;     // unix time of the last automatic check
    std::string theme = "Firn";          // a built-in or user theme name (see app/src/ui/Theme.h)
    std::string extra_tube_dir, extra_brush_dir, extra_texture_dir;

    static std::string directory();          // created on demand
    static std::string layout_path();        // imgui.ini location
    void load();
    void save() const;
    void touch_recent(const std::string& path);
};
