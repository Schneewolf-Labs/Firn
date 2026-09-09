// PSP9: SDL2 + OpenGL3 + Dear ImGui (docking) bootstrap.
#include <cstdio>

#include <SDL.h>
#include <SDL_opengl.h>

#include "App.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"  // DockBuilder
#include "imgui_impl_sdl2.h"

// PSP9-style default workspace, applied only when no imgui.ini layout exists:
//   Tools strip | Tool Options across the top, Image centre | Materials/Overview
//   over Layers/History on the right.
static void build_default_layout(ImGuiID dockspace_id) {
    ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace_id);
    if (node && !node->IsLeafNode()) return;  // layout restored from imgui.ini

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, vp->WorkSize);

    ImGuiID centre = dockspace_id;
    ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.08f, nullptr, &centre);
    ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.22f, nullptr, &centre);
    ImGuiID top = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Up, 0.10f, nullptr, &centre);
    ImGuiID right_bottom = 0;
    ImGuiID right_top = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.45f, nullptr, &right_bottom);

    ImGui::DockBuilderDockWindow("Tools", left);
    ImGui::DockBuilderDockWindow("Tool Options", top);
    ImGui::DockBuilderDockWindow("Image", centre);
    ImGui::DockBuilderDockWindow("Materials", right_top);
    ImGui::DockBuilderDockWindow("Overview", right_top);
    ImGui::DockBuilderDockWindow("Layers", right_bottom);
    ImGui::DockBuilderDockWindow("History", right_bottom);
    ImGui::DockBuilderFinish(dockspace_id);
}

int main(int argc, char** argv) {
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

    auto flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_MAXIMIZED);
    SDL_Window* window = SDL_CreateWindow("Paint Shop Pro 9", SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED, 1400, 900, flags);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    App app;
    bool first_frame = true;
    if (argc > 1) {
        if (!app.open_document(argv[1])) std::fprintf(stderr, "%s\n", app.status.c_str());
    }

    while (!app.quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) app.quit = true;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window))
                app.quit = true;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        app.handle_shortcuts();
        app.sync_canvas_texture();

        const ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        if (first_frame) {
            first_frame = false;
            build_default_layout(dockspace_id);
        }
        app.draw_menu();
        app.draw_canvas();
        app.draw_palettes();
        app.draw_dialogs();
        if (app.show_imgui_demo) ImGui::ShowDemoWindow(&app.show_imgui_demo);

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.16f, 0.16f, 0.16f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    if (app.canvas_tex) glDeleteTextures(1, &app.canvas_tex);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
