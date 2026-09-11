#pragma once
#include "imgui.h"
#include "ui/MenuBuilder.h"
#include "ui/Shortcut.h"

// The default MenuBuilder: draws through ImGui, exactly as the menu bar
// always has. Used for the in-window menu bar on every platform (and as the
// native macOS menu bar's fallback, see NativeMenu_mac.mm) and for every
// context menu (those stay ImGui popups even where the top bar is native).
struct ImGuiMenuBuilder final : MenuBuilder {
    bool begin_menu(const char* label, bool enabled) override {
        return ImGui::BeginMenu(label, enabled);
    }
    void end_menu() override { ImGui::EndMenu(); }

    void item(const char* label, const char* shortcut, bool enabled,
              const std::function<void()>& action, bool selected, const char* tooltip) override {
        if (ImGui::MenuItem(label, shortcut ? SC(shortcut) : nullptr, selected, enabled)) pending = action;
        if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    }

    // Dispatch only after all menu predicates have been evaluated and popups
    // ended: actions may close documents or replace the layer stack.
    void dispatch() { if (pending) { auto action = std::move(pending); action(); } }
    std::function<void()> pending;

    void separator() override { ImGui::Separator(); }
    void text(const char* label) override { ImGui::TextDisabled("%s", label); }
    void imgui_only(const std::function<void()>& body) override { body(); }
    void push_id(int id) override { ImGui::PushID(id); }
    void pop_id() override { ImGui::PopID(); }
};
