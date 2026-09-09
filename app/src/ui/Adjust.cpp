// Adjustment and effect dialogs with live preview. Each dialog edits its
// parameters, the preview session re-applies the operation to the active
// layer (clipped to the selection), and OK records one history entry.
#include <algorithm>
#include <cmath>

#include "App.h"
#include "firn/adjust.h"
#include "firn/effects.h"
#include "firn/raster.h"
#include "imgui.h"

using namespace firn;

// --- Preview session -------------------------------------------------------

void App::preview_begin(const char* name) {
    if (!doc || active_layer() < 0) return;
    preview.active = true;
    preview.layer = active_layer();
    preview.name = name;
    preview.before = doc->layer(preview.layer).pixels;
    preview.histogram = adjust::histogram_luma(preview.before);
    preview.dirty = true;
    preview.live = static_cast<size_t>(preview.before.width()) * preview.before.height() <= 1024 * 1024;
}

void App::preview_update(const std::function<void(Image&)>& op, bool force) {
    if (!preview.active || !doc || !preview.dirty) return;
    // Large layers only re-render once the slider is released.
    if (!force && !preview.live && ImGui::IsAnyItemActive()) return;
    Image work = preview.before;
    op(work);
    raster::apply_through_mask(work, preview.before, doc->selection());
    doc->layer(preview.layer).pixels = std::move(work);
    doc->touch();
    preview.dirty = false;
}

void App::preview_commit() {
    if (!preview.active || !doc) return;
    commit(std::make_unique<LayerSnapshotCommand>(preview.layer, preview.name, preview.before, doc->layer(preview.layer).pixels));
    preview = Preview{};
}

void App::preview_cancel() {
    if (preview.active && doc && preview.layer < doc->layer_count()) {
        doc->layer(preview.layer).pixels = preview.before;
        doc->touch();
    }
    preview = Preview{};
}

namespace {

// One modal dialog: `body` draws the controls and returns true when a
// parameter changed; `op` applies the current parameters to an image.
template <class Body, class Op>
void adjust_modal(App& app, const char* title, Body body, Op op) {
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (ImGui::IsWindowAppearing()) app.preview_begin(title);
    if (body()) app.preview.dirty = true;
    app.preview_update(op);
    ImGui::Separator();
    if (!app.preview.live) ImGui::TextDisabled("Large image: preview updates when a slider is released.");
    const bool ok = ImGui::Button("OK", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (ok) {
        app.preview_update(op, true);
        app.preview_commit();
        ImGui::CloseCurrentPopup();
    } else if (cancel) {
        app.preview_cancel();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void draw_histogram(const std::array<int, 256>& h, ImVec2 size) {
    int peak = 1;
    for (int v : h) peak = std::max(peak, v);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(30, 30, 30, 255));
    for (int i = 0; i < 256; ++i) {
        const float x0 = p0.x + size.x * i / 256.0f, x1 = p0.x + size.x * (i + 1) / 256.0f;
        const float hh = size.y * std::sqrt(static_cast<float>(h[i]) / peak);  // sqrt so small counts stay visible
        dl->AddRectFilled(ImVec2(x0, p0.y + size.y - hh), ImVec2(x1, p0.y + size.y), IM_COL32(180, 180, 180, 255));
    }
    ImGui::Dummy(size);
}

// Editable curve: click to add a point, drag to move, right-click to remove.
bool curve_editor(App& app, const ImVec2 size) {
    bool changed = false;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("curve", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    auto to_screen = [&](float x, float y) { return ImVec2(p0.x + x / 255.0f * size.x, p0.y + size.y - y / 255.0f * size.y); };
    auto from_screen = [&](ImVec2 s) {
        return std::pair<float, float>{std::clamp((s.x - p0.x) / size.x * 255.0f, 0.0f, 255.0f),
                                       std::clamp((p0.y + size.y - s.y) / size.y * 255.0f, 0.0f, 255.0f)};
    };
    auto& pts = app.curve_points;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    auto nearest = [&]() {
        int best = -1;
        float bd = 10.0f;
        for (size_t i = 0; i < pts.size(); ++i) {
            const ImVec2 s = to_screen(pts[i].first, pts[i].second);
            const float d = std::hypot(s.x - mouse.x, s.y - mouse.y);
            if (d < bd) { bd = d; best = static_cast<int>(i); }
        }
        return best;
    };
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        app.curve_drag = nearest();
        if (app.curve_drag < 0) {
            pts.push_back(from_screen(mouse));
            std::sort(pts.begin(), pts.end());
            app.curve_drag = static_cast<int>(std::find(pts.begin(), pts.end(), pts.back()) - pts.begin());
            for (size_t i = 0; i < pts.size(); ++i) if (pts[i] == from_screen(mouse)) app.curve_drag = static_cast<int>(i);
            changed = true;
        }
    }
    if (app.curve_drag >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        auto p = from_screen(mouse);
        // Keep endpoints on the edges and points ordered.
        if (app.curve_drag == 0) p.first = 0;
        else if (app.curve_drag == static_cast<int>(pts.size()) - 1) p.first = 255;
        else p.first = std::clamp(p.first, pts[app.curve_drag - 1].first + 1, pts[app.curve_drag + 1].first - 1);
        if (pts[app.curve_drag] != p) { pts[app.curve_drag] = p; changed = true; }
    } else {
        app.curve_drag = -1;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        const int n = nearest();
        if (n > 0 && n < static_cast<int>(pts.size()) - 1) { pts.erase(pts.begin() + n); changed = true; }
    }
    // Draw: grid, curve, points.
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(30, 30, 30, 255));
    for (int i = 1; i < 4; ++i) {
        dl->AddLine(ImVec2(p0.x + size.x * i / 4, p0.y), ImVec2(p0.x + size.x * i / 4, p0.y + size.y), IM_COL32(60, 60, 60, 255));
        dl->AddLine(ImVec2(p0.x, p0.y + size.y * i / 4), ImVec2(p0.x + size.x, p0.y + size.y * i / 4), IM_COL32(60, 60, 60, 255));
    }
    const adjust::Lut lut = adjust::curve_lut(pts);
    for (int x = 1; x < 256; ++x) dl->AddLine(to_screen(x - 1.0f, lut[x - 1]), to_screen(static_cast<float>(x), lut[x]), IM_COL32(230, 230, 230, 255), 1.5f);
    for (size_t i = 0; i < pts.size(); ++i)
        dl->AddCircleFilled(to_screen(pts[i].first, pts[i].second), 4.0f, static_cast<int>(i) == app.curve_drag ? IM_COL32(255, 200, 0, 255) : IM_COL32(120, 180, 255, 255));
    return changed;
}

}  // namespace

void App::draw_adjust_dialogs() {
    static const char* kTitles[] = {nullptr, "Brightness/Contrast", "Curves", "Gamma Correction", "Levels", "Threshold",
                                    "Channel Mixer", "Colorize", "Hue/Saturation/Lightness", "Average", "Gaussian Blur",
                                    "Posterize", "Solarize", "Unsharp Mask", "Median", "Motion Blur", "Mosaic",
                                    "Add Noise", "Drop Shadow", "Color Balance", "Sepia Toning", "Hue Map", "Wave",
                                    "Pinch", "Twirl", "Buttonize", "Inner Bevel", "Cutout", "Ripple", "Spherize",
                                    "Lens Distortion", "Halftone", "Chrome", "Outer Bevel"};
    if (open_adjust != Adj::None) {
        if (doc && active_layer() >= 0) ImGui::OpenPopup(kTitles[static_cast<int>(open_adjust)]);
        open_adjust = Adj::None;
    }

    adjust_modal(*this, "Brightness/Contrast",
        [&] { bool c = ImGui::SliderInt("Brightness", &bc_brightness, -255, 255); c |= ImGui::SliderInt("Contrast", &bc_contrast, -100, 100); return c; },
        [&](Image& img) { adjust::apply_lut(img, adjust::brightness_contrast_lut(bc_brightness, bc_contrast)); });

    adjust_modal(*this, "Curves",
        [&] {
            bool c = curve_editor(*this, ImVec2(256, 256));
            ImGui::TextDisabled("Click to add a point, drag to move, right-click to remove.");
            if (ImGui::SmallButton("Reset")) { curve_points = {{0, 0}, {255, 255}}; c = true; }
            return c;
        },
        [&](Image& img) { adjust::apply_lut(img, adjust::curve_lut(curve_points)); });

    adjust_modal(*this, "Gamma Correction",
        [&] { return ImGui::SliderFloat("Gamma", &gamma_value, 0.1f, 5.0f, "%.2f", ImGuiSliderFlags_Logarithmic); },
        [&](Image& img) { adjust::apply_lut(img, adjust::gamma_lut(gamma_value)); });

    adjust_modal(*this, "Levels",
        [&] {
            draw_histogram(preview.histogram, ImVec2(256, 80));
            bool c = false;
            ImGui::TextUnformatted("Input levels");
            c |= ImGui::SliderInt("Low##in", &lv_in_lo, 0, 254);
            c |= ImGui::SliderFloat("Gamma", &lv_gamma, 0.1f, 5.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            c |= ImGui::SliderInt("High##in", &lv_in_hi, 1, 255);
            if (lv_in_hi <= lv_in_lo) lv_in_hi = lv_in_lo + 1;
            ImGui::TextUnformatted("Output levels");
            c |= ImGui::SliderInt("Low##out", &lv_out_lo, 0, 255);
            c |= ImGui::SliderInt("High##out", &lv_out_hi, 0, 255);
            if (ImGui::SmallButton("Reset")) { lv_in_lo = 0; lv_in_hi = 255; lv_gamma = 1; lv_out_lo = 0; lv_out_hi = 255; c = true; }
            return c;
        },
        [&](Image& img) { adjust::apply_lut(img, adjust::levels_lut(lv_in_lo, lv_gamma, lv_in_hi, lv_out_lo, lv_out_hi)); });

    adjust_modal(*this, "Threshold",
        [&] { draw_histogram(preview.histogram, ImVec2(256, 60)); return ImGui::SliderInt("Threshold", &threshold_value, 1, 255); },
        [&](Image& img) { adjust::grayscale_then_threshold(img, threshold_value); });

    adjust_modal(*this, "Channel Mixer",
        [&] {
            bool c = ImGui::Checkbox("Monochrome", &mixer.monochrome);
            if (!mixer.monochrome) { ImGui::SameLine(); ImGui::SetNextItemWidth(100); ImGui::Combo("Output channel", &mixer_row, "Red\0Green\0Blue\0"); }
            const int row = mixer.monochrome ? 0 : mixer_row;
            c |= ImGui::SliderFloat("Red", &mixer.mix[row][0], -200.0f, 200.0f, "%.0f%%");
            c |= ImGui::SliderFloat("Green", &mixer.mix[row][1], -200.0f, 200.0f, "%.0f%%");
            c |= ImGui::SliderFloat("Blue", &mixer.mix[row][2], -200.0f, 200.0f, "%.0f%%");
            c |= ImGui::SliderFloat("Constant", &mixer.constant[row], -200.0f, 200.0f, "%.0f%%");
            if (ImGui::SmallButton("Reset")) { mixer = adjust::ChannelMix{}; c = true; }
            return c;
        },
        [&](Image& img) { adjust::channel_mixer(img, mixer); });

    adjust_modal(*this, "Colorize",
        [&] {
            bool c = ImGui::SliderInt("Hue", &colorize_hue, 0, 359);
            c |= ImGui::SliderInt("Saturation", &colorize_sat, 0, 255);
            return c;
        },
        [&](Image& img) { adjust::colorize(img, colorize_hue, colorize_sat); });

    adjust_modal(*this, "Hue/Saturation/Lightness",
        [&] {
            bool c = ImGui::SliderInt("Hue", &hsl_h, -180, 180);
            c |= ImGui::SliderInt("Saturation", &hsl_s, -100, 100);
            c |= ImGui::SliderInt("Lightness", &hsl_l, -100, 100);
            return c;
        },
        [&](Image& img) { adjust::hsl_adjust(img, hsl_h, hsl_s, hsl_l); });

    adjust_modal(*this, "Average",
        [&] { return ImGui::SliderInt("Radius", &box_radius, 1, 50); },
        [&](Image& img) { raster::box_blur(img, box_radius); });

    adjust_modal(*this, "Gaussian Blur",
        [&] { return ImGui::SliderFloat("Radius", &blur_radius, 0.1f, 100.0f, "%.1f", ImGuiSliderFlags_Logarithmic); },
        [&](Image& img) { raster::gaussian_blur(img, blur_radius); });

    adjust_modal(*this, "Posterize",
        [&] { return ImGui::SliderInt("Levels", &posterize_levels, 2, 255, "%d", ImGuiSliderFlags_Logarithmic); },
        [&](Image& img) { adjust::apply_lut(img, adjust::posterize_lut(posterize_levels)); });

    adjust_modal(*this, "Solarize",
        [&] { return ImGui::SliderInt("Threshold", &solarize_threshold, 1, 254); },
        [&](Image& img) { adjust::apply_lut(img, adjust::solarize_lut(solarize_threshold)); });

    adjust_modal(*this, "Unsharp Mask",
        [&] {
            bool c = ImGui::SliderFloat("Radius", &usm_radius, 0.1f, 50.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
            c |= ImGui::SliderInt("Strength", &usm_strength, 1, 500, "%d%%");
            c |= ImGui::SliderInt("Clipping", &usm_clipping, 0, 100);
            return c;
        },
        [&](Image& img) { effects::unsharp_mask(img, usm_radius, usm_strength, usm_clipping); });

    adjust_modal(*this, "Median",
        [&] { return ImGui::SliderInt("Filter aperture", &median_radius, 1, 10, "%d px radius"); },
        [&](Image& img) { effects::median(img, median_radius); });

    adjust_modal(*this, "Motion Blur",
        [&] {
            bool c = ImGui::SliderFloat("Angle", &motion_angle, 0.0f, 359.0f, "%.0f");
            c |= ImGui::SliderInt("Strength", &motion_strength, 1, 100, "%d px");
            return c;
        },
        [&](Image& img) { effects::motion_blur(img, motion_angle, motion_strength); });

    adjust_modal(*this, "Mosaic",
        [&] {
            bool c = ImGui::SliderInt("Block width", &mosaic_w, 1, 100);
            if (mosaic_square) mosaic_h = mosaic_w;
            else c |= ImGui::SliderInt("Block height", &mosaic_h, 1, 100);
            if (ImGui::Checkbox("Symmetric", &mosaic_square)) { mosaic_h = mosaic_w; c = true; }
            return c;
        },
        [&](Image& img) { effects::mosaic(img, mosaic_w, mosaic_h); });

    adjust_modal(*this, "Add Noise",
        [&] {
            bool c = ImGui::SliderInt("Noise", &noise_percent, 0, 100, "%d%%");
            c |= ImGui::Checkbox("Gaussian", &noise_gaussian);
            ImGui::SameLine();
            c |= ImGui::Checkbox("Monochrome", &noise_mono);
            return c;
        },
        [&](Image& img) { effects::add_noise(img, noise_percent, noise_gaussian, noise_mono, 12345); });

    adjust_modal(*this, "Drop Shadow",
        [&] {
            bool c = ImGui::SliderInt("Vertical offset", &shadow_y, -100, 100);
            c |= ImGui::SliderInt("Horizontal offset", &shadow_x, -100, 100);
            float op = shadow_opacity * 100.0f;
            if (ImGui::SliderFloat("Opacity", &op, 0.0f, 100.0f, "%.0f%%")) { shadow_opacity = op / 100.0f; c = true; }
            c |= ImGui::SliderFloat("Blur", &shadow_blur, 0.0f, 100.0f, "%.1f");
            c |= ImGui::ColorEdit3("Color", shadow_color, ImGuiColorEditFlags_NoInputs);
            if (doc && doc->layer(active_layer()).background) ImGui::TextDisabled("On a Background layer only the selection casts a shadow.");
            return c;
        },
        [&](Image& img) {
            auto c8 = [](float f) { return static_cast<uint8_t>(f * 255.0f + 0.5f); };
            effects::drop_shadow(img, shadow_x, shadow_y, shadow_opacity, shadow_blur, {c8(shadow_color[0]), c8(shadow_color[1]), c8(shadow_color[2]), 255});
        });

    adjust_modal(*this, "Color Balance",
        [&] {
            bool c = false;
            ImGui::RadioButton("Shadows", &cb_range, 0); ImGui::SameLine();
            ImGui::RadioButton("Midtones", &cb_range, 1); ImGui::SameLine();
            ImGui::RadioButton("Highlights", &cb_range, 2);
            int* v = cb_range == 0 ? color_balance.shadows : cb_range == 1 ? color_balance.midtones : color_balance.highlights;
            c |= ImGui::SliderInt("Cyan - Red", &v[0], -100, 100);
            c |= ImGui::SliderInt("Magenta - Green", &v[1], -100, 100);
            c |= ImGui::SliderInt("Yellow - Blue", &v[2], -100, 100);
            c |= ImGui::Checkbox("Preserve luminosity", &color_balance.preserve_luminosity);
            if (ImGui::SmallButton("Reset")) { color_balance = adjust::ColorBalance{}; c = true; }
            return c;
        },
        [&](Image& img) { adjust::color_balance(img, color_balance); });

    adjust_modal(*this, "Sepia Toning",
        [&] { return ImGui::SliderInt("Amount to age", &sepia_amount, 1, 100); },
        [&](Image& img) { adjust::sepia(img, sepia_amount); });

    adjust_modal(*this, "Hue Map",
        [&] {
            bool c = false;
            static const char* bands[10] = {"Red", "Orange", "Yellow", "Chartreuse", "Green", "Spring", "Cyan", "Azure", "Blue", "Magenta"};
            for (int i = 0; i < 10; ++i) {
                ImGui::PushID(i);
                ImGui::SetNextItemWidth(220);
                c |= ImGui::SliderInt(bands[i], &hue_map_params.shift[i], -180, 180);
                ImGui::PopID();
            }
            ImGui::Separator();
            c |= ImGui::SliderInt("Saturation shift", &hue_map_params.saturation, -100, 100);
            c |= ImGui::SliderInt("Lightness shift", &hue_map_params.lightness, -100, 100);
            if (ImGui::SmallButton("Reset")) { hue_map_params = adjust::HueMap{}; c = true; }
            return c;
        },
        [&](Image& img) { adjust::hue_map(img, hue_map_params); });

    adjust_modal(*this, "Wave",
        [&] {
            bool c = ImGui::SliderFloat("Horizontal amplitude", &wave_ha, 0.0f, 100.0f, "%.0f");
            c |= ImGui::SliderFloat("Horizontal wavelength", &wave_hw, 1.0f, 500.0f, "%.0f");
            c |= ImGui::SliderFloat("Vertical amplitude", &wave_va, 0.0f, 100.0f, "%.0f");
            c |= ImGui::SliderFloat("Vertical wavelength", &wave_vw, 1.0f, 500.0f, "%.0f");
            return c;
        },
        [&](Image& img) { effects::wave(img, wave_ha, wave_hw, wave_va, wave_vw); });

    adjust_modal(*this, "Pinch",
        [&] { return ImGui::SliderInt("Strength (negative = punch)", &pinch_strength, -100, 100); },
        [&](Image& img) { effects::pinch(img, pinch_strength); });

    adjust_modal(*this, "Twirl",
        [&] { return ImGui::SliderFloat("Degrees", &twirl_degrees, -720.0f, 720.0f, "%.0f"); },
        [&](Image& img) { effects::twirl(img, twirl_degrees); });

    adjust_modal(*this, "Buttonize",
        [&] {
            bool c = ImGui::SliderInt("Edge width", &button_width, 1, 200);
            float op = button_opacity * 100.0f;
            if (ImGui::SliderFloat("Opacity", &op, 0.0f, 100.0f, "%.0f%%")) { button_opacity = op / 100.0f; c = true; }
            c |= ImGui::Checkbox("Transparent edge", &button_transparent);
            if (!button_transparent) { ImGui::SameLine(); c |= ImGui::ColorEdit3("Color", button_color, ImGuiColorEditFlags_NoInputs); }
            return c;
        },
        [&](Image& img) {
            auto c8 = [](float f) { return static_cast<uint8_t>(f * 255.0f + 0.5f); };
            effects::buttonize(img, button_width, button_opacity, {c8(button_color[0]), c8(button_color[1]), c8(button_color[2]), 255}, button_transparent);
        });

    adjust_modal(*this, "Inner Bevel",
        [&] {
            bool c = ImGui::SliderInt("Width", &bevel_width, 1, 100);
            c |= ImGui::SliderFloat("Light angle", &bevel_angle, 0.0f, 359.0f, "%.0f");
            c |= ImGui::SliderFloat("Depth", &bevel_depth, 0.1f, 3.0f, "%.1f");
            c |= ImGui::SliderFloat("Ambience", &bevel_ambient, 0.3f, 1.5f, "%.2f");
            ImGui::TextDisabled(doc && doc->has_selection() ? "Applies inside the selection." : "Applies inside the layer's opaque area.");
            return c;
        },
        [&](Image& img) {
            effects::inner_bevel(img, doc && doc->has_selection() ? doc->selection().data() : nullptr, bevel_width, bevel_angle, bevel_depth, bevel_ambient);
        });

    adjust_modal(*this, "Cutout",
        [&] {
            bool c = ImGui::SliderInt("Vertical offset", &cutout_y, -100, 100);
            c |= ImGui::SliderInt("Horizontal offset", &cutout_x, -100, 100);
            float op = cutout_opacity * 100.0f;
            if (ImGui::SliderFloat("Opacity", &op, 0.0f, 100.0f, "%.0f%%")) { cutout_opacity = op / 100.0f; c = true; }
            c |= ImGui::SliderFloat("Blur", &cutout_blur, 0.0f, 100.0f, "%.1f");
            c |= ImGui::ColorEdit3("Shadow color", cutout_color, ImGuiColorEditFlags_NoInputs);
            ImGui::TextDisabled(doc && doc->has_selection() ? "Cut into the selection." : "Cut into the layer's opaque area.");
            return c;
        },
        [&](Image& img) {
            auto c8 = [](float f) { return static_cast<uint8_t>(f * 255.0f + 0.5f); };
            effects::cutout(img, doc && doc->has_selection() ? doc->selection().data() : nullptr, cutout_x, cutout_y, cutout_opacity, cutout_blur,
                            {c8(cutout_color[0]), c8(cutout_color[1]), c8(cutout_color[2]), 255});
        });

    adjust_modal(*this, "Ripple",
        [&] {
            bool c = ImGui::SliderFloat("Amplitude", &ripple_amp, 0.0f, 100.0f, "%.0f");
            c |= ImGui::SliderFloat("Wavelength", &ripple_wave, 2.0f, 300.0f, "%.0f");
            return c;
        },
        [&](Image& img) { effects::ripple(img, ripple_amp, ripple_wave); });

    adjust_modal(*this, "Spherize",
        [&] { return ImGui::SliderInt("Strength (negative = dish)", &spherize_strength, -100, 100); },
        [&](Image& img) { effects::spherize(img, spherize_strength); });

    adjust_modal(*this, "Lens Distortion",
        [&] { return ImGui::SliderInt("Strength (negative = pincushion)", &lens_strength, -100, 100); },
        [&](Image& img) { effects::lens_distortion(img, lens_strength); });

    adjust_modal(*this, "Halftone",
        [&] {
            bool c = ImGui::SliderInt("Cell size", &halftone_cell, 2, 50);
            c |= ImGui::SliderFloat("Angle", &halftone_angle, 0.0f, 90.0f, "%.0f");
            c |= ImGui::ColorEdit3("Ink", halftone_ink, ImGuiColorEditFlags_NoInputs);
            ImGui::SameLine();
            c |= ImGui::ColorEdit3("Paper", halftone_paper, ImGuiColorEditFlags_NoInputs);
            return c;
        },
        [&](Image& img) {
            auto c8 = [](float f) { return static_cast<uint8_t>(f * 255.0f + 0.5f); };
            effects::halftone(img, halftone_cell, halftone_angle, {c8(halftone_ink[0]), c8(halftone_ink[1]), c8(halftone_ink[2]), 255},
                              {c8(halftone_paper[0]), c8(halftone_paper[1]), c8(halftone_paper[2]), 255});
        });

    adjust_modal(*this, "Chrome",
        [&] {
            bool c = ImGui::SliderInt("Flaws (bands)", &chrome_bands, 1, 20);
            c |= ImGui::SliderFloat("Brightness", &chrome_brightness, 0.2f, 2.0f, "%.2f");
            return c;
        },
        [&](Image& img) { effects::chrome(img, chrome_bands, chrome_brightness); });

    adjust_modal(*this, "Outer Bevel",
        [&] {
            bool c = ImGui::SliderInt("Width", &obevel_width, 1, 100);
            c |= ImGui::SliderFloat("Light angle", &obevel_angle, 0.0f, 359.0f, "%.0f");
            c |= ImGui::SliderFloat("Depth", &obevel_depth, 0.1f, 3.0f, "%.1f");
            c |= ImGui::ColorEdit3("Color", obevel_color, ImGuiColorEditFlags_NoInputs);
            ImGui::TextDisabled(doc && doc->has_selection() ? "Raised around the selection." : "Raised around the layer's opaque area.");
            return c;
        },
        [&](Image& img) {
            auto c8 = [](float f) { return static_cast<uint8_t>(f * 255.0f + 0.5f); };
            effects::outer_bevel(img, doc && doc->has_selection() ? doc->selection().data() : nullptr, obevel_width, obevel_angle, obevel_depth,
                                 {c8(obevel_color[0]), c8(obevel_color[1]), c8(obevel_color[2]), 255});
        });
}
