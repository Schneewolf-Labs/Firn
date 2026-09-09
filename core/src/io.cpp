#include "psp9/io.h"

#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PSD
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace psp9::io {

std::optional<Image> load(const std::string& path, std::string* err) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px) {
        if (err) *err = stbi_failure_reason() ? stbi_failure_reason() : "unknown error";
        return std::nullopt;
    }
    Image img(w, h);
    std::memcpy(img.data(), px, img.size_bytes());
    stbi_image_free(px);
    return img;
}

bool save_png(const Image& img, const std::string& path, std::string* err) {
    int ok = stbi_write_png(path.c_str(), img.width(), img.height(), 4, img.data(), img.width() * 4);
    if (!ok && err) *err = "stbi_write_png failed";
    return ok != 0;
}

}  // namespace psp9::io
