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
    const Value& path = p.get("Path");
    if (path.is_array() && path.size() >= 2) {
        const int rel = static_cast<int>(path[1].as_number());
        return std::clamp(app.active_layer() + rel, 0, static_cast<int>(app.doc->layer_count()) - 1);
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
        if (layer >= 0) { r.set("LayerName", Value::string(doc->layer(layer).name)); r.set("LayerType", Value::string(doc->layer(layer).is_vector() ? "Vector" : doc->layer(layer).is_adjustment() ? "Adjustment" : doc->layer(layer).type == LayerType::Group ? "Group" : "Raster")); }
        return json::dump(r);
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
        (flag("IsHorizontal", true) ? guides_h : guides_v).push_back(static_cast<float>(num("Position", 0)));
        return json::dump(result_ok());
    }
    if (name == "ShowGuides") { show_guides = !show_guides; return json::dump(result_ok()); }

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
        const raster::Filter f = rt == "Pixel" ? raster::Filter::Nearest : rt == "Bilinear" ? raster::Filter::Bilinear : raster::Filter::Bicubic;
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
    if (name == "AutoSaturationEnhancement") return adjust("Automatic Saturation Enhancement", [](Image& i) { photo::auto_saturation(i, 1, 1, true); });
    if (name == "AutoColorBalance") return adjust("Automatic Color Balance", [s = static_cast<int>(num("Strength", 30)), t = static_cast<int>(num("Temperature", 6500))](Image& i) { photo::auto_color_balance(i, s, t); });
    if (name == "HistogramEqualize") return adjust("Histogram Equalize", adjust::histogram_equalize);
    if (name == "HistogramStretch") return adjust("Histogram Stretch", adjust::histogram_stretch);

    // --- effects ---
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
        if (new_layer && active_is_raster()) { layer_new(); }
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

    // --- materials, colors ---
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
    if (name == "ColorInc16" || name == "ColorInc256" || name == "IncreaseColorsTo16Million" || name == "DecreaseColorsTo16Million") return json::dump(result_ok());

    *ok = false;
    return "unsupported command " + name;
}
