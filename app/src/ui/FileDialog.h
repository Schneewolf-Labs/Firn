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
    std::string directory() const { return dir_.string(); }
    void set_directory(const std::string& dir);

private:
    struct Entry {
        std::string name;
        bool is_dir = false;
        uintmax_t size = 0;
        std::filesystem::file_time_type modified;
        // Thumbnail, decoded lazily for visible rows (GL texture id).
        unsigned int thumb = 0;
        int thumb_w = 0, thumb_h = 0;      // texture size
        int image_w = 0, image_h = 0;      // the file's own size
        bool thumb_tried = false;
    };
    void set_dir(const std::filesystem::path& dir);
    void refresh();
    void load_thumbnail(Entry& e);
    void free_thumbnails();
    bool matches_filter(const std::string& name) const;
    bool accept(const std::string& typed);
    // Save mode: the "Save as type" choice, an index into exts_.
    void set_type_from_name(const std::string& name);
    void apply_type_to_name();

    Mode mode_ = Mode::Open;
    std::string title_;
    std::vector<std::string> exts_;
    bool open_requested_ = false;
    int type_ = 0;
    bool show_thumbs_ = true;
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
