#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// A minimal zip reader and writer (methods store and deflate, no zip64),
// enough for OpenRaster containers. Deflate goes through stb's codec.
namespace firn::zip {

struct Entry {
    std::string name;
    std::vector<uint8_t> data;
    bool store = false;   // write uncompressed (OpenRaster's mimetype entry)
};

std::vector<uint8_t> write(const std::vector<Entry>& entries);

// The central directory only: names and where their data sits. Reading a
// container this way costs nothing until an entry is actually wanted.
struct Index {
    struct Item {
        std::string name;
        size_t offset = 0;      // local header
        uint32_t csize = 0, usize = 0;
        uint16_t method = 0;
    };
    std::vector<Item> items;
    const Item* find(const std::string& name) const;
};

bool open(const uint8_t* data, size_t size, Index& out, std::string* err = nullptr);
bool extract(const uint8_t* data, size_t size, const Index::Item& item, std::vector<uint8_t>& out, std::string* err = nullptr);

struct Archive {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
    const std::vector<uint8_t>* find(const std::string& name) const;
};

// Every entry, inflated.
bool read(const uint8_t* data, size_t size, Archive& out, std::string* err = nullptr);

}  // namespace firn::zip
