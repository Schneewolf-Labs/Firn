// Autosave and crash recovery. Every few minutes (Preferences), each
// modified document is snapshotted and written on a background thread to
// <config>/autosave/<uid>.ora with a sidecar naming its title and
// path. A normal save or close removes the file; at startup, leftover
// files mean a crash, and a prompt offers to reopen them.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include "App.h"
#include "firn/io_psp.h"
#include "imgui.h"

namespace fs = std::filesystem;
using namespace firn;

namespace {

std::atomic<int> saves_in_flight{0};

std::string autosave_dir() {
    const fs::path dir = fs::path(Config::directory()) / "autosave";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir.string();
}

// Files are named by a per-run session id plus the document id, so copies
// left by an earlier run never collide with this run's.
const std::string kSession = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
std::string key_for(int uid) { return kSession + "-" + std::to_string(uid); }
std::string autosave_path(const std::string& key) { return autosave_dir() + "/" + key + ".ora"; }
std::string sidecar_path(const std::string& key) { return autosave_dir() + "/" + key + ".txt"; }

}  // namespace

void App::autosave_forget(const std::string& key) {
    std::error_code ec;
    fs::remove(autosave_path(key), ec);
    fs::remove(sidecar_path(key), ec);
}

void App::autosave_forget(int uid) { autosave_forget(key_for(uid)); }

// Runs once per frame; cheap unless a save is due.
void App::autosave_tick() {
    if (config.autosave_minutes <= 0 || docs.empty()) return;
    const double now = ImGui::GetTime();
    if (now - autosave_last < config.autosave_minutes * 60.0) return;
    // Not in the middle of a gesture or a dialog, and no save still running.
    if (ImGui::IsAnyMouseDown() || preview.active || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || saves_in_flight.load() > 0) return;
    autosave_last = now;
    int written = 0;
    for (int i = 0; i < static_cast<int>(docs.size()); ++i) {
        const bool current = i == current_doc;
        const Document* d = current ? doc.get() : docs[i].doc.get();
        const size_t cursor = current ? history.cursor() : docs[i].history.cursor();
        if (!d || !document_modified(i) || docs[i].autosave_cursor == cursor) continue;
        docs[i].autosave_cursor = cursor;
        // Sidecar first (tiny), then the pixels on a worker thread from a snapshot.
        {
            std::ofstream side(sidecar_path(key_for(docs[i].uid)));
            side << document_title(i) << "\n" << (current ? doc_path : docs[i].doc_path) << "\n";
        }
        auto copy = std::make_shared<Document>(d->width(), d->height());
        copy->restore(d->snapshot());
        const std::string path = autosave_path(key_for(docs[i].uid));
        ++saves_in_flight;
        std::thread([copy, path] {
            std::string err;
            const std::string tmp = path + ".part";
            if (io::save_psp(*copy, tmp, &err)) { std::error_code ec; fs::rename(tmp, path, ec); }
            --saves_in_flight;
        }).detach();
        ++written;
    }
    if (written) status = "Autosaved " + std::to_string(written) + (written == 1 ? " image" : " images");
}

// Startup: leftover autosave files are offered for recovery.
void App::check_recovery() {
    recover_files.clear();
    recovery_error.clear();
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(autosave_dir(), fs::directory_options::skip_permission_denied, ec)) {
        if (!de.is_regular_file(ec) || (de.path().extension() != ".ora" && de.path().extension() != ".pspimage")) continue;
        RecoverEntry e;
        e.file = de.path().string();
        e.key = de.path().stem().string();
        std::ifstream side(sidecar_path(e.key));
        std::getline(side, e.title);
        std::getline(side, e.original_path);
        if (e.title.empty()) e.title = de.path().filename().string();
        recover_files.push_back(std::move(e));
    }
    show_recovery_dialog = !recover_files.empty();
}

void App::draw_recovery_dialog() {
    if (show_recovery_dialog && !ImGui::IsPopupOpen("Recover Unsaved Work")) ImGui::OpenPopup("Recover Unsaved Work");
    if (!show_recovery_dialog) return;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    const ImVec2 space = ImGui::GetMainViewport()->WorkSize;
    const float width = std::min(640.0f * ui_scale, std::max(300.0f, space.x - 32));
    ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(width, std::max(120.0f, space.y - 32)));
    if (!ImGui::BeginPopupModal("Recover Unsaved Work", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted("Unsaved recovery copies are available for these images:");
    ImGui::TextUnformatted("Recover opens them as unsaved documents. Your original files stay unchanged.");
    for (const RecoverEntry& e : recover_files) ImGui::BulletText("%s%s%s", e.title.c_str(), e.original_path.empty() ? "" : "  (", e.original_path.empty() ? "" : (e.original_path + ")").c_str());
    ImGui::Spacing();
    if (ImGui::Button("Recover images", ImVec2(0, 0))) {
        recovery_error.clear();
        std::vector<RecoverEntry> failed;
        for (const RecoverEntry& e : recover_files) {
            std::string err; std::vector<std::string> warnings;
            auto d = io::load_psp(e.file, &err, &warnings);
            if (!d) { status = "Could not recover " + e.title + ": " + err; recovery_error += status + "\n"; failed.push_back(e); continue; }
            add_document(std::move(d), e.original_path);
            if (e.original_path.empty()) doc_title = e.title;
            saved_cursor = static_cast<size_t>(-1);  // recovered work counts as unsaved
            autosave_forget(e.key);
        }
        recover_files = std::move(failed);
        show_recovery_dialog = !recover_files.empty();
        if (!show_recovery_dialog) ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete recovery copies", ImVec2(0, 0))) {
        for (const RecoverEntry& e : recover_files) autosave_forget(e.key);
        recover_files.clear();
        show_recovery_dialog = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Start without recovering", ImVec2(0, 0))) { show_recovery_dialog = false; ImGui::CloseCurrentPopup(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep recovery copies. Review them from the start screen or next time Firn opens.");
    if (!recovery_error.empty()) ImGui::TextWrapped("%s", recovery_error.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
}
