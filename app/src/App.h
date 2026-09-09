#pragma once
#include <memory>
#include <string>

#include <SDL_opengl.h>

#include "psp9/commands.h"
#include "psp9/document.h"

// Application state shared by all UI panels. The UI is immediate-mode: every
// frame it reads this and the Document and emits Commands. Nothing in the UI
// owns pixels.
struct App {
    // Document
    std::unique_ptr<psp9::Document> doc;
    psp9::CommandStack history;
    std::string doc_path;

    // Canvas view
    GLuint canvas_tex = 0;
    uint64_t canvas_tex_revision = ~0ull;  // revision the texture was built from
    float zoom = 1.0f;
    float pan_x = 0.0f, pan_y = 0.0f;  // canvas offset in screen px, relative to view centre
    bool fit_requested = true;

    // Tool state (placeholder until real tools land)
    enum class Tool { Pan, Zoom, Select, Paint, Eraser, Fill, Text };
    Tool tool = Tool::Pan;
    float brush_size = 16.0f;
    float fg_color[4] = {0.f, 0.f, 0.f, 1.f};
    float bg_color[4] = {1.f, 1.f, 1.f, 1.f};

    // Dialog state
    bool show_open_dialog = false;
    bool show_save_dialog = false;
    bool show_new_dialog = false;
    bool show_blur_dialog = false;
    bool show_imgui_demo = false;
    char path_buf[1024] = {};
    int new_w = 800, new_h = 600;
    int blur_radius = 3;
    std::string status;
    bool quit = false;

    // Actions (implemented in App.cpp)
    void new_document(int w, int h);
    bool open_document(const std::string& path);
    bool save_document_png(const std::string& path);
    void run(std::unique_ptr<psp9::Command> cmd);
    void undo();
    void redo();
    int active_layer() const;

    // Per-frame UI (ui/*.cpp)
    void draw_menu();
    void draw_canvas();
    void draw_palettes();
    void draw_dialogs();
    void handle_shortcuts();

    // Keeps the GL texture in sync with the document's composite.
    void sync_canvas_texture();
};
