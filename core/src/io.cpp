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

#include "webp/decode.h"
#include "webp/encode.h"
#include "webp/mux.h"

namespace firn::io {

namespace {

bool is_webp(const uint8_t* data, size_t size) {
    return size >= 12 && std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WEBP", 4) == 0;
}

std::optional<Image> decode_webp(const uint8_t* data, size_t size, std::string* err) {
    int w = 0, h = 0;
    if (!WebPGetInfo(data, size, &w, &h)) { if (err) *err = "not a WebP image"; return std::nullopt; }
    Image img(w, h);
    if (!WebPDecodeRGBAInto(data, size, img.data(), img.size_bytes(), w * 4)) { if (err) *err = "WebP decode failed"; return std::nullopt; }
    return img;
}

std::vector<uint8_t> read_file(const std::string& path);   // defined with the ICC helpers below

}  // namespace

std::vector<uint8_t> encode_png(const Image& img) {
    int len = 0;
    stbi_write_png_compression_level = img.size_bytes() > (32u << 20) ? 4 : 8;
    unsigned char* png = stbi_write_png_to_mem(img.data(), img.width() * 4, img.width(), img.height(), 4, &len);
    if (!png) return {};
    std::vector<uint8_t> out(png, png + len);
    STBIW_FREE(png);
    return out;
}

std::optional<Image> load_memory(const uint8_t* data, size_t size, std::string* err) {
    if (is_webp(data, size)) return decode_webp(data, size, err);
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &n, 4);
    if (!px) {
        if (err) *err = stbi_failure_reason() ? stbi_failure_reason() : "unknown error";
        return std::nullopt;
    }
    Image img(w, h);
    std::memcpy(img.data(), px, img.size_bytes());
    stbi_image_free(px);
    return img;
}

std::optional<Image> load(const std::string& path, std::string* err) {
    {
        // WebP by signature, whatever the extension.
        std::ifstream f(path, std::ios::binary);
        uint8_t head[12] = {};
        if (f && f.read(reinterpret_cast<char*>(head), 12) && is_webp(head, 12)) {
            const std::vector<uint8_t> bytes = read_file(path);
            return decode_webp(bytes.data(), bytes.size(), err);
        }
    }
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

std::optional<Image16> load16_memory(const uint8_t* data, size_t size) {
    if (!stbi_is_16_bit_from_memory(data, static_cast<int>(size))) return std::nullopt;
    int w = 0, h = 0, n = 0;
    stbi_us* px = stbi_load_16_from_memory(data, static_cast<int>(size), &w, &h, &n, 4);
    if (!px) return std::nullopt;
    Image16 img(w, h);
    std::memcpy(img.data(), px, img.size() * 2);
    stbi_image_free(px);
    return img;
}

std::vector<uint8_t> encode_png16(const Image16& img) {
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
    // Big images take a lighter compression level: several times faster for a few percent of size.
    unsigned char* z = stbi_zlib_compress(raw.data(), static_cast<int>(raw.size()), &zlen, raw.size() > (32u << 20) ? 4 : 8);
    if (!z) return {};
    std::vector<uint8_t> out{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    auto be32 = [&](uint32_t v) { ihdr.push_back(static_cast<uint8_t>(v >> 24)); ihdr.push_back(static_cast<uint8_t>(v >> 16)); ihdr.push_back(static_cast<uint8_t>(v >> 8)); ihdr.push_back(static_cast<uint8_t>(v)); };
    be32(static_cast<uint32_t>(img.width())); be32(static_cast<uint32_t>(img.height()));
    ihdr.push_back(16); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);   // 16-bit RGBA
    png_chunk(out, "IHDR", ihdr);
    png_chunk(out, "IDAT", std::vector<uint8_t>(z, z + zlen));
    STBIW_FREE(z);
    png_chunk(out, "IEND", {});
    return out;
}

bool save_png16(const Image16& img, const std::string& path, std::string* err) {
    const std::vector<uint8_t> out = encode_png16(img);
    if (out.empty()) { if (err) *err = "deflate failed"; return false; }
    std::ofstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot write " + path; return false; }
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(f);
}

namespace {
std::vector<uint8_t> read_file(const std::string& path) { std::ifstream f(path, std::ios::binary); return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
bool write_file(const std::string& path, const std::vector<uint8_t>& d) { std::ofstream f(path, std::ios::binary); f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size())); return static_cast<bool>(f); }
uint32_t rd32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3]; }
}  // namespace

std::vector<uint8_t> read_icc(const std::string& path) {
    const std::vector<uint8_t> d = read_file(path);
    if (is_webp(d.data(), d.size())) {
        // The ICCP chunk of the WebP container.
        std::vector<uint8_t> out;
        WebPData data{d.data(), d.size()};
        if (WebPMux* mux = WebPMuxCreate(&data, 0)) {
            WebPData icc{};
            if (WebPMuxGetChunk(mux, "ICCP", &icc) == WEBP_MUX_OK && icc.size) out.assign(icc.bytes, icc.bytes + icc.size);
            WebPMuxDelete(mux);
        }
        return out;
    }
    if (d.size() > 8 && d[0] == 0x89 && d[1] == 'P') {
        size_t p = 8;
        while (p + 12 <= d.size()) {
            const uint32_t len = rd32(&d[p]);
            if (p + 12 + len > d.size()) break;
            if (std::memcmp(&d[p + 4], "iCCP", 4) == 0) {
                size_t q = p + 8;
                while (q < p + 8 + len && d[q]) ++q;   // profile name
                q += 2;                                // null + compression method
                if (q < p + 8 + len) {
                    int outlen = 0;
                    char* z = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(&d[q]), static_cast<int>(p + 8 + len - q), &outlen);
                    if (z) { std::vector<uint8_t> out(z, z + outlen); STBI_FREE(z); return out; }
                }
                return {};
            }
            if (std::memcmp(&d[p + 4], "IDAT", 4) == 0) break;
            p += 12 + len;
        }
        return {};
    }
    if (d.size() > 4 && d[0] == 0xFF && d[1] == 0xD8) {
        // APP2 "ICC_PROFILE\0" segments, sequence-numbered.
        std::vector<std::pair<int, std::vector<uint8_t>>> parts;
        size_t p = 2;
        while (p + 4 <= d.size() && d[p] == 0xFF) {
            const uint8_t marker = d[p + 1];
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { p += 2; continue; }
            const size_t len = (static_cast<size_t>(d[p + 2]) << 8) | d[p + 3];
            if (marker == 0xE2 && len > 16 && p + 2 + len <= d.size() && std::memcmp(&d[p + 4], "ICC_PROFILE", 12) == 0)
                parts.emplace_back(d[p + 16], std::vector<uint8_t>(d.begin() + static_cast<long>(p + 18), d.begin() + static_cast<long>(p + 2 + len)));
            if (marker == 0xDA) break;
            p += 2 + len;
        }
        std::sort(parts.begin(), parts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<uint8_t> out;
        for (auto& pr : parts) out.insert(out.end(), pr.second.begin(), pr.second.end());
        return out;
    }
    return {};
}

bool embed_icc(const std::string& path, const std::vector<uint8_t>& icc, std::string* err) {
    if (icc.empty()) return true;
    std::vector<uint8_t> d = read_file(path);
    if (is_webp(d.data(), d.size())) {
        WebPData data{d.data(), d.size()};
        WebPMux* mux = WebPMuxCreate(&data, 1);
        if (!mux) { if (err) *err = "cannot rewrite the WebP container"; return false; }
        WebPData chunk{icc.data(), icc.size()};
        WebPData assembled{};
        const bool ok = WebPMuxSetChunk(mux, "ICCP", &chunk, 1) == WEBP_MUX_OK && WebPMuxAssemble(mux, &assembled) == WEBP_MUX_OK;
        WebPMuxDelete(mux);
        if (!ok) { if (err) *err = "cannot add the profile to the WebP file"; return false; }
        const std::vector<uint8_t> out(assembled.bytes, assembled.bytes + assembled.size);
        WebPDataClear(&assembled);
        return write_file(path, out) || (err && (*err = "cannot write " + path, false));
    }
    if (d.size() > 8 && d[0] == 0x89 && d[1] == 'P') {
        // Insert an iCCP chunk right after IHDR.
        const uint32_t ihdr_len = rd32(&d[8]);
        const size_t at = 8 + 12 + ihdr_len;
        int zlen = 0;
        unsigned char* z = stbi_zlib_compress(const_cast<unsigned char*>(icc.data()), static_cast<int>(icc.size()), &zlen, 8);
        if (!z) { if (err) *err = "deflate failed"; return false; }
        std::vector<uint8_t> chunk;
        const char* name = "ICC profile";
        chunk.insert(chunk.end(), name, name + std::strlen(name) + 1);
        chunk.push_back(0);
        chunk.insert(chunk.end(), z, z + zlen);
        STBIW_FREE(z);
        std::vector<uint8_t> out(d.begin(), d.begin() + static_cast<long>(at));
        png_chunk(out, "iCCP", chunk);
        out.insert(out.end(), d.begin() + static_cast<long>(at), d.end());
        return write_file(path, out) || (err && (*err = "cannot write " + path, false));
    }
    if (d.size() > 4 && d[0] == 0xFF && d[1] == 0xD8) {
        std::vector<uint8_t> out(d.begin(), d.begin() + 2);
        const size_t max_chunk = 65533 - 16;
        const int total = static_cast<int>((icc.size() + max_chunk - 1) / max_chunk);
        for (int i = 0; i < total; ++i) {
            const size_t start = static_cast<size_t>(i) * max_chunk, n = std::min(max_chunk, icc.size() - start);
            const size_t len = 2 + 12 + 2 + n;
            out.push_back(0xFF); out.push_back(0xE2); out.push_back(static_cast<uint8_t>(len >> 8)); out.push_back(static_cast<uint8_t>(len & 255));
            const char* tag = "ICC_PROFILE";
            out.insert(out.end(), tag, tag + 12);
            out.push_back(static_cast<uint8_t>(i + 1)); out.push_back(static_cast<uint8_t>(total));
            out.insert(out.end(), icc.begin() + static_cast<long>(start), icc.begin() + static_cast<long>(start + n));
        }
        out.insert(out.end(), d.begin() + 2, d.end());
        return write_file(path, out) || (err && (*err = "cannot write " + path, false));
    }
    if (err) *err = "profiles embed in PNG and JPEG files only";
    return false;
}

meta::Metadata read_metadata(const std::string& path) {
    const std::vector<uint8_t> d = read_file(path);
    if (d.size() > 4 && d[0] == 0xFF && d[1] == 0xD8) return meta::parse_jpeg(d.data(), d.size());
    if (d.size() > 8 && d[0] == 0x89 && d[1] == 'P') return meta::parse_png(d.data(), d.size());
    return {};
}

bool embed_metadata(const std::string& path, const meta::Metadata& md, std::string* err) {
    const std::vector<uint8_t> d = read_file(path);
    std::vector<uint8_t> out;
    if (d.size() > 4 && d[0] == 0xFF && d[1] == 0xD8) out = meta::apply_jpeg(d, md);
    else if (d.size() > 8 && d[0] == 0x89 && d[1] == 'P') out = meta::apply_png(d, md);
    else return true;   // nowhere to put it, and nothing was lost
    if (out == d) return true;
    if (write_file(path, out)) return true;
    if (err) *err = "cannot write " + path;
    return false;
}

bool save_png(const Image& img, const std::string& path, std::string* err) {
    stbi_write_png_compression_level = img.size_bytes() > (32u << 20) ? 4 : 8;
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
    if (ext == "webp") {
        // Quality 100 means lossless, as the dialog offers; below that, lossy at that quality.
        uint8_t* out = nullptr;
        const size_t n = jpeg_quality >= 100 ? WebPEncodeLosslessRGBA(img.data(), img.width(), img.height(), img.width() * 4, &out)
                                             : WebPEncodeRGBA(img.data(), img.width(), img.height(), img.width() * 4, static_cast<float>(std::clamp(jpeg_quality, 1, 99)), &out);
        if (!n || !out) { if (err) *err = "WebP encode failed"; return false; }
        std::ofstream f(path, std::ios::binary);
        const bool written = f && f.write(reinterpret_cast<const char*>(out), static_cast<std::streamsize>(n));
        WebPFree(out);
        if (!written && err) *err = "cannot write " + path;
        return written;
    }
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
    static const std::vector<std::string> v{"ora", "pspimage", "psp", "psptube", "pspframe", "psd", "psb", "png", "jpg", "jpeg", "webp", "bmp", "tga", "gif", "pnm", "ppm", "pgm"};
    return v;
}

const std::vector<std::string>& save_extensions() {
    static const std::vector<std::string> v{"ora", "pspimage", "png", "jpg", "jpeg", "webp", "bmp", "tga"};
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
