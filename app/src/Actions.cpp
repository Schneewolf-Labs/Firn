// The action registry. Each entry names something the program can do, says
// what it takes, and calls the same App function the menu item calls, so
// the two can never drift apart.
#include "Actions.h"

#include <algorithm>
#include <cstdio>
#include <cmath>

#include "App.h"
#include "InheritedCommands.h"
#include "firn/commands.h"
#include "firn/inpaint.h"
#include "firn/metadata.h"

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
bool need_selection(App& app, bool* ok, std::string& err) {
    if (!need_doc(app, ok, err)) return false;
    if (app.doc->has_selection() && app.doc->selection().any()) return true;
    err = fail(ok, "nothing is selected");
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
    add("file.save_as", "Save the image to a path, format taken from the extension",
        {{"path", "string", "where to write it", true},
         {"quality", "number", "JPEG and WebP quality, 1 to 100; 100 is lossless WebP", false, nullptr, "the last quality used"}},
        [](App& app, const Value& p, bool* ok) {
            std::string e;
            if (!need_doc(app, ok, e)) return e;
            const std::string path = str(p, "path");
            if (path.empty()) return fail(ok, "file.save_as needs a path");
            // Saving a JPEG from the menu asks for the quality; a script says
            // it up front, or accepts the one already set, and is never asked.
            if (p.find("quality")) app.jpeg_quality = std::clamp(num(p, "quality", app.jpeg_quality), 1, 100);
            app.pending_jpeg_path = path;
            const bool saved = app.save_document(path);
            app.show_jpeg_dialog = false;
            app.pending_jpeg_path.clear();
            return saved ? ok_json() : fail(ok, app.status);
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
        [](App& app, const Value&, bool* ok) {
            std::string e;
            if (!need_raster(app, ok, e) || !need_selection(app, ok, e)) return e;
            // Synchronous here: a script expects the fill to be finished when
            // the call returns. The menu item runs it on a worker thread.
            app.content_aware_fill(false);
            return ok_json();
        });

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

    add("image.metadata", "List the Exif tags and text notes the image carries", {},
        [](App& app, const Value&, bool* ok) {
            std::string e;
            if (!need_doc(app, ok, e)) return e;
            Value list = Value::array();
            for (const firn::meta::Entry& m : app.doc->metadata().entries) {
                Value row = Value::object();
                row.set("group", Value::string(firn::meta::group_name(m.group)));
                row.set("name", Value::string(m.name()));
                row.set("value", Value::string(m.text()));
                row.set("editable", Value::boolean(m.editable()));
                list.push(std::move(row));
            }
            Value out = Value::object();
            out.set("ok", Value::boolean(true));
            out.set("metadata", std::move(list));
            return firn::json::dump(out);
        });
    add("image.set_metadata", "Set one Exif tag or text note",
        {{"name", "string", "the Exif tag name, or the keyword of a text note"},
         {"value", "string", "the new value"},
         {"group", "string", "which directory the tag is in", false, "Image,Exif,GPS,Interop,Text", "Image"}},
        [](App& app, const Value& p, bool* ok) {
            std::string e;
            if (!need_doc(app, ok, e)) return e;
            const std::string name = str(p, "name"), group = str(p, "group", "Image");
            if (name.empty()) return fail(ok, "name is required");
            firn::meta::Metadata md = app.doc->metadata();
            bool done = false;
            if (group == "Text") done = md.set_text(name, str(p, "value"));
            else {
                const firn::meta::Group g = group == "Exif"      ? firn::meta::Group::Exif
                                            : group == "GPS"     ? firn::meta::Group::GPS
                                            : group == "Interop" ? firn::meta::Group::Interop
                                                                 : firn::meta::Group::Image;
                const uint16_t tag = firn::meta::tag_for_name(g, name);
                if (!tag) return fail(ok, "no " + group + " tag is called " + name);
                done = md.set(g, tag, str(p, "value"));
            }
            if (!done) return fail(ok, "that value does not fit the tag");
            app.run(std::make_unique<firn::MetadataCommand>("Metadata", std::move(md)));
            return ok_json();
        });
    add("image.strip_metadata", "Remove metadata from the image",
        {{"what", "string", "everything, or only what identifies the photographer and the place", false, "all,private", "all"}},
        [](App& app, const Value& p, bool* ok) {
            std::string e;
            if (!need_doc(app, ok, e)) return e;
            firn::meta::Metadata md = app.doc->metadata();
            if (str(p, "what", "all") == "private") md.remove_private();
            else md.entries.clear();
            app.run(std::make_unique<firn::MetadataCommand>("Strip Metadata", std::move(md)));
            return ok_json();
        });

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
    add("layer.properties", "Set the active layer's name, opacity, blend mode, visibility or clipping",
        {{"name", "string", ""}, {"opacity", "number", "percent, 0 to 100"}, {"blend", "string", "blend mode name, as the palette shows it"}, {"visible", "bool", ""},
         {"clipped", "bool", "show the layer only where the layer below does"},
         {"pass_through", "bool", "groups only: its members act on the whole image below the group"},
         {"lock_alpha", "bool", "protect the clear parts of the layer from painting and fills"}},
        [](App& app, const Value& p, bool* ok) {
            std::string e;
            if (!need_doc(app, ok, e)) return e;
            const int i = app.active_layer();
            if (i < 0) return fail(ok, "no active layer");
            firn::LayerProps before = app.doc->props(i), after = before;
            if (p.find("name")) after.name = str(p, "name", after.name.c_str());
            if (p.find("opacity")) after.opacity = std::clamp(fnum(p, "opacity", 100.0f) / 100.0f, 0.0f, 1.0f);
            if (p.find("visible")) after.visible = flag(p, "visible", true);
            if (p.find("lock_alpha")) after.lock_alpha = flag(p, "lock_alpha", false);
            if (p.find("pass_through")) {
                if (app.doc->layer(static_cast<size_t>(i)).type != firn::LayerType::Group)
                    return fail(ok, "pass_through is for group layers");
                after.pass_through = flag(p, "pass_through", false);
            }
            if (p.find("clipped")) {
                if (flag(p, "clipped", false) && !app.can_clip_layer()) return fail(ok, "there is no layer below this one to clip to");
                after.clipped = flag(p, "clipped", false);
            }
            if (p.find("blend")) {
                const std::string want = str(p, "blend");
                for (int b = 0; b < static_cast<int>(firn::BlendMode::Count); ++b)
                    if (want == firn::blend_mode_name(static_cast<firn::BlendMode>(b))) after.blend = static_cast<firn::BlendMode>(b);
            }
            app.layer_set_props(before, after);
            return ok_json();
        });
    add("layer.blend_ranges", "Limit the active layer to a range of tones, its own or the ones below it",
        {{"channel", "string", "which value the stops are read from", false, "gray,red,green,blue", "gray"},
         {"this_layer", "string", "four stops in 0..255, as \"low0 low1 high1 high0\": hidden, fading in, fading out, hidden"},
         {"underlying", "string", "the same four stops, read from what is composited below"},
         {"reset", "bool", "put both ranges back to the full 0..255", false, nullptr, "false"}},
        [](App& app, const Value& p, bool* ok) {
            std::string e;
            if (!need_doc(app, ok, e)) return e;
            const int i = app.active_layer();
            if (i < 0) return fail(ok, "no active layer");
            firn::LayerProps before = app.doc->props(i), after = before;
            if (flag(p, "reset", false)) after.ranges = firn::BlendRanges{};
            if (p.find("channel")) {
                const std::string c = str(p, "channel", "gray");
                after.ranges.channel = c == "red"     ? firn::BlendRanges::Channel::Red
                                       : c == "green" ? firn::BlendRanges::Channel::Green
                                       : c == "blue"  ? firn::BlendRanges::Channel::Blue
                                                      : firn::BlendRanges::Channel::Gray;
            }
            // Four whitespace-separated stops, kept in order so a range
            // cannot turn inside out.
            auto stops = [&](const char* key, firn::BlendRange& r) {
                if (!p.find(key)) return true;
                int v[4] = {0, 0, 255, 255};
                const std::string text = str(p, key);
                if (std::sscanf(text.c_str(), "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]) != 4) return false;
                for (int& x : v) x = std::clamp(x, 0, 255);
                for (int k = 1; k < 4; ++k) v[k] = std::max(v[k], v[k - 1]);
                r.low0 = static_cast<uint8_t>(v[0]); r.low1 = static_cast<uint8_t>(v[1]);
                r.high1 = static_cast<uint8_t>(v[2]); r.high0 = static_cast<uint8_t>(v[3]);
                return true;
            };
            if (!stops("this_layer", after.ranges.source)) return fail(ok, "this_layer needs four numbers, such as \"0 0 128 192\"");
            if (!stops("underlying", after.ranges.under)) return fail(ok, "underlying needs four numbers, such as \"0 0 128 192\"");
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
    add("app.describe", "Describe all actions or one named action", {{"name", "string", "Optional exact action name"}},
        [](App& app, const Value& p, bool* ok) {
            const auto name = str(p, "name");
            if (!name.empty() && !find_action(name)) return fail(ok, "unknown action " + name);
            return describe_json(app, name);
        });
    add_drawing_actions(a);
    add_vector_actions(a);
    for (auto& entry : a) {
        const std::string name(entry.name);
        if (name == "layer.new" || name == "layer.new_vector" || name == "layer.properties" || name == "layer.select" ||
            name == "select.all" || name == "select.none" || name == "select.invert" || name == "select.rect" || name == "select.ellipse") entry.batch_safe = true;
    }
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

std::string describe_json(App& app, const std::string& name) {
    Value root = Value::object();
    root.set("firn", Value::string("action api"));
    root.set("version", Value::number(2));
    root.set("coordinate_system", Value::string("image pixels; origin at top left; positive x right and y down"));
    Value list = Value::array();
    for (const Action& a : actions()) {
        if (!name.empty() && name != a.name) continue;
        Value e = Value::object();
        e.set("name", Value::string(a.name));
        e.set("summary", Value::string(a.summary));
        if (a.detail) e.set("detail", Value::string(a.detail));
        e.set("input_schema", action_schema(a));
        e.set("batch_safe", Value::boolean(a.batch_safe));
        if (a.examples.size()) e.set("examples", a.examples);
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
    root.set("commands_note", Value::string("Legacy compatibility commands have no validated schemas and cannot run in app.batch. Prefer the typed actions above. A trailing * is a prefix."));
    Value replacements = Value::object();
    replacements.set("Fill", Value::string("edit.fill"));
    replacements.set("Selection", Value::string("select.rect / select.ellipse"));
    replacements.set("NewRasterLayer", Value::string("layer.new"));
    replacements.set("NewVectorLayer", Value::string("layer.new_vector"));
    root.set("legacy_replacements", std::move(replacements));
    return firn::json::dump(root);
}
