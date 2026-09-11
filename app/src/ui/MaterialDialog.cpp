// The Material Properties dialog and the material widgets of the Materials
// palette and toolbar: a material is a color, gradient or pattern, with an
// optional texture over it, or transparent (no paint). Clicking a material
// box opens the dialog for that material; Cancel restores what it had.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>

#include "App.h"
#include "MaterialDialog.h"
#include "firn/adjust.h"
#include "firn/io.h"
#include "firn/io_psp.h"
#include "imgui.h"

using namespace firn;

namespace {

ImU32 over_white(Color c) {
    const float a = c.a / 255.0f;
    return IM_COL32(static_cast<int>(c.r * a + 255 * (1 - a)), static_cast<int>(c.g * a + 255 * (1 - a)), static_cast<int>(c.b * a + 255 * (1 - a)), 255);
}

// Draws what a material looks like into a rect: solid color, gradient run,
// or pattern tile, dimmed by its texture, crossed out when transparent.
void draw_material_swatch(App& app, bool foreground, ImVec2 p0, ImVec2 p1) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const App::Material& m = foreground ? app.fg_material : app.bg_material;
    const vec::PaintStyle st = app.material_style(foreground);
    const float w = p1.x - p0.x, h = p1.y - p0.y;
    // Checkerboard base so alpha shows.
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            dl->AddRectFilled(ImVec2(p0.x + w * x / 4, p0.y + h * y / 4), ImVec2(p0.x + w * (x + 1) / 4, p0.y + h * (y + 1) / 4), ((x + y) & 1) ? IM_COL32(160, 160, 160, 255) : IM_COL32(210, 210, 210, 255));
    if (m.transparent) {
        dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 200));
        dl->AddLine(p0, p1, IM_COL32(220, 0, 0, 255), 2.0f);
        dl->AddLine(ImVec2(p1.x, p0.y), ImVec2(p0.x, p1.y), IM_COL32(220, 0, 0, 255), 2.0f);
    } else if (st.kind == vec::PaintStyle::Kind::Gradient) {
        const int n = std::max(4, static_cast<int>(w / 2));
        for (int i = 0; i < n; ++i) {
            float t = static_cast<float>(i) / (n - 1);
            if (st.gradient.repeats > 0) t = std::fmod(t * (st.gradient.repeats + 1), 1.0f);
            if (st.gradient.invert) t = 1.0f - t;
            const Color c = st.gradient.at(t);
            dl->AddRectFilled(ImVec2(p0.x + w * i / n, p0.y), ImVec2(p0.x + w * (i + 1) / n + 1, p1.y), IM_COL32(c.r, c.g, c.b, c.a));
        }
    } else if (st.kind == vec::PaintStyle::Kind::Pattern && st.pattern) {
        // A few tiles of the pattern, sampled coarsely.
        const int cols = std::max(1, static_cast<int>(w / 3)), rows = std::max(1, static_cast<int>(h / 3));
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < cols; ++x) {
                const int px = static_cast<int>(x * 3 / std::max(st.pattern_scale, 0.05f)) % st.pattern->width();
                const int py = static_cast<int>(y * 3 / std::max(st.pattern_scale, 0.05f)) % st.pattern->height();
                const Color c = st.pattern->get(px, py);
                dl->AddRectFilled(ImVec2(p0.x + w * x / cols, p0.y + h * y / rows), ImVec2(p0.x + w * (x + 1) / cols + 1, p0.y + h * (y + 1) / rows + 1), IM_COL32(c.r, c.g, c.b, c.a));
            }
    } else {
        dl->AddRectFilled(p0, p1, IM_COL32(st.color.r, st.color.g, st.color.b, st.color.a));
    }
    if (!m.transparent && st.texture) {
        // Texture: darken by the texture's lightness on a coarse grid.
        const int cols = std::max(1, static_cast<int>(w / 3)), rows = std::max(1, static_cast<int>(h / 3));
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < cols; ++x) {
                const float f = vec::texture_factor(st, x * 3.0f, y * 3.0f, 0, 0);
                if (f < 0.999f) dl->AddRectFilled(ImVec2(p0.x + w * x / cols, p0.y + h * y / rows), ImVec2(p0.x + w * (x + 1) / cols + 1, p0.y + h * (y + 1) / rows + 1), IM_COL32(0, 0, 0, static_cast<int>((1 - f) * 255)));
            }
    }
    dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 255));
}

void set_color(float* dst, ImVec4 c) { dst[0] = c.x; dst[1] = c.y; dst[2] = c.z; }

// Hue ring with a saturation/value square inside; left click sets the
// foreground, right click the background.
void frame_picker(App& app, float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    const float r_out = size * 0.5f, r_in = size * 0.39f;
    const int seg = 64;
    for (int i = 0; i < seg; ++i) {
        const float a0 = 6.2832f * i / seg, a1 = 6.2832f * (i + 1) / seg;
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(static_cast<float>(i) / seg, 1.0f, 1.0f, r, g, b);
        const ImVec2 q[4] = {ImVec2(c.x + std::cos(a0) * r_in, c.y + std::sin(a0) * r_in), ImVec2(c.x + std::cos(a0) * r_out, c.y + std::sin(a0) * r_out),
                             ImVec2(c.x + std::cos(a1) * r_out, c.y + std::sin(a1) * r_out), ImVec2(c.x + std::cos(a1) * r_in, c.y + std::sin(a1) * r_in)};
        dl->AddConvexPolyFilled(q, 4, IM_COL32(static_cast<int>(r * 255), static_cast<int>(g * 255), static_cast<int>(b * 255), 255));
    }
    const float half = r_in * 0.68f;
    const ImVec2 s0(c.x - half, c.y - half), s1(c.x + half, c.y + half);
    float hr, hg, hb;
    ImGui::ColorConvertHSVtoRGB(app.frame_hue, 1.0f, 1.0f, hr, hg, hb);
    const ImU32 hue_col = IM_COL32(static_cast<int>(hr * 255), static_cast<int>(hg * 255), static_cast<int>(hb * 255), 255);
    dl->AddRectFilledMultiColor(s0, s1, IM_COL32(255, 255, 255, 255), hue_col, hue_col, IM_COL32(255, 255, 255, 255));
    dl->AddRectFilledMultiColor(s0, s1, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 255), IM_COL32(0, 0, 0, 255));
    dl->AddRect(s0, s1, IM_COL32(0, 0, 0, 255));
    // Hue marker.
    dl->AddCircle(ImVec2(c.x + std::cos(app.frame_hue * 6.2832f) * (r_in + r_out) * 0.5f, c.y + std::sin(app.frame_hue * 6.2832f) * (r_in + r_out) * 0.5f), 4.0f, IM_COL32(0, 0, 0, 255), 0, 2.0f);
    ImGui::InvisibleButton("##frame", ImVec2(size, size), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool left = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool right = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (!left && !right) return;
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const float dx = mp.x - c.x, dy = mp.y - c.y, dist = std::sqrt(dx * dx + dy * dy);
    float* target = left ? app.fg_color : app.bg_color;
    App::Material& m = left ? app.fg_material : app.bg_material;
    if (mp.x >= s0.x && mp.x <= s1.x && mp.y >= s0.y && mp.y <= s1.y) {
        const float sat = (mp.x - s0.x) / (s1.x - s0.x), val = 1.0f - (mp.y - s0.y) / (s1.y - s0.y);
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(app.frame_hue, sat, val, r, g, b);
        set_color(target, ImVec4(r, g, b, 1)); m.kind = 0;
    } else if (dist >= r_in * 0.95f && dist <= r_out * 1.05f) {
        float hue = std::atan2(dy, dx) / 6.2832f;
        if (hue < 0) hue += 1.0f;
        app.frame_hue = hue;
        float h, s, v;
        ImGui::ColorConvertRGBtoHSV(target[0], target[1], target[2], h, s, v);
        if (s < 0.05f) s = 1.0f;
        if (v < 0.05f) v = 1.0f;
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(hue, s, v, r, g, b);
        set_color(target, ImVec4(r, g, b, 1)); m.kind = 0;
    }
}

// Hue across, lightness down: white at the top, black at the bottom.
void rainbow_picker(App& app, float width, float height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const int cols = std::max(8, static_cast<int>(width / 3));
    for (int i = 0; i < cols; ++i) {
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(static_cast<float>(i) / cols, 1.0f, 1.0f, r, g, b);
        const ImU32 hue = IM_COL32(static_cast<int>(r * 255), static_cast<int>(g * 255), static_cast<int>(b * 255), 255);
        const float x0 = p.x + width * i / cols, x1 = p.x + width * (i + 1) / cols + 1;
        dl->AddRectFilledMultiColor(ImVec2(x0, p.y), ImVec2(x1, p.y + height * 0.5f), IM_COL32(255, 255, 255, 255), IM_COL32(255, 255, 255, 255), hue, hue);
        dl->AddRectFilledMultiColor(ImVec2(x0, p.y + height * 0.5f), ImVec2(x1, p.y + height), hue, hue, IM_COL32(0, 0, 0, 255), IM_COL32(0, 0, 0, 255));
    }
    // A gray column at the right edge.
    dl->AddRectFilledMultiColor(ImVec2(p.x + width, p.y), ImVec2(p.x + width + 12, p.y + height), IM_COL32(255, 255, 255, 255), IM_COL32(255, 255, 255, 255), IM_COL32(0, 0, 0, 255), IM_COL32(0, 0, 0, 255));
    dl->AddRect(p, ImVec2(p.x + width + 12, p.y + height), IM_COL32(0, 0, 0, 255));
    ImGui::InvisibleButton("##rainbow", ImVec2(width + 12, height), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool left = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool right = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (ImGui::IsItemHovered()) {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        const float u = std::clamp((mp.x - p.x) / width, 0.0f, 1.0f), v = std::clamp((mp.y - p.y) / height, 0.0f, 1.0f);
        float r, g, b;
        if (mp.x > p.x + width) r = g = b = 1.0f - v;
        else if (v < 0.5f) { ImGui::ColorConvertHSVtoRGB(u, v * 2.0f, 1.0f, r, g, b); }
        else { ImGui::ColorConvertHSVtoRGB(u, 1.0f, 1.0f - (v - 0.5f) * 2.0f, r, g, b); }
        ImGui::SetTooltip("%d, %d, %d", static_cast<int>(r * 255 + 0.5f), static_cast<int>(g * 255 + 0.5f), static_cast<int>(b * 255 + 0.5f));
        if (left || right) {
            set_color(left ? app.fg_color : app.bg_color, ImVec4(r, g, b, 1));
            (left ? app.fg_material : app.bg_material).kind = 0;
        }
    }
}

// RGB, HSL and HTML entry, the way the original's color dialog offers them:
// red/green/blue and hue/saturation/lightness each 0..255, plus a hex field.
void color_numbers(App& app, float* col) {
    auto to8 = [](float v) { return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    int rgb[3] = {to8(col[0]), to8(col[1]), to8(col[2])};
    bool changed = false;
    ImGui::TextUnformatted("Red");
    for (int i = 0; i < 3; ++i) {
        static const char* kLabels[3] = {"##r", "##g", "##b"};
        static const char* kNames[3] = {"Red", "Green", "Blue"};
        if (i) { ImGui::TextUnformatted(kNames[i]); }
        ImGui::SameLine(92);
        ImGui::SetNextItemWidth(80);
        if (ImGui::InputInt(kLabels[i], &rgb[i])) { rgb[i] = std::clamp(rgb[i], 0, 255); changed = true; }
    }
    if (changed) for (int i = 0; i < 3; ++i) col[i] = rgb[i] / 255.0f;

    // HSL in the original's 0..255 scale.
    const adjust::HSL hsl = adjust::rgb_to_hsl(static_cast<uint8_t>(to8(col[0])), static_cast<uint8_t>(to8(col[1])), static_cast<uint8_t>(to8(col[2])));
    int hsl255[3] = {static_cast<int>(hsl.h / 360.0f * 255.0f + 0.5f), static_cast<int>(hsl.s * 255.0f + 0.5f), static_cast<int>(hsl.l * 255.0f + 0.5f)};
    bool hsl_changed = false;
    for (int i = 0; i < 3; ++i) {
        static const char* kLabels[3] = {"##h", "##s", "##l"};
        static const char* kNames[3] = {"Hue", "Saturation", "Lightness"};
        ImGui::TextUnformatted(kNames[i]);
        ImGui::SameLine(92);
        ImGui::SetNextItemWidth(80);
        if (ImGui::InputInt(kLabels[i], &hsl255[i])) { hsl255[i] = std::clamp(hsl255[i], 0, 255); hsl_changed = true; }
    }
    if (hsl_changed) {
        adjust::HSL h{hsl255[0] / 255.0f * 360.0f, hsl255[1] / 255.0f, hsl255[2] / 255.0f};
        uint8_t r = 0, g = 0, b = 0;
        adjust::hsl_to_rgb(h, &r, &g, &b);
        col[0] = r / 255.0f; col[1] = g / 255.0f; col[2] = b / 255.0f;
    }

    // HTML code. The buffer follows the color unless it is being typed in.
    ImGui::TextUnformatted("HTML");
    ImGui::SameLine(92);
    ImGui::SetNextItemWidth(80);
    const bool typing = ImGui::IsItemActive();
    if (!typing) std::snprintf(app.html_color, sizeof(app.html_color), "#%02X%02X%02X", to8(col[0]), to8(col[1]), to8(col[2]));
    if (ImGui::InputText("##html", app.html_color, sizeof(app.html_color), ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_AutoSelectAll)) {
        const char* t = app.html_color;
        while (*t == '#' || *t == ' ') ++t;
        unsigned v = 0;
        if (std::strlen(t) >= 6 && std::sscanf(t, "%6x", &v) == 1) {
            col[0] = ((v >> 16) & 255) / 255.0f; col[1] = ((v >> 8) & 255) / 255.0f; col[2] = (v & 255) / 255.0f;
        }
    }
}

// A gradient drawn as a strip, over a checker so opacity stops show.
void gradient_strip(ImDrawList* dl, const vec::Gradient& g, ImVec2 p0, ImVec2 p1) {
    const int cells = std::max(8, static_cast<int>(p1.x - p0.x) / 2);
    for (int i = 0; i < cells; ++i) {
        const float x0 = p0.x + (p1.x - p0.x) * i / cells, x1 = p0.x + (p1.x - p0.x) * (i + 1) / cells + 1.0f;
        dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x1, p1.y), over_white(g.at(static_cast<float>(i) / (cells - 1))));
    }
    dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 255));
}

// The gradient as it will paint: style, angle, center, repeats and invert
// applied over a square, the way the original previews it.
void gradient_style_preview(const vec::Gradient& g, float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const int cells = 56;
    const float step = size / cells;
    for (int y = 0; y < cells; ++y)
        for (int x = 0; x < cells; ++x) {
            const Color c = g.at_point(x + 0.5f, y + 0.5f, 0, 0, static_cast<float>(cells), static_cast<float>(cells));
            dl->AddRectFilled(ImVec2(p.x + x * step, p.y + y * step), ImVec2(p.x + (x + 1) * step + 1.0f, p.y + (y + 1) * step + 1.0f), over_white(c));
        }
    dl->AddRect(p, ImVec2(p.x + size, p.y + size), IM_COL32(0, 0, 0, 255));
    ImGui::Dummy(ImVec2(size, size));
}

// The 48 basic colors of the classic Windows color dialog.
const uint8_t kBasic[48][3] = {
    {255, 128, 128}, {255, 255, 128}, {128, 255, 128}, {0, 255, 128}, {128, 255, 255}, {0, 128, 255}, {255, 128, 192}, {255, 128, 255},
    {255, 0, 0}, {255, 255, 0}, {128, 255, 0}, {0, 255, 64}, {0, 255, 255}, {0, 128, 192}, {128, 128, 192}, {255, 0, 255},
    {128, 64, 64}, {255, 128, 64}, {0, 255, 0}, {0, 128, 128}, {0, 64, 128}, {128, 128, 255}, {128, 0, 64}, {255, 0, 128},
    {128, 0, 0}, {255, 128, 0}, {0, 128, 0}, {0, 128, 64}, {0, 0, 255}, {0, 0, 160}, {128, 0, 128}, {128, 0, 255},
    {64, 0, 0}, {128, 64, 0}, {0, 64, 0}, {0, 64, 64}, {0, 0, 128}, {0, 0, 64}, {64, 0, 64}, {64, 0, 128},
    {0, 0, 0}, {128, 128, 0}, {128, 128, 64}, {128, 128, 128}, {64, 128, 128}, {192, 192, 192}, {64, 0, 64}, {255, 255, 255}};

// Gradient editor: color stops below the strip, opacity stops above; click
// selects, drag moves, double-click on the strip adds, Delete removes.
void gradient_editor(App& app, vec::Gradient& g) {
    const float w = 360.0f, h = 24.0f, tri = 8.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 strip0(p.x, p.y + tri + 2), strip1(p.x + w, p.y + tri + 2 + h);
    // Sort stops so the editor and the sampler agree.
    std::sort(g.colors.begin(), g.colors.end(), [](const vec::GradientStop& a, const vec::GradientStop& b) { return a.pos < b.pos; });
    std::sort(g.opacities.begin(), g.opacities.end(), [](const vec::OpacityStop& a, const vec::OpacityStop& b) { return a.pos < b.pos; });
    // Checkerboard then the run.
    for (int i = 0; i < static_cast<int>(w / 8); ++i)
        for (int j = 0; j < static_cast<int>(h / 8) + 1; ++j)
            dl->AddRectFilled(ImVec2(strip0.x + i * 8, strip0.y + j * 8), ImVec2(std::min(strip0.x + i * 8 + 8, strip1.x), std::min(strip0.y + j * 8 + 8, strip1.y)), ((i + j) & 1) ? IM_COL32(160, 160, 160, 255) : IM_COL32(210, 210, 210, 255));
    const int n = static_cast<int>(w / 2);
    for (int i = 0; i < n; ++i) {
        const Color c = g.at(static_cast<float>(i) / (n - 1));
        dl->AddRectFilled(ImVec2(strip0.x + w * i / n, strip0.y), ImVec2(strip0.x + w * (i + 1) / n + 1, strip1.y), IM_COL32(c.r, c.g, c.b, c.a));
    }
    dl->AddRect(strip0, strip1, IM_COL32(0, 0, 0, 255));
    // Markers.
    auto marker_x = [&](float pos) { return strip0.x + w * std::clamp(pos, 0.0f, 100.0f) / 100.0f; };
    for (size_t i = 0; i < g.opacities.size(); ++i) {
        const float x = marker_x(g.opacities[i].pos);
        const int v = static_cast<int>(g.opacities[i].opacity * 2.55f);
        const ImVec2 t[3] = {ImVec2(x, strip0.y - 1), ImVec2(x - tri * 0.7f, strip0.y - tri - 1), ImVec2(x + tri * 0.7f, strip0.y - tri - 1)};
        dl->AddTriangleFilled(t[0], t[1], t[2], IM_COL32(v, v, v, 255));
        dl->AddTriangle(t[0], t[1], t[2], static_cast<int>(i) == app.gradient_sel_opacity ? IM_COL32(255, 160, 0, 255) : IM_COL32(0, 0, 0, 255), static_cast<int>(i) == app.gradient_sel_opacity ? 2.0f : 1.0f);
    }
    for (size_t i = 0; i < g.colors.size(); ++i) {
        const float x = marker_x(g.colors[i].pos);
        const Color c = g.colors[i].color;
        const ImVec2 t[3] = {ImVec2(x, strip1.y + 1), ImVec2(x - tri * 0.7f, strip1.y + tri + 1), ImVec2(x + tri * 0.7f, strip1.y + tri + 1)};
        dl->AddTriangleFilled(t[0], t[1], t[2], IM_COL32(c.r, c.g, c.b, 255));
        dl->AddTriangle(t[0], t[1], t[2], static_cast<int>(i) == app.gradient_sel_color ? IM_COL32(255, 160, 0, 255) : IM_COL32(0, 0, 0, 255), static_cast<int>(i) == app.gradient_sel_color ? 2.0f : 1.0f);
    }
    ImGui::InvisibleButton("##gradedit", ImVec2(w, h + 2 * tri + 4));
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        app.gradient_sel_color = app.gradient_sel_opacity = -1;
        if (mp.y > strip1.y) {
            for (size_t i = 0; i < g.colors.size(); ++i) if (std::abs(marker_x(g.colors[i].pos) - mp.x) <= tri) app.gradient_sel_color = static_cast<int>(i);
        } else if (mp.y < strip0.y) {
            for (size_t i = 0; i < g.opacities.size(); ++i) if (std::abs(marker_x(g.opacities[i].pos) - mp.x) <= tri) app.gradient_sel_opacity = static_cast<int>(i);
        }
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const float pos = std::clamp((mp.x - strip0.x) / w * 100.0f, 0.0f, 100.0f);
        if (mp.y < strip0.y) {
            g.opacities.push_back({g.at(pos / 100.0f).a / 2.55f, pos, 50});
            std::sort(g.opacities.begin(), g.opacities.end(), [](const vec::OpacityStop& a, const vec::OpacityStop& b) { return a.pos < b.pos; });
            for (size_t i = 0; i < g.opacities.size(); ++i) if (g.opacities[i].pos == pos) app.gradient_sel_opacity = static_cast<int>(i);
        } else {
            Color c = g.at(pos / 100.0f); c.a = 255;
            g.colors.push_back({c, pos, 50});
            std::sort(g.colors.begin(), g.colors.end(), [](const vec::GradientStop& a, const vec::GradientStop& b) { return a.pos < b.pos; });
            for (size_t i = 0; i < g.colors.size(); ++i) if (g.colors[i].pos == pos) app.gradient_sel_color = static_cast<int>(i);
        }
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const float pos = std::clamp((mp.x - strip0.x) / w * 100.0f, 0.0f, 100.0f);
        if (app.gradient_sel_color >= 0 && app.gradient_sel_color < static_cast<int>(g.colors.size())) {
            g.colors[app.gradient_sel_color].pos = pos;
            // Keep the selection on the dragged stop after the resort.
            const Color c = g.colors[app.gradient_sel_color].color;
            std::sort(g.colors.begin(), g.colors.end(), [](const vec::GradientStop& a, const vec::GradientStop& b) { return a.pos < b.pos; });
            for (size_t i = 0; i < g.colors.size(); ++i) if (g.colors[i].pos == pos && g.colors[i].color.r == c.r && g.colors[i].color.g == c.g && g.colors[i].color.b == c.b) app.gradient_sel_color = static_cast<int>(i);
        } else if (app.gradient_sel_opacity >= 0 && app.gradient_sel_opacity < static_cast<int>(g.opacities.size())) {
            g.opacities[app.gradient_sel_opacity].pos = pos;
            const float o = g.opacities[app.gradient_sel_opacity].opacity;
            std::sort(g.opacities.begin(), g.opacities.end(), [](const vec::OpacityStop& a, const vec::OpacityStop& b) { return a.pos < b.pos; });
            for (size_t i = 0; i < g.opacities.size(); ++i) if (g.opacities[i].pos == pos && g.opacities[i].opacity == o) app.gradient_sel_opacity = static_cast<int>(i);
        }
    }
    ImGui::TextDisabled("Double-click the strip to add a color stop, above it for an opacity stop.");
    if (app.gradient_sel_color >= 0 && app.gradient_sel_color < static_cast<int>(g.colors.size())) {
        vec::GradientStop& st = g.colors[app.gradient_sel_color];
        float c[3] = {st.color.r / 255.0f, st.color.g / 255.0f, st.color.b / 255.0f};
        ImGui::SetNextItemWidth(160);
        if (ImGui::ColorEdit3("Stop color", c)) st.color = {static_cast<uint8_t>(c[0] * 255 + 0.5f), static_cast<uint8_t>(c[1] * 255 + 0.5f), static_cast<uint8_t>(c[2] * 255 + 0.5f), 255};
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::SliderFloat("Location", &st.pos, 0.0f, 100.0f, "%.0f%%");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::SliderFloat("Midpoint", &st.mid, 1.0f, 99.0f, "%.0f%%");
        ImGui::SameLine();
        if (ImGui::Button("Delete") && g.colors.size() > 1) { g.colors.erase(g.colors.begin() + app.gradient_sel_color); app.gradient_sel_color = -1; }
    } else if (app.gradient_sel_opacity >= 0 && app.gradient_sel_opacity < static_cast<int>(g.opacities.size())) {
        vec::OpacityStop& st = g.opacities[app.gradient_sel_opacity];
        ImGui::SetNextItemWidth(120);
        ImGui::SliderFloat("Opacity", &st.opacity, 0.0f, 100.0f, "%.0f%%");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::SliderFloat("Location", &st.pos, 0.0f, 100.0f, "%.0f%%");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::SliderFloat("Midpoint", &st.mid, 1.0f, 99.0f, "%.0f%%");
        ImGui::SameLine();
        if (ImGui::Button("Delete") && g.opacities.size() > 1) { g.opacities.erase(g.opacities.begin() + app.gradient_sel_opacity); app.gradient_sel_opacity = -1; }
    } else {
        ImGui::TextDisabled("Click a stop marker to edit it.");
    }
}

std::shared_ptr<const Image> load_pattern_file(const std::string& path) {
    std::string err;
    if (io::is_psp_extension(path)) {
        std::vector<std::string> warnings;
        auto d = io::load_document(path, &err, &warnings);
        if (!d) return nullptr;
        return std::make_shared<Image>(d->composite());
    }
    auto img = io::load(path, &err);
    if (!img) return nullptr;
    return std::make_shared<Image>(std::move(*img));
}

}  // namespace

// --- Shared widgets ------------------------------------------------------------

bool material_box(App& app, bool foreground, ImVec2 size) {
    ImGui::PushID(foreground ? "fgbox" : "bgbox");
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##box", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    draw_material_swatch(app, foreground, p, ImVec2(p.x + size.x, p.y + size.y));
    if (ImGui::IsItemHovered()) {
        const App::Material& m = foreground ? app.fg_material : app.bg_material;
        const char* kind = m.transparent ? "transparent" : m.kind == 1 ? "gradient" : m.kind == 2 ? "pattern" : "color";
        ImGui::SetTooltip("%s material: %s%s\nClick to edit, right-click to swap", foreground ? "Foreground" : "Background", kind, m.texture_on && m.texture ? " with texture" : "");
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            for (int i = 0; i < 4; ++i) std::swap(app.fg_color[i], app.bg_color[i]);
            std::swap(app.fg_material, app.bg_material);
        }
    }
    ImGui::PopID();
    if (clicked && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) { app.open_material_dialog(foreground); return true; }
    return false;
}

void draw_materials_header(App& app) {
    const float box = 40.0f;
    material_box(app, true, ImVec2(box, box));
    ImGui::SameLine();
    material_box(app, false, ImVec2(box, box));
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (ImGui::SmallButton("Swap")) {
        for (int i = 0; i < 4; ++i) std::swap(app.fg_color[i], app.bg_color[i]);
        std::swap(app.fg_material, app.bg_material);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Swap the foreground and background materials");
    if (ImGui::SmallButton("B/W")) {
        app.fg_color[0] = app.fg_color[1] = app.fg_color[2] = 0; app.bg_color[0] = app.bg_color[1] = app.bg_color[2] = 1;
        app.fg_material.kind = app.bg_material.kind = 0; app.fg_material.transparent = app.bg_material.transparent = false;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Black foreground on white background");
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Checkbox("Fg off", &app.fg_material.transparent);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Transparent foreground: shapes get no stroke, text no outline");
    ImGui::Checkbox("Bg off", &app.bg_material.transparent);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Transparent background: shapes and text get no fill");
    ImGui::EndGroup();
    // Picker tabs like the original's Frame / Rainbow / Swatches.
    if (ImGui::BeginTabBar("##matview")) {
        if (ImGui::BeginTabItem("Frame")) { app.material_view = 0; frame_picker(app, std::min(ImGui::GetContentRegionAvail().x - 4.0f, 200.0f)); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Rainbow")) { app.material_view = 1; rainbow_picker(app, std::max(60.0f, ImGui::GetContentRegionAvail().x - 20.0f), 90.0f); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Swatches")) { app.material_view = 2; ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

// --- The dialog ------------------------------------------------------------

void App::open_material_dialog(bool foreground) {
    material_dialog_fg = foreground;
    material_backup = foreground ? fg_material : bg_material;
    std::memcpy(color_backup, foreground ? fg_color : bg_color, sizeof(color_backup));
    const Material& m = foreground ? fg_material : bg_material;
    material_tab = std::clamp(m.kind, 0, 2);
    material_tab_request = material_tab;
    gradient_sel_color = gradient_sel_opacity = -1;
    show_material_dialog = true;
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(fg_color[0], fg_color[1], fg_color[2], h, s, v);
    frame_hue = h;
}

void App::draw_material_dialog() {
    if (show_material_dialog) { ImGui::OpenPopup("Material Properties"); show_material_dialog = false; }
    if (!ImGui::BeginPopupModal("Material Properties", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const bool fg = material_dialog_fg;
    Material& m = fg ? fg_material : bg_material;
    float* col = fg ? fg_color : bg_color;

    ImGui::TextDisabled(fg ? "Foreground material" : "Background material");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 150.0f);
    ImGui::Checkbox("Transparent", &m.transparent);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("No paint at all: shapes lose this stroke or fill");

    if (ImGui::BeginTabBar("##mattabs")) {
        // The tab matching the material's kind is selected once, when the
        // dialog opens; after that the user's clicks rule.
        ImGuiTabItemFlags sel[3] = {0, 0, 0};
        if (material_tab_request >= 0 && material_tab_request < 3) { sel[material_tab_request] = ImGuiTabItemFlags_SetSelected; material_tab_request = -1; }
        if (ImGui::BeginTabItem("Color", nullptr, sel[0])) {
            material_tab = 0;
            m.kind = 0;
            ImGui::ColorPicker4("##picker", col, ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha);
            ImGui::SameLine();
            ImGui::BeginGroup();
            color_numbers(*this, col);
            ImGui::EndGroup();
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextUnformatted("Basic colors");
            for (int i = 0; i < 48; ++i) {
                ImGui::PushID(i);
                const ImVec4 c(kBasic[i][0] / 255.0f, kBasic[i][1] / 255.0f, kBasic[i][2] / 255.0f, 1.0f);
                if (ImGui::ColorButton("##basic", c, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18))) set_color(col, c);
                ImGui::PopID();
                if (i % 8 != 7) ImGui::SameLine(0, 2);
            }
            ImGui::Spacing();
            ImGui::TextUnformatted("Recent");
            for (size_t i = 0; i < recent_colors.size() && i < 16; ++i) {
                ImGui::PushID(static_cast<int>(100 + i));
                const Color& c = recent_colors[i];
                if (ImGui::ColorButton("##recent", ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f), ImGuiColorEditFlags_NoAlpha, ImVec2(18, 18))) set_color(col, ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f));
                ImGui::PopID();
                if (i % 8 != 7 && i + 1 < recent_colors.size() && i + 1 < 16) ImGui::SameLine(0, 2);
            }
            ImGui::EndGroup();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Gradient", nullptr, sel[1])) {
            material_tab = 1;
            m.kind = 1;
            ensure_gradients();
            const vec::PaintStyle st = material_style(fg);
            const char* current = m.gradient_index >= 0 && m.gradient_index < static_cast<int>(gradient_library.size()) ? gradient_library[m.gradient_index].name.c_str()
                                  : m.gradient_index == -2 ? (m.gradient.name.empty() ? "(edited)" : m.gradient.name.c_str()) : "Foreground-Background";
            // The library as strips, the way the original shows it.
            ImGui::TextDisabled("%s", current);
            const ImVec2 cell(84, 22);
            const int per_row = 4;
            ImGui::BeginChild("##gradlist", ImVec2((cell.x + 8) * per_row + 24, 108), true);
            {
                // The child's own draw list, so the strips scroll and clip with it.
                ImDrawList* gdl = ImGui::GetWindowDrawList();
                int shown = 0;
                auto entry = [&](const vec::Gradient& g, int index, const char* label) {
                    ImGui::PushID(index);
                    const ImVec2 p0 = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("##g", cell);
                    const bool picked = ImGui::IsItemClicked();
                    gradient_strip(gdl, g, p0, ImVec2(p0.x + cell.x, p0.y + cell.y));
                    if (m.gradient_index == index) gdl->AddRect(ImVec2(p0.x - 2, p0.y - 2), ImVec2(p0.x + cell.x + 2, p0.y + cell.y + 2), IM_COL32(255, 200, 60, 255), 0, 0, 2.0f);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", label);
                    if (picked) {
                        m.gradient_index = index;
                        if (index >= 0) m.gradient = gradient_library[static_cast<size_t>(index)];
                        gradient_sel_color = gradient_sel_opacity = -1;
                    }
                    ImGui::PopID();
                    if (++shown % per_row) ImGui::SameLine(0, 8);
                };
                entry(st.gradient, -1, "Foreground-Background");
                for (size_t i = 0; i < gradient_library.size(); ++i) entry(gradient_library[i], static_cast<int>(i), gradient_library[i].name.c_str());
            }
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextDisabled("Preview");
            gradient_style_preview(material_style(fg).gradient, 104.0f);
            ImGui::EndGroup();
            if (ImGui::Button("Edit stops") && m.gradient_index == -1) {
                // Editing the default gradient turns it into a private copy.
                m.gradient = st.gradient;
                m.gradient.name = "";
                m.gradient_index = -2;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Take a copy of the gradient to edit its stops (library gradients edit in place until you pick another)");
            if (m.gradient_index != -1) {
                vec::Gradient& g = m.gradient;
                gradient_editor(*this, g);
                if (m.gradient_index >= 0) m.gradient_index = -2;  // edited: no longer the library entry as such
            } else {
                // Preview of the two-color default.
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                for (int i = 0; i < 180; ++i) { const Color c = st.gradient.at(i / 179.0f); dl->AddRectFilled(ImVec2(p.x + i * 2, p.y), ImVec2(p.x + i * 2 + 3, p.y + 24), over_white(c)); }
                dl->AddRect(p, ImVec2(p.x + 360, p.y + 24), IM_COL32(0, 0, 0, 255));
                ImGui::Dummy(ImVec2(360, 26));
                ImGui::TextDisabled("Runs from the foreground color to the background color.");
            }
            ImGui::Separator();
            ImGui::SetNextItemWidth(130);
            ImGui::Combo("Style", &m.gradient_style, "Linear\0Rectangular\0Sunburst\0Radial\0");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            ImGui::SliderFloat("Angle", &m.gradient_angle, 0.0f, 360.0f, "%.0f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            ImGui::SliderInt("Repeats", &m.gradient_repeats, 0, 20);
            ImGui::SameLine();
            ImGui::Checkbox("Invert", &m.gradient_invert);
            if (m.gradient_style != 0) {
                ImGui::SetNextItemWidth(120);
                ImGui::SliderFloat("Center X", &m.gradient.center_x, 0.0f, 100.0f, "%.0f%%");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120);
                ImGui::SliderFloat("Center Y", &m.gradient.center_y, 0.0f, 100.0f, "%.0f%%");
            }
            // Save the current run as a library gradient.
            ImGui::SetNextItemWidth(200);
            ImGui::InputTextWithHint("##gname", "Name for Save As", gradient_save_name, sizeof(gradient_save_name));
            ImGui::SameLine();
            if (ImGui::Button("Save As...") && gradient_save_name[0]) {
                vec::Gradient g = material_style(fg).gradient;
                g.name = gradient_save_name;
                namespace fs = std::filesystem;
                const fs::path dir = fs::path(Config::directory()) / "gradients";
                std::error_code ec;
                fs::create_directories(dir, ec);
                std::string safe = g.name;
                for (char& ch : safe) if (ch == '/' || ch == '\\' || ch == ':') ch = '_';
                std::string err;
                if (io::save_gradients({g}, (dir / (safe + ".PspGradient")).string(), &err)) {
                    gradient_library.push_back(g);
                    std::sort(gradient_library.begin(), gradient_library.end(), [](const vec::Gradient& a, const vec::Gradient& b) { return a.name < b.name; });
                    for (size_t i = 0; i < gradient_library.size(); ++i) if (gradient_library[i].name == g.name) { m.gradient_index = static_cast<int>(i); m.gradient = gradient_library[i]; }
                    status = "Saved gradient " + g.name;
                } else status = "Save gradient: " + err;
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Pattern", nullptr, sel[2])) {
            material_tab = 2;
            m.kind = 2;
            ensure_patterns();
            const char* current = m.pattern_index >= 0 && m.pattern_index < static_cast<int>(pattern_library.size()) ? pattern_library[m.pattern_index].name.c_str()
                                  : m.pattern_index <= -2 ? "(open image)" : "(none)";
            ImGui::SetNextItemWidth(260);
            if (ImGui::BeginCombo("Pattern", current)) {
                // Open images first, as the original offers them.
                for (int i = 0; i < static_cast<int>(docs.size()); ++i) {
                    ImGui::PushID(1000 + i);
                    const std::string label = "Image: " + document_title(i);
                    if (ImGui::Selectable(label.c_str(), m.pattern_index == -2 - i)) {
                        const Document* d = i == current_doc ? doc.get() : docs[i].doc.get();
                        if (d) { m.pattern = std::make_shared<Image>(d->composite()); m.pattern_index = -2 - i; }
                    }
                    ImGui::PopID();
                }
                if (!docs.empty()) ImGui::Separator();
                for (size_t i = 0; i < pattern_library.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(pattern_library[i].name.c_str(), m.pattern_index == static_cast<int>(i))) {
                        m.pattern_index = static_cast<int>(i);
                        m.pattern = load_pattern_file(pattern_library[i].path);
                        if (!m.pattern) status = "Could not load pattern " + pattern_library[i].path;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            if (m.pattern && !m.pattern->empty()) {
                ImGui::SetNextItemWidth(120);
                ImGui::SliderFloat("Scale", &m.pattern_scale, 0.1f, 4.0f, "%.2f");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120);
                ImGui::SliderFloat("Angle", &m.pattern_angle, 0.0f, 360.0f, "%.0f");
                ImGui::TextDisabled("%d x %d tile", m.pattern->width(), m.pattern->height());
            } else {
                ImGui::TextDisabled("Pick a pattern from the library or an open image.");
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // Texture over whatever the tab chose.
    ImGui::Separator();
    ImGui::Checkbox("Texture", &m.texture_on);
    ImGui::SameLine();
    ensure_textures();
    const char* tex = m.texture_index >= 0 && m.texture_index < static_cast<int>(textures.size()) ? textures[m.texture_index].name.c_str() : "(none)";
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo("##texture", tex)) {
        for (size_t i = 0; i < textures.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(textures[i].name.c_str(), m.texture_index == static_cast<int>(i))) {
                m.texture_index = static_cast<int>(i);
                m.texture = texture_image(m.texture_index);
                m.texture_on = m.texture != nullptr;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (m.texture_on && m.texture) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::SliderFloat("Scale##tex", &m.texture_scale, 0.1f, 4.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::SliderFloat("Angle##tex", &m.texture_angle, 0.0f, 360.0f, "%.0f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        float strength = m.texture_strength * 100.0f;
        if (ImGui::SliderFloat("Strength", &strength, 0.0f, 100.0f, "%.0f%%")) m.texture_strength = strength / 100.0f;
    }

    // Current / previous preview and the buttons.
    ImGui::Separator();
    ImGui::TextUnformatted("Current");
    ImGui::SameLine(90);
    ImGui::TextUnformatted("Previous");
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        draw_material_swatch(*this, fg, p, ImVec2(p.x + 70, p.y + 36));
        // Previous: temporarily swap in the backup to draw it.
        Material live = m; float live_col[4]; std::memcpy(live_col, col, sizeof(live_col));
        m = material_backup; std::memcpy(col, color_backup, sizeof(color_backup));
        draw_material_swatch(*this, fg, ImVec2(p.x + 90, p.y), ImVec2(p.x + 160, p.y + 36));
        m = live; std::memcpy(col, live_col, sizeof(live_col));
        ImGui::Dummy(ImVec2(160, 38));
    }
    const bool ok = ImGui::Button("OK", ImVec2(90, 0));
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(90, 0))) { m = material_backup; std::memcpy(col, color_backup, sizeof(color_backup)); }
    if (ok) {
        // The brush's paper texture follows the foreground material's texture.
        if (fg) { if (m.texture_on && m.texture_index >= 0) select_texture(m.texture_index); else if (!m.texture_on) select_texture(-1); }
        note_recent_color(col);
        ImGui::CloseCurrentPopup();
    } else if (cancel) {
        m = material_backup;
        std::memcpy(col, color_backup, sizeof(color_backup));
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
