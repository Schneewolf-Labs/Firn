#include "firn/adjustment.h"

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
        default: return "Adjustment";
    }
}

bool Adjustment::operator==(const Adjustment& o) const {
    if (kind != o.kind || brightness != o.brightness || contrast != o.contrast || levels != o.levels || curves != o.curves) return false;
    if (hue != o.hue || saturation != o.saturation || lightness != o.lightness || colorize != o.colorize || colorize_hue != o.colorize_hue || colorize_saturation != o.colorize_saturation) return false;
    if (hsl_ranges != o.hsl_ranges || threshold != o.threshold || posterize != o.posterize) return false;
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

void Adjustment::apply(Image& img) const {
    switch (kind) {
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

}  // namespace firn
