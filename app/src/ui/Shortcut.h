#pragma once
#include <string>

// Menu shortcut labels are written literally as "Ctrl+...", which reads
// wrong on macOS where the actual modifier is Cmd (App::handle_shortcuts
// already accepts io.KeySuper there; this only changes what the menu shows).
#ifdef __APPLE__
inline const char* mac_shortcut_label(const char* s) {
    static std::string buf;
    buf = s;
    for (size_t p = 0; (p = buf.find("Ctrl", p)) != std::string::npos; p += 3)
        buf.replace(p, 4, "Cmd");
    return buf.c_str();
}
#define SC(s) mac_shortcut_label(s)
#else
#define SC(s) (s)
#endif
