#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "firn/document.h"
#include "firn/vector.h"

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
// The native format's vector layer and adjustment layer payloads as byte
// strings, so other containers (OpenRaster) can carry them unchanged.
std::vector<uint8_t> vector_objects_to_bytes(const std::vector<vec::Object>& objects);
bool vector_objects_from_bytes(const uint8_t* data, size_t size, std::vector<vec::Object>& out);
std::vector<uint8_t> adjustment_to_bytes(const Adjustment& a);
bool adjustment_from_bytes(const uint8_t* data, size_t size, Adjustment& out);
// OpenRaster (core/src/io_ora.cpp): Firn's project format, readable by
// the other open-source editors; Firn-only data rides in extension attributes.
bool is_ora_extension(const std::string& path);
std::unique_ptr<Document> load_ora(const std::string& path, std::string* err, std::vector<std::string>* warnings);
std::unique_ptr<Document> load_ora_from_memory(const uint8_t* data, size_t size, std::string* err, std::vector<std::string>* warnings);
std::vector<uint8_t> save_ora_to_memory(const Document& doc);
bool save_ora(const Document& doc, const std::string& path, std::string* err);
// Photoshop PSD/PSB import (core/src/io_psd.cpp): layers, groups, masks, 8 and 16 bit RGB or grayscale.
std::unique_ptr<Document> load_psd(const std::string& path, std::string* err, std::vector<std::string>* warnings);
bool is_psd_extension(const std::string& path);

// Preset shape files (.PspShape) are ordinary images holding vector layers;
// returns the objects of every vector layer, or empty on failure.
std::vector<vec::Object> load_preset_shapes(const std::string& path, std::string* err = nullptr);

// Gradient files (.PspGradient) are Photoshop .grd (version 3) files.
std::vector<vec::Gradient> load_gradients(const std::string& path, std::string* err = nullptr);
// Writes gradients in the same file format (one file may hold several).
bool save_gradients(const std::vector<vec::Gradient>& gradients, const std::string& path, std::string* err = nullptr);

// Styled line files (.PspStyledLine): caps and dash segments.
std::optional<vec::LineStyle> load_styled_line(const std::string& path, std::string* err = nullptr);
bool save_styled_line(const vec::LineStyle& line, const std::string& path, std::string* err = nullptr);

// Picture tube metadata (block id 11): how a tube image is divided into
// cells and how the tool should place them.
struct TubeInfo {
    int step = 0;         // stamp spacing in pixels
    int columns = 1, rows = 1, total = 1;
    int placement = 1;    // 1 random, 2 continuous
    int selection = 1;    // 1 random, 2 incremental, 3 angular, 4 pressure, 5 velocity
};
std::optional<TubeInfo> load_psp_tube_info(const uint8_t* data, size_t size);
std::optional<TubeInfo> load_psp_tube_info(const std::string& path);

// The full-size composite the original stored in the file, if it has one
// in channel (non-JPEG) form. Used by tests to check our compositing.
std::optional<Image> load_psp_stored_composite(const uint8_t* data, size_t size);

// Writes a version 6.0 file (zlib channels) with every layer, its name,
// position, opacity, blend mode and visibility, plus the composite bank the
// original expects (JPEG thumbnail and full-size composite).
bool save_psp(const Document& doc, const std::string& path, std::string* err);
std::vector<uint8_t> save_psp_to_memory(const Document& doc);

// Saves by extension: the native container keeps layers; anything else
// writes the flattened composite through io::save.
bool save_document(const Document& doc, const std::string& path, std::string* err, int jpeg_quality = 90);

}  // namespace firn::io
