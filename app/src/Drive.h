#pragma once
#include <cstdint>
#include <deque>
#include <string>

#include "imgui.h"

struct App;
struct SDL_Window;

// Scripted driving over a Unix socket (FIRN_DRIVE=<socket path>): synthetic
// ImGui mouse and keyboard events for a virtual cursor, framebuffer
// screenshots, and state queries. The real pointer is never touched, and
// real mouse events are ignored while driving so a stray pointer cannot
// interfere. Each command is acknowledged with "ok ...\n" once its frames
// have run, so clients never guess at timing. See scripts/drive.py.
// On Windows FIRN_DRIVE names an address file for a loopback TCP port
// instead (DriveAddress.h); the protocol is the same once connected.
class Driver {
public:
    ~Driver();
    bool start(const std::string& socket_path);
    bool active() const { return listen_fd_ >= 0; }
    // Call between ImGui_ImplSDL2_NewFrame() and ImGui::NewFrame().
    void before_frame(App& app, SDL_Window* window);
    // Call after ImGui::Render() and the GL draw, before the swap.
    void after_render(App& app);
    // Draws the virtual cursor; call inside the frame.
    void draw_cursor();

private:
    struct Step {
        enum Kind { MousePos, MouseDown, MouseUp, KeyDown, KeyUp, Chars, Wheel, Wait, Shot, Tool, Ack, Layer, Quit, Set, Save, Open, Adjust, Do, Profile, Drop } kind = MousePos;
        Step() = default;
        Step(Kind k) : kind(k) {}   // NOLINT: implicit on purpose, steps_.push_back({Step::Ack})
        float x = 0, y = 0;
        int button = 0;
        ImGuiKey key = ImGuiKey_None;
        std::string text;
        int frames = 0;
        double seconds = 0;      // Wait: minimum wall time as well (double-click separation)
        double deadline = 0;
    };
    void poll_socket();
    void drop_client();
    bool parse_line(const std::string& line, App& app);
    void ack(const std::string& payload);
    std::string state_text(App& app) const;
    std::string state_json(App& app) const;   // the same facts, for clients that parse JSON
    bool state_json_ = false;
    ImVec2 image_to_window(const App& app, float x, float y) const;

    SDL_Window* window_ = nullptr;
    // File descriptors, or Winsock SOCKETs (whose INVALID_SOCKET is also -1 here).
    std::intptr_t listen_fd_ = -1, client_fd_ = -1;
    std::string address_path_;   // Windows: the address file this instance wrote
    std::string token_;          // Windows: what a client must send first
    bool authed_ = true;
    std::string inbuf_;
    std::deque<Step> steps_;
    bool cursor_valid_ = false;
    float cx_ = 0, cy_ = 0;
    std::string pending_shot_;
    std::string pending_ack_;
    bool shot_after_render_ = false;
};
