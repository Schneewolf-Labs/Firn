#include "Drive.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include "DriveAddress.h"   // winsock2.h, which has to come before windows.h
#include <filesystem>
#include <random>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif
#include <SDL.h>
#include <SDL_opengl.h>

#include "App.h"
#include "firn/io.h"

namespace {

std::string lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

ImGuiKey key_by_name(const std::string& name) {
    const std::string want = lower(name);
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        const char* n = ImGui::GetKeyName(static_cast<ImGuiKey>(k));
        if (n && want == lower(n)) return static_cast<ImGuiKey>(k);
    }
    if (want == "ctrl" || want == "control") return ImGuiKey_LeftCtrl;
    if (want == "shift") return ImGuiKey_LeftShift;
    if (want == "alt") return ImGuiKey_LeftAlt;
    if (want == "return") return ImGuiKey_Enter;
    if (want == "esc") return ImGuiKey_Escape;
    return ImGuiKey_None;
}

double io_double_click_gap() { return ImGui::GetIO().MouseDoubleClickTime + 0.1; }

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) { if (c == sep) { if (!cur.empty()) out.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool save_framebuffer(SDL_Window* window, const std::string& path, std::string* err) {
    // The real drawable size, not GL_VIEWPORT: ImGui_ImplOpenGL3_RenderDrawData
    // restores the viewport to whatever it was before the call, which main.cpp
    // sets from the logical window size, not the (larger, on Retina) framebuffer.
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(window, &w, &h);
    if (w <= 0 || h <= 0) { if (err) *err = "empty drawable"; return false; }
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    firn::Image img(w, h);
    for (int y = 0; y < h; ++y) {
        std::memcpy(img.data() + static_cast<size_t>(y) * w * 4, px.data() + static_cast<size_t>(h - 1 - y) * w * 4, static_cast<size_t>(w) * 4);
        for (int x = 0; x < w; ++x) img.data()[(static_cast<size_t>(y) * w + x) * 4 + 3] = 255;
    }
    return firn::io::save_png(img, path, err);
}

}  // namespace

#ifdef _WIN32

namespace fs = std::filesystem;

Driver::~Driver() {
    if (client_fd_ >= 0) closesocket(static_cast<SOCKET>(client_fd_));
    if (listen_fd_ >= 0) closesocket(static_cast<SOCKET>(listen_fd_));
    if (!address_path_.empty()) { std::error_code ec; fs::remove(address_path_, ec); }
}

bool Driver::start(const std::string& path) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    // The same refusals as a Unix socket: never take over a live instance's
    // address, and never overwrite a file that is not an address at all.
    std::error_code ec;
    if (fs::exists(path, ec)) {
        int port = 0;
        std::string token;
        if (!drive_address::read(path, port, token)) {
            std::fprintf(stderr, "FIRN_DRIVE: refusing to replace a file that is not a driver address: %s\n", path.c_str());
            return false;
        }
        const SOCKET probe = drive_address::connect(path);
        if (probe != INVALID_SOCKET) {
            closesocket(probe);
            std::fprintf(stderr, "FIRN_DRIVE: socket already in use or inaccessible: %s\n", path.c_str());
            return false;
        }
    }
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    const BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;   // any free port; the address file says which
    int len = sizeof(addr);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 32) != 0 ||
        getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        std::fprintf(stderr, "FIRN_DRIVE: cannot listen on loopback: error %d\n", WSAGetLastError());
        closesocket(s);
        return false;
    }
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
    std::random_device random;   // RtlGenRandom on MSVC
    char hex[33];
    for (int i = 0; i < 4; ++i) std::snprintf(hex + i * 8, 9, "%08x", static_cast<unsigned>(random()));
    token_ = hex;
    // Written beside and renamed over, so a client never reads half a file.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << "firn-drive " << ntohs(addr.sin_port) << " " << token_ << "\n";
        out.flush();
        if (!out) ec = std::make_error_code(std::errc::io_error);
    }
    if (!ec) fs::rename(tmp, path, ec);
    if (ec) {
        std::fprintf(stderr, "FIRN_DRIVE: cannot write %s: %s\n", path.c_str(), ec.message().c_str());
        fs::remove(tmp, ec);
        closesocket(s);
        return false;
    }
    listen_fd_ = static_cast<std::intptr_t>(s);
    address_path_ = path;
    std::fprintf(stderr, "FIRN_DRIVE: listening on %s (127.0.0.1:%d)\n", path.c_str(), ntohs(addr.sin_port));
    return true;
}

void Driver::poll_socket() {
    if (client_fd_ < 0) {
        const SOCKET c = accept(static_cast<SOCKET>(listen_fd_), nullptr, nullptr);
        if (c == INVALID_SOCKET) return;
        u_long nonblocking = 1;
        ioctlsocket(c, FIONBIO, &nonblocking);
        const BOOL nodelay = TRUE;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        client_fd_ = static_cast<std::intptr_t>(c);
        authed_ = false;
    }
    char buf[4096];
    while (true) {
        const int n = recv(static_cast<SOCKET>(client_fd_), buf, sizeof(buf), 0);
        if (n > 0) inbuf_.append(buf, static_cast<size_t>(n));
        else if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) { drop_client(); return; }
        else break;
    }
    // The first line has to be the token from the address file.
    if (!authed_) {
        const size_t nl = inbuf_.find('\n');
        if (nl == std::string::npos) { if (inbuf_.size() > 256) drop_client(); return; }
        std::string line = inbuf_.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        inbuf_.erase(0, nl + 1);
        if (line != token_) { drop_client(); return; }
        authed_ = true;
    }
}

void Driver::drop_client() {
    closesocket(static_cast<SOCKET>(client_fd_));
    client_fd_ = -1;
    inbuf_.clear();
    steps_.clear();
}

void Driver::ack(const std::string& payload) {
    if (client_fd_ < 0) return;
    const std::string line = "ok " + payload + "\n";
    size_t off = 0;
    while (off < line.size()) {
        const int n = send(static_cast<SOCKET>(client_fd_), line.data() + off, static_cast<int>(line.size() - off), 0);
        if (n == SOCKET_ERROR) { if (WSAGetLastError() == WSAEWOULDBLOCK) { Sleep(1); continue; } break; }
        off += static_cast<size_t>(n);
    }
}

#else

Driver::~Driver() {
    if (client_fd_ >= 0) close(client_fd_);
    if (listen_fd_ >= 0) close(listen_fd_);
}

bool Driver::start(const std::string& path) {
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        std::fprintf(stderr, "FIRN_DRIVE: socket path is too long: %s\n", path.c_str());
        close(listen_fd_); listen_fd_ = -1; return false;
    }
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    const int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe < 0) { close(listen_fd_); listen_fd_ = -1; return false; }
    const int connected = connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    const int connect_error = errno;
    close(probe);
    if (connected == 0 || (connect_error != ENOENT && connect_error != ECONNREFUSED)) {
        std::fprintf(stderr, "FIRN_DRIVE: socket already in use or inaccessible: %s\n", path.c_str());
        close(listen_fd_); listen_fd_ = -1; return false;
    }
    if (connect_error == ECONNREFUSED) {
        struct stat st{};
        if (lstat(path.c_str(), &st) != 0 || !S_ISSOCK(st.st_mode)) {
            std::fprintf(stderr, "FIRN_DRIVE: refusing to replace a non-socket: %s\n", path.c_str());
            close(listen_fd_); listen_fd_ = -1; return false;
        }
        unlink(path.c_str());
    }
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listen_fd_, 32) != 0) {
        std::fprintf(stderr, "FIRN_DRIVE: cannot listen on %s: %s\n", path.c_str(), std::strerror(errno));
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    fcntl(listen_fd_, F_SETFL, O_NONBLOCK);
    std::fprintf(stderr, "FIRN_DRIVE: listening on %s\n", path.c_str());
    return true;
}

void Driver::poll_socket() {
    if (client_fd_ < 0) {
        client_fd_ = accept(listen_fd_, nullptr, nullptr);
        if (client_fd_ < 0) return;
        fcntl(client_fd_, F_SETFL, O_NONBLOCK);
#ifdef SO_NOSIGPIPE
        const int no_sigpipe = 1;
        setsockopt(client_fd_, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
    }
    char buf[4096];
    while (true) {
        const ssize_t n = read(client_fd_, buf, sizeof(buf));
        if (n > 0) inbuf_.append(buf, static_cast<size_t>(n));
        else if (n == 0) { drop_client(); return; }
        else break;
    }
}

void Driver::drop_client() {
    close(client_fd_);
    client_fd_ = -1;
    inbuf_.clear();
    steps_.clear();
}

void Driver::ack(const std::string& payload) {
    if (client_fd_ < 0) return;
    const std::string line = "ok " + payload + "\n";
    size_t off = 0;
    while (off < line.size()) {
        const ssize_t n = send(client_fd_, line.data() + off, line.size() - off,
#ifdef MSG_NOSIGNAL
                               MSG_NOSIGNAL
#else
                               0
#endif
        );
        if (n <= 0) { if (errno == EAGAIN) { usleep(1000); continue; } break; }
        off += static_cast<size_t>(n);
    }
}

#endif  // _WIN32

ImVec2 Driver::image_to_window(const App& app, float x, float y) const {
    if (!app.doc) return ImVec2(x, y);
    const float dw = app.doc->width() * app.zoom, dh = app.doc->height() * app.zoom;
    return ImVec2(app.canvas_center.x + app.pan_x - dw * 0.5f + x * app.zoom, app.canvas_center.y + app.pan_y - dh * 0.5f + y * app.zoom);
}

std::string Driver::state_text(App& app) const {
    std::ostringstream o;
    o << "tool=\"" << app.tool().name() << "\"";
    o << " docs=" << app.docs.size() << " windows=" << (app.image_windows ? 1 : 0);
    o << " pen=" << (app.pen.present ? 1 : 0) << " pressure=" << app.pen.pressure << " ui_scale=" << app.ui_scale << " auto_scale=" << app.auto_ui_scale << " font=\"" << (app.font_current_path.empty() ? "sans" : app.font_current_path == "builtin" ? "builtin" : app.font_current_path.substr(app.font_current_path.find_last_of("/\\") + 1)) << "\" font_size=" << app.font_current_size;
    if (app.doc) {
        o << " title=\"" << app.doc_title << "\" modified=" << (app.modified() ? 1 : 0);
        o << " size=" << app.doc->width() << "x" << app.doc->height() << " depth=" << app.doc->bit_depth() << " layers=" << app.doc->layer_count();
        const int a = app.active_layer();
        o << " active=" << a;
        if (a >= 0) {
            const firn::Layer& L = app.doc->layer(a);
            o << " active_name=\"" << L.name << "\" active_type=" << (L.is_vector() ? "vector" : L.is_adjustment() ? "adjustment" : L.type == firn::LayerType::Group ? "group" : "raster");
            if (L.is_vector()) {
                o << " objects=" << L.objects.size() << " selected=" << app.selected_objects().size();
            }
        }
        const ImVec2 origin = image_to_window(app, 0, 0);
        o << " zoom=" << app.zoom << " origin=" << origin.x << "," << origin.y;
        o << " history=" << app.history.size() << " cursor=" << app.history.cursor();
        if (app.history.cursor() > 0) o << " last=\"" << app.history.at(app.history.cursor() - 1).name() << "\"";
        o << " selection=" << (app.doc->has_selection() ? 1 : 0);
        { const firn::icc::Profile prof = app.document_profile(); o << " icc=\"" << (app.doc->icc().empty() ? "" : prof.description) << "\""; }
    }
    o << " popup=" << (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) ? 1 : 0);
    std::string status = app.status.substr(0, app.status.find('\n'));
    for (char& c : status) if (c == '"') c = '\'';
    o << " status=\"" << status << "\"";
    return o.str();
}

std::string Driver::state_json(App& app) const {
    using firn::json::Value;
    Value r = Value::object();
    r.set("tool", Value::string(app.tool().name()));
    r.set("documents", Value::number(static_cast<double>(app.docs.size())));
    r.set("popup", Value::boolean(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)));
    r.set("status", Value::string(app.status.substr(0, app.status.find('\n'))));
    if (app.doc) {
        Value d = Value::object();
        d.set("title", Value::string(app.doc_title));
        d.set("path", Value::string(app.doc_path));
        d.set("modified", Value::boolean(app.modified()));
        d.set("width", Value::number(app.doc->width()));
        d.set("height", Value::number(app.doc->height()));
        d.set("depth", Value::number(app.doc->bit_depth()));
        d.set("selection", Value::boolean(app.doc->has_selection()));
        d.set("zoom", Value::number(app.zoom));
        const ImVec2 origin = image_to_window(app, 0, 0);
        Value o = Value::array();
        o.push(Value::number(origin.x));
        o.push(Value::number(origin.y));
        d.set("origin", std::move(o));
        d.set("history", Value::number(static_cast<double>(app.history.size())));
        d.set("history_cursor", Value::number(static_cast<double>(app.history.cursor())));
        if (app.history.cursor() > 0) d.set("last", Value::string(app.history.at(app.history.cursor() - 1).name()));
        d.set("active_layer", Value::number(app.active_layer()));
        Value layers = Value::array();
        for (size_t i = 0; i < app.doc->layer_count(); ++i) {
            const firn::Layer& L = app.doc->layer(i);
            Value e = Value::object();
            e.set("index", Value::number(static_cast<double>(i)));
            e.set("name", Value::string(L.name));
            e.set("type", Value::string(L.is_vector() ? "vector" : L.is_adjustment() ? (L.adjustment.is_filter() ? "filter" : "adjustment")
                                        : L.type == firn::LayerType::Group ? "group" : "raster"));
            e.set("visible", Value::boolean(L.visible));
            e.set("opacity", Value::number(L.opacity * 100.0));
            e.set("blend", Value::string(firn::blend_mode_name(L.blend)));
            e.set("depth", Value::number(L.depth));
            e.set("mask", Value::boolean(L.has_mask()));
            e.set("clipped", Value::boolean(L.clipped));
            layers.push(std::move(e));
        }
        d.set("layers", std::move(layers));
        r.set("image", std::move(d));
    }
    return firn::json::dump(r);
}

// One text command becomes a list of per-frame steps ending in an Ack.
bool Driver::parse_line(const std::string& line, App& app) {
    std::vector<std::string> a = split(line, ' ');
    if (a.empty()) return false;
    const std::string& op = a[0];
    auto num = [&](size_t i, float def = 0.0f) { return i < a.size() ? static_cast<float>(std::atof(a[i].c_str())) : def; };
    auto point = [&](const std::string& s, bool img) {
        const auto xy = split(s, ',');
        float x = xy.size() > 0 ? static_cast<float>(std::atof(xy[0].c_str())) : 0.0f;
        float y = xy.size() > 1 ? static_cast<float>(std::atof(xy[1].c_str())) : 0.0f;
        if (img) { const ImVec2 w = image_to_window(app, x, y); x = w.x; y = w.y; }
        return ImVec2(x, y);
    };
    auto button = [&](size_t i) { if (i >= a.size()) return 0; const std::string& s = a[i]; return s == "3" || s == "right" ? 1 : s == "2" || s == "middle" ? 2 : 0; };
    auto wait = [&](int n, double secs = 0.0) { Step s{Step::Wait}; s.frames = n; s.seconds = secs; steps_.push_back(s); };
    // After a button release, wait past ImGui's double-click window so the
    // next command's press is never read as a double-click.
    auto settle = [&]() { wait(3, io_double_click_gap()); };
    auto pos = [&](ImVec2 p) { Step s{Step::MousePos}; s.x = p.x; s.y = p.y; steps_.push_back(s); };
    auto down = [&](int b) { Step s{Step::MouseDown}; s.button = b; steps_.push_back(s); };
    auto up = [&](int b) { Step s{Step::MouseUp}; s.button = b; steps_.push_back(s); };
    const bool img = op.size() > 4 && op.compare(op.size() - 4, 4, "_img") == 0;
    const std::string base = img ? op.substr(0, op.size() - 4) : op;
    if (base == "mv" && a.size() >= 3) {
        pos(point(a[1] + "," + a[2], img)); wait(1);
    } else if (base == "click" && a.size() >= 3) {
        pos(point(a[1] + "," + a[2], img)); wait(1); down(button(3)); wait(2); up(button(3)); settle();
    } else if (base == "dbl" && a.size() >= 3) {
        pos(point(a[1] + "," + a[2], img)); wait(1);
        down(0); wait(1); up(0); wait(1); down(0); wait(1); up(0); settle();
    } else if (base == "down" ) { down(button(1)); wait(2);
    } else if (base == "up") { up(button(1)); settle();
    } else if (base == "drag" && a.size() >= 3) {
        std::vector<ImVec2> pts;
        int b = 0;
        for (size_t i = 1; i < a.size(); ++i) {
            if (a[i].find(',') == std::string::npos) { b = button(i); continue; }
            pts.push_back(point(a[i], img));
        }
        if (pts.size() < 2) return false;
        pos(pts[0]); wait(2); down(b); wait(2);
        for (size_t i = 0; i + 1 < pts.size(); ++i)
            for (int k = 1; k <= 12; ++k) { const float t = k / 12.0f; pos(ImVec2(pts[i].x + (pts[i + 1].x - pts[i].x) * t, pts[i].y + (pts[i + 1].y - pts[i].y) * t)); wait(1); }
        wait(2); up(b); settle();
    } else if (op == "key" && a.size() >= 2) {
        // key NAME [ctrl] [shift] [alt]
        std::vector<ImGuiKey> mods;
        for (size_t i = 2; i < a.size(); ++i) { const ImGuiKey m = key_by_name(a[i]); if (m != ImGuiKey_None) mods.push_back(m); }
        const ImGuiKey k = key_by_name(a[1]);
        if (k == ImGuiKey_None) return false;
        // ImGui wants the modifier flag (ImGuiMod_*) as well as the physical key.
        // On macOS it swaps Cmd and Ctrl on the way in, so the shortcut a Mac
        // user reaches for is Cmd: a script asking for "ctrl" has to send
        // Super, which ImGui then turns back into Ctrl. Without this every
        // scripted shortcut silently does nothing there.
        auto mod_flag = [](ImGuiKey m) {
            return m == ImGuiKey_LeftCtrl    ? ImGuiMod_Ctrl
                   : m == ImGuiKey_LeftSuper ? ImGuiMod_Super
                   : m == ImGuiKey_LeftShift ? ImGuiMod_Shift
                   : m == ImGuiKey_LeftAlt   ? ImGuiMod_Alt
                                             : ImGuiKey_None;
        };
#ifdef __APPLE__
        for (ImGuiKey& m : mods)
            if (m == ImGuiKey_LeftCtrl) m = ImGuiKey_LeftSuper;
#endif
        for (ImGuiKey m : mods) {
            { Step s{Step::KeyDown}; s.key = m; steps_.push_back(s); }
            if (mod_flag(m) != ImGuiKey_None) { Step s{Step::KeyDown}; s.key = mod_flag(m); steps_.push_back(s); }
        }
        wait(1);
        { Step s{Step::KeyDown}; s.key = k; steps_.push_back(s); }
        wait(2);
        { Step s{Step::KeyUp}; s.key = k; steps_.push_back(s); }
        for (ImGuiKey m : mods) {
            { Step s{Step::KeyUp}; s.key = m; steps_.push_back(s); }
            if (mod_flag(m) != ImGuiKey_None) { Step s{Step::KeyUp}; s.key = mod_flag(m); steps_.push_back(s); }
        }
        wait(2);
    } else if (op == "type" && a.size() >= 2) {
        Step s{Step::Chars}; s.text = line.substr(5); steps_.push_back(s); wait(2);
    } else if (op == "wheel" && a.size() >= 2) {
        Step s{Step::Wheel}; s.y = num(1); steps_.push_back(s); wait(2);
    } else if (op == "wait" && a.size() >= 2) {
        wait(std::max(1, static_cast<int>(num(1))));
    } else if (op == "shot" && a.size() >= 2) {
        Step s{Step::Shot}; s.text = line.substr(5); steps_.push_back(s);
    } else if (op == "tool" && a.size() >= 2) {
        Step s{Step::Tool}; s.text = line.substr(5); steps_.push_back(s); wait(1);
    } else if (op == "layer" && a.size() >= 2) {
        Step s{Step::Layer}; s.text = a[1]; steps_.push_back(s); wait(1);
    } else if (op == "set" && a.size() >= 3) {
        Step s{Step::Set}; s.text = a[1]; s.x = num(2); steps_.push_back(s); wait(1);
    } else if (op == "save" && a.size() >= 2) {
        Step s{Step::Save}; s.text = line.substr(5); steps_.push_back(s); wait(1);
    } else if (op == "open" && a.size() >= 2) {
        Step s{Step::Open}; s.text = line.substr(5); steps_.push_back(s); wait(2);
    } else if (op == "drop" && a.size() >= 2) {
        // drop:PATH pushes the SDL drop event, exercising the same path as a real drag-and-drop.
        Step s{Step::Drop}; s.text = line.substr(5); steps_.push_back(s); wait(2);
    } else if (op == "do" && a.size() >= 2) {
        // do <Command> <json params>. One action belongs to the driver: a
        // screenshot has to be taken after a render, not during a command.
        if (a[1] == "app.screenshot") {
            firn::json::Value params;
            std::string path;
            if (firn::json::parse(line.substr(3 + a[1].size() + 1), params)) path = params.get("path").as_string();
            if (path.empty()) { ack("error app.screenshot needs a path"); return true; }
            Step s{Step::Shot}; s.text = path; steps_.push_back(s);
        } else {
            Step s{Step::Do}; s.text = line.substr(3); steps_.push_back(s); wait(1);
        }
    } else if (op == "profile" && a.size() >= 3) {
        // profile assign|convert|remove sRGB|AdobeRGB|ProPhoto
        Step s{Step::Profile}; s.text = a[1] + " " + a[2]; steps_.push_back(s); wait(2);
    } else if (op == "adjust" && a.size() >= 2) {
        Step s{Step::Adjust}; s.text = line.substr(7); steps_.push_back(s); wait(2);
    } else if (op == "state" || op == "state_json") {
        state_json_ = op == "state_json";
        // ack carries the state
    } else if (op == "quit") {
        steps_.push_back({Step::Quit});
    } else {
        return false;
    }
    steps_.push_back({Step::Ack});
    return true;
}

void Driver::before_frame(App& app, SDL_Window* window) {
    window_ = window;
    poll_socket();
    ImGuiIO& io = ImGui::GetIO();
    // Keep the virtual cursor authoritative: the SDL backend may have queued
    // the real pointer's position this frame; ours is added last and wins.
    if (cursor_valid_) io.AddMousePosEvent(cx_, cy_);
    // Fetch the next command when idle.
    if (steps_.empty()) {
        const size_t nl = inbuf_.find('\n');
        if (nl == std::string::npos) return;
        std::string line = inbuf_.substr(0, nl);
        inbuf_.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) return;
        if (!parse_line(line, app)) { ack("error unknown command: " + line); return; }
    }
    // Run one step (Wait consumes frames).
    while (!steps_.empty()) {
        Step& s = steps_.front();
        bool consumed_frame = false;
        switch (s.kind) {
            case Step::MousePos: cx_ = s.x; cy_ = s.y; cursor_valid_ = true; io.AddMousePosEvent(cx_, cy_); consumed_frame = true; break;
            case Step::MouseDown: io.AddMouseButtonEvent(s.button, true); consumed_frame = true; break;
            case Step::MouseUp: io.AddMouseButtonEvent(s.button, false); consumed_frame = true; break;
            case Step::KeyDown: io.AddKeyEvent(s.key, true); consumed_frame = true; break;
            case Step::KeyUp: io.AddKeyEvent(s.key, false); consumed_frame = true; break;
            case Step::Chars: io.AddInputCharactersUTF8(s.text.c_str()); consumed_frame = true; break;
            case Step::Wheel: io.AddMouseWheelEvent(0.0f, s.y); consumed_frame = true; break;
            case Step::Wait: {
                const double now = SDL_GetTicks64() / 1000.0;
                if (s.deadline == 0.0) s.deadline = now + s.seconds;
                if (--s.frames > 0 || now < s.deadline) { if (s.frames <= 0) s.frames = 1; return; }
                consumed_frame = true;
                break;
            }
            case Step::Shot: pending_shot_ = s.text; shot_after_render_ = true; steps_.pop_front(); return;   // ack after the render
            case Step::Tool: {
                bool found = false;
                for (size_t i = 0; i < app.tools.size(); ++i) if (s.text == app.tools[i]->name()) { app.select_tool(static_cast<int>(i)); found = true; }
                if (!found) { steps_.clear(); ack("error no tool named " + s.text); return; }
                consumed_frame = true;
                break;
            }
            case Step::Layer: if (app.doc) app.doc->set_active_layer(std::atoi(s.text.c_str())); consumed_frame = true; break;
            case Step::Set: {
                // Tool options that scripts set directly instead of hunting for the widget.
                const std::string& n = s.text;
                const float v = s.x;
                if (n == "material_dialog") app.open_material_dialog(v < 2);   // 1 foreground, 2 background
                else if (n == "theme_editor") app.open_theme_editor();
                else if (n == "shortcuts_dialog") app.show_shortcuts_dialog = true;
                else if (n == "brush_size") app.brush.size = v;
                else if (n == "smooth_mode") app.smooth_mode = static_cast<int>(v);
                else if (n == "smooth_amount") app.smooth_amount = v;
                else if (n == "fgsel_size") app.fgsel_size = static_cast<int>(v);
                else if (n == "layer_styles") app.open_layer_styles(app.active_layer());
                else if (n == "style_shadow" || n == "style_glow" || n == "style_inner_glow" || n == "style_stroke" || n == "style_bevel") {
                    // Toggle one effect with its defaults on the active layer (no history; for tests).
                    if (app.doc && app.active_layer() >= 0) {
                        firn::LayerStyle& st = app.doc->layer(app.active_layer()).style;
                        if (n == "style_shadow") st.drop_shadow = v != 0; else if (n == "style_glow") st.outer_glow = v != 0; else if (n == "style_inner_glow") st.inner_glow = v != 0;
                        else if (n == "style_stroke") st.stroke = v != 0; else st.bevel = v != 0;
                        app.doc->touch();
                    }
                }
                else if (n == "assistant_kind") app.assistant_kind = static_cast<int>(v);
                else if (n == "assistant_choice") app.assistant_choice = static_cast<int>(v);
                else if (n == "filter_layer") app.layer_new_adjustment(static_cast<firn::Adjustment::Kind>(static_cast<int>(v)));
                else if (n == "assistant_snap") app.assistant_snap = v != 0;
                else if (n == "csmudge_rate") app.csmudge_rate = static_cast<int>(v);
                else if (n == "csmudge_mode") app.csmudge_mode = static_cast<int>(v);
                else if (n == "csmudge_length") app.csmudge_length = static_cast<int>(v);
                else if (n == "symmetry_mode") app.symmetry_mode = static_cast<int>(v);
                else if (n == "symmetry_count") app.symmetry_count = static_cast<int>(v);
                else if (n == "symmetry_x") app.symmetry_x = v;
                else if (n == "symmetry_y") app.symmetry_y = v;
                else if (n == "pen_pressure") { app.pen.present = v >= 0; app.pen.pressure = std::clamp(v, 0.0f, 1.0f); app.pen.last_seen = ImGui::GetTime() + 1e6; }
                else if (n == "pen_eraser") { app.pen.present = true; app.pen.eraser = v != 0; app.pen.last_seen = ImGui::GetTime() + 1e6; }
                else if (n == "ui_scale") { app.config.ui_scale = v; app.apply_theme(app.config.theme); }
                else if (n == "autosave_now") { app.config.autosave_minutes = 1; app.autosave_last = -1e9; }
                else if (n == "effect_browser") { app.reset_effect_browser(); app.show_effect_browser = v != 0; }
                else if (n == "theme_index") { app.ensure_themes(); const int i = static_cast<int>(v); if (i >= 0 && i < static_cast<int>(app.themes.size())) { app.config.theme = app.themes[i].name; app.apply_theme(app.config.theme); } }
                else if (n == "sel_type") app.sel_freehand_type = static_cast<int>(v);
                else if (n == "sel_shape") app.sel_shape = static_cast<int>(v);
                else if (n == "sel_range") app.sel_range = static_cast<int>(v);
                else if (n == "sel_smoothing") app.sel_smoothing = static_cast<int>(v);
                else if (n == "sel_dialog") app.show_sel_dialog = static_cast<int>(v);
                else if (n == "selection_edit") app.set_selection_edit(v != 0);
                else if (n == "material_kind") { app.fg_material.kind = static_cast<int>(v); if (v == 1) { app.fg_material.gradient_index = -1; } }
                else if (n == "material_gradient") { app.ensure_gradients(); const int i = static_cast<int>(v); if (i >= 0 && i < static_cast<int>(app.gradient_library.size())) { app.fg_material.kind = 1; app.fg_material.gradient_index = i; app.fg_material.gradient = app.gradient_library[i]; } }
                else if (n == "material_texture") { const int i = static_cast<int>(v); app.fg_material.texture = app.texture_image(i); app.fg_material.texture_index = i; app.fg_material.texture_on = app.fg_material.texture != nullptr; }
                else if (n == "material_transparent") app.fg_material.transparent = v != 0;
                else if (n == "bg_transparent") app.bg_material.transparent = v != 0;
                else if (n == "material_view") app.material_view = static_cast<int>(v);
                else if (n == "image_windows") { app.image_windows = v != 0; if (app.image_windows) app.arrange_request = App::Arrange::Cascade; }
                else if (n == "arrange") app.arrange_request = static_cast<App::Arrange>(static_cast<int>(v));  // 1 cascade, 2 tile horizontally, 3 tile vertically
                else if (n == "create_as_vector") app.create_as_vector = v != 0;
                else if (n == "shape_fill") app.shape_fill = v != 0;
                else if (n == "shape_stroke") app.shape_stroke = v != 0;
                else if (n == "shape_antialias") app.shape_antialias = v != 0;
                else if (n == "shape_kind") { app.shape_kind = static_cast<int>(v); app.shape_library_index = -1; }
                else if (n == "shape_library") { app.ensure_shape_library(); app.shape_library_index = static_cast<int>(v); }
                else if (n == "line_width") app.line_width = v;
                else if (n == "line_style") { app.ensure_line_styles(); app.line_index = static_cast<int>(v); }
                else if (n == "pen_mode") app.pen_mode = static_cast<int>(v);
                else if (n == "pen_close") app.pen_close = v != 0;
                else if (n == "fg_kind") { app.ensure_gradients(); app.fg_material.kind = static_cast<int>(v); }
                else if (n == "bg_kind") { app.ensure_gradients(); app.bg_material.kind = static_cast<int>(v); }
                else if (n == "fg_gradient") { app.ensure_gradients(); app.fg_material.gradient_index = static_cast<int>(v); if (v >= 0 && v < app.gradient_library.size()) app.fg_material.gradient = app.gradient_library[static_cast<size_t>(v)]; }
                else if (n == "bg_gradient") { app.ensure_gradients(); app.bg_material.gradient_index = static_cast<int>(v); if (v >= 0 && v < app.gradient_library.size()) app.bg_material.gradient = app.gradient_library[static_cast<size_t>(v)]; }
                else if (n == "bg_gradient_angle") app.bg_material.gradient_angle = v;
                else if (n == "fg_gradient_angle") app.fg_material.gradient_angle = v;
                else if (n == "text_size") app.text_size = v;
                else if (n == "text_stroke") app.text_stroke = v;
                else { steps_.clear(); ack("error unknown setting " + n); return; }
                consumed_frame = true;
                break;
            }
            case Step::Quit:
                // There is no next frame to process the trailing Ack.
                // Confirm the request before shutting the socket down.
                app.quit = true;
                steps_.clear();
                ack("result {\"ok\":true}");
                return;
            case Step::Do: {
                const size_t sp = s.text.find(' ');
                const std::string cmd = s.text.substr(0, sp);
                firn::json::Value params = firn::json::Value::object();
                std::string perr;
                if (sp != std::string::npos && !firn::json::parse(s.text.substr(sp + 1), params, &perr)) { steps_.clear(); ack("error bad json: " + perr); return; }
                bool okc = true;
                const std::string result = app.do_command(cmd, params, &okc);
                steps_.clear();
                ack(okc ? "result " + result : "error " + result);
                return;
            }
            case Step::Profile: {
                const std::string action = s.text.substr(0, s.text.find(' ')), which = s.text.substr(s.text.find(' ') + 1);
                const firn::icc::Profile prof = which == "AdobeRGB" ? firn::icc::adobe_rgb() : which == "ProPhoto" ? firn::icc::prophoto_rgb() : firn::icc::srgb();
                const std::vector<uint8_t> bytes = firn::icc::encode(prof, prof.description);
                if (action == "assign") app.assign_profile(bytes, "Assign Profile");
                else if (action == "convert") app.convert_to_profile(prof, bytes, "Convert to Profile");
                else app.assign_profile({}, "Remove Profile");
                consumed_frame = true;
                break;
            }
            case Step::Adjust: if (!app.open_adjust_by_title(s.text.c_str())) { steps_.clear(); ack("error no dialog titled " + s.text); return; } consumed_frame = true; break;
            case Step::Save: app.pending_jpeg_path = s.text;  /* no quality prompt while driving */ if (!app.save_document(s.text)) { steps_.clear(); ack("error " + app.status); return; } consumed_frame = true; break;
            case Step::Open: if (!app.open_document(s.text)) { steps_.clear(); ack("error " + app.status); return; } consumed_frame = true; break;
            case Step::Drop: {
                SDL_Event e{};
                e.type = SDL_DROPFILE;
                e.drop.windowID = SDL_GetWindowID(window);
                e.drop.file = static_cast<char*>(SDL_malloc(s.text.size() + 1));
                std::memcpy(e.drop.file, s.text.c_str(), s.text.size() + 1);
                SDL_PushEvent(&e);
                consumed_frame = true;
                break;
            }
            case Step::Ack: steps_.pop_front(); ack(state_json_ ? state_json(app) : state_text(app)); state_json_ = false; continue;
        }
        steps_.pop_front();
        if (consumed_frame) return;
    }
}

void Driver::after_render(App& app) {
    if (!shot_after_render_) return;
    shot_after_render_ = false;
    std::string err;
    if (!save_framebuffer(window_, pending_shot_, &err)) { steps_.clear(); ack("error screenshot: " + err); return; }
    (void)app;
}

void Driver::draw_cursor() {
    if (!cursor_valid_) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 p(cx_, cy_);
    const ImVec2 tri[3] = {p, ImVec2(p.x + 12, p.y + 6), ImVec2(p.x + 6, p.y + 12)};
    dl->AddTriangleFilled(tri[0], tri[1], tri[2], IM_COL32(255, 255, 255, 230));
    dl->AddTriangle(tri[0], tri[1], tri[2], IM_COL32(0, 0, 0, 255), 1.0f);
    dl->AddLine(p, ImVec2(p.x + 16, p.y + 16), IM_COL32(0, 0, 0, 255), 2.0f);
}
