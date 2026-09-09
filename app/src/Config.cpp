#include "Config.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

std::string Config::directory() {
    fs::path base;
    if (const char* x = std::getenv("XDG_CONFIG_HOME")) base = x;
    else if (const char* a = std::getenv("APPDATA")) base = a;
    else if (const char* h = std::getenv("HOME")) base = fs::path(h) / ".config";
    else base = fs::temp_directory_path();
    const fs::path dir = base / "firn";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir.string();
}

std::string Config::layout_path() { return (fs::path(directory()) / "layout.ini").string(); }

void Config::load() {
    std::ifstream f(fs::path(directory()) / "firn.cfg");
    std::string line;
    recent_files.clear();
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "recent") recent_files.push_back(value);
        else if (key == "window_w") window_w = std::max(320, std::atoi(value.c_str()));
        else if (key == "window_h") window_h = std::max(240, std::atoi(value.c_str()));
        else if (key == "last_directory") last_directory = value;
        else if (key == "show_rulers") show_rulers = value == "1";
        else if (key == "show_grid") show_grid = value == "1";
        else if (key == "grid_spacing") grid_spacing = std::clamp(std::atoi(value.c_str()), 1, 1000);
        else if (key == "jpeg_quality") jpeg_quality = std::clamp(std::atoi(value.c_str()), 1, 100);
        else if (key == "undo_limit") undo_limit = std::clamp(std::atoi(value.c_str()), 1, 10000);
        else if (key == "checker_size") checker_size = std::clamp(std::atoi(value.c_str()), 2, 64);
        else if (key == "color_managed_display") color_managed_display = value == "1";
        else if (key == "image_windows") image_windows = value == "1";
        else if (key == "new_width") new_width = std::clamp(std::atoi(value.c_str()), 1, 30000);
        else if (key == "new_height") new_height = std::clamp(std::atoi(value.c_str()), 1, 30000);
        else if (key == "extra_tube_dir") extra_tube_dir = value;
        else if (key == "extra_brush_dir") extra_brush_dir = value;
        else if (key == "extra_texture_dir") extra_texture_dir = value;
    }
}

void Config::save() const {
    std::ofstream f(fs::path(directory()) / "firn.cfg");
    for (const auto& r : recent_files) f << "recent=" << r << "\n";
    f << "window_w=" << window_w << "\nwindow_h=" << window_h << "\n";
    f << "last_directory=" << last_directory << "\n";
    f << "show_rulers=" << (show_rulers ? 1 : 0) << "\nshow_grid=" << (show_grid ? 1 : 0) << "\n";
    f << "grid_spacing=" << grid_spacing << "\n";
    f << "jpeg_quality=" << jpeg_quality << "\nundo_limit=" << undo_limit << "\nchecker_size=" << checker_size << "\n";
    f << "color_managed_display=" << (color_managed_display ? 1 : 0) << "\n";
    f << "image_windows=" << (image_windows ? 1 : 0) << "\n";
    f << "new_width=" << new_width << "\nnew_height=" << new_height << "\n";
    f << "extra_tube_dir=" << extra_tube_dir << "\nextra_brush_dir=" << extra_brush_dir << "\nextra_texture_dir=" << extra_texture_dir << "\n";
}

void Config::touch_recent(const std::string& path) {
    recent_files.erase(std::remove(recent_files.begin(), recent_files.end(), path), recent_files.end());
    recent_files.insert(recent_files.begin(), path);
    if (recent_files.size() > 10) recent_files.resize(10);
}
