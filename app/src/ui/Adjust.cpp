// Adjustment and effect dialogs with live preview. Each dialog edits its
// parameters, the preview session re-applies the operation to the active
// layer (clipped to the selection), and OK records one history entry.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "App.h"
#include "firn/adjust.h"
#include "firn/effects.h"
#include "firn/photo.h"
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
    if (!preview.active || !doc) return;
    const bool dragging = !force && ImGui::IsAnyItemActive();
    // Once the slider is released, replace an approximate preview with the exact result.
    if (!preview.dirty && !(preview.approximate && !dragging)) return;

    if (preview.live || !dragging) {
        Image work = preview.before;
        op(work);
        raster::apply_through_mask(work, preview.before, doc->selection());
        doc->layer(preview.layer).pixels = std::move(work);
        doc->touch();
        preview.dirty = false;
        preview.approximate = false;
        return;
    }

    // Large layer while dragging: process only what is on screen (plus a
    // margin for kernels), or a downscaled proxy when most of the image is
    // visible. Exact output follows on release.
    const int W = preview.before.width(), H = preview.before.height();
    const int margin = 64;
    raster::Rect r = visible_image_rect.empty() ? raster::Rect{0, 0, W, H} : visible_image_rect;
    r = raster::Rect{r.x0 - margin, r.y0 - margin, r.x1 + margin, r.y1 + margin}.clipped(W, H);
    const long area = static_cast<long>(r.x1 - r.x0) * (r.y1 - r.y0);
    Image work = preview.before;
    if (area <= 1536L * 1024L) {
        Image part = raster::crop(preview.before, r);
        op(part);
        for (int y = r.y0; y < r.y1; ++y)
            std::memcpy(work.data() + (static_cast<size_t>(y) * W + r.x0) * 4, part.data() + static_cast<size_t>(y - r.y0) * part.width() * 4,
                        static_cast<size_t>(r.x1 - r.x0) * 4);
    } else {
        const float scale = std::min(1.0f, 1024.0f / std::max(W, H));
        Image proxy = raster::resample(preview.before, std::max(1, static_cast<int>(W * scale)), std::max(1, static_cast<int>(H * scale)), raster::Filter::Bilinear);
        op(proxy);
        work = raster::resample(proxy, W, H, raster::Filter::Bilinear);
        r = {0, 0, W, H};
    }
    raster::apply_through_mask(work, preview.before, doc->selection());
    doc->layer(preview.layer).pixels = std::move(work);
    doc->touch(r);
    preview.dirty = false;
    preview.approximate = true;
}

void App::preview_commit(const std::function<void(Image16&)>& op16) {
    if (!preview.active || !doc) return;
    Layer& L = doc->layer(preview.layer);
    if (L.is_deep() && op16) {
        // Re-run the operation on the 16-bit data through a command (the
        // 8-bit preview is discarded).
        const Image after = L.pixels;
        L.pixels = preview.before;
        const std::string name = preview.name;
        run(std::make_unique<AdjustCommand>(preview.layer, name, [after](Image& img) { img = after; }, op16));
        preview = Preview{};
        return;
    }
    auto cmd = std::make_unique<LayerSnapshotCommand>(preview.layer, preview.name, preview.before, L.pixels);
    cmd->capture_deep(*doc);
    if (L.is_deep() == false && preview.before.width() > 0 && before_was_deep_) status = preview.name + ": the layer is now 8 bits per channel";
    commit(std::move(cmd));
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
void adjust_modal(App& app, const char* title, Body body, Op op, std::function<void(Image16&)> op16 = nullptr) {
    // The Effect Browser borrows every dialog's operation for its thumbnails.
    if (app.effect_capture) app.effect_ops[title] = std::function<void(Image&)>(op);
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (ImGui::IsWindowAppearing()) { app.preview_begin(title); app.before_was_deep_ = app.doc && app.doc->layer(app.preview.layer).is_deep(); }
    if (body()) app.preview.dirty = true;
    app.preview_update(op);
    ImGui::Separator();
    if (!app.preview.live) ImGui::TextDisabled("Large image: the preview is approximate while a slider is held.");
    const bool ok = ImGui::Button("OK", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (ok) {
        app.preview_update(op, true);
        app.preview_commit(op16);
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

}  // namespace

// Editable curve: click to add a point, drag to move, right-click to remove.
// Shared with the adjustment layer dialog (edits app.curve_points).
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

namespace {

}  // namespace

static const char* kTitles[] = {nullptr, "Brightness/Contrast", "Curves", "Gamma Correction", "Levels", "Threshold",
                                    "Channel Mixer", "Colorize", "Hue/Saturation/Lightness", "Average", "Gaussian Blur",
                                    "Posterize", "Solarize", "Unsharp Mask", "Median", "Motion Blur", "Mosaic",
                                    "Add Noise", "Drop Shadow", "Color Balance", "Sepia Toning", "Hue Map", "Wave",
                                    "Pinch", "Twirl", "Buttonize", "Inner Bevel", "Cutout", "Ripple", "Spherize",
                                    "Lens Distortion", "Halftone", "Chrome", "Outer Bevel", "Fade Correction",
                                    "Kaleidoscope", "Sunburst",
                                    "Automatic Color Balance", "Automatic Contrast Enhancement", "Automatic Saturation Enhancement",
                                    "Clarify", "Black and White Points", "Histogram Adjustment", "Salt and Pepper Filter",
                                    "JPEG Artifact Removal", "Fill Flash", "Backlighting", "Chromatic Aberration Removal",
                                    "Digital Camera Noise Removal",
                                    "Curlicues", "Displacement Map", "Polar Coordinates", "Spiky Halo", "Warp", "Wind",
                                    "Circle", "Cylinder", "Pentagon", "Perspective", "Skew", "Feedback", "Pattern",
                                    "Rotating Mirror", "Offset", "Seamless Tiling", "Page Curl",
                                    "Aged Newspaper", "Balls and Bubbles", "Colored Edges", "Colored Foil", "Contours", "Enamel",
                                    "Glowing Edges", "Hot Wax Coating", "Magnifying Lens", "Neon Glow", "Topography", "Lights",
                                    "Blinds", "Fine Leather", "Rough Leather", "Fur", "Mosaic - Antique", "Mosaic - Glass",
                                    "Polished Stone", "Sandstone", "Sculpture", "Soft Plastic", "Straw Wall", "Texture", "Tiles",
                                    "Weave", "Black Pencil", "Brush Strokes", "Charcoal", "Colored Chalk", "Colored Pencil", "Pencil",
                                    "User Defined Filter"};

effects::Edge App::edge_setting() const {
    return effects::Edge{edge_mode, Color{static_cast<uint8_t>(edge_color[0] * 255 + 0.5f), static_cast<uint8_t>(edge_color[1] * 255 + 0.5f), static_cast<uint8_t>(edge_color[2] * 255 + 0.5f), 255}};
}

Color App::float_rgb(const float* f) {
    return Color{static_cast<uint8_t>(f[0] * 255 + 0.5f), static_cast<uint8_t>(f[1] * 255 + 0.5f), static_cast<uint8_t>(f[2] * 255 + 0.5f), 255};
}

int App::adjust_count() { return static_cast<int>(sizeof(kTitles) / sizeof(kTitles[0])); }
const char* App::adjust_title(int i) { return i > 0 && i < adjust_count() ? kTitles[i] : ""; }

bool App::open_adjust_by_title(const char* title) {
    for (size_t i = 1; i < sizeof(kTitles) / sizeof(kTitles[0]); ++i)
        if (std::strcmp(kTitles[i], title) == 0) { open_adjust = static_cast<Adj>(i); return true; }
    return false;
}

void App::draw_adjust_dialogs() {

    if (open_adjust != Adj::None) {
        if (doc && active_layer() >= 0) ImGui::OpenPopup(kTitles[static_cast<int>(open_adjust)]);
        open_adjust = Adj::None;
    }

    adjust_modal(*this, "Brightness/Contrast",
        [&] { bool c = ImGui::SliderInt("Brightness", &bc_brightness, -255, 255); c |= ImGui::SliderInt("Contrast", &bc_contrast, -100, 100); return c; },
        [&](Image& img) { adjust::apply_lut(img, adjust::brightness_contrast_lut(bc_brightness, bc_contrast)); },
        [&](Image16& img) { raster16::brightness_contrast(img, bc_brightness, bc_contrast); });

    adjust_modal(*this, "Curves",
        [&] {
            bool c = curve_editor(*this, ImVec2(256, 256));
            ImGui::TextDisabled("Click to add a point, drag to move, right-click to remove.");
            if (ImGui::SmallButton("Reset")) { curve_points = {{0, 0}, {255, 255}}; c = true; }
            return c;
        },
        [&](Image& img) { adjust::apply_lut(img, adjust::curve_lut(curve_points)); },
        [&](Image16& img) { raster16::curves(img, curve_points); });

    adjust_modal(*this, "Gamma Correction",
        [&] {
            bool c = ImGui::Checkbox("Link channels", &gamma_link);
            static const char* names[3] = {"Red", "Green", "Blue"};
            for (int i = 0; i < 3; ++i) {
                if (ImGui::SliderFloat(gamma_link ? (i == 0 ? "Gamma" : "##g") : names[i], &gamma_rgb[i], 0.1f, 5.0f, "%.2f", ImGuiSliderFlags_Logarithmic)) {
                    if (gamma_link) gamma_rgb[0] = gamma_rgb[1] = gamma_rgb[2] = gamma_rgb[i];
                    c = true;
                }
                if (gamma_link) break;
            }
            return c;
        },
        [&](Image& img) { adjust::apply_luts(img, adjust::gamma_lut(gamma_rgb[0]), adjust::gamma_lut(gamma_rgb[1]), adjust::gamma_lut(gamma_rgb[2])); },
        [&](Image16& img) { raster16::gamma(img, gamma_link ? gamma_value : gamma_rgb[0], gamma_link ? gamma_value : gamma_rgb[1], gamma_link ? gamma_value : gamma_rgb[2]); });

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
        [&](Image& img) { adjust::apply_lut(img, adjust::levels_lut(lv_in_lo, lv_gamma, lv_in_hi, lv_out_lo, lv_out_hi)); },
        [&](Image16& img) { raster16::levels(img, lv_in_lo, lv_gamma, lv_in_hi, lv_out_lo, lv_out_hi); });

    adjust_modal(*this, "Threshold",
        [&] { draw_histogram(preview.histogram, ImVec2(256, 60)); return ImGui::SliderInt("Threshold", &threshold_value, 1, 255); },
        [&](Image& img) { adjust::grayscale_then_threshold(img, threshold_value); },
        [&](Image16& img) { raster16::threshold(img, threshold_value); });

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
        [&](Image& img) { adjust::channel_mixer(img, mixer); },
        [&](Image16& img) { raster16::channel_mixer(img, mixer); });

    adjust_modal(*this, "Colorize",
        [&] {
            bool c = ImGui::SliderInt("Hue", &colorize_hue, 0, 359);
            c |= ImGui::SliderInt("Saturation", &colorize_sat, 0, 255);
            return c;
        },
        [&](Image& img) { adjust::colorize(img, colorize_hue, colorize_sat); },
        [&](Image16& img) { raster16::colorize(img, colorize_hue, colorize_sat); });

    adjust_modal(*this, "Hue/Saturation/Lightness",
        [&] {
            bool c = ImGui::SliderInt("Hue", &hsl_h, -180, 180);
            c |= ImGui::SliderInt("Saturation", &hsl_s, -100, 100);
            c |= ImGui::SliderInt("Lightness", &hsl_l, -100, 100);
            return c;
        },
        [&](Image& img) { adjust::hsl_adjust(img, hsl_h, hsl_s, hsl_l); },
        [&](Image16& img) { raster16::hsl_adjust(img, hsl_h, hsl_s, hsl_l); });

    adjust_modal(*this, "Average",
        [&] { return ImGui::SliderInt("Radius", &box_radius, 1, 50); },
        [&](Image& img) { raster::box_blur(img, box_radius); });

    adjust_modal(*this, "Gaussian Blur",
        [&] { return ImGui::SliderFloat("Radius", &blur_radius, 0.1f, 100.0f, "%.1f", ImGuiSliderFlags_Logarithmic); },
        [&](Image& img) { raster::gaussian_blur(img, blur_radius); },
        [&](Image16& img) { raster16::gaussian_blur(img, blur_radius); });

    adjust_modal(*this, "Posterize",
        [&] { return ImGui::SliderInt("Levels", &posterize_levels, 2, 255, "%d", ImGuiSliderFlags_Logarithmic); },
        [&](Image& img) { adjust::apply_lut(img, adjust::posterize_lut(posterize_levels)); },
        [&](Image16& img) { raster16::posterize(img, posterize_levels); });

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
        [&](Image& img) { adjust::color_balance(img, color_balance); },
        [&](Image16& img) { raster16::color_balance(img, color_balance); });

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

    adjust_modal(*this, "Fade Correction",
        [&] { return ImGui::SliderInt("Amount of correction", &fade_amount, 1, 100); },
        [&](Image& img) { adjust::fade_correction(img, fade_amount); });

    adjust_modal(*this, "Automatic Color Balance",
        [&] { bool c = ImGui::SliderInt("Strength", &acb_strength, 0, 100); c |= ImGui::SliderInt("Illuminant temperature (K)", &acb_temperature, 2000, 12000); return c; },
        [&](Image& img) { photo::auto_color_balance(img, acb_strength, acb_temperature); });
    adjust_modal(*this, "Automatic Contrast Enhancement",
        [&] { bool c = ImGui::Combo("Bias", &ace_bias, "Lighter\0Neutral\0Darker\0"); c |= ImGui::Combo("Strength", &ace_strength, "Normal\0Mild\0"); c |= ImGui::Combo("Appearance", &ace_appearance, "Flat\0Natural\0Bold\0"); return c; },
        [&](Image& img) { photo::auto_contrast_enhance(img, ace_bias, ace_strength, ace_appearance); });
    adjust_modal(*this, "Automatic Saturation Enhancement",
        [&] { bool c = ImGui::Combo("Bias", &ase_bias, "Less colorful\0Normal\0More colorful\0"); c |= ImGui::Combo("Strength", &ase_strength, "Weak\0Normal\0Strong\0"); c |= ImGui::Checkbox("Skin tones present", &ase_skin); return c; },
        [&](Image& img) { photo::auto_saturation(img, ase_bias, ase_strength, ase_skin); });
    adjust_modal(*this, "Clarify",
        [&] { return ImGui::SliderInt("Strength of effect", &clarify_strength, 1, 5); },
        [&](Image& img) { photo::clarify(img, clarify_strength); });
    adjust_modal(*this, "Black and White Points",
        [&] {
            bool c = ImGui::ColorEdit3("Source black", bwp_src_black, ImGuiColorEditFlags_NoInputs); ImGui::SameLine(); c |= ImGui::ColorEdit3("Destination black", bwp_dst_black, ImGuiColorEditFlags_NoInputs);
            c |= ImGui::ColorEdit3("Source white", bwp_src_white, ImGuiColorEditFlags_NoInputs); ImGui::SameLine(); c |= ImGui::ColorEdit3("Destination white", bwp_dst_white, ImGuiColorEditFlags_NoInputs);
            ImGui::TextDisabled("Pick the darkest and lightest colors that should become the destination black and white.");
            return c;
        },
        [&](Image& img) {
            photo::black_white_points(img, float_rgb(bwp_src_black), float_rgb(bwp_src_white), float_rgb(bwp_dst_black), float_rgb(bwp_dst_white));
        });
    adjust_modal(*this, "Histogram Adjustment",
        [&] {
            draw_histogram(preview.histogram, ImVec2(256, 80));
            bool c = ImGui::Combo("Edit", &ha_channel, "Luminance\0Red\0Green\0Blue\0");
            c |= ImGui::SliderFloat("Low clip %", &ha_low, 0.0f, 50.0f, "%.2f");
            c |= ImGui::SliderFloat("High clip %", &ha_high, 0.0f, 50.0f, "%.2f");
            c |= ImGui::SliderFloat("Gamma", &ha_gamma, 0.1f, 7.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            c |= ImGui::SliderInt("Midtones (compress / expand)", &ha_midtones, -100, 100);
            return c;
        },
        [&](Image& img) { photo::histogram_adjust(img, ha_low, ha_high, ha_gamma, ha_midtones, ha_channel); });
    adjust_modal(*this, "Salt and Pepper Filter",
        [&] { bool c = ImGui::SliderInt("Speck size", &sp_size, 3, 9); c |= ImGui::SliderInt("Sensitivity to specks", &sp_sensitivity, 1, 30); c |= ImGui::Checkbox("Include all lower speck sizes", &sp_smaller); c |= ImGui::Checkbox("Aggressive action", &sp_aggressive); return c; },
        [&](Image& img) { photo::salt_and_pepper(img, sp_size, sp_sensitivity, sp_smaller, sp_aggressive); });
    adjust_modal(*this, "JPEG Artifact Removal",
        [&] { bool c = ImGui::Combo("Strength", &jpeg_strength, "Low\0Normal\0High\0Maximum\0"); c |= ImGui::SliderInt("Crispness", &jpeg_crispness, 0, 100); return c; },
        [&](Image& img) { photo::jpeg_artifact_removal(img, jpeg_strength, jpeg_crispness); });
    adjust_modal(*this, "Fill Flash",
        [&] { return ImGui::SliderInt("Strength", &flash_strength, 0, 100); },
        [&](Image& img) { photo::fill_flash(img, flash_strength); });
    adjust_modal(*this, "Backlighting",
        [&] { return ImGui::SliderInt("Strength", &backlight_strength, 0, 100); },
        [&](Image& img) { photo::backlighting(img, backlight_strength); });
    adjust_modal(*this, "Chromatic Aberration Removal",
        [&] { bool c = ImGui::SliderFloat("Red fringe (px at corners)", &ca_red, -20.0f, 20.0f, "%.1f"); c |= ImGui::SliderFloat("Blue fringe (px at corners)", &ca_blue, -20.0f, 20.0f, "%.1f"); return c; },
        [&](Image& img) { photo::chromatic_aberration(img, ca_red, ca_blue); });
    adjust_modal(*this, "Digital Camera Noise Removal",
        [&] { bool c = ImGui::SliderInt("Strength", &nr_strength, 0, 100); c |= ImGui::SliderInt("Correction blend %", &nr_blend, 0, 100); c |= ImGui::SliderInt("Sharpening %", &nr_sharpen, 0, 100); return c; },
        [&](Image& img) { photo::noise_removal(img, nr_strength, nr_blend, nr_sharpen); });

    auto edge_options = [&] {
        bool c = ImGui::Combo("Edge mode", &edge_mode, "Wrap\0Repeat\0Color\0Transparent\0");
        if (edge_mode == 2) { ImGui::SameLine(); c |= ImGui::ColorEdit3("##edgecol", edge_color, ImGuiColorEditFlags_NoInputs); }
        return c;
    };
    // Operations may run after this function returns (the Effect Browser
    // keeps them), so they call members (edge_setting, float_rgb), never locals.
    adjust_modal(*this, "Curlicues",
        [&] { bool c = ImGui::SliderInt("Columns", &curl_cols, 1, 20); c |= ImGui::SliderInt("Rows", &curl_rows, 1, 20); c |= ImGui::SliderInt("Radius", &curl_radius, 1, 100); c |= ImGui::SliderInt("Strength", &curl_strength, -100, 100); return c; },
        [&](Image& img) { effects::curlicues(img, curl_cols, curl_rows, curl_radius, curl_strength); });
    adjust_modal(*this, "Displacement Map",
        [&] {
            bool c = false;
            const std::string cur = dmap_source >= 0 && dmap_source < static_cast<int>(docs.size()) ? document_title(dmap_source) : "This image";
            ImGui::SetNextItemWidth(220);
            if (ImGui::BeginCombo("Displacement map", cur.c_str())) {
                if (ImGui::Selectable("This image", dmap_source < 0)) { dmap_source = -1; c = true; }
                for (int i = 0; i < static_cast<int>(docs.size()); ++i) { ImGui::PushID(i); if (ImGui::Selectable(document_title(i).c_str(), i == dmap_source)) { dmap_source = i; c = true; } ImGui::PopID(); }
                ImGui::EndCombo();
            }
            c |= ImGui::Checkbox("2D offsets (red = x, green = y)", &dmap_2d);
            c |= ImGui::SliderFloat("Intensity %", &dmap_intensity, 0.0f, 100.0f, "%.1f");
            c |= ImGui::SliderFloat("Blur", &dmap_blur, 0.0f, 50.0f, "%.1f");
            c |= edge_options();
            return c;
        },
        [&](Image& img) {
            Document* d = dmap_source >= 0 ? document_at(dmap_source) : nullptr;
            const Image map = d ? d->composite() : preview.active && !preview.before.empty() ? preview.before : img;
            effects::displacement_map(img, map, dmap_intensity, dmap_2d, dmap_blur, edge_setting());
        });
    adjust_modal(*this, "Polar Coordinates",
        [&] { bool c = ImGui::Checkbox("Rectangular to polar", &polar_rect); c |= edge_options(); return c; },
        [&](Image& img) { effects::polar_coordinates(img, polar_rect, edge_setting()); });
    adjust_modal(*this, "Spiky Halo",
        [&] { bool c = ImGui::SliderFloat("Radius %", &halo_radius, 1.0f, 200.0f, "%.0f"); c |= ImGui::SliderInt("Spikes", &halo_spikes, 1, 100); c |= ImGui::SliderFloat("Radius offset %", &halo_offset, 0.0f, 100.0f, "%.0f"); c |= ImGui::SliderInt("Bend", &halo_bend, -100, 100); return c; },
        [&](Image& img) { effects::spiky_halo(img, halo_radius, halo_spikes, halo_offset, halo_bend); });
    adjust_modal(*this, "Warp",
        [&] { bool c = ImGui::SliderFloat("Center X %", &warp_cx, 0.0f, 100.0f, "%.0f"); c |= ImGui::SliderFloat("Center Y %", &warp_cy, 0.0f, 100.0f, "%.0f"); c |= ImGui::SliderFloat("Size %", &warp_size, 1.0f, 100.0f, "%.0f"); c |= ImGui::SliderInt("Strength", &warp_strength_fx, -100, 100); return c; },
        [&](Image& img) { effects::warp(img, warp_cx, warp_cy, warp_size, warp_strength_fx); });
    adjust_modal(*this, "Wind",
        [&] { bool c = ImGui::Checkbox("From left", &wind_left); c |= ImGui::SliderInt("Strength", &wind_strength, 1, 100); return c; },
        [&](Image& img) { effects::wind(img, wind_left, wind_strength); });
    adjust_modal(*this, "Circle",
        [&] { return edge_options(); },
        [&](Image& img) { effects::circle(img, edge_setting()); });
    adjust_modal(*this, "Cylinder",
        [&] { bool c = ImGui::Checkbox("Vertical", &cyl_vertical); c |= ImGui::SliderInt("Strength", &cyl_strength, 0, 100); return c; },
        [&](Image& img) { effects::cylinder(img, cyl_vertical, cyl_strength); });
    adjust_modal(*this, "Pentagon",
        [&] { return edge_options(); },
        [&](Image& img) { effects::pentagon(img, edge_setting()); });
    adjust_modal(*this, "Perspective",
        [&] { bool c = ImGui::Checkbox("Vertical", &persp_vertical); c |= ImGui::SliderInt("Distortion", &persp_distortion, -100, 100); c |= edge_options(); return c; },
        [&](Image& img) { effects::perspective(img, persp_vertical, persp_distortion, edge_setting()); });
    adjust_modal(*this, "Skew",
        [&] { bool c = ImGui::Checkbox("Vertical", &skew_vertical); c |= ImGui::SliderInt("Angle", &skew_angle, -45, 45); c |= edge_options(); return c; },
        [&](Image& img) { effects::skew(img, skew_vertical, skew_angle, edge_setting()); });
    adjust_modal(*this, "Feedback",
        [&] { bool c = ImGui::SliderInt("Opacity", &fb_opacity, 1, 100); c |= ImGui::SliderInt("Intensity", &fb_intensity, 1, 20); c |= ImGui::SliderFloat("Center X %", &fb_cx, 0.0f, 100.0f, "%.0f"); c |= ImGui::SliderFloat("Center Y %", &fb_cy, 0.0f, 100.0f, "%.0f"); c |= ImGui::Checkbox("Elliptical", &fb_elliptical); return c; },
        [&](Image& img) { effects::feedback(img, fb_opacity, fb_intensity, fb_cx, fb_cy, fb_elliptical); });
    adjust_modal(*this, "Pattern",
        [&] { bool c = ImGui::SliderFloat("Angle", &pat_angle, 0.0f, 360.0f, "%.0f"); c |= ImGui::SliderFloat("Center X %", &pat_cx, 0.0f, 100.0f, "%.0f"); c |= ImGui::SliderFloat("Center Y %", &pat_cy, 0.0f, 100.0f, "%.0f"); c |= ImGui::SliderFloat("Scale %", &pat_scale, 1.0f, 100.0f, "%.0f"); c |= ImGui::SliderInt("Rotation", &pat_rotation, 0, 360); return c; },
        [&](Image& img) { effects::pattern(img, pat_angle, pat_cx, pat_cy, pat_scale, pat_rotation); });
    adjust_modal(*this, "Rotating Mirror",
        [&] { bool c = ImGui::SliderFloat("Angle", &mirror_angle, 0.0f, 360.0f, "%.0f"); c |= ImGui::SliderFloat("Center X %", &mirror_cx, 0.0f, 100.0f, "%.0f"); c |= ImGui::SliderFloat("Center Y %", &mirror_cy, 0.0f, 100.0f, "%.0f"); c |= edge_options(); return c; },
        [&](Image& img) { effects::rotating_mirror(img, mirror_angle, mirror_cx, mirror_cy, edge_setting()); });
    adjust_modal(*this, "Offset",
        [&] { bool c = ImGui::SliderInt("Horizontal", &offset_x, -2000, 2000); c |= ImGui::SliderInt("Vertical", &offset_y, -2000, 2000); c |= edge_options(); return c; },
        [&](Image& img) { effects::offset(img, offset_x, offset_y, edge_setting()); });
    adjust_modal(*this, "Seamless Tiling",
        [&] { bool c = ImGui::Combo("Method", &tile_method, "Edge\0Corner\0Mirror\0"); c |= ImGui::Combo("Direction", &tile_direction, "Bidirectional\0Horizontal\0Vertical\0"); c |= ImGui::SliderInt("Transition", &tile_transition, 0, 100); return c; },
        [&](Image& img) { effects::seamless_tiling(img, tile_method, tile_direction, tile_transition); });
    adjust_modal(*this, "Page Curl",
        [&] {
            bool c = ImGui::Combo("Corner", &curl_corner, "Top left\0Top right\0Bottom left\0Bottom right\0");
            c |= ImGui::SliderFloat("Width %", &curl_w, 1.0f, 100.0f, "%.0f"); c |= ImGui::SliderFloat("Height %", &curl_h, 1.0f, 100.0f, "%.0f");
            c |= ImGui::SliderInt("Radius", &curl_r, 2, 200);
            c |= ImGui::ColorEdit3("Back of page", curl_back, ImGuiColorEditFlags_NoInputs); ImGui::SameLine(); c |= ImGui::ColorEdit3("Fill", curl_fill, ImGuiColorEditFlags_NoInputs);
            c |= ImGui::Checkbox("Transparent fill", &curl_transparent);
            return c;
        },
        [&](Image& img) {
            effects::page_curl(img, curl_corner, curl_w, curl_h, curl_r, float_rgb(curl_back), float_rgb(curl_fill), curl_transparent);
        });

    auto angle_color = [&] { bool c = ImGui::SliderFloat("Angle", &fx_angle, 0.0f, 360.0f, "%.0f"); ImGui::SameLine(); c |= ImGui::ColorEdit3("Color", fx_color, ImGuiColorEditFlags_NoInputs); return c; };
    auto blur_detail = [&] { bool c = ImGui::SliderInt("Blur", &fx_blur, 0, 50); c |= ImGui::SliderInt("Detail", &fx_detail, 1, 100); return c; };
    adjust_modal(*this, "Aged Newspaper", [&] { return ImGui::SliderInt("Amount to age", &fx_amount, 1, 100); }, [&](Image& img) { effects::aged_newspaper(img, fx_amount); });
    adjust_modal(*this, "Balls and Bubbles",
        [&] { bool c = ImGui::SliderInt("Count", &fx_count, 1, 300); c |= ImGui::SliderInt("Minimum size", &fx_min, 2, 300); c |= ImGui::SliderInt("Maximum size", &fx_max, 2, 400); c |= ImGui::SliderInt("Opacity", &fx_opacity, 0, 100); c |= ImGui::Checkbox("Bubbles (else balls)", &fx_bubbles); ImGui::SameLine(); c |= ImGui::ColorEdit3("Ball color", fx_color, ImGuiColorEditFlags_NoInputs); return c; },
        [&](Image& img) { effects::balls_and_bubbles(img, fx_count, fx_min, fx_max, fx_opacity, fx_bubbles, float_rgb(fx_color), 7); });
    adjust_modal(*this, "Colored Edges", [&] { bool c = ImGui::SliderInt("Luminance", &fx_luminance, 0, 100); c |= ImGui::SliderInt("Blur", &fx_blur, 0, 20); c |= ImGui::ColorEdit3("Color", fx_color, ImGuiColorEditFlags_NoInputs); return c; }, [&](Image& img) { effects::colored_edges(img, fx_luminance, fx_blur, float_rgb(fx_color)); });
    adjust_modal(*this, "Colored Foil", [&] { bool c = blur_detail(); c |= angle_color(); return c; }, [&](Image& img) { effects::colored_foil(img, fx_blur, fx_detail, float_rgb(fx_color), fx_angle); });
    adjust_modal(*this, "Contours", [&] { bool c = ImGui::SliderInt("Luminance", &fx_luminance, 0, 100); c |= ImGui::SliderInt("Blur", &fx_blur, 0, 20); c |= ImGui::SliderInt("Detail", &fx_detail, 2, 20); c |= ImGui::ColorEdit3("Color", fx_color2, ImGuiColorEditFlags_NoInputs); return c; }, [&](Image& img) { effects::contours(img, fx_luminance, fx_blur, fx_detail, float_rgb(fx_color2)); });
    adjust_modal(*this, "Enamel", [&] { bool c = blur_detail(); c |= ImGui::SliderInt("Density", &fx_density, 0, 100); c |= angle_color(); return c; }, [&](Image& img) { effects::enamel(img, fx_blur, fx_detail, fx_density, fx_angle, float_rgb(fx_color)); });
    adjust_modal(*this, "Glowing Edges", [&] { bool c = ImGui::SliderInt("Intensity", &fx_intensity, 1, 100); c |= ImGui::SliderInt("Sharpness", &fx_sharpness, 1, 100); return c; }, [&](Image& img) { effects::glowing_edges(img, fx_intensity, fx_sharpness); });
    adjust_modal(*this, "Hot Wax Coating", [&] { return ImGui::ColorEdit3("Wax (foreground material)", fg_color, ImGuiColorEditFlags_NoInputs); }, [&](Image& img) { effects::hot_wax(img, float_rgb(fg_color)); });
    adjust_modal(*this, "Magnifying Lens", [&] { bool c = ImGui::SliderFloat("Center X %", &fx_cx, 0, 100, "%.0f"); c |= ImGui::SliderFloat("Center Y %", &fx_cy, 0, 100, "%.0f"); c |= ImGui::SliderFloat("Size %", &fx_size_pct, 1, 100, "%.0f"); c |= ImGui::SliderInt("Refraction", &fx_refraction, 0, 100); c |= ImGui::SliderInt("Shading", &fx_shading, 0, 100); return c; }, [&](Image& img) { effects::magnifying_lens(img, fx_cx, fx_cy, fx_size_pct, fx_refraction, fx_shading); });
    adjust_modal(*this, "Neon Glow", [&] { bool c = ImGui::SliderInt("Detail", &fx_detail, 1, 100); c |= ImGui::SliderInt("Opacity", &fx_opacity, 0, 100); return c; }, [&](Image& img) { effects::neon_glow(img, fx_detail, fx_opacity); });
    adjust_modal(*this, "Topography", [&] { bool c = ImGui::SliderInt("Width", &fx_width, 1, 100); c |= ImGui::SliderInt("Density", &fx_density, 2, 32); c |= angle_color(); return c; }, [&](Image& img) { effects::topography(img, fx_width, fx_density, fx_angle, float_rgb(fx_color)); });
    adjust_modal(*this, "Lights",
        [&] {
            bool c = ImGui::SliderInt("Darkness", &fx_darkness, 0, 100);
            if (!fx_lights[0].on && !fx_lights[1].on && !fx_lights[2].on && !fx_lights[3].on && !fx_lights[4].on) { fx_lights[0].on = true; c = true; }
            for (int i = 0; i < 5; ++i) {
                ImGui::PushID(i);
                effects::Light& L = fx_lights[i];
                char label[16]; std::snprintf(label, sizeof(label), "Light %d", i + 1);
                c |= ImGui::Checkbox(label, &L.on);
                if (L.on) {
                    ImGui::SameLine(); ImGui::SetNextItemWidth(70); c |= ImGui::SliderFloat("X", &L.x, 0, 100, "%.0f");
                    ImGui::SameLine(); ImGui::SetNextItemWidth(70); c |= ImGui::SliderFloat("Y", &L.y, 0, 100, "%.0f");
                    ImGui::SameLine(); ImGui::SetNextItemWidth(70); c |= ImGui::SliderFloat("Dir", &L.direction, 0, 360, "%.0f");
                    ImGui::SameLine(); ImGui::SetNextItemWidth(70); c |= ImGui::SliderFloat("Cone", &L.cone, 1, 360, "%.0f");
                    ImGui::SameLine(); ImGui::SetNextItemWidth(70); c |= ImGui::SliderInt("Int", &L.intensity, 0, 100);
                    float lc[3] = {L.color.r / 255.0f, L.color.g / 255.0f, L.color.b / 255.0f};
                    ImGui::SameLine(); if (ImGui::ColorEdit3("##lc", lc, ImGuiColorEditFlags_NoInputs)) { L.color = float_rgb(lc); c = true; }
                }
                ImGui::PopID();
            }
            return c;
        },
        [&](Image& img) { effects::lights(img, fx_lights, 5, fx_darkness); });
    adjust_modal(*this, "Blinds", [&] { bool c = ImGui::SliderInt("Width", &fx_width, 2, 200); c |= ImGui::SliderInt("Opacity", &fx_opacity, 0, 100); c |= ImGui::Checkbox("Horizontal", &fx_horizontal); ImGui::SameLine(); c |= ImGui::Checkbox("Light from left/top", &fx_from_left); ImGui::SameLine(); c |= ImGui::ColorEdit3("Color", fx_color2, ImGuiColorEditFlags_NoInputs); return c; }, [&](Image& img) { effects::blinds(img, fx_width, fx_opacity, fx_horizontal, fx_from_left, float_rgb(fx_color2)); });
    adjust_modal(*this, "Fine Leather", [&] { bool c = ImGui::SliderInt("Color amount", &fx_amount, 0, 100); c |= ImGui::SliderInt("Blur", &fx_blur, 0, 20); c |= ImGui::SliderInt("Transparency", &fx_opacity, 0, 100); c |= angle_color(); return c; }, [&](Image& img) { effects::leather(img, false, fx_amount, fx_angle, fx_blur, fx_opacity, float_rgb(fx_color), 3); });
    adjust_modal(*this, "Rough Leather", [&] { bool c = ImGui::SliderInt("Color amount", &fx_amount, 0, 100); c |= ImGui::SliderInt("Blur", &fx_blur, 0, 20); c |= ImGui::SliderInt("Transparency", &fx_opacity, 0, 100); c |= angle_color(); return c; }, [&](Image& img) { effects::leather(img, true, fx_amount, fx_angle, fx_blur, fx_opacity, float_rgb(fx_color), 4); });
    adjust_modal(*this, "Fur", [&] { bool c = ImGui::SliderInt("Blur", &fx_blur, 0, 20); c |= ImGui::SliderInt("Density", &fx_density, 1, 100); c |= ImGui::SliderInt("Length", &fx_length, 2, 100); c |= ImGui::SliderInt("Transparency", &fx_opacity, 0, 100); return c; }, [&](Image& img) { effects::fur(img, fx_blur, fx_density, fx_length, fx_opacity, 5); });
    adjust_modal(*this, "Mosaic - Antique", [&] { bool c = ImGui::SliderInt("Columns", &fx_columns, 1, 100); c |= ImGui::SliderInt("Rows", &fx_rows, 1, 100); c |= ImGui::SliderInt("Diffusion", &fx_diffusion, 0, 100); c |= ImGui::SliderInt("Grout width", &fx_grout, 0, 100); c |= ImGui::SliderInt("Grout transparency", &fx_grout_alpha, 0, 100); return c; }, [&](Image& img) { effects::mosaic_antique(img, fx_columns, fx_rows, 0, fx_diffusion, fx_grout, fx_grout_alpha); });
    adjust_modal(*this, "Mosaic - Glass", [&] { bool c = ImGui::SliderInt("Columns", &fx_columns, 1, 100); c |= ImGui::SliderInt("Rows", &fx_rows, 1, 100); c |= ImGui::SliderInt("Curvature", &fx_curvature, 0, 100); c |= ImGui::SliderInt("Edge width", &fx_grout, 0, 100); c |= ImGui::SliderInt("Grout transparency", &fx_grout_alpha, 0, 100); return c; }, [&](Image& img) { effects::mosaic_glass(img, fx_columns, fx_rows, fx_curvature, fx_grout, fx_grout_alpha); });
    adjust_modal(*this, "Polished Stone", [&] { bool c = blur_detail(); c |= ImGui::SliderInt("Color amount", &fx_amount, 0, 100); c |= angle_color(); return c; }, [&](Image& img) { effects::polished_stone(img, fx_blur, fx_detail, fx_angle, fx_amount, float_rgb(fx_color)); });
    adjust_modal(*this, "Sandstone", [&] { bool c = blur_detail(); c |= angle_color(); return c; }, [&](Image& img) { effects::sandstone(img, fx_blur, fx_detail, fx_angle, float_rgb(fx_color), 2); });
    adjust_modal(*this, "Sculpture", [&] { bool c = ImGui::SliderInt("Smoothness", &fx_smooth, 0, 100); c |= ImGui::SliderInt("Depth", &fx_depth, 1, 100); c |= angle_color(); return c; }, [&](Image& img) { effects::sculpture(img, fx_smooth, fx_depth, fx_angle, float_rgb(fx_color)); });
    adjust_modal(*this, "Soft Plastic", [&] { bool c = blur_detail(); c |= ImGui::SliderInt("Density", &fx_density, 0, 100); c |= angle_color(); return c; }, [&](Image& img) { effects::soft_plastic(img, fx_blur, fx_detail, fx_density, fx_angle, float_rgb(fx_color)); });
    adjust_modal(*this, "Straw Wall", [&] { bool c = blur_detail(); c |= ImGui::SliderInt("Density", &fx_density, 1, 100); c |= angle_color(); return c; }, [&](Image& img) { effects::straw_wall(img, fx_blur, fx_detail, fx_density, fx_angle, float_rgb(fx_color), 6); });
    adjust_modal(*this, "Texture",
        [&] {
            ensure_textures();
            const char* cur = texture_index >= 0 && texture_index < static_cast<int>(textures.size()) ? textures[static_cast<size_t>(texture_index)].name.c_str() : "(noise)";
            bool c = false;
            ImGui::SetNextItemWidth(200);
            if (ImGui::BeginCombo("Texture", cur)) {
                if (ImGui::Selectable("(noise)", texture_index < 0)) { select_texture(-1); c = true; }
                for (size_t i = 0; i < textures.size(); ++i) { ImGui::PushID(static_cast<int>(i)); if (ImGui::Selectable(textures[i].name.c_str(), static_cast<int>(i) == texture_index)) { select_texture(static_cast<int>(i)); c = true; } ImGui::PopID(); }
                ImGui::EndCombo();
            }
            c |= ImGui::SliderInt("Size %", &fx_size, 10, 400); c |= ImGui::SliderInt("Smoothness", &fx_smooth, 0, 100); c |= ImGui::SliderInt("Depth", &fx_depth, 1, 100); c |= angle_color();
            return c;
        },
        [&](Image& img) {
            Image bump;
            if (texture_index >= 0 && texture_index < static_cast<int>(textures.size()) && textures[static_cast<size_t>(texture_index)].texture) {
                const auto& t = *textures[static_cast<size_t>(texture_index)].texture;
                bump = Image(t.width, t.height);
                for (int i = 0; i < t.width * t.height; ++i) { uint8_t* p = bump.data() + static_cast<size_t>(i) * 4; p[0] = p[1] = p[2] = t.coverage[static_cast<size_t>(i)]; p[3] = 255; }
            }
            effects::texture(img, bump, fx_size, fx_smooth, fx_depth, fx_angle, float_rgb(fx_color));
        });
    adjust_modal(*this, "Tiles", [&] { bool c = ImGui::Combo("Shape", &fx_shape, "Square\0Hexagon\0Triangle\0"); c |= ImGui::SliderInt("Size", &fx_size, 4, 200); c |= ImGui::SliderInt("Border", &fx_border, 0, 100); c |= ImGui::SliderInt("Smoothness", &fx_smooth, 0, 100); c |= ImGui::SliderInt("Depth", &fx_depth, 1, 100); c |= angle_color(); return c; }, [&](Image& img) { effects::tiles(img, fx_shape, fx_size, fx_border, fx_smooth, fx_depth, fx_angle, float_rgb(fx_color)); });
    adjust_modal(*this, "Weave", [&] { bool c = ImGui::SliderInt("Gap", &fx_gap, 0, 100); c |= ImGui::SliderInt("Width", &fx_stroke_width, 1, 100); c |= ImGui::SliderInt("Opacity", &fx_opacity, 0, 100); c |= ImGui::ColorEdit3("Gap color", fx_color2, ImGuiColorEditFlags_NoInputs); ImGui::SameLine(); c |= ImGui::ColorEdit3("Weave color", fx_color, ImGuiColorEditFlags_NoInputs); c |= ImGui::Checkbox("Fill gaps", &fx_fill_gaps); return c; }, [&](Image& img) { effects::weave(img, fx_gap, fx_stroke_width, fx_opacity, float_rgb(fx_color2), float_rgb(fx_color), fx_fill_gaps); });
    adjust_modal(*this, "Black Pencil", [&] { bool c = ImGui::SliderInt("Detail", &fx_detail, 1, 100); c |= ImGui::SliderInt("Opacity", &fx_opacity, 0, 100); return c; }, [&](Image& img) { effects::black_pencil(img, fx_detail, fx_opacity); });
    adjust_modal(*this, "Brush Strokes", [&] { bool c = ImGui::SliderInt("Length", &fx_length, 2, 100); c |= ImGui::SliderInt("Density", &fx_density, 1, 100); c |= ImGui::SliderInt("Width", &fx_stroke_width, 1, 30); c |= ImGui::SliderInt("Opacity", &fx_opacity, 0, 100); return c; }, [&](Image& img) { effects::brush_strokes(img, fx_length, fx_density, fx_stroke_width, fx_opacity, 8); });
    adjust_modal(*this, "Charcoal", [&] { bool c = ImGui::SliderInt("Detail", &fx_detail, 1, 100); c |= ImGui::SliderInt("Opacity", &fx_opacity, 0, 100); return c; }, [&](Image& img) { effects::charcoal(img, fx_detail, fx_opacity); });
    adjust_modal(*this, "Colored Chalk", [&] { bool c = ImGui::SliderInt("Detail", &fx_detail, 1, 100); c |= ImGui::SliderInt("Opacity", &fx_opacity, 0, 100); return c; }, [&](Image& img) { effects::colored_chalk(img, fx_detail, fx_opacity); });
    adjust_modal(*this, "Colored Pencil", [&] { bool c = ImGui::SliderInt("Detail", &fx_detail, 1, 100); c |= ImGui::SliderInt("Opacity", &fx_opacity, 0, 100); return c; }, [&](Image& img) { effects::colored_pencil(img, fx_detail, fx_opacity); });
    adjust_modal(*this, "Pencil", [&] { bool c = ImGui::SliderInt("Luminance", &fx_luminance, 0, 100); c |= ImGui::SliderInt("Blur", &fx_blur, 0, 20); c |= ImGui::ColorEdit3("Color", fx_color2, ImGuiColorEditFlags_NoInputs); return c; }, [&](Image& img) { effects::pencil(img, fx_luminance, fx_blur, float_rgb(fx_color2)); });
    adjust_modal(*this, "User Defined Filter",
        [&] {
            bool c = false;
            for (int j = 0; j < 5; ++j) {
                for (int i = 0; i < 5; ++i) {
                    ImGui::PushID(j * 5 + i);
                    ImGui::SetNextItemWidth(48);
                    c |= ImGui::InputFloat("##k", &fx_kernel[j * 5 + i], 0.0f, 0.0f, "%.0f");
                    ImGui::PopID();
                    if (i < 4) ImGui::SameLine();
                }
            }
            ImGui::SetNextItemWidth(100); c |= ImGui::InputFloat("Divisor", &fx_divisor, 0.0f, 0.0f, "%.1f");
            ImGui::SameLine(); ImGui::SetNextItemWidth(100); c |= ImGui::InputFloat("Bias", &fx_bias, 0.0f, 0.0f, "%.0f");
            if (ImGui::SmallButton("Compute divisor")) { float sum = 0; for (float k : fx_kernel) sum += k; fx_divisor = sum != 0.0f ? sum : 1.0f; c = true; }
            return c;
        },
        [&](Image& img) { effects::user_defined_filter(img, fx_kernel, fx_divisor, fx_bias); });

    adjust_modal(*this, "Kaleidoscope",
        [&] {
            bool c = ImGui::SliderInt("Petals", &kal_petals, 2, 32);
            c |= ImGui::SliderFloat("Rotation", &kal_angle, 0.0f, 359.0f, "%.0f");
            c |= ImGui::SliderFloat("Radius", &kal_radius, 1.0f, 100.0f, "%.0f%%");
            return c;
        },
        [&](Image& img) { effects::kaleidoscope(img, kal_petals, kal_angle, kal_radius); });

    adjust_modal(*this, "Sunburst",
        [&] {
            bool c = ImGui::SliderFloat("Horizontal position", &sun_x, 0.0f, 1.0f, "%.2f");
            c |= ImGui::SliderFloat("Vertical position", &sun_y, 0.0f, 1.0f, "%.2f");
            c |= ImGui::SliderFloat("Brightness", &sun_brightness, 0.0f, 2.0f, "%.2f");
            c |= ImGui::SliderInt("Rays", &sun_rays, 0, 64);
            c |= ImGui::SliderFloat("Ray brightness", &sun_ray_brightness, 0.0f, 2.0f, "%.2f");
            c |= ImGui::ColorEdit3("Color", sun_color, ImGuiColorEditFlags_NoInputs);
            return c;
        },
        [&](Image& img) {
            auto c8 = [](float f) { return static_cast<uint8_t>(f * 255.0f + 0.5f); };
            effects::sunburst(img, sun_x, sun_y, sun_brightness, sun_rays, sun_ray_brightness, {c8(sun_color[0]), c8(sun_color[1]), c8(sun_color[2]), 255});
        });
}
