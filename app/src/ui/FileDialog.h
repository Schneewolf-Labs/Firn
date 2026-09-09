#pragma once
#include <filesystem>
#include <string>
#include <vector>

// A modal file browser built from ImGui widgets, so it behaves the same on
// every platform. Call open() once, then draw() every frame; draw() returns
// true on the frame a path is accepted.
class FileDialog {
public:
    enum class Mode { Open, Save };

    // `extensions` are lower-case without the dot; the first is the default
    // appended on save when the user types a bare name.
    void open(Mode mode, std::string title, std::vector<std::string> extensions, const std::string& initial_path);
    bool draw();
    const std::string& path() const { return result_; }

private:
    struct Entry {
        std::string name;
        bool is_dir = false;
        uintmax_t size = 0;
        std::filesystem::file_time_type modified;
    };
    void set_dir(const std::filesystem::path& dir);
    void refresh();
    bool matches_filter(const std::string& name) const;
    bool accept(const std::string& typed);

    Mode mode_ = Mode::Open;
    std::string title_;
    std::vector<std::string> exts_;
    bool open_requested_ = false;
    bool show_all_ = false;
    bool show_hidden_ = false;
    std::filesystem::path dir_;
    std::vector<Entry> entries_;
    int selected_ = -1;
    char dir_buf_[1024] = {};
    char name_buf_[1024] = {};
    std::string result_;
    std::string error_;
};
