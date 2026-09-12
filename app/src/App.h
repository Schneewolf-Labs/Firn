#pragma once
#include <functional>
#include <map>
#include <memory>
#include <array>
#include <string>
#include <vector>

#include <SDL_opengl.h>

#include "imgui.h"
#include "firn/adjust.h"
#include "firn/effects.h"
#include "firn/commands.h"
#include "firn/icc.h"
#include "firn/io_psp.h"
#include "firn/json.h"
#include "firn/document.h"
#include "firn/raster.h"
#include "firn/raster16.h"
#include "firn/text.h"
#include "firn/vector.h"
#include "Config.h"
#include "Tablet.h"
#include "tools/Tool.h"
#include "ui/FileDialog.h"
#include "ui/Theme.h"

struct MenuBuilder;

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
    uint64_t saved_state = 0;
    float zoom = 1.0f, pan_x = 0.0f, pan_y = 0.0f;
    bool fit_requested = true;
    firn::raster::Rect crop_rect;
    // Windowed view: a stable id for the ImGui window, its texture while the
    // document is parked, and whether it has been placed in the workspace.
    int uid = 0;
    GLuint tex = 0;
    uint64_t tex_revision = ~0ull;
    bool placed = false;
    ImVec2 win_pos, win_size;         // last frame's window rect, for keeping it inside the workspace
    uint64_t autosave_state = static_cast<size_t>(-1);   // history state at the last autosave
};

struct App {
    App();
    ~App();   // defined in App.cpp, where AdjustState is complete

    // Document (the current one; see DocState)
    std::unique_ptr<firn::Document> doc;
    firn::CommandStack history;
    std::string doc_path;
    std::string doc_title;
    uint64_t saved_state = 0;
    std::vector<DocState> docs;         // one slot per open image; docs[current_doc].doc is null (it lives above)
    int current_doc = -1;
    int untitled_counter = 0;
    int select_tab_request = -1;        // tab index to select on the next frame
    int next_doc_uid = 1;
    // Windowed view: every open image in its own window over the Image
    // workspace, each with its own zoom, instead of tabs in one view.
    bool image_windows = false;
    enum class Arrange { None, Cascade, TileHorizontally, TileVertically };
    Arrange arrange_request = Arrange::None;
    ImVec2 workspace_pos, workspace_size;   // the Image window's client area, in window mode
    void draw_document_windows();
    void draw_document_context_items(int index);   // right-click menu of a tab or window title
    void draw_parked_view(DocState& s, ImVec2 view_pos, ImVec2 view_size);
    void upload_document_texture(DocState& s);
    void draw_canvas_view(ImVec2 view_pos, ImVec2 view_size);
    int pending_close = -1;             // document awaiting the unsaved-changes prompt
    bool pending_quit = false;
    bool modified() const { return doc && history.state_id() != saved_state; }
    void stash_current();               // App members -> docs[current_doc]
    void activate_document(int index);
    void add_document(std::unique_ptr<firn::Document> d, const std::string& path);
    void close_document(int index, bool force = false);
    void request_quit();
    std::string document_title(int index) const;
    bool document_modified(int index) const;

    // The Adjust and Effects dialog parameters (app/src/ui/AdjustState.h),
    // held by pointer so their header stays out of everything that includes
    // this one.
    std::unique_ptr<struct AdjustState> adjust_state;

    std::unique_ptr<struct EffectState> effect_state;   // app/src/ui/EffectState.h
    std::unique_ptr<struct ToolState> tool_state;   // app/src/tools/ToolState.h
    std::unique_ptr<struct TextDialogState> text_dialog_state;   // app/src/ui/TextDialogState.h
    std::unique_ptr<struct ThemeEditorState> theme_editor_state;   // app/src/ui/ThemeEditorState.h
    std::unique_ptr<struct MaterialDialogState> material_dialog_state;   // app/src/ui/MaterialDialogState.h
    std::unique_ptr<struct MenuState> menu_state;   // app/src/ui/MenuState.h
    std::unique_ptr<struct SelectionMenuState> selection_menu_state;   // app/src/ui/SelectionMenuState.h
    std::unique_ptr<struct EffectBrowserState> fx_browser;   // app/src/ui/EffectBrowserState.h
    std::unique_ptr<struct VectorDialogState> vector_dialog_state;   // app/src/ui/VectorDialogState.h
    std::unique_ptr<struct PaletteState> palette_state;   // app/src/ui/PaletteState.h
    std::unique_ptr<struct AdjustLayerState> adjust_layer_state;   // app/src/ui/AdjustLayerState.h
    Config config;
    bool show_rulers = true, show_grid = false, show_guides = true;
    bool snap_to_guides = true, snap_to_grid = false;
    int grid_spacing = 10;
    // Guides and painting assistants live on the current Document (so they
    // travel with the image and are saved in the project format); these
    // read as empty when no image is open.
    std::vector<float>& guides_h();
    const std::vector<float>& guides_h() const;
    std::vector<float>& guides_v();
    const std::vector<float>& guides_v() const;
    std::vector<firn::Assistant>& assistants();
    const std::vector<firn::Assistant>& assistants() const;
    // Guide being dragged: kind 0 none, 1 horizontal, 2 vertical; index -1 = new
    int guide_drag_kind = 0, guide_drag_index = -1;
    void snap_point(float& x, float& y) const;
    bool show_assistants = true, assistant_snap = true;
    mutable std::vector<float> no_guides_;              // returned when there is no document
    mutable std::vector<firn::Assistant> no_assistants_;
    int assistant_kind = 0;                 // Assistant tool: what a click or drag creates
    int assistant_choice = -1;              // which assistant strokes follow; -1 = the nearest
    // The assistant a stroke starting at (sx, sy) should follow (-1 = none).
    int nearest_assistant(float sx, float sy) const;
    // Moves (x, y) onto assistant `i`'s line for a stroke that started at (sx, sy).
    void assist_point(int i, float sx, float sy, float& x, float& y) const;

    // Canvas view
    GLuint canvas_tex = 0;
    uint64_t canvas_tex_revision = ~0ull;  // revision the texture was built from
    firn::Image composite_cache;            // what the texture holds; updated per dirty rect
    float zoom = 1.0f;
    float pan_x = 0.0f, pan_y = 0.0f;  // canvas offset in screen px, relative to view center
    bool fit_requested = true;
    ImVec2 canvas_center;               // view center in screen space, updated by draw_canvas
    ImVec2 canvas_view_size{0, 0};      // the canvas view's size, likewise

    // Tools
    std::vector<std::unique_ptr<Tool>> tools;
    int tool_index = 0;
    int active_button = -1;             // mouse button of the gesture in progress, or -1
    firn::raster::Brush brush;
    bool clone_aligned = true, clone_sample_merged = false;
    // Selection tool options (shared by Selection / Freehand / Magic Wand)
    int sel_shape = 0;                  // Selection tool shape: see kSelectionShapes in Tools.cpp
    int sel_mode = 0;                   // mask::Combine as int: 0 replace, 1 add, 2 subtract, 3 intersect
    float sel_feather = 0.0f;
    bool sel_antialias = true;
    int sel_freehand_type = 0;          // 0 freehand, 1 point to point, 2 smart edge, 3 edge seeker
    int sel_range = 10;                 // edge seeker search radius
    int sel_smoothing = 0;              // outline smoothing 0..100
    bool show_marquee = true;
    // Selections > Modify dialog parameters.
    bool sel_preserve_corners = true;
    bool sel_aa_inside = true, sel_aa_outside = true;
    // Edit Selection: paint the selection as a mask with the ordinary tools.
    bool selection_edit = false;
    firn::Mask selection_edit_before;
    void set_selection_edit(bool on);
    void select_from_mask();
    void select_from_vector();
    void promote_selection_to_layer(bool floating);
    void defloat();
    bool has_floating_layer() const;
    void draw_selections_menu(MenuBuilder& m);
    void draw_selection_dialogs();
    void draw_layer_menu_items(MenuBuilder& m);
    int fgsel_size = 24;                  // Foreground Select: mark brush size
    int csmudge_rate = 30, csmudge_length = 60, csmudge_mode = 0;   // Color Smudge: color added per stamp (%), how far it is carried (%), 0 smearing / 1 dulling
    bool fgsel_merged = true;             //   classify on the merged image
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
    enum class PendingFileOp { None, Open, SaveAs, LoadSelection, SaveSelection, LoadPalette, SavePalette, SavePdf, LoadSwatches, SaveSwatches, LoadProfile, ImportTheme, ExportTheme };
    PendingFileOp file_op = PendingFileOp::None;
    bool show_new_dialog = false;
    // Adjustment / effect dialogs with live preview (ui/Adjust.cpp)
    enum class Adj { None, BrightnessContrast, Curves, Gamma, Levels, Threshold, ChannelMixer, Colorize, HSL,
                     Average, Gaussian, Posterize, Solarize, UnsharpMask, Median, MotionBlur, Mosaic, AddNoise, DropShadow,
                     ColorBalance, Sepia, HueMap, Wave, Pinch, Twirl, Buttonize, InnerBevel, Cutout,
                     Ripple, Spherize, Lens, Halftone, Chrome, OuterBevel, FadeCorrection, Kaleidoscope, Sunburst,
                     AutoColor, AutoContrast, AutoSaturation, Clarify, BlackWhitePoints, HistogramAdjust, SaltPepper, EdgeSmooth,
                     JpegArtifacts, FillFlash, Backlighting, ChromaticAberration, NoiseRemoval,
                     Curlicues, DisplacementMap, PolarCoordinates, SpikyHalo, Warp, Wind, Circle, Cylinder, Pentagon,
                     Perspective, Skew, Feedback, Pattern, RotatingMirror, Offset, SeamlessTiling, PageCurl,
                     AgedNewspaper, BallsBubbles, ColoredEdges, ColoredFoil, Contours, Enamel, GlowingEdges, HotWax,
                     MagnifyingLens, NeonGlow, Topography, Lights, Blinds, FineLeather, RoughLeather, Fur, MosaicAntique,
                     MosaicGlass, PolishedStone, Sandstone, Sculpture, SoftPlastic, StrawWall, Texture, Tiles, Weave,
                     BlackPencil, BrushStrokes, Charcoal, ColoredChalk, ColoredPencil, Pencil, UserFilter, ColorToAlpha };
    Adj open_adjust = Adj::None;
    static int adjust_count();
    static const char* adjust_title(int i);
    firn::effects::Edge edge_setting() const;     // the edge-mode option as the effects want it
    static firn::Color float_rgb(const float* f);
    // Effect Browser (app/src/ui/EffectBrowser.cpp): thumbnails of every
    // dialog's operation on the active layer, taken from the dialogs themselves.
    bool show_effect_browser = false;
    // The last adjustment or effect applied through a dialog, for Edit >
    // Repeat. The op reads the dialog's current settings, so repeating uses
    // what was last chosen.
    std::string last_effect;
    std::function<void(firn::Image&)> last_effect_op;
    std::function<void(firn::Image16&)> last_effect_op16;
    bool effect_capture = false;
    std::map<std::string, std::function<void(firn::Image&)>> effect_ops;
    std::vector<GLuint> browser_tex;
    std::vector<uint8_t> browser_state;   // 0 pending, 1 done, 2 failed
    void draw_effect_browser();
    void reset_effect_browser();
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
    bool before_was_deep_ = false;
    firn::raster::Rect visible_image_rect;  // part of the image inside the canvas view, updated by draw_canvas
    void preview_begin(const char* name);
    void preview_update(const std::function<void(firn::Image&)>& op, bool force = false);
    void preview_commit(const std::function<void(firn::Image16&)>& op16 = nullptr);   // with op16, 16-bit layers get an exact re-run
    void preview_cancel();
    void draw_adjust_dialogs();
    bool open_adjust_by_title(const char* title);   // opens an adjust/effect dialog by its title (scripting, driver)
    // Scripting: runs one of the original's App.Do commands (Script.cpp);
    // returns a JSON result, or a message with *ok false.
    std::string do_command(const std::string& name, const firn::json::Value& params, bool* ok);
    // Parameters, remembered between uses like the original's dialogs.
    int colorize_hue = 0, colorize_sat = 128;
    float gamma_rgb[3] = {1.0f, 1.0f, 1.0f}; bool gamma_link = true;
    // Photo fixes
    // Geometric / distortion / reflection / image effects
    int dmap_source = -1; float dmap_intensity = 10.0f, dmap_blur = 0.0f; bool dmap_2d = false;
    bool polar_rect = true;
    bool wind_left = true; int wind_strength = 30;
    bool cyl_vertical = false; int cyl_strength = 50;
    bool persp_vertical = false; int persp_distortion = 40;
    bool skew_vertical = false; int skew_angle = 20;
    int fb_opacity = 60, fb_intensity = 5; float fb_cx = 50.0f, fb_cy = 50.0f; bool fb_elliptical = false;
    // Artistic / texture / art media effects
    int curl_corner = 3; float curl_w = 40.0f, curl_h = 40.0f; int curl_r = 30; float curl_back[3] = {0.9f, 0.9f, 0.9f}, curl_fill[3] = {1, 1, 1}; bool curl_transparent = false;
    bool show_info_dialog = false;
    // Pen tablet (app/src/Tablet.cpp): pressure, tilt, eraser tip.
    PenState pen;
    bool pen_size = true, pen_opacity = false;   // what pressure drives (Config)
    int smooth_mode = 0;                          // brush stroke smoothing: 0 none, 1 basic, 2 weighted, 3 stabilizer
    float smooth_amount = 30.0f;
    // Symmetry painting (raster::Symmetry): mode index, rotational copies,
    // axis center in image pixels (negative = the image center), and a
    // one-shot "next click places the center" flag.
    int symmetry_mode = 0, symmetry_count = 6;
    float symmetry_x = -1.0f, symmetry_y = -1.0f;
    bool symmetry_place = false;
    firn::raster::Symmetry symmetry() const;
    int pen_prev_tool = -1;                      // tool to restore when the eraser tip lifts
    void pen_tick();                             // per frame: presence timeout, eraser tip switching
    // Autosave and recovery (app/src/Autosave.cpp)
    double autosave_last = 0.0;
    struct RecoverEntry { std::string file, title, original_path, key; };
    std::vector<RecoverEntry> recover_files;
    bool show_recovery_dialog = false;
    std::string recovery_error;
    void autosave_tick();
    void autosave_forget(int uid);
    void autosave_forget(const std::string& key);
    void check_recovery();
    void draw_recovery_dialog();
    bool show_about_dialog = false;
    bool show_shortcuts_dialog = false;
    void draw_shortcuts_dialog();       // Help > Keyboard Shortcuts (app/src/ui/About.cpp)
    GLuint about_tex = 0;               // the icon, uploaded when the About window first opens
    std::string about_gl;               // renderer and version strings, read once
    void draw_about_dialog();           // app/src/ui/About.cpp
    // Themes (app/src/ui/Theme.h, ThemeEditor.cpp): built-ins plus the
    // user's files; the current one is named by config.theme.
    std::vector<Theme> themes;
    bool themes_loaded = false;
    // HiDPI: the effective UI scale (config.ui_scale, or the display's factor
    // measured at startup) multiplies the style sizes and the font size.
    float ui_scale = 1.0f;
    float auto_ui_scale = 1.0f;
    void set_auto_ui_scale(float s);
    // The drawable/window pixel ratio (Retina, Wayland fractional scale):
    // rasterizes the font atlas sharper without changing logical sizes. Unlike
    // ui_scale this is not a user "make things bigger" preference, so it never
    // multiplies style sizes on its own (see main.cpp).
    float font_density = 1.0f;
    void ensure_themes();
    const Theme* find_theme(const std::string& name) const;
    void apply_theme(const std::string& name);
    void apply_theme_values(const Theme& t);
    bool save_theme(Theme t, const std::string& name, std::string* err);
    void import_theme(const std::string& path);
    void export_theme(const std::string& path);
    std::string default_ui_font();
    // Font changes rebuild the atlas between frames.
    bool font_pending = false;
    std::string font_pending_path, font_current_path;
    float font_pending_size = 13.0f, font_current_size = 13.0f;
    void apply_pending_font();
    bool show_theme_editor = false;
    void open_theme_editor();
    void draw_theme_editor();
    bool show_prefs_dialog = false;
    void apply_config();                // push config values into live state
    firn::adjust::ChannelMix mixer;
    std::vector<std::pair<float, float>> curve_points{{0.0f, 0.0f}, {255.0f, 255.0f}};
    int mosaic_w = 8, mosaic_h = 8; bool mosaic_square = true;
    int noise_percent = 20; bool noise_gaussian = false, noise_mono = false;
    int shadow_x = 5, shadow_y = 5; float shadow_opacity = 0.5f, shadow_blur = 5.0f; float shadow_color[3] = {0, 0, 0};
    firn::adjust::ColorBalance color_balance; int cb_range = 1;
    int button_width = 10; float button_opacity = 0.75f; float button_color[3] = {0.5f, 0.5f, 0.5f}; bool button_transparent = false;
    int bevel_width = 10; float bevel_angle = 315, bevel_depth = 1.0f, bevel_ambient = 1.0f;
    // Mask overlay while editing: red tint over hidden areas
    bool show_mask_overlay = true;
    GLuint overlay_tex = 0;
    uint64_t overlay_tex_revision = ~0ull;
    void sync_overlay_texture();
    int show_sel_dialog = 0;            // a SelDialog id (app/src/ui/SelectionMenu.cpp)
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
    // Canvas size dialog
    int canvas_w = 0, canvas_h = 0, canvas_anchor = 4;  // 3x3 anchor, 4 = center
    // Rotate dialog
    // Color management (ui/ImageMenu.cpp)
    bool color_managed_display = true;
    firn::Image display_cache;               // composite converted to sRGB for the screen
    std::vector<uint8_t> display_icc_key;    // profile the cached transform was built for
    std::unique_ptr<firn::icc::Transform> display_transform;
    firn::icc::Profile document_profile() const;   // parsed embedded profile (invalid when untagged)
    bool display_needs_transform() const;
    void assign_profile(const std::vector<uint8_t>& icc, const std::string& name);
    void convert_to_profile(const firn::icc::Profile& to, const std::vector<uint8_t>& icc, const std::string& name);
    void request_load_profile();
    // Print (File > Print...)
    bool show_print_dialog = false;
    int print_paper = 0;                  // 0 Letter, 1 A4, 2 Legal
    bool print_landscape = false, print_center = true, print_fit = true;
    float print_margin = 0.5f, print_scale = 100.0f;
    int print_dpi = 300;
    std::string print_printer;
    void request_print_pdf();
    void print_to_pdf(const std::string& path, bool send);
    // Swatches and recent colors (Materials palette)
    std::vector<firn::Color> swatches, recent_colors;
    bool materials_all_tools = true;
    bool swatches_loaded = false;
    void ensure_swatches();
    void save_swatches();
    void note_recent_color(const float* rgba);
    void request_load_swatches();
    void request_save_swatches();
    // Image menu (ui/ImageMenu.cpp)
    bool show_borders_dialog = false, show_frame_dialog = false, show_depth_dialog = false, show_combine_dialog = false, show_arith_dialog = false;
    int border_l = 10, border_r = 10, border_t = 10, border_b = 10; bool border_symmetric = true;
    int depth_colors = 256; bool depth_dither = true;
    int combine_mode = 0, combine_src[4] = {0, 1, 2, 3};
    int arith_a = 0, arith_b = 1, arith_op = 0, arith_channel = 0, arith_bias = 0; float arith_divisor = 1.0f; bool arith_clip = true;
    struct FrameEntry { std::string path, name; };
    std::vector<FrameEntry> frame_library;
    bool frames_loaded = false;
    int frame_index = 0; bool frame_inside = true, frame_flip = false, frame_mirror = false;
    void ensure_frames();
    void draw_image_dialogs();
    void image_count_colors();
    void image_decrease_depth(int colors, bool dither);
    void image_split_channels(int mode);
    void request_load_palette();
    void request_save_palette();
    void load_palette(const std::string& path);
    void save_palette(const std::string& path);
    firn::Document* document_at(int index);   // any open document by tab index
    // Warp and remover tools
    int warp_mode = 0, warp_strength = 50;
    int mesh_cols = 4, mesh_rows = 4;
    int scratch_width = 12, remover_feather = 4;
    // Deform family options
    int straighten_mode = 0;             // 0 auto, 1 vertical, 2 horizontal
    bool straighten_all_layers = true, straighten_crop = true;
    bool perspective_all_layers = true, perspective_crop = false;
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
        // Texture over any of the three, and the transparency switch that
        // turns the material off (no fill / no stroke).
        bool texture_on = false;
        int texture_index = -1;
        std::shared_ptr<const firn::Image> texture;
        float texture_scale = 1.0f, texture_angle = 0.0f, texture_strength = 1.0f;
        bool transparent = false;
    };
    Material fg_material, bg_material;
    Material material_backup;           // what the dialog opened with; Material is nested here, so it cannot move out with the rest
    firn::vec::PaintStyle material_style(bool foreground) const;   // as a paint style
    // Material Properties dialog (app/src/ui/MaterialDialog.cpp).
    bool show_material_dialog = false;
    bool material_dialog_fg = true;
    int material_view = 0;              // Materials palette: 0 frame, 1 rainbow, 2 swatches
    void open_material_dialog(bool foreground);
    void draw_material_dialog();
    std::shared_ptr<const firn::Image> texture_image(int index);   // loads and caches a paper texture as an image
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
    std::vector<firn::vec::Object> vector_props_before;
    int text_edit_object = -1;           // object the text dialog is editing, or -1 for a new one
    void draw_vector_dialogs();
    void layer_new_vector();
    // Adjustment layers: the dialog edits the layer live; Cancel on a new
    // layer undoes its creation.
    void layer_new_adjustment(firn::Adjustment::Kind kind);
    void open_adjustment_dialog(int layer, bool created);
    void draw_adjustment_layer_dialog();
    // Layer Styles dialog (app/src/ui/LayerStyles.cpp): edits the layer live, commits on OK.
    void open_layer_styles(int layer);
    void draw_layer_styles_dialog();
    bool show_layer_styles_dialog = false;
    int style_layer_index = -1;
    firn::LayerStyle style_before;
    bool show_adjust_layer_dialog = false;
    bool adj_layer_created = false;
    void layer_convert_to_raster();
    std::vector<firn::vec::Path> text_paths(const firn::vec::TextInfo& t, std::vector<int>* glyph_ids = nullptr) const;  // outlines, block top-left at (0, 0)
    void place_text_object(firn::vec::Object& o, const firn::vec::TextInfo& t, float x, float y) const;   // rebuilds o's paths at (x, y) with rotation
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
    struct TextureEntry { std::string path, name; std::shared_ptr<const firn::raster::BrushTip> texture; std::shared_ptr<const firn::Image> image; };
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
    void ensure_tubes();
    bool load_tube(int index);
    // Text tool
    std::vector<firn::text::FontInfo> fonts;
    bool fonts_loaded = false;
    int font_index = 0;
    std::shared_ptr<firn::text::Font> text_font;
    float text_size = 48.0f;
    bool text_antialias = true;
    float text_stroke = 0.0f;           // outline width in px, foreground material
    int text_x = 0, text_y = 0;
    bool show_text_dialog = false;
    void ensure_fonts();
    void draw_text_dialog();
    firn::LayerProps layer_props_edit;  // dialog working copy
    bool show_imgui_demo = false;
    int new_w = 800, new_h = 600;
    float blur_radius = 3.0f;
    std::string status;
    bool quit = false;

    // Actions (implemented in App.cpp)
    void new_document(int w, int h);
    bool open_document(const std::string& path);
    bool save_document(const std::string& path);
    // The same save with the writing on a worker thread, for the places a
    // person is sitting there waiting: File > Save and Save As. Every other
    // caller (scripts, the driver, the close prompt) stays synchronous,
    // because they act on the result immediately.
    void save_document_async(const std::string& path);
    void open_document_async(const std::string& path);
    void after_saved(const std::string& path);
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
    int prev_tool_index = -1;      // for the scripts' SelectPreviousTool
    void zoom_about(ImVec2 screen, float factor);

    // Selections and clipboard
    void set_selection(const char* name, firn::Mask m);    // runs a SelectionCommand
    void apply_selection_gesture(const char* name, firn::Mask shape);  // combine per sel_mode + feather
    void select_all();
    void select_none();
    void select_invert();
    void copy_image(firn::Image out, const char* what);   // both clipboards, cut to the selection
    void copy();
    void copy_merged();          // the composite, not just the active layer
    void paste_into_selection(); // scales the clipboard to the selection and paints it through
    void repeat_last_effect();   // re-applies the last Adjust/Effects dialog with its settings
    void content_aware_fill(bool background = true);   // rebuilds the selection from the rest of the picture
    void revert();               // reloads the file from disk, dropping every change
    bool show_revert_prompt = false;
    void zoom_to_rect(firn::raster::Rect r);   // fills the view with an image rect
    void zoom_to_selection();
    // Layers palette: the layer whose name is being edited in place (-1 none).
    void cut();
    void clear_selection();
    void paste_as_new_layer();
    void paste_as_new_image();
    bool clipboard_for_paste(firn::Image& px, firn::raster::Rect& bounds);   // system clipboard, else the internal one
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
    // Layers palette drag and drop: puts layer `from` where `onto` sits.
    void layer_move_onto(int from, int onto);
    void layer_merge(int kind);     // 0 down, 1 visible, 2 all
    void layer_view_only(bool current_only);   // hide every other layer, or show all
    void layer_promote_background();
    // A slow operation on a worker thread; see BackgroundJob.h.
    std::unique_ptr<struct BackgroundJob> job;
    bool job_running() const { return job != nullptr; }
    void draw_background_job();
    const firn::Mask* paint_clip(int layer);
    firn::Mask paint_clip_cache;   // narrowed selection for a protected layer
    bool can_clip_layer() const;
    void layer_toggle_clipped();
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

    // Per-frame UI (ui/*.cpp). draw_menu is the whole menu tree (File through
    // Help) as a single source of truth: the caller supplies a MenuBuilder,
    // either ImGuiMenuBuilder (the in-window bar, every platform) or the
    // native macOS one (NativeMenu_mac.mm), which decide how it is shown.
    void draw_menu(MenuBuilder& m);
    void draw_toolbar();
    void draw_status_bar();
    float toolbar_height = 30.0f;
    float status_height = 22.0f;
    int cursor_x = 0, cursor_y = 0;     // image coordinates under the pointer, for the status bar
    bool cursor_inside = false;
    void draw_canvas();
    void draw_palettes();
    void draw_dialogs();
    void handle_shortcuts();

    // Keeps the GL texture in sync with the document's composite.
    void sync_canvas_texture();
};
