#include <algorithm>
#include <cmath>
#include <cstdio>

#include <SDL_opengl.h>

#include "App.h"
#include "firn/icc.h"
#include "imgui.h"

// The image window. Draws the composite texture with zoom/pan, a checkerboard
// behind it for transparency, handles wheel-zoom and middle/space-drag pan,
// and routes everything else to the active tool. In tabbed mode the "Image"
// dock window holds one tab per document; in windowed mode it is the
// workspace and every document floats over it in its own window.
void App::draw_canvas() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Image", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    if (image_windows) {
        workspace_pos = ImGui::GetCursorScreenPos();
        workspace_size = ImGui::GetContentRegionAvail();
        ImGui::GetWindowDrawList()->AddRectFilled(workspace_pos, ImVec2(workspace_pos.x + workspace_size.x, workspace_pos.y + workspace_size.y), IM_COL32(45, 45, 45, 255));
        ImGui::End();
        draw_document_windows();
        return;
    }

    // One tab per open document. Selecting a tab activates that document;
    // the close button asks about unsaved changes.
    if (!docs.empty() && ImGui::BeginTabBar("##docs", ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_FittingPolicyScroll)) {
        int close_request = -1, select_request = -1;
        for (int i = 0; i < static_cast<int>(docs.size()); ++i) {
            char label[300];
            std::snprintf(label, sizeof(label), "%s%s###doc%d", document_title(i).c_str(), document_modified(i) ? "*" : "", docs[i].uid);
            bool open = true;
            const ImGuiTabItemFlags flags = select_tab_request == i ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(label, &open, flags)) {
                if (i != current_doc && select_tab_request < 0) select_request = i;
                ImGui::EndTabItem();
            }
            if (!open) close_request = i;
        }
        select_tab_request = -1;
        ImGui::EndTabBar();
        if (select_request >= 0) activate_document(select_request);
        if (close_request >= 0) close_document(close_request);
    }
    draw_canvas_view(ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail());
    ImGui::End();
}

// Windowed mode: one floating window per document inside the workspace.
// The active document draws the full canvas; the others draw a read-only
// view from their own texture and become active when clicked or focused.
void App::draw_document_windows() {
    const int n = static_cast<int>(docs.size());
    int activate = -1, close = -1;
    const float chrome = ImGui::GetFrameHeight();  // title bar
    for (int i = 0; i < n; ++i) {
        DocState& s = docs[i];
        char label[300];
        std::snprintf(label, sizeof(label), "%s%s###docwin%d", document_title(i).c_str(), document_modified(i) ? "*" : "", s.uid);
        const firn::Document* d = i == current_doc ? doc.get() : s.doc.get();

        // Placement: an explicit arrangement, else a cascade slot for a new window.
        ImVec2 pos, size;
        bool place = false;
        if (arrange_request == Arrange::Cascade) {
            const float step = chrome + 6.0f;
            size = ImVec2(std::max(200.0f, workspace_size.x * 0.65f), std::max(150.0f, workspace_size.y * 0.65f));
            pos = ImVec2(workspace_pos.x + step * i, workspace_pos.y + step * i);
            place = true;
        } else if (arrange_request == Arrange::TileHorizontally || arrange_request == Arrange::TileVertically) {
            // Horizontally: rows of full width stacked top to bottom. Vertically: columns side by side.
            int major = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(n))));
            int minor = (n + major - 1) / major;
            if (arrange_request == Arrange::TileHorizontally) std::swap(major, minor);
            // major = columns, minor = rows
            const int col = i % major, row = i / major;
            const int cols_in_row = std::min(major, n - row * major);
            const float cw = workspace_size.x / cols_in_row, rh = workspace_size.y / minor;
            pos = ImVec2(workspace_pos.x + cw * col, workspace_pos.y + rh * row);
            size = ImVec2(cw, rh);
            place = true;
        } else if (!s.placed && d) {
            const float step = chrome + 6.0f;
            const int slot = i % 8;
            size = ImVec2(std::clamp(d->width() * 1.0f + 4.0f, 240.0f, workspace_size.x * 0.7f), std::clamp(d->height() * 1.0f + chrome + 4.0f, 180.0f, workspace_size.y * 0.7f));
            pos = ImVec2(workspace_pos.x + step * slot, workspace_pos.y + step * slot);
            place = true;
        }
        if (place) {
            ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(size, ImGuiCond_Always);
            s.placed = true;
            if (i == current_doc) fit_requested = true; else s.fit_requested = true;
        }
        if (select_tab_request == i) ImGui::SetNextWindowFocus();

        bool open = true;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(160, 120));
        const bool shown = ImGui::Begin(label, &open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar(2);
        if (shown) {
            const ImVec2 view_pos = ImGui::GetCursorScreenPos();
            const ImVec2 view_size = ImGui::GetContentRegionAvail();
            if (i == current_doc) {
                draw_canvas_view(view_pos, view_size);
            } else {
                draw_parked_view(s, view_pos, view_size);
                const bool clicked = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle));
                if (clicked) activate = i;
            }
        }
        ImGui::End();
        if (!open) close = i;
    }
    select_tab_request = -1;
    arrange_request = Arrange::None;
    if (activate >= 0) activate_document(activate);
    if (close >= 0) close_document(close);
}

// A parked document: its composite at its own zoom and pan, wheel to zoom,
// middle drag to pan. Any other click activates it (handled by the caller).
void App::draw_parked_view(DocState& s, ImVec2 view_pos, ImVec2 view_size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(view_pos, ImVec2(view_pos.x + view_size.x, view_pos.y + view_size.y), IM_COL32(60, 60, 60, 255));
    if (!s.doc || view_size.x <= 0 || view_size.y <= 0) return;
    upload_document_texture(s);
    const float img_w = static_cast<float>(s.doc->width()), img_h = static_cast<float>(s.doc->height());
    const ImVec2 center(view_pos.x + view_size.x * 0.5f, view_pos.y + view_size.y * 0.5f);
    if (s.fit_requested && view_size.x > 64.0f && view_size.y > 64.0f) {
        s.zoom = std::clamp(std::min(view_size.x / img_w, view_size.y / img_h) * 0.95f, 0.01f, 64.0f);
        s.pan_x = s.pan_y = 0.0f;
        s.fit_requested = false;
    }
    ImGui::SetCursorScreenPos(view_pos);
    ImGui::InvisibleButton("parked", view_size, ImGuiButtonFlags_MouseButtonMiddle);
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) {
        const float old_zoom = s.zoom;
        s.zoom = std::clamp(s.zoom * std::pow(1.15f, io.MouseWheel), 0.01f, 64.0f);
        const float k = s.zoom / old_zoom;
        const float mx = io.MousePos.x - center.x, my = io.MousePos.y - center.y;
        s.pan_x = mx - (mx - s.pan_x) * k;
        s.pan_y = my - (my - s.pan_y) * k;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) { s.pan_x += io.MouseDelta.x; s.pan_y += io.MouseDelta.y; }
    const float dw = img_w * s.zoom, dh = img_h * s.zoom;
    const ImVec2 p0(center.x + s.pan_x - dw * 0.5f, center.y + s.pan_y - dh * 0.5f);
    const ImVec2 p1(p0.x + dw, p0.y + dh);
    dl->PushClipRect(view_pos, ImVec2(view_pos.x + view_size.x, view_pos.y + view_size.y), true);
    dl->AddRectFilled(p0, p1, IM_COL32(175, 175, 175, 255));
    if (s.tex) dl->AddImage((ImTextureID)(intptr_t)s.tex, p0, p1);
    dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), IM_COL32(0, 0, 0, 255));
    dl->PopClipRect();
}

// Composite of a parked document as a texture (color managed like the
// active one). Parked documents do not change, so this uploads once.
void App::upload_document_texture(DocState& s) {
    if (!s.doc) return;
    if (s.tex && s.tex_revision == s.doc->revision()) return;
    firn::Image img = s.doc->composite();
    if (color_managed_display && !s.doc->icc().empty()) {
        const firn::icc::Profile p = firn::icc::parse(s.doc->icc());
        if (p.valid && p.matrix_trc && !p.is_srgb()) firn::icc::Transform(p, firn::icc::srgb()).apply(img);
    }
    if (!s.tex) {
        glGenTextures(1, &s.tex);
        glBindTexture(GL_TEXTURE_2D, s.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, s.tex);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width(), img.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, img.data());
    s.tex_revision = s.doc->revision();
}

// The active document's view: everything from rulers to marching ants,
// inside whatever window is current.
void App::draw_canvas_view(ImVec2 view_pos, ImVec2 view_size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Rulers take a strip along the top and left; the canvas view shrinks.
    const float ruler = show_rulers && doc ? 18.0f : 0.0f;
    const ImVec2 ruler_origin = view_pos;
    view_pos.x += ruler; view_pos.y += ruler;
    view_size.x -= ruler; view_size.y -= ruler;

    if (!doc || !canvas_tex || view_size.x <= 0 || view_size.y <= 0) {
        dl->AddRectFilled(view_pos, ImVec2(view_pos.x + view_size.x, view_pos.y + view_size.y), IM_COL32(60, 60, 60, 255));
        return;
    }

    const float img_w = static_cast<float>(doc->width());
    const float img_h = static_cast<float>(doc->height());
    canvas_center = ImVec2(view_pos.x + view_size.x * 0.5f, view_pos.y + view_size.y * 0.5f);

    // Defer the fit until the window has been laid out; on the first frame the
    // dock node has not been sized yet and the view is a few pixels wide.
    if (fit_requested && view_size.x > 64.0f && view_size.y > 64.0f) {
        zoom = std::min(view_size.x / img_w, view_size.y / img_h) * 0.95f;
        zoom = std::clamp(zoom, 0.01f, 64.0f);
        pan_x = pan_y = 0.0f;
        fit_requested = false;
    }

    // Input: the whole view is one invisible button so we get hover/drag state.
    ImGui::SetCursorScreenPos(view_pos);
    ImGui::InvisibleButton("canvas", view_size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                               ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImGuiIO& io = ImGui::GetIO();

    if (hovered && io.MouseWheel != 0.0f) zoom_about(io.MousePos, std::pow(1.15f, io.MouseWheel));

    const bool space = ImGui::IsKeyDown(ImGuiKey_Space);
    const bool pan_drag = ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                          ((tool().pans_with_left_drag() || space) && ImGui::IsMouseDragging(ImGuiMouseButton_Left));
    if (active && pan_drag) {
        pan_x += io.MouseDelta.x;
        pan_y += io.MouseDelta.y;
    }

    // Image rect in screen space.
    const float dw = img_w * zoom, dh = img_h * zoom;
    const ImVec2 p0(canvas_center.x + pan_x - dw * 0.5f, canvas_center.y + pan_y - dh * 0.5f);
    const ImVec2 p1(p0.x + dw, p0.y + dh);

    visible_image_rect = firn::raster::Rect{static_cast<int>(std::floor((view_pos.x - p0.x) / zoom)), static_cast<int>(std::floor((view_pos.y - p0.y) / zoom)),
                                      static_cast<int>(std::ceil((view_pos.x + view_size.x - p0.x) / zoom)), static_cast<int>(std::ceil((view_pos.y + view_size.y - p0.y) / zoom))}
                             .clipped(doc->width(), doc->height());

    // Tool dispatch. One gesture = one button held from press to release.
    ToolInput in;
    in.screen = io.MousePos;
    in.origin = p0;
    in.zoom = zoom;
    in.img_x = (io.MousePos.x - p0.x) / zoom;
    in.img_y = (io.MousePos.y - p0.y) / zoom;
    in.inside = in.img_x >= 0 && in.img_y >= 0 && in.img_x < img_w && in.img_y < img_h;
    in.dl = dl;

    if (tool().wants_snap() && (snap_to_guides || snap_to_grid)) snap_point(in.img_x, in.img_y);

    // Guides: drag out of a ruler to create, drag an existing one to move,
    // drop it back on a ruler to remove.
    if (show_guides && doc) {
        const bool over_top_ruler = ruler > 0 && io.MousePos.y >= ruler_origin.y && io.MousePos.y < view_pos.y && io.MousePos.x >= view_pos.x && io.MousePos.x < view_pos.x + view_size.x;
        const bool over_left_ruler = ruler > 0 && io.MousePos.x >= ruler_origin.x && io.MousePos.x < view_pos.x && io.MousePos.y >= view_pos.y && io.MousePos.y < view_pos.y + view_size.y;
        const bool window_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        if (guide_drag_kind == 0 && active_button < 0 && window_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const float tol = 5.0f;
            if (over_top_ruler) { guide_drag_kind = 1; guide_drag_index = -1; }
            else if (over_left_ruler) { guide_drag_kind = 2; guide_drag_index = -1; }
            else if (hovered) {
                for (size_t i = 0; i < guides_h.size(); ++i) if (std::abs(p0.y + guides_h[i] * zoom - io.MousePos.y) <= tol) { guide_drag_kind = 1; guide_drag_index = static_cast<int>(i); }
                for (size_t i = 0; i < guides_v.size(); ++i) if (std::abs(p0.x + guides_v[i] * zoom - io.MousePos.x) <= tol) { guide_drag_kind = 2; guide_drag_index = static_cast<int>(i); }
            }
            if (guide_drag_kind == 1 && guide_drag_index < 0) { guides_h.push_back(in.img_y); guide_drag_index = static_cast<int>(guides_h.size()) - 1; }
            if (guide_drag_kind == 2 && guide_drag_index < 0) { guides_v.push_back(in.img_x); guide_drag_index = static_cast<int>(guides_v.size()) - 1; }
        }
        if (guide_drag_kind != 0) {
            std::vector<float>& gv = guide_drag_kind == 1 ? guides_h : guides_v;
            if (guide_drag_index >= 0 && guide_drag_index < static_cast<int>(gv.size())) {
                gv[guide_drag_index] = std::round(guide_drag_kind == 1 ? (io.MousePos.y - p0.y) / zoom : (io.MousePos.x - p0.x) / zoom);
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    const bool on_ruler = guide_drag_kind == 1 ? io.MousePos.y < view_pos.y : io.MousePos.x < view_pos.x;
                    if (on_ruler) gv.erase(gv.begin() + guide_drag_index);
                    guide_drag_kind = 0; guide_drag_index = -1;
                }
            } else { guide_drag_kind = 0; guide_drag_index = -1; }
        }
    }
    const bool guide_busy = guide_drag_kind != 0;

    const bool tool_takes_left = !tool().pans_with_left_drag() && !space && !guide_busy;
    if (active_button < 0 && hovered && !guide_busy) {
        if (tool_takes_left && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) active_button = ImGuiMouseButton_Left;
        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) active_button = ImGuiMouseButton_Right;
        if (active_button >= 0) { note_recent_color(active_button == ImGuiMouseButton_Left ? fg_color : bg_color); tool().on_press(*this, in, static_cast<ImGuiMouseButton>(active_button)); }
    } else if (active_button >= 0) {
        const auto b = static_cast<ImGuiMouseButton>(active_button);
        if (ImGui::IsMouseDown(b)) {
            if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) tool().on_drag(*this, in, b);
        } else {
            tool().on_release(*this, in, b);
            active_button = -1;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && active_button >= 0) {
        tool().cancel(*this);
        active_button = -1;
    }

    dl->PushClipRect(view_pos, ImVec2(view_pos.x + view_size.x, view_pos.y + view_size.y), true);
    dl->AddRectFilled(view_pos, ImVec2(view_pos.x + view_size.x, view_pos.y + view_size.y), IM_COL32(60, 60, 60, 255));

    // Checkerboard for transparency.
    {
        const float cell = static_cast<float>(config.checker_size);
        const float x0 = std::max(p0.x, view_pos.x), y0 = std::max(p0.y, view_pos.y);
        const float x1 = std::min(p1.x, view_pos.x + view_size.x), y1 = std::min(p1.y, view_pos.y + view_size.y);
        int row = 0;
        for (float y = y0 - std::fmod(y0 - p0.y, cell); y < y1; y += cell, ++row) {
            int col = row;
            for (float x = x0 - std::fmod(x0 - p0.x, cell); x < x1; x += cell, ++col) {
                ImU32 c = (col % 2 == 0) ? IM_COL32(200, 200, 200, 255) : IM_COL32(150, 150, 150, 255);
                dl->AddRectFilled(ImVec2(std::max(x, x0), std::max(y, y0)),
                                  ImVec2(std::min(x + cell, x1), std::min(y + cell, y1)), c);
            }
        }
    }

    dl->AddImage((ImTextureID)(intptr_t)canvas_tex, p0, p1);
    sync_overlay_texture();
    if (mask_edit && show_mask_overlay && overlay_tex && overlay_tex_revision == doc->revision())
        dl->AddImage((ImTextureID)(intptr_t)overlay_tex, p0, p1);
    dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), IM_COL32(0, 0, 0, 255));

    // Grid: image-space lines every grid_spacing pixels, once they are far enough apart.
    if (show_grid && grid_spacing > 0 && grid_spacing * zoom >= 4.0f) {
        const ImU32 gc = IM_COL32(0, 0, 0, 90);
        for (int gx = 0; gx <= doc->width(); gx += grid_spacing) {
            const float sx = p0.x + gx * zoom;
            if (sx >= view_pos.x && sx <= view_pos.x + view_size.x) dl->AddLine(ImVec2(sx, std::max(p0.y, view_pos.y)), ImVec2(sx, std::min(p1.y, view_pos.y + view_size.y)), gc);
        }
        for (int gy = 0; gy <= doc->height(); gy += grid_spacing) {
            const float sy = p0.y + gy * zoom;
            if (sy >= view_pos.y && sy <= view_pos.y + view_size.y) dl->AddLine(ImVec2(std::max(p0.x, view_pos.x), sy), ImVec2(std::min(p1.x, view_pos.x + view_size.x), sy), gc);
        }
    }
    if ((hovered || active_button >= 0 || tool().overlay_always()) && !guide_busy) tool().draw_overlay(*this, in);

    // Guides.
    if (show_guides) {
        const ImU32 gc = IM_COL32(0, 160, 255, 200);
        for (float g : guides_h) { const float sy = p0.y + g * zoom; if (sy >= view_pos.y && sy <= view_pos.y + view_size.y) dl->AddLine(ImVec2(view_pos.x, sy), ImVec2(view_pos.x + view_size.x, sy), gc); }
        for (float g : guides_v) { const float sx = p0.x + g * zoom; if (sx >= view_pos.x && sx <= view_pos.x + view_size.x) dl->AddLine(ImVec2(sx, view_pos.y), ImVec2(sx, view_pos.y + view_size.y), gc); }
    }

    // Marching ants along the selection boundary. Each unit edge is one
    // segment; color alternates along the outline and cycles with time.
    sync_ants();
    if (!ants.empty()) {
        const int phase = static_cast<int>(ImGui::GetTime() * 10.0);
        const float vx0 = view_pos.x, vy0 = view_pos.y, vx1 = view_pos.x + view_size.x, vy1 = view_pos.y + view_size.y;
        for (const Edge& e : ants) {
            const float sx = p0.x + e.x * zoom, sy = p0.y + e.y * zoom;
            const float ex = e.horizontal ? sx + zoom : sx, ey = e.horizontal ? sy : sy + zoom;
            if (std::max(sx, ex) < vx0 || std::min(sx, ex) > vx1 || std::max(sy, ey) < vy0 || std::min(sy, ey) > vy1) continue;
            const ImU32 col = (((e.x + e.y + phase) / 4) & 1) ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255);
            dl->AddLine(ImVec2(sx, sy), ImVec2(ex, ey), col, 1.0f);
        }
    }
    dl->PopClipRect();

    // Rulers, drawn last so they sit over the canvas edge.
    if (ruler > 0.0f) {
        const ImU32 bg = IM_COL32(40, 40, 40, 255), fg = IM_COL32(200, 200, 200, 255);
        const ImVec2 r0 = ruler_origin;
        dl->AddRectFilled(r0, ImVec2(r0.x + view_size.x + ruler, r0.y + ruler), bg);
        dl->AddRectFilled(r0, ImVec2(r0.x + ruler, r0.y + view_size.y + ruler), bg);
        // Pick a tick step in image pixels giving >= 60 screen px between labels.
        int step = 1;
        static const int steps[] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000};
        for (int st : steps) { step = st; if (st * zoom >= 60.0f) break; }
        const int minor = std::max(1, step / 5);
        auto label = [&](int v, ImVec2 at) { char b[16]; std::snprintf(b, sizeof(b), "%d", v); dl->AddText(at, fg, b); };
        const int first_x = static_cast<int>(std::floor((view_pos.x - p0.x) / zoom / minor)) * minor;
        const int last_x = static_cast<int>(std::ceil((view_pos.x + view_size.x - p0.x) / zoom));
        for (int v = first_x; v <= last_x; v += minor) {
            const float sx = p0.x + v * zoom;
            if (sx < view_pos.x) continue;
            const bool major = v % step == 0;
            dl->AddLine(ImVec2(sx, r0.y + (major ? 4.0f : 12.0f)), ImVec2(sx, r0.y + ruler), fg);
            if (major) label(v, ImVec2(sx + 2, r0.y + 1));
        }
        const int first_y = static_cast<int>(std::floor((view_pos.y - p0.y) / zoom / minor)) * minor;
        const int last_y = static_cast<int>(std::ceil((view_pos.y + view_size.y - p0.y) / zoom));
        for (int v = first_y; v <= last_y; v += minor) {
            const float sy = p0.y + v * zoom;
            if (sy < view_pos.y) continue;
            const bool major = v % step == 0;
            dl->AddLine(ImVec2(r0.x + (major ? 4.0f : 12.0f), sy), ImVec2(r0.x + ruler, sy), fg);
            if (major) label(v, ImVec2(r0.x + 1, sy + 1));
        }
        // Cursor position markers.
        if (hovered) {
            dl->AddLine(ImVec2(io.MousePos.x, r0.y), ImVec2(io.MousePos.x, r0.y + ruler), IM_COL32(255, 120, 0, 255));
            dl->AddLine(ImVec2(r0.x, io.MousePos.y), ImVec2(r0.x + ruler, io.MousePos.y), IM_COL32(255, 120, 0, 255));
        }
    }

    // Cursor facts for the status bar.
    cursor_inside = hovered && in.inside;
    cursor_x = static_cast<int>(std::floor(in.img_x));
    cursor_y = static_cast<int>(std::floor(in.img_y));
}
