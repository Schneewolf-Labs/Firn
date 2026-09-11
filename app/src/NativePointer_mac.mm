// Preserve event-local coordinates for mouse events posted to this process.
// SDL's Cocoa/global cursor polling can report the physical pointer instead.
#include "NativePointer.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <SDL.h>
#include <SDL_syswm.h>
#import <Cocoa/Cocoa.h>
#include <deque>
namespace native_pointer {
namespace {
struct Event { float x, y; int button; bool down; };
std::deque<Event> pending;
id monitor = nil;
NSWindow* target = nil;
Uint32 window_id = 0;

int backend_queue_start = 0;
}
void init(SDL_Window* window) {
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info)) return;
    target = info.info.cocoa.window;
    window_id = SDL_GetWindowID(window);
    const NSEventMask mask = NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged |
        NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged |
        NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp |
        NSEventMaskRightMouseDown | NSEventMaskRightMouseUp |
        NSEventMaskOtherMouseDown | NSEventMaskOtherMouseUp;
    monitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent*(NSEvent* e) {
        if (e.window != target) return e;
        NSView* view = target.contentView;
        NSPoint p = [view convertPoint:e.locationInWindow fromView:nil];
        float y = static_cast<float>(view.isFlipped ? p.y : view.bounds.size.height - p.y);
        int button = -1;
        bool down = false;
        switch (e.type) {
            case NSEventTypeLeftMouseDown: down = true; [[fallthrough]];
            case NSEventTypeLeftMouseUp: button = 0; break;
            case NSEventTypeRightMouseDown: down = true; [[fallthrough]];
            case NSEventTypeRightMouseUp: button = 1; break;
            case NSEventTypeOtherMouseDown: down = true; [[fallthrough]];
            case NSEventTypeOtherMouseUp: button = static_cast<int>(e.buttonNumber); break;
            default: break;
        }
        const Event event{static_cast<float>(p.x), y, button, down};
        // Keep the latest motion within an OS pump, but never coalesce
        // across a button transition. High-rate mice must not build up
        // a backlog of one frame per hardware sample.
        if (button < 0 && !pending.empty() && pending.back().button < 0)
            pending.back() = event;
        else pending.push_back(event);
        return e;
    }];
}
bool handles(const SDL_Event& e) {
    if (!monitor) return false;
    if (e.type == SDL_MOUSEMOTION) return e.motion.windowID == window_id;
    if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP)
        return e.button.windowID == window_id;
    return false;
}
void before_backend() {
    backend_queue_start = ImGui::GetCurrentContext()->InputEventsQueue.Size;
}
void before_frame(bool scripted) {
    if (monitor && !scripted) {
        // Only remove the backend's global-cursor fallback, never native
        // events or focus/wheel events queued before NewFrame.
        auto& queue = ImGui::GetCurrentContext()->InputEventsQueue;
        for (int i = queue.Size - 1; i >= backend_queue_start; --i)
            if (queue[i].Type == ImGuiInputEventType_MousePos) queue.erase(queue.Data + i);
        ImGuiIO& io = ImGui::GetIO();
        if (!pending.empty()) {
            // A posted drag may arrive entirely in one OS event pump.
            // Consume one event per render so tools see its intermediate
            // positions while the button is still held.
            const Event e = pending.front();
            pending.pop_front();
            io.AddMousePosEvent(e.x, e.y);
            if (e.button >= 0 && e.button < 5) io.AddMouseButtonEvent(e.button, e.down);
        }
    } else {
        pending.clear();
    }
}
void shutdown() {
    if (monitor) [NSEvent removeMonitor:monitor];
    monitor = nil; target = nil; pending.clear();
}
}
