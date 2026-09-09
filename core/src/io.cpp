#include "firn/io.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PSD
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace firn::io {

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

namespace {

std::string ext_of(const std::string& path) {
    const auto dot = path.rfind('.');
    const auto slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string e = path.substr(dot + 1);
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

// Flatten straight-alpha RGBA onto white as RGB for formats without alpha.
std::vector<uint8_t> to_rgb_on_white(const Image& img) {
    const size_t n = static_cast<size_t>(img.width()) * img.height();
    std::vector<uint8_t> out(n * 3);
    const uint8_t* s = img.data();
    for (size_t i = 0; i < n; ++i) {
        const int a = s[i * 4 + 3];
        for (int c = 0; c < 3; ++c) out[i * 3 + c] = static_cast<uint8_t>((s[i * 4 + c] * a + 255 * (255 - a) + 127) / 255);
    }
    return out;
}

}  // namespace

bool save(const Image& img, const std::string& path, std::string* err, int jpeg_quality) {
    const std::string ext = ext_of(path);
    int ok = 0;
    if (ext == "png") return save_png(img, path, err);
    if (ext == "jpg" || ext == "jpeg") {
        auto rgb = to_rgb_on_white(img);
        ok = stbi_write_jpg(path.c_str(), img.width(), img.height(), 3, rgb.data(), std::clamp(jpeg_quality, 1, 100));
    } else if (ext == "bmp") {
        auto rgb = to_rgb_on_white(img);
        ok = stbi_write_bmp(path.c_str(), img.width(), img.height(), 3, rgb.data());
    } else if (ext == "tga") {
        ok = stbi_write_tga(path.c_str(), img.width(), img.height(), 4, img.data());
    } else {
        if (err) *err = ext.empty() ? "no file extension" : "unsupported format: " + ext;
        return false;
    }
    if (!ok && err) *err = "could not write " + path;
    return ok != 0;
}

const std::vector<std::string>& load_extensions() {
    static const std::vector<std::string> v{"pspimage", "psp", "psptube", "pspframe", "png", "jpg", "jpeg", "bmp", "tga", "gif", "pnm", "ppm", "pgm"};
    return v;
}

const std::vector<std::string>& save_extensions() {
    static const std::vector<std::string> v{"pspimage", "png", "jpg", "jpeg", "bmp", "tga"};
    return v;
}

}  // namespace firn::io
