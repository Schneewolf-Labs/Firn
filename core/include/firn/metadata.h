#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Image metadata: Exif (JPEG APP1 and the PNG eXIf chunk) and PNG text
// chunks. Entries keep their TIFF type and value bytes so everything a file
// carries survives a round trip, while the text rendering is what the
// interface shows and edits. Editing is limited to the kinds of value a
// person actually writes: text, numbers and rationals.
namespace firn::meta {

// TIFF value types, as they appear in an IFD entry.
enum Type : uint16_t {
    kByte = 1, kAscii = 2, kShort = 3, kLong = 4, kRational = 5,
    kSByte = 6, kUndefined = 7, kSShort = 8, kSLong = 9, kSRational = 10,
    kFloat = 11, kDouble = 12,
};

// Which directory an entry came from. Text entries are PNG text chunks.
enum class Group { Image, Exif, GPS, Interop, Text };

const char* group_name(Group g);

struct Entry {
    Group group = Group::Image;
    uint16_t tag = 0;              // TIFF tag, 0 for text chunks
    uint16_t type = kAscii;
    uint32_t count = 0;            // components, not bytes
    std::vector<uint8_t> value;    // little-endian component bytes
    std::string key;               // text chunk keyword, empty for Exif

    bool operator==(const Entry& o) const {
        return group == o.group && tag == o.tag && type == o.type && count == o.count && value == o.value && key == o.key;
    }
    std::string name() const;      // tag name, or the keyword for text
    std::string text() const;      // human-readable rendering
    bool editable() const;         // can text() be typed back in?
    bool set_text(const std::string& s);   // parse text() back into value
};

struct Metadata {
    std::vector<Entry> entries;

    bool empty() const { return entries.empty(); }
    size_t size() const { return entries.size(); }
    const Entry* find(Group g, uint16_t tag) const;
    const Entry* find_text(const std::string& key) const;
    // Adds or replaces an entry. Exif tags keep the type they had, or take
    // the one the tag table gives them. Returns false when the tag is
    // unknown or the text does not fit the type.
    bool set(Group g, uint16_t tag, const std::string& text);
    bool set_text(const std::string& key, const std::string& value);
    void remove(Group g, uint16_t tag);
    void remove_text(const std::string& key);
    // Drops everything that identifies where and how the picture was taken:
    // GPS, camera serial numbers, owner and maker notes.
    void remove_private();
    void sort();                   // by group, then tag or keyword
};

// The tag a name refers to, or 0. Names are the Exif ones ("DateTimeOriginal").
uint16_t tag_for_name(Group g, const std::string& name);

// Parsers. `parse_tiff` takes the bytes after "Exif\0\0" (a TIFF header).
Metadata parse_tiff(const uint8_t* data, size_t size);
Metadata parse_jpeg(const uint8_t* data, size_t size);
Metadata parse_png(const uint8_t* data, size_t size);

// Serializers. `build_tiff` writes a little-endian TIFF block holding the
// Exif entries; empty when there are none. `thumbnail` is a JPEG of the
// picture as it now stands, written as the second directory (IFD1) the way
// a camera writes its own. The one a file arrived with is deliberately not
// carried through: after an edit it would show a picture that is no longer
// there, which is how cropped-out detail leaks out of a photo.
std::vector<uint8_t> build_tiff(const Metadata& md, const std::vector<uint8_t>& thumbnail = {});
// Rewrites a whole file's metadata, returning the new bytes. Existing Exif
// and text chunks are replaced. Returns the input unchanged when the format
// has nowhere to put metadata.
std::vector<uint8_t> apply_jpeg(const std::vector<uint8_t>& file, const Metadata& md, const std::vector<uint8_t>& thumbnail = {});
std::vector<uint8_t> apply_png(const std::vector<uint8_t>& file, const Metadata& md, const std::vector<uint8_t>& thumbnail = {});

}  // namespace firn::meta
