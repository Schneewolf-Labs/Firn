#pragma once
#include <memory>
#include <string>
#include <vector>

#include <SDL_opengl.h>

#include "imgui.h"
#include "firn/commands.h"
#include "firn/document.h"
#include "firn/raster.h"
#include "tools/Tool.h"

// Application state shared by all UI panels. The UI is immediate-mode: every
// frame it reads this and the Document and emits Commands. Nothing in the UI
// owns pixels.
struct App {
    App();

    // Document
    std::unique_ptr<firn::Document> doc;
    firn::CommandStack history;
    std::string doc_path;

    // Canvas view
    GLuint canvas_tex = 0;
    uint64_t canvas_tex_revision = ~0ull;  // revision the texture was built from
    float zoom = 1.0f;
    float pan_x = 0.0f, pan_y = 0.0f;  // canvas offset in screen px, relative to view centre
    bool fit_requested = true;
    ImVec2 canvas_centre;               // view centre in screen space, updated by draw_canvas

    // Tools
    std::vector<std::unique_ptr<Tool>> tools;
    int tool_index = 0;
    int active_button = -1;             // mouse button of the gesture in progress, or -1
    firn::raster::Brush brush;
    int fill_tolerance = 20;
    float fill_opacity = 1.0f;
    float fg_color[4] = {0.f, 0.f, 0.f, 1.f};
    float bg_color[4] = {1.f, 1.f, 1.f, 1.f};

    // Dialog state
    bool show_open_dialog = false;
    bool show_save_dialog = false;
    bool show_new_dialog = false;
    bool show_blur_dialog = false;
    bool blur_gaussian = false;         // which blur the pending dialog is for
    bool show_bc_dialog = false;
    bool show_imgui_demo = false;
    char path_buf[1024] = {};
    int new_w = 800, new_h = 600;
    float blur_radius = 3.0f;
    int bc_brightness = 0, bc_contrast = 0;
    std::string status;
    bool quit = false;

    // Actions (implemented in App.cpp)
    void new_document(int w, int h);
    bool open_document(const std::string& path);
    bool save_document_png(const std::string& path);
    void run(std::unique_ptr<firn::Command> cmd);       // execute and record
    void commit(std::unique_ptr<firn::Command> cmd);    // record an already-applied edit
    void undo();
    void redo();
    int active_layer() const;
    Tool& tool() { return *tools[tool_index]; }
    void select_tool(int index);
    void zoom_about(ImVec2 screen, float factor);

    // Per-frame UI (ui/*.cpp)
    void draw_menu();
    void draw_canvas();
    void draw_palettes();
    void draw_dialogs();
    void handle_shortcuts();

    // Keeps the GL texture in sync with the document's composite.
    void sync_canvas_texture();
};
