#include "App.h"

#include <algorithm>
#include <cstdio>

#include "imgui.h"
#include "firn/io.h"

using namespace firn;

App::App() : tools(make_default_tools()) {}

void App::new_document(int w, int h) {
    tool().cancel(*this);
    doc = std::make_unique<Document>(w, h);
    Layer& bg = doc->add_layer("Background");
    bg.background = true;
    bg.pixels.fill({255, 255, 255, 255});
    history.clear();
    doc_path.clear();
    fit_requested = true;
    status = "New image " + std::to_string(w) + "x" + std::to_string(h);
}

bool App::open_document(const std::string& path) {
    std::string err;
    auto img = io::load(path, &err);
    if (!img) {
        status = "Open failed: " + err;
        return false;
    }
    tool().cancel(*this);
    doc = std::make_unique<Document>(img->width(), img->height());
    Layer& bg = doc->add_layer("Background");
    bg.background = true;
    bg.pixels = std::move(*img);
    history.clear();
    doc_path = path;
    fit_requested = true;
    status = "Opened " + path;
    return true;
}

bool App::save_document_png(const std::string& path) {
    if (!doc) return false;
    std::string err;
    if (!io::save_png(doc->composite(), path, &err)) {
        status = "Save failed: " + err;
        return false;
    }
    doc_path = path;
    status = "Saved " + path;
    return true;
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
    const bool ctrl = io.KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { io.KeyShift ? redo() : undo(); }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) show_new_dialog = true;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) show_open_dialog = true;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) show_save_dialog = true;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_I, false) && doc && active_layer() >= 0)
        run(std::make_unique<InvertCommand>(active_layer()));
    if (ctrl) return;

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
