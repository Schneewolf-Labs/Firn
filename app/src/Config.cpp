#include "Config.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

std::string Config::server_name_for(const std::string& url) {
    std::string s = url;
    for (const char* scheme : {"http://", "https://"})
        if (s.compare(0, std::string(scheme).size(), scheme) == 0) { s = s.substr(std::string(scheme).size()); break; }
    while (!s.empty() && (s.back() == '/' || s.back() == ' ')) s.pop_back();
    // A name is stored before a bar, so it cannot contain one.
    s.erase(std::remove(s.begin(), s.end(), '|'), s.end());
    return s.empty() ? "server" : s;
}

std::string Config::directory() {
    fs::path base;
    fs::path name = "firn";
    if (const char* x = std::getenv("XDG_CONFIG_HOME")) base = x;
    else if (const char* a = std::getenv("APPDATA")) base = a;
#ifdef __APPLE__
    else if (const char* h = std::getenv("HOME")) { base = fs::path(h) / "Library" / "Application Support"; name = "Firn"; }
#else
    else if (const char* h = std::getenv("HOME")) base = fs::path(h) / ".config";
#endif
    else base = fs::temp_directory_path();
    const fs::path dir = base / name;
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir.string();
}

std::string Config::layout_path() { return (fs::path(directory()) / "layout.ini").string(); }

void Config::load() {
    std::ifstream f(fs::path(directory()) / "firn.cfg");
    std::string line;
    recent_files.clear();
    generate_servers.clear();
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "recent") recent_files.push_back(value);
        else if (key == "generate_server") {
            // name|address, split at the first bar: an address may contain
            // one, a name may not (they are stripped on the way in).
            const auto bar = value.find('|');
            if (bar != std::string::npos && bar + 1 < value.size())
                generate_servers.push_back({value.substr(0, bar), value.substr(bar + 1)});
        }
        else if (key == "window_w") window_w = std::max(320, std::atoi(value.c_str()));
        else if (key == "window_h") window_h = std::max(240, std::atoi(value.c_str()));
        else if (key == "last_directory") last_directory = value;
        else if (key == "show_rulers") show_rulers = value == "1";
        else if (key == "show_grid") show_grid = value == "1";
        else if (key == "grid_spacing") grid_spacing = std::clamp(std::atoi(value.c_str()), 1, 1000);
        else if (key == "jpeg_quality") jpeg_quality = std::clamp(std::atoi(value.c_str()), 1, 100);
        else if (key == "undo_limit") undo_limit = std::clamp(std::atoi(value.c_str()), 1, 10000);
        else if (key == "undo_memory_mb") undo_memory_mb = std::clamp(std::atoi(value.c_str()), 64, 65536);
        else if (key == "autosave_minutes") autosave_minutes = std::clamp(std::atoi(value.c_str()), 0, 120);
        else if (key == "ui_scale") ui_scale = std::clamp(static_cast<float>(std::atof(value.c_str())), 0.0f, 4.0f);
        else if (key == "pen_size") pen_size = value == "1";
        else if (key == "pen_opacity") pen_opacity = value == "1";
        else if (key == "smooth_mode") smooth_mode = std::clamp(std::atoi(value.c_str()), 0, 3);
        else if (key == "smooth_amount") smooth_amount = std::clamp(static_cast<float>(std::atof(value.c_str())), 1.0f, 100.0f);
        else if (key == "checker_size") checker_size = std::clamp(std::atoi(value.c_str()), 2, 64);
        else if (key == "check_updates") check_updates = value == "1";
        else if (key == "generate_url") generate_url = value;
        else if (key == "right_palette") right_palette = value;
        else if (key == "last_update_check") last_update_check = std::atoll(value.c_str());
        else if (key == "color_managed_display") color_managed_display = value == "1";
        else if (key == "fit_tool_options") fit_tool_options = value == "1";
        else if (key == "image_windows") image_windows = value == "1";
        else if (key == "theme") theme = value;
        else if (key == "new_width") new_width = std::clamp(std::atoi(value.c_str()), 1, 30000);
        else if (key == "new_height") new_height = std::clamp(std::atoi(value.c_str()), 1, 30000);
        else if (key == "extra_tube_dir") extra_tube_dir = value;
        else if (key == "extra_brush_dir") extra_brush_dir = value;
        else if (key == "extra_texture_dir") extra_texture_dir = value;
    }
    // A config from before the list had one address and no names: keep it,
    // under a name taken from the address itself.
    if (generate_servers.empty() && !generate_url.empty())
        generate_servers.push_back({server_name_for(generate_url), generate_url});
    // An address that is not one of the list (hand-edited, or the entry was
    // removed) is left alone if set, and otherwise follows the first entry.
    if (generate_url.empty() && !generate_servers.empty()) generate_url = generate_servers.front().url;
}

bool Config::use_generate_server(size_t index) {
    if (index >= generate_servers.size() || generate_url == generate_servers[index].url) return false;
    generate_url = generate_servers[index].url;
    return true;
}

int Config::current_generate_server() const {
    for (size_t i = 0; i < generate_servers.size(); ++i)
        if (generate_servers[i].url == generate_url) return static_cast<int>(i);
    return -1;
}

void Config::save() const {
    std::ofstream f(fs::path(directory()) / "firn.cfg");
    for (const auto& r : recent_files) f << "recent=" << r << "\n";
    f << "window_w=" << window_w << "\nwindow_h=" << window_h << "\n";
    f << "last_directory=" << last_directory << "\n";
    f << "show_rulers=" << (show_rulers ? 1 : 0) << "\nshow_grid=" << (show_grid ? 1 : 0) << "\n";
    f << "grid_spacing=" << grid_spacing << "\n";
    f << "jpeg_quality=" << jpeg_quality << "\nundo_limit=" << undo_limit << "\nundo_memory_mb=" << undo_memory_mb << "\nchecker_size=" << checker_size << "\n";
    f << "check_updates=" << (check_updates ? 1 : 0) << "\nlast_update_check=" << last_update_check << "\n";
    for (const auto& g : generate_servers) f << "generate_server=" << g.name << "|" << g.url << "\n";
    f << "generate_url=" << generate_url << "\n";
    f << "right_palette=" << right_palette << "\n";
    f << "color_managed_display=" << (color_managed_display ? 1 : 0) << "\n";
    f << "fit_tool_options=" << (fit_tool_options ? 1 : 0) << "\n";
    f << "image_windows=" << (image_windows ? 1 : 0) << "\n";
    f << "autosave_minutes=" << autosave_minutes << "\n";
    f << "ui_scale=" << ui_scale << "\n";
    f << "pen_size=" << (pen_size ? 1 : 0) << "\npen_opacity=" << (pen_opacity ? 1 : 0) << "\n";
    f << "smooth_mode=" << smooth_mode << "\nsmooth_amount=" << smooth_amount << "\n";
    f << "theme=" << theme << "\n";
    f << "new_width=" << new_width << "\nnew_height=" << new_height << "\n";
    f << "extra_tube_dir=" << extra_tube_dir << "\nextra_brush_dir=" << extra_brush_dir << "\nextra_texture_dir=" << extra_texture_dir << "\n";
}

void Config::touch_recent(const std::string& path) {
    recent_files.erase(std::remove(recent_files.begin(), recent_files.end(), path), recent_files.end());
    recent_files.insert(recent_files.begin(), path);
    if (recent_files.size() > 10) recent_files.resize(10);
}
