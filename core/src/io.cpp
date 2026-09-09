#include "firn/io.h"
#include <fstream>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

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

std::optional<Image16> load16(const std::string& path, std::string* err) {
    if (!stbi_is_16_bit(path.c_str())) return std::nullopt;
    int w = 0, h = 0, n = 0;
    stbi_us* px = stbi_load_16(path.c_str(), &w, &h, &n, 4);
    if (!px) { if (err) *err = stbi_failure_reason() ? stbi_failure_reason() : "unknown error"; return std::nullopt; }
    Image16 img(w, h);
    std::memcpy(img.data(), px, img.size() * 2);
    stbi_image_free(px);
    return img;
}

namespace {
uint32_t crc32_of(const uint8_t* data, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; } init = true; }
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 255] ^ (crc >> 8);
    return crc;
}
void png_chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    auto be32 = [&](uint32_t v) { out.push_back(static_cast<uint8_t>(v >> 24)); out.push_back(static_cast<uint8_t>(v >> 16)); out.push_back(static_cast<uint8_t>(v >> 8)); out.push_back(static_cast<uint8_t>(v)); };
    be32(static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    be32(crc32_of(td.data(), td.size()) ^ 0xFFFFFFFFu);
}
}  // namespace

bool save_png16(const Image16& img, const std::string& path, std::string* err) {
    // Filter type 0 rows of big-endian RGBA16, deflated with stb's encoder.
    const size_t row = static_cast<size_t>(img.width()) * 8;
    std::vector<uint8_t> raw((row + 1) * img.height());
    for (int y = 0; y < img.height(); ++y) {
        uint8_t* d = raw.data() + static_cast<size_t>(y) * (row + 1);
        *d++ = 0;
        const uint16_t* s = img.data() + static_cast<size_t>(y) * img.width() * 4;
        for (size_t i = 0; i < static_cast<size_t>(img.width()) * 4; ++i) { *d++ = static_cast<uint8_t>(s[i] >> 8); *d++ = static_cast<uint8_t>(s[i] & 255); }
    }
    int zlen = 0;
    unsigned char* z = stbi_zlib_compress(raw.data(), static_cast<int>(raw.size()), &zlen, 8);
    if (!z) { if (err) *err = "deflate failed"; return false; }
    std::vector<uint8_t> out{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    auto be32 = [&](uint32_t v) { ihdr.push_back(static_cast<uint8_t>(v >> 24)); ihdr.push_back(static_cast<uint8_t>(v >> 16)); ihdr.push_back(static_cast<uint8_t>(v >> 8)); ihdr.push_back(static_cast<uint8_t>(v)); };
    be32(static_cast<uint32_t>(img.width())); be32(static_cast<uint32_t>(img.height()));
    ihdr.push_back(16); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);   // 16-bit RGBA
    png_chunk(out, "IHDR", ihdr);
    png_chunk(out, "IDAT", std::vector<uint8_t>(z, z + zlen));
    STBIW_FREE(z);
    png_chunk(out, "IEND", {});
    std::ofstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot write " + path; return false; }
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(f);
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


std::vector<Color> load_palette(const std::string& path, std::string* err) {
    std::ifstream f(path);
    std::vector<Color> out;
    if (!f) { if (err) *err = "cannot open " + path; return out; }
    std::string line;
    std::getline(f, line);
    if (line.rfind("JASC-PAL", 0) != 0) { if (err) *err = "not a JASC-PAL palette"; return out; }
    std::getline(f, line);  // version
    int count = 0;
    f >> count;
    for (int i = 0; i < count && f; ++i) {
        int r, g, b;
        if (!(f >> r >> g >> b)) break;
        out.push_back({static_cast<uint8_t>(std::clamp(r, 0, 255)), static_cast<uint8_t>(std::clamp(g, 0, 255)), static_cast<uint8_t>(std::clamp(b, 0, 255)), 255});
    }
    if (out.empty() && err) *err = "empty palette";
    return out;
}

bool save_palette(const std::vector<Color>& palette, const std::string& path, std::string* err) {
    std::ofstream f(path);
    if (!f) { if (err) *err = "cannot write " + path; return false; }
    f << "JASC-PAL\r\n0100\r\n" << palette.size() << "\r\n";
    for (const Color& c : palette) f << static_cast<int>(c.r) << ' ' << static_cast<int>(c.g) << ' ' << static_cast<int>(c.b) << "\r\n";
    return static_cast<bool>(f);
}

}  // namespace firn::io
