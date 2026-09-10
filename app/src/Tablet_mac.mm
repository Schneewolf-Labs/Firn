// macOS pen input: tablet data rides on the NSEvent SDL hands us.
#include "Tablet.h"

#import <Cocoa/Cocoa.h>
#include <SDL.h>
#include <SDL_syswm.h>

#include "imgui.h"

namespace tablet {

void init(SDL_Window*) { SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE); }
void poll(PenState&) {}

void syswm(const SDL_Event& event, PenState& pen) {
    const SDL_SysWMmsg* m = event.syswm.msg;
    if (!m || m->subsystem != SDL_SYSWM_COCOA) return;
    NSEvent* e = m->msg.cocoa.event;
    if (!e) return;
    const NSEventType t = [e type];
    if (t == NSEventTypeTabletProximity) {
        pen.present = [e isEnteringProximity];
        pen.eraser = [e pointingDeviceType] == NSPointingDeviceTypeEraser;
        pen.last_seen = ImGui::GetTime();
        return;
    }
    const bool tablet_point = t == NSEventTypeTabletPoint ||
        ((t == NSEventTypeMouseMoved || t == NSEventTypeLeftMouseDragged || t == NSEventTypeLeftMouseDown) && [e subtype] == NSEventSubtypeTabletPoint);
    if (!tablet_point) return;
    pen.present = true;
    pen.pressure = [e pressure];
    const NSPoint tilt = [e tilt];
    pen.tilt_x = static_cast<float>(tilt.x * 60.0);
    pen.tilt_y = static_cast<float>(tilt.y * 60.0);
    pen.last_seen = ImGui::GetTime();
}

const char* backend() { return "Cocoa tablet events"; }

}  // namespace tablet
