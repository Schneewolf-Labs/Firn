#pragma once
#include <functional>

// Abstracts "building a menu" so App::draw_menu and its helpers (the Layers
// and Selections menu bodies) are the single source of truth for the app's
// menu tree, rendered through either backend: ImGuiMenuBuilder (the
// in-window ImGui menu bar, every platform, and every context menu) or
// NativeMenuBuilder (the real macOS menu bar, NativeMenu_mac.mm).
//
// Shortcuts are passed as the canonical "Ctrl+..." form; each backend
// decides how to show or bind it (ImGui swaps Ctrl for Cmd on macOS for
// display only, see ui/Shortcut.h; the native backend turns it into a real
// key equivalent and modifier mask).
struct MenuBuilder {
    virtual ~MenuBuilder() = default;

    // Returns whether to render this submenu's contents; always call
    // end_menu() when it does, matching ImGui::BeginMenu's own contract.
    virtual bool begin_menu(const char* label, bool enabled = true) = 0;
    virtual void end_menu() = 0;

    // action runs when the item is activated: immediately, this frame, on
    // the ImGui backend; whenever Cocoa delivers the click on the native
    // one. `selected` draws a checkmark without any other effect.
    virtual void item(const char* label, const char* shortcut, bool enabled,
                       const std::function<void()>& action, bool selected = false,
                       const char* tooltip = nullptr) = 0;

    virtual void separator() = 0;
    virtual void text(const char* label) = 0;   // a disabled, non-clickable line

    // Disambiguates a run of dynamically generated items with the same or
    // repeated labels (Recent Files, the Window list, alpha channels). The
    // native backend addresses items by position and does not need this;
    // ImGui does, to keep its ID stack from colliding.
    virtual void push_id(int id) = 0;
    virtual void pop_id() = 0;

    // The one menu entry (View > Grid spacing) that embeds a live input
    // widget: ImGui-only, since a native menu can't host one the same way.
    // The ImGui backend runs body() in place; the native backend skips it
    // (the same value is editable in Preferences).
    virtual void imgui_only(const std::function<void()>& body) = 0;

    // Toggles *value in place and shows its state as a checkmark.
    void toggle(const char* label, const char* shortcut, bool* value, bool enabled = true) {
        item(label, shortcut, enabled, [value] { *value = !*value; }, *value);
    }
};
