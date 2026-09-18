// Ctrl+K: type a few letters of anything the menus can do and press Return.
// The list comes from walking the real menu tree through
// CollectorMenuBuilder, so it cannot fall behind what the menus offer.
#include <algorithm>
#include <cctype>
#include <cstring>

#include "App.h"
#include "imgui.h"
#include "ui/CollectorMenuBuilder.h"
#include "ui/PaletteState.h"
#include "ui/Shortcut.h"

using namespace firn;

namespace {

// Subsequence matching with a score: letters in order, rewarding matches
// that start a word and runs that stay together, so "mosgl" finds
// "Mosaic - Glass" and "nwl" finds "New Raster Layer". Returns false when
// the needle is not a subsequence at all.
bool fuzzy(const std::string& haystack, const std::string& needle, int* score, std::vector<int>* hits) {
    if (needle.empty()) { *score = 0; return true; }
    int s = 0, run = 0;
    size_t h = 0;
    hits->clear();
    for (size_t n = 0; n < needle.size(); ++n) {
        const char want = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[n])));
        bool found = false;
        while (h < haystack.size()) {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(haystack[h])));
            if (c == want) {
                const bool word_start = h == 0 || haystack[h - 1] == ' ' || haystack[h - 1] == '>' || haystack[h - 1] == '-';
                s += word_start ? 12 : 3;
                run = run ? run + 1 : 1;
                s += run * 2;
                hits->push_back(static_cast<int>(h));
                ++h;
                found = true;
                break;
            }
            run = 0;
            ++h;
        }
        if (!found) return false;
    }
    // A short path that matched is usually the one meant.
    s -= static_cast<int>(haystack.size()) / 8;
    *score = s;
    return true;
}

}  // namespace

void App::open_command_palette() {
    palette_state->cmd_query[0] = 0;
    palette_state->cmd_selected = 0;
    show_command_palette = true;
}

void App::draw_command_palette() {
    if (show_command_palette) {
        // Collect on the way in: the menu predicates read the document, so
        // what is enabled reflects the moment the palette was opened.
        CollectorMenuBuilder collector;
        draw_menu(collector);
        palette_state->cmd_entries.clear();
        for (CollectorMenuBuilder::Entry& e : collector.entries)
            palette_state->cmd_entries.push_back({e.path, e.shortcut, e.enabled, std::move(e.action)});
        ImGui::OpenPopup("Command Palette");
        show_command_palette = false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.22f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_Always);
    if (!ImGui::BeginPopup("Command Palette", ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) return;

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool submitted = ImGui::InputTextWithHint("##cmd", "Search menus", palette_state->cmd_query,
                                                    sizeof palette_state->cmd_query, ImGuiInputTextFlags_EnterReturnsTrue);

    // Rank what matches. Disabled entries stay in the list, greyed, so a
    // search never silently comes up empty because of the current selection.
    struct Hit { int score; size_t index; };
    std::vector<Hit> hits;
    const std::string query = palette_state->cmd_query;
    std::vector<int> where;
    for (size_t i = 0; i < palette_state->cmd_entries.size(); ++i) {
        int score = 0;
        if (!fuzzy(palette_state->cmd_entries[i].path, query, &score, &where)) continue;
        if (!palette_state->cmd_entries[i].enabled) score -= 40;
        hits.push_back({score, i});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    if (hits.size() > 60) hits.resize(60);

    if (hits.empty()) palette_state->cmd_selected = 0;
    else palette_state->cmd_selected = std::clamp(palette_state->cmd_selected, 0, static_cast<int>(hits.size()) - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) palette_state->cmd_selected = std::min(palette_state->cmd_selected + 1, static_cast<int>(hits.size()) - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) palette_state->cmd_selected = std::max(palette_state->cmd_selected - 1, 0);

    std::function<void()> run_now;
    ImGui::BeginChild("##cmdlist", ImVec2(0, 320), false);
    for (size_t k = 0; k < hits.size(); ++k) {
        const auto& e = palette_state->cmd_entries[hits[k].index];
        ImGui::PushID(static_cast<int>(k));
        const bool current = static_cast<int>(k) == palette_state->cmd_selected;
        if (!e.enabled) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        if (ImGui::Selectable(e.path.c_str(), current, ImGuiSelectableFlags_AllowDoubleClick) && e.enabled) run_now = e.action;
        if (!e.enabled) ImGui::PopStyleColor();
        if (!e.shortcut.empty()) {
            const char* sc = SC(e.shortcut.c_str());
            const float w = ImGui::CalcTextSize(sc).x;
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - w);
            ImGui::TextDisabled("%s", sc);
        }
        if (current && (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) || ImGui::IsKeyPressed(ImGuiKey_UpArrow, false))) ImGui::SetScrollHereY(0.5f);
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (submitted && !hits.empty()) {
        const auto& e = palette_state->cmd_entries[hits[static_cast<size_t>(palette_state->cmd_selected)].index];
        if (e.enabled) run_now = e.action;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    if (run_now) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        // After the popup is closed: an action may open another modal, close
        // the document or replace the layer stack.
        run_now();
        return;
    }
    ImGui::EndPopup();
}
