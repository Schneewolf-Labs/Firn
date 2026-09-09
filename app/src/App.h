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
#include "firn/io_psp.h"
#include "firn/document.h"
#include "firn/raster.h"
#include "firn/text.h"
#include "firn/vector.h"
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
    firn::Image composite_cache;            // what the texture holds; updated per dirty rect
    float zoom = 1.0f;
    float pan_x = 0.0f, pan_y = 0.0f;  // canvas offset in screen px, relative to view center
    bool fit_requested = true;
    ImVec2 canvas_center;               // view center in screen space, updated by draw_canvas

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
                     ColorBalance, Sepia, HueMap, Wave, Pinch, Twirl, Buttonize, InnerBevel, Cutout,
                     Ripple, Spherize, Lens, Halftone, Chrome, OuterBevel, FadeCorrection, Kaleidoscope, Sunburst };
    Adj open_adjust = Adj::None;
    struct Preview {
        bool active = false;
        size_t layer = 0;
        std::string name;
        firn::Image before;
        std::array<int, 256> histogram{};
        bool dirty = true;
        bool live = true;               // small layer: exact re-apply on every change
        bool approximate = false;       // the layer holds a region/proxy preview, not the exact result
    } preview;
    firn::raster::Rect visible_image_rect;  // part of the image inside the canvas view, updated by draw_canvas
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
    float gamma_rgb[3] = {1.0f, 1.0f, 1.0f}; bool gamma_link = true;
    int fade_amount = 45;
    float redeye_strength = 1.0f;
    int kal_petals = 6; float kal_angle = 0, kal_radius = 50;
    float sun_x = 0.5f, sun_y = 0.5f, sun_brightness = 0.8f, sun_ray_brightness = 0.6f; int sun_rays = 12; float sun_color[3] = {1, 1, 0.9f};
    bool show_info_dialog = false;
    bool show_prefs_dialog = false;
    Config prefs_edit;                  // working copy while the dialog is open
    void apply_config();                // push config values into live state
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
    float ripple_amp = 5, ripple_wave = 30;
    int spherize_strength = 50, lens_strength = 30;
    int halftone_cell = 6; float halftone_angle = 45; float halftone_ink[3] = {0, 0, 0}, halftone_paper[3] = {1, 1, 1};
    int chrome_bands = 4; float chrome_brightness = 1.0f;
    int obevel_width = 8; float obevel_angle = 315, obevel_depth = 1.0f; float obevel_color[3] = {0.7f, 0.7f, 0.7f};
    // Mask overlay while editing: red tint over hidden areas
    bool show_mask_overlay = true;
    GLuint overlay_tex = 0;
    uint64_t overlay_tex_revision = ~0ull;
    void sync_overlay_texture();
    int show_sel_dialog = 0;            // 1 expand, 2 contract, 3 feather
    bool show_layer_props_dialog = false;
    bool show_jpeg_dialog = false;
    int jpeg_quality = 90;
    std::string pending_jpeg_path;
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
    int canvas_w = 0, canvas_h = 0, canvas_anchor = 4;  // 3x3 anchor, 4 = center
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
    // Vector objects. Shape, line, and text tools make vector objects when
    // create_as_vector is on (on a vector layer, created as needed).
    bool create_as_vector = false;
    int pen_mode = 0;                   // 0 point to point, 1 freehand, 2 edit nodes
    bool pen_close = false;
    void draw_line_style_combo();       // styled line picker for the tool options
    void draw_create_as_vector();       // the checkbox shared by the shape tools
    // Materials beyond a flat color: gradient or pattern, combined with the
    // foreground/background color for the solid case.
    struct Material {
        int kind = 0;                    // 0 color, 1 gradient, 2 pattern
        int gradient_index = -1;         // into gradient_library, -1 = the two-color default
        firn::vec::Gradient gradient;    // colors/opacities of the chosen gradient
        int gradient_style = 0;          // vec::GradientStyle
        float gradient_angle = 0.0f;
        int gradient_repeats = 0;
        bool gradient_invert = false;
        int pattern_index = -1;
        std::shared_ptr<const firn::Image> pattern;
        float pattern_scale = 1.0f, pattern_angle = 0.0f;
    };
    Material fg_material, bg_material;
    firn::vec::PaintStyle material_style(bool foreground) const;   // as a paint style
    // Libraries scanned from ~/.config/firn/*, FIRN_*_DIRS, Preferences, and the backup.
    struct ShapeEntry { std::string path, name; std::vector<firn::vec::Object> objects; int width = 0, height = 0; };
    std::vector<ShapeEntry> shape_library;
    bool shape_library_loaded = false;
    int shape_library_index = -1;       // -1 = one of the built-in shapes (shape_kind)
    bool shape_retain_style = true;     // library shapes keep their own stroke/fill
    void ensure_shape_library();
    std::vector<firn::vec::Gradient> gradient_library;
    bool gradients_loaded = false;
    void ensure_gradients();
    struct LineEntry { std::string path; firn::vec::LineStyle line; };
    std::vector<LineEntry> line_library;
    bool lines_loaded = false;
    int line_index = -1;                // -1 = solid
    void ensure_line_styles();
    struct PatternEntry { std::string path, name; };
    std::vector<PatternEntry> pattern_library;
    bool patterns_loaded = false;
    void ensure_patterns();
    // Object editing
    int vector_layer_for_edit(bool create);   // active vector layer, or a new one; -1 when none
    void add_vector_object(firn::vec::Object o, const std::string& name);   // onto vector_layer_for_edit(true), selected
    std::vector<firn::vec::Object> shape_objects(float x0, float y0, float x1, float y1) const;   // preset shape(s) for a drag rect, unstyled
    void apply_object_style(firn::vec::Object& o, bool stroke, bool fill, ImGuiMouseButton button) const;
    std::vector<size_t> selected_objects() const;     // indices into the active vector layer
    void select_objects(const std::vector<size_t>& indices, bool add = false);
    void objects_changed(const char* name, std::vector<firn::vec::Object> before);  // commits the live edit
    void object_align(int how);          // 0 top, 1 bottom, 2 left, 3 right, 4 vertical center, 5 horizontal center, 6 center in canvas, 7 h center in canvas, 8 v center in canvas
    void object_distribute(int how);     // 0 v top, 1 v center, 2 v bottom, 3 h left, 4 h center, 5 h right, 6 space v, 7 space h
    void object_same_size(int how);      // 0 height, 1 width, 2 both
    void object_arrange(int delta);
    void object_group();
    void object_ungroup();
    void object_delete();
    void object_select_all();
    void object_select_none();
    void object_text_to_curves(bool per_character);
    void open_vector_properties();
    void open_text_edit();               // re-opens the text dialog on a selected text object
    bool show_vector_props_dialog = false;
    firn::vec::Object vector_props_edit; // dialog working copy (first selected object)
    std::vector<firn::vec::Object> vector_props_before;
    int vector_props_layer = -1;
    int vector_props_index = -1;
    int text_edit_object = -1;           // object the text dialog is editing, or -1 for a new one
    void draw_vector_dialogs();
    void layer_new_vector();
    void layer_convert_to_raster();
    std::vector<firn::vec::Path> text_paths(const firn::vec::TextInfo& t, std::vector<int>* glyph_ids = nullptr) const;  // outlines, block top-left at (0, 0)
    void place_text_object(firn::vec::Object& o, const firn::vec::TextInfo& t, float x, float y) const;   // rebuilds o's paths at (x, y) with rotation
    int text_vec_layer = -1;             // vector layer the text dialog previews on (create_as_vector)
    int text_vec_index = -1;             // object being previewed there
    std::vector<firn::vec::Object> text_vec_before;
    // Custom brush tips
    struct TipEntry { std::string path, name; std::shared_ptr<const firn::raster::BrushTip> tip; };
    std::vector<TipEntry> brush_tips;
    bool brush_tips_loaded = false;
    int brush_tip_index = -1;            // -1 = built-in shape
    void ensure_brush_tips();
    void select_brush_tip(int index);
    void brush_tip_from_selection();
    // Paper textures
    struct TextureEntry { std::string path, name; std::shared_ptr<const firn::raster::BrushTip> texture; };
    std::vector<TextureEntry> textures;
    bool textures_loaded = false;
    int texture_index = -1;
    void ensure_textures();
    void select_texture(int index);
    // Picture tubes
    struct TubeEntry { std::string path, name; };
    std::vector<TubeEntry> tubes;
    bool tubes_loaded = false;
    int tube_index = -1;
    firn::Image tube_image;              // the loaded tube sheet
    firn::io::TubeInfo tube_info;
    std::string tube_loaded_path;
    float tube_scale = 1.0f;
    int tube_step_override = 0;          // 0 = use the tube's own step
    int tube_placement = 0, tube_selection = 0;  // 0 = as in the file
    void ensure_tubes();
    bool load_tube(int index);
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
    bool active_is_raster() const;      // false for groups and when nothing is active
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
    bool show_alpha_save_dialog = false;
    char alpha_name_buf[128] = "Selection #1";
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
    void layer_new_group();
    void layer_ungroup();
    void layer_set_mask(const char* name, firn::Mask m, bool enabled = true);
    void layer_mask_from_selection();
    void layer_mask_from_image();

    // Mask edit mode: tools paint a grayscale proxy of the active layer's
    // mask instead of its pixels; the mask follows live.
    bool mask_edit = false;
    firn::Image mask_proxy;
    size_t mask_proxy_layer = 0;
    void set_mask_edit(bool on);
    void refresh_mask_proxy();
    firn::Image& paint_pixels(size_t layer);            // layer pixels, or the mask proxy in mask edit mode
    void paint_touched(size_t layer, const firn::raster::Rect* rect = nullptr);  // after live edits to paint_pixels()
    void commit_pixels(size_t layer, const std::string& name, firn::Image before, const firn::Image& after);
    void layer_set_props(const firn::LayerProps& before, const firn::LayerProps& after);
    void open_layer_properties();

    // Geometry
    firn::Color background_fill() const;  // bg material as an opaque color
    void crop_to_selection();
    void crop_to(firn::raster::Rect r);
    void rotate(float degrees_cw);
    void open_resize_dialog();
    void open_canvas_dialog();

    // Per-frame UI (ui/*.cpp)
    void draw_menu();
    void draw_toolbar();
    void draw_status_bar();
    static constexpr float toolbar_height = 30.0f;
    static constexpr float status_height = 22.0f;
    int cursor_x = 0, cursor_y = 0;     // image coordinates under the pointer, for the status bar
    bool cursor_inside = false;
    void draw_canvas();
    void draw_palettes();
    void draw_dialogs();
    void handle_shortcuts();

    // Keeps the GL texture in sync with the document's composite.
    void sync_canvas_texture();
};
