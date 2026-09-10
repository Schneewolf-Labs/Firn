#pragma once
// Pen tablets: pressure, tilt and the eraser tip, read from the platform's
// own input path since SDL2 has none. X11 uses XInput2 raw events on a
// private connection; Windows reads pen info from WM_POINTER messages;
// macOS takes them from the NSEvent (Tablet_mac.mm).
struct SDL_Window;
union SDL_Event;

struct PenState {
    bool present = false;      // a pen reported something recently
    float pressure = 1.0f;     // 0..1 while in contact
    float tilt_x = 0.0f, tilt_y = 0.0f;   // degrees, when the tablet reports them
    bool eraser = false;       // the eraser end is in use
    double last_seen = 0.0;    // ImGui time of the last report
};

namespace tablet {
void init(SDL_Window* window);
void poll(PenState& pen);                       // once per frame
void syswm(const SDL_Event& event, PenState& pen);   // SDL_SYSWMEVENT
const char* backend();                          // for Help > About
}
