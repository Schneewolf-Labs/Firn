#include "firn/blend.h"

#include "firn/json.h"

#include <algorithm>
#include <cmath>

namespace firn {

namespace {
json::Value range_json(const BlendRange& r) {
    json::Value a = json::Value::array();
    for (uint8_t v : {r.low0, r.low1, r.high1, r.high0}) a.push(json::Value::number(v));
    return a;
}
BlendRange range_from(const json::Value& v, const BlendRange& def) {
    if (!v.is_array() || v.size() < 4) return def;
    BlendRange r;
    r.low0 = static_cast<uint8_t>(std::clamp(static_cast<int>(v[0].as_number(0)), 0, 255));
    r.low1 = static_cast<uint8_t>(std::clamp(static_cast<int>(v[1].as_number(0)), 0, 255));
    r.high1 = static_cast<uint8_t>(std::clamp(static_cast<int>(v[2].as_number(255)), 0, 255));
    r.high0 = static_cast<uint8_t>(std::clamp(static_cast<int>(v[3].as_number(255)), 0, 255));
    return r;
}
}  // namespace

json::Value blend_ranges_json(const BlendRanges& b) {
    json::Value v = json::Value::object();
    v.set("channel", json::Value::number(static_cast<int>(b.channel)));
    v.set("source", range_json(b.source));
    v.set("under", range_json(b.under));
    return v;
}

BlendRanges blend_ranges_from_json(const json::Value& v) {
    BlendRanges b;
    b.channel = static_cast<BlendRanges::Channel>(std::clamp(static_cast<int>(v.get("channel").as_number(0)), 0, 3));
    b.source = range_from(v.get("source"), b.source);
    b.under = range_from(v.get("under"), b.under);
    return b;
}

const char* blend_channel_name(BlendRanges::Channel c) {
    switch (c) {
        case BlendRanges::Channel::Red: return "Red";
        case BlendRanges::Channel::Green: return "Green";
        case BlendRanges::Channel::Blue: return "Blue";
        default: break;
    }
    return "Gray";
}

const char* blend_mode_name(BlendMode m) {
    static const char* names[] = {"Normal", "Darken", "Lighten", "Hue", "Saturation", "Color", "Luminance",
                                  "Multiply", "Screen", "Dissolve", "Overlay", "Hard Light", "Soft Light",
                                  "Difference", "Dodge", "Burn", "Exclusion"};
    const auto i = static_cast<size_t>(m);
    return i < static_cast<size_t>(BlendMode::Count) ? names[i] : "?";
}

namespace blend {
namespace {

struct RGB { float r, g, b; };

float sep(BlendMode m, float cb, float cs) {
    switch (m) {
        case BlendMode::Darken:     return std::min(cb, cs);
        case BlendMode::Lighten:    return std::max(cb, cs);
        case BlendMode::Multiply:   return cb * cs;
        case BlendMode::Screen:     return cb + cs - cb * cs;
        case BlendMode::Overlay:    return sep(BlendMode::HardLight, cs, cb);
        case BlendMode::HardLight:  return cs <= 0.5f ? cb * 2.0f * cs : sep(BlendMode::Screen, cb, 2.0f * cs - 1.0f);
        case BlendMode::SoftLight: {
            if (cs <= 0.5f) return cb - (1.0f - 2.0f * cs) * cb * (1.0f - cb);
            const float d = cb <= 0.25f ? ((16.0f * cb - 12.0f) * cb + 4.0f) * cb : std::sqrt(cb);
            return cb + (2.0f * cs - 1.0f) * (d - cb);
        }
        case BlendMode::Difference: return std::abs(cb - cs);
        case BlendMode::Exclusion:  return cb + cs - 2.0f * cb * cs;
        case BlendMode::Dodge:      return cb <= 0.0f ? 0.0f : cs >= 1.0f ? 1.0f : std::min(1.0f, cb / (1.0f - cs));
        case BlendMode::Burn:       return cb >= 1.0f ? 1.0f : cs <= 0.0f ? 0.0f : 1.0f - std::min(1.0f, (1.0f - cb) / cs);
        default:                    return cs;  // Normal
    }
}

// Non-separable modes, per the PDF/W3C compositing spec.
float lum(RGB c) { return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b; }

RGB clip_color(RGB c) {
    const float l = lum(c);
    const float n = std::min({c.r, c.g, c.b}), x = std::max({c.r, c.g, c.b});
    if (n < 0.0f) { c.r = l + (c.r - l) * l / (l - n); c.g = l + (c.g - l) * l / (l - n); c.b = l + (c.b - l) * l / (l - n); }
    if (x > 1.0f) { c.r = l + (c.r - l) * (1 - l) / (x - l); c.g = l + (c.g - l) * (1 - l) / (x - l); c.b = l + (c.b - l) * (1 - l) / (x - l); }
    return c;
}

RGB set_lum(RGB c, float l) {
    const float d = l - lum(c);
    return clip_color({c.r + d, c.g + d, c.b + d});
}

float sat(RGB c) { return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}); }

RGB set_sat(RGB c, float s) {
    float* ch[3] = {&c.r, &c.g, &c.b};
    std::sort(ch, ch + 3, [](float* a, float* b) { return *a < *b; });
    float& mn = *ch[0]; float& md = *ch[1]; float& mx = *ch[2];
    if (mx > mn) { md = (md - mn) * s / (mx - mn); mx = s; }
    else md = mx = 0.0f;
    mn = 0.0f;
    return c;
}

RGB nonsep(BlendMode m, RGB cb, RGB cs) {
    switch (m) {
        case BlendMode::Hue:        return set_lum(set_sat(cs, sat(cb)), lum(cb));
        case BlendMode::Saturation: return set_lum(set_sat(cb, sat(cs)), lum(cb));
        case BlendMode::Color:      return set_lum(cs, lum(cb));
        case BlendMode::Luminance:  return set_lum(cb, lum(cs));
        default:                    return cs;
    }
}

}  // namespace

void pixel(uint8_t* d, const uint8_t* s, float opacity, BlendMode mode, uint32_t hash) {
    float as = (s[3] / 255.0f) * opacity;
    if (as <= 0.0f) return;

    if (mode == BlendMode::Dissolve) {
        // The source shows fully opaque in a random subset of pixels whose
        // density equals its alpha; elsewhere it is dropped entirely.
        if ((hash & 0xFFFF) / 65535.0f >= as) return;
        d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
        return;
    }

    const float ad = d[3] / 255.0f;
    const float ao = as + ad * (1.0f - as);
    if (ao <= 0.0f) return;

    const RGB cb{d[0] / 255.0f, d[1] / 255.0f, d[2] / 255.0f};
    const RGB cs{s[0] / 255.0f, s[1] / 255.0f, s[2] / 255.0f};
    RGB b;
    if (mode == BlendMode::Hue || mode == BlendMode::Saturation || mode == BlendMode::Color || mode == BlendMode::Luminance)
        b = nonsep(mode, cb, cs);
    else
        b = {sep(mode, cb.r, cs.r), sep(mode, cb.g, cs.g), sep(mode, cb.b, cs.b)};

    // Co = (1 - as) * ad * Cb + (1 - ad) * as * Cs + as * ad * B(Cb, Cs), then un-premultiply.
    const float* pb = &cb.r; const float* ps = &cs.r; const float* pm = &b.r;
    for (int i = 0; i < 3; ++i) {
        const float co = ((1.0f - as) * ad * pb[i] + (1.0f - ad) * as * ps[i] + as * ad * pm[i]) / ao;
        d[i] = static_cast<uint8_t>(std::clamp(co, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    d[3] = static_cast<uint8_t>(ao * 255.0f + 0.5f);
}

}  // namespace blend
}  // namespace firn
