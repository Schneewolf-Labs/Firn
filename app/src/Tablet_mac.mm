// macOS pen input. SDL2's system-window messages carry nothing on Cocoa,
// so a local event monitor watches the NSEvents before SDL sees them and
// keeps the latest tablet data for poll() to hand over on the main thread.
#include "Tablet.h"

#import <Cocoa/Cocoa.h>
#include <SDL.h>

#include "imgui.h"

namespace tablet {

namespace {
PenState g_latest;
bool g_dirty = false;
id g_monitor = nil;
}  // namespace

void init(SDL_Window*) {
    if (g_monitor) return;
    const NSEventMask mask = NSEventMaskTabletPoint | NSEventMaskTabletProximity | NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseDown;
    g_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent*(NSEvent* e) {
        const NSEventType t = [e type];
        if (t == NSEventTypeTabletProximity) {
            g_latest.present = [e isEnteringProximity];
            g_latest.eraser = [e pointingDeviceType] == NSPointingDeviceTypeEraser;
            g_dirty = true;
            return e;
        }
        const bool tablet_point = t == NSEventTypeTabletPoint || [e subtype] == NSEventSubtypeTabletPoint;
        if (!tablet_point) return e;
        g_latest.present = true;
        g_latest.pressure = static_cast<float>([e pressure]);
        const NSPoint tilt = [e tilt];
        g_latest.tilt_x = static_cast<float>(tilt.x * 60.0);
        g_latest.tilt_y = static_cast<float>(tilt.y * 60.0);
        g_dirty = true;
        return e;
    }];
}

void poll(PenState& pen) {
    if (!g_dirty) return;
    g_dirty = false;
    pen.present = g_latest.present;
    pen.pressure = g_latest.pressure;
    pen.tilt_x = g_latest.tilt_x;
    pen.tilt_y = g_latest.tilt_y;
    pen.eraser = g_latest.eraser;
    pen.last_seen = ImGui::GetTime();
}

void syswm(const SDL_Event&, PenState&) {}

const char* backend() { return g_monitor ? "Cocoa tablet events" : "none"; }

}  // namespace tablet
