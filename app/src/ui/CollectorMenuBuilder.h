#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ui/MenuBuilder.h"

// A third MenuBuilder that records the menu tree instead of drawing it, so
// the command palette can search everything the menus offer without anyone
// maintaining a second list. App::draw_menu is run through this exactly as
// it is run through the ImGui and native backends, which is what keeps the
// palette from drifting: a menu item that exists is findable by definition.
//
// Do not build the palette on the action table instead. That covers what is
// scriptable -- fewer than a hundred entries -- and would miss every effect,
// Canvas Size, layer styles, masks and guides.
struct CollectorMenuBuilder final : MenuBuilder {
    struct Entry {
        std::string path;        // "Effects > Texture Effects > Mosaic - Glass"
        std::string label;       // the leaf on its own
        std::string shortcut;    // canonical "Ctrl+..." form, empty when there is none
        bool enabled = true;
        std::function<void()> action;
    };
    std::vector<Entry> entries;

    // Every submenu is entered whether or not it is enabled: a disabled item
    // should still be findable, and shown as unavailable, rather than
    // vanishing from search because of the current selection.
    bool begin_menu(const char* label, bool enabled) override {
        stack_.emplace_back(label);
        enabled_.push_back(!enabled_.empty() ? (enabled_.back() && enabled) : enabled);
        return true;
    }
    void end_menu() override {
        if (!stack_.empty()) stack_.pop_back();
        if (!enabled_.empty()) enabled_.pop_back();
    }

    void item(const char* label, const char* shortcut, bool enabled,
              const std::function<void()>& action, bool selected, const char* tooltip) override {
        (void)selected;
        (void)tooltip;
        if (!label || !*label) return;
        Entry e;
        for (const std::string& s : stack_) e.path += s + " > ";
        e.path += label;
        e.label = label;
        if (shortcut) e.shortcut = shortcut;
        e.enabled = enabled && (enabled_.empty() || enabled_.back());
        e.action = action;
        entries.push_back(std::move(e));
    }

    // A separator, a caption, an embedded widget and the ID stack are all
    // about drawing; there is nothing to record.
    void separator() override {}
    void text(const char*) override {}
    void imgui_only(const std::function<void()>&) override {}
    void push_id(int) override {}
    void pop_id() override {}

private:
    std::vector<std::string> stack_;
    std::vector<bool> enabled_;
};
