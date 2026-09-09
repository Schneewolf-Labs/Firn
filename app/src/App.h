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
#include "ui/FileDialog.h"

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
    // Selection tool options (shared by Selection / Freehand / Magic Wand)
    int sel_shape = 0;                  // 0 rectangle, 1 ellipse
    int sel_mode = 0;                   // mask::Combine as int: 0 replace, 1 add, 2 subtract, 3 intersect
    float sel_feather = 0.0f;
    bool sel_antialias = true;
    int wand_tolerance = 20;
    bool wand_contiguous = true;
    bool wand_sample_merged = false;

    // Marching ants: unit edges of the selection outline, cached per selection revision.
    struct Edge { int x, y; bool horizontal; };
    std::vector<Edge> ants;
    uint64_t ants_revision = ~0ull;

    // Internal clipboard: document-sized RGBA with the selection baked into alpha.
    struct Clipboard { firn::Image pixels; firn::raster::Rect bounds; bool empty() const { return pixels.empty(); } };
    Clipboard clipboard;
    float fg_color[4] = {0.f, 0.f, 0.f, 1.f};
    float bg_color[4] = {1.f, 1.f, 1.f, 1.f};

    // Dialog state
    FileDialog file_dialog;
    enum class PendingFileOp { None, Open, SaveAs };
    PendingFileOp file_op = PendingFileOp::None;
    bool show_new_dialog = false;
    bool show_blur_dialog = false;
    bool blur_gaussian = false;         // which blur the pending dialog is for
    bool show_bc_dialog = false;
    int show_sel_dialog = 0;            // 1 expand, 2 contract, 3 feather
    bool show_layer_props_dialog = false;
    firn::LayerProps layer_props_edit;  // dialog working copy
    firn::LayerProps layer_props_before; // props at the start of a live slider drag
    bool show_imgui_demo = false;
    int new_w = 800, new_h = 600;
    float blur_radius = 3.0f;
    int bc_brightness = 0, bc_contrast = 0;
    int sel_modify_px = 1;
    std::string status;
    bool quit = false;

    // Actions (implemented in App.cpp)
    void new_document(int w, int h);
    bool open_document(const std::string& path);
    bool save_document(const std::string& path);
    void request_open();
    void request_save_as();
    void save();  // to doc_path, or Save As when there is none
    void run(std::unique_ptr<firn::Command> cmd);       // execute and record
    void commit(std::unique_ptr<firn::Command> cmd);    // record an already-applied edit
    void undo();
    void redo();
    int active_layer() const;
    Tool& tool() { return *tools[tool_index]; }
    void select_tool(int index);
    void zoom_about(ImVec2 screen, float factor);

    // Selections and clipboard
    void set_selection(const char* name, firn::Mask m);    // runs a SelectionCommand
    void apply_selection_gesture(const char* name, firn::Mask shape);  // combine per sel_mode + feather
    void select_all();
    void select_none();
    void select_invert();
    void copy();
    void cut();
    void clear_selection();
    void paste_as_new_layer();
    void paste_as_new_image();
    void sync_ants();

    // Layers
    void layer_new();
    void layer_duplicate();
    void layer_delete();
    void layer_arrange(int delta);  // +1 up (towards top), -1 down; large values go to top/bottom
    void layer_merge(int kind);     // 0 down, 1 visible, 2 all
    void layer_promote_background();
    void layer_set_props(const firn::LayerProps& before, const firn::LayerProps& after);
    void open_layer_properties();

    // Per-frame UI (ui/*.cpp)
    void draw_menu();
    void draw_canvas();
    void draw_palettes();
    void draw_dialogs();
    void handle_shortcuts();

    // Keeps the GL texture in sync with the document's composite.
    void sync_canvas_texture();
};
