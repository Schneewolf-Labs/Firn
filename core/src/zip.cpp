#include "firn/zip.h"

#include <cstdlib>
#include <cstring>

#include "stb/stb_image.h"   // stbi_zlib_decode_noheader_malloc

extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace firn::zip {

namespace {

uint32_t crc32(const uint8_t* data, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; } init = true; }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 255] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void put16(std::vector<uint8_t>& o, uint32_t v) { o.push_back(static_cast<uint8_t>(v)); o.push_back(static_cast<uint8_t>(v >> 8)); }
void put32(std::vector<uint8_t>& o, uint32_t v) { put16(o, v & 0xFFFF); put16(o, v >> 16); }
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24)); }

// Raw deflate: stb produces a zlib stream (2-byte header, 4-byte Adler trailer).
std::vector<uint8_t> deflate(const std::vector<uint8_t>& data) {
    if (data.empty()) return {};
    int zlen = 0;
    unsigned char* z = stbi_zlib_compress(const_cast<unsigned char*>(data.data()), static_cast<int>(data.size()), &zlen, 8);
    if (!z || zlen < 6) { if (z) std::free(z); return {}; }
    std::vector<uint8_t> out(z + 2, z + zlen - 4);
    std::free(z);
    return out;
}

}  // namespace

std::vector<uint8_t> write(const std::vector<Entry>& entries) {
    std::vector<uint8_t> out, central;
    uint32_t count = 0;
    for (const Entry& e : entries) {
        std::vector<uint8_t> packed;
        uint16_t method = 0;
        if (!e.store) {
            packed = deflate(e.data);
            if (!packed.empty() && packed.size() < e.data.size()) method = 8;
        }
        const std::vector<uint8_t>& body = method == 8 ? packed : e.data;
        const uint32_t crc = crc32(e.data.data(), e.data.size());
        const uint32_t offset = static_cast<uint32_t>(out.size());
        // Local file header.
        put32(out, 0x04034b50u); put16(out, 20); put16(out, 0); put16(out, method); put16(out, 0); put16(out, 0x21);   // time 0, date 1980-01-01
        put32(out, crc); put32(out, static_cast<uint32_t>(body.size())); put32(out, static_cast<uint32_t>(e.data.size()));
        put16(out, static_cast<uint32_t>(e.name.size())); put16(out, 0);
        out.insert(out.end(), e.name.begin(), e.name.end());
        out.insert(out.end(), body.begin(), body.end());
        // Central directory record.
        put32(central, 0x02014b50u); put16(central, 20); put16(central, 20); put16(central, 0); put16(central, method); put16(central, 0); put16(central, 0x21);
        put32(central, crc); put32(central, static_cast<uint32_t>(body.size())); put32(central, static_cast<uint32_t>(e.data.size()));
        put16(central, static_cast<uint32_t>(e.name.size())); put16(central, 0); put16(central, 0); put16(central, 0); put16(central, 0); put32(central, 0); put32(central, offset);
        central.insert(central.end(), e.name.begin(), e.name.end());
        ++count;
    }
    const uint32_t cd_offset = static_cast<uint32_t>(out.size());
    out.insert(out.end(), central.begin(), central.end());
    put32(out, 0x06054b50u); put16(out, 0); put16(out, 0); put16(out, count); put16(out, count);
    put32(out, static_cast<uint32_t>(central.size())); put32(out, cd_offset); put16(out, 0);
    return out;
}

const std::vector<uint8_t>* Archive::find(const std::string& name) const {
    for (const auto& f : files) if (f.first == name) return &f.second;
    return nullptr;
}

bool read(const uint8_t* data, size_t size, Archive& out, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (size < 22) return fail("not a zip file");
    // End of central directory: scan back over a possible comment.
    size_t eocd = size - 22;
    const size_t stop = size > 22 + 65535 ? size - 22 - 65535 : 0;
    while (get32(data + eocd) != 0x06054b50u) { if (eocd == stop) return fail("zip: no central directory"); --eocd; }
    const uint32_t count = get16(data + eocd + 10);
    size_t cd = get32(data + eocd + 16);
    for (uint32_t i = 0; i < count; ++i) {
        if (cd + 46 > size || get32(data + cd) != 0x02014b50u) return fail("zip: bad central directory");
        const uint16_t method = get16(data + cd + 10);
        const uint32_t csize = get32(data + cd + 20), usize = get32(data + cd + 24);
        const uint16_t nlen = get16(data + cd + 28), xlen = get16(data + cd + 30), clen = get16(data + cd + 32);
        const uint32_t local = get32(data + cd + 42);
        if (cd + 46 + nlen > size) return fail("zip: bad entry name");
        std::string name(reinterpret_cast<const char*>(data + cd + 46), nlen);
        cd += 46 + nlen + xlen + clen;
        if (local + 30 > size || get32(data + local) != 0x04034b50u) return fail("zip: bad local header");
        const size_t body = local + 30 + get16(data + local + 26) + get16(data + local + 28);
        if (body + csize > size) return fail("zip: truncated entry");
        std::vector<uint8_t> bytes;
        if (method == 0) bytes.assign(data + body, data + body + csize);
        else if (method == 8) {
            int outlen = 0;
            char* raw = csize ? stbi_zlib_decode_noheader_malloc(reinterpret_cast<const char*>(data + body), static_cast<int>(csize), &outlen) : nullptr;
            if (csize && !raw) return fail("zip: inflate failed");
            if (raw) { bytes.assign(raw, raw + outlen); std::free(raw); }
        } else return fail("zip: unsupported compression method");
        if (bytes.size() != usize) return fail("zip: size mismatch");
        out.files.emplace_back(std::move(name), std::move(bytes));
    }
    return true;
}

}  // namespace firn::zip
