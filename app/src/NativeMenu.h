#pragma once
// The real macOS menu bar (NativeMenu_mac.mm), built from App::draw_menu
// through NativeMenuBuilder so it never drifts from the in-window ImGui
// menu used on every other platform. Safe to include and call from any
// platform's main.cpp; init/update are no-ops off Apple (there is no
// NativeMenu.cpp — see Tablet.h for the same pattern with a real per-
// platform implementation instead of a no-op).
struct App;
struct SDL_Window;

namespace native_menu {
#ifdef __APPLE__
void init(App& app, SDL_Window* window);   // once, after the window exists
void update(App& app);                     // every frame: reconciles the tree
#else
inline void init(App&, SDL_Window*) {}
inline void update(App&) {}
#endif
}
