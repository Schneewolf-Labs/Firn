#include "Tablet.h"

#include <cstring>
#include <string>
#include <vector>

#include <SDL.h>
#include <SDL_syswm.h>

#include "imgui.h"

#if defined(FIRN_HAVE_XI2)
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#endif

namespace tablet {

#if defined(FIRN_HAVE_XI2)

namespace {
struct Device { int id; int pressure_axis; double pmin, pmax; int tilt_x_axis, tilt_y_axis; bool eraser; };
Display* g_dpy = nullptr;
int g_opcode = 0;
std::vector<Device> g_devices;

void query_devices() {
    g_devices.clear();
    int n = 0;
    XIDeviceInfo* info = XIQueryDevice(g_dpy, XIAllDevices, &n);
    const Atom pressure = XInternAtom(g_dpy, "Abs Pressure", True), tilt_x = XInternAtom(g_dpy, "Abs Tilt X", True), tilt_y = XInternAtom(g_dpy, "Abs Tilt Y", True);
    for (int i = 0; i < n; ++i) {
        Device d{info[i].deviceid, -1, 0, 1, -1, -1, false};
        for (int c = 0; c < info[i].num_classes; ++c) {
            if (info[i].classes[c]->type != XIValuatorClass) continue;
            const auto* v = reinterpret_cast<XIValuatorClassInfo*>(info[i].classes[c]);
            if (pressure && v->label == pressure) { d.pressure_axis = v->number; d.pmin = v->min; d.pmax = v->max; }
            else if (tilt_x && v->label == tilt_x) d.tilt_x_axis = v->number;
            else if (tilt_y && v->label == tilt_y) d.tilt_y_axis = v->number;
        }
        if (d.pressure_axis < 0) continue;
        std::string name = info[i].name ? info[i].name : "";
        for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        d.eraser = name.find("eraser") != std::string::npos;
        g_devices.push_back(d);
    }
    if (info) XIFreeDeviceInfo(info);
}
}  // namespace

void init(SDL_Window*) {
    const char* driver = SDL_GetCurrentVideoDriver();
    if (!driver || std::strcmp(driver, "x11") != 0) return;
    g_dpy = XOpenDisplay(nullptr);
    if (!g_dpy) return;
    int event = 0, error = 0;
    if (!XQueryExtension(g_dpy, "XInputExtension", &g_opcode, &event, &error)) { XCloseDisplay(g_dpy); g_dpy = nullptr; return; }
    int major = 2, minor = 2;
    if (XIQueryVersion(g_dpy, &major, &minor) != Success) { XCloseDisplay(g_dpy); g_dpy = nullptr; return; }
    // Raw motion from every device reaches every client that asks, grabs or not.
    unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {};
    XISetMask(bits, XI_RawMotion);
    XISetMask(bits, XI_HierarchyChanged);
    XIEventMask mask{XIAllDevices, sizeof(bits), bits};
    XISelectEvents(g_dpy, DefaultRootWindow(g_dpy), &mask, 1);
    XFlush(g_dpy);
    query_devices();
}

void poll(PenState& pen) {
    if (!g_dpy) return;
    while (XPending(g_dpy) > 0) {
        XEvent ev;
        XNextEvent(g_dpy, &ev);
        if (ev.type != GenericEvent || ev.xcookie.extension != g_opcode) continue;
        if (!XGetEventData(g_dpy, &ev.xcookie)) continue;
        if (ev.xcookie.evtype == XI_HierarchyChanged) { query_devices(); XFreeEventData(g_dpy, &ev.xcookie); continue; }
        if (ev.xcookie.evtype == XI_RawMotion) {
            const auto* raw = static_cast<const XIRawEvent*>(ev.xcookie.data);
            const Device* dev = nullptr;
            for (const Device& d : g_devices) if (d.id == raw->sourceid || d.id == raw->deviceid) { dev = &d; break; }
            if (dev) {
                int k = 0;
                for (int axis = 0; axis < raw->valuators.mask_len * 8; ++axis) {
                    if (!XIMaskIsSet(raw->valuators.mask, axis)) continue;
                    const double v = raw->valuators.values[k++];
                    if (axis == dev->pressure_axis) pen.pressure = static_cast<float>(dev->pmax > dev->pmin ? (v - dev->pmin) / (dev->pmax - dev->pmin) : v);
                    else if (axis == dev->tilt_x_axis) pen.tilt_x = static_cast<float>(v);
                    else if (axis == dev->tilt_y_axis) pen.tilt_y = static_cast<float>(v);
                }
                pen.present = true;
                pen.eraser = dev->eraser;
                pen.last_seen = ImGui::GetTime();
            }
        }
        XFreeEventData(g_dpy, &ev.xcookie);
    }
}

void syswm(const SDL_Event&, PenState&) {}
const char* backend() { return g_dpy ? (g_devices.empty() ? "XInput2 (no pen found)" : "XInput2") : "none"; }

#elif defined(_WIN32)

void init(SDL_Window*) { SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE); }
void poll(PenState&) {}

void syswm(const SDL_Event& event, PenState& pen) {
    const SDL_SysWMmsg* m = event.syswm.msg;
    if (!m || m->subsystem != SDL_SYSWM_WINDOWS) return;
    const UINT msg = m->msg.win.msg;
    if (msg != WM_POINTERUPDATE && msg != WM_POINTERDOWN && msg != WM_POINTERUP) return;
    const UINT32 id = GET_POINTERID_WPARAM(m->msg.win.wParam);
    POINTER_INPUT_TYPE type;
    if (!GetPointerType(id, &type) || type != PT_PEN) return;
    POINTER_PEN_INFO info;
    if (!GetPointerPenInfo(id, &info)) return;
    pen.present = true;
    pen.pressure = (info.penMask & PEN_MASK_PRESSURE) ? info.pressure / 1024.0f : 1.0f;
    pen.tilt_x = (info.penMask & PEN_MASK_TILT_X) ? static_cast<float>(info.tiltX) : 0.0f;
    pen.tilt_y = (info.penMask & PEN_MASK_TILT_Y) ? static_cast<float>(info.tiltY) : 0.0f;
    pen.eraser = (info.penFlags & (PEN_FLAG_ERASER | PEN_FLAG_INVERTED)) != 0;
    pen.last_seen = ImGui::GetTime();
}
const char* backend() { return "Windows pointer input"; }

#elif defined(__APPLE__)

// Implemented in Tablet_mac.mm.

#else

void init(SDL_Window*) {}
void poll(PenState&) {}
void syswm(const SDL_Event&, PenState&) {}
const char* backend() { return "none"; }

#endif

}  // namespace tablet
