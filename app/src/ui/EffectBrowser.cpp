// Effects > Effect Browser: a grid of every adjustment and effect applied
// with its current settings to a thumbnail of the active layer. Clicking a
// tile opens that dialog. The operations come from the dialogs themselves
// (adjust_modal records them while the browser is open), so nothing is
// duplicated; a few thumbnails are rendered per frame to stay responsive.
#include <algorithm>
#include <cstring>

#include <SDL_opengl.h>

#include "App.h"
#include "firn/raster.h"
#include "imgui.h"

using namespace firn;

namespace {
const float kTile = 112.0f;
}

void App::reset_effect_browser() {
    for (GLuint t : browser_tex) if (t) glDeleteTextures(1, &t);
    browser_tex.assign(static_cast<size_t>(adjust_count()), 0);
    browser_state.assign(static_cast<size_t>(adjust_count()), 0);
    browser_source = Image();
    browser_revision = ~0ull;
    browser_layer = -1;
}

void App::draw_effect_browser() {
    effect_capture = show_effect_browser;
    if (!show_effect_browser) return;
    if (!doc || active_layer() < 0 || !doc->layer(active_layer()).is_raster()) { show_effect_browser = false; effect_ops.clear(); return; }
    if (!ImGui::IsPopupOpen("Effect Browser")) ImGui::OpenPopup("Effect Browser");
    ImGui::SetNextWindowSize(ImVec2(860, 620), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Effect Browser", nullptr, ImGuiWindowFlags_NoScrollbar)) return;

    // Source thumbnail: the active layer, scaled to the tile.
    if (browser_source.empty() || browser_revision != doc->revision() || browser_layer != active_layer()) {
        const Image& px = doc->layer(active_layer()).pixels;
        const float k = std::min(kTile / std::max(1, px.width()), kTile / std::max(1, px.height()));
        const int w = std::max(1, static_cast<int>(px.width() * k)), h = std::max(1, static_cast<int>(px.height() * k));
        browser_source = raster::resample(px, w, h, raster::Filter::Bilinear);
        browser_revision = doc->revision();
        browser_layer = active_layer();
        for (GLuint& t : browser_tex) if (t) { glDeleteTextures(1, &t); t = 0; }
        std::fill(browser_state.begin(), browser_state.end(), 0);
    }

    static char filter[64] = {};
    ImGui::SetNextItemWidth(240);
    ImGui::InputTextWithHint("##filter", "Filter by name", filter, sizeof(filter));
    ImGui::SameLine();
    int done = 0;
    for (uint8_t st : browser_state) done += st != 0;
    ImGui::TextDisabled("%d of %d rendered. Click a tile to open its dialog; each uses its current settings.", done, adjust_count() - 1);

    std::string want = filter;
    for (char& ch : want) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const float footer = ImGui::GetFrameHeightWithSpacing() + 8;
    int chosen = -1;
    int rendered_now = 0;
    if (ImGui::BeginChild("tiles", ImVec2(0, -footer), ImGuiChildFlags_Borders)) {
        const int per_row = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (kTile + 12)));
        int col = 0;
        for (int i = 1; i < adjust_count(); ++i) {
            const char* title = adjust_title(i);
            std::string low = title;
            for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (!want.empty() && low.find(want) == std::string::npos) continue;
            // Render this tile's thumbnail (a couple per frame).
            if (browser_state[i] == 0 && rendered_now < 2) {
                auto it = effect_ops.find(title);
                if (it == effect_ops.end()) {
                    // The dialog has not registered yet (first frame); try next frame.
                } else {
                    ++rendered_now;
                    Image img = browser_source;
                    it->second(img);
                    if (img.width() != browser_source.width() || img.height() != browser_source.height()) img = raster::resample(img, browser_source.width(), browser_source.height(), raster::Filter::Bilinear);
                    GLuint t = 0;
                    glGenTextures(1, &t);
                    glBindTexture(GL_TEXTURE_2D, t);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width(), img.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, img.data());
                    browser_tex[i] = t;
                    browser_state[i] = 1;
                }
            }
            ImGui::PushID(i);
            ImGui::BeginGroup();
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const ImVec2 tile(kTile + 8, kTile + 8 + ImGui::GetTextLineHeight() + 4);
            if (ImGui::InvisibleButton("##tile", tile)) chosen = i;
            const bool hov = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, ImVec2(p0.x + tile.x, p0.y + tile.y), hov ? ImGui::GetColorU32(ImGuiCol_HeaderHovered) : ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
            const ImVec2 img0(p0.x + 4 + (kTile - browser_source.width()) * 0.5f, p0.y + 4 + (kTile - browser_source.height()) * 0.5f);
            if (browser_tex[i]) dl->AddImage((ImTextureID)(intptr_t)browser_tex[i], img0, ImVec2(img0.x + browser_source.width(), img0.y + browser_source.height()));
            else dl->AddRectFilled(img0, ImVec2(img0.x + browser_source.width(), img0.y + browser_source.height()), IM_COL32(80, 80, 80, 255));
            // Name, trimmed to the tile.
            std::string label = title;
            while (!label.empty() && ImGui::CalcTextSize(label.c_str()).x > kTile) label.pop_back();
            if (label.size() < std::strlen(title)) { if (label.size() > 2) label.resize(label.size() - 2); label += ".."; }
            dl->AddText(ImVec2(p0.x + 4 + (kTile - ImGui::CalcTextSize(label.c_str()).x) * 0.5f, p0.y + kTile + 8), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
            if (hov) ImGui::SetTooltip("%s", title);
            ImGui::EndGroup();
            ImGui::PopID();
            if (++col % per_row != 0) ImGui::SameLine();
        }
    }
    ImGui::EndChild();
    if (ImGui::Button("Close", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) chosen = 0;
    if (chosen >= 0) {
        show_effect_browser = false;
        effect_capture = false;
        effect_ops.clear();
        for (GLuint& t : browser_tex) if (t) { glDeleteTextures(1, &t); t = 0; }
        ImGui::CloseCurrentPopup();
        if (chosen > 0) open_adjust = static_cast<Adj>(chosen);
    }
    ImGui::EndPopup();
}
