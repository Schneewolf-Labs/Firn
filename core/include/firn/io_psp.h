#pragma once
#include <memory>
#include <string>
#include <vector>

#include "firn/document.h"

namespace firn::io {

// Reads the original program's native container (.PspImage; .PspTube,
// .PspFrame and .PspSelection share it). Raster layers come through with
// name, position, opacity, blend mode and visibility. Vector, adjustment,
// mask, group and art-media layers are skipped with a warning; if no raster
// layer could be read, the embedded full-size composite is used instead.
// Returns nullptr and sets `err` on a malformed file.
std::unique_ptr<Document> load_psp(const std::string& path, std::string* err, std::vector<std::string>* warnings);

// Same, from memory (used by tests).
std::unique_ptr<Document> load_psp_from_memory(const uint8_t* data, size_t size, std::string* err,
                                               std::vector<std::string>* warnings);

// Opens any supported file as a Document: the native container above, or a
// single-layer document with a Background layer for PNG/JPEG/etc.
std::unique_ptr<Document> load_document(const std::string& path, std::string* err, std::vector<std::string>* warnings);

bool is_psp_extension(const std::string& path);

// Writes a version 6.0 file (zlib channels) with every layer, its name,
// position, opacity, blend mode and visibility, plus the composite bank the
// original expects (JPEG thumbnail and full-size composite).
bool save_psp(const Document& doc, const std::string& path, std::string* err);
std::vector<uint8_t> save_psp_to_memory(const Document& doc);

// Saves by extension: the native container keeps layers; anything else
// writes the flattened composite through io::save.
bool save_document(const Document& doc, const std::string& path, std::string* err);

}  // namespace firn::io
