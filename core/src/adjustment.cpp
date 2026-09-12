#include "firn/adjustment.h"
#include "firn/effects.h"
#include "firn/raster.h"

#include <cmath>

#include <algorithm>
#include <cstring>

namespace firn {

const char* Adjustment::kind_name(Kind k) {
    switch (k) {
        case Kind::Levels: return "Levels";
        case Kind::Curves: return "Curves";
        case Kind::BrightnessContrast: return "Brightness/Contrast";
        case Kind::ColorBalance: return "Color Balance";
        case Kind::HSL: return "Hue/Saturation/Lightness";
        case Kind::ChannelMixer: return "Channel Mixer";
        case Kind::Invert: return "Invert";
        case Kind::Threshold: return "Threshold";
        case Kind::Posterize: return "Posterize";
        case Kind::GradientMap: return "Gradient Map";
        case Kind::GaussianBlur: return "Gaussian Blur";
        case Kind::Average: return "Average";
        case Kind::UnsharpMask: return "Unsharp Mask";
        default: return "Adjustment";
    }
}

bool Adjustment::operator==(const Adjustment& o) const {
    if (kind != o.kind || brightness != o.brightness || contrast != o.contrast || levels != o.levels || curves != o.curves) return false;
    if (hue != o.hue || saturation != o.saturation || lightness != o.lightness || colorize != o.colorize || colorize_hue != o.colorize_hue || colorize_saturation != o.colorize_saturation) return false;
    if (hsl_ranges != o.hsl_ranges || threshold != o.threshold || posterize != o.posterize) return false;
    if (blur_radius != o.blur_radius || average_radius != o.average_radius || unsharp_radius != o.unsharp_radius || unsharp_strength != o.unsharp_strength || unsharp_clipping != o.unsharp_clipping) return false;
    if (color_balance.preserve_luminosity != o.color_balance.preserve_luminosity) return false;
    for (int i = 0; i < 3; ++i)
        if (color_balance.shadows[i] != o.color_balance.shadows[i] || color_balance.midtones[i] != o.color_balance.midtones[i] || color_balance.highlights[i] != o.color_balance.highlights[i]) return false;
    if (mixer.monochrome != o.mixer.monochrome) return false;
    for (int c = 0; c < 3; ++c) {
        if (mixer.constant[c] != o.mixer.constant[c]) return false;
        for (int i = 0; i < 3; ++i) if (mixer.mix[c][i] != o.mixer.mix[c][i]) return false;
    }
    return true;
}

int Adjustment::reach() const {
    switch (kind) {
        case Kind::GaussianBlur: return static_cast<int>(std::ceil(blur_radius * 3.0f)) + 1;
        case Kind::Average: return average_radius + 1;
        case Kind::UnsharpMask: return static_cast<int>(std::ceil(unsharp_radius * 3.0f)) + 1;
        default: return 0;
    }
}

void Adjustment::apply(Image& img) const {
    switch (kind) {
        case Kind::GaussianBlur: raster::gaussian_blur(img, blur_radius); return;
        case Kind::Average: raster::box_blur(img, average_radius); return;
        case Kind::UnsharpMask: effects::unsharp_mask(img, unsharp_radius, unsharp_strength, unsharp_clipping); return;
        case Kind::GradientMap: {
            // Lightness picks a colour along the gradient; alpha is left
            // alone, so a mapped layer keeps its shape.
            Color table[256];
            for (int i = 0; i < 256; ++i) table[i] = gradient.at(i / 255.0f);
            uint8_t* p = img.data();
            for (size_t i = 0; i < img.size_bytes(); i += 4) {
                const int v = (p[i] * 77 + p[i + 1] * 151 + p[i + 2] * 28) >> 8;
                const Color& c = table[v];
                p[i] = c.r; p[i + 1] = c.g; p[i + 2] = c.b;
            }
            break;
        }
        case Kind::BrightnessContrast:
            adjust::apply_lut(img, adjust::brightness_contrast_lut(brightness, contrast));
            break;
        case Kind::Levels: {
            const Levels& m = levels[0];
            adjust::Lut master = adjust::levels_lut(m.in_low, m.gamma, m.in_high, m.out_low, m.out_high);
            adjust::Lut per[3];
            bool any_channel = false;
            for (int c = 0; c < 3; ++c) {
                const Levels& l = levels[c + 1];
                if (l != Levels{}) any_channel = true;
                adjust::Lut lut = adjust::levels_lut(l.in_low, l.gamma, l.in_high, l.out_low, l.out_high);
                for (int i = 0; i < 256; ++i) per[c][i] = lut[master[i]];
            }
            if (any_channel) adjust::apply_luts(img, per[0], per[1], per[2]);
            else adjust::apply_lut(img, master);
            break;
        }
        case Kind::Curves: {
            adjust::Lut master = adjust::curve_lut(curves[0]);
            adjust::Lut per[3];
            for (int c = 0; c < 3; ++c) {
                adjust::Lut lut = adjust::curve_lut(curves[c + 1]);
                for (int i = 0; i < 256; ++i) per[c][i] = lut[master[i]];
            }
            adjust::apply_luts(img, per[0], per[1], per[2]);
            break;
        }
        case Kind::ColorBalance: adjust::color_balance(img, color_balance); break;
        case Kind::HSL:
            if (colorize) adjust::colorize(img, colorize_hue, std::clamp(colorize_saturation, 0, 100) * 255 / 100);
            else adjust::hsl_adjust(img, hue, saturation, lightness);
            break;
        case Kind::ChannelMixer: adjust::channel_mixer(img, mixer); break;
        case Kind::Invert: {
            adjust::Lut lut;
            for (int i = 0; i < 256; ++i) lut[i] = static_cast<uint8_t>(255 - i);
            adjust::apply_lut(img, lut);
            break;
        }
        case Kind::Threshold: adjust::grayscale_then_threshold(img, threshold); break;
        case Kind::Posterize: adjust::apply_lut(img, adjust::posterize_lut(std::clamp(posterize, 2, 255))); break;
        default: break;
    }
}


// --- The whole structure as JSON, for the project format -------------------

namespace {
json::Value num_array(const int* v, size_t n) {
    json::Value a = json::Value::array();
    for (size_t i = 0; i < n; ++i) a.push(json::Value::number(v[i]));
    return a;
}
void read_num_array(const json::Value& v, int* out, size_t n) {
    for (size_t i = 0; i < n; ++i) out[i] = static_cast<int>(v[i].as_number(out[i]));
}
}  // namespace

json::Value Adjustment::to_json() const {
    json::Value v = json::Value::object();
    v.set("kind", json::Value::number(static_cast<int>(kind)));
    v.set("brightness", json::Value::number(brightness));
    v.set("contrast", json::Value::number(contrast));
    json::Value lv = json::Value::array();
    for (const Levels& l : levels) {
        json::Value e = json::Value::object();
        e.set("gamma", json::Value::number(l.gamma));
        e.set("in_low", json::Value::number(l.in_low));
        e.set("in_high", json::Value::number(l.in_high));
        e.set("out_low", json::Value::number(l.out_low));
        e.set("out_high", json::Value::number(l.out_high));
        lv.push(std::move(e));
    }
    v.set("levels", std::move(lv));
    json::Value cv = json::Value::array();
    for (const std::vector<std::pair<float, float>>& c : curves) {
        json::Value pts = json::Value::array();
        for (const std::pair<float, float>& pt : c) {
            json::Value p = json::Value::array();
            p.push(json::Value::number(pt.first));
            p.push(json::Value::number(pt.second));
            pts.push(std::move(p));
        }
        cv.push(std::move(pts));
    }
    v.set("curves", std::move(cv));
    v.set("hue", json::Value::number(hue));
    v.set("saturation", json::Value::number(saturation));
    v.set("lightness", json::Value::number(lightness));
    v.set("colorize", json::Value::boolean(colorize));
    v.set("colorize_hue", json::Value::number(colorize_hue));
    v.set("colorize_saturation", json::Value::number(colorize_saturation));
    json::Value hr = json::Value::array();
    for (const std::array<int32_t, 7>& r : hsl_ranges) {
        json::Value e = json::Value::array();
        for (int32_t x : r) e.push(json::Value::number(x));
        hr.push(std::move(e));
    }
    v.set("hsl_ranges", std::move(hr));
    json::Value cb = json::Value::object();
    cb.set("shadows", num_array(color_balance.shadows, 3));
    cb.set("midtones", num_array(color_balance.midtones, 3));
    cb.set("highlights", num_array(color_balance.highlights, 3));
    cb.set("preserve_luminosity", json::Value::boolean(color_balance.preserve_luminosity));
    v.set("color_balance", std::move(cb));
    json::Value mx = json::Value::object();
    json::Value rows = json::Value::array();
    for (int r = 0; r < 3; ++r) {
        json::Value row = json::Value::array();
        for (int c = 0; c < 3; ++c) row.push(json::Value::number(mixer.mix[r][c]));
        rows.push(std::move(row));
    }
    mx.set("mix", std::move(rows));
    json::Value cst = json::Value::array();
    for (float c : mixer.constant) cst.push(json::Value::number(c));
    mx.set("constant", std::move(cst));
    mx.set("monochrome", json::Value::boolean(mixer.monochrome));
    v.set("mixer", std::move(mx));
    v.set("gradient", vec::gradient_json(gradient));
    v.set("gradient_index", json::Value::number(gradient_index));
    v.set("threshold", json::Value::number(threshold));
    v.set("posterize", json::Value::number(posterize));
    v.set("blur_radius", json::Value::number(blur_radius));
    v.set("average_radius", json::Value::number(average_radius));
    v.set("unsharp_radius", json::Value::number(unsharp_radius));
    v.set("unsharp_strength", json::Value::number(unsharp_strength));
    v.set("unsharp_clipping", json::Value::number(unsharp_clipping));
    return v;
}

Adjustment Adjustment::from_json(const json::Value& v) {
    Adjustment a;
    auto n = [&](const char* k, double def) { return v.get(k).as_number(def); };
    a.kind = static_cast<Kind>(static_cast<int>(n("kind", static_cast<int>(a.kind))));
    a.brightness = static_cast<int>(n("brightness", a.brightness));
    a.contrast = static_cast<int>(n("contrast", a.contrast));
    const json::Value& lv = v.get("levels");
    for (size_t i = 0; i < a.levels.size(); ++i) {
        const json::Value& e = lv[i];
        if (!e.is_object()) continue;
        a.levels[i].gamma = static_cast<float>(e.get("gamma").as_number(a.levels[i].gamma));
        a.levels[i].in_low = static_cast<int>(e.get("in_low").as_number(a.levels[i].in_low));
        a.levels[i].in_high = static_cast<int>(e.get("in_high").as_number(a.levels[i].in_high));
        a.levels[i].out_low = static_cast<int>(e.get("out_low").as_number(a.levels[i].out_low));
        a.levels[i].out_high = static_cast<int>(e.get("out_high").as_number(a.levels[i].out_high));
    }
    const json::Value& cv = v.get("curves");
    for (size_t i = 0; i < a.curves.size(); ++i) {
        const json::Value& pts = cv[i];
        if (!pts.is_array() || pts.size() < 2) continue;
        std::vector<std::pair<float, float>> out;
        for (size_t k = 0; k < pts.size(); ++k)
            out.emplace_back(static_cast<float>(pts[k][0].as_number(0)), static_cast<float>(pts[k][1].as_number(0)));
        a.curves[i] = std::move(out);
    }
    a.hue = static_cast<int>(n("hue", a.hue));
    a.saturation = static_cast<int>(n("saturation", a.saturation));
    a.lightness = static_cast<int>(n("lightness", a.lightness));
    a.colorize = v.get("colorize").as_bool(a.colorize);
    a.colorize_hue = static_cast<int>(n("colorize_hue", a.colorize_hue));
    a.colorize_saturation = static_cast<int>(n("colorize_saturation", a.colorize_saturation));
    const json::Value& hr = v.get("hsl_ranges");
    for (size_t i = 0; i < a.hsl_ranges.size(); ++i)
        for (size_t k = 0; k < a.hsl_ranges[i].size(); ++k)
            a.hsl_ranges[i][k] = static_cast<int32_t>(hr[i][k].as_number(a.hsl_ranges[i][k]));
    const json::Value& cb = v.get("color_balance");
    read_num_array(cb.get("shadows"), a.color_balance.shadows, 3);
    read_num_array(cb.get("midtones"), a.color_balance.midtones, 3);
    read_num_array(cb.get("highlights"), a.color_balance.highlights, 3);
    a.color_balance.preserve_luminosity = cb.get("preserve_luminosity").as_bool(a.color_balance.preserve_luminosity);
    const json::Value& mx = v.get("mixer");
    const json::Value& rows = mx.get("mix");
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            a.mixer.mix[r][c] = static_cast<float>(rows[static_cast<size_t>(r)][static_cast<size_t>(c)].as_number(a.mixer.mix[r][c]));
    const json::Value& cst = mx.get("constant");
    for (int c = 0; c < 3; ++c) a.mixer.constant[c] = static_cast<float>(cst[static_cast<size_t>(c)].as_number(a.mixer.constant[c]));
    a.mixer.monochrome = mx.get("monochrome").as_bool(a.mixer.monochrome);
    if (v.get("gradient").is_object()) a.gradient = vec::gradient_from_json(v.get("gradient"));
    a.gradient_index = static_cast<int>(n("gradient_index", a.gradient_index));
    a.threshold = static_cast<int>(n("threshold", a.threshold));
    a.posterize = static_cast<int>(n("posterize", a.posterize));
    a.blur_radius = static_cast<float>(n("blur_radius", a.blur_radius));
    a.average_radius = static_cast<int>(n("average_radius", a.average_radius));
    a.unsharp_radius = static_cast<float>(n("unsharp_radius", a.unsharp_radius));
    a.unsharp_strength = static_cast<int>(n("unsharp_strength", a.unsharp_strength));
    a.unsharp_clipping = static_cast<int>(n("unsharp_clipping", a.unsharp_clipping));
    return a;
}

}  // namespace firn
