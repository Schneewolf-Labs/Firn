// The real macOS menu bar. Built every frame from App::draw_menu through
// NativeMenuBuilder, which reconciles a persistent NSMenu tree by position
// against what that one shared function body describes this frame (the
// same body the in-window ImGui menu draws from on every other platform,
// see ui/MenuBuilder.h) rather than duplicating the ~200-item tree by hand.
#include "NativeMenu.h"

#import <Cocoa/Cocoa.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "App.h"
#include "ui/MenuBuilder.h"

// Carries a std::function across into Cocoa's target-action dispatch:
// NSMenuItem's representedObject must be an Objective-C object, so this
// wraps the (otherwise non-Objective-C) callable Menu.cpp hands us.
@interface FirnMenuAction : NSObject
@property(nonatomic) std::function<void()> fn;
@end
@implementation FirnMenuAction
@end

@interface FirnMenuDispatcher : NSObject
- (void)fire:(NSMenuItem*)sender;
@end
@implementation FirnMenuDispatcher
- (void)fire:(NSMenuItem*)sender {
    if (FirnMenuAction* a = sender.representedObject)
        if (a.fn) a.fn();
}
@end

namespace {

FirnMenuDispatcher* g_dispatcher = nil;

// Attaches action, replacing whatever the slot held (a stale representedObject
// from a previous frame's differently-shaped menu is harmless to overwrite).
void set_action(NSMenuItem* item, std::function<void()> action) {
    FirnMenuAction* a = [[FirnMenuAction alloc] init];
    a.fn = std::move(action);
    item.representedObject = a;
    item.target = g_dispatcher;
    item.action = @selector(fire:);
}

// Only "Ctrl+..." shortcuts (the app's Cmd-accepting accelerators, see
// App::handle_shortcuts) become real key equivalents. Bare shortcuts like
// "+"/"-"/"Delete"/"Shift+I" stay off the native menu: Cocoa would intercept
// them globally, including while a text field has focus, where the app's
// own ImGui-level handling already correctly defers to io.WantTextInput.
void set_shortcut(NSMenuItem* item, const char* shortcut) {
    if (!shortcut || !std::strstr(shortcut, "Ctrl") || ImGui::GetIO().WantTextInput ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        item.keyEquivalent = @"";
        item.keyEquivalentModifierMask = 0;
        return;
    }
    NSEventModifierFlags mods = 0;
    std::string key;
    std::string s(shortcut);
    for (size_t pos = 0; pos <= s.size();) {
        const size_t plus = s.find('+', pos);
        const std::string tok = s.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos);
        if (tok == "Ctrl") mods |= NSEventModifierFlagCommand;
        else if (tok == "Shift") mods |= NSEventModifierFlagShift;
        else if (tok == "Alt") mods |= NSEventModifierFlagOption;
        else key = tok;
        if (plus == std::string::npos) break;
        pos = plus + 1;
    }
    item.keyEquivalent = key.size() == 1 ? [NSString stringWithFormat:@"%c", static_cast<char>(std::tolower(static_cast<unsigned char>(key[0])))] : @"";
    item.keyEquivalentModifierMask = mods;
}

// Reconciles a persistent NSMenu tree by position: the Nth begin_menu/item/
// separator call this frame addresses the Nth child of the current NSMenu,
// reusing it when its kind already matches (a plain item can't become a
// separator in place) and replacing it otherwise. Menu structure is static
// frame to frame except a few dynamic lists (Recent Files, the Window list,
// alpha channels), which this handles the same way as everything else.
class NativeMenuBuilder final : public MenuBuilder {
public:
    explicit NativeMenuBuilder(NSMenu* root, NSInteger start_index) { stack_.push_back({root, start_index}); }
    ~NativeMenuBuilder() override { trim(); }

    bool begin_menu(const char* label, bool enabled) override {
        NSMenuItem* item = slot(false);
        item.title = @(label);
        item.enabled = enabled;
        item.action = nil;   // a submenu parent is never itself clickable
        item.target = nil;
        if (!item.submenu) {
            NSMenu* sub = [[NSMenu alloc] initWithTitle:@(label)];
            sub.autoenablesItems = NO;
            item.submenu = sub;
        } else {
            item.submenu.title = @(label);
        }
        // Matches ImGui::BeginMenu exactly: a disabled menu is never entered.
        // The menu bodies rely on that to dereference doc/layer unconditionally
        // once past their enabled check; a disabled submenu can't be opened
        // regardless of what (possibly stale) items are left inside it.
        if (!enabled) return false;
        stack_.push_back({item.submenu, 0});
        return true;
    }
    void end_menu() override {
        trim();
        stack_.pop_back();
    }

    void item(const char* label, const char* shortcut, bool enabled, const std::function<void()>& action,
              bool selected, const char* tooltip) override {
        NSMenuItem* mi = slot(false);
        mi.title = @(label);
        mi.enabled = enabled;
        mi.submenu = nil;
        mi.state = selected ? NSControlStateValueOn : NSControlStateValueOff;
        mi.toolTip = tooltip ? @(tooltip) : nil;
        set_shortcut(mi, shortcut);
        set_action(mi, action);
    }

    void separator() override { slot(true); }

    void text(const char* label) override {
        NSMenuItem* mi = slot(false);
        mi.title = @(label);
        mi.enabled = NO;
        mi.submenu = nil;
        mi.state = NSControlStateValueOff;
        mi.keyEquivalent = @"";
        mi.action = nil;
        mi.target = nil;
    }

    void imgui_only(const std::function<void()>&) override {}   // no native equivalent; see MenuBuilder.h
    void push_id(int) override {}                               // native addresses by position, not label
    void pop_id() override {}

private:
    struct Frame { NSMenu* menu; NSInteger next; };
    std::vector<Frame> stack_;

    NSMenuItem* slot(bool want_separator) {
        Frame& f = stack_.back();
        if (f.next < static_cast<NSInteger>(f.menu.itemArray.count)) {
            NSMenuItem* existing = [f.menu itemAtIndex:f.next];
            if (existing.isSeparatorItem == want_separator) { ++f.next; return existing; }
            [f.menu removeItemAtIndex:f.next];
        }
        NSMenuItem* fresh = want_separator ? [NSMenuItem separatorItem] : [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
        [f.menu insertItem:fresh atIndex:f.next];
        ++f.next;
        return fresh;
    }
    void trim() {
        Frame& f = stack_.back();
        while (static_cast<NSInteger>(f.menu.itemArray.count) > f.next) [f.menu removeItemAtIndex:f.menu.itemArray.count - 1];
    }
};

}  // namespace

namespace native_menu {

void init(App& app, SDL_Window*) {
    if (g_dispatcher) return;
    g_dispatcher = [[FirnMenuDispatcher alloc] init];
    NSMenu* main_menu = NSApp.mainMenu;
    if (!main_menu || main_menu.itemArray.count == 0) return;   // SDL always creates the app menu at index 0
    // Route the app menu's SDL-provided About/Quit through the app's own
    // logic instead of Cocoa's generic panel and an unconditional terminate:
    // request_quit() is what makes an unsaved-changes prompt possible.
    for (NSMenuItem* item in [main_menu itemAtIndex:0].submenu.itemArray) {
        if (item.action == @selector(orderFrontStandardAboutPanel:)) set_action(item, [&app] { app.show_about_dialog = true; });
        else if (item.action == @selector(terminate:)) set_action(item, [&app] { app.request_quit(); });
    }
}

void update(App& app) {
    if (!g_dispatcher) return;
    NSMenu* main_menu = NSApp.mainMenu;
    if (!main_menu) return;
    // SDL's main loop is a plain while(), not [NSApp run]: nothing else drains
    // an autorelease pool between frames, so the NSStrings this creates every
    // frame (@(label) etc.) would otherwise pile up for the life of the process.
    @autoreleasepool {
        NativeMenuBuilder m(main_menu, 1);   // index 0 is the app menu SDL created; leave it alone
        app.draw_menu(m);
    }
}

}  // namespace native_menu
