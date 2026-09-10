#include "ui/FileDialog.h"

#include <SDL_opengl.h>

#include "firn/io.h"
#include "firn/io_psp.h"
#include "firn/raster.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "imgui.h"

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extension_of(const std::string& name) {
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return {};
    return lower(name.substr(dot + 1));
}

fs::path home_dir() {
    if (const char* h = std::getenv("HOME")) return h;
    if (const char* h = std::getenv("USERPROFILE")) return h;
    return fs::current_path();
}

std::string human_size(uintmax_t n) {
    char buf[32];
    if (n < 1024) std::snprintf(buf, sizeof(buf), "%ju B", n);
    else if (n < 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", n / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%.1f MB", n / (1024.0 * 1024.0));
    return buf;
}

std::string human_time(fs::file_time_type t) {
    // file_time_type -> system_clock is only portable via clock_cast (C++20),
    // which some libstdc++ versions lack; fall back to the epoch offset trick.
    using namespace std::chrono;
    const auto sys = time_point_cast<system_clock::duration>(t - fs::file_time_type::clock::now() + system_clock::now());
    const std::time_t tt = system_clock::to_time_t(sys);
    char buf[32];
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

}  // namespace

void FileDialog::open(Mode mode, std::string title, std::vector<std::string> extensions, const std::string& initial_path) {
    mode_ = mode;
    title_ = std::move(title);
    exts_ = std::move(extensions);
    result_.clear();
    error_.clear();
    selected_ = -1;
    open_requested_ = true;

    fs::path init = initial_path.empty() ? fs::path() : fs::path(initial_path);
    std::error_code ec;
    type_ = 0;
    if (!init.empty()) set_type_from_name(init.filename().string());
    if (!init.empty() && fs::is_directory(init, ec)) {
        set_dir(init);
        name_buf_[0] = 0;
    } else if (!init.empty() && init.has_parent_path() && fs::is_directory(init.parent_path(), ec)) {
        set_dir(init.parent_path());
        std::snprintf(name_buf_, sizeof(name_buf_), "%s", init.filename().string().c_str());
    } else {
        if (dir_.empty()) set_dir(fs::current_path(ec));
        else refresh();
        if (mode_ == Mode::Save && !init.empty()) std::snprintf(name_buf_, sizeof(name_buf_), "%s", init.filename().string().c_str());
        else name_buf_[0] = 0;
    }
}

void FileDialog::set_directory(const std::string& dir) {
    std::error_code ec;
    if (!dir.empty() && fs::is_directory(dir, ec)) dir_ = dir;
}

void FileDialog::set_dir(const fs::path& dir) {
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(dir, ec);
    dir_ = ec ? dir : canon;
    std::snprintf(dir_buf_, sizeof(dir_buf_), "%s", dir_.string().c_str());
    selected_ = -1;
    refresh();
}

void FileDialog::free_thumbnails() {
    for (Entry& e : entries_)
        if (e.thumb) { GLuint t = e.thumb; glDeleteTextures(1, &t); e.thumb = 0; }
}

// Decodes one file into a small texture (once per entry; failures are
// remembered so a bad file is not retried every frame).
void FileDialog::load_thumbnail(Entry& e) {
    e.thumb_tried = true;
    const fs::path path = dir_ / e.name;
    if (e.size > 64u * 1024 * 1024) return;  // too big to decode for a thumbnail
    std::optional<firn::Image> img;
    std::string err;
    if (firn::io::is_psp_extension(path.string()) || firn::io::is_ora_extension(path.string()) || firn::io::is_psd_extension(path.string())) {
        std::vector<std::string> warnings;
        auto d = firn::io::load_document(path.string(), &err, &warnings);
        if (d) img = d->composite();
    } else {
        img = firn::io::load(path.string(), &err);
    }
    if (!img || img->empty()) return;
    e.image_w = img->width(); e.image_h = img->height();
    const float scale = std::min(1.0f, std::min(96.0f / img->width(), 96.0f / img->height()));
    const int tw = std::max(1, static_cast<int>(img->width() * scale)), th = std::max(1, static_cast<int>(img->height() * scale));
    firn::Image small = scale < 1.0f ? firn::raster::resample(*img, tw, th, firn::raster::Filter::Bilinear) : *img;
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, small.width(), small.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, small.data());
    e.thumb = t; e.thumb_w = small.width(); e.thumb_h = small.height();
}

void FileDialog::refresh() {
    free_thumbnails();
    entries_.clear();
    error_.clear();
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(dir_, fs::directory_options::skip_permission_denied, ec)) {
        Entry e;
        e.name = de.path().filename().string();
        if (!show_hidden_ && !e.name.empty() && e.name[0] == '.') continue;
        std::error_code ec2;
        e.is_dir = de.is_directory(ec2);
        if (!e.is_dir && !matches_filter(e.name)) continue;
        if (!e.is_dir) e.size = de.file_size(ec2);
        e.modified = de.last_write_time(ec2);
        entries_.push_back(std::move(e));
    }
    if (ec) error_ = "Cannot read directory: " + ec.message();
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        return lower(a.name) < lower(b.name);
    });
}

namespace {

// Display names for the "Save as type" list; anything else shows its extension.
const char* type_label(const std::string& ext) {
    if (ext == "ora") return "OpenRaster image (*.ora)";
    if (ext == "pspimage") return "Classic image (*.pspimage)";
    if (ext == "png") return "PNG (*.png)";
    if (ext == "jpg") return "JPEG (*.jpg, *.jpeg)";
    if (ext == "jpeg") return nullptr;  // folded into jpg
    if (ext == "webp") return "WebP (*.webp)";
    if (ext == "bmp") return "Windows bitmap (*.bmp)";
    if (ext == "tga") return "Targa (*.tga)";
    return ext.c_str();
}

bool same_type(const std::string& a, const std::string& b) {
    return a == b || (a == "jpg" && b == "jpeg") || (a == "jpeg" && b == "jpg");
}

}  // namespace

bool FileDialog::matches_filter(const std::string& name) const {
    if (show_all_ || exts_.empty()) return true;
    const std::string ext = extension_of(name);
    if (mode_ == Mode::Save && type_ >= 0 && type_ < static_cast<int>(exts_.size())) return same_type(ext, exts_[type_]);
    return std::find(exts_.begin(), exts_.end(), ext) != exts_.end();
}

void FileDialog::set_type_from_name(const std::string& name) {
    const std::string ext = extension_of(name);
    for (size_t i = 0; i < exts_.size(); ++i)
        if (same_type(ext, exts_[i]) && type_label(exts_[i])) { type_ = static_cast<int>(i); return; }
}

// Replaces (or adds) the extension in the name field to match the chosen type.
void FileDialog::apply_type_to_name() {
    if (type_ < 0 || type_ >= static_cast<int>(exts_.size())) return;
    std::string name = name_buf_;
    if (name.empty()) return;
    const std::string ext = extension_of(name);
    if (std::find(exts_.begin(), exts_.end(), ext) != exts_.end()) name.erase(name.size() - ext.size() - 1);
    name += "." + exts_[type_];
    std::snprintf(name_buf_, sizeof(name_buf_), "%s", name.c_str());
}

// Resolve what the user typed or picked into a final path. Returns false
// (with error_ set, or after navigating) when the dialog should stay open.
bool FileDialog::accept(const std::string& typed) {
    if (typed.empty()) return false;
    fs::path p = typed;
    if (p.is_relative()) p = dir_ / p;
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        set_dir(p);
        name_buf_[0] = 0;
        return false;
    }
    if (mode_ == Mode::Open) {
        if (!fs::exists(p, ec)) { error_ = "File not found: " + p.filename().string(); return false; }
    } else {
        // A typed extension the app can write wins; otherwise the chosen type applies.
        const std::string ext = extension_of(p.filename().string());
        if (std::find(exts_.begin(), exts_.end(), ext) == exts_.end() && !exts_.empty())
            p += "." + exts_[type_ >= 0 && type_ < static_cast<int>(exts_.size()) ? type_ : 0];
        if (!fs::is_directory(p.parent_path(), ec)) { error_ = "No such folder: " + p.parent_path().string(); return false; }
    }
    result_ = p.string();
    return true;
}

bool FileDialog::draw() {
    if (open_requested_) {
        ImGui::OpenPopup(title_.c_str());
        open_requested_ = false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(std::min(760.0f, vp->WorkSize.x * 0.8f), std::min(520.0f, vp->WorkSize.y * 0.8f)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title_.c_str(), nullptr, ImGuiWindowFlags_NoScrollbar)) return false;

    bool accepted = false;
    std::error_code ec;

    // Toolbar: up, home, path box.
    if (ImGui::Button("Up") && dir_.has_parent_path() && dir_.parent_path() != dir_) set_dir(dir_.parent_path());
    ImGui::SameLine();
    if (ImGui::Button("Home")) set_dir(home_dir());
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) refresh();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##dir", dir_buf_, sizeof(dir_buf_), ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (fs::is_directory(dir_buf_, ec)) set_dir(dir_buf_);
        else error_ = "No such folder";
    }

    // Listing, with a preview pane of the selected image at the right.
    const float footer = ImGui::GetFrameHeightWithSpacing() * (mode_ == Mode::Save ? 3 : 2) + ImGui::GetTextLineHeightWithSpacing() + 8;
    const float preview_w = 180.0f;
    const float row_h = show_thumbs_ ? 40.0f : 0.0f;
    int decoded = 0;  // thumbnails decoded this frame; a couple keeps the dialog responsive
    if (ImGui::BeginTable("files", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
                          ImVec2(-preview_w, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(entries_.size()), show_thumbs_ ? row_h + 2.0f : 0.0f);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                Entry& e = entries_[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                const std::string label = e.is_dir ? "[" + e.name + "]" : e.name;
                if (show_thumbs_ && !e.is_dir && !e.thumb_tried && decoded < 2 && matches_filter(e.name)) { load_thumbnail(e); ++decoded; }
                const ImVec2 row0 = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable(show_thumbs_ ? "##row" : label.c_str(), selected_ == i,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, row_h))) {
                    selected_ = i;
                    if (!e.is_dir) std::snprintf(name_buf_, sizeof(name_buf_), "%s", e.name.c_str());
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (e.is_dir) { set_dir(dir_ / e.name); ImGui::PopID(); break; }
                        if (accept(e.name)) accepted = true;
                    }
                }
                if (show_thumbs_) {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    if (e.thumb) {
                        const float k = std::min(row_h / e.thumb_w, row_h / e.thumb_h);
                        const float w = e.thumb_w * k, h = e.thumb_h * k;
                        const ImVec2 p0(row0.x + 2 + (row_h - w) * 0.5f, row0.y + (row_h - h) * 0.5f);
                        dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(90, 90, 90, 255));
                        dl->AddImage((ImTextureID)(intptr_t)e.thumb, p0, ImVec2(p0.x + w, p0.y + h));
                    } else if (e.is_dir) {
                        const ImVec2 f0(row0.x + 8, row0.y + 12), f1(row0.x + row_h - 4, row0.y + row_h - 8);
                        dl->AddRectFilled(f0, f1, IM_COL32(200, 170, 80, 255), 2.0f);
                        dl->AddRectFilled(f0, ImVec2(f0.x + (f1.x - f0.x) * 0.45f, f0.y - 4), IM_COL32(200, 170, 80, 255), 2.0f);
                    }
                    dl->AddText(ImVec2(row0.x + row_h + 8, row0.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
                }
                const float text_dy = show_thumbs_ ? (row_h - ImGui::GetTextLineHeight()) * 0.5f : 0.0f;
                ImGui::TableNextColumn();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + text_dy);
                if (!e.is_dir) ImGui::TextUnformatted(human_size(e.size).c_str());
                ImGui::TableNextColumn();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + text_dy);
                ImGui::TextUnformatted(human_time(e.modified).c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::SameLine();
    // Preview pane.
    if (ImGui::BeginChild("preview", ImVec2(0, -footer), ImGuiChildFlags_Borders)) {
        if (selected_ >= 0 && selected_ < static_cast<int>(entries_.size()) && !entries_[selected_].is_dir) {
            Entry& e = entries_[selected_];
            if (!e.thumb_tried && matches_filter(e.name)) load_thumbnail(e);
            if (e.thumb) {
                const float side = std::max(40.0f, ImGui::GetContentRegionAvail().x - 4.0f);
                const float k = std::min(side / e.thumb_w, side / e.thumb_h);
                const float w = e.thumb_w * k, h = e.thumb_h * k;
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(90, 90, 90, 255));
                ImGui::Image((ImTextureID)(intptr_t)e.thumb, ImVec2(w, h));
                ImGui::Text("%d x %d", e.image_w, e.image_h);
            } else {
                ImGui::TextDisabled("No preview");
            }
            ImGui::TextWrapped("%s", e.name.c_str());
        } else {
            ImGui::TextDisabled("Preview");
        }
    }
    ImGui::EndChild();

    // Filename + filter row.
    ImGui::TextUnformatted(mode_ == Mode::Save ? "Save as:" : "File:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-260);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (ImGui::InputText("##name", name_buf_, sizeof(name_buf_), ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (accept(name_buf_)) accepted = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("All files", &show_all_)) refresh();
    ImGui::SameLine();
    if (ImGui::Checkbox("Hidden", &show_hidden_)) refresh();
    ImGui::SameLine();
    ImGui::Checkbox("Thumbnails", &show_thumbs_);
    if (mode_ == Mode::Save && !exts_.empty()) {
        ImGui::TextUnformatted("Save as type:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-260);
        const char* current = type_ >= 0 && type_ < static_cast<int>(exts_.size()) && type_label(exts_[type_]) ? type_label(exts_[type_]) : "";
        if (ImGui::BeginCombo("##type", current)) {
            for (size_t i = 0; i < exts_.size(); ++i) {
                const char* label = type_label(exts_[i]);
                if (!label) continue;
                if (ImGui::Selectable(label, static_cast<int>(i) == type_)) { type_ = static_cast<int>(i); apply_type_to_name(); refresh(); }
            }
            ImGui::EndCombo();
        }
    }

    // Status + buttons.
    if (!error_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", error_.c_str());
    else if (mode_ == Mode::Save && name_buf_[0] && fs::exists(dir_ / name_buf_, ec))
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "A file with this name exists and will be replaced.");
    else if (!exts_.empty()) {
        std::string f;
        for (const auto& e : exts_) f += (f.empty() ? "" : ", ") + e;
        ImGui::TextDisabled("%s", f.c_str());
    } else ImGui::TextUnformatted("");

    const char* ok = mode_ == Mode::Save ? "Save" : "Open";
    if (ImGui::Button(ok, ImVec2(100, 0))) {
        if (accept(name_buf_)) accepted = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }
    if (accepted) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return accepted;
}
