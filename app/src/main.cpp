// Firn: SDL2 + OpenGL3 + Dear ImGui (docking) bootstrap.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#include <SDL.h>
#include <SDL_opengl.h>
#ifdef _WIN32
#include <windows.h>  // SDL_opengl.h needs it first on Windows; NOMINMAX is set project-wide
#endif

#include "App.h"
#include "Drive.h"
#include "Config.h"
#include "firn/io.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"  // DockBuilder
#include "imgui_impl_sdl2.h"

// Set in one place so the About box, window title, and docs stay in sync.
static const char* kAppTitle = "Firn";

// Default workspace, applied only when no imgui.ini layout exists:
//   Tools strip | Tool Options across the top, Image center | Materials/Overview
//   over Layers/History on the right.
static void build_default_layout(ImGuiID dockspace_id) {
    ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace_id);
    if (node && !node->IsLeafNode()) return;  // layout restored from imgui.ini

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(vp->WorkSize.x, vp->WorkSize.y - App::toolbar_height - App::status_height));

    ImGuiID center = dockspace_id;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.12f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.22f, nullptr, &center);
    ImGuiID top = ImGui::DockBuilderSplitNode(center, ImGuiDir_Up, 0.10f, nullptr, &center);
    ImGuiID right_bottom = 0;
    ImGuiID right_top = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.45f, nullptr, &right_bottom);

    ImGui::DockBuilderDockWindow("Tools", left);
    ImGui::DockBuilderDockWindow("Tool Options", top);
    ImGui::DockBuilderDockWindow("Image", center);
    ImGui::DockBuilderDockWindow("Materials", right_top);
    ImGui::DockBuilderDockWindow("Overview", right_top);
    ImGui::DockBuilderDockWindow("Layers", right_bottom);
    ImGui::DockBuilderDockWindow("History", right_bottom);
    ImGui::DockBuilderFinish(dockspace_id);
}

// The icon is embedded at build time from assets/icon-128.png (see
// app/CMakeLists.txt); Windows builds also carry it as an exe resource.
extern const unsigned char kFirnIconPng[];
extern const size_t kFirnIconPng_size;

static void set_window_icon(SDL_Window* window) {
    std::string err;
    auto icon = firn::io::load_memory(kFirnIconPng, kFirnIconPng_size, &err);
    if (!icon) { std::fprintf(stderr, "window icon: %s\n", err.c_str()); return; }
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(icon->data(), icon->width(), icon->height(), 32, icon->width() * 4, SDL_PIXELFORMAT_RGBA32);
    if (!s) return;
    SDL_SetWindowIcon(window, s);
    SDL_FreeSurface(s);
}

int main(int argc, char** argv) {
#ifdef SDL_MAIN_HANDLED
    SDL_SetMainReady();
#endif
    // Leave SIGINT/SIGTERM to the OS: SDL would otherwise convert them into a
    // quit request, and a modified document would sit on the unsaved-changes
    // prompt instead of the process ending.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    const char* glsl_version = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    auto flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    // Window size is overridable for driving the app from scripts/screenshots.
    Config boot;
    boot.load();
    int win_w = boot.window_w, win_h = boot.window_h;
    if (const char* e = std::getenv("FIRN_WINDOW")) std::sscanf(e, "%dx%d", &win_w, &win_h);
    SDL_Window* window = SDL_CreateWindow(kAppTitle, SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED, win_w, win_h, flags);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    set_window_icon(window);
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    const std::string ini_path = Config::layout_path();
    io.IniFilename = ini_path.c_str();
    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    App app;
    // HiDPI: the drawable-to-window ratio (macOS, Wayland) or the display DPI (X11, Windows).
    {
        int ww = 1, wh = 1, dw = 1, dh = 1;
        SDL_GetWindowSize(window, &ww, &wh);
        SDL_GL_GetDrawableSize(window, &dw, &dh);
        float scale = ww > 0 ? static_cast<float>(dw) / static_cast<float>(ww) : 1.0f;
        const char* driver = SDL_GetCurrentVideoDriver();
        const bool x11 = driver && std::strcmp(driver, "x11") == 0;
        float ddpi = 0.0f;
        // X11 reports the monitor's physical DPI, which says nothing about the
        // desktop's scale; there the toolkit scale variables decide instead.
        if (scale <= 1.01f && !x11 && SDL_GetDisplayDPI(SDL_GetWindowDisplayIndex(window), &ddpi, nullptr, nullptr) == 0 && ddpi > 0.0f) scale = ddpi / 96.0f;
        if (scale <= 1.01f) {
            if (const char* e = std::getenv("GDK_SCALE")) scale = static_cast<float>(std::atof(e));
            else if (const char* q = std::getenv("QT_SCALE_FACTOR")) scale = static_cast<float>(std::atof(q));
        }
#if !defined(_WIN32) && !defined(__APPLE__)
        if (scale <= 1.01f && x11) {
            // Xft.dpi is what X11 desktops set when the user picks a scale.
            if (FILE* p = popen("xrdb -query 2>/dev/null", "r")) {
                char line[256];
                while (std::fgets(line, sizeof(line), p))
                    if (std::strncmp(line, "Xft.dpi:", 8) == 0) { const float dpi = static_cast<float>(std::atof(line + 8)); if (dpi > 0) scale = dpi / 96.0f; }
                pclose(p);
            }
        }
#endif
        if (const char* e = std::getenv("FIRN_UI_SCALE")) scale = static_cast<float>(std::atof(e));
        scale = std::round(scale * 4.0f) / 4.0f;   // quarter steps: 1, 1.25, 1.5, ...
        if (scale > 1.01f) app.set_auto_ui_scale(scale);
    }
    Driver driver;
    if (const char* sock = std::getenv("FIRN_DRIVE")) driver.start(sock);
    bool first_frame = true;
    if (argc > 1) {
        if (!app.open_document(argv[1])) std::fprintf(stderr, "%s\n", app.status.c_str());
    }
    app.check_recovery();
    app.autosave_last = 0.0;

    while (!app.quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            // While scripted, the real pointer must not reach the UI.
            if (driver.active() && (event.type == SDL_MOUSEMOTION || event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP || event.type == SDL_MOUSEWHEEL)) continue;
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) app.request_quit();
            // Files dropped onto the window open as documents (one per file).
            if (event.type == SDL_DROPFILE && event.drop.file) {
                if (!app.open_document(event.drop.file)) std::fprintf(stderr, "%s\n", app.status.c_str());
                SDL_free(event.drop.file);
            }
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window))
                app.request_quit();
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        app.apply_pending_font();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        if (driver.active()) driver.before_frame(app, window);
        ImGui::NewFrame();

        app.handle_shortcuts();
        app.autosave_tick();
        app.sync_canvas_texture();

        // Toolbar above and status bar below the dock space.
        app.draw_toolbar();
        app.draw_status_bar();
        ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImVec2 dock_pos(vp->WorkPos.x, vp->WorkPos.y + App::toolbar_height);
        const ImVec2 dock_size(vp->WorkSize.x, vp->WorkSize.y - App::toolbar_height - App::status_height);
        ImGui::SetNextWindowPos(dock_pos);
        ImGui::SetNextWindowSize(dock_size);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##dockhost", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar(3);
        const ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
        ImGui::DockSpace(dockspace_id, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::End();
        if (first_frame) {
            first_frame = false;
            build_default_layout(dockspace_id);
        }
        app.draw_menu();
        app.draw_canvas();
        app.draw_palettes();
        app.draw_dialogs();
        if (app.show_imgui_demo) ImGui::ShowDemoWindow(&app.show_imgui_demo);
        if (driver.active()) driver.draw_cursor();

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.16f, 0.16f, 0.16f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (driver.active()) driver.after_render(app);
        SDL_GL_SwapWindow(window);
    }

    if (!std::getenv("FIRN_WINDOW")) SDL_GetWindowSize(window, &app.config.window_w, &app.config.window_h);
    app.config.show_rulers = app.show_rulers;
    app.config.show_grid = app.show_grid;
    app.config.grid_spacing = app.grid_spacing;
    app.config.jpeg_quality = app.jpeg_quality;
    app.config.save();
    if (app.canvas_tex) glDeleteTextures(1, &app.canvas_tex);
    if (app.overlay_tex) glDeleteTextures(1, &app.overlay_tex);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
