#pragma once
#include <functional>
#include <memory>
#include <array>
#include <string>
#include <vector>

#include <SDL_opengl.h>

#include "imgui.h"
#include "firn/adjust.h"
#include "firn/commands.h"
#include "firn/document.h"
#include "firn/raster.h"
#include "firn/text.h"
#include "Config.h"
#include "tools/Tool.h"
#include "ui/FileDialog.h"

// Application state shared by all UI panels. The UI is immediate-mode: every
// frame it reads this and the Document and emits Commands. Nothing in the UI
// owns pixels.
// Everything that belongs to one open image. The current document's copy
// lives directly in App's members; inactive ones are parked in App::docs.
struct DocState {
    std::unique_ptr<firn::Document> doc;
    firn::CommandStack history;
    std::string doc_path;
    std::string title;
    size_t saved_cursor = 0;
    float zoom = 1.0f, pan_x = 0.0f, pan_y = 0.0f;
    bool fit_requested = true;
    firn::raster::Rect crop_rect;
    std::vector<float> guides_h, guides_v;  // image-space y / x positions
};

struct App {
    App();

    // Document (the current one; see DocState)
    std::unique_ptr<firn::Document> doc;
    firn::CommandStack history;
    std::string doc_path;
    std::string doc_title;
    size_t saved_cursor = 0;
    std::vector<DocState> docs;         // one slot per open image; docs[current_doc].doc is null (it lives above)
    int current_doc = -1;
    int untitled_counter = 0;
    int select_tab_request = -1;        // tab index to select on the next frame
    int pending_close = -1;             // document awaiting the unsaved-changes prompt
    bool pending_quit = false;
    bool modified() const { return doc && history.cursor() != saved_cursor; }
    void stash_current();               // App members -> docs[current_doc]
    void activate_document(int index);
    void add_document(std::unique_ptr<firn::Document> d, const std::string& path);
    void close_document(int index, bool force = false);
    void request_quit();
    std::string document_title(int index) const;
    bool document_modified(int index) const;

    Config config;
    bool show_rulers = true, show_grid = false, show_guides = true;
    bool snap_to_guides = true, snap_to_grid = false;
    int grid_spacing = 10;
    std::vector<float> guides_h, guides_v;  // current document's guides (image coords)
    // Guide being dragged: kind 0 none, 1 horizontal, 2 vertical; index -1 = new
    int guide_drag_kind = 0, guide_drag_index = -1;
    void snap_point(float& x, float& y) const;

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
    int retouch_amount = 20;            // % per stroke for lighten/darken, saturation, hue
    bool clone_aligned = true, clone_sample_merged = false;
    int replacer_tolerance = 30;
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
    enum class PendingFileOp { None, Open, SaveAs, LoadSelection, SaveSelection };
    PendingFileOp file_op = PendingFileOp::None;
    bool show_new_dialog = false;
    // Adjustment / effect dialogs with live preview (ui/Adjust.cpp)
    enum class Adj { None, BrightnessContrast, Curves, Gamma, Levels, Threshold, ChannelMixer, Colorize, HSL,
                     Average, Gaussian, Posterize, Solarize, UnsharpMask, Median, MotionBlur, Mosaic, AddNoise, DropShadow,
                     ColorBalance, Sepia, HueMap, Wave, Pinch, Twirl, Buttonize, InnerBevel, Cutout };
    Adj open_adjust = Adj::None;
    struct Preview {
        bool active = false;
        size_t layer = 0;
        std::string name;
        firn::Image before;
        std::array<int, 256> histogram{};
        bool dirty = true;
        bool live = true;               // small layer: re-apply on every change
    } preview;
    void preview_begin(const char* name);
    void preview_update(const std::function<void(firn::Image&)>& op, bool force = false);
    void preview_commit();
    void preview_cancel();
    void draw_adjust_dialogs();
    // Parameters, remembered between uses like the original's dialogs.
    int colorize_hue = 0, colorize_sat = 128;
    int hsl_h = 0, hsl_s = 0, hsl_l = 0;
    int lv_in_lo = 0, lv_in_hi = 255, lv_out_lo = 0, lv_out_hi = 255;
    float lv_gamma = 1.0f, gamma_value = 1.0f;
    int threshold_value = 128, posterize_levels = 6, solarize_threshold = 128;
    firn::adjust::ChannelMix mixer;
    int mixer_row = 0;
    std::vector<std::pair<float, float>> curve_points{{0, 0}, {255, 255}};
    int curve_drag = -1;
    float usm_radius = 2.0f; int usm_strength = 100, usm_clipping = 0;
    int median_radius = 1;
    float motion_angle = 0.0f; int motion_strength = 10;
    int mosaic_w = 8, mosaic_h = 8; bool mosaic_square = true;
    int noise_percent = 20; bool noise_gaussian = false, noise_mono = false;
    int shadow_x = 5, shadow_y = 5; float shadow_opacity = 0.5f, shadow_blur = 5.0f; float shadow_color[3] = {0, 0, 0};
    firn::adjust::ColorBalance color_balance; int cb_range = 1;
    int sepia_amount = 50;
    firn::adjust::HueMap hue_map_params;
    float wave_ha = 5, wave_hw = 40, wave_va = 0, wave_vw = 40;
    int pinch_strength = 50;
    float twirl_degrees = 90;
    int button_width = 10; float button_opacity = 0.75f; float button_color[3] = {0.5f, 0.5f, 0.5f}; bool button_transparent = false;
    int bevel_width = 10; float bevel_angle = 315, bevel_depth = 1.0f, bevel_ambient = 1.0f;
    int cutout_x = 5, cutout_y = 5; float cutout_opacity = 0.6f, cutout_blur = 5; float cutout_color[3] = {0, 0, 0};
    int show_sel_dialog = 0;            // 1 expand, 2 contract, 3 feather
    bool show_layer_props_dialog = false;
    bool show_resize_dialog = false;
    bool show_canvas_dialog = false;
    bool show_rotate_dialog = false;
    // Resize dialog
    int resize_w = 0, resize_h = 0;
    float resize_pct = 100.0f;
    bool resize_lock = true;
    int resize_by_percent = 0;
    int resize_filter = 2;              // raster::Filter
    // Canvas size dialog
    int canvas_w = 0, canvas_h = 0, canvas_anchor = 4;  // 3x3 anchor, 4 = centre
    // Rotate dialog
    float rotate_degrees = 15.0f;
    int rotate_cw = 1;
    // Crop tool rect (image coords), empty when none
    firn::raster::Rect crop_rect;
    // Line / shape tools
    float line_width = 3.0f;
    bool shape_antialias = true, shape_fill = true, shape_stroke = true;
    int shape_kind = 0;                 // see kShapeNames in Tools.cpp
    float shape_radius = 10.0f;         // rounded rectangle corner radius
    int shape_sides = 6, star_points = 5;
    float star_inner = 0.5f;
    // Text tool
    std::vector<firn::text::FontInfo> fonts;
    bool fonts_loaded = false;
    int font_index = 0;
    std::shared_ptr<firn::text::Font> text_font;
    char text_buf[2048] = "Text";
    float text_size = 48.0f;
    bool text_antialias = true;
    int text_align = 0;
    float text_stroke = 0.0f;           // outline width in px, foreground material
    float text_angle = 0.0f;            // degrees clockwise
    int text_x_offset = 0, text_y_offset = 0;  // placement shift from stroke padding / rotation
    int text_x = 0, text_y = 0;
    bool show_text_dialog = false;
    int text_temp_layer = -1;           // preview layer while the dialog is open
    int text_prev_active = -1;
    void ensure_fonts();
    void draw_text_dialog();
    firn::LayerProps layer_props_edit;  // dialog working copy
    firn::LayerProps layer_props_before; // props at the start of a live slider drag
    bool show_imgui_demo = false;
    int new_w = 800, new_h = 600;
    float blur_radius = 3.0f;
    int box_radius = 3;
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
    void request_load_selection();
    void request_save_selection();
    void load_selection(const std::string& path);   // any image: luminance x alpha becomes the mask
    void save_selection(const std::string& path);   // .PspSelection (or any writable format)
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

    // Geometry
    firn::Color background_fill() const;  // bg material as an opaque colour
    void crop_to_selection();
    void crop_to(firn::raster::Rect r);
    void rotate(float degrees_cw);
    void open_resize_dialog();
    void open_canvas_dialog();

    // Per-frame UI (ui/*.cpp)
    void draw_menu();
    void draw_canvas();
    void draw_palettes();
    void draw_dialogs();
    void handle_shortcuts();

    // Keeps the GL texture in sync with the document's composite.
    void sync_canvas_texture();
};
