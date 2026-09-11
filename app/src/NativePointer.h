#pragma once
struct SDL_Window;
union SDL_Event;
namespace native_pointer {
#ifdef __APPLE__
void init(SDL_Window* window);
bool handles(const SDL_Event& event);
void before_backend();
void before_frame(bool scripted);
void shutdown();
#else
inline void init(SDL_Window*) {}
inline bool handles(const SDL_Event&) { return false; }
inline void before_backend() {}
inline void before_frame(bool) {}
inline void shutdown() {}
#endif
}
