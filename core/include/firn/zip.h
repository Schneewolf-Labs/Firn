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

struct Archive {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
    const std::vector<uint8_t>* find(const std::string& name) const;
};

bool read(const uint8_t* data, size_t size, Archive& out, std::string* err = nullptr);

}  // namespace firn::zip
