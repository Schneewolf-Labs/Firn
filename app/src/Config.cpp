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
    }
}

void Config::save() const {
    std::ofstream f(fs::path(directory()) / "firn.cfg");
    for (const auto& r : recent_files) f << "recent=" << r << "\n";
    f << "window_w=" << window_w << "\nwindow_h=" << window_h << "\n";
    f << "last_directory=" << last_directory << "\n";
    f << "show_rulers=" << (show_rulers ? 1 : 0) << "\nshow_grid=" << (show_grid ? 1 : 0) << "\n";
    f << "grid_spacing=" << grid_spacing << "\n";
}

void Config::touch_recent(const std::string& path) {
    recent_files.erase(std::remove(recent_files.begin(), recent_files.end(), path), recent_files.end());
    recent_files.insert(recent_files.begin(), path);
    if (recent_files.size() > 10) recent_files.resize(10);
}
