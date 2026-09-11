// Scripting: the original's App.Do(Environment, 'Command', {params}) calls,
// received as JSON over the driver socket from scripts/firn-script.py.
// Parameter names follow the original's command API reference; unknown
// parameters are ignored and unknown commands are reported.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>

#include "App.h"
#include "firn/adjust.h"
#include "firn/commands.h"
#include "firn/effects.h"
#include "firn/io.h"
#include "firn/io_psp.h"
#include "firn/json.h"
#include "firn/mask.h"
#include "firn/photo.h"
#include "firn/raster.h"
#include "firn/text.h"
#include "firn/vector.h"

using namespace firn;
using json::Value;

namespace {

Color color_param(const Value& v, Color def) {
    if (v.is_array() && v.size() >= 3) return {static_cast<uint8_t>(std::clamp(v[0].as_number(), 0.0, 255.0)), static_cast<uint8_t>(std::clamp(v[1].as_number(), 0.0, 255.0)), static_cast<uint8_t>(std::clamp(v[2].as_number(), 0.0, 255.0)), 255};
    if (v.is_object()) return {static_cast<uint8_t>(v.get("Red").as_number()), static_cast<uint8_t>(v.get("Green").as_number()), static_cast<uint8_t>(v.get("Blue").as_number()), 255};
    return def;
}

raster::Rect rect_param(const Value& v, raster::Rect def) {
    if (v.is_array() && v.size() >= 4) return {static_cast<int>(v[0].as_number()), static_cast<int>(v[1].as_number()), static_cast<int>(v[2].as_number()), static_cast<int>(v[3].as_number())};
    if (v.is_object()) return {static_cast<int>(v.get("Left").as_number()), static_cast<int>(v.get("Top").as_number()), static_cast<int>(v.get("Right").as_number()), static_cast<int>(v.get("Bottom").as_number())};
    return def;
}

BlendMode blend_param(const Value& v, BlendMode def) {
    const std::string s = v.as_string();
    for (int i = 0; i < static_cast<int>(BlendMode::Count); ++i)
        if (s == blend_mode_name(static_cast<BlendMode>(i))) return static_cast<BlendMode>(i);
    return def;
}

// A layer index from the original's Path tuple (level, relative offset, ...) or a name.
int layer_from_params(App& app, const Value& p) {
    if (!app.doc) return -1;
    const Value& name = p.get("SelectedLayerName");
    if (name.is_string() && !name.str.empty())
        for (size_t i = 0; i < app.doc->layer_count(); ++i) if (app.doc->layer(i).name == name.str) return static_cast<int>(i);
    // Path: (level, relative index, [child indices...], flag). Large values
    // mean "all the way"; child indices descend into the current group,
    // counted from its first member (1-based).
    const Value& path = p.get("Path");
    if (path.is_array() && path.size() >= 2) {
        const int n = static_cast<int>(app.doc->layer_count());
        const int rel = static_cast<int>(path[1].as_number());
        int target = rel <= -9999 ? 0 : rel >= 9999 ? n - 1 : std::clamp(app.active_layer() + rel, 0, n - 1);
        const Value& kids = path[2];
        if (kids.is_array()) {
            for (size_t i = 0; i < kids.size(); ++i) {
                if (target < 0 || target >= n || app.doc->layer(static_cast<size_t>(target)).type != LayerType::Group) break;
                const int child = std::max(1, static_cast<int>(kids[i].as_number()));
                target = std::clamp(target + child, 0, n - 1);
            }
        }
        return target;
    }
    return app.active_layer();
}

Value result_ok() { Value r = Value::object(); r.set("ok", Value::boolean(true)); return r; }

}  // namespace

std::string App::do_command(const std::string& name, const Value& p, bool* ok) {
    *ok = true;
    const bool has_doc = doc != nullptr;
    const int layer = active_layer();
    auto need_doc = [&]() { if (!has_doc) { *ok = false; return std::string("no image is open"); } return std::string(); };
    auto need_raster = [&]() { if (!active_is_raster()) { *ok = false; return std::string("the active layer is not a raster layer"); } return std::string(); };
    auto adjust = [&](const std::string& cmd_name, std::function<void(Image&)> fn) -> std::string {
        if (auto e = need_raster(); !e.empty()) return e;
        run(std::make_unique<AdjustCommand>(active_layer(), cmd_name, std::move(fn)));
        return json::dump(result_ok());
    };
    auto num = [&](const char* key, double def) { return p.get(key).as_number(def); };
    auto flag = [&](const char* key, bool def) { return p.get(key).as_bool(def); };
    auto layer_props_from = [&](const Value& general, LayerProps& props) {
        if (const Value* n = general.find("Name")) props.name = n->as_string(props.name);
        if (const Value* o = general.find("Opacity")) props.opacity = static_cast<float>(std::clamp(o->as_number(100), 0.0, 100.0) / 100.0);
        if (const Value* v = general.find("IsVisible")) props.visible = v->as_bool(true);
        if (const Value* b = general.find("BlendMode")) props.blend = blend_param(*b, props.blend);
    };

    // --- files and documents ---
    if (name == "NewFile") {
        const int w = std::max(1, static_cast<int>(num("Width", 300))), h = std::max(1, static_cast<int>(num("Height", 300)));
        new_document(w, h);
        if (doc) {
            Layer& L = doc->layer(0);
            if (flag("Transparent", false)) { L.pixels = Image(w, h, {0, 0, 0, 0}); L.background = false; }
            else { const Color c = color_param(p.get("FillMaterial.Color"), {255, 255, 255, 255}); L.pixels = Image(w, h, {c.r, c.g, c.b, 255}); }
            doc->touch();
        }
        return json::dump(result_ok());
    }
    if (name == "FileOpen") {
        std::string path;
        const Value& list = p.get("FileList");
        if (list.is_array() && list.size() > 0) path = list[0].as_string();
        else path = p.get("FileName").as_string();
        const std::string folder = p.get("Folder").as_string();
        if (!folder.empty() && !path.empty() && path[0] != '/') path = folder + "/" + path;
        if (path.empty() || !open_document(path)) { *ok = false; return "FileOpen: " + (path.empty() ? std::string("no file given") : status); }
        return json::dump(result_ok());
    }
    if (name == "FileSave") { if (auto e = need_doc(); !e.empty()) return e; save(); return json::dump(result_ok()); }
    if (name == "FileSaveAs") {
        if (auto e = need_doc(); !e.empty()) return e;
        const std::string path = p.get("FileName").as_string();
        if (path.empty() || !save_document(path)) { *ok = false; return "FileSaveAs: " + status; }
        return json::dump(result_ok());
    }
    if (name == "FileClose") { if (has_doc) close_document(current_doc, true); return json::dump(result_ok()); }
    if (name == "SelectDocument") {
        const int idx = static_cast<int>(num("SelectedImage", 0));
        if (idx < 0 || idx >= static_cast<int>(docs.size())) { *ok = false; return "SelectDocument: no image " + std::to_string(idx); }
        activate_document(idx);
        return json::dump(result_ok());
    }
    if (name == "ReturnImageInfo" || name == "ImageInfo") {
        if (auto e = need_doc(); !e.empty()) return e;
        Value r = result_ok();
        r.set("Width", Value::number(doc->width())); r.set("Height", Value::number(doc->height()));
        r.set("LayerCount", Value::number(static_cast<double>(doc->layer_count())));
        r.set("ActiveLayer", Value::number(layer)); r.set("Title", Value::string(doc_title)); r.set("FileName", Value::string(doc_path));
        r.set("HasSelection", Value::boolean(doc->has_selection()));
        bool alpha = false;
        for (size_t i = 0; i < doc->layer_count() && !alpha; ++i) if (!doc->layer(i).background) alpha = true;
        r.set("PixelFormat", Value::string(alpha ? "BGRA" : "BGR"));
        r.set("LayerNum", Value::number(static_cast<double>(doc->layer_count())));
        r.set("Name", Value::string(doc_title));
        r.set("BitsPerPixel", Value::number(alpha ? 32 : 24));
        if (layer >= 0) { r.set("LayerName", Value::string(doc->layer(layer).name)); r.set("LayerType", Value::string(doc->layer(layer).is_vector() ? "Vector" : doc->layer(layer).is_adjustment() ? "Adjustment" : doc->layer(layer).type == LayerType::Group ? "Group" : "Raster")); }
        return json::dump(r);
    }
    if (name == "ReturnLayerProperties") {
        if (auto e = need_doc(); !e.empty()) return e;
        if (layer < 0) { *ok = false; return "no layer"; }
        const Layer& L = doc->layer(layer);
        Value r = result_ok();
        r.set("Name", Value::string(L.name));
        std::string type = "Raster";
        if (L.is_vector()) type = "Vector"; else if (L.type == LayerType::Group) type = "Group";
        else if (L.is_adjustment()) {
            using K = Adjustment::Kind;
            const K k = L.adjustment.kind;
            type = k == K::Levels ? "Levels" : k == K::Curves ? "Curves" : k == K::BrightnessContrast ? "BrightnessContrast" : k == K::ColorBalance ? "ColorBalance" : k == K::HSL ? "HueSatLum" : k == K::ChannelMixer ? "ChannelMixer" : k == K::Invert ? "Invert" : k == K::Threshold ? "Threshold" : "Posterize";
        }
        r.set("LayerType", Value::string(type));
        r.set("IsBackground", Value::boolean(L.background));
        r.set("LayerNum", Value::number(layer));
        r.set("IsVisible", Value::boolean(L.visible));
        r.set("Opacity", Value::number(std::lround(L.opacity * 100)));
        r.set("BlendMode", Value::string(blend_mode_name(L.blend)));
        const raster::Rect b = L.pixels.empty() ? raster::Rect{0, 0, doc->width(), doc->height()} : raster::content_bounds(L.pixels);
        Value rect = Value::array();
        Value pos = Value::array(); pos.push(Value::number(b.x0)); pos.push(Value::number(b.y0));
        rect.push(pos); rect.push(Value::number(b.x1 - b.x0)); rect.push(Value::number(b.y1 - b.y0));
        r.set("LayerRect", rect);
        Value general = Value::object();
        general.set("Name", Value::string(L.name)); general.set("Opacity", Value::number(std::lround(L.opacity * 100)));
        general.set("IsVisible", Value::boolean(L.visible)); general.set("BlendMode", Value::string(blend_mode_name(L.blend)));
        r.set("General", general);
        return json::dump(r);
    }
    if (name == "GetRasterSelectionRect") {
        if (auto e = need_doc(); !e.empty()) return e;
        Value r = result_ok();
        raster::Rect b{0, 0, 0, 0};
        if (doc->has_selection()) {
            const Mask& m = doc->selection();
            int x0 = m.width(), y0 = m.height(), x1 = -1, y1 = -1;
            for (int y = 0; y < m.height(); ++y) for (int x = 0; x < m.width(); ++x) if (m.at(x, y)) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
            if (x1 >= x0) b = {x0, y0, x1 + 1, y1 + 1};
        }
        Value rect = Value::array();
        for (int v : {b.x0, b.y0, b.x1, b.y1}) rect.push(Value::number(v));
        r.set("Rect", rect); r.set("Left", Value::number(b.x0)); r.set("Top", Value::number(b.y0)); r.set("Right", Value::number(b.x1)); r.set("Bottom", Value::number(b.y1));
        r.set("HasSelection", Value::boolean(doc->has_selection()));
        r.set("Type", Value::string(doc->has_selection() ? "Rectangle" : "None"));
        return json::dump(r);
    }
    if (name == "Mover" || name == "AdjustPosition") {
        if (auto e = need_doc(); !e.empty()) return e;
        const Value& off = p.get("Offset");
        const int dx = static_cast<int>(off[0].as_number()), dy = static_cast<int>(off[1].as_number());
        if (p.get("Object").as_string("Layer") == "Selection" && doc->has_selection()) {
            const Mask& src = doc->selection();
            Mask m(src.width(), src.height());
            for (int y = 0; y < m.height(); ++y) for (int x = 0; x < m.width(); ++x) { const int sx = x - dx, sy = y - dy; if (sx >= 0 && sy >= 0 && sx < m.width() && sy < m.height()) m.at(x, y) = src.at(sx, sy); }
            set_selection("Move Selection", std::move(m));
            return json::dump(result_ok());
        }
        if (auto e = need_raster(); !e.empty()) return e;
        const Layer& L = doc->layer(layer);
        Image moved = raster::shifted(L.pixels, dx, dy);
        if (L.background) { const Color fill = background_fill(); uint8_t* q = moved.data(); for (size_t i = 0; i < moved.size_bytes(); i += 4) if (q[i + 3] == 0) { q[i] = fill.r; q[i + 1] = fill.g; q[i + 2] = fill.b; q[i + 3] = 255; } }
        run(std::make_unique<LayerSnapshotCommand>(layer, "Move", L.pixels, std::move(moved)));
        return json::dump(result_ok());
    }
    if (name == "CountImageColors") {
        if (auto e = need_doc(); !e.empty()) return e;
        Value r = result_ok(); r.set("NumberOfColors", Value::number(static_cast<double>(raster::count_colors(doc->composite()))));
        return json::dump(r);
    }
    if (name == "MsgBox") {
        status = "Script: " + p.get("Text").as_string();
        std::fprintf(stderr, "[script] MsgBox: %s\n", p.get("Text").as_string().c_str());
        Value r = result_ok(); r.set("ButtonPressed", Value::number(1));
        return json::dump(r);
    }
    if (name == "UndoLastCmd") { undo(); return json::dump(result_ok()); }
    if (name == "RedoLastUndo") { redo(); return json::dump(result_ok()); }
    if (name == "AddGuide") {
        if (auto e = need_doc(); !e.empty()) return e;
        (flag("IsHorizontal", true) ? guides_h() : guides_v()).push_back(static_cast<float>(num("Position", 0)));
        return json::dump(result_ok());
    }
    if (name == "ShowGuides" || name == "ShowGrid") {
        bool& flagref = name == "ShowGrid" ? show_grid : show_guides;
        const std::string want = p.get(name.c_str()).as_string("Toggle");
        flagref = want == "Show" ? true : want == "Hide" ? false : !flagref;
        return json::dump(result_ok());
    }

    // --- layers ---
    if (name == "NewRasterLayer" || name == "NewVectorLayer" || name == "NewLayerGroup") {
        if (auto e = need_doc(); !e.empty()) return e;
        if (name == "NewRasterLayer") layer_new(); else if (name == "NewVectorLayer") layer_new_vector(); else layer_new_group();
        LayerProps before = doc->props(active_layer()), after = before;
        layer_props_from(p.get("General"), after);
        if (!(before == after)) run(std::make_unique<LayerPropertiesCommand>(active_layer(), before, after));
        return json::dump(result_ok());
    }
    if (name.rfind("NewAdjustmentLayer", 0) == 0) {
        if (auto e = need_doc(); !e.empty()) return e;
        const std::string kind = name.substr(18);
        Adjustment a;
        using K = Adjustment::Kind;
        if (kind == "Levels") a.kind = K::Levels; else if (kind == "Curves") a.kind = K::Curves; else if (kind == "BrightnessContrast") a.kind = K::BrightnessContrast;
        else if (kind == "ColorBalance") a.kind = K::ColorBalance; else if (kind == "HSL") a.kind = K::HSL; else if (kind == "ChannelMixer") a.kind = K::ChannelMixer;
        else if (kind == "Invert") a.kind = K::Invert; else if (kind == "Threshold") a.kind = K::Threshold; else if (kind == "Posterize") a.kind = K::Posterize;
        else { *ok = false; return "unknown adjustment layer " + kind; }
        a.brightness = static_cast<int>(p.get("BrightnessContrast.Brightness").as_number(0)); a.contrast = static_cast<int>(p.get("BrightnessContrast.Contrast").as_number(0));
        a.threshold = static_cast<int>(p.get("Threshold.Threshold").as_number(128)); a.posterize = static_cast<int>(p.get("Posterize.Level").as_number(6));
        const Value& lv = p.get("Levels.RGB");
        if (lv.is_array() && lv.size() >= 5) { a.levels[0].in_low = static_cast<int>(lv[0].as_number()); a.levels[0].gamma = static_cast<float>(lv[1].as_number(1)); a.levels[0].in_high = static_cast<int>(lv[2].as_number(255)); a.levels[0].out_low = static_cast<int>(lv[3].as_number()); a.levels[0].out_high = static_cast<int>(lv[4].as_number(255)); }
        const Value& hm = p.get("HSL.Master");
        if (hm.is_array() && hm.size() >= 3) { a.hue = static_cast<int>(hm[0].as_number()); a.saturation = static_cast<int>(hm[1].as_number()); a.lightness = static_cast<int>(hm[2].as_number()); }
        run(std::make_unique<AddAdjustmentLayerCommand>(Adjustment::kind_name(a.kind), a));
        LayerProps before = doc->props(active_layer()), after = before;
        layer_props_from(p.get("General"), after);
        if (!(before == after)) run(std::make_unique<LayerPropertiesCommand>(active_layer(), before, after));
        return json::dump(result_ok());
    }
    if (name == "LayerProperties") {
        if (auto e = need_doc(); !e.empty()) return e;
        const int target = layer_from_params(*this, p);
        if (target < 0) { *ok = false; return "no layer"; }
        doc->set_active_layer(target);
        LayerProps before = doc->props(target), after = before;
        layer_props_from(p.get("General"), after);
        if (!(before == after)) run(std::make_unique<LayerPropertiesCommand>(target, before, after));
        return json::dump(result_ok());
    }
    if (name == "SelectLayer") {
        if (auto e = need_doc(); !e.empty()) return e;
        const int target = layer_from_params(*this, p);
        if (target < 0) { *ok = false; return "no such layer"; }
        doc->set_active_layer(target);
        return json::dump(result_ok());
    }
    if (name == "LayerDuplicate") { if (auto e = need_doc(); !e.empty()) return e; layer_duplicate(); return json::dump(result_ok()); }
    if (name == "DeleteLayer") { if (auto e = need_doc(); !e.empty()) return e; doc->set_active_layer(layer_from_params(*this, p)); layer_delete(); return json::dump(result_ok()); }
    if (name == "LayerMergeAll") { if (auto e = need_doc(); !e.empty()) return e; layer_merge(2); return json::dump(result_ok()); }
    if (name == "LayerMergeVisible") { if (auto e = need_doc(); !e.empty()) return e; layer_merge(1); return json::dump(result_ok()); }
    if (name == "LayerMergeDown") { if (auto e = need_doc(); !e.empty()) return e; layer_merge(0); return json::dump(result_ok()); }
    if (name == "LayerPromoteBackground") { if (auto e = need_doc(); !e.empty()) return e; layer_promote_background(); return json::dump(result_ok()); }
    if (name == "LayerConvertToRaster") { if (auto e = need_doc(); !e.empty()) return e; layer_convert_to_raster(); return json::dump(result_ok()); }
    if (name == "LayerArrange" || name == "LayerArrangeMoveUp" || name == "LayerArrangeMoveDown" || name == "LayerArrangeToTop" || name == "LayerArrangeToBottom") {
        if (auto e = need_doc(); !e.empty()) return e;
        const int n = static_cast<int>(doc->layer_count());
        if (name == "LayerArrangeToTop") layer_arrange(n); else if (name == "LayerArrangeToBottom") layer_arrange(-n);
        else if (name == "LayerArrangeMoveDown") layer_arrange(-1);
        else layer_arrange(name == "LayerArrangeMoveUp" || flag("MoveAboveSibling", true) ? 1 : -1);
        return json::dump(result_ok());
    }
    if (name == "LayerArrangeMoveIn" || name == "LayerArrangeMoveOut") {
        if (auto e = need_doc(); !e.empty()) return e;
        const size_t idx = static_cast<size_t>(layer);
        if (name == "LayerArrangeMoveIn") {
            if (idx == 0) return json::dump(result_ok());
            const Layer& below = doc->layer(idx - 1);
            const int depth = below.type == LayerType::Group ? below.depth + 1 : below.depth;
            if (depth <= doc->layer(idx).depth) return json::dump(result_ok());
            run(std::make_unique<StateEditCommand>("Move Into Group", [idx, depth](Document& d) { d.layer(idx).depth = depth; }));
        } else {
            if (doc->layer(idx).depth == 0) return json::dump(result_ok());
            run(std::make_unique<StateEditCommand>("Move Out Of Group", [idx](Document& d) {
                const int depth = d.layer(idx).depth - 1;
                // Find the enclosing group and place the layer just above its span.
                size_t g = idx;
                while (g > 0 && !(d.layer(g).type == LayerType::Group && d.layer(g).depth == depth)) --g;
                const size_t end = d.group_end(g);
                std::unique_ptr<Layer> moved = d.remove_layer(idx);
                moved->depth = depth;
                d.insert_layer(std::move(moved), end - 1);
                d.set_active_layer(static_cast<int>(end - 1));
            }));
        }
        return json::dump(result_ok());
    }
    if (name == "LayerArrangeUngroup") { if (auto e = need_doc(); !e.empty()) return e; layer_ungroup(); return json::dump(result_ok()); }
    if (name == "LayerSetVisibility") {
        if (auto e = need_doc(); !e.empty()) return e;
        const int target = layer_from_params(*this, p);
        LayerProps before = doc->props(target), after = before;
        const std::string cmd = p.get("Command").as_string("Toggle");
        after.visible = cmd == "Show" ? true : cmd == "Hide" ? false : !before.visible;
        doc->set_active_layer(target);
        run(std::make_unique<LayerPropertiesCommand>(target, before, after));
        return json::dump(result_ok());
    }
    if (name == "NewMaskLayerShow" || name == "NewMaskLayerHide") {
        if (auto e = need_doc(); !e.empty()) return e;
        if (flag("UseSelection", true) && doc->has_selection()) layer_mask_from_selection();
        else layer_set_mask("New Mask Layer", Mask(doc->width(), doc->height(), name == "NewMaskLayerShow" ? 255 : 0));
        return json::dump(result_ok());
    }

    // --- selections and clipboard ---
    if (name == "SelectAll") { if (auto e = need_doc(); !e.empty()) return e; select_all(); return json::dump(result_ok()); }
    if (name == "SelectNone") { if (auto e = need_doc(); !e.empty()) return e; select_none(); return json::dump(result_ok()); }
    if (name == "SelectInvert") { if (auto e = need_doc(); !e.empty()) return e; select_invert(); return json::dump(result_ok()); }
    if (name == "Selection" || name == "ModifySelection") {
        if (auto e = need_doc(); !e.empty()) return e;
        raster::Rect r;
        if (name == "Selection") {
            const Value& a = p.get("Start"); const Value& b = p.get("End");
            r = {static_cast<int>(a[0].as_number()), static_cast<int>(a[1].as_number()), static_cast<int>(b[0].as_number()), static_cast<int>(b[1].as_number())};
        } else r = rect_param(p.get("Selection"), {});
        const int x0 = std::min(r.x0, r.x1), x1 = std::max(r.x0, r.x1), y0 = std::min(r.y0, r.y1), y1 = std::max(r.y0, r.y1);
        const bool ellipse = p.get("SelectionShape").as_string("Rectangle") == "Ellipse";
        const bool aa = p.get("General.Antialias").as_bool(true);
        Mask m = ellipse ? mask::ellipse(doc->width(), doc->height(), (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (x1 - x0) * 0.5f, (y1 - y0) * 0.5f, aa)
                         : mask::rectangle(doc->width(), doc->height(), static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1), static_cast<float>(y1), aa);
        const std::string mode = p.get("General.Mode").as_string("Replace");
        sel_mode = mode == "Add" ? 1 : mode == "Remove" ? 2 : mode == "Intersect" ? 3 : 0;
        sel_feather = static_cast<float>(p.get("General.Feather").as_number(0));
        apply_selection_gesture("Selection", std::move(m));
        return json::dump(result_ok());
    }
    if (name == "SelectFeather" || name == "SelectExpand" || name == "SelectContract") {
        if (auto e = need_doc(); !e.empty()) return e;
        if (!doc->has_selection()) return json::dump(result_ok());
        Mask m = doc->selection();
        if (name == "SelectFeather") mask::feather(m, static_cast<float>(num("FeatherAmount", 1)));
        else if (name == "SelectExpand") mask::expand(m, static_cast<int>(num("ExpandAmount", 1)));
        else mask::contract(m, static_cast<int>(num("ContractAmount", 1)));
        set_selection(name == "SelectFeather" ? "Feather" : name == "SelectExpand" ? "Expand Selection" : "Contract Selection", std::move(m));
        return json::dump(result_ok());
    }
    if (name == "SelectPromote") {
        if (auto e = need_doc(); !e.empty()) return e;
        copy();
        paste_as_new_layer();
        if (!p.get("LayerName").as_string().empty() && active_layer() >= 0) { LayerProps b = doc->props(active_layer()), a = b; a.name = p.get("LayerName").as_string(); run(std::make_unique<LayerPropertiesCommand>(active_layer(), b, a)); }
        return json::dump(result_ok());
    }
    if (name == "Copy") { if (auto e = need_doc(); !e.empty()) return e; copy(); return json::dump(result_ok()); }
    if (name == "Cut") { if (auto e = need_doc(); !e.empty()) return e; cut(); return json::dump(result_ok()); }
    if (name == "ClearSelection") { if (auto e = need_doc(); !e.empty()) return e; clear_selection(); return json::dump(result_ok()); }
    if (name == "PasteAsNewLayer") { if (auto e = need_doc(); !e.empty()) return e; paste_as_new_layer(); return json::dump(result_ok()); }
    if (name == "PasteAsNewImage") { paste_as_new_image(); return json::dump(result_ok()); }

    // --- geometry ---
    if (name == "Resize") {
        if (auto e = need_doc(); !e.empty()) return e;
        const bool percent = p.get("CurrentDimensionUnits").as_string("Percent") == "Percent";
        double w = num("Width", 0), h = num("Height", 0);
        if (percent) { w = doc->width() * (w > 0 ? w : 100) / 100.0; h = doc->height() * (h > 0 ? h : 100) / 100.0; }
        if (flag("MaintainAspectRatio", true) && w > 0) h = doc->height() * w / doc->width();
        const std::string rt = p.get("ResampleType").as_string("SmartSize");
        // The original's own scripts all pass SmartSize, its default.
        const raster::Filter f = rt == "Pixel" ? raster::Filter::Nearest : rt == "Bilinear" ? raster::Filter::Bilinear
                                 : rt == "EdgeDirected" ? raster::Filter::EdgeDirected
                                 : rt == "Bicubic" ? raster::Filter::Bicubic : raster::Filter::Smart;
        run(std::make_unique<ResizeCommand>(std::max(1, static_cast<int>(std::lround(w))), std::max(1, static_cast<int>(std::lround(h))), f));
        fit_requested = true;
        return json::dump(result_ok());
    }
    if (name == "Rotate") {
        if (auto e = need_doc(); !e.empty()) return e;
        const double deg = num("RotAngleDegrees", 90);
        rotate(static_cast<float>(flag("Direction", false) ? -deg : deg));   // Direction True = counter-clockwise
        return json::dump(result_ok());
    }
    if (name == "Crop" || name == "CropToSelection") {
        if (auto e = need_doc(); !e.empty()) return e;
        if (name == "CropToSelection" || flag("SelectedArea", false)) crop_to_selection();
        else crop_to(rect_param(p.get("CropRect"), {0, 0, doc->width(), doc->height()}));
        return json::dump(result_ok());
    }
    if (name == "Flip") { if (auto e = need_doc(); !e.empty()) return e; run(std::make_unique<FlipCommand>()); return json::dump(result_ok()); }
    if (name == "Mirror") { if (auto e = need_doc(); !e.empty()) return e; run(std::make_unique<MirrorCommand>()); return json::dump(result_ok()); }
    if (name == "ResizeCanvas") {
        if (auto e = need_doc(); !e.empty()) return e;
        const int nw = std::max(1, static_cast<int>(num("NewWidth", doc->width()))), nh = std::max(1, static_cast<int>(num("NewHeight", doc->height())));
        const std::string hp = p.get("HoriPlace").as_string("Center"), vp = p.get("VertPlace").as_string("Center");
        int ox = (nw - doc->width()) / 2, oy = (nh - doc->height()) / 2;
        if (hp == "Left") ox = 0; else if (hp == "Right") ox = nw - doc->width(); else if (hp == "Custom") ox = static_cast<int>(num("PlaceLeft", ox));
        if (vp == "Top") oy = 0; else if (vp == "Bottom") oy = nh - doc->height(); else if (vp == "Custom") oy = static_cast<int>(num("PlaceTop", oy));
        run(std::make_unique<CanvasSizeCommand>(nw, nh, ox, oy, color_param(p.get("FillColor"), background_fill())));
        fit_requested = true;
        return json::dump(result_ok());
    }
    if (name == "AddBorders") {
        if (auto e = need_doc(); !e.empty()) return e;
        const int l = static_cast<int>(num("Left", 10)), r = static_cast<int>(num("Right", 10)), t = static_cast<int>(num("Top", 10)), b = static_cast<int>(num("Bottom", 10));
        run(std::make_unique<CanvasSizeCommand>(doc->width() + l + r, doc->height() + t + b, l, t, color_param(p.get("Color"), background_fill())));
        return json::dump(result_ok());
    }

    // --- adjustments ---
    if (name == "ColorAdjustBrightnessContrast") return adjust("Brightness/Contrast", [b = static_cast<int>(p.get("BrightnessContrast.Brightness").as_number()), c = static_cast<int>(p.get("BrightnessContrast.Contrast").as_number())](Image& i) { adjust::apply_lut(i, adjust::brightness_contrast_lut(b, c)); });
    if (name == "ColorAdjustHSL") {
        const Value& m = p.get("HSL.Master");
        const int h = static_cast<int>(m[0].as_number()), s = static_cast<int>(m[1].as_number()), l = static_cast<int>(m[2].as_number());
        if (p.get("HSL.Colorize").as_bool(false)) { const Value& mc = p.get("HSL.MasterColorize"); return adjust("Colorize", [hh = static_cast<int>(mc[0].as_number()), ss = static_cast<int>(mc[1].as_number())](Image& i) { adjust::colorize(i, hh, ss * 255 / 100); }); }
        return adjust("Hue/Saturation/Lightness", [h, s, l](Image& i) { adjust::hsl_adjust(i, h, s, l); });
    }
    if (name == "ColorAdjustLevels") {
        const Value& lv = p.get("Levels.RGB");
        const int lo = static_cast<int>(lv[0].as_number(0)), hi = static_cast<int>(lv[2].as_number(255)), olo = static_cast<int>(lv[3].as_number(0)), ohi = static_cast<int>(lv[4].as_number(255));
        const float g = static_cast<float>(lv[1].as_number(1.0));
        return adjust("Levels", [=](Image& i) { adjust::apply_lut(i, adjust::levels_lut(lo, g, hi, olo, ohi)); });
    }
    if (name == "ColorAdjustThreshold") return adjust("Threshold", [t = static_cast<int>(p.get("Threshold.Threshold").as_number(128))](Image& i) { adjust::grayscale_then_threshold(i, t); });
    if (name == "ColorAdjustGammaCorrect") {
        const float r = static_cast<float>(p.get("Gamma.Red").as_number(1)), g = static_cast<float>(p.get("Gamma.Green").as_number(1)), b = static_cast<float>(p.get("Gamma.Blue").as_number(1));
        return adjust("Gamma Correction", [=](Image& i) { adjust::apply_luts(i, adjust::gamma_lut(r), adjust::gamma_lut(g), adjust::gamma_lut(b)); });
    }
    if (name == "ColorAdjustColorBalance") {
        adjust::ColorBalance cb;
        cb.preserve_luminosity = p.get("ColorBalance.PreserveLuminance").as_bool(true);
        for (int i = 0; i < 3; ++i) { cb.highlights[i] = static_cast<int>(p.get("ColorBalance.Highlight")[static_cast<size_t>(i)].as_number()); cb.midtones[i] = static_cast<int>(p.get("ColorBalance.Midtone")[static_cast<size_t>(i)].as_number()); cb.shadows[i] = static_cast<int>(p.get("ColorBalance.Shadow")[static_cast<size_t>(i)].as_number()); }
        return adjust("Color Balance", [cb](Image& i) { adjust::color_balance(i, cb); });
    }
    if (name == "ColorAdjustChannelMixer") {
        adjust::ChannelMix m;
        m.monochrome = p.get("ChannelMixer.Monochrome").as_bool(false);
        const char* rows[3] = {"Red", "Green", "Blue"};
        for (int r = 0; r < 3; ++r) { const Value& t = p.get(std::string("ChannelMixer.") + rows[r]); if (t.is_array() && t.size() >= 4) { for (int c = 0; c < 3; ++c) m.mix[r][c] = static_cast<float>(t[static_cast<size_t>(c)].as_number()); m.constant[r] = static_cast<float>(t[3].as_number()); } }
        return adjust("Channel Mixer", [m](Image& i) { adjust::channel_mixer(i, m); });
    }
    if (name == "Colorize") return adjust("Colorize", [h = static_cast<int>(num("Hue", 0)), s = static_cast<int>(num("Saturation", 128))](Image& i) { adjust::colorize(i, h, s); });
    if (name == "NegativeImage") return adjust("Negative Image", [](Image& i) { adjust::Lut lut; for (int k = 0; k < 256; ++k) lut[static_cast<size_t>(k)] = static_cast<uint8_t>(255 - k); adjust::apply_lut(i, lut); });
    if (name == "Greyscale" || name == "Grayscale") return adjust("Grayscale", raster::grayscale);
    if (name == "Posterize") return adjust("Posterize", [l = static_cast<int>(num("Levels", 7))](Image& i) { adjust::apply_lut(i, adjust::posterize_lut(l)); });
    if (name == "Solarize") return adjust("Solarize", [t = static_cast<int>(num("Threshold", 254))](Image& i) { adjust::apply_lut(i, adjust::solarize_lut(t)); });
    if (name == "Sepia") return adjust("Sepia Toning", [a = static_cast<int>(num("Percent", 75))](Image& i) { adjust::sepia(i, a); });
    if (name == "FadeCorrection") return adjust("Fade Correction", [a = static_cast<int>(num("CorrectionAmount", 45))](Image& i) { adjust::fade_correction(i, a); });
    if (name == "Clarify") return adjust("Clarify", [s = static_cast<int>(num("Strength", 2))](Image& i) { photo::clarify(i, s); });
    if (name == "OneStepPhotoFix") return adjust("One Step Photo Fix", photo::one_step_photo_fix);
    if (name == "AutoContrastEnhancement") {
        const std::string bias = p.get("Bias").as_string("Neutral"), strength = p.get("Strength").as_string("Normal"), app = p.get("Appearance").as_string("Natural");
        return adjust("Automatic Contrast Enhancement", [=](Image& i) { photo::auto_contrast_enhance(i, bias == "Lighter" ? 0 : bias == "Darker" ? 2 : 1, strength == "Mild" ? 1 : 0, app == "Flat" ? 0 : app == "Bold" ? 2 : 1); });
    }
    if (name == "AutoSaturationEnhancement")
        return adjust("Automatic Saturation Enhancement", [b = static_cast<int>(num("Bias", 1)), st = static_cast<int>(num("Strength", 1)),
                                                          skin = flag("Skintones", false)](Image& i) { photo::auto_saturation(i, b, st, skin); });
    if (name == "AutoColorBalance")
        return adjust("Automatic Color Balance", [s = static_cast<int>(num("Strength", 30)), t = static_cast<int>(num("Temperature", 6500)),
                                                  cast = flag("RemoveColorCast", false)](Image& i) { photo::auto_color_balance(i, s, t, cast); });
    if (name == "HistogramEqualize") return adjust("Histogram Equalize", adjust::histogram_equalize);
    if (name == "HistogramStretch") return adjust("Histogram Stretch", adjust::histogram_stretch);

    // --- commands the bundled scripts use, with their parameter names ---
    if (name == "SelectPreviousTool") {
        if (prev_tool_index < 0) { *ok = false; return std::string("no previous tool"); }
        select_tool(prev_tool_index);
        return json::dump(result_ok());
    }
    if (name == "FloatSelection") {
        if (auto e = need_doc(); !e.empty()) return e;
        if (!doc->has_selection()) { *ok = false; return std::string("no selection"); }
        promote_selection_to_layer(true);
        return json::dump(result_ok());
    }
    if (name == "SavePalette") {
        const std::string path = p.get("SavePaletteFileName").as_string("");
        if (path.empty()) { *ok = false; return std::string("SavePalette needs SavePaletteFileName"); }
        save_palette(path);
        return json::dump(result_ok());
    }
    // The original's script runner batches undo; we always record one entry
    // per command, so there is nothing to switch on.
    if (name == "EnableOptimizedScriptUndo") return json::dump(result_ok());

    if (name == "ContentAwareFill") { if (auto e = need_doc(); !e.empty()) return e; content_aware_fill(); return json::dump(result_ok()); }
    if (name == "PasteIntoSelection") { if (auto e = need_doc(); !e.empty()) return e; paste_into_selection(); return json::dump(result_ok()); }
    if (name == "MaskShowAll") {
        if (auto e = need_doc(); !e.empty()) return e;
        layer_set_mask("New Mask Layer", Mask(doc->width(), doc->height(), 255));
        return json::dump(result_ok());
    }
    if (name == "SplitToRGB" || name == "SplitToHSL" || name == "SplitToCMYK") {
        if (auto e = need_doc(); !e.empty()) return e;
        image_split_channels(name == "SplitToRGB" ? 0 : name == "SplitToHSL" ? 1 : 2);
        return json::dump(result_ok());
    }
    if (name == "SelectTool") {
        // The original's tool names; ours differ in a few places.
        std::string want = p.get("Tool").as_string("");
        static const std::pair<const char*, const char*> alias[] = {
            {"Paintbrush", "Paint Brush"}, {"Airbrush", "Airbrush"}, {"Mover", "Move"}, {"Dropper", "Dropper"},
            {"CloneBrush", "Clone Brush"}, {"ColorReplacer", "Color Replacer"}, {"ScratchRemover", "Scratch Remover"},
            {"RedEye", "Red-eye Removal"}, {"FloodFill", "Flood Fill"}, {"PictureTube", "Picture Tube"},
            {"PresetShapes", "Preset Shape"}, {"VectorObjectSelector", "Object Selector"}, {"Text", "Text"},
            {"Freehand", "Freehand Selection"}, {"MagicWand", "Magic Wand"}, {"WarpBrush", "Warp Brush"},
            {"MeshWarp", "Mesh Warp"}, {"Straighten", "Straighten"}, {"PerspectiveCorrection", "Perspective Correction"}};
        for (const auto& a : alias) if (want == a.first) { want = a.second; break; }
        for (size_t i = 0; i < tools.size(); ++i)
            if (want == tools[i]->name()) { select_tool(static_cast<int>(i)); return json::dump(result_ok()); }
        *ok = false;
        return "no tool named " + want;
    }
    if (name == "SelectSmooth") {
        if (auto e = need_doc(); !e.empty()) return e;
        if (!doc->has_selection()) { *ok = false; return std::string("no selection"); }
        Mask m = doc->selection();
        mask::smooth(m, static_cast<int>(num("SmoothAmount", 50)), flag("PreserveCorners", false));
        if (flag("Antialias", false)) mask::shape_antialias(m, true, true);
        set_selection("Smooth Selection", std::move(m));
        return json::dump(result_ok());
    }
    if (name == "SelectSaveAlpha") {
        if (auto e = need_doc(); !e.empty()) return e;
        if (!doc->has_selection()) { *ok = false; return std::string("no selection"); }
        const std::string alpha = p.get("AlphaName").as_string("Selection #" + std::to_string(doc->alpha_channels().size() + 1));
        auto& channels = doc->alpha_channels();
        for (auto& ch : channels)
            if (ch.name == alpha) {
                if (!flag("Overwrite", false)) { *ok = false; return "an alpha channel named " + alpha + " already exists"; }
                ch.mask = doc->selection();
                return json::dump(result_ok());
            }
        channels.push_back({alpha, doc->selection()});
        return json::dump(result_ok());
    }
    if (name == "SelectLoadAlpha") {
        if (auto e = need_doc(); !e.empty()) return e;
        const auto& channels = doc->alpha_channels();
        if (channels.empty()) { *ok = false; return std::string("the image has no alpha channels"); }
        const std::string alpha = p.get("AlphaName").as_string("");
        size_t index = static_cast<size_t>(std::max(0.0, num("AlphaIndex", 0)));
        if (!alpha.empty()) {
            bool found = false;
            for (size_t i = 0; i < channels.size(); ++i) if (channels[i].name == alpha) { index = i; found = true; break; }
            if (!found) { *ok = false; return "no alpha channel named " + alpha; }
        }
        if (index >= channels.size()) { *ok = false; return std::string("alpha channel out of range"); }
        Mask m = channels[index].mask;
        if (flag("Invert", false)) mask::invert(m);
        const std::string mode = p.get("SelectionOperation").as_string("Replace");
        if (mode != "Replace" && doc->has_selection()) {
            Mask combined = doc->selection();
            mask::combine(combined, m, mode == "Add" ? mask::Combine::Add : mode == "Subtract" ? mask::Combine::Subtract : mask::Combine::Intersect);
            m = std::move(combined);
        }
        set_selection("Load Selection From Alpha Channel", std::move(m));
        return json::dump(result_ok());
    }

    // --- effects ---
    if (name == "EdgePreservingSmooth") return adjust("Edge Preserving Smooth", [f = static_cast<int>(num("SmoothingFactor", 30))](Image& i) { photo::edge_preserving_smooth(i, f); });
    if (name == "BrushStrokes")
        // Our filter takes length, density, width and opacity; the original's
        // bristle, angle, color and softness settings have no equivalent.
        return adjust("Brush Strokes", [len = static_cast<int>(num("Length", 15)), den = static_cast<int>(num("Density", 26)),
                                        w = static_cast<int>(num("Width", 6)), op = static_cast<int>(num("Opacity", 0))](Image& i) { effects::brush_strokes(i, len, den, w, op, 1); });
    if (name == "InnerBevel") {
        const Mask* sel = doc && doc->has_selection() ? &doc->selection() : nullptr;
        return adjust("Inner Bevel", [region = sel ? sel->data() : nullptr, w = static_cast<int>(num("Width", 8)),
                                      ang = static_cast<float>(num("Angle", 315)), d = static_cast<float>(num("Depth", 20)),
                                      amb = static_cast<float>(num("Ambience", 0))](Image& i) { effects::inner_bevel(i, region, w, ang, d, amb); });
    }
    if (name == "BlurAverage") return adjust("Average", [r = std::max(1, static_cast<int>(num("Aperture", 3)) / 2)](Image& i) { raster::box_blur(i, r); });
    if (name == "GlowingEdges") return adjust("Glowing Edges", [in = static_cast<int>(num("Intensity", 50)), sh = static_cast<int>(num("Sharpness", 50))](Image& i) { effects::glowing_edges(i, in, sh); });
    if (name == "ColoredEdges") return adjust("Colored Edges", [lum = static_cast<int>(num("Luminance", 50)), b = static_cast<int>(num("Blur", 3)), c = color_param(p.get("Color"), {0, 0, 0, 255})](Image& i) { effects::colored_edges(i, lum, b, c); });
    if (name == "SaltAndPepper")
        // "Agressive" is the original's spelling; the scripts use it verbatim.
        return adjust("Salt and Pepper Filter", [sz = static_cast<int>(num("SpeckSize", 3)), sn = static_cast<int>(num("Sensitivity", 5)),
                                                 lower = flag("IncludeAllLowerSizes", true), agg = flag("Agressive", false)](Image& i) { photo::salt_and_pepper(i, sz, sn, lower, agg); });
    if (name == "JPEGArtifactRemoval") {
        static const char* kStrength[] = {"Low", "Normal", "High", "Maximum"};
        const std::string want = p.get("Strength").as_string("Normal");
        int strength = 1;
        for (int i = 0; i < 4; ++i) if (want == kStrength[i]) strength = i;
        return adjust("JPEG Artifact Removal", [strength, cr = static_cast<int>(num("RestoreCrispness", 50))](Image& i) { photo::jpeg_artifact_removal(i, strength, cr); });
    }
    if (name == "DigitalCameraNoiseRemoval")
        // One strength from the three detail sliders, which is what our filter takes.
        return adjust("Digital Camera Noise Removal",
                      [st = static_cast<int>((num("SmallDetails", 50) + num("MediumDetails", 50) + num("LargeDetails", 50)) / 3.0),
                       bl = static_cast<int>(num("Blending", 70)), sh = static_cast<int>(num("Sharpening", 0))](Image& i) { photo::noise_removal(i, st, bl, sh); });
    if (name == "ColorAdjustCurves") {
        std::vector<std::pair<float, float>> pts;
        const Value& rgb = p.get("CurveParams.RGB");
        for (size_t i = 0; i < rgb.size(); ++i)
            if (rgb[i].size() >= 2) pts.emplace_back(static_cast<float>(rgb[i][0].as_number()), static_cast<float>(rgb[i][1].as_number()));
        if (pts.size() < 2) pts = {{0, 0}, {255, 255}};
        return adjust("Curves", [pts](Image& i) { adjust::apply_lut(i, adjust::curve_lut(pts)); });
    }
    if (name == "ColorAdjustHueMap") {
        adjust::HueMap m;
        const Value& shifts = p.get("HueShift");
        for (size_t i = 0; i < shifts.size() && i < 10; ++i) m.shift[i] = static_cast<int>(shifts[i].as_number());
        m.saturation = static_cast<int>(num("SaturationShift", 0));
        m.lightness = static_cast<int>(num("LightnessShift", 0));
        return adjust("Hue Map", [m](Image& i) { adjust::hue_map(i, m); });
    }
    if (name == "HistogramAdjustment") {
        // The luminance channel's clip limits, gamma and output range as a LUT.
        const Value& ch = p.get("LuminanceChannel");
        const int low = static_cast<int>(ch.get("LowClipLimit").as_number(0)), high = static_cast<int>(ch.get("HighClipLimit").as_number(255));
        const float gamma = static_cast<float>(ch.get("Gamma").as_number(1.0));
        const int out_low = static_cast<int>(ch.get("MinOutput").as_number(0)), out_high = static_cast<int>(ch.get("MaxOutput").as_number(255));
        return adjust("Histogram Adjustment", [=](Image& i) { adjust::apply_lut(i, adjust::levels_lut(low, gamma, high, out_low, out_high)); });
    }

    if (name == "GaussianBlur") return adjust("Gaussian Blur", [r = static_cast<float>(num("Radius", 1))](Image& i) { raster::gaussian_blur(i, r); });
    if (name == "Blur") return adjust("Blur", effects::soften);
    if (name == "BlurMore") return adjust("Blur More", effects::blur_more);
    if (name == "Sharpen") return adjust("Sharpen", effects::sharpen);
    if (name == "SharpenMore") return adjust("Sharpen More", effects::sharpen_more);
    if (name == "Soften") return adjust("Soften", effects::soften);
    if (name == "SoftenMore") return adjust("Soften More", effects::soften_more);
    if (name == "UnsharpMask") return adjust("Unsharp Mask", [r = static_cast<float>(num("Radius", 2)), s = static_cast<int>(num("Strength", 100)), c = static_cast<int>(num("Clipping", 5))](Image& i) { effects::unsharp_mask(i, r, s, c); });
    if (name == "Median") return adjust("Median Filter", [a = static_cast<int>(num("Aperture", 3))](Image& i) { effects::median(i, std::max(1, a / 2)); });
    if (name == "Despeckle") return adjust("Despeckle", [](Image& i) { effects::median(i, 1); });
    if (name == "MotionBlur") return adjust("Motion Blur", [a = static_cast<float>(num("Angle", 0)), s = static_cast<int>(num("Strength", 10))](Image& i) { effects::motion_blur(i, a, s); });
    if (name == "AddNoise") return adjust("Add Noise", [a = static_cast<int>(num("NoiseAmount", 50)), g = p.get("NoiseType").as_string("Random") == "Gaussian", m = flag("Monochrome", false)](Image& i) { effects::add_noise(i, a, g, m); });
    if (name == "FindEdges") return adjust("Find Edges", effects::find_edges);
    if (name == "EnhanceEdges") return adjust("Enhance Edges", effects::enhance_edges);
    if (name == "EnhanceEdgesMore") return adjust("Enhance Edges More", effects::enhance_edges_more);
    if (name == "Emboss") return adjust("Emboss", effects::emboss);
    if (name == "Erode") return adjust("Erode", effects::erode);
    if (name == "Dilate") return adjust("Dilate", effects::dilate);
    if (name == "Pixelate" || name == "Mosaic") return adjust("Mosaic", [w = static_cast<int>(num("BlockWidth", 4)), h = static_cast<int>(num("BlockHeight", 4))](Image& i) { effects::mosaic(i, w, h); });
    if (name == "DropShadow") {
        const bool new_layer = flag("NewLayer", false);
        const int ox = static_cast<int>(num("Horizontal", 10)), oy = static_cast<int>(num("Vertical", 10));
        const float op = static_cast<float>(num("Opacity", 50) / 100.0), blur = static_cast<float>(num("Blur", 5));
        const Color c = color_param(p.get("Color"), {0, 0, 0, 255});
        if (new_layer) {
            if (auto e = need_raster(); !e.empty()) return e;
            // The shadow alone on a layer beneath this one.
            Image shadow = doc->layer(active_layer()).pixels;
            const Image original = shadow;
            effects::drop_shadow(shadow, ox, oy, op, blur, c);
            uint8_t* d = shadow.data(); const uint8_t* o = original.data();
            for (size_t i = 0; i < shadow.size_bytes(); i += 4) if (o[i + 3]) { d[i] = d[i + 1] = d[i + 2] = 0; d[i + 3] = 0; }
            const int above = active_layer();
            run(std::make_unique<PasteLayerCommand>("Drop Shadow", std::move(shadow)));
            layer_arrange(-1);
            doc->set_active_layer(std::min(above + 1, static_cast<int>(doc->layer_count()) - 1));
            return json::dump(result_ok());
        }
        return adjust("Drop Shadow", [=](Image& i) { effects::drop_shadow(i, ox, oy, op, blur, c); });
    }
    if (name == "UserDefinedFilter") {
        float k[25] = {0};
        const Value& kv = p.get("Kernel");
        for (size_t i = 0; i < 25 && i < kv.size(); ++i) k[i] = static_cast<float>(kv[i].as_number());
        const float div = static_cast<float>(num("Divisor", 1)), bias = static_cast<float>(num("Bias", 0));
        return adjust("User Defined Filter", [=](Image& i) { effects::user_defined_filter(i, k, div, bias); });
    }
    if (name == "Wave") return adjust("Wave", [ha = static_cast<float>(num("HorzAmplitude", 5)), hw = static_cast<float>(num("HorzWavelength", 40)), va = static_cast<float>(num("VertAmplitude", 0)), vw = static_cast<float>(num("VertWavelength", 40))](Image& i) { effects::wave(i, ha, hw, va, vw); });
    if (name == "Pinch") return adjust("Pinch", [s = static_cast<int>(num("Strength", 50))](Image& i) { effects::pinch(i, s); });
    if (name == "Twirl") return adjust("Twirl", [d = static_cast<float>(num("Degrees", 90))](Image& i) { effects::twirl(i, d); });
    if (name == "Ripple") return adjust("Ripple", [a = static_cast<float>(num("Amplitude", 5)), w = static_cast<float>(num("Wavelength", 30))](Image& i) { effects::ripple(i, a, w); });
    if (name == "Spherize") return adjust("Spherize", [s = static_cast<int>(num("Strength", 50))](Image& i) { effects::spherize(i, s); });
    if (name == "Kaleidoscope") return adjust("Kaleidoscope", [pt = static_cast<int>(num("NumberOfPetals", 6)), a = static_cast<float>(num("RotationAngle", 0)), r = static_cast<float>(num("RadialSuction", 50))](Image& i) { effects::kaleidoscope(i, pt, a, r); });
    if (name == "PageCurl") return adjust("Page Curl", [c = color_param(p.get("BackColor"), {230, 230, 230, 255}), f = color_param(p.get("Color"), {255, 255, 255, 255}), r = static_cast<int>(num("Radius", 30)), corner = static_cast<int>(num("Corner", 3))](Image& i) { effects::page_curl(i, corner, 40, 40, r, c, f, false); });
    if (name == "Buttonize") return adjust("Buttonize", [w = static_cast<int>(num("Width", 10)), o = static_cast<float>(num("Opacity", 75) / 100.0), c = color_param(p.get("Color"), {128, 128, 128, 255}), t = flag("Transparent", false)](Image& i) { effects::buttonize(i, w, o, c, t); });
    if (name == "Halftone") return adjust("Halftone", [cell = static_cast<int>(num("Size", 6)), a = static_cast<float>(num("Angle", 45)), ink = color_param(p.get("InkColor"), {0, 0, 0, 255}), paper = color_param(p.get("BackgroundColor"), {255, 255, 255, 255})](Image& i) { effects::halftone(i, cell, a, ink, paper); });
    if (name == "Chrome") return adjust("Chrome", [b = static_cast<int>(num("Flaws", 4)), br = static_cast<float>(num("Brightness", 100) / 100.0)](Image& i) { effects::chrome(i, b, br); });
    if (name == "AgedNewspaper") return adjust("Aged Newspaper", [a = static_cast<int>(num("Amount", 50))](Image& i) { effects::aged_newspaper(i, a); });
    if (name == "Blinds") return adjust("Blinds", [w = static_cast<int>(num("Width", 8)), o = static_cast<int>(num("Opacity", 60)), h = flag("Horizontal", false), l = flag("LightFromLeftTop", true), c = color_param(p.get("Color"), {0, 0, 0, 255})](Image& i) { effects::blinds(i, w, o, h, l, c); });
    if (name == "Weave") return adjust("Weave", [g = static_cast<int>(num("GapWidth", 3)), w = static_cast<int>(num("WeaveWidth", 4)), o = static_cast<int>(num("WeaveOpacity", 70)), gc = color_param(p.get("GapColor"), {0, 0, 0, 255}), wc = color_param(p.get("WeaveColor"), {255, 255, 255, 255}), f = flag("FillGaps", true)](Image& i) { effects::weave(i, g, w, o, gc, wc, f); });

    // --- text ---
    if (name == "TextEx") {
        if (auto e = need_doc(); !e.empty()) return e;
        ensure_fonts();
        std::string text = p.get("Characters").as_string();
        const Value& strings = p.get("Strings");
        if (text.empty() && strings.is_array()) for (size_t i = 0; i < strings.size(); ++i) { if (i) text += '\n'; text += strings[i].as_string(); }
        const std::string font_name = p.get("Font").as_string();
        int fi = font_index;
        for (size_t i = 0; i < fonts.size(); ++i) if (!font_name.empty() && fonts[i].family == font_name && (fonts[i].style == "Regular" || fi == font_index)) { fi = static_cast<int>(i); if (fonts[i].style == "Regular") break; }
        if (fonts.empty()) { *ok = false; return "TextEx: no fonts found"; }
        fi = std::clamp(fi, 0, static_cast<int>(fonts.size()) - 1);
        if (!text_font || text_font->info().path != fonts[static_cast<size_t>(fi)].path) { font_index = fi; text_font = text::Font::load(fonts[static_cast<size_t>(fi)].path); }
        if (!text_font) { *ok = false; return "TextEx: cannot load font"; }
        const Value& start = p.get("Start");
        const float x = static_cast<float>(start[0].as_number()), y = static_cast<float>(start[1].as_number());
        const float size = static_cast<float>(num("PointSize", 24) * 4.0 / 3.0);   // points to pixels at 96 dpi
        const std::string just = p.get("SetText").as_string("Left");
        const int align = just == "Center" ? 1 : just == "Right" ? 2 : 0;
        vec::TextInfo t;
        t.text = text; t.font_path = text_font->info().path; t.font_family = fonts[static_cast<size_t>(fi)].family + "  " + fonts[static_cast<size_t>(fi)].style;
        t.size = size; t.align = align; t.antialias = p.get("AntialiasStyle").as_string("Sharp") != "Off";
        vec::Object o;
        o.name = text.substr(0, text.find('\n')).substr(0, 32);
        place_text_object(o, t, x, y);
        // The original's Start is the baseline start; move the block up so the first baseline lands on it.
        o.translate(0, -o.text.baseline);
        o.fill = vec::PaintStyle{}; o.stroke = vec::PaintStyle{};
        if (const Value* fc = p.get("Fill").find("Color")) { o.fill.kind = vec::PaintStyle::Kind::Solid; o.fill.color = color_param(*fc, {0, 0, 0, 255}); }
        else { o.fill.kind = vec::PaintStyle::Kind::Solid; o.fill.color = background_fill(); }
        const float lw = static_cast<float>(num("LineWidth", 0));
        if (lw > 0 && p.get("Stroke").find("Color")) { o.stroke.kind = vec::PaintStyle::Kind::Solid; o.stroke.color = color_param(p.get("Stroke.Color"), {0, 0, 0, 255}); o.stroke_width = lw; }
        o.antialias = t.antialias;
        if (p.get("CreateAs").as_string("Vector") == "Vector") {
            add_vector_object(o, "Text");
        } else {
            Image px(doc->width(), doc->height(), {0, 0, 0, 0});
            vec::rasterize({o}, px);
            if (p.get("CreateAs").as_string() == "Selection") {
                Mask m(doc->width(), doc->height());
                for (size_t i = 0; i < m.size(); ++i) m.data()[i] = px.data()[i * 4 + 3];
                set_selection("Text Selection", std::move(m));
            } else run(std::make_unique<PasteLayerCommand>(o.name.empty() ? "Text" : o.name, std::move(px)));
        }
        return json::dump(result_ok());
    }

    // --- materials, colors ---
    if (name == "GetMaterial") {
        const float* src = flag("IsPrimary", true) ? fg_color : bg_color;
        Value r = result_ok();
        Value mat = Value::object();
        Value col = Value::array();
        for (int i = 0; i < 3; ++i) col.push(Value::number(std::lround(src[i] * 255)));
        mat.set("Color", col); mat.set("Pattern", Value::null()); mat.set("Gradient", Value::null()); mat.set("Texture", Value::null()); mat.set("Art", Value::null());
        r.set("CurrentMaterial", mat);
        return json::dump(r);
    }
    if (name == "SetMaterial") {
        const Value& m = p.get("Material");
        const Color c = color_param(m.get("Color"), {0, 0, 0, 255});
        float* dst = flag("IsPrimary", true) ? fg_color : bg_color;
        dst[0] = c.r / 255.0f; dst[1] = c.g / 255.0f; dst[2] = c.b / 255.0f;
        return json::dump(result_ok());
    }
    if (name == "Fill") {
        if (auto e = need_raster(); !e.empty()) return e;
        const Color c = color_param(p.get("Material.Color"), flag("UseForeground", true) ? Color{static_cast<uint8_t>(fg_color[0] * 255), static_cast<uint8_t>(fg_color[1] * 255), static_cast<uint8_t>(fg_color[2] * 255), 255} : background_fill());
        const Value& pt = p.get("Point");
        if (pt.is_array() && pt.size() >= 2 && (pt[0].as_number() != 0 || pt[1].as_number() != 0)) {
            const int tol = static_cast<int>(num("Tolerance", 20));
            const float op = static_cast<float>(num("Opacity", 100) / 100.0);
            const int x = static_cast<int>(pt[0].as_number()), y = static_cast<int>(pt[1].as_number());
            return adjust("Flood Fill", [=](Image& i) { raster::flood_fill(i, x, y, c, tol, op, nullptr); });
        }
        run(std::make_unique<FillCommand>(active_layer(), c));
        return json::dump(result_ok());
    }
    if (name == "DecreaseColorsTo2" || name == "DecreaseColorsTo16" || name == "DecreaseColorsTo256" || name == "DecreaseColorsToX") {
        const int colors = name == "DecreaseColorsTo2" ? 2 : name == "DecreaseColorsTo16" ? 16 : name == "DecreaseColorsTo256" ? 256 : static_cast<int>(num("NumberOfColors", 256));
        image_decrease_depth(colors, p.get("ReductionMethod").as_string("ErrorDiffusionDither") != "NearestColorMatch");
        return json::dump(result_ok());
    }
    if (name == "ColorInc16" || name == "ColorInc256" || name == "IncreaseColorsTo16Million") return json::dump(result_ok());
    // The next version's 16-bits-per-channel commands: 16 million colors
    // means 8 bits per channel there.
    if (name == "IncreaseColorsTo16Bit" || name == "DecreaseColorsTo16Million") {
        const int bits = name == "IncreaseColorsTo16Bit" ? 16 : 8;
        if (doc->bit_depth() != bits)
            run(std::make_unique<StateEditCommand>(bits == 16 ? "Increase to 16 Bits per Channel" : "Decrease to 8 Bits per Channel", [bits](Document& d) { d.set_bit_depth(bits); }));
        return json::dump(result_ok());
    }

    *ok = false;
    return "unsupported command " + name;
}
