#pragma once
#include <optional>
#include <string>
#include <vector>

#include "firn/image.h"

namespace firn::io {

// PNG/JPEG/BMP/TGA via stb_image. Returns nullopt on failure and sets `err`.
std::optional<Image> load(const std::string& path, std::string* err = nullptr);
// The same decoders over an in-memory file (embedded resources).
std::optional<Image> load_memory(const uint8_t* data, size_t size, std::string* err = nullptr);
// PNG bytes of an image (for the clipboard and other in-memory uses); empty on failure.
std::vector<uint8_t> encode_png(const Image& img);
// 16-bit PNG (and other 16-bit files stb reads): nullopt when the file has 8-bit channels.
std::optional<Image16> load16(const std::string& path, std::string* err = nullptr);
bool save_png16(const Image16& img, const std::string& path, std::string* err = nullptr);
// Embedded ICC profile of a PNG (iCCP) or JPEG (APP2), empty when none.
std::vector<uint8_t> read_icc(const std::string& path);
// Adds a profile to an already written PNG or JPEG file in place.
bool embed_icc(const std::string& path, const std::vector<uint8_t>& icc, std::string* err = nullptr);

// Writes PNG. Returns false on failure.
bool save_png(const Image& img, const std::string& path, std::string* err = nullptr);

// Picks the format from the extension: png, jpg/jpeg (quality 0..100),
// bmp, tga. Formats without alpha are written over white.
bool save(const Image& img, const std::string& path, std::string* err = nullptr, int jpeg_quality = 90);

// Extensions load()/save() understand, lower-case without the dot.
const std::vector<std::string>& load_extensions();  // includes the native container formats
const std::vector<std::string>& save_extensions();


// JASC-PAL palette files (the original's .PspPalette): "JASC-PAL", "0100",
// a count, then "r g b" lines.
std::vector<Color> load_palette(const std::string& path, std::string* err = nullptr);
bool save_palette(const std::vector<Color>& palette, const std::string& path, std::string* err = nullptr);
}  // namespace firn::io
