#pragma once
#include <optional>
#include <string>

#include "psp9/image.h"

namespace psp9::io {

// PNG/JPEG/BMP/TGA via stb_image. Returns nullopt on failure and sets `err`.
std::optional<Image> load(const std::string& path, std::string* err = nullptr);

// Writes PNG. Returns false on failure.
bool save_png(const Image& img, const std::string& path, std::string* err = nullptr);

}  // namespace psp9::io
