// Adjustment and effect dialogs with live preview. Each dialog edits its
// parameters, the preview session re-applies the operation to the active
// layer (clipped to the selection), and OK records one history entry.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "App.h"
#include "BackgroundJob.h"
#include "ui/EffectState.h"
#include "AdjustState.h"
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

// Applies the dialog's operation at full resolution on a worker thread and
// commits it when it lands. Returns false when it is not worth the trouble
// (a small layer, or a 16-bit layer, whose path re-runs the operation on the
// deep data and is left alone), in which case the caller does it inline.
bool App::preview_commit_async(std::function<void(Image&)> op, const std::function<void(Image16&)>& op16) {
    if (!preview.active || !doc || job) return false;
    Layer& L = doc->layer(preview.layer);
    if (L.is_deep() && op16) return false;
    // Below a few megapixels the work is over before a modal could usefully
    // appear, and going through a thread would only add a flicker.
    if (static_cast<size_t>(preview.before.width()) * preview.before.height() < 3u * 1000u * 1000u) return false;

    const std::string name = preview.name;
    const size_t layer = preview.layer;
    const Image before = preview.before;
    // Leave the approximate preview on screen while the exact one computes.
    preview = Preview{};

    job = std::make_unique<BackgroundJob>();
    job->name = name;
    job->layer = layer;
    job->result = before;
    job->cancellable = false;   // an effect has no progress to report or safe point to stop at
    BackgroundJob* j = job.get();
    j->done = std::async(std::launch::async, [j, op = std::move(op)]() mutable {
        op(j->result);
        return true;
    });
    status = name + "...";
    return true;
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
        // The exact, full-resolution apply. On a big layer this is seconds of
        // work, and running it here froze the window with nothing on screen
        // to say why, so it goes to a worker with the usual progress modal.
        // Edit > Repeat is recorded either way.
        app.last_effect = title;
        app.last_effect_op = std::function<void(Image&)>(op);
        app.last_effect_op16 = op16;
        if (app.preview_commit_async(std::function<void(Image&)>(op), op16)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
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
        app.effect_state->curve_drag = nearest();
        if (app.effect_state->curve_drag < 0) {
            pts.push_back(from_screen(mouse));
            std::sort(pts.begin(), pts.end());
            app.effect_state->curve_drag = static_cast<int>(std::find(pts.begin(), pts.end(), pts.back()) - pts.begin());
            for (size_t i = 0; i < pts.size(); ++i) if (pts[i] == from_screen(mouse)) app.effect_state->curve_drag = static_cast<int>(i);
            changed = true;
        }
    }
    if (app.effect_state->curve_drag >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        auto p = from_screen(mouse);
        // Keep endpoints on the edges and points ordered.
        if (app.effect_state->curve_drag == 0) p.first = 0;
        else if (app.effect_state->curve_drag == static_cast<int>(pts.size()) - 1) p.first = 255;
        else p.first = std::clamp(p.first, pts[app.effect_state->curve_drag - 1].first + 1, pts[app.effect_state->curve_drag + 1].first - 1);
        if (pts[app.effect_state->curve_drag] != p) { pts[app.effect_state->curve_drag] = p; changed = true; }
    } else {
        app.effect_state->curve_drag = -1;
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
        dl->AddCircleFilled(to_screen(pts[i].first, pts[i].second), 4.0f, static_cast<int>(i) == app.effect_state->curve_drag ? IM_COL32(255, 200, 0, 255) : IM_COL32(120, 180, 255, 255));
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
                                    "Clarify", "Black and White Points", "Histogram Adjustment", "Salt and Pepper Filter", "Edge Preserving Smooth",
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
                                    "User Defined Filter", "Color to Alpha"};

effects::Edge App::edge_setting() const {
    return effects::Edge{effect_state->edge_mode, Color{static_cast<uint8_t>(effect_state->edge_color[0] * 255 + 0.5f), static_cast<uint8_t>(effect_state->edge_color[1] * 255 + 0.5f), static_cast<uint8_t>(effect_state->edge_color[2] * 255 + 0.5f), 255}};
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
    if (!effect_state) effect_state = std::make_unique<EffectState>();
    if (!adjust_state) adjust_state = std::make_unique<AdjustState>();

    if (open_adjust != Adj::None) {
        if (doc && active_layer() >= 0) ImGui::OpenPopup(kTitles[static_cast<int>(open_adjust)]);
        open_adjust = Adj::None;
    }

    adjust_modal(*this, "Brightness/Contrast",
        [&] { bool c = ImGui::SliderInt("Brightness", &adjust_state->bc_brightness, -255, 255); c |= ImGui::SliderInt("Contrast", &adjust_state->bc_contrast, -100, 100); return c; },
        [&](Image& img) { adjust::apply_lut(img, adjust::brightness_contrast_lut(adjust_state->bc_brightness, adjust_state->bc_contrast)); },
        [&](Image16& img) { raster16::brightness_contrast(img, adjust_state->bc_brightness, adjust_state->bc_contrast); });

    adjust_modal(*this, "Color to Alpha",
        [&] {
            bool c = ImGui::ColorEdit3("Color", adjust_state->cta_color);
            ImGui::SameLine();
            if (ImGui::SmallButton("Background")) { for (int i = 0; i < 3; ++i) adjust_state->cta_color[i] = bg_color[i]; c = true; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Foreground")) { for (int i = 0; i < 3; ++i) adjust_state->cta_color[i] = fg_color[i]; c = true; }
            c |= ImGui::SliderFloat("Transparency threshold", &adjust_state->cta_transparency, 0.0f, 1.0f, "%.2f");
            c |= ImGui::SliderFloat("Opacity threshold", &adjust_state->cta_opacity, 0.0f, 1.0f, "%.2f");
            ImGui::TextDisabled("The color becomes transparent; other pixels keep what the color cannot explain.");
            return c;
        },
        [&](Image& img) { raster::color_to_alpha(img, float_rgb(adjust_state->cta_color), adjust_state->cta_transparency, adjust_state->cta_opacity); });

    adjust_modal(*this, "Curves",
        [&] {
            bool c = curve_editor(*this, ImVec2(256, 256));
            ImGui::TextDisabled("Click to add a point, drag to move, right-click to remove.");
            if (ImGui::SmallButton("Reset")) { curve_points = {{0.0f, 0.0f}, {255.0f, 255.0f}}; c = true; }
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
        [&](Image16& img) { raster16::gamma(img, gamma_link ? effect_state->gamma_value : gamma_rgb[0], gamma_link ? effect_state->gamma_value : gamma_rgb[1], gamma_link ? effect_state->gamma_value : gamma_rgb[2]); });

    adjust_modal(*this, "Levels",
        [&] {
            draw_histogram(preview.histogram, ImVec2(256, 80));
            bool c = false;
            ImGui::TextUnformatted("Input levels");
            c |= ImGui::SliderInt("Low##in", &effect_state->lv_in_lo, 0, 254);
            c |= ImGui::SliderFloat("Gamma", &effect_state->lv_gamma, 0.1f, 5.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            c |= ImGui::SliderInt("High##in", &effect_state->lv_in_hi, 1, 255);
            if (effect_state->lv_in_hi <= effect_state->lv_in_lo) effect_state->lv_in_hi = effect_state->lv_in_lo + 1;
            ImGui::TextUnformatted("Output levels");
            c |= ImGui::SliderInt("Low##out", &effect_state->lv_out_lo, 0, 255);
            c |= ImGui::SliderInt("High##out", &effect_state->lv_out_hi, 0, 255);
            if (ImGui::SmallButton("Reset")) { effect_state->lv_in_lo = 0; effect_state->lv_in_hi = 255; effect_state->lv_gamma = 1; effect_state->lv_out_lo = 0; effect_state->lv_out_hi = 255; c = true; }
            return c;
        },
        [&](Image& img) { adjust::apply_lut(img, adjust::levels_lut(effect_state->lv_in_lo, effect_state->lv_gamma, effect_state->lv_in_hi, effect_state->lv_out_lo, effect_state->lv_out_hi)); },
        [&](Image16& img) { raster16::levels(img, effect_state->lv_in_lo, effect_state->lv_gamma, effect_state->lv_in_hi, effect_state->lv_out_lo, effect_state->lv_out_hi); });

    adjust_modal(*this, "Threshold",
        [&] { draw_histogram(preview.histogram, ImVec2(256, 60)); return ImGui::SliderInt("Threshold", &effect_state->threshold_value, 1, 255); },
        [&](Image& img) { adjust::grayscale_then_threshold(img, effect_state->threshold_value); },
        [&](Image16& img) { raster16::threshold(img, effect_state->threshold_value); });

    adjust_modal(*this, "Channel Mixer",
        [&] {
            bool c = ImGui::Checkbox("Monochrome", &mixer.monochrome);
            if (!mixer.monochrome) { ImGui::SameLine(); ImGui::SetNextItemWidth(100); ImGui::Combo("Output channel", &effect_state->mixer_row, "Red\0Green\0Blue\0"); }
            const int row = mixer.monochrome ? 0 : effect_state->mixer_row;
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
            bool c = ImGui::SliderInt("Hue", &effect_state->hsl_h, -180, 180);
            c |= ImGui::SliderInt("Saturation", &effect_state->hsl_s, -100, 100);
            c |= ImGui::SliderInt("Lightness", &effect_state->hsl_l, -100, 100);
            return c;
        },
        [&](Image& img) { adjust::hsl_adjust(img, effect_state->hsl_h, effect_state->hsl_s, effect_state->hsl_l); },
        [&](Image16& img) { raster16::hsl_adjust(img, effect_state->hsl_h, effect_state->hsl_s, effect_state->hsl_l); });

    adjust_modal(*this, "Average",
        [&] { return ImGui::SliderInt("Radius", &effect_state->box_radius, 1, 50); },
        [&](Image& img) { raster::box_blur(img, effect_state->box_radius); });

    adjust_modal(*this, "Gaussian Blur",
        [&] { return ImGui::SliderFloat("Radius", &blur_radius, 0.1f, 100.0f, "%.1f", ImGuiSliderFlags_Logarithmic); },
        [&](Image& img) { raster::gaussian_blur(img, blur_radius); },
        [&](Image16& img) { raster16::gaussian_blur(img, blur_radius); });

    adjust_modal(*this, "Posterize",
        [&] { return ImGui::SliderInt("Levels", &effect_state->posterize_levels, 2, 255, "%d", ImGuiSliderFlags_Logarithmic); },
        [&](Image& img) { adjust::apply_lut(img, adjust::posterize_lut(effect_state->posterize_levels)); },
        [&](Image16& img) { raster16::posterize(img, effect_state->posterize_levels); });

    adjust_modal(*this, "Solarize",
        [&] { return ImGui::SliderInt("Threshold", &effect_state->solarize_threshold, 1, 254); },
        [&](Image& img) { adjust::apply_lut(img, adjust::solarize_lut(effect_state->solarize_threshold)); });

    adjust_modal(*this, "Unsharp Mask",
        [&] {
            bool c = ImGui::SliderFloat("Radius", &effect_state->usm_radius, 0.1f, 50.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
            c |= ImGui::SliderInt("Strength", &effect_state->usm_strength, 1, 500, "%d%%");
            c |= ImGui::SliderInt("Clipping", &effect_state->usm_clipping, 0, 100);
            return c;
        },
        [&](Image& img) { effects::unsharp_mask(img, effect_state->usm_radius, effect_state->usm_strength, effect_state->usm_clipping); });

    adjust_modal(*this, "Median",
        [&] { return ImGui::SliderInt("Filter aperture", &effect_state->median_radius, 1, 10, "%d px radius"); },
        [&](Image& img) { effects::median(img, effect_state->median_radius); });

    adjust_modal(*this, "Motion Blur",
        [&] {
            bool c = ImGui::SliderFloat("Angle", &effect_state->motion_angle, 0.0f, 359.0f, "%.0f");
            c |= ImGui::SliderInt("Strength", &effect_state->motion_strength, 1, 100, "%d px");
            return c;
        },
        [&](Image& img) { effects::motion_blur(img, effect_state->motion_angle, effect_state->motion_strength); });

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
        [&] { return ImGui::SliderInt("Amount to age", &effect_state->sepia_amount, 1, 100); },
        [&](Image& img) { adjust::sepia(img, effect_state->sepia_amount); });

    adjust_modal(*this, "Hue Map",
        [&] {
            bool c = false;
            static const char* bands[10] = {"Red", "Orange", "Yellow", "Chartreuse", "Green", "Spring", "Cyan", "Azure", "Blue", "Magenta"};
            for (int i = 0; i < 10; ++i) {
                ImGui::PushID(i);
                ImGui::SetNextItemWidth(220);
                c |= ImGui::SliderInt(bands[i], &effect_state->hue_map_params.shift[i], -180, 180);
                ImGui::PopID();
            }
            ImGui::Separator();
            c |= ImGui::SliderInt("Saturation shift", &effect_state->hue_map_params.saturation, -100, 100);
            c |= ImGui::SliderInt("Lightness shift", &effect_state->hue_map_params.lightness, -100, 100);
            if (ImGui::SmallButton("Reset")) { effect_state->hue_map_params = adjust::HueMap{}; c = true; }
            return c;
        },
        [&](Image& img) { adjust::hue_map(img, effect_state->hue_map_params); });

    adjust_modal(*this, "Wave",
        [&] {
            bool c = ImGui::SliderFloat("Horizontal amplitude", &effect_state->wave_ha, 0.0f, 100.0f, "%.0f");
            c |= ImGui::SliderFloat("Horizontal wavelength", &effect_state->wave_hw, 1.0f, 500.0f, "%.0f");
            c |= ImGui::SliderFloat("Vertical amplitude", &effect_state->wave_va, 0.0f, 100.0f, "%.0f");
            c |= ImGui::SliderFloat("Vertical wavelength", &effect_state->wave_vw, 1.0f, 500.0f, "%.0f");
            return c;
        },
        [&](Image& img) { effects::wave(img, effect_state->wave_ha, effect_state->wave_hw, effect_state->wave_va, effect_state->wave_vw); });

    adjust_modal(*this, "Pinch",
        [&] { return ImGui::SliderInt("Strength (negative = punch)", &effect_state->pinch_strength, -100, 100); },
        [&](Image& img) { effects::pinch(img, effect_state->pinch_strength); });

    adjust_modal(*this, "Twirl",
        [&] { return ImGui::SliderFloat("Degrees", &effect_state->twirl_degrees, -720.0f, 720.0f, "%.0f"); },
        [&](Image& img) { effects::twirl(img, effect_state->twirl_degrees); });

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
            bool c = ImGui::SliderInt("Vertical offset", &effect_state->cutout_y, -100, 100);
            c |= ImGui::SliderInt("Horizontal offset", &effect_state->cutout_x, -100, 100);
            float op = effect_state->cutout_opacity * 100.0f;
            if (ImGui::SliderFloat("Opacity", &op, 0.0f, 100.0f, "%.0f%%")) { effect_state->cutout_opacity = op / 100.0f; c = true; }
            c |= ImGui::SliderFloat("Blur", &effect_state->cutout_blur, 0.0f, 100.0f, "%.1f");
            c |= ImGui::ColorEdit3("Shadow color", effect_state->cutout_color, ImGuiColorEditFlags_NoInputs);
            ImGui::TextDisabled(doc && doc->has_selection() ? "Cut into the selection." : "Cut into the layer's opaque area.");
            return c;
        },
        [&](Image& img) {
            auto c8 = [](float f) { return static_cast<uint8_t>(f * 255.0f + 0.5f); };
            effects::cutout(img, doc && doc->has_selection() ? doc->selection().data() : nullptr, effect_state->cutout_x, effect_state->cutout_y, effect_state->cutout_opacity, effect_state->cutout_blur,
                            {c8(effect_state->cutout_color[0]), c8(effect_state->cutout_color[1]), c8(effect_state->cutout_color[2]), 255});
        });

    adjust_modal(*this, "Ripple",
        [&] {
            bool c = ImGui::SliderFloat("Amplitude", &effect_state->ripple_amp, 0.0f, 100.0f, "%.0f");
            c |= ImGui::SliderFloat("Wavelength", &effect_state->ripple_wave, 2.0f, 300.0f, "%.0f");
            return c;
        },
        [&](Image& img) { effects::ripple(img, effect_state->ripple_amp, effect_state->ripple_wave); });

    adjust_modal(*this, "Spherize",
        [&] { return ImGui::SliderInt("Strength (negative = dish)", &effect_state->spherize_strength, -100, 100); },
        [&](Image& img) { effects::spherize(img, effect_state->spherize_strength); });

    adjust_modal(*this, "Lens Distortion",
        [&] { return ImGui::SliderInt("Strength (negative = pincushion)", &effect_state->lens_strength, -100, 100); },
        [&](Image& img) { effects::lens_distortion(img, effect_state->lens_strength); });

    adjust_modal(*this, "Halftone",
        [&] {
            bool c = ImGui::SliderInt("Cell size", &effect_state->halftone_cell, 2, 50);
            c |= ImGui::SliderFloat("Angle", &effect_state->halftone_angle, 0.0f, 90.0f, "%.0f");
            c |= ImGui::ColorEdit3("Ink", effect_state->halftone_ink, ImGuiColorEditFlags_NoInputs);
            ImGui::SameLine();
            c |= ImGui::ColorEdit3("Paper", effect_state->halftone_paper, ImGuiColorEditFlags_NoInputs);
            return c;
        },
        [&](Image& img) {
            auto c8 = [](float f) { return static_cast<uint8_t>(f * 255.0f + 0.5f); };
            effects::halftone(img, effect_state->halftone_cell, effect_state->halftone_angle, {c8(effect_state->halftone_ink[0]), c8(effect_state->halftone_ink[1]), c8(effect_state->halftone_ink[2]), 255},
                              {c8(effect_state->halftone_paper[0]), c8(effect_state->halftone_paper[1]), c8(effect_state->halftone_paper[2]), 255});
        });

    adjust_modal(*this, "Chrome",
        [&] {
            bool c = ImGui::SliderInt("Flaws (bands)", &effect_state->chrome_bands, 1, 20);
            c |= ImGui::SliderFloat("Brightness", &effect_state->chrome_brightness, 0.2f, 2.0f, "%.2f");
            return c;
        },
        [&](Image& img) { effects::chrome(img, effect_state->chrome_bands, effect_state->chrome_brightness); });

    adjust_modal(*this, "Outer Bevel",
        [&] {
            bool c = ImGui::SliderInt("Width", &effect_state->obevel_width, 1, 100);
            c |= ImGui::SliderFloat("Light angle", &effect_state->obevel_angle, 0.0f, 359.0f, "%.0f");
            c |= ImGui::SliderFloat("Depth", &effect_state->obevel_depth, 0.1f, 3.0f, "%.1f");
            c |= ImGui::ColorEdit3("Color", effect_state->obevel_color, ImGuiColorEditFlags_NoInputs);
            ImGui::TextDisabled(doc && doc->has_selection() ? "Raised around the selection." : "Raised around the layer's opaque area.");
            return c;
        },
        [&](Image& img) {
            auto c8 = [](float f) { return static_cast<uint8_t>(f * 255.0f + 0.5f); };
            effects::outer_bevel(img, doc && doc->has_selection() ? doc->selection().data() : nullptr, effect_state->obevel_width, effect_state->obevel_angle, effect_state->obevel_depth,
                                 {c8(effect_state->obevel_color[0]), c8(effect_state->obevel_color[1]), c8(effect_state->obevel_color[2]), 255});
        });

    adjust_modal(*this, "Fade Correction",
        [&] { return ImGui::SliderInt("Amount of correction", &effect_state->fade_amount, 1, 100); },
        [&](Image& img) { adjust::fade_correction(img, effect_state->fade_amount); });

    adjust_modal(*this, "Automatic Color Balance",
        [&] {
            bool c = ImGui::SliderInt("Strength", &adjust_state->acb_strength, 0, 100);
            c |= ImGui::SliderInt("Illuminant temperature (K)", &adjust_state->acb_temperature, 2000, 12000);
            c |= ImGui::Checkbox("Remove color cast", &adjust_state->acb_remove_cast);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Neutralizes an overall cast. Leave it off for a picture whose colors are meant to be strong.");
            return c;
        },
        [&](Image& img) { photo::auto_color_balance(img, adjust_state->acb_strength, adjust_state->acb_temperature, adjust_state->acb_remove_cast); });
    adjust_modal(*this, "Automatic Contrast Enhancement",
        [&] { bool c = ImGui::Combo("Bias", &adjust_state->ace_bias, "Lighter\0Neutral\0Darker\0"); c |= ImGui::Combo("Strength", &adjust_state->ace_strength, "Normal\0Mild\0"); c |= ImGui::Combo("Appearance", &adjust_state->ace_appearance, "Flat\0Natural\0Bold\0"); return c; },
        [&](Image& img) { photo::auto_contrast_enhance(img, adjust_state->ace_bias, adjust_state->ace_strength, adjust_state->ace_appearance); });
    adjust_modal(*this, "Automatic Saturation Enhancement",
        [&] { bool c = ImGui::Combo("Bias", &adjust_state->ase_bias, "Less colorful\0Normal\0More colorful\0"); c |= ImGui::Combo("Strength", &adjust_state->ase_strength, "Weak\0Normal\0Strong\0"); c |= ImGui::Checkbox("Skin tones present", &adjust_state->ase_skin); return c; },
        [&](Image& img) { photo::auto_saturation(img, adjust_state->ase_bias, adjust_state->ase_strength, adjust_state->ase_skin); });
    adjust_modal(*this, "Clarify",
        [&] { return ImGui::SliderInt("Strength of effect", &adjust_state->clarify_strength, 1, 5); },
        [&](Image& img) { photo::clarify(img, adjust_state->clarify_strength); });
    adjust_modal(*this, "Black and White Points",
        [&] {
            bool c = ImGui::ColorEdit3("Source black", effect_state->bwp_src_black, ImGuiColorEditFlags_NoInputs); ImGui::SameLine(); c |= ImGui::ColorEdit3("Destination black", effect_state->bwp_dst_black, ImGuiColorEditFlags_NoInputs);
            c |= ImGui::ColorEdit3("Source white", effect_state->bwp_src_white, ImGuiColorEditFlags_NoInputs); ImGui::SameLine(); c |= ImGui::ColorEdit3("Destination white", effect_state->bwp_dst_white, ImGuiColorEditFlags_NoInputs);
            ImGui::TextDisabled("Pick the darkest and lightest colors that should become the destination black and white.");
            return c;
        },
        [&](Image& img) {
            photo::black_white_points(img, float_rgb(effect_state->bwp_src_black), float_rgb(effect_state->bwp_src_white), float_rgb(effect_state->bwp_dst_black), float_rgb(effect_state->bwp_dst_white));
        });
    adjust_modal(*this, "Histogram Adjustment",
        [&] {
            draw_histogram(preview.histogram, ImVec2(256, 80));
            bool c = ImGui::Combo("Edit", &adjust_state->ha_channel, "Luminance\0Red\0Green\0Blue\0");
            c |= ImGui::SliderFloat("Low clip %", &adjust_state->ha_low, 0.0f, 50.0f, "%.2f");
            c |= ImGui::SliderFloat("High clip %", &adjust_state->ha_high, 0.0f, 50.0f, "%.2f");
            c |= ImGui::SliderFloat("Gamma", &adjust_state->ha_gamma, 0.1f, 7.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            c |= ImGui::SliderInt("Midtones (compress / expand)", &adjust_state->ha_midtones, -100, 100);
            return c;
        },
        [&](Image& img) { photo::histogram_adjust(img, adjust_state->ha_low, adjust_state->ha_high, adjust_state->ha_gamma, adjust_state->ha_midtones, adjust_state->ha_channel); });
    adjust_modal(*this, "Edge Preserving Smooth",
        [&] { return ImGui::SliderInt("Smoothing", &effect_state->edge_smooth_amount, 1, 100); },
        [&](Image& img) { photo::edge_preserving_smooth(img, effect_state->edge_smooth_amount); });
    adjust_modal(*this, "Salt and Pepper Filter",
        [&] { bool c = ImGui::SliderInt("Speck size", &adjust_state->sp_size, 3, 9); c |= ImGui::SliderInt("Sensitivity to specks", &adjust_state->sp_sensitivity, 1, 30); c |= ImGui::Checkbox("Include all lower speck sizes", &adjust_state->sp_smaller); c |= ImGui::Checkbox("Aggressive action", &adjust_state->sp_aggressive); return c; },
        [&](Image& img) { photo::salt_and_pepper(img, adjust_state->sp_size, adjust_state->sp_sensitivity, adjust_state->sp_smaller, adjust_state->sp_aggressive); });
    adjust_modal(*this, "JPEG Artifact Removal",
        [&] { bool c = ImGui::Combo("Strength", &effect_state->jpeg_strength, "Low\0Normal\0High\0Maximum\0"); c |= ImGui::SliderInt("Crispness", &effect_state->jpeg_crispness, 0, 100); return c; },
        [&](Image& img) { photo::jpeg_artifact_removal(img, effect_state->jpeg_strength, effect_state->jpeg_crispness); });
    adjust_modal(*this, "Fill Flash",
        [&] { return ImGui::SliderInt("Strength", &adjust_state->flash_strength, 0, 100); },
        [&](Image& img) { photo::fill_flash(img, adjust_state->flash_strength); });
    adjust_modal(*this, "Backlighting",
        [&] { return ImGui::SliderInt("Strength", &adjust_state->backlight_strength, 0, 100); },
        [&](Image& img) { photo::backlighting(img, adjust_state->backlight_strength); });
    adjust_modal(*this, "Chromatic Aberration Removal",
        [&] { bool c = ImGui::SliderFloat("Red fringe (px at corners)", &effect_state->ca_red, -20.0f, 20.0f, "%.1f"); c |= ImGui::SliderFloat("Blue fringe (px at corners)", &effect_state->ca_blue, -20.0f, 20.0f, "%.1f"); return c; },
        [&](Image& img) { photo::chromatic_aberration(img, effect_state->ca_red, effect_state->ca_blue); });
    adjust_modal(*this, "Digital Camera Noise Removal",
        [&] { bool c = ImGui::SliderInt("Strength", &effect_state->nr_strength, 0, 100); c |= ImGui::SliderInt("Correction blend %", &effect_state->nr_blend, 0, 100); c |= ImGui::SliderInt("Sharpening %", &effect_state->nr_sharpen, 0, 100); return c; },
        [&](Image& img) { photo::noise_removal(img, effect_state->nr_strength, effect_state->nr_blend, effect_state->nr_sharpen); });

    auto edge_options = [&] {
        bool c = ImGui::Combo("Edge mode", &effect_state->edge_mode, "Wrap\0Repeat\0Color\0Transparent\0");
        if (effect_state->edge_mode == 2) { ImGui::SameLine(); c |= ImGui::ColorEdit3("##edgecol", effect_state->edge_color, ImGuiColorEditFlags_NoInputs); }
        return c;
    };
    // Operations may run after this function returns (the Effect Browser
    // keeps them), so they call members (edge_setting, float_rgb), never locals.
    adjust_modal(*this, "Curlicues",
        [&] { bool c = ImGui::SliderInt("Columns", &effect_state->curl_cols, 1, 20); c |= ImGui::SliderInt("Rows", &effect_state->curl_rows, 1, 20); c |= ImGui::SliderInt("Radius", &effect_state->curl_radius, 1, 100); c |= ImGui::SliderInt("Strength", &effect_state->curl_strength, -100, 100); return c; },
        [&](Image& img) { effects::curlicues(img, effect_state->curl_cols, effect_state->curl_rows, effect_state->curl_radius, effect_state->curl_strength); });
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
        [&] { bool c = ImGui::SliderFloat("Radius %", &effect_state->halo_radius, 1.0f, 200.0f, "%.0f"); c |= ImGui::SliderInt("Spikes", &effect_state->halo_spikes, 1, 100); c |= ImGui::SliderFloat("Radius offset %", &effect_state->halo_offset, 0.0f, 100.0f, "%.0f"); c |= ImGui::SliderInt("Bend", &effect_state->halo_bend, -100, 100); return c; },
        [&](Image& img) { effects::spiky_halo(img, effect_state->halo_radius, effect_state->halo_spikes, effect_state->halo_offset, effect_state->halo_bend); });
    adjust_modal(*this, "Warp",
        [&] { bool c = ImGui::SliderFloat("Center X %", &effect_state->warp_cx, 0.0f, 100.0f, "%.0f"); c |= ImGui::SliderFloat("Center Y %", &effect_state->warp_cy, 0.0f, 100.0f, "%.0f"); c |= ImGui::SliderFloat("Size %", &effect_state->warp_size, 1.0f, 100.0f, "%.0f"); c |= ImGui::SliderInt("Strength", &effect_state->warp_strength_fx, -100, 100); return c; },
        [&](Image& img) { effects::warp(img, effect_state->warp_cx, effect_state->warp_cy, effect_state->warp_size, effect_state->warp_strength_fx); });
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
        [&] { bool c = ImGui::SliderFloat("Angle", &effect_state->pat_angle, 0.0f, 360.0f, "%.0f"); c |= ImGui::SliderFloat("Center X %", &effect_state->pat_cx, 0.0f, 100.0f, "%.0f"); c |= ImGui::SliderFloat("Center Y %", &effect_state->pat_cy, 0.0f, 100.0f, "%.0f"); c |= ImGui::SliderFloat("Scale %", &effect_state->pat_scale, 1.0f, 100.0f, "%.0f"); c |= ImGui::SliderInt("Rotation", &effect_state->pat_rotation, 0, 360); return c; },
        [&](Image& img) { effects::pattern(img, effect_state->pat_angle, effect_state->pat_cx, effect_state->pat_cy, effect_state->pat_scale, effect_state->pat_rotation); });
    adjust_modal(*this, "Rotating Mirror",
        [&] { bool c = ImGui::SliderFloat("Angle", &effect_state->mirror_angle, 0.0f, 360.0f, "%.0f"); c |= ImGui::SliderFloat("Center X %", &effect_state->mirror_cx, 0.0f, 100.0f, "%.0f"); c |= ImGui::SliderFloat("Center Y %", &effect_state->mirror_cy, 0.0f, 100.0f, "%.0f"); c |= edge_options(); return c; },
        [&](Image& img) { effects::rotating_mirror(img, effect_state->mirror_angle, effect_state->mirror_cx, effect_state->mirror_cy, edge_setting()); });
    adjust_modal(*this, "Offset",
        [&] { bool c = ImGui::SliderInt("Horizontal", &effect_state->offset_x, -2000, 2000); c |= ImGui::SliderInt("Vertical", &effect_state->offset_y, -2000, 2000); c |= edge_options(); return c; },
        [&](Image& img) { effects::offset(img, effect_state->offset_x, effect_state->offset_y, edge_setting()); });
    adjust_modal(*this, "Seamless Tiling",
        [&] { bool c = ImGui::Combo("Method", &effect_state->tile_method, "Edge\0Corner\0Mirror\0"); c |= ImGui::Combo("Direction", &effect_state->tile_direction, "Bidirectional\0Horizontal\0Vertical\0"); c |= ImGui::SliderInt("Transition", &effect_state->tile_transition, 0, 100); return c; },
        [&](Image& img) { effects::seamless_tiling(img, effect_state->tile_method, effect_state->tile_direction, effect_state->tile_transition); });
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

    auto angle_color = [&] { bool c = ImGui::SliderFloat("Angle", &adjust_state->fx_angle, 0.0f, 360.0f, "%.0f"); ImGui::SameLine(); c |= ImGui::ColorEdit3("Color", adjust_state->fx_color, ImGuiColorEditFlags_NoInputs); return c; };
    auto blur_detail = [&] { bool c = ImGui::SliderInt("Blur", &adjust_state->fx_blur, 0, 50); c |= ImGui::SliderInt("Detail", &adjust_state->fx_detail, 1, 100); return c; };
    adjust_modal(*this, "Aged Newspaper", [&] { return ImGui::SliderInt("Amount to age", &adjust_state->fx_amount, 1, 100); }, [&](Image& img) { effects::aged_newspaper(img, adjust_state->fx_amount); });
    adjust_modal(*this, "Balls and Bubbles",
        [&] { bool c = ImGui::SliderInt("Count", &adjust_state->fx_count, 1, 300); c |= ImGui::SliderInt("Minimum size", &adjust_state->fx_min, 2, 300); c |= ImGui::SliderInt("Maximum size", &adjust_state->fx_max, 2, 400); c |= ImGui::SliderInt("Opacity", &adjust_state->fx_opacity, 0, 100); c |= ImGui::Checkbox("Bubbles (else balls)", &adjust_state->fx_bubbles); ImGui::SameLine(); c |= ImGui::ColorEdit3("Ball color", adjust_state->fx_color, ImGuiColorEditFlags_NoInputs); return c; },
        [&](Image& img) { effects::balls_and_bubbles(img, adjust_state->fx_count, adjust_state->fx_min, adjust_state->fx_max, adjust_state->fx_opacity, adjust_state->fx_bubbles, float_rgb(adjust_state->fx_color), 7); });
    adjust_modal(*this, "Colored Edges", [&] { bool c = ImGui::SliderInt("Luminance", &adjust_state->fx_luminance, 0, 100); c |= ImGui::SliderInt("Blur", &adjust_state->fx_blur, 0, 20); c |= ImGui::ColorEdit3("Color", adjust_state->fx_color, ImGuiColorEditFlags_NoInputs); return c; }, [&](Image& img) { effects::colored_edges(img, adjust_state->fx_luminance, adjust_state->fx_blur, float_rgb(adjust_state->fx_color)); });
    adjust_modal(*this, "Colored Foil", [&] { bool c = blur_detail(); c |= angle_color(); return c; }, [&](Image& img) { effects::colored_foil(img, adjust_state->fx_blur, adjust_state->fx_detail, float_rgb(adjust_state->fx_color), adjust_state->fx_angle); });
    adjust_modal(*this, "Contours", [&] { bool c = ImGui::SliderInt("Luminance", &adjust_state->fx_luminance, 0, 100); c |= ImGui::SliderInt("Blur", &adjust_state->fx_blur, 0, 20); c |= ImGui::SliderInt("Detail", &adjust_state->fx_detail, 2, 20); c |= ImGui::ColorEdit3("Color", adjust_state->fx_color2, ImGuiColorEditFlags_NoInputs); return c; }, [&](Image& img) { effects::contours(img, adjust_state->fx_luminance, adjust_state->fx_blur, adjust_state->fx_detail, float_rgb(adjust_state->fx_color2)); });
    adjust_modal(*this, "Enamel", [&] { bool c = blur_detail(); c |= ImGui::SliderInt("Density", &adjust_state->fx_density, 0, 100); c |= angle_color(); return c; }, [&](Image& img) { effects::enamel(img, adjust_state->fx_blur, adjust_state->fx_detail, adjust_state->fx_density, adjust_state->fx_angle, float_rgb(adjust_state->fx_color)); });
    adjust_modal(*this, "Glowing Edges", [&] { bool c = ImGui::SliderInt("Intensity", &adjust_state->fx_intensity, 1, 100); c |= ImGui::SliderInt("Sharpness", &adjust_state->fx_sharpness, 1, 100); return c; }, [&](Image& img) { effects::glowing_edges(img, adjust_state->fx_intensity, adjust_state->fx_sharpness); });
    adjust_modal(*this, "Hot Wax Coating", [&] { return ImGui::ColorEdit3("Wax (foreground material)", fg_color, ImGuiColorEditFlags_NoInputs); }, [&](Image& img) { effects::hot_wax(img, float_rgb(fg_color)); });
    adjust_modal(*this, "Magnifying Lens", [&] { bool c = ImGui::SliderFloat("Center X %", &adjust_state->fx_cx, 0, 100, "%.0f"); c |= ImGui::SliderFloat("Center Y %", &adjust_state->fx_cy, 0, 100, "%.0f"); c |= ImGui::SliderFloat("Size %", &adjust_state->fx_size_pct, 1, 100, "%.0f"); c |= ImGui::SliderInt("Refraction", &adjust_state->fx_refraction, 0, 100); c |= ImGui::SliderInt("Shading", &adjust_state->fx_shading, 0, 100); return c; }, [&](Image& img) { effects::magnifying_lens(img, adjust_state->fx_cx, adjust_state->fx_cy, adjust_state->fx_size_pct, adjust_state->fx_refraction, adjust_state->fx_shading); });
    adjust_modal(*this, "Neon Glow", [&] { bool c = ImGui::SliderInt("Detail", &adjust_state->fx_detail, 1, 100); c |= ImGui::SliderInt("Opacity", &adjust_state->fx_opacity, 0, 100); return c; }, [&](Image& img) { effects::neon_glow(img, adjust_state->fx_detail, adjust_state->fx_opacity); });
    adjust_modal(*this, "Topography", [&] { bool c = ImGui::SliderInt("Width", &adjust_state->fx_width, 1, 100); c |= ImGui::SliderInt("Density", &adjust_state->fx_density, 2, 32); c |= angle_color(); return c; }, [&](Image& img) { effects::topography(img, adjust_state->fx_width, adjust_state->fx_density, adjust_state->fx_angle, float_rgb(adjust_state->fx_color)); });
    adjust_modal(*this, "Lights",
        [&] {
            bool c = ImGui::SliderInt("Darkness", &effect_state->fx_darkness, 0, 100);
            if (!effect_state->fx_lights[0].on && !effect_state->fx_lights[1].on && !effect_state->fx_lights[2].on && !effect_state->fx_lights[3].on && !effect_state->fx_lights[4].on) { effect_state->fx_lights[0].on = true; c = true; }
            for (int i = 0; i < 5; ++i) {
                ImGui::PushID(i);
                effects::Light& L = effect_state->fx_lights[i];
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
        [&](Image& img) { effects::lights(img, effect_state->fx_lights, 5, effect_state->fx_darkness); });
    adjust_modal(*this, "Blinds", [&] { bool c = ImGui::SliderInt("Width", &adjust_state->fx_width, 2, 200); c |= ImGui::SliderInt("Opacity", &adjust_state->fx_opacity, 0, 100); c |= ImGui::Checkbox("Horizontal", &adjust_state->fx_horizontal); ImGui::SameLine(); c |= ImGui::Checkbox("Light from left/top", &adjust_state->fx_from_left); ImGui::SameLine(); c |= ImGui::ColorEdit3("Color", adjust_state->fx_color2, ImGuiColorEditFlags_NoInputs); return c; }, [&](Image& img) { effects::blinds(img, adjust_state->fx_width, adjust_state->fx_opacity, adjust_state->fx_horizontal, adjust_state->fx_from_left, float_rgb(adjust_state->fx_color2)); });
    adjust_modal(*this, "Fine Leather", [&] { bool c = ImGui::SliderInt("Color amount", &adjust_state->fx_amount, 0, 100); c |= ImGui::SliderInt("Blur", &adjust_state->fx_blur, 0, 20); c |= ImGui::SliderInt("Transparency", &adjust_state->fx_opacity, 0, 100); c |= angle_color(); return c; }, [&](Image& img) { effects::leather(img, false, adjust_state->fx_amount, adjust_state->fx_angle, adjust_state->fx_blur, adjust_state->fx_opacity, float_rgb(adjust_state->fx_color), 3); });
    adjust_modal(*this, "Rough Leather", [&] { bool c = ImGui::SliderInt("Color amount", &adjust_state->fx_amount, 0, 100); c |= ImGui::SliderInt("Blur", &adjust_state->fx_blur, 0, 20); c |= ImGui::SliderInt("Transparency", &adjust_state->fx_opacity, 0, 100); c |= angle_color(); return c; }, [&](Image& img) { effects::leather(img, true, adjust_state->fx_amount, adjust_state->fx_angle, adjust_state->fx_blur, adjust_state->fx_opacity, float_rgb(adjust_state->fx_color), 4); });
    adjust_modal(*this, "Fur", [&] { bool c = ImGui::SliderInt("Blur", &adjust_state->fx_blur, 0, 20); c |= ImGui::SliderInt("Density", &adjust_state->fx_density, 1, 100); c |= ImGui::SliderInt("Length", &adjust_state->fx_length, 2, 100); c |= ImGui::SliderInt("Transparency", &adjust_state->fx_opacity, 0, 100); return c; }, [&](Image& img) { effects::fur(img, adjust_state->fx_blur, adjust_state->fx_density, adjust_state->fx_length, adjust_state->fx_opacity, 5); });
    adjust_modal(*this, "Mosaic - Antique", [&] { bool c = ImGui::SliderInt("Columns", &adjust_state->fx_columns, 1, 100); c |= ImGui::SliderInt("Rows", &adjust_state->fx_rows, 1, 100); c |= ImGui::SliderInt("Diffusion", &adjust_state->fx_diffusion, 0, 100); c |= ImGui::SliderInt("Grout width", &adjust_state->fx_grout, 0, 100); c |= ImGui::SliderInt("Grout transparency", &adjust_state->fx_grout_alpha, 0, 100); return c; }, [&](Image& img) { effects::mosaic_antique(img, adjust_state->fx_columns, adjust_state->fx_rows, 0, adjust_state->fx_diffusion, adjust_state->fx_grout, adjust_state->fx_grout_alpha); });
    adjust_modal(*this, "Mosaic - Glass", [&] { bool c = ImGui::SliderInt("Columns", &adjust_state->fx_columns, 1, 100); c |= ImGui::SliderInt("Rows", &adjust_state->fx_rows, 1, 100); c |= ImGui::SliderInt("Curvature", &adjust_state->fx_curvature, 0, 100); c |= ImGui::SliderInt("Edge width", &adjust_state->fx_grout, 0, 100); c |= ImGui::SliderInt("Grout transparency", &adjust_state->fx_grout_alpha, 0, 100); return c; }, [&](Image& img) { effects::mosaic_glass(img, adjust_state->fx_columns, adjust_state->fx_rows, adjust_state->fx_curvature, adjust_state->fx_grout, adjust_state->fx_grout_alpha); });
    adjust_modal(*this, "Polished Stone", [&] { bool c = blur_detail(); c |= ImGui::SliderInt("Color amount", &adjust_state->fx_amount, 0, 100); c |= angle_color(); return c; }, [&](Image& img) { effects::polished_stone(img, adjust_state->fx_blur, adjust_state->fx_detail, adjust_state->fx_angle, adjust_state->fx_amount, float_rgb(adjust_state->fx_color)); });
    adjust_modal(*this, "Sandstone", [&] { bool c = blur_detail(); c |= angle_color(); return c; }, [&](Image& img) { effects::sandstone(img, adjust_state->fx_blur, adjust_state->fx_detail, adjust_state->fx_angle, float_rgb(adjust_state->fx_color), 2); });
    adjust_modal(*this, "Sculpture", [&] { bool c = ImGui::SliderInt("Smoothness", &adjust_state->fx_smooth, 0, 100); c |= ImGui::SliderInt("Depth", &adjust_state->fx_depth, 1, 100); c |= angle_color(); return c; }, [&](Image& img) { effects::sculpture(img, adjust_state->fx_smooth, adjust_state->fx_depth, adjust_state->fx_angle, float_rgb(adjust_state->fx_color)); });
    adjust_modal(*this, "Soft Plastic", [&] { bool c = blur_detail(); c |= ImGui::SliderInt("Density", &adjust_state->fx_density, 0, 100); c |= angle_color(); return c; }, [&](Image& img) { effects::soft_plastic(img, adjust_state->fx_blur, adjust_state->fx_detail, adjust_state->fx_density, adjust_state->fx_angle, float_rgb(adjust_state->fx_color)); });
    adjust_modal(*this, "Straw Wall", [&] { bool c = blur_detail(); c |= ImGui::SliderInt("Density", &adjust_state->fx_density, 1, 100); c |= angle_color(); return c; }, [&](Image& img) { effects::straw_wall(img, adjust_state->fx_blur, adjust_state->fx_detail, adjust_state->fx_density, adjust_state->fx_angle, float_rgb(adjust_state->fx_color), 6); });
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
            c |= ImGui::SliderInt("Size %", &adjust_state->fx_size, 10, 400); c |= ImGui::SliderInt("Smoothness", &adjust_state->fx_smooth, 0, 100); c |= ImGui::SliderInt("Depth", &adjust_state->fx_depth, 1, 100); c |= angle_color();
            return c;
        },
        [&](Image& img) {
            Image bump;
            if (texture_index >= 0 && texture_index < static_cast<int>(textures.size()) && textures[static_cast<size_t>(texture_index)].texture)
                bump = textures[static_cast<size_t>(texture_index)].texture->to_image();
            effects::texture(img, bump, adjust_state->fx_size, adjust_state->fx_smooth, adjust_state->fx_depth, adjust_state->fx_angle, float_rgb(adjust_state->fx_color));
        });
    adjust_modal(*this, "Tiles", [&] { bool c = ImGui::Combo("Shape", &adjust_state->fx_shape, "Square\0Hexagon\0Triangle\0"); c |= ImGui::SliderInt("Size", &adjust_state->fx_size, 4, 200); c |= ImGui::SliderInt("Border", &adjust_state->fx_border, 0, 100); c |= ImGui::SliderInt("Smoothness", &adjust_state->fx_smooth, 0, 100); c |= ImGui::SliderInt("Depth", &adjust_state->fx_depth, 1, 100); c |= angle_color(); return c; }, [&](Image& img) { effects::tiles(img, adjust_state->fx_shape, adjust_state->fx_size, adjust_state->fx_border, adjust_state->fx_smooth, adjust_state->fx_depth, adjust_state->fx_angle, float_rgb(adjust_state->fx_color)); });
    adjust_modal(*this, "Weave", [&] { bool c = ImGui::SliderInt("Gap", &adjust_state->fx_gap, 0, 100); c |= ImGui::SliderInt("Width", &adjust_state->fx_stroke_width, 1, 100); c |= ImGui::SliderInt("Opacity", &adjust_state->fx_opacity, 0, 100); c |= ImGui::ColorEdit3("Gap color", adjust_state->fx_color2, ImGuiColorEditFlags_NoInputs); ImGui::SameLine(); c |= ImGui::ColorEdit3("Weave color", adjust_state->fx_color, ImGuiColorEditFlags_NoInputs); c |= ImGui::Checkbox("Fill gaps", &adjust_state->fx_fill_gaps); return c; }, [&](Image& img) { effects::weave(img, adjust_state->fx_gap, adjust_state->fx_stroke_width, adjust_state->fx_opacity, float_rgb(adjust_state->fx_color2), float_rgb(adjust_state->fx_color), adjust_state->fx_fill_gaps); });
    adjust_modal(*this, "Black Pencil", [&] { bool c = ImGui::SliderInt("Detail", &adjust_state->fx_detail, 1, 100); c |= ImGui::SliderInt("Opacity", &adjust_state->fx_opacity, 0, 100); return c; }, [&](Image& img) { effects::black_pencil(img, adjust_state->fx_detail, adjust_state->fx_opacity); });
    adjust_modal(*this, "Brush Strokes", [&] { bool c = ImGui::SliderInt("Length", &adjust_state->fx_length, 2, 100); c |= ImGui::SliderInt("Density", &adjust_state->fx_density, 1, 100); c |= ImGui::SliderInt("Width", &adjust_state->fx_stroke_width, 1, 30); c |= ImGui::SliderInt("Opacity", &adjust_state->fx_opacity, 0, 100); return c; }, [&](Image& img) { effects::brush_strokes(img, adjust_state->fx_length, adjust_state->fx_density, adjust_state->fx_stroke_width, adjust_state->fx_opacity, 8); });
    adjust_modal(*this, "Charcoal", [&] { bool c = ImGui::SliderInt("Detail", &adjust_state->fx_detail, 1, 100); c |= ImGui::SliderInt("Opacity", &adjust_state->fx_opacity, 0, 100); return c; }, [&](Image& img) { effects::charcoal(img, adjust_state->fx_detail, adjust_state->fx_opacity); });
    adjust_modal(*this, "Colored Chalk", [&] { bool c = ImGui::SliderInt("Detail", &adjust_state->fx_detail, 1, 100); c |= ImGui::SliderInt("Opacity", &adjust_state->fx_opacity, 0, 100); return c; }, [&](Image& img) { effects::colored_chalk(img, adjust_state->fx_detail, adjust_state->fx_opacity); });
    adjust_modal(*this, "Colored Pencil", [&] { bool c = ImGui::SliderInt("Detail", &adjust_state->fx_detail, 1, 100); c |= ImGui::SliderInt("Opacity", &adjust_state->fx_opacity, 0, 100); return c; }, [&](Image& img) { effects::colored_pencil(img, adjust_state->fx_detail, adjust_state->fx_opacity); });
    adjust_modal(*this, "Pencil", [&] { bool c = ImGui::SliderInt("Luminance", &adjust_state->fx_luminance, 0, 100); c |= ImGui::SliderInt("Blur", &adjust_state->fx_blur, 0, 20); c |= ImGui::ColorEdit3("Color", adjust_state->fx_color2, ImGuiColorEditFlags_NoInputs); return c; }, [&](Image& img) { effects::pencil(img, adjust_state->fx_luminance, adjust_state->fx_blur, float_rgb(adjust_state->fx_color2)); });
    adjust_modal(*this, "User Defined Filter",
        [&] {
            bool c = false;
            for (int j = 0; j < 5; ++j) {
                for (int i = 0; i < 5; ++i) {
                    ImGui::PushID(j * 5 + i);
                    ImGui::SetNextItemWidth(48);
                    c |= ImGui::InputFloat("##k", &adjust_state->fx_kernel[j * 5 + i], 0.0f, 0.0f, "%.0f");
                    ImGui::PopID();
                    if (i < 4) ImGui::SameLine();
                }
            }
            ImGui::SetNextItemWidth(100); c |= ImGui::InputFloat("Divisor", &adjust_state->fx_divisor, 0.0f, 0.0f, "%.1f");
            ImGui::SameLine(); ImGui::SetNextItemWidth(100); c |= ImGui::InputFloat("Bias", &adjust_state->fx_bias, 0.0f, 0.0f, "%.0f");
            if (ImGui::SmallButton("Compute divisor")) { float sum = 0; for (float k : adjust_state->fx_kernel) sum += k; adjust_state->fx_divisor = sum != 0.0f ? sum : 1.0f; c = true; }
            return c;
        },
        [&](Image& img) { effects::user_defined_filter(img, adjust_state->fx_kernel, adjust_state->fx_divisor, adjust_state->fx_bias); });

    adjust_modal(*this, "Kaleidoscope",
        [&] {
            bool c = ImGui::SliderInt("Petals", &adjust_state->kal_petals, 2, 32);
            c |= ImGui::SliderFloat("Rotation", &adjust_state->kal_angle, 0.0f, 359.0f, "%.0f");
            c |= ImGui::SliderFloat("Radius", &adjust_state->kal_radius, 1.0f, 100.0f, "%.0f%%");
            return c;
        },
        [&](Image& img) { effects::kaleidoscope(img, adjust_state->kal_petals, adjust_state->kal_angle, adjust_state->kal_radius); });

    adjust_modal(*this, "Sunburst",
        [&] {
            bool c = ImGui::SliderFloat("Horizontal position", &effect_state->sun_x, 0.0f, 1.0f, "%.2f");
            c |= ImGui::SliderFloat("Vertical position", &effect_state->sun_y, 0.0f, 1.0f, "%.2f");
            c |= ImGui::SliderFloat("Brightness", &effect_state->sun_brightness, 0.0f, 2.0f, "%.2f");
            c |= ImGui::SliderInt("Rays", &effect_state->sun_rays, 0, 64);
            c |= ImGui::SliderFloat("Ray brightness", &effect_state->sun_ray_brightness, 0.0f, 2.0f, "%.2f");
            c |= ImGui::ColorEdit3("Color", effect_state->sun_color, ImGuiColorEditFlags_NoInputs);
            return c;
        },
        [&](Image& img) {
            auto c8 = [](float f) { return static_cast<uint8_t>(f * 255.0f + 0.5f); };
            effects::sunburst(img, effect_state->sun_x, effect_state->sun_y, effect_state->sun_brightness, effect_state->sun_rays, effect_state->sun_ray_brightness, {c8(effect_state->sun_color[0]), c8(effect_state->sun_color[1]), c8(effect_state->sun_color[2]), 255});
        });
}
