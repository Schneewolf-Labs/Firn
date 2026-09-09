#pragma once
#include <optional>
#include <string>
#include <vector>

#include "firn/image.h"

namespace firn::io {

// PNG/JPEG/BMP/TGA via stb_image. Returns nullopt on failure and sets `err`.
std::optional<Image> load(const std::string& path, std::string* err = nullptr);

// Writes PNG. Returns false on failure.
bool save_png(const Image& img, const std::string& path, std::string* err = nullptr);

// Picks the format from the extension: png, jpg/jpeg (quality 0..100),
// bmp, tga. Formats without alpha are written over white.
bool save(const Image& img, const std::string& path, std::string* err = nullptr, int jpeg_quality = 90);

// Extensions load()/save() understand, lower-case without the dot.
const std::vector<std::string>& load_extensions();
const std::vector<std::string>& save_extensions();

}  // namespace firn::io
