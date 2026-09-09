#include "App.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "firn/io.h"
#include "firn/io_psp.h"
#include "firn/mask.h"

using namespace firn;

App::App() : tools(make_default_tools()) {
    config.load();
    file_dialog.set_directory(config.last_directory);
    show_rulers = config.show_rulers;
    show_grid = config.show_grid;
    grid_spacing = config.grid_spacing;
}

// --- Documents -----------------------------------------------------------

void App::stash_current() {
    if (current_doc < 0 || current_doc >= static_cast<int>(docs.size())) return;
    DocState& s = docs[current_doc];
    s.doc = std::move(doc);
    s.history = std::move(history);
    s.doc_path = doc_path;
    s.title = doc_title;
    s.saved_cursor = saved_cursor;
    s.zoom = zoom; s.pan_x = pan_x; s.pan_y = pan_y;
    s.fit_requested = fit_requested;
    s.crop_rect = crop_rect;
    history = CommandStack();
}

void App::activate_document(int index) {
    if (index < 0 || index >= static_cast<int>(docs.size()) || index == current_doc) return;
    tool().cancel(*this);
    preview_cancel();
    stash_current();
    DocState& s = docs[index];
    doc = std::move(s.doc);
    history = std::move(s.history);
    doc_path = s.doc_path;
    doc_title = s.title;
    saved_cursor = s.saved_cursor;
    zoom = s.zoom; pan_x = s.pan_x; pan_y = s.pan_y;
    fit_requested = s.fit_requested;
    crop_rect = s.crop_rect;
    current_doc = index;
    canvas_tex_revision = ~0ull;  // force re-upload
    select_tab_request = index;
}

void App::add_document(std::unique_ptr<Document> d, const std::string& path) {
    tool().cancel(*this);
    preview_cancel();
    stash_current();
    docs.emplace_back();
    current_doc = static_cast<int>(docs.size()) - 1;
    doc = std::move(d);
    history.clear();
    doc_path = path;
    if (path.empty()) doc_title = "Untitled " + std::to_string(++untitled_counter);
    else { const auto slash = path.find_last_of("/\\"); doc_title = slash == std::string::npos ? path : path.substr(slash + 1); }
    saved_cursor = 0;
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
    return s.doc && s.history.cursor() != s.saved_cursor;
}

void App::close_document(int index, bool force) {
    if (index < 0 || index >= static_cast<int>(docs.size())) return;
    if (!force && document_modified(index)) { pending_close = index; return; }
    if (index == current_doc) {
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
            saved_cursor = s.saved_cursor; zoom = s.zoom; pan_x = s.pan_x; pan_y = s.pan_y; fit_requested = s.fit_requested; crop_rect = s.crop_rect;
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
    auto loaded = io::load_document(path, &err, &warnings);
    if (!loaded) {
        status = "Open failed: " + err;
        return false;
    }
    add_document(std::move(loaded), path);
    config.touch_recent(path);
    config.last_directory = file_dialog.directory();
    status = "Opened " + path;
    for (const auto& w : warnings) status += "\n" + w;
    return true;
}

bool App::save_document(const std::string& path) {
    if (!doc) return false;
    std::string err;
    if (!io::save_document(*doc, path, &err)) {
        status = "Save failed: " + err;
        return false;
    }
    doc_path = path;
    { const auto slash = path.find_last_of("/\\"); doc_title = slash == std::string::npos ? path : path.substr(slash + 1); }
    saved_cursor = history.cursor();
    config.touch_recent(path);
    status = "Saved " + path;
    if (!io::is_psp_extension(path) && doc->layer_count() > 1) status += "\nFlattened: only .PspImage keeps layers.";
    return true;
}

void App::request_open() {
    file_op = PendingFileOp::Open;
    file_dialog.open(FileDialog::Mode::Open, "Open Image", io::load_extensions(), doc_path);
}

void App::request_save_as() {
    if (!doc) return;
    file_op = PendingFileOp::SaveAs;
    // Layered documents default to the native container; single-layer ones keep their format.
    std::string suggested = doc_path.empty() ? "untitled" : doc_path;
    const bool layered = doc->layer_count() > 1 || !doc->layer(0).background;
    if (doc_path.empty() || (layered && !io::is_psp_extension(doc_path))) {
        const auto dot = suggested.rfind('.');
        const auto slash = suggested.find_last_of("/\\");
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) suggested.resize(dot);
        suggested += layered ? ".pspimage" : ".png";
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
    if (!doc_path.empty() && std::find(exts.begin(), exts.end(), ext) != exts.end()) save_document(doc_path);
    else request_save_as();
}

void App::run(std::unique_ptr<Command> cmd) {
    if (!doc) return;
    status = cmd->name();
    history.run(*doc, std::move(cmd));
}

void App::commit(std::unique_ptr<Command> cmd) {
    if (!doc) return;
    status = cmd->name();
    history.push_applied(std::move(cmd));
}

void App::select_tool(int index) {
    if (index < 0 || index >= static_cast<int>(tools.size()) || index == tool_index) return;
    tool().cancel(*this);
    active_button = -1;
    tool_index = index;
}

void App::zoom_about(ImVec2 screen, float factor) {
    const float old_zoom = zoom;
    zoom = std::clamp(zoom * factor, 0.01f, 64.0f);
    const float k = zoom / old_zoom;
    const float mx = screen.x - canvas_centre.x, my = screen.y - canvas_centre.y;
    pan_x = mx - (mx - pan_x) * k;
    pan_y = my - (my - pan_y) * k;
}

void App::undo() {
    tool().cancel(*this);
    if (doc && history.can_undo()) {
        status = "Undo " + history.at(history.cursor() - 1).name();
        history.undo(*doc);
    }
}

void App::redo() {
    tool().cancel(*this);
    if (doc && history.can_redo()) {
        status = "Redo " + history.at(history.cursor()).name();
        history.redo(*doc);
    }
}

int App::active_layer() const { return doc ? doc->active_layer() : -1; }

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

void App::copy() {
    if (!doc || active_layer() < 0) return;
    const Image& src = doc->layer(active_layer()).pixels;
    Image out = src;
    raster::Rect bounds{0, 0, doc->width(), doc->height()};
    if (doc->has_selection()) {
        const Mask& sel = doc->selection();
        bounds = sel.bounds();
        uint8_t* p = out.data();
        for (size_t i = 0; i < sel.size(); ++i) p[i * 4 + 3] = static_cast<uint8_t>((p[i * 4 + 3] * sel.data()[i] + 127) / 255);
    }
    clipboard = {std::move(out), bounds};
    status = "Copied";
}

void App::clear_selection() {
    if (!doc || active_layer() < 0) return;
    const Layer& L = doc->layer(active_layer());
    // The original clears a Background layer to the background colour.
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
    if (!doc || clipboard.empty()) return;
    // Clipboard from a different-sized document is placed at its top-left.
    Image px(doc->width(), doc->height());
    const int w = std::min(px.width(), clipboard.pixels.width()), h = std::min(px.height(), clipboard.pixels.height());
    for (int y = 0; y < h; ++y)
        std::memcpy(px.data() + static_cast<size_t>(y) * px.width() * 4,
                    clipboard.pixels.data() + static_cast<size_t>(y) * clipboard.pixels.width() * 4, static_cast<size_t>(w) * 4);
    run(std::make_unique<PasteLayerCommand>("Raster " + std::to_string(doc->layer_count()), std::move(px)));
}

void App::paste_as_new_image() {
    if (clipboard.empty()) return;
    const raster::Rect b = clipboard.bounds;
    if (b.empty()) return;
    tool().cancel(*this);
    doc = std::make_unique<Document>(b.x1 - b.x0, b.y1 - b.y0);
    Layer& L = doc->add_layer("Raster 1");
    for (int y = b.y0; y < b.y1; ++y)
        std::memcpy(L.pixels.data() + static_cast<size_t>(y - b.y0) * L.pixels.width() * 4,
                    clipboard.pixels.data() + (static_cast<size_t>(y) * clipboard.pixels.width() + b.x0) * 4,
                    static_cast<size_t>(b.x1 - b.x0) * 4);
    history.clear();
    doc_path.clear();
    fit_requested = true;
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
    if (!doc || active_layer() < 0) return;
    const int n = static_cast<int>(doc->layer_count());
    const int from = active_layer();
    const int to = std::clamp(from + delta, 0, n - 1);
    if (to != from) run(std::make_unique<ArrangeLayerCommand>(from, to));
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

void App::layer_promote_background() {
    if (doc && active_layer() >= 0 && doc->layer(active_layer()).background)
        run(std::make_unique<PromoteBackgroundCommand>(active_layer()));
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
        return;
    }
    if (canvas_tex && canvas_tex_revision == doc->revision()) return;

    Image composite = doc->composite();
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, composite.width(), composite.height(), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, composite.data());
    canvas_tex_revision = doc->revision();
}

void App::handle_shortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) return;  // dialogs own the keyboard
    const bool ctrl = io.KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { io.KeyShift ? redo() : undo(); }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) show_new_dialog = true;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) request_open();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_W, false)) close_document(current_doc);
    if (ctrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false)) request_save_as();
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) save();
    if (ctrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_I, false)) select_invert();
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_I, false) && doc && active_layer() >= 0)
        run(std::make_unique<InvertCommand>(active_layer()));
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) select_all();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) select_none();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) copy();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_X, false)) cut();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) paste_as_new_image();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_L, false)) paste_as_new_layer();
    if (ctrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_R, false)) crop_to_selection();
    if (ctrl) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) clear_selection();

    // Single-key tool shortcuts, as in the original (A pan, Z zoom, E dropper, B brush, X eraser, F fill).
    for (size_t i = 0; i < tools.size(); ++i) {
        const char* sc = tools[i]->shortcut();
        if (!sc) continue;
        const ImGuiKey key = static_cast<ImGuiKey>(ImGuiKey_A + (sc[0] - 'A'));
        if (ImGui::IsKeyPressed(key, false)) select_tool(static_cast<int>(i));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true)) brush.size = std::max(1.0f, brush.size - std::max(1.0f, brush.size * 0.1f));
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true)) brush.size = std::min(500.0f, brush.size + std::max(1.0f, brush.size * 0.1f));
}
