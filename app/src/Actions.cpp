// The action registry. Each entry names something the program can do, says
// what it takes, and calls the same App function the menu item calls, so
// the two can never drift apart.
#include "Actions.h"

#include <algorithm>
#include <cstdio>

#include "App.h"
#include "InheritedCommands.h"
#include "firn/commands.h"
#include "firn/inpaint.h"

using firn::json::Value;

namespace {

std::string ok_json() { return "{\"ok\":true}"; }
std::string ok_json(const std::string& key, const std::string& value) {
    Value v = Value::object();
    v.set("ok", Value::boolean(true));
    v.set(key, Value::string(value));
    return firn::json::dump(v);
}
std::string fail(bool* ok, const std::string& why) { *ok = false; return why; }

// Common guards, so every handler reports the same way.
bool need_doc(App& app, bool* ok, std::string& err) {
    if (app.doc) return true;
    err = fail(ok, "no image is open");
    return false;
}
bool need_raster(App& app, bool* ok, std::string& err) {
    if (!need_doc(app, ok, err)) return false;
    if (app.active_is_raster()) return true;
    err = fail(ok, "the active layer is not a raster layer");
    return false;
}

int num(const Value& p, const char* key, int def) { return static_cast<int>(p.get(key).as_number(def)); }
float fnum(const Value& p, const char* key, float def) { return static_cast<float>(p.get(key).as_number(def)); }
bool flag(const Value& p, const char* key, bool def) { return p.get(key).as_bool(def); }
std::string str(const Value& p, const char* key, const char* def = "") { return p.get(key).as_string(def); }

firn::raster::Filter filter_by_name(const std::string& n) {
    if (n == "nearest" || n == "pixel") return firn::raster::Filter::Nearest;
    if (n == "bilinear") return firn::raster::Filter::Bilinear;
    if (n == "bicubic") return firn::raster::Filter::Bicubic;
    if (n == "lanczos") return firn::raster::Filter::Lanczos;
    if (n == "mitchell") return firn::raster::Filter::Mitchell;
    if (n == "edge_directed") return firn::raster::Filter::EdgeDirected;
    return firn::raster::Filter::Smart;
}

std::vector<Action> build() {
    std::vector<Action> a;
    auto add = [&](const char* name, const char* summary, std::vector<Action::Param> params,
                   std::function<std::string(App&, const Value&, bool*)> run) {
        Action entry;
        entry.name = name;
        entry.summary = summary;
        entry.params = std::move(params);
        entry.run = std::move(run);
        a.push_back(std::move(entry));
    };

    // --- documents ---
    add("file.new", "Create an image", {{"width", "number", "pixels", false, nullptr, "800"}, {"height", "number", "pixels", false, nullptr, "600"}, {"color", "string", "#RRGGBB background, or transparent", false, nullptr, "white"}},
        [](App& app, const Value& p, bool*) {
            app.new_document(std::max(1, num(p, "width", 800)), std::max(1, num(p, "height", 600)));
            const std::string c = str(p, "color");
            if (app.doc && !c.empty()) {
                firn::Layer& L = app.doc->layer(0);
                if (c == "transparent") { L.pixels = firn::Image(app.doc->width(), app.doc->height(), {0, 0, 0, 0}); L.background = false; }
                else {
                    unsigned v = 0;
                    if (std::sscanf(c.c_str() + (c[0] == '#' ? 1 : 0), "%6x", &v) == 1)
                        L.pixels = firn::Image(app.doc->width(), app.doc->height(),
                                               {static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v), 255});
                }
                app.doc->touch();
            }
            return ok_json();
        });
    add("file.open", "Open an image file", {{"path", "string", "file to open", true}},
        [](App& app, const Value& p, bool* ok) {
            const std::string path = str(p, "path");
            if (path.empty()) return fail(ok, "file.open needs a path");
            if (!app.open_document(path)) return fail(ok, "could not open " + path);
            return ok_json();
        });
    add("file.save", "Save the image where it came from", {},
        [](App& app, const Value&, bool* ok) {
            std::string e;
            if (!need_doc(app, ok, e)) return e;
            if (app.doc_path.empty()) return fail(ok, "this image has no path yet; use file.save_as");
            return app.save_document(app.doc_path) ? ok_json() : fail(ok, app.status);
        });
    add("file.save_as", "Save the image to a path, format taken from the extension", {{"path", "string", "where to write it", true}},
        [](App& app, const Value& p, bool* ok) {
            std::string e;
            if (!need_doc(app, ok, e)) return e;
            const std::string path = str(p, "path");
            if (path.empty()) return fail(ok, "file.save_as needs a path");
            return app.save_document(path) ? ok_json() : fail(ok, app.status);
        });
    add("file.close", "Close the current image, discarding changes", {},
        [](App& app, const Value&, bool* ok) {
            std::string e;
            if (!need_doc(app, ok, e)) return e;
            app.close_document(app.current_doc, true);
            return ok_json();
        });
    add("file.revert", "Load the saved file again, dropping every change", {},
        [](App& app, const Value&, bool* ok) {
            std::string e;
            if (!need_doc(app, ok, e)) return e;
            app.revert();
            return ok_json();
        });

    // --- history and clipboard ---
    add("edit.undo", "Undo one step", {}, [](App& app, const Value&, bool*) { app.undo(); return ok_json(); });
    add("edit.redo", "Redo one step", {}, [](App& app, const Value&, bool*) { app.redo(); return ok_json(); });
    add("edit.cut", "Cut the selection", {}, [](App& app, const Value&, bool* ok) { std::string e; if (!need_raster(app, ok, e)) return e; app.cut(); return ok_json(); });
    add("edit.copy", "Copy the active layer through the selection", {},
        [](App& app, const Value&, bool* ok) { std::string e; if (!need_raster(app, ok, e)) return e; app.copy(); return ok_json(); });
    add("edit.copy_merged", "Copy the composite through the selection", {},
        [](App& app, const Value&, bool* ok) { std::string e; if (!need_doc(app, ok, e)) return e; app.copy_merged(); return ok_json(); });
    add("edit.paste_as_image", "Paste the clipboard as a new image", {}, [](App& app, const Value&, bool*) { app.paste_as_new_image(); return ok_json(); });
    add("edit.paste_as_layer", "Paste the clipboard as a new layer", {},
        [](App& app, const Value&, bool* ok) { std::string e; if (!need_doc(app, ok, e)) return e; app.paste_as_new_layer(); return ok_json(); });
    add("edit.paste_into_selection", "Scale the clipboard into the selection", {},
        [](App& app, const Value&, bool* ok) { std::string e; if (!need_raster(app, ok, e)) return e; app.paste_into_selection(); return ok_json(); });
    add("edit.clear", "Clear the selection", {},
        [](App& app, const Value&, bool* ok) { std::string e; if (!need_raster(app, ok, e)) return e; app.clear_selection(); return ok_json(); });
    add("edit.repeat", "Apply the last adjustment or effect again", {},
        [](App& app, const Value&, bool* ok) { std::string e; if (!need_raster(app, ok, e)) return e; app.repeat_last_effect(); return ok_json(); });
    add("edit.content_aware_fill", "Rebuild the selection from the rest of the picture", {},
        [](App& app, const Value&, bool* ok) { std::string e; if (!need_raster(app, ok, e)) return e; app.content_aware_fill(); return ok_json(); });

    // --- view ---
    add("view.zoom", "Set the zoom, or fit / actual size", {{"zoom", "number", "1 is actual size", false, nullptr, "1"}, {"mode", "string", "which way to zoom", false, "fit,actual,set", "fit unless zoom is given"}},
        [](App& app, const Value& p, bool* ok) {
            std::string e;
            if (!need_doc(app, ok, e)) return e;
            const std::string mode = str(p, "mode", p.find("zoom") ? "set" : "fit");
            if (mode == "fit") app.fit_requested = true;
            else if (mode == "actual") { app.zoom = 1.0f; app.pan_x = app.pan_y = 0.0f; app.fit_requested = false; }
            else { app.zoom = std::clamp(fnum(p, "zoom", 1.0f), 0.01f, 64.0f); app.fit_requested = false; }
            return ok_json();
        });
    add("view.zoom_to_selection", "Fill the window with the selection", {},
        [](App& app, const Value&, bool* ok) { std::string e; if (!need_doc(app, ok, e)) return e; app.zoom_to_selection(); return ok_json(); });
    add("view.toggle", "Turn a view aid on or off", {{"what", "string", "the aid to switch", true, "rulers,grid,guides,assistants,marquee,snap_guides,snap_grid,snap_assistants"}, {"on", "bool", "leave it out to toggle", false, nullptr, "the opposite of now"}},
        [](App& app, const Value& p, bool* ok) {
            const std::string what = str(p, "what");
            bool* target = what == "rulers" ? &app.show_rulers : what == "grid" ? &app.show_grid : what == "guides" ? &app.show_guides
                           : what == "assistants" ? &app.show_assistants : what == "marquee" ? &app.show_marquee
                           : what == "snap_guides" ? &app.snap_to_guides : what == "snap_grid" ? &app.snap_to_grid
                           : what == "snap_assistants" ? &app.assistant_snap : nullptr;
            if (!target) return fail(ok, "view.toggle does not know \"" + what + "\"");
            *target = p.find("on") ? flag(p, "on", true) : !*target;
            return ok_json();
        });

    // --- whole-image geometry ---
    add("image.flip", "Flip top to bottom", {}, [](App& app, const Value&, bool* ok) {
        std::string e; if (!need_doc(app, ok, e)) return e;
        app.run(std::make_unique<firn::FlipCommand>()); return ok_json(); });
    add("image.mirror", "Mirror left to right", {}, [](App& app, const Value&, bool* ok) {
        std::string e; if (!need_doc(app, ok, e)) return e;
        app.run(std::make_unique<firn::MirrorCommand>()); return ok_json(); });
    add("image.rotate", "Rotate the image", {{"degrees", "number", "clockwise", false, nullptr, "90"}},
        [](App& app, const Value& p, bool* ok) {
            std::string e;
            if (!need_doc(app, ok, e)) return e;
            app.rotate(fnum(p, "degrees", 90.0f));
            return ok_json();
        });
    add("image.resize", "Resize the image", {{"width", "number", "pixels"}, {"height", "number", "pixels"}, {"filter", "string", "how to resample", false, "smart,lanczos,mitchell,bicubic,bilinear,nearest,edge_directed", "smart"}},
        [](App& app, const Value& p, bool* ok) {
            std::string e;
            if (!need_doc(app, ok, e)) return e;
            const int w = num(p, "width", app.doc->width()), h = num(p, "height", app.doc->height());
            if (w < 1 || h < 1) return fail(ok, "image.resize needs a positive width and height");
            app.run(std::make_unique<firn::ResizeCommand>(w, h, filter_by_name(str(p, "filter", "smart"))));
            return ok_json();
        });
    add("image.crop_to_selection", "Crop to the selection", {},
        [](App& app, const Value&, bool* ok) { std::string e; if (!need_doc(app, ok, e)) return e; app.crop_to_selection(); return ok_json(); });

    // --- selection ---
    add("select.all", "Select everything", {}, [](App& app, const Value&, bool* ok) { std::string e; if (!need_doc(app, ok, e)) return e; app.select_all(); return ok_json(); });
    add("select.none", "Drop the selection", {}, [](App& app, const Value&, bool* ok) { std::string e; if (!need_doc(app, ok, e)) return e; app.select_none(); return ok_json(); });
    add("select.invert", "Invert the selection", {}, [](App& app, const Value&, bool* ok) { std::string e; if (!need_doc(app, ok, e)) return e; app.select_invert(); return ok_json(); });
    add("select.rect", "Select a rectangle, in image pixels", {{"x0", "number", ""}, {"y0", "number", ""}, {"x1", "number", ""}, {"y1", "number", ""}, {"feather", "number", "pixels of soft edge", false, nullptr, "0"}},
        [](App& app, const Value& p, bool* ok) {
            std::string e;
            if (!need_doc(app, ok, e)) return e;
            firn::Mask m = firn::mask::rectangle(app.doc->width(), app.doc->height(), fnum(p, "x0", 0), fnum(p, "y0", 0),
                                                 fnum(p, "x1", 0), fnum(p, "y1", 0), true);
            const float f = fnum(p, "feather", 0);
            if (f > 0) firn::mask::feather(m, f);
            app.set_selection("Select Rectangle", std::move(m));
            return ok_json();
        });
    add("select.ellipse", "Select an ellipse inside a rectangle, in image pixels", {{"x0", "number", ""}, {"y0", "number", ""}, {"x1", "number", ""}, {"y1", "number", ""}, {"feather", "number", "pixels of soft edge", false, nullptr, "0"}},
        [](App& app, const Value& p, bool* ok) {
            std::string e;
            if (!need_doc(app, ok, e)) return e;
            const float x0 = fnum(p, "x0", 0), y0 = fnum(p, "y0", 0), x1 = fnum(p, "x1", 0), y1 = fnum(p, "y1", 0);
            firn::Mask m = firn::mask::ellipse(app.doc->width(), app.doc->height(), (x0 + x1) / 2, (y0 + y1) / 2,
                                               std::abs(x1 - x0) / 2, std::abs(y1 - y0) / 2, true);
            const float f = fnum(p, "feather", 0);
            if (f > 0) firn::mask::feather(m, f);
            app.set_selection("Select Ellipse", std::move(m));
            return ok_json();
        });

    // --- layers ---
    add("layer.new", "Add a raster layer", {}, [](App& app, const Value&, bool* ok) { std::string e; if (!need_doc(app, ok, e)) return e; app.layer_new(); return ok_json(); });
    add("layer.new_vector", "Add a vector layer", {}, [](App& app, const Value&, bool* ok) { std::string e; if (!need_doc(app, ok, e)) return e; app.layer_new_vector(); return ok_json(); });
    add("layer.new_group", "Group the active layer", {}, [](App& app, const Value&, bool* ok) { std::string e; if (!need_doc(app, ok, e)) return e; app.layer_new_group(); return ok_json(); });
    add("layer.duplicate", "Duplicate the active layer", {}, [](App& app, const Value&, bool* ok) { std::string e; if (!need_doc(app, ok, e)) return e; app.layer_duplicate(); return ok_json(); });
    add("layer.delete", "Delete the active layer", {}, [](App& app, const Value&, bool* ok) { std::string e; if (!need_doc(app, ok, e)) return e; app.layer_delete(); return ok_json(); });
    add("layer.select", "Make a layer active, by index from the bottom", {{"index", "number", "0 is the bottom", true}},
        [](App& app, const Value& p, bool* ok) {
            std::string e;
            if (!need_doc(app, ok, e)) return e;
            const int i = num(p, "index", 0);
            if (i < 0 || i >= static_cast<int>(app.doc->layer_count())) return fail(ok, "no layer at that index");
            app.doc->set_active_layer(i);
            return ok_json();
        });
    add("layer.arrange", "Move the active layer up or down its group", {{"steps", "number", "positive is up", false, nullptr, "1"}},
        [](App& app, const Value& p, bool* ok) { std::string e; if (!need_doc(app, ok, e)) return e; app.layer_arrange(num(p, "steps", 1)); return ok_json(); });
    add("layer.move_onto", "Restack a layer where another one sits", {{"from", "number", "index", true}, {"onto", "number", "index", true}},
        [](App& app, const Value& p, bool* ok) { std::string e; if (!need_doc(app, ok, e)) return e; app.layer_move_onto(num(p, "from", -1), num(p, "onto", -1)); return ok_json(); });
    add("layer.merge", "Merge layers", {{"what", "string", "which layers to merge", false, "down,visible,all", "down"}},
        [](App& app, const Value& p, bool* ok) {
            std::string e;
            if (!need_doc(app, ok, e)) return e;
            const std::string what = str(p, "what", "down");
            app.layer_merge(what == "all" ? 2 : what == "visible" ? 1 : 0);
            return ok_json();
        });
    add("layer.properties", "Set the active layer's name, opacity, blend mode or visibility",
        {{"name", "string", ""}, {"opacity", "number", "percent, 0 to 100"}, {"blend", "string", "blend mode name, as the palette shows it"}, {"visible", "bool", ""}},
        [](App& app, const Value& p, bool* ok) {
            std::string e;
            if (!need_doc(app, ok, e)) return e;
            const int i = app.active_layer();
            if (i < 0) return fail(ok, "no active layer");
            firn::LayerProps before = app.doc->props(i), after = before;
            if (p.find("name")) after.name = str(p, "name", after.name.c_str());
            if (p.find("opacity")) after.opacity = std::clamp(fnum(p, "opacity", 100.0f) / 100.0f, 0.0f, 1.0f);
            if (p.find("visible")) after.visible = flag(p, "visible", true);
            if (p.find("blend")) {
                const std::string want = str(p, "blend");
                for (int b = 0; b < static_cast<int>(firn::BlendMode::Count); ++b)
                    if (want == firn::blend_mode_name(static_cast<firn::BlendMode>(b))) after.blend = static_cast<firn::BlendMode>(b);
            }
            app.layer_set_props(before, after);
            return ok_json();
        });
    add("layer.promote_background", "Turn the Background layer into an ordinary one", {},
        [](App& app, const Value&, bool* ok) { std::string e; if (!need_doc(app, ok, e)) return e; app.layer_promote_background(); return ok_json(); });

    // --- tools ---
    add("tool.select", "Choose a tool by the name shown in the palette", {{"name", "string", "the name shown in the tools palette", true}},
        [](App& app, const Value& p, bool* ok) {
            const std::string want = str(p, "name");
            for (size_t i = 0; i < app.tools.size(); ++i)
                if (want == app.tools[i]->name()) { app.select_tool(static_cast<int>(i)); return ok_json(); }
            return fail(ok, "no tool named \"" + want + "\"");
        });
    add("tool.brush_size", "Set the brush size in pixels", {{"size", "number", "pixels, 1 to 500", true}},
        [](App& app, const Value& p, bool*) { app.brush.size = std::clamp(fnum(p, "size", 10.0f), 1.0f, 500.0f); return ok_json(); });
    add("tool.color", "Set the foreground or background color", {{"color", "string", "#RRGGBB", true}, {"which", "string", "which material", false, "foreground,background", "foreground"}},
        [](App& app, const Value& p, bool* ok) {
            unsigned v = 0;
            const std::string c = str(p, "color");
            if (std::sscanf(c.c_str() + (!c.empty() && c[0] == '#' ? 1 : 0), "%6x", &v) != 1) return fail(ok, "tool.color needs #RRGGBB");
            float* t = str(p, "which", "foreground") == "background" ? app.bg_color : app.fg_color;
            t[0] = ((v >> 16) & 255) / 255.0f; t[1] = ((v >> 8) & 255) / 255.0f; t[2] = (v & 255) / 255.0f;
            return ok_json();
        });

    // --- the program itself ---
    add("app.screenshot", "Write what the window is showing to a PNG", {{"path", "string", "file to write", true}},
        [](App&, const Value& p, bool* ok) {
            const std::string path = str(p, "path");
            if (path.empty()) return fail(ok, "app.screenshot needs a path");
            return ok_json("pending", path);   // the driver performs it after the next render
        });
    add("app.describe", "List every action, tool and option this build offers", {},
        [](App& app, const Value&, bool*) { return describe_json(app); });
    return a;
}

}  // namespace

const std::vector<Action>& actions() {
    static const std::vector<Action> table = build();
    return table;
}

const Action* find_action(const std::string& name) {
    for (const Action& a : actions())
        if (name == a.name) return &a;
    return nullptr;
}

std::string describe_json(App& app) {
    Value root = Value::object();
    root.set("firn", Value::string("action api"));
    root.set("version", Value::number(1));
    Value list = Value::array();
    for (const Action& a : actions()) {
        Value e = Value::object();
        e.set("name", Value::string(a.name));
        e.set("summary", Value::string(a.summary));
        if (a.detail) e.set("detail", Value::string(a.detail));
        // JSON Schema for the parameters, which is the shape a tool-calling
        // client already knows how to read.
        Value schema = Value::object();
        schema.set("type", Value::string("object"));
        Value props = Value::object();
        Value required = Value::array();
        for (const Action::Param& q : a.params) {
            Value pv = Value::object();
            pv.set("type", Value::string(q.type == std::string("bool") ? "boolean" : q.type));
            pv.set("description", Value::string(q.summary));
            if (q.choices) {
                Value list = Value::array();
                std::string acc;
                for (const char* c = q.choices;; ++c) {
                    if (*c == ',' || *c == '\0') { if (!acc.empty()) list.push(Value::string(acc)); acc.clear(); if (!*c) break; }
                    else acc += *c;
                }
                pv.set("enum", std::move(list));
            }
            if (q.fallback) pv.set("default", Value::string(q.fallback));
            props.set(q.name, std::move(pv));
            if (q.required) required.push(Value::string(q.name));
        }
        schema.set("properties", std::move(props));
        schema.set("required", std::move(required));
        schema.set("additionalProperties", Value::boolean(false));
        e.set("input_schema", std::move(schema));
        list.push(std::move(e));
    }
    root.set("actions", std::move(list));
    Value tools = Value::array();
    for (const auto& t : app.tools) tools.push(Value::string(t->name()));
    root.set("tools", std::move(tools));
    // The inherited command names, read out of Script.cpp at build time, so
    // describe covers everything that can be called rather than half of it.
    Value inherited = Value::array();
    for (const char* c : kInheritedCommands) inherited.push(Value::string(c));
    root.set("commands", std::move(inherited));
    root.set("commands_note", Value::string("names from the program this grew out of, taking its parameter names; see docs/COMMANDS.md. A trailing * is a prefix."));
    return firn::json::dump(root);
}
