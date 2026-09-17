#include "App.h"
#include "GenerateBackend.h"
#include "ui/AdjustState.h"
#include "ui/AdjustLayerState.h"
#include "ui/PaletteState.h"
#include "ui/VectorDialogState.h"
#include "ui/EffectBrowserState.h"
#include "ui/SelectionMenuState.h"
#include "ui/MenuState.h"
#include "ui/MaterialDialogState.h"
#include "ui/ThemeEditorState.h"
#include "ui/TextDialogState.h"
#include "tools/ToolState.h"
#include "ui/EffectState.h"
#include "firn/inpaint.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

#include "BackgroundJob.h"
#include "Clipboard.h"
#include "Version.h"
#include "imgui.h"
#include "firn/io.h"
#include "firn/io_psp.h"
#include "firn/mask.h"

using namespace firn;

static Image mask_to_image(const Mask& m);

App::~App() {
    // A worker may still be reading the document or writing a file. Waiting
    // here rather than relying on member destruction order means a future
    // reshuffle of App's members cannot turn this into a use-after-free.
    if (job && job->done.valid()) job->done.wait();
}

App::App() : tools(make_default_tools()) {
    adjust_layer_state = std::make_unique<AdjustLayerState>();
    palette_state = std::make_unique<PaletteState>();
    vector_dialog_state = std::make_unique<VectorDialogState>();
    fx_browser = std::make_unique<EffectBrowserState>();
    selection_menu_state = std::make_unique<SelectionMenuState>();
    menu_state = std::make_unique<MenuState>();
    material_dialog_state = std::make_unique<MaterialDialogState>();
    theme_editor_state = std::make_unique<ThemeEditorState>();
    text_dialog_state = std::make_unique<TextDialogState>();
    tool_state = std::make_unique<ToolState>();
    adjust_state = std::make_unique<AdjustState>();
    effect_state = std::make_unique<EffectState>();
    config.load();
    file_dialog.set_directory(config.last_directory);
    apply_config();
}

firn::raster::Symmetry App::symmetry() const {
    firn::raster::Symmetry s;
    s.mode = static_cast<firn::raster::Symmetry::Mode>(std::clamp(symmetry_mode, 0, 5));
    s.count = std::clamp(symmetry_count, 2, 64);
    s.cx = symmetry_x >= 0.0f || !doc ? symmetry_x : doc->width() * 0.5f;
    s.cy = symmetry_y >= 0.0f || !doc ? symmetry_y : doc->height() * 0.5f;
    return s;
}

std::vector<float>& App::guides_h() { return doc ? doc->guides_h() : no_guides_; }
const std::vector<float>& App::guides_h() const { return doc ? doc->guides_h() : no_guides_; }
std::vector<float>& App::guides_v() { return doc ? doc->guides_v() : no_guides_; }
const std::vector<float>& App::guides_v() const { return doc ? doc->guides_v() : no_guides_; }
std::vector<Assistant>& App::assistants() { return doc ? doc->assistants() : no_assistants_; }
const std::vector<Assistant>& App::assistants() const { return doc ? doc->assistants() : no_assistants_; }

int App::nearest_assistant(float sx, float sy) const {
    int best = -1;
    float best_d = 1e30f;
    for (size_t i = 0; i < assistants().size(); ++i) {
        const Assistant& a = assistants()[i];
        float d;
        if (a.kind == Assistant::Kind::VanishingPoint) d = std::hypot(sx - a.x0, sy - a.y0);
        else {
            // Distance to the ruler's line (parallel rulers count the same way: pick the nearest).
            const float dx = a.x1 - a.x0, dy = a.y1 - a.y0, len = std::hypot(dx, dy);
            d = len > 1e-3f ? std::abs((sx - a.x0) * dy - (sy - a.y0) * dx) / len : std::hypot(sx - a.x0, sy - a.y0);
        }
        if (d < best_d) { best_d = d; best = static_cast<int>(i); }
    }
    return best;
}

void App::assist_point(int i, float sx, float sy, float& x, float& y) const {
    if (i < 0 || i >= static_cast<int>(assistants().size())) return;
    const Assistant& a = assistants()[i];
    float ox, oy, dx, dy;   // a point on the line and its direction
    if (a.kind == Assistant::Kind::VanishingPoint) { ox = a.x0; oy = a.y0; dx = sx - a.x0; dy = sy - a.y0; }
    else if (a.kind == Assistant::Kind::Parallel) { ox = sx; oy = sy; dx = a.x1 - a.x0; dy = a.y1 - a.y0; }
    else { ox = a.x0; oy = a.y0; dx = a.x1 - a.x0; dy = a.y1 - a.y0; }
    const float len = std::hypot(dx, dy);
    if (len < 1e-3f) return;
    dx /= len; dy /= len;
    const float t = (x - ox) * dx + (y - oy) * dy;
    x = ox + dx * t; y = oy + dy * t;
}

void App::apply_config() {
    show_rulers = config.show_rulers;
    show_grid = config.show_grid;
    grid_spacing = config.grid_spacing;
    jpeg_quality = config.jpeg_quality;
    new_w = config.new_width;
    new_h = config.new_height;
    history.set_limit(config.undo_limit);
    history.set_memory_limit(static_cast<size_t>(config.undo_memory_mb) << 20);
    for (DocState& d : docs) { d.history.set_limit(config.undo_limit); d.history.set_memory_limit(static_cast<size_t>(config.undo_memory_mb) << 20); }
    if (color_managed_display != config.color_managed_display) {
        color_managed_display = config.color_managed_display;
        canvas_tex_revision = ~0ull;
        for (DocState& d : docs) d.tex_revision = ~0ull;
    }
    image_windows = config.image_windows;
    pen_size = config.pen_size; pen_opacity = config.pen_opacity;
    smooth_mode = config.smooth_mode; smooth_amount = config.smooth_amount;
    apply_theme(config.theme);
    // Library folders may have changed: rescan on next use.
    tubes_loaded = brush_tips_loaded = textures_loaded = false;
    tubes.clear(); brush_tips.clear(); textures.clear();
    tube_index = -1; brush_tip_index = -1; texture_index = -1;
    brush.tip.reset(); brush.texture.reset();
}

// --- Documents -----------------------------------------------------------

void App::stash_current() {
    if (current_doc < 0 || current_doc >= static_cast<int>(docs.size())) return;
    DocState& s = docs[current_doc];
    s.doc = std::move(doc);
    s.history = std::move(history);
    s.doc_path = doc_path;
    s.title = doc_title;
    s.saved_state = saved_state;
    s.zoom = zoom; s.pan_x = pan_x; s.pan_y = pan_y;
    s.fit_requested = fit_requested;
    s.crop_rect = crop_rect;
    history = CommandStack();
}

// Snaps to the nearest guide within 8 screen pixels, then to the grid.
void App::snap_point(float& x, float& y) const {
    const float tol = 8.0f / std::max(zoom, 0.01f);
    if (snap_to_guides && show_guides) {
        for (float g : guides_v()) if (std::abs(g - x) <= tol) { x = g; break; }
        for (float g : guides_h()) if (std::abs(g - y) <= tol) { y = g; break; }
    }
    if (snap_to_grid && grid_spacing > 0) {
        x = std::round(x / grid_spacing) * grid_spacing;
        y = std::round(y / grid_spacing) * grid_spacing;
    }
}

void App::activate_document(int index) {
    if (index < 0 || index >= static_cast<int>(docs.size()) || index == current_doc) return;
    set_selection_edit(false);
    set_mask_edit(false);
    tool().cancel(*this);
    preview_cancel();
    stash_current();
    DocState& s = docs[index];
    doc = std::move(s.doc);
    history = std::move(s.history);
    doc_path = s.doc_path;
    doc_title = s.title;
    saved_state = s.saved_state;
    zoom = s.zoom; pan_x = s.pan_x; pan_y = s.pan_y;
    fit_requested = s.fit_requested;
    crop_rect = s.crop_rect;
    current_doc = index;
    canvas_tex_revision = ~0ull;  // force re-upload
    select_tab_request = index;
}

void App::add_document(std::unique_ptr<Document> d, const std::string& path) {
    set_selection_edit(false);
    set_mask_edit(false);
    tool().cancel(*this);
    preview_cancel();
    stash_current();
    docs.emplace_back();
    docs.back().uid = next_doc_uid++;
    current_doc = static_cast<int>(docs.size()) - 1;
    doc = std::move(d);
    history.clear();
    history.set_limit(config.undo_limit);
    history.set_memory_limit(static_cast<size_t>(config.undo_memory_mb) << 20);
    doc_path = path;
    if (path.empty()) doc_title = "Untitled " + std::to_string(++untitled_counter);
    else { const auto slash = path.find_last_of("/\\"); doc_title = slash == std::string::npos ? path : path.substr(slash + 1); }
    saved_state = 0;
    zoom = 1.0f; pan_x = pan_y = 0.0f;
    fit_requested = true;
    crop_rect = {};
    canvas_tex_revision = ~0ull;
    select_tab_request = current_doc;
}

std::string App::document_title(int index) const {
    return index == current_doc ? doc_title : docs[index].title;
}

bool App::document_modified(int index) const {
    if (index == current_doc) return modified();
    const DocState& s = docs[index];
    return s.doc && s.history.state_id() != s.saved_state;
}

void App::close_document(int index, bool force) {
    if (index < 0 || index >= static_cast<int>(docs.size())) return;
    if (index == current_doc) set_selection_edit(false);
    if (!force && document_modified(index)) { pending_close = index; return; }
    if (docs[index].tex) { glDeleteTextures(1, &docs[index].tex); docs[index].tex = 0; }
    autosave_forget(docs[index].uid);   // closing (or discarding) ends the need for a recovery copy
    if (index == current_doc) {
        set_mask_edit(false);
        tool().cancel(*this);
        preview_cancel();
        doc.reset();
        history.clear();
        doc_path.clear();
        docs.erase(docs.begin() + index);
        current_doc = -1;
        canvas_tex_revision = ~0ull;
        if (!docs.empty()) {
            // activate_document stashes the (now empty) current state; there is none.
            const int next = std::min(index, static_cast<int>(docs.size()) - 1);
            DocState& s = docs[next];
            doc = std::move(s.doc); history = std::move(s.history); doc_path = s.doc_path; doc_title = s.title;
            saved_state = s.saved_state; zoom = s.zoom; pan_x = s.pan_x; pan_y = s.pan_y; fit_requested = s.fit_requested; crop_rect = s.crop_rect;
            current_doc = next;
            select_tab_request = next;
        }
    } else {
        docs.erase(docs.begin() + index);
        if (index < current_doc) --current_doc;
    }
    if (pending_quit && docs.empty()) quit = true;
}

void App::request_quit() {
    // A worker is reading the document or writing a file. Leaving now would
    // stack the unsaved-changes prompt on top of the progress modal, and the
    // wait would happen during teardown instead of somewhere visible.
    if (job) { status = job->name + " is still running."; return; }
    set_selection_edit(false);
    for (size_t i = 0; i < docs.size(); ++i)
        if (document_modified(static_cast<int>(i))) { pending_quit = true; pending_close = static_cast<int>(i); return; }
    quit = true;
}

void App::new_document(int w, int h) {
    auto d = std::make_unique<Document>(w, h);
    Layer& bg = d->add_layer("Background");
    bg.background = true;
    bg.pixels.fill({255, 255, 255, 255});
    add_document(std::move(d), "");
    status = "New image " + std::to_string(w) + "x" + std::to_string(h);
}

bool App::open_document(const std::string& path) {
    std::string err;
    std::vector<std::string> warnings;
    if (!std::filesystem::exists(path)) {
        fail("Open failed: no such file: " + path);
        return false;
    }
    auto loaded = io::load_document(path, &err, &warnings);
    if (!loaded) {
        fail("Open failed: " + err);
        return false;
    }
    add_document(std::move(loaded), path);
    config.touch_recent(path);
    config.last_directory = file_dialog.directory();
    status = "Opened " + path;
    for (const auto& w : warnings) status += "\n" + w;
    return true;
}

void App::open_document_async(const std::string& path) {
    if (job) { status = job->name + " is still running."; return; }
    if (!std::filesystem::exists(path)) { fail("Open failed: no such file: " + path); return; }
    job = std::make_unique<BackgroundJob>();
    job->kind = BackgroundJob::Kind::Open;
    job->name = "Opening";
    job->path = path;
    job->cancellable = false;   // reading a file is not worth interrupting
    BackgroundJob* j = job.get();
    // The worker builds a document of its own and touches nothing the main
    // thread owns, so this one is safe even without the modal.
    j->done = std::async(std::launch::async, [j] {
        j->loaded = io::load_document(j->path, &j->error, &j->warnings);
        return j->loaded != nullptr;
    });
    status = "Opening " + path + "...";
}

bool App::save_document(const std::string& path) {
    if (!doc) return false;
    set_selection_edit(false);
    // JPEG: ask for the quality first; the dialog calls back with the path.
    {
        const auto dot = path.rfind('.');
        std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if ((ext == "jpg" || ext == "jpeg" || ext == "webp") && pending_jpeg_path != path) {
            pending_jpeg_path = path;
            show_jpeg_dialog = true;
            return false;
        }
        pending_jpeg_path.clear();
    }
    std::string err;
    if (!io::save_document(*doc, path, &err, jpeg_quality)) {
        fail("Save failed: " + err);
        return false;
    }
    after_saved(path);
    return true;
}

// Everything that happens once the bytes are on disk. Shared with the
// background save, which does the writing on a worker and then calls this
// on the main thread.
void App::after_saved(const std::string& path) {
    doc_path = path;
    { const auto slash = path.find_last_of("/\\"); doc_title = slash == std::string::npos ? path : path.substr(slash + 1); }
    // A format that cannot hold what the document holds has not really saved
    // it. Marking the document clean there is how a layered image could be
    // written to a PNG and then closed, without a prompt, losing every layer
    // -- so the file is written, said to be written, and the document stays
    // modified until it is put somewhere that keeps all of it.
    const bool keeps_everything = io::is_psp_extension(path) || io::is_ora_extension(path);
    const bool lossy = !keeps_everything && (doc->layer_count() > 1 || doc->bit_depth() == 16 || !doc->alpha_channels().empty());
    if (!lossy) {
        saved_state = history.state_id();
        if (current_doc >= 0 && current_doc < static_cast<int>(docs.size())) { docs[current_doc].autosave_state = saved_state; autosave_forget(docs[current_doc].uid); }
    }
    config.touch_recent(path);
    say("Saved " + path);
    if (lossy) {
        status_severity = Severity::Warning;
        status += " - the file is flat; this image is still unsaved";
        status += "\nOnly .ora and .pspimage keep layers, 16-bit channels and saved selections.";
    }
    if (io::is_psp_extension(path)) {
        bool extras = false;
        for (size_t i = 0; i < doc->layer_count(); ++i) extras |= doc->layer(i).style.any() || (doc->layer(i).is_adjustment() && doc->layer(i).adjustment.is_filter());
        if (extras) status += "\nClassic format: filter layers and layer styles are kept for Firn only; the original shows the layers without them.";
        if (!doc->icc().empty()) status += "\nClassic format: the color profile is not stored (.ora keeps it).";
    }
    // A close that was waiting on this Save As can go ahead now.
    if (pending_close_after_save >= 0) {
        const int idx = pending_close_after_save;
        pending_close_after_save = -1;
        if (!lossy && idx < static_cast<int>(docs.size())) {
            close_document(idx, true);
            if (pending_quit) request_quit();
            else if (closing_all && !docs.empty()) close_document(0);
            else closing_all = false;
        } else {
            closing_all = false;
            pending_quit = false;
        }
    }
}

void App::save_document_async(const std::string& path) {
    if (!doc) return;
    if (job) { status = job->name + " is still running."; return; }
    set_selection_edit(false);
    {
        const auto dot = path.rfind('.');
        std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if ((ext == "jpg" || ext == "jpeg" || ext == "webp") && pending_jpeg_path != path) {
            pending_jpeg_path = path;
            show_jpeg_dialog = true;
            return;
        }
        pending_jpeg_path.clear();
    }
    job = std::make_unique<BackgroundJob>();
    job->kind = BackgroundJob::Kind::Save;
    job->name = "Saving";
    job->path = path;
    job->cancellable = false;   // a half-written file helps nobody
    BackgroundJob* j = job.get();
    const Document* d = doc.get();
    const int quality = jpeg_quality;
    // Safe because the modal stops the document changing and the action
    // layer refuses to run while a job is going.
    j->done = std::async(std::launch::async, [j, d, quality] {
        return io::save_document(*d, j->path, &j->error, quality);
    });
    status = "Saving " + path + "...";
}

void App::request_open() {
    file_op = PendingFileOp::Open;
    file_dialog.open(FileDialog::Mode::Open, "Open Image", io::load_extensions(), doc_path);
}

void App::request_save_as() {
    if (!doc) return;
    file_op = PendingFileOp::SaveAs;
    // Layered documents default to the project format (.ora); a classic
    // file stays classic; single-layer ones keep their format.
    std::string suggested = doc_path.empty() ? "untitled" : doc_path;
    const bool layered = doc->layer_count() > 1 || !doc->layer(0).background;
    if (doc_path.empty() || (layered && !io::is_psp_extension(doc_path) && !io::is_ora_extension(doc_path))) {
        const auto dot = suggested.rfind('.');
        const auto slash = suggested.find_last_of("/\\");
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) suggested.resize(dot);
        suggested += layered ? ".ora" : ".png";
    }
    file_dialog.open(FileDialog::Mode::Save, "Save As", io::save_extensions(), suggested);
}

void App::save() {
    if (!doc) return;
    // Only save in place to a format we can write; an opened .gif goes through Save As.
    const auto& exts = io::save_extensions();
    const auto dot = doc_path.rfind('.');
    std::string ext = dot == std::string::npos ? "" : doc_path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!doc_path.empty() && std::find(exts.begin(), exts.end(), ext) != exts.end()) save_document_async(doc_path);
    else request_save_as();
}

void App::run(std::unique_ptr<Command> cmd) {
    if (!doc) return;
    say(cmd->name());
    const int a = active_layer();
    const bool was_deep = a >= 0 && doc->layer(a).is_deep();
    history.run(*doc, std::move(cmd));
    if (was_deep && a < static_cast<int>(doc->layer_count()) && !doc->layer(a).is_deep() && doc->layer(a).is_raster())
        status += " (this operation runs at 8 bits per channel; the layer was reduced)";
}

void App::commit(std::unique_ptr<Command> cmd) {
    if (!doc) return;
    say(cmd->name());
    history.push_applied(std::move(cmd));
}

void App::select_tool(int index) {
    if (index < 0 || index >= static_cast<int>(tools.size()) || index == tool_index) return;
    tool().cancel(*this);
    active_button = -1;
    prev_tool_index = tool_index;
    tool_index = index;
}

void App::zoom_about(ImVec2 screen, float factor) {
    const float old_zoom = zoom;
    zoom = std::clamp(zoom * factor, 0.01f, 64.0f);
    const float k = zoom / old_zoom;
    const float mx = screen.x - canvas_center.x, my = screen.y - canvas_center.y;
    pan_x = mx - (mx - pan_x) * k;
    pan_y = my - (my - pan_y) * k;
}

void App::undo() {
    set_selection_edit(false);
    tool().cancel(*this);
    if (doc && history.can_undo()) {
        status = "Undo " + history.at(history.cursor() - 1).name();
        history.undo(*doc);
        refresh_mask_proxy();
    }
}

void App::redo() {
    set_selection_edit(false);
    tool().cancel(*this);
    if (doc && history.can_redo()) {
        status = "Redo " + history.at(history.cursor()).name();
        history.redo(*doc);
        refresh_mask_proxy();
    }
}

// The mask proxy mirrors the document; anything that changes the mask
// outside a stroke (undo, redo, mask commands) must rebuild it.
void App::refresh_mask_proxy() {
    if (!mask_edit) return;
    if (!doc || mask_proxy_layer >= doc->layer_count() || !doc->layer(mask_proxy_layer).has_mask()) { set_mask_edit(false); return; }
    mask_proxy = mask_to_image(doc->layer(mask_proxy_layer).mask);
}

int App::active_layer() const { return doc ? doc->active_layer() : -1; }

bool App::active_is_raster() const { return doc && active_layer() >= 0 && doc->layer(active_layer()).is_raster(); }

// --- Selections ----------------------------------------------------------

void App::set_selection(const char* name, Mask m) {
    if (!doc) return;
    run(std::make_unique<SelectionCommand>(name, std::move(m)));
}

void App::apply_selection_gesture(const char* name, Mask shape) {
    if (!doc) return;
    if (sel_feather > 0.0f) mask::feather(shape, sel_feather);
    Mask result = doc->selection();
    mask::combine(result, shape, static_cast<mask::Combine>(sel_mode));
    set_selection(name, std::move(result));
}

void App::select_all() {
    if (doc) set_selection("Select All", Mask(doc->width(), doc->height(), 255));
}

void App::select_none() {
    if (doc && doc->has_selection()) set_selection("Select None", Mask());
}

void App::select_invert() {
    if (!doc) return;
    Mask m = doc->has_selection() ? doc->selection() : Mask(doc->width(), doc->height(), 0);
    mask::invert(m);
    set_selection("Invert Selection", std::move(m));
}

// --- Clipboard -----------------------------------------------------------

// Puts a document-sized image on both clipboards, cut to the selection.
void App::copy_image(Image out, const char* what) {
    raster::Rect bounds{0, 0, doc->width(), doc->height()};
    if (doc->has_selection()) {
        const Mask& sel = doc->selection();
        bounds = sel.bounds();
        uint8_t* p = out.data();
        for (size_t i = 0; i < sel.size(); ++i) p[i * 4 + 3] = static_cast<uint8_t>((p[i * 4 + 3] * sel.data()[i] + 127) / 255);
    }
    clipboard = {std::move(out), bounds};
    // The same pixels go to the system clipboard for other programs.
    const Image cropped = raster::crop(clipboard.pixels, bounds);
    status = clipboard::write_image(cropped) ? what : what + std::string(" (internal only: ") + clipboard::unavailable_reason() + ")";
}

void App::copy() {
    if (!doc || active_layer() < 0) return;
    copy_image(doc->layer(active_layer()).pixels, "Copied");
}

void App::copy_merged() {
    if (!doc) return;
    copy_image(doc->composite(), "Copied merged");
}

// The clipboard scaled to the selection's bounding box and painted through
// the selection, so it takes the selection's shape and soft edge.
void App::paste_into_selection() {
    if (!doc || !active_is_raster()) { status = "Paste Into Selection needs a raster layer."; return; }
    if (!doc->has_selection()) { status = "Paste Into Selection needs a selection."; return; }
    Image src;
    raster::Rect b;
    if (!clipboard_for_paste(src, b)) { status = "Nothing to paste"; return; }
    const Mask& sel = doc->selection();
    const raster::Rect box = sel.bounds();
    if (box.empty()) { status = "Nothing selected"; return; }
    const int bw = box.x1 - box.x0, bh = box.y1 - box.y0;
    Image piece = raster::crop(src, b);
    if (piece.empty()) { status = "Nothing to paste"; return; }
    if (piece.width() != bw || piece.height() != bh) piece = raster::resample(piece, bw, bh, raster::Filter::Bilinear);
    const size_t layer = active_layer();
    Image& px = paint_pixels(layer);
    Image before = px;
    for (int y = box.y0; y < box.y1; ++y)
        for (int x = box.x0; x < box.x1; ++x) {
            const float m = sel.at(x, y) / 255.0f;
            if (m <= 0.0f) continue;
            raster::blend_over(px, x, y, piece.get(x - box.x0, y - box.y0), m);
        }
    paint_touched(layer, &box);
    commit_pixels(layer, "Paste Into Selection", std::move(before), px);
    status = "Pasted into the selection";
}

// Throws away every change and loads the file again. The document object is
// replaced, so anything pointing into the old one is reset first.
void App::revert() {
    if (!doc || doc_path.empty()) { status = "Revert needs a file that has been saved."; return; }
    std::string err;
    std::vector<std::string> warnings;
    auto fresh = io::load_document(doc_path, &err, &warnings);
    if (!fresh) { status = "Revert failed: " + err; return; }
    tool().cancel(*this);
    mask_edit = false;
    selection_edit = false;
    preview = Preview{};
    crop_rect = {};
    doc = std::move(fresh);
    history = CommandStack();
    history.set_memory_limit(config.undo_memory_mb * 1024ull * 1024ull);
    saved_state = 0;
    canvas_tex_revision = ~0ull;
    status = "Reverted to the saved " + doc_path;
    for (const std::string& w : warnings) status += "\n" + w;
}

// Centers `r` in the canvas view and zooms so it fills it.
void App::zoom_to_rect(raster::Rect r) {
    if (!doc || r.empty() || canvas_view_size.x < 32.0f || canvas_view_size.y < 32.0f) return;
    const float rw = static_cast<float>(r.x1 - r.x0), rh = static_cast<float>(r.y1 - r.y0);
    zoom = std::clamp(std::min(canvas_view_size.x / rw, canvas_view_size.y / rh) * 0.9f, 0.01f, 64.0f);
    fit_requested = false;
    const float cx = (r.x0 + r.x1) * 0.5f, cy = (r.y0 + r.y1) * 0.5f;
    pan_x = (doc->width() * 0.5f - cx) * zoom;
    pan_y = (doc->height() * 0.5f - cy) * zoom;
}

void App::zoom_to_selection() {
    if (!doc || !doc->has_selection()) { status = "Zoom to Selection needs a selection."; return; }
    zoom_to_rect(doc->selection().bounds());
}

// Selections > Content-Aware Fill: synthesizes the selected area from the
// rest of the layer, which is how an unwanted object is removed.
void App::content_aware_fill(bool background) {
    if (!doc || !active_is_raster()) { status = "Content-Aware Fill needs a raster layer."; return; }
    if (!doc->has_selection() || !doc->selection().any()) { status = "Content-Aware Fill needs a selection."; return; }
    if (job) { status = job->name + " is still running."; return; }
    const Mask area = doc->selection();
    const size_t layer = static_cast<size_t>(active_layer());
    if (!background) {
        // Scripts want it finished when the call returns.
        status = "Filling from the surrounding picture...";
        run(std::make_unique<AdjustCommand>(layer, "Content-Aware Fill",
                                            [area](Image& i) { inpaint::content_aware_fill(i, area); }));
        return;
    }
    // Off the interface thread, so the window keeps drawing and the fill can
    // be called off. The worker owns `result` until its future is ready.
    job = std::make_unique<BackgroundJob>();
    job->name = "Content-Aware Fill";
    job->layer = layer;
    job->result = doc->layer(layer).pixels;
    BackgroundJob* j = job.get();
    j->done = std::async(std::launch::async, [j, area] {
        inpaint::Options opt;
        opt.on_progress = [j](float p) {
            j->progress.store(p, std::memory_order_relaxed);
            return !j->cancel.load(std::memory_order_relaxed);
        };
        return inpaint::content_aware_fill(j->result, area, opt);
    });
    status = "Filling from the surrounding picture...";
}

bool App::generate_configured() const {
    return !config.generate_url.empty() && firn::genhttp::available();
}

void App::generative_fill(const std::string& prompt, bool background) {
    if (!doc || !active_is_raster()) { status = "Generative Fill needs a raster layer."; return; }
    if (!doc->has_selection() || !doc->selection().any()) { status = "Generative Fill needs a selection."; return; }
    if (config.generate_url.empty()) { status = "Generative Fill: set a server address in Preferences first."; return; }
    if (!firn::genhttp::available()) { status = "Generative Fill needs curl, which is not installed."; return; }
    if (job) { status = job->name + " is still running."; return; }

    const size_t layer = static_cast<size_t>(active_layer());
    gen::Request req;
    req.name = "Generative Fill";
    req.init = doc->layer(layer).pixels;
    req.region = doc->selection();
    req.disposition = gen::Disposition::IntoRegion;
    req.feather = 6.0f;
    req.revision = doc->revision();
    req.layer = static_cast<int>(layer);
    if (!prompt.empty()) req.params.set("prompt", json::Value::string(prompt));
    req.params.set("strength", json::Value::number(generate_strength));
    if (generate_seed >= 0) req.params.set("seed", json::Value::number(generate_seed));

    gen::Backend send = firn::genhttp::backend(config.generate_url);
    if (!background) {
        // A script wants the work finished when the call returns.
        gen::Progress p;
        gen::Result r = send(req, p);
        if (!r.ok) { status = "Generative Fill: " + (r.error.empty() ? std::string("no result") : r.error); return; }
        Image out = req.init;
        gen::composite_into(out, r.image, req.region, req.feather);
        run(std::make_unique<AdjustCommand>(layer, "Generative Fill", [img = std::move(out)](Image& i) { i = img; }));
        status = "Generative Fill done";
        return;
    }

    job = std::make_unique<BackgroundJob>();
    job->name = "Generative Fill";
    job->layer = layer;
    job->result = req.init;
    BackgroundJob* j = job.get();
    j->done = std::async(std::launch::async, [j, req = std::move(req), send = std::move(send)]() mutable {
        gen::Progress p;
        // The service decides whether it will still take a cancel; the
        // button follows what it says rather than the other way round.
        std::atomic<bool>* cancel = &j->cancel;
        std::thread relay([&p, cancel, j] {
            while (p.status.load() != gen::Status::Done && p.status.load() != gen::Status::Failed) {
                if (cancel->load()) p.cancel.store(true);
                j->progress.store(std::max(0.0f, p.fraction.load()), std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        gen::Result r = send(req, p);
        p.status.store(gen::Status::Done);
        relay.join();
        if (!r.ok) { j->error = r.error; return false; }
        gen::composite_into(j->result, r.image, req.region, req.feather);
        return true;
    });
    status = "Asking the model...";
}

void App::generative_edit(const std::string& prompt, bool background) {
    if (!doc || !active_is_raster()) { status = "Generative Edit needs a raster layer."; return; }
    if (prompt.empty()) { status = "Generative Edit needs an instruction."; return; }
    if (config.generate_url.empty()) { status = "Generative Edit: set a server address in Preferences first."; return; }
    if (!firn::genhttp::available()) { status = "Generative Edit needs curl, which is not installed."; return; }
    if (job) { status = job->name + " is still running."; return; }

    const size_t layer = static_cast<size_t>(active_layer());
    gen::Request req;
    req.name = "Generative Edit";
    req.init = doc->layer(layer).pixels;
    req.conditioning = gen::Conditioning::Reference;
    req.disposition = gen::Disposition::ReplaceLayer;   // the model returns the whole picture
    req.revision = doc->revision();
    req.layer = static_cast<int>(layer);
    req.params.set("prompt", json::Value::string(prompt));
    if (generate_seed >= 0) req.params.set("seed", json::Value::number(generate_seed));

    gen::Backend send = firn::genhttp::backend(config.generate_url);
    if (!background) {
        gen::Progress p;
        gen::Result r = send(req, p);
        if (!r.ok) { status = "Generative Edit: " + (r.error.empty() ? std::string("no result") : r.error); return; }
        // Whole-picture: the region is empty, so this takes the frame entire
        // after bringing it back to the layer's size.
        Image out = req.init;
        gen::composite_into(out, r.image, Mask(), 0.0f);
        run(std::make_unique<AdjustCommand>(layer, "Generative Edit", [img = std::move(out)](Image& i) { i = img; }));
        status = "Generative Edit done";
        return;
    }

    job = std::make_unique<BackgroundJob>();
    job->name = "Generative Edit";
    job->layer = layer;
    job->result = req.init;
    BackgroundJob* j = job.get();
    j->done = std::async(std::launch::async, [j, req = std::move(req), send = std::move(send)]() mutable {
        gen::Progress p;
        std::atomic<bool>* cancel = &j->cancel;
        std::thread relay([&p, cancel, j] {
            while (p.status.load() != gen::Status::Done && p.status.load() != gen::Status::Failed) {
                if (cancel->load()) p.cancel.store(true);
                j->progress.store(std::max(0.0f, p.fraction.load()), std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        gen::Result r = send(req, p);
        p.status.store(gen::Status::Done);
        relay.join();
        if (!r.ok) { j->error = r.error; return false; }
        gen::composite_into(j->result, r.image, Mask(), 0.0f);
        return true;
    });
    status = "Asking the model...";
}

void App::draw_background_job() {
    if (!job) return;
    if (!job->opened) { ImGui::OpenPopup("Working"); job->opened = true; }
    const bool ready = job->done.valid() && job->done.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    if (ImGui::BeginPopupModal("Working", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted(job->name.c_str());
        if (job->cancellable) {
            ImGui::ProgressBar(job->progress.load(std::memory_order_relaxed), ImVec2(280, 0));
            const bool cancelling = job->cancel.load(std::memory_order_relaxed);
            ImGui::BeginDisabled(cancelling);
            if (ImGui::Button("Cancel", ImVec2(90, 0))) job->cancel.store(true, std::memory_order_relaxed);
            ImGui::EndDisabled();
            if (cancelling) { ImGui::SameLine(); ImGui::TextDisabled("Stopping..."); }
        } else {
            // Nothing useful to report and nothing safe to interrupt, so an
            // indeterminate bar rather than a fake number.
            const float t = static_cast<float>(ImGui::GetTime());
            ImGui::ProgressBar(-0.4f * t, ImVec2(280, 0), "");
        }
        if (ready) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (!ready) return;

    const bool finished = job->done.get();
    if (job->kind == BackgroundJob::Kind::Save) {
        if (finished) after_saved(job->path);
        else fail("Save failed: " + (job->error.empty() ? std::string("unknown error") : job->error));
    } else if (job->kind == BackgroundJob::Kind::Open) {
        if (!finished) {
            fail("Open failed: " + (job->error.empty() ? std::string("unknown error") : job->error));
        } else {
            add_document(std::move(job->loaded), job->path);
            config.touch_recent(job->path);
            config.last_directory = file_dialog.directory();
            status = "Opened " + job->path;
            for (const std::string& w : job->warnings) status += "\n" + w;
        }
    } else {
        const size_t layer = job->layer;
        if (!finished) status = job->error.empty() ? job->name + " cancelled" : job->name + ": " + job->error;
        else if (!doc || layer >= doc->layer_count() || !doc->layer(layer).is_raster()) status = job->name + ": the layer is gone";
        else {
            // The work is done, so the command only has to hand the pixels
            // over; a redo copies them rather than filling again.
            run(std::make_unique<AdjustCommand>(layer, job->name,
                                                [img = std::move(job->result)](Image& i) { i = img; }));
            status = job->name + " done";
        }
    }
    job.reset();
}

void App::repeat_last_effect() {
    if (!last_effect_op) { status = "Nothing to repeat yet."; return; }
    if (!doc || !active_is_raster()) { status = "Repeat needs a raster layer."; return; }
    run(std::make_unique<AdjustCommand>(active_layer(), last_effect, last_effect_op, last_effect_op16));
}

// Pixels to paste: the system clipboard when another program filled it,
// otherwise the internal one (which keeps the copied region's position).
bool App::clipboard_for_paste(Image& px, raster::Rect& bounds) {
    std::optional<Image> sys = clipboard::read_image();
    if (sys && !sys->empty()) {
        bool ours = false;
        if (!clipboard.empty()) {
            const raster::Rect b = clipboard.bounds;
            if (sys->width() == b.x1 - b.x0 && sys->height() == b.y1 - b.y0) {
                const Image cropped = raster::crop(clipboard.pixels, b);
                ours = std::memcmp(cropped.data(), sys->data(), cropped.size_bytes()) == 0;
            }
        }
        if (!ours) { bounds = {0, 0, sys->width(), sys->height()}; px = std::move(*sys); return true; }
    }
    if (clipboard.empty()) return false;
    px = clipboard.pixels;
    bounds = clipboard.bounds;
    return !bounds.empty();
}

void App::clear_selection() {
    if (!doc || active_layer() < 0) return;
    const Layer& L = doc->layer(active_layer());
    // The original clears a Background layer to the background color.
    auto c = [](float f) { return static_cast<uint8_t>(f * 255.0f + 0.5f); };
    const Color fill = L.background ? Color{c(bg_color[0]), c(bg_color[1]), c(bg_color[2]), 255} : Color{0, 0, 0, 0};
    run(std::make_unique<ClearCommand>(active_layer(), fill));
}

void App::cut() {
    copy();
    clear_selection();
    status = "Cut";
}

void App::paste_as_new_layer() {
    if (!doc) return;
    Image src;
    raster::Rect b;
    if (!clipboard_for_paste(src, b)) { status = "Nothing to paste"; return; }
    // The internal clipboard is document-sized (its region stays in place);
    // an image from another program is placed at the top-left.
    Image px(doc->width(), doc->height());
    const int w = std::min(px.width(), src.width()), h = std::min(px.height(), src.height());
    for (int y = 0; y < h; ++y)
        std::memcpy(px.data() + static_cast<size_t>(y) * px.width() * 4, src.data() + static_cast<size_t>(y) * src.width() * 4, static_cast<size_t>(w) * 4);
    run(std::make_unique<PasteLayerCommand>("Raster " + std::to_string(doc->layer_count()), std::move(px)));
}

void App::paste_as_new_image() {
    Image src;
    raster::Rect b;
    if (!clipboard_for_paste(src, b)) { status = "Nothing to paste"; return; }
    auto d = std::make_unique<Document>(b.x1 - b.x0, b.y1 - b.y0);
    Layer& L = d->add_layer("Raster 1");
    for (int y = b.y0; y < b.y1; ++y)
        std::memcpy(L.pixels.data() + static_cast<size_t>(y - b.y0) * L.pixels.width() * 4,
                    src.data() + (static_cast<size_t>(y) * src.width() + b.x0) * 4, static_cast<size_t>(b.x1 - b.x0) * 4);
    add_document(std::move(d), "");
    status = "Pasted as new image";
}

// --- Layers --------------------------------------------------------------

void App::layer_new() {
    if (doc) run(std::make_unique<AddLayerCommand>("Raster " + std::to_string(doc->layer_count())));
}

void App::layer_duplicate() {
    if (doc && active_layer() >= 0) run(std::make_unique<DuplicateLayerCommand>(active_layer()));
}

void App::layer_delete() {
    if (doc && active_layer() >= 0 && doc->layer_count() > 1) run(std::make_unique<RemoveLayerCommand>(active_layer()));
}

void App::layer_arrange(int delta) {
    if (!doc || active_layer() < 0 || delta == 0) return;
    run(std::make_unique<ArrangeLayerCommand>(active_layer(), delta));
}

void App::layer_move_onto(int from, int onto) {
    if (!doc || from == onto || from < 0 || onto < 0) return;
    const int n = static_cast<int>(doc->layer_count());
    if (from >= n || onto >= n) return;
    // Land directly on top of the target, at its depth. A group is stepped
    // over as a whole, so the moved layer becomes its sibling rather than
    // splitting it from its members.
    const Layer& target = doc->layer(onto);
    const size_t before = target.type == LayerType::Group ? doc->group_end(onto) : static_cast<size_t>(onto) + 1;
    if (doc->layer(from).type == LayerType::Group && before > static_cast<size_t>(from) && before < doc->group_end(from)) {
        status = "A group cannot be moved into itself.";
        return;
    }
    run(std::make_unique<MoveLayerCommand>(from, before, target.depth));
}

void App::layer_merge(int kind) {
    if (!doc) return;
    using K = MergeLayersCommand::Kind;
    if (kind == 0) {
        if (active_layer() > 0) run(std::make_unique<MergeLayersCommand>(K::Down, active_layer()));
    } else if (kind == 1) {
        run(std::make_unique<MergeLayersCommand>(K::Visible));
    } else {
        run(std::make_unique<MergeLayersCommand>(K::All));
    }
}

void App::layer_new_group() {
    if (doc && active_layer() >= 0) run(std::make_unique<NewLayerGroupCommand>(active_layer()));
}

void App::layer_ungroup() {
    if (doc && active_layer() >= 0 && doc->layer(active_layer()).type == LayerType::Group)
        run(std::make_unique<UngroupCommand>(active_layer()));
}

void App::layer_set_mask(const char* name, Mask m, bool enabled) {
    if (doc && active_layer() >= 0) run(std::make_unique<SetMaskCommand>(active_layer(), name, std::move(m), enabled));
    refresh_mask_proxy();
}

void App::layer_mask_from_selection() {
    if (!doc || active_layer() < 0) return;
    Mask m = doc->has_selection() ? doc->selection() : Mask(doc->width(), doc->height(), 255);
    layer_set_mask("New Mask Layer", std::move(m));
}

void App::layer_mask_from_image() {
    if (!doc || active_layer() < 0) return;
    const Image flat = doc->composite();
    Mask m(doc->width(), doc->height());
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x) {
            const Color c = flat.get(x, y);
            m.at(x, y) = static_cast<uint8_t>((c.r * 299 + c.g * 587 + c.b * 114 + 500) / 1000 * c.a / 255);
        }
    layer_set_mask("New Mask Layer", std::move(m));
}

// --- Custom brush tips ------------------------------------------------------

void App::ensure_brush_tips() {
    if (brush_tips_loaded) return;
    brush_tips_loaded = true;
    namespace fs = std::filesystem;
    std::vector<fs::path> dirs;
    if (!config.extra_brush_dir.empty()) dirs.emplace_back(config.extra_brush_dir);
    if (const char* extra = std::getenv("FIRN_BRUSH_DIRS")) {
        std::string s = extra;
        size_t start = 0;
        while (start <= s.size()) {
            const size_t end = s.find(':', start);
            dirs.emplace_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    dirs.emplace_back(fs::path(Config::directory()) / "brushes");
#ifdef FIRN_SOURCE_DIR
    dirs.emplace_back(fs::path(FIRN_SOURCE_DIR) / "WindowsInstall" / "Brushes");
#endif
    for (const fs::path& d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) continue;
        for (const auto& de : fs::recursive_directory_iterator(d, fs::directory_options::skip_permission_denied, ec)) {
            if (!de.is_regular_file(ec)) continue;
            std::string ext = de.path().extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".pspbrush" && ext != ".png") continue;
            brush_tips.push_back({de.path().string(), de.path().stem().string(), nullptr});
        }
    }
    std::sort(brush_tips.begin(), brush_tips.end(), [](const TipEntry& a, const TipEntry& b) { return a.name < b.name; });
}

void App::select_brush_tip(int index) {
    if (index < 0 || index >= static_cast<int>(brush_tips.size())) { brush_tip_index = -1; brush.tip.reset(); return; }
    TipEntry& e = brush_tips[index];
    if (!e.tip) {
        std::string err;
        auto d = io::load_document(e.path, &err, nullptr);
        if (!d) { status = "Brush tip failed: " + err; return; }
        e.tip = raster::BrushTip::from_image(d->composite());
    }
    brush_tip_index = index;
    brush.tip = e.tip;
}

void App::brush_tip_from_selection() {
    if (!doc || !doc->has_selection()) return;
    const raster::Rect r = doc->selection().bounds();
    Image cut = raster::crop(doc->composite(), r);
    // Selection coverage becomes the tip's alpha.
    for (int y = 0; y < cut.height(); ++y)
        for (int x = 0; x < cut.width(); ++x) {
            uint8_t* p = cut.data() + (static_cast<size_t>(y) * cut.width() + x) * 4;
            p[3] = static_cast<uint8_t>(p[3] * doc->selection().at(x + r.x0, y + r.y0) / 255);
        }
    ensure_brush_tips();
    brush_tips.insert(brush_tips.begin(), {"", "From selection " + std::to_string(cut.width()) + "x" + std::to_string(cut.height()), raster::BrushTip::from_image(cut)});
    brush_tip_index = 0;
    brush.tip = brush_tips[0].tip;
    status = "Brush tip created from the selection.";
}

// --- Paper textures --------------------------------------------------------

void App::ensure_textures() {
    if (textures_loaded) return;
    textures_loaded = true;
    namespace fs = std::filesystem;
    std::vector<fs::path> dirs;
    if (!config.extra_texture_dir.empty()) dirs.emplace_back(config.extra_texture_dir);
    if (const char* extra = std::getenv("FIRN_TEXTURE_DIRS")) {
        std::string s = extra;
        size_t start = 0;
        while (start <= s.size()) {
            const size_t end = s.find(':', start);
            dirs.emplace_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    dirs.emplace_back(fs::path(Config::directory()) / "textures");
#ifdef FIRN_SOURCE_DIR
    dirs.emplace_back(fs::path(FIRN_SOURCE_DIR) / "WindowsInstall" / "Textures");
#endif
    for (const fs::path& d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) continue;
        for (const auto& de : fs::recursive_directory_iterator(d, fs::directory_options::skip_permission_denied, ec)) {
            if (!de.is_regular_file(ec)) continue;
            std::string ext = de.path().extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".bmp" && ext != ".png" && ext != ".jpg" && ext != ".jpeg") continue;
            textures.push_back({de.path().string(), de.path().stem().string(), nullptr, nullptr});
        }
    }
    std::sort(textures.begin(), textures.end(), [](const TextureEntry& a, const TextureEntry& b) { return a.name < b.name; });
}

std::shared_ptr<const Image> App::texture_image(int index) {
    ensure_textures();
    if (index < 0 || index >= static_cast<int>(textures.size())) return nullptr;
    TextureEntry& e = textures[index];
    if (!e.image) {
        std::string err;
        auto img = io::load(e.path, &err);
        if (!img) { status = "Texture failed: " + err; return nullptr; }
        e.image = std::make_shared<const Image>(std::move(*img));
    }
    return e.image;
}

void App::select_texture(int index) {
    if (index < 0 || index >= static_cast<int>(textures.size())) { texture_index = -1; brush.texture.reset(); return; }
    TextureEntry& e = textures[index];
    if (!e.texture) {
        auto img = texture_image(index);
        if (!img) return;
        e.texture = raster::BrushTip::texture_from_image(*img);
    }
    texture_index = index;
    brush.texture = e.texture;
}

// --- Picture tubes ---------------------------------------------------------

void App::ensure_tubes() {
    if (tubes_loaded) return;
    tubes_loaded = true;
    namespace fs = std::filesystem;
    std::vector<fs::path> dirs;
    if (!config.extra_tube_dir.empty()) dirs.emplace_back(config.extra_tube_dir);
    if (const char* extra = std::getenv("FIRN_TUBE_DIRS")) {
        std::string s = extra;
        size_t start = 0;
        while (start <= s.size()) {
            const size_t end = s.find(':', start);
            dirs.emplace_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    dirs.emplace_back(fs::path(Config::directory()) / "tubes");
#ifdef FIRN_SOURCE_DIR
    dirs.emplace_back(fs::path(FIRN_SOURCE_DIR) / "WindowsInstall" / "Picture Tubes");
#endif
    for (const fs::path& d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) continue;
        for (const auto& de : fs::recursive_directory_iterator(d, fs::directory_options::skip_permission_denied, ec)) {
            if (!de.is_regular_file(ec)) continue;
            std::string ext = de.path().extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".psptube") continue;
            tubes.push_back({de.path().string(), de.path().stem().string()});
        }
    }
    std::sort(tubes.begin(), tubes.end(), [](const TubeEntry& a, const TubeEntry& b) { return a.name < b.name; });
    if (!tubes.empty() && tube_index < 0) load_tube(0);
}

bool App::export_tube(const std::string& path, const io::TubeInfo& info) {
    if (!doc) { status = "Export Picture Tube: no image open."; return false; }
    std::string err;
    if (!io::save_psp_tube(*doc, info, path, &err)) { status = "Export Picture Tube: " + err; return false; }
    // A tube written into a folder Firn scans should show up in the tool
    // without a restart.
    tubes_loaded = false;
    tubes.clear();
    tube_index = -1;
    tube_loaded_path.clear();
    status = "Exported " + std::to_string(info.total) + " cell" + (info.total == 1 ? "" : "s") + " to " + path;
    return true;
}

bool App::load_tube(int index) {
    if (index < 0 || index >= static_cast<int>(tubes.size())) return false;
    std::string err;
    auto d = io::load_psp(tubes[index].path, &err, nullptr);
    if (!d) { status = "Tube failed: " + err; return false; }
    tube_image = d->composite();
    tube_info = io::load_psp_tube_info(tubes[index].path).value_or(io::TubeInfo{});
    tube_index = index;
    tube_loaded_path = tubes[index].path;
    return true;
}

// --- Mask editing ----------------------------------------------------------

namespace {
Image mask_to_image_impl(const Mask& m) {
    Image img(m.width(), m.height());
    for (size_t i = 0; i < m.size(); ++i) {
        uint8_t* p = img.data() + i * 4;
        p[0] = p[1] = p[2] = m.data()[i];
        p[3] = 255;
    }
    return img;
}
Mask image_to_mask(const Image& img) {
    Mask m(img.width(), img.height());
    for (size_t i = 0; i < m.size(); ++i) {
        const uint8_t* p = img.data() + i * 4;
        m.data()[i] = static_cast<uint8_t>((p[0] * 299 + p[1] * 587 + p[2] * 114 + 500) / 1000 * p[3] / 255);
    }
    return m;
}
}  // namespace

static Image mask_to_image(const Mask& m) { return mask_to_image_impl(m); }

// Edit Selection: the selection becomes a grayscale proxy that every
// painting tool works on (white selects), shown as a red overlay; the
// document's own selection is cleared meanwhile so nothing clips to it.
void App::set_selection_edit(bool on) {
    if (!doc) return;
    tool().cancel(*this);
    if (on && !selection_edit) {
        if (mask_edit) set_mask_edit(false);
        selection_edit = true;
        selection_edit_before = doc->selection();
        mask_proxy = mask_to_image(doc->has_selection() ? doc->selection() : Mask(doc->width(), doc->height(), 0));
        doc->set_selection(Mask());
        overlay_tex_revision = ~0ull;
        status = "Editing the selection: paint white to select, black to deselect. Selections > Edit Selection again to finish.";
    } else if (!on && selection_edit) {
        selection_edit = false;
        Mask after = image_to_mask(mask_proxy);
        mask_proxy = Image();
        doc->set_selection(selection_edit_before);
        if (!after.any()) after = Mask();
        const bool unchanged = after.width() == selection_edit_before.width() &&
            after.height() == selection_edit_before.height() &&
            (after.size() == 0 || std::memcmp(after.data(), selection_edit_before.data(), after.size()) == 0);
        // Leaving an untouched edit mode must not discard a pending redo branch.
        if (!unchanged) set_selection("Edit Selection", std::move(after));
        overlay_tex_revision = ~0ull;
    }
}

// Pen presence fades after a few seconds without reports; the eraser tip
// picks the Eraser tool and puts the previous tool back when it lifts.
void App::pen_tick() {
    if (pen.present && ImGui::GetTime() - pen.last_seen > 3.0) { pen.present = false; pen.eraser = false; }
    if (active_button >= 0) return;   // never switch tools mid-gesture
    const bool on_eraser = std::strcmp(tool().name(), "Eraser") == 0;
    if (pen.present && pen.eraser && !on_eraser && pen_prev_tool < 0) {
        for (size_t i = 0; i < tools.size(); ++i)
            if (std::strcmp(tools[i]->name(), "Eraser") == 0) { pen_prev_tool = tool_index; select_tool(static_cast<int>(i)); break; }
    } else if ((!pen.eraser || !pen.present) && pen_prev_tool >= 0) {
        if (on_eraser) select_tool(pen_prev_tool);
        pen_prev_tool = -1;
    }
}

void App::layer_view_only(bool current_only) {
    if (!doc || active_layer() < 0) return;
    const int cur = active_layer();
    run(std::make_unique<StateEditCommand>(current_only ? "View Current Only" : "View All", [cur, current_only](Document& d) {
        for (size_t i = 0; i < d.layer_count(); ++i) d.layer(i).visible = !current_only || static_cast<int>(i) == cur || d.layer(i).type == LayerType::Group;
    }));
}

void App::set_mask_edit(bool on) {
    tool().cancel(*this);
    if (on && selection_edit) set_selection_edit(false);
    if (on && doc && active_layer() >= 0 && doc->layer(active_layer()).has_mask()) {
        mask_edit = true;
        mask_proxy_layer = active_layer();
        mask_proxy = mask_to_image(doc->layer(mask_proxy_layer).mask);
        status = "Editing the mask of \"" + doc->layer(mask_proxy_layer).name + "\": paint black to hide, white to show.";
    } else {
        mask_edit = false;
        if (!selection_edit) mask_proxy = Image();
    }
}

Image& App::paint_pixels(size_t layer) {
    if (selection_edit) return mask_proxy;
    if (mask_edit && layer == mask_proxy_layer && doc && layer < doc->layer_count() && doc->layer(layer).has_mask()) return mask_proxy;
    return doc->layer(layer).pixels;
}

void App::paint_touched(size_t layer, const raster::Rect* rect) {
    if (selection_edit) { overlay_tex_revision = ~0ull; doc->touch(); return; }
    if (mask_edit && layer == mask_proxy_layer && doc && layer < doc->layer_count() && doc->layer(layer).has_mask())
        doc->layer(layer).mask = image_to_mask(mask_proxy);
    if (rect) doc->touch(*rect);
    else doc->touch();
}

void App::commit_pixels(size_t layer, const std::string& name, Image before, const Image& after) {
    if (selection_edit) {
        // The proxy is the selection being edited; one command when editing ends.
        (void)layer; (void)name; (void)before;
        mask_proxy = after;
        overlay_tex_revision = ~0ull;
        doc->touch();
        return;
    }
    if (mask_edit && layer == mask_proxy_layer && doc && doc->layer(layer).has_mask()) {
        // Record the mask change; the proxy already holds `after`.
        Mask before_mask = image_to_mask(before);
        Mask after_mask = image_to_mask(after);
        doc->layer(layer).mask = before_mask;  // command computes the delta from the document state
        run(std::make_unique<SetMaskCommand>(layer, name + " (Mask)", std::move(after_mask), doc->layer(layer).mask_enabled));
        return;
    }
    auto cmd = std::make_unique<LayerSnapshotCommand>(layer, name, std::move(before), after);
    if (doc->layer(layer).is_deep()) { cmd->capture_deep(*doc); status = name + ": the layer is now 8 bits per channel (painting runs at 8 bits)"; }
    commit(std::move(cmd));
}

void App::layer_promote_background() {
    if (doc && active_layer() >= 0 && doc->layer(active_layer()).background)
        run(std::make_unique<PromoteBackgroundCommand>(active_layer()));
}

// Clip the active layer to the one below, or release it. A Background layer
// has nothing under it, and the bottom of a group has nothing in the group
// to clip to.
// What a painting tool may touch on this layer: the selection, narrowed to
// the pixels that already exist when the layer's transparency is protected.
// The narrowed mask is cached because a stroke asks for it on every press.
void App::start_update_check(bool manual) {
    if (update_checking) return;
    if (!update::available()) {
        if (manual) status = "Checking for updates needs curl.";
        return;
    }
    if (!manual) {
        // Once a day at most, and only if the user asked for it.
        if (!config.check_updates) return;
        const long long now = static_cast<long long>(std::time(nullptr));
        if (now - config.last_update_check < 24 * 60 * 60) return;
        config.last_update_check = now;
    }
    update_checking = true;
    update_notified = false;
    update_future = update::check_async(kFirnVersion);
    if (manual) status = "Checking for a newer version...";
}

void App::poll_update_check() {
    if (!update_checking || !update_future.valid()) return;
    if (update_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    update_result = update_future.get();
    update_checking = false;
    // Silent when there is nothing to say or the request failed: nobody
    // editing a picture needs to hear that a server was unreachable.
    if (update_result.checked && update_result.newer && !update_notified) {
        status = "Firn " + update_result.version + " is available (Help > About)";
        update_notified = true;
    }
}

const firn::Mask* App::paint_clip(int layer) {
    if (!doc) return nullptr;
    const Mask& sel = doc->selection();
    if (layer < 0 || static_cast<size_t>(layer) >= doc->layer_count()) return &sel;
    const Layer& L = doc->layer(static_cast<size_t>(layer));
    if (!L.lock_alpha || !L.is_raster() || L.pixels.empty()) return &sel;
    const Image& px = L.pixels;
    Mask m(px.width(), px.height(), 0);
    const bool has_sel = doc->has_selection();
    for (int y = 0; y < px.height(); ++y)
        for (int x = 0; x < px.width(); ++x) {
            const uint8_t a = px.get(x, y).a;
            if (!a) continue;
            m.at(x, y) = has_sel ? static_cast<uint8_t>(a * sel.at(x, y) / 255) : a;
        }
    paint_clip_cache = std::move(m);
    return &paint_clip_cache;
}

bool App::can_clip_layer() const {
    const int i = active_layer();
    // The active index can outlive the stack it pointed into, so check it
    // against the current layer count rather than trusting it.
    if (!doc || i <= 0 || static_cast<size_t>(i) >= doc->layer_count()) return false;
    const Layer& L = doc->layer(static_cast<size_t>(i));
    if (L.background) return false;
    const Layer& below = doc->layer(static_cast<size_t>(i) - 1);
    // The layer below must be at the same depth: the first member of a group
    // sits above the group layer itself, which is not something to clip to.
    return below.depth == L.depth;
}

void App::layer_toggle_clipped() {
    if (!can_clip_layer()) return;
    const size_t i = static_cast<size_t>(active_layer());
    LayerProps before = doc->props(i), after = before;
    after.clipped = !before.clipped;
    layer_set_props(before, after);
    status = after.clipped ? "Clipped to the layer below" : "Released from the layer below";
}

void App::layer_set_props(const LayerProps& before, const LayerProps& after) {
    if (doc && active_layer() >= 0 && !(before == after))
        run(std::make_unique<LayerPropertiesCommand>(active_layer(), before, after));
}

void App::open_layer_properties() {
    if (!doc || active_layer() < 0) return;
    layer_props_edit = doc->props(active_layer());
    show_layer_props_dialog = true;
}

// --- Geometry ------------------------------------------------------------

Color App::background_fill() const {
    auto c = [](float f) { return static_cast<uint8_t>(f * 255.0f + 0.5f); };
    return {c(bg_color[0]), c(bg_color[1]), c(bg_color[2]), 255};
}

void App::crop_to(raster::Rect r) {
    if (!doc) return;
    r = r.clipped(doc->width(), doc->height());
    if (r.empty()) return;
    tool().cancel(*this);
    crop_rect = {};
    run(std::make_unique<CropCommand>(r));
    fit_requested = true;
}

void App::crop_to_selection() {
    if (doc && doc->has_selection()) crop_to(doc->selection().bounds());
}

void App::rotate(float degrees_cw) {
    if (!doc) return;
    tool().cancel(*this);
    run(std::make_unique<RotateCommand>(degrees_cw, background_fill()));
    fit_requested = true;
}

void App::open_resize_dialog() {
    if (!doc) return;
    resize_w = doc->width();
    resize_h = doc->height();
    resize_pct = 100.0f;
    show_resize_dialog = true;
}

void App::open_canvas_dialog() {
    if (!doc) return;
    canvas_w = doc->width();
    canvas_h = doc->height();
    show_canvas_dialog = true;
}

void App::request_load_selection() {
    if (!doc) return;
    file_op = PendingFileOp::LoadSelection;
    file_dialog.open(FileDialog::Mode::Open, "Load Selection From Disk", {"pspselection", "pspimage", "png", "bmp", "jpg", "jpeg", "tga"}, "");
}

void App::request_save_selection() {
    if (!doc || !doc->has_selection()) return;
    file_op = PendingFileOp::SaveSelection;
    file_dialog.open(FileDialog::Mode::Save, "Save Selection To Disk", {"pspselection", "png"}, "selection.pspselection");
}

void App::load_selection(const std::string& path) {
    if (!doc) return;
    std::string err;
    auto src = io::load_document(path, &err, nullptr);
    if (!src) { status = "Load selection failed: " + err; return; }
    const Image flat = src->composite();
    Mask m(doc->width(), doc->height());
    // Selection files are white where selected; any image works via luminance x alpha.
    for (int y = 0; y < m.height() && y < flat.height(); ++y)
        for (int x = 0; x < m.width() && x < flat.width(); ++x) {
            const Color c = flat.get(x, y);
            const int luma = (c.r * 299 + c.g * 587 + c.b * 114 + 500) / 1000;
            m.at(x, y) = static_cast<uint8_t>(luma * c.a / 255);
        }
    set_selection("Load Selection", std::move(m));
    status = "Loaded selection from " + path;
}

void App::save_selection(const std::string& path) {
    if (!doc || !doc->has_selection()) return;
    const Mask& sel = doc->selection();
    Document out(doc->width(), doc->height());
    Layer& L = out.add_layer("Selection");
    for (int y = 0; y < sel.height(); ++y)
        for (int x = 0; x < sel.width(); ++x) L.pixels.set(x, y, {255, 255, 255, sel.at(x, y)});
    std::string err;
    if (!io::save_document(out, path, &err)) { status = "Save selection failed: " + err; return; }
    status = "Saved selection to " + path;
}

// Rebuild the marching-ants edge list when the selection changes. An edge is
// recorded wherever a selected pixel (>=128) borders an unselected one.
void App::sync_ants() {
    if (!doc || !doc->has_selection()) { ants.clear(); ants_revision = doc ? doc->selection_revision() : ~0ull; return; }
    if (ants_revision == doc->selection_revision()) return;
    ants_revision = doc->selection_revision();
    ants.clear();
    const Mask& m = doc->selection();
    const int w = m.width(), h = m.height();
    auto in = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && m.at(x, y) >= 128; };
    for (int y = 0; y <= h; ++y)
        for (int x = 0; x <= w; ++x) {
            if (x < w && in(x, y) != in(x, y - 1)) ants.push_back({x, y, true});
            if (y < h && in(x, y) != in(x - 1, y)) ants.push_back({x, y, false});
        }
}

void App::sync_canvas_texture() {
    if (!doc) {
        if (canvas_tex) { glDeleteTextures(1, &canvas_tex); canvas_tex = 0; }
        canvas_tex_revision = ~0ull;
        composite_cache = Image();
        return;
    }
    if (canvas_tex && canvas_tex_revision == doc->revision()) return;

    // Incremental path: same document size and a known dirty rect.
    const raster::Rect dirty = doc->take_dirty();
    const bool cache_ok = canvas_tex && composite_cache.width() == doc->width() && composite_cache.height() == doc->height() && canvas_tex_revision != ~0ull;
    if (cache_ok && !dirty.empty() && (dirty.x1 - dirty.x0) * (dirty.y1 - dirty.y0) < doc->width() * doc->height()) {
        doc->composite_into(composite_cache, dirty);
        const Image* upload = &composite_cache;
        if (display_needs_transform()) {
            if (display_cache.width() != composite_cache.width() || display_cache.height() != composite_cache.height()) display_cache = composite_cache;
            for (int y = dirty.y0; y < dirty.y1; ++y)
                std::memcpy(display_cache.data() + (static_cast<size_t>(y) * display_cache.width() + dirty.x0) * 4, composite_cache.data() + (static_cast<size_t>(y) * composite_cache.width() + dirty.x0) * 4, static_cast<size_t>(dirty.x1 - dirty.x0) * 4);
            display_transform->apply_rect(display_cache, dirty.x0, dirty.y0, dirty.x1, dirty.y1);
            upload = &display_cache;
        }
        glBindTexture(GL_TEXTURE_2D, canvas_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, upload->width());
        glTexSubImage2D(GL_TEXTURE_2D, 0, dirty.x0, dirty.y0, dirty.x1 - dirty.x0, dirty.y1 - dirty.y0, GL_RGBA, GL_UNSIGNED_BYTE,
                        upload->data() + (static_cast<size_t>(dirty.y0) * upload->width() + dirty.x0) * 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        canvas_tex_revision = doc->revision();
        return;
    }

    const bool reuse_texture = canvas_tex && composite_cache.width() == doc->width() && composite_cache.height() == doc->height();
    composite_cache = doc->composite();
    if (display_needs_transform()) { display_cache = composite_cache; display_transform->apply(display_cache); }
    const Image& composite = display_needs_transform() ? display_cache : composite_cache;
    if (!canvas_tex) {
        glGenTextures(1, &canvas_tex);
        glBindTexture(GL_TEXTURE_2D, canvas_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, canvas_tex);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (reuse_texture)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, composite.width(), composite.height(), GL_RGBA, GL_UNSIGNED_BYTE, composite.data());
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, composite.width(), composite.height(), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, composite.data());
    canvas_tex_revision = doc->revision();
}

// Red tint where the edited mask hides pixels, as a second texture.
void App::sync_overlay_texture() {
    const bool want = doc && show_mask_overlay && (selection_edit || (mask_edit && mask_proxy_layer < doc->layer_count() && doc->layer(mask_proxy_layer).has_mask()));
    if (!want) { overlay_tex_revision = ~0ull; return; }
    if (overlay_tex && overlay_tex_revision == doc->revision()) return;
    const Mask m = selection_edit ? image_to_mask(mask_proxy) : doc->layer(mask_proxy_layer).mask;
    std::vector<uint8_t> px(m.size() * 4);
    for (size_t i = 0; i < m.size(); ++i) {
        px[i * 4 + 0] = 255; px[i * 4 + 1] = 0; px[i * 4 + 2] = 0;
        px[i * 4 + 3] = static_cast<uint8_t>((255 - m.data()[i]) * 0.5f);
    }
    if (!overlay_tex) {
        glGenTextures(1, &overlay_tex);
        glBindTexture(GL_TEXTURE_2D, overlay_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, overlay_tex);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m.width(), m.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    overlay_tex_revision = doc->revision();
}

void App::handle_shortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) return;  // dialogs own the keyboard
    // ImGui does not swap Cmd and Ctrl itself: io.ConfigMacOSXBehaviors only
    // changes widget-internal editing keys. A physical Cmd press only sets
    // io.KeySuper, so on macOS the app's own Ctrl+ shortcuts must accept it too.
    const bool ctrl = io.KeyCtrl || (io.ConfigMacOSXBehaviors && io.KeySuper);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { io.KeyShift ? redo() : undo(); }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) show_new_dialog = true;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) request_open();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_P, false) && doc) show_print_dialog = true;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_W, false)) close_document(current_doc);
    if (ctrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false)) request_save_as();
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) save();
    if (ctrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_I, false)) select_invert();
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_I, false) && doc && active_layer() >= 0)
        run(std::make_unique<InvertCommand>(active_layer()));
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) select_all();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) select_none();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) { if (io.KeyShift) defloat(); else if (doc && doc->has_selection() && !has_floating_layer()) promote_selection_to_layer(true); }
    if (ctrl && io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_G, false)) layer_toggle_clipped();
    if (ctrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_M, false)) show_marquee = !show_marquee;
    if (ctrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_C, false)) copy_merged();
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) copy();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_X, false)) cut();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) paste_as_new_image();
    if (ctrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_L, false)) paste_into_selection();
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_L, false)) paste_as_new_layer();
    if (ctrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Y, false)) repeat_last_effect();
    if (ctrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_R, false)) crop_to_selection();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_0, false)) { if (io.KeyAlt) { zoom = 1.0f; pan_x = pan_y = 0.0f; } else fit_requested = true; }
    if (ctrl) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) clear_selection();
    if (ImGui::IsKeyPressed(ImGuiKey_Equal, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, false)) zoom_about(canvas_center, 1.25f);
    if (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false)) zoom_about(canvas_center, 0.8f);
    if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_I, false)) show_info_dialog = true;

    // Single-key tool shortcuts, as in the original (A pan, Z zoom, E dropper, B brush, X eraser, F fill).
    // A modifier means the key belongs to something else, such as Shift+I for Image Information.
    for (size_t i = 0; i < tools.size() && !io.KeyShift && !io.KeyAlt && !io.KeySuper; ++i) {
        const char* sc = tools[i]->shortcut();
        if (!sc) continue;
        const ImGuiKey key = static_cast<ImGuiKey>(ImGuiKey_A + (sc[0] - 'A'));
        if (ImGui::IsKeyPressed(key, false)) select_tool(static_cast<int>(i));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true)) brush.size = std::max(1.0f, brush.size - std::max(1.0f, brush.size * 0.1f));
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true)) brush.size = std::min(500.0f, brush.size + std::max(1.0f, brush.size * 0.1f));
}

// --- Color management ------------------------------------------------------

icc::Profile App::document_profile() const {
    if (!doc || doc->icc().empty()) return icc::Profile{};
    return icc::parse(doc->icc());
}

bool App::display_needs_transform() const {
    if (!color_managed_display || !doc || doc->icc().empty()) return false;
    App* self = const_cast<App*>(this);
    if (display_icc_key != doc->icc() || !display_transform) {
        const icc::Profile p = document_profile();
        self->display_icc_key = doc->icc();
        self->display_transform = std::make_unique<icc::Transform>(p, icc::srgb());
        if (p.is_srgb()) self->display_transform->identity = true;
    }
    return !display_transform->identity;
}

void App::assign_profile(const std::vector<uint8_t>& bytes, const std::string& name) {
    if (!doc) return;
    run(std::make_unique<StateEditCommand>(name, [bytes](Document& d) { d.set_icc(bytes); }));
    canvas_tex_revision = ~0ull;
}

void App::convert_to_profile(const icc::Profile& to, const std::vector<uint8_t>& bytes, const std::string& name) {
    if (!doc) return;
    icc::Profile from = document_profile();
    if (!from.matrix_trc) from = icc::srgb();
    if (!to.matrix_trc) { status = name + ": that profile is not an RGB matrix profile"; return; }
    icc::Transform t(from, to);
    run(std::make_unique<StateEditCommand>(name, [t, bytes](Document& d) {
        for (size_t i = 0; i < d.layer_count(); ++i) {
            Layer& L = d.layer(i);
            if (!L.is_raster()) continue;
            if (L.is_deep()) { Image16 deep = *L.deep; t.apply(deep); L.set_deep(std::move(deep)); }
            else t.apply(L.pixels);
        }
        d.set_icc(bytes);
    }));
    canvas_tex_revision = ~0ull;
}

void App::request_load_profile() {
    if (!doc) return;
    file_op = PendingFileOp::LoadProfile;
    file_dialog.open(FileDialog::Mode::Open, "Assign Profile From File", {"icc", "icm"}, "");
}
