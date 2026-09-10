// Tool icons drawn from primitives, so they scale with the UI and need no
// image assets. Each glyph is designed in a unit square and drawn into a
// `size` px box at `p`; `col` is the text color of the palette.
#include <cmath>
#include <cstring>
#include <initializer_list>

#include "Icons.h"
#include "imgui.h"

namespace {

struct Pen {
    ImDrawList* dl;
    ImVec2 o;
    float s;
    ImU32 col;
    float th;
    ImVec2 at(float x, float y) const { return ImVec2(o.x + x * s, o.y + y * s); }
    void line(float x0, float y0, float x1, float y1) const { dl->AddLine(at(x0, y0), at(x1, y1), col, th); }
    void rect(float x0, float y0, float x1, float y1, float r = 0.0f) const { dl->AddRect(at(x0, y0), at(x1, y1), col, r * s, 0, th); }
    void fill_rect(float x0, float y0, float x1, float y1, float r = 0.0f) const { dl->AddRectFilled(at(x0, y0), at(x1, y1), col, r * s); }
    void circle(float cx, float cy, float r) const { dl->AddCircle(at(cx, cy), r * s, col, 0, th); }
    void fill_circle(float cx, float cy, float r) const { dl->AddCircleFilled(at(cx, cy), r * s, col); }
    void dot(float cx, float cy) const { dl->AddCircleFilled(at(cx, cy), th * 0.9f, col); }
    void poly(std::initializer_list<float> xy, bool closed = true) const {
        ImVec2 pts[16];
        int n = 0;
        for (auto it = xy.begin(); it != xy.end() && n < 16; it += 2) pts[n++] = at(*it, *(it + 1));
        dl->AddPolyline(pts, n, col, closed ? ImDrawFlags_Closed : 0, th);
    }
    void fill_poly(std::initializer_list<float> xy) const {
        ImVec2 pts[16];
        int n = 0;
        for (auto it = xy.begin(); it != xy.end() && n < 16; it += 2) pts[n++] = at(*it, *(it + 1));
        dl->AddConvexPolyFilled(pts, n, col);
    }
    void dashed_rect(float x0, float y0, float x1, float y1) const {
        const float d = 0.14f;
        for (float x = x0; x < x1; x += 2 * d) { line(x, y0, std::fmin(x + d, x1), y0); line(x, y1, std::fmin(x + d, x1), y1); }
        for (float y = y0; y < y1; y += 2 * d) { line(x0, y, x0, std::fmin(y + d, y1)); line(x1, y, x1, std::fmin(y + d, y1)); }
    }
    void arrow(float x0, float y0, float x1, float y1) const {
        line(x0, y0, x1, y1);
        const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-4f) return;
        const float ux = dx / len, uy = dy / len, h = 0.18f;
        line(x1, y1, x1 - ux * h - uy * h * 0.6f, y1 - uy * h + ux * h * 0.6f);
        line(x1, y1, x1 - ux * h + uy * h * 0.6f, y1 - uy * h - ux * h * 0.6f);
    }
    void star(float cx, float cy, float r) const {
        ImVec2 pts[10];
        for (int k = 0; k < 10; ++k) {
            const float a = -1.5708f + 6.2832f * k / 10.0f, rr = (k % 2) ? r * 0.45f : r;
            pts[k] = at(cx + std::cos(a) * rr, cy + std::sin(a) * rr);
        }
        dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, th);
    }
    void sparkle(float cx, float cy, float r) const { line(cx - r, cy, cx + r, cy); line(cx, cy - r, cx, cy + r); }
    void text_a() const { poly({0.2f, 0.9f, 0.5f, 0.1f, 0.8f, 0.9f}, false); line(0.32f, 0.62f, 0.68f, 0.62f); }
};

}  // namespace

void draw_tool_icon(ImDrawList* dl, const char* name, ImVec2 p, float size, ImU32 col) {
    const Pen g{dl, p, size, col, std::fmax(1.0f, size / 11.0f)};
    auto is = [&](const char* n) { return std::strcmp(name, n) == 0; };
    if (is("Pan")) {  // hand: palm with four fingers and a thumb
        g.fill_rect(0.3f, 0.45f, 0.78f, 0.9f, 0.15f);
        for (int i = 0; i < 4; ++i) { const float x = 0.36f + i * 0.13f; g.line(x, 0.5f, x, i == 0 || i == 3 ? 0.28f : 0.14f); }
        g.line(0.3f, 0.6f, 0.12f, 0.45f);
    } else if (is("Zoom")) {
        g.circle(0.42f, 0.42f, 0.28f);
        g.line(0.63f, 0.63f, 0.92f, 0.92f);
        g.line(0.3f, 0.42f, 0.54f, 0.42f); g.line(0.42f, 0.3f, 0.42f, 0.54f);
    } else if (is("Move")) {
        g.arrow(0.5f, 0.5f, 0.5f, 0.06f); g.arrow(0.5f, 0.5f, 0.5f, 0.94f);
        g.arrow(0.5f, 0.5f, 0.06f, 0.5f); g.arrow(0.5f, 0.5f, 0.94f, 0.5f);
    } else if (is("Crop")) {
        g.poly({0.3f, 0.05f, 0.3f, 0.7f, 0.95f, 0.7f}, false);
        g.poly({0.05f, 0.3f, 0.7f, 0.3f, 0.7f, 0.95f}, false);
    } else if (is("Deform")) {
        g.rect(0.2f, 0.2f, 0.8f, 0.8f);
        for (float x : {0.2f, 0.8f}) for (float y : {0.2f, 0.8f}) g.fill_rect(x - 0.1f, y - 0.1f, x + 0.1f, y + 0.1f);
        g.fill_rect(0.4f, 0.1f, 0.6f, 0.3f);
    } else if (is("Straighten")) {
        g.line(0.1f, 0.7f, 0.9f, 0.35f);
        g.line(0.1f, 0.85f, 0.9f, 0.85f);
        g.fill_circle(0.1f, 0.7f, 0.07f); g.fill_circle(0.9f, 0.35f, 0.07f);
    } else if (is("Perspective Correction")) {
        g.poly({0.3f, 0.15f, 0.7f, 0.15f, 0.92f, 0.85f, 0.08f, 0.85f});
        g.line(0.45f, 0.15f, 0.3f, 0.85f); g.line(0.55f, 0.15f, 0.7f, 0.85f);
    } else if (is("Mesh Warp")) {
        for (int i = 0; i <= 2; ++i) { const float t = 0.15f + i * 0.35f; g.line(0.15f, t, 0.85f, t); }
        g.line(0.15f, 0.15f, 0.15f, 0.85f); g.line(0.85f, 0.15f, 0.85f, 0.85f);
        g.poly({0.5f, 0.15f, 0.62f, 0.35f, 0.4f, 0.6f, 0.5f, 0.85f}, false);
    } else if (is("Selection")) {
        g.dashed_rect(0.12f, 0.2f, 0.88f, 0.8f);
    } else if (is("Freehand Selection")) {
        g.poly({0.5f, 0.15f, 0.8f, 0.25f, 0.85f, 0.5f, 0.6f, 0.62f, 0.4f, 0.55f, 0.2f, 0.62f, 0.15f, 0.4f, 0.28f, 0.2f});
        g.line(0.42f, 0.6f, 0.35f, 0.92f);
        g.circle(0.38f, 0.66f, 0.07f);
    } else if (is("Magic Wand")) {
        g.line(0.15f, 0.85f, 0.65f, 0.35f);
        g.sparkle(0.72f, 0.28f, 0.2f);
        g.line(0.6f, 0.16f, 0.84f, 0.4f); g.line(0.6f, 0.4f, 0.84f, 0.16f);
    } else if (is("Dropper")) {
        g.line(0.2f, 0.8f, 0.6f, 0.4f);
        g.fill_poly({0.55f, 0.3f, 0.7f, 0.15f, 0.85f, 0.3f, 0.7f, 0.45f});
        g.line(0.15f, 0.85f, 0.25f, 0.75f);
        g.line(0.8f, 0.1f, 0.9f, 0.2f);
    } else if (is("Paint Brush")) {
        g.line(0.85f, 0.15f, 0.45f, 0.55f);
        g.fill_poly({0.42f, 0.5f, 0.55f, 0.62f, 0.3f, 0.9f, 0.12f, 0.85f});
    } else if (is("Airbrush")) {
        g.rect(0.35f, 0.35f, 0.65f, 0.9f, 0.08f);
        g.fill_rect(0.42f, 0.22f, 0.58f, 0.35f);
        g.line(0.5f, 0.22f, 0.5f, 0.12f);
        g.dot(0.78f, 0.18f); g.dot(0.9f, 0.3f); g.dot(0.82f, 0.42f); g.dot(0.92f, 0.12f);
    } else if (is("Warp Brush")) {
        for (int i = 0; i < 3; ++i) {
            const float r = 0.12f + i * 0.13f;
            dl->PathArcTo(g.at(0.5f, 0.5f), r * size, 0.4f + i * 0.5f, 3.6f + i * 0.5f);
            dl->PathStroke(col, 0, g.th);
        }
    } else if (is("Lighten/Darken")) {
        g.circle(0.5f, 0.5f, 0.32f);
        dl->PathArcTo(g.at(0.5f, 0.5f), 0.32f * size, -1.5708f, 1.5708f);
        dl->PathFillConvex(col);
    } else if (is("Dodge/Burn")) {
        g.circle(0.4f, 0.4f, 0.25f);
        dl->PathArcTo(g.at(0.4f, 0.4f), 0.25f * size, 0.7854f, 3.927f);
        dl->PathFillConvex(col);
        g.line(0.58f, 0.58f, 0.9f, 0.9f);
    } else if (is("Smudge")) {
        g.fill_rect(0.42f, 0.1f, 0.62f, 0.62f, 0.1f);
        g.poly({0.3f, 0.62f, 0.5f, 0.72f, 0.7f, 0.62f, 0.85f, 0.85f, 0.15f, 0.85f}, false);
    } else if (is("Soften Brush")) {
        g.fill_circle(0.5f, 0.5f, 0.14f);
        g.circle(0.5f, 0.5f, 0.24f);
        g.circle(0.5f, 0.5f, 0.34f);
    } else if (is("Sharpen Brush")) {
        g.fill_poly({0.5f, 0.1f, 0.75f, 0.9f, 0.25f, 0.9f});
    } else if (is("Saturation Up/Down")) {
        g.poly({0.5f, 0.1f, 0.75f, 0.5f, 0.7f, 0.75f, 0.5f, 0.88f, 0.3f, 0.75f, 0.25f, 0.5f});
        g.line(0.42f, 0.62f, 0.58f, 0.62f); g.line(0.5f, 0.54f, 0.5f, 0.7f);
    } else if (is("Hue Up/Down")) {
        g.circle(0.5f, 0.5f, 0.35f);
        for (int i = 0; i < 6; ++i) { const float a = i * 1.0472f; g.line(0.5f, 0.5f, 0.5f + std::cos(a) * 0.35f, 0.5f + std::sin(a) * 0.35f); }
    } else if (is("Red-eye Removal")) {
        g.poly({0.08f, 0.5f, 0.3f, 0.25f, 0.7f, 0.25f, 0.92f, 0.5f, 0.7f, 0.75f, 0.3f, 0.75f});
        g.circle(0.5f, 0.5f, 0.16f);
        g.fill_circle(0.5f, 0.5f, 0.07f);
    } else if (is("Clone Brush")) {
        g.rect(0.12f, 0.12f, 0.6f, 0.6f, 0.08f);
        g.fill_rect(0.4f, 0.4f, 0.88f, 0.88f, 0.08f);
    } else if (is("Heal Brush")) {
        g.circle(0.5f, 0.5f, 0.36f);
        g.line(0.5f, 0.28f, 0.5f, 0.72f); g.line(0.28f, 0.5f, 0.72f, 0.5f);
    } else if (is("Scratch Remover")) {
        ImVec2 pts[4] = {g.at(0.12f, 0.62f), g.at(0.62f, 0.12f), g.at(0.88f, 0.38f), g.at(0.38f, 0.88f)};
        dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, g.th);
        g.dot(0.45f, 0.45f); g.dot(0.55f, 0.55f); g.dot(0.55f, 0.45f); g.dot(0.45f, 0.55f);
    } else if (is("Object Remover")) {
        g.dashed_rect(0.12f, 0.12f, 0.88f, 0.88f);
        g.line(0.32f, 0.32f, 0.68f, 0.68f); g.line(0.68f, 0.32f, 0.32f, 0.68f);
    } else if (is("Color Replacer")) {
        g.circle(0.35f, 0.5f, 0.22f);
        g.fill_circle(0.7f, 0.5f, 0.22f);
        g.arrow(0.3f, 0.12f, 0.75f, 0.12f);
    } else if (is("Eraser")) {
        ImVec2 pts[4] = {g.at(0.1f, 0.65f), g.at(0.55f, 0.15f), g.at(0.9f, 0.45f), g.at(0.45f, 0.92f)};
        dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, g.th);
        g.line(0.3f, 0.43f, 0.68f, 0.72f);
    } else if (is("Flood Fill")) {
        ImVec2 pts[4] = {g.at(0.2f, 0.45f), g.at(0.55f, 0.1f), g.at(0.9f, 0.45f), g.at(0.55f, 0.8f)};
        dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, g.th);
        g.line(0.2f, 0.45f, 0.2f, 0.65f);
        g.fill_poly({0.12f, 0.7f, 0.2f, 0.55f, 0.28f, 0.7f, 0.2f, 0.85f});
    } else if (is("Picture Tube")) {
        g.rect(0.1f, 0.3f, 0.6f, 0.7f, 0.05f);
        g.line(0.6f, 0.4f, 0.72f, 0.4f); g.line(0.6f, 0.6f, 0.72f, 0.6f);
        g.dot(0.82f, 0.3f); g.dot(0.9f, 0.5f); g.dot(0.82f, 0.7f);
    } else if (is("Text")) {
        g.text_a();
    } else if (is("Line")) {
        g.line(0.2f, 0.8f, 0.8f, 0.2f);
        g.fill_rect(0.1f, 0.7f, 0.3f, 0.9f); g.fill_rect(0.7f, 0.1f, 0.9f, 0.3f);
    } else if (is("Preset Shape")) {
        g.star(0.5f, 0.52f, 0.42f);
    } else if (is("Object Selector")) {
        g.fill_poly({0.2f, 0.12f, 0.2f, 0.72f, 0.36f, 0.58f, 0.48f, 0.85f, 0.58f, 0.8f, 0.46f, 0.55f, 0.66f, 0.55f});
        g.dashed_rect(0.55f, 0.1f, 0.9f, 0.45f);
    } else if (is("Pen")) {
        g.poly({0.5f, 0.1f, 0.75f, 0.45f, 0.5f, 0.85f, 0.25f, 0.45f});
        g.line(0.5f, 0.45f, 0.5f, 0.85f);
        g.fill_circle(0.5f, 0.45f, 0.07f);
    } else {
        g.circle(0.5f, 0.5f, 0.3f);
    }
}
