#include "firn/metadata.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <numeric>
#include <set>
#include <sstream>

#include "stb/stb_image.h"   // stbi_zlib_decode_malloc (implemented in io.cpp)

namespace firn::meta {

namespace {

struct TagInfo {
    uint16_t tag;
    const char* name;
    uint16_t type;
};

const TagInfo kImageTags[] = {
    {0x010E, "ImageDescription", kAscii}, {0x010F, "Make", kAscii},
    {0x0110, "Model", kAscii},            {0x0112, "Orientation", kShort},
    {0x011A, "XResolution", kRational},   {0x011B, "YResolution", kRational},
    {0x0128, "ResolutionUnit", kShort},   {0x0131, "Software", kAscii},
    {0x0132, "DateTime", kAscii},         {0x013B, "Artist", kAscii},
    {0x0213, "YCbCrPositioning", kShort}, {0x8298, "Copyright", kAscii},
    {0x9C9B, "XPTitle", kByte},           {0x9C9C, "XPComment", kByte},
    {0x9C9D, "XPAuthor", kByte},          {0x9C9E, "XPKeywords", kByte},
    {0x9C9F, "XPSubject", kByte},
};

const TagInfo kExifTags[] = {
    {0x829A, "ExposureTime", kRational},        {0x829D, "FNumber", kRational},
    {0x8822, "ExposureProgram", kShort},        {0x8827, "ISOSpeedRatings", kShort},
    {0x9000, "ExifVersion", kUndefined},        {0x9003, "DateTimeOriginal", kAscii},
    {0x9004, "DateTimeDigitized", kAscii},      {0x9010, "OffsetTime", kAscii},
    {0x9011, "OffsetTimeOriginal", kAscii},     {0x9012, "OffsetTimeDigitized", kAscii},
    {0x9201, "ShutterSpeedValue", kSRational},  {0x9202, "ApertureValue", kRational},
    {0x9204, "ExposureBiasValue", kSRational},  {0x9205, "MaxApertureValue", kRational},
    {0x9207, "MeteringMode", kShort},           {0x9208, "LightSource", kShort},
    {0x9209, "Flash", kShort},                  {0x920A, "FocalLength", kRational},
    {0x927C, "MakerNote", kUndefined},          {0x9286, "UserComment", kUndefined},
    {0x9290, "SubSecTime", kAscii},             {0x9291, "SubSecTimeOriginal", kAscii},
    {0x9292, "SubSecTimeDigitized", kAscii},    {0xA000, "FlashpixVersion", kUndefined},
    {0xA001, "ColorSpace", kShort},             {0xA002, "PixelXDimension", kLong},
    {0xA003, "PixelYDimension", kLong},         {0xA402, "ExposureMode", kShort},
    {0xA403, "WhiteBalance", kShort},           {0xA404, "DigitalZoomRatio", kRational},
    {0xA405, "FocalLengthIn35mmFilm", kShort},  {0xA406, "SceneCaptureType", kShort},
    {0xA408, "Contrast", kShort},               {0xA409, "Saturation", kShort},
    {0xA40A, "Sharpness", kShort},              {0xA420, "ImageUniqueID", kAscii},
    {0xA430, "CameraOwnerName", kAscii},        {0xA431, "BodySerialNumber", kAscii},
    {0xA432, "LensSpecification", kRational},   {0xA433, "LensMake", kAscii},
    {0xA434, "LensModel", kAscii},              {0xA435, "LensSerialNumber", kAscii},
};

const TagInfo kGpsTags[] = {
    {0x0000, "GPSVersionID", kByte},        {0x0001, "GPSLatitudeRef", kAscii},
    {0x0002, "GPSLatitude", kRational},     {0x0003, "GPSLongitudeRef", kAscii},
    {0x0004, "GPSLongitude", kRational},    {0x0005, "GPSAltitudeRef", kByte},
    {0x0006, "GPSAltitude", kRational},     {0x0007, "GPSTimeStamp", kRational},
    {0x000B, "GPSDOP", kRational},          {0x000C, "GPSSpeedRef", kAscii},
    {0x000D, "GPSSpeed", kRational},        {0x0010, "GPSImgDirectionRef", kAscii},
    {0x0011, "GPSImgDirection", kRational}, {0x0012, "GPSMapDatum", kAscii},
    {0x001B, "GPSProcessingMethod", kUndefined}, {0x001D, "GPSDateStamp", kAscii},
    {0x001F, "GPSHPositioningError", kRational},
};

const TagInfo kInteropTags[] = {
    {0x0001, "InteroperabilityIndex", kAscii}, {0x0002, "InteroperabilityVersion", kUndefined},
};

struct TagTable {
    const TagInfo* rows;
    size_t count;
};

TagTable table_for(Group g) {
    switch (g) {
        case Group::Image: return {kImageTags, sizeof(kImageTags) / sizeof(TagInfo)};
        case Group::Exif: return {kExifTags, sizeof(kExifTags) / sizeof(TagInfo)};
        case Group::GPS: return {kGpsTags, sizeof(kGpsTags) / sizeof(TagInfo)};
        case Group::Interop: return {kInteropTags, sizeof(kInteropTags) / sizeof(TagInfo)};
        case Group::Text: break;
    }
    return {nullptr, 0};
}

const TagInfo* lookup(Group g, uint16_t tag) {
    const TagTable t = table_for(g);
    for (size_t i = 0; i < t.count; ++i)
        if (t.rows[i].tag == tag) return &t.rows[i];
    return nullptr;
}

// Bytes one component of a type occupies.
size_t type_size(uint16_t type) {
    switch (type) {
        case kByte: case kAscii: case kSByte: case kUndefined: return 1;
        case kShort: case kSShort: return 2;
        case kLong: case kSLong: case kFloat: return 4;
        case kRational: case kSRational: case kDouble: return 8;
        default: return 0;
    }
}
// Width of the integers inside one component, for byte swapping.
size_t swap_unit(uint16_t type) {
    if (type == kRational || type == kSRational) return 4;
    return type_size(type);
}

uint16_t rd16(const uint8_t* p, bool le) { return le ? static_cast<uint16_t>(p[0] | (p[1] << 8)) : static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t rd32(const uint8_t* p, bool le) {
    return le ? (static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24))
              : (static_cast<uint32_t>(p[3]) | (static_cast<uint32_t>(p[2]) << 8) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[0]) << 24));
}
uint32_t rd32le(const uint8_t* p) { return rd32(p, true); }
void wr16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(static_cast<uint8_t>(v)); o.push_back(static_cast<uint8_t>(v >> 8)); }
void wr32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i))); }

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    return s.substr(b);
}

bool printable(const std::vector<uint8_t>& v) {
    for (uint8_t c : v)
        if (c != 0 && (c < 0x20 || c > 0x7E)) return false;
    return true;
}

std::string number_text(const Entry& e) {
    std::ostringstream o;
    const size_t sz = type_size(e.type);
    for (uint32_t i = 0; i < e.count && (i + 1) * sz <= e.value.size(); ++i) {
        if (i) o << ' ';
        const uint8_t* p = e.value.data() + i * sz;
        switch (e.type) {
            case kByte: o << static_cast<int>(p[0]); break;
            case kSByte: o << static_cast<int>(static_cast<int8_t>(p[0])); break;
            case kShort: o << rd16(p, true); break;
            case kSShort: o << static_cast<int16_t>(rd16(p, true)); break;
            case kLong: o << rd32le(p); break;
            case kSLong: o << static_cast<int32_t>(rd32le(p)); break;
            case kFloat: { float f; std::memcpy(&f, p, 4); o << f; break; }
            case kDouble: { double d; std::memcpy(&d, p, 8); o << d; break; }
            case kRational: case kSRational: {
                const int64_t n = e.type == kRational ? static_cast<int64_t>(rd32le(p)) : static_cast<int32_t>(rd32le(p));
                const int64_t d = e.type == kRational ? static_cast<int64_t>(rd32le(p + 4)) : static_cast<int32_t>(rd32le(p + 4));
                if (d == 0) o << n << "/0";
                else if (n % d == 0) o << (n / d);
                else if (n == 1 || (d > n && n != 0 && d % n == 0)) o << "1/" << (d / (n ? n : 1));
                else { char buf[32]; std::snprintf(buf, sizeof buf, "%g", static_cast<double>(n) / static_cast<double>(d)); o << buf; }
                break;
            }
            default: break;
        }
    }
    return o.str();
}

// "1/200", "2.8" or "5" into a rational.
bool parse_rational(const std::string& s, int64_t& num, int64_t& den) {
    const size_t slash = s.find('/');
    if (slash != std::string::npos) {
        num = std::strtoll(s.substr(0, slash).c_str(), nullptr, 10);
        den = std::strtoll(s.substr(slash + 1).c_str(), nullptr, 10);
        return den != 0;
    }
    const double v = std::strtod(s.c_str(), nullptr);
    if (!std::isfinite(v)) return false;
    den = 1;
    double scaled = v;
    while (den < 1000000 && std::fabs(scaled - std::llround(scaled)) > 1e-9) { den *= 10; scaled = v * static_cast<double>(den); }
    num = std::llround(scaled);
    const int64_t g = std::gcd(num < 0 ? -num : num, den);
    if (g > 1) { num /= g; den /= g; }
    return true;
}

std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

uint32_t crc32_of(const uint8_t* data, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 255] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void png_chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    const uint32_t len = static_cast<uint32_t>(data.size());
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>(len >> (8 * i)));
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    const uint32_t crc = crc32_of(td.data(), td.size());
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>(crc >> (8 * i)));
}

bool is_metadata_chunk(const uint8_t* type) {
    return std::memcmp(type, "tEXt", 4) == 0 || std::memcmp(type, "zTXt", 4) == 0 ||
           std::memcmp(type, "iTXt", 4) == 0 || std::memcmp(type, "eXIf", 4) == 0;
}

}  // namespace

const char* group_name(Group g) {
    switch (g) {
        case Group::Image: return "Image";
        case Group::Exif: return "Exif";
        case Group::GPS: return "GPS";
        case Group::Interop: return "Interop";
        case Group::Text: return "Text";
    }
    return "";
}

std::string Entry::name() const {
    if (group == Group::Text) return key;
    if (const TagInfo* t = lookup(group, tag)) return t->name;
    char buf[16];
    std::snprintf(buf, sizeof buf, "Tag 0x%04X", tag);
    return buf;
}

std::string Entry::text() const {
    if (group == Group::Text) return std::string(value.begin(), value.end());
    if (type == kAscii) return trim(std::string(value.begin(), value.end()));
    if (type == kUndefined) {
        if (tag == 0x9286 && value.size() > 8) {   // UserComment: an 8-byte character code first
            if (std::memcmp(value.data(), "ASCII", 5) == 0) return trim(std::string(value.begin() + 8, value.end()));
            return "(" + std::to_string(value.size() - 8) + " bytes)";
        }
        if (printable(value)) return trim(std::string(value.begin(), value.end()));
        return "(" + std::to_string(value.size()) + " bytes)";
    }
    // Windows XP tags hold UTF-16LE in a BYTE array; show the ASCII of it.
    if (type == kByte && tag >= 0x9C9B && tag <= 0x9C9F) {
        std::string s;
        for (size_t i = 0; i + 1 < value.size(); i += 2)
            if (value[i]) s.push_back(static_cast<char>(value[i]));
        return trim(s);
    }
    return number_text(*this);
}

bool Entry::editable() const {
    if (group == Group::Text) return true;
    if (type == kAscii) return true;
    if (type == kUndefined) return tag == 0x9286;
    if (type == kByte && tag >= 0x9C9B && tag <= 0x9C9F) return true;
    return type_size(type) != 0 && count <= 8 && type != kFloat && type != kDouble;
}

bool Entry::set_text(const std::string& s) {
    if (group == Group::Text) { value.assign(s.begin(), s.end()); count = static_cast<uint32_t>(value.size()); return true; }
    if (type == kAscii) {
        value.assign(s.begin(), s.end());
        value.push_back(0);
        count = static_cast<uint32_t>(value.size());
        return true;
    }
    if (type == kUndefined && tag == 0x9286) {
        value.assign(8, 0);
        std::memcpy(value.data(), "ASCII\0\0\0", 8);
        value.insert(value.end(), s.begin(), s.end());
        count = static_cast<uint32_t>(value.size());
        return true;
    }
    if (type == kByte && tag >= 0x9C9B && tag <= 0x9C9F) {
        value.clear();
        for (char c : s) { value.push_back(static_cast<uint8_t>(c)); value.push_back(0); }
        value.push_back(0);
        value.push_back(0);
        count = static_cast<uint32_t>(value.size());
        return true;
    }
    const std::vector<std::string> words = split_words(s);
    if (words.empty()) return false;
    std::vector<uint8_t> out;
    for (const std::string& w : words) {
        switch (type) {
            case kByte: case kSByte: out.push_back(static_cast<uint8_t>(std::strtol(w.c_str(), nullptr, 10))); break;
            case kShort: case kSShort: wr16(out, static_cast<uint16_t>(std::strtol(w.c_str(), nullptr, 10))); break;
            case kLong: case kSLong: wr32(out, static_cast<uint32_t>(std::strtoll(w.c_str(), nullptr, 10))); break;
            case kRational: case kSRational: {
                int64_t n = 0, d = 1;
                if (!parse_rational(w, n, d)) return false;
                wr32(out, static_cast<uint32_t>(n));
                wr32(out, static_cast<uint32_t>(d));
                break;
            }
            default: return false;
        }
    }
    value = std::move(out);
    count = static_cast<uint32_t>(words.size());
    return true;
}

const Entry* Metadata::find(Group g, uint16_t tag) const {
    for (const Entry& e : entries)
        if (e.group == g && e.tag == tag) return &e;
    return nullptr;
}

const Entry* Metadata::find_text(const std::string& key) const {
    for (const Entry& e : entries)
        if (e.group == Group::Text && e.key == key) return &e;
    return nullptr;
}

bool Metadata::set(Group g, uint16_t tag, const std::string& text) {
    if (g == Group::Text) return false;
    for (Entry& e : entries)
        if (e.group == g && e.tag == tag) return e.set_text(text);
    const TagInfo* t = lookup(g, tag);
    if (!t) return false;
    Entry e;
    e.group = g;
    e.tag = tag;
    e.type = t->type;
    if (!e.set_text(text)) return false;
    entries.push_back(std::move(e));
    sort();
    return true;
}

bool Metadata::set_text(const std::string& key, const std::string& value) {
    if (key.empty() || key.size() > 79) return false;
    for (Entry& e : entries)
        if (e.group == Group::Text && e.key == key) return e.set_text(value);
    Entry e;
    e.group = Group::Text;
    e.key = key;
    e.type = kAscii;
    e.set_text(value);
    entries.push_back(std::move(e));
    sort();
    return true;
}

void Metadata::remove(Group g, uint16_t tag) {
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.group == g && e.tag == tag; }), entries.end());
}

void Metadata::remove_text(const std::string& key) {
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.group == Group::Text && e.key == key; }), entries.end());
}

void Metadata::remove_private() {
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry& e) {
                      if (e.group == Group::GPS) return true;
                      if (e.group != Group::Exif) return false;
                      return e.tag == 0x927C || e.tag == 0xA430 || e.tag == 0xA431 || e.tag == 0xA435 || e.tag == 0xA420;
                  }),
                  entries.end());
}

void Metadata::sort() {
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.group != b.group) return static_cast<int>(a.group) < static_cast<int>(b.group);
        if (a.group == Group::Text) return a.key < b.key;
        return a.tag < b.tag;
    });
}

uint16_t tag_for_name(Group g, const std::string& name) {
    const TagTable t = table_for(g);
    for (size_t i = 0; i < t.count; ++i)
        if (name == t.rows[i].name) return t.rows[i].tag;
    if (name.size() > 2 && name.compare(0, 2, "0x") == 0) return static_cast<uint16_t>(std::strtoul(name.c_str() + 2, nullptr, 16));
    return 0;
}

namespace {

void parse_ifd(const uint8_t* base, size_t size, uint32_t off, bool le, Group group, Metadata& md, std::set<uint32_t>& seen, int depth) {
    if (depth > 4 || off == 0 || off + 2 > size || !seen.insert(off).second) return;
    const uint32_t n = rd16(base + off, le);
    if (off + 2 + static_cast<size_t>(n) * 12 > size) return;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* p = base + off + 2 + static_cast<size_t>(i) * 12;
        const uint16_t tag = rd16(p, le);
        const uint16_t type = rd16(p + 2, le);
        const uint32_t count = rd32(p + 4, le);
        if (tag == 0x8769) { parse_ifd(base, size, rd32(p + 8, le), le, Group::Exif, md, seen, depth + 1); continue; }
        if (tag == 0x8825) { parse_ifd(base, size, rd32(p + 8, le), le, Group::GPS, md, seen, depth + 1); continue; }
        if (tag == 0xA005) { parse_ifd(base, size, rd32(p + 8, le), le, Group::Interop, md, seen, depth + 1); continue; }
        const size_t esz = type_size(type);
        if (esz == 0 || count == 0 || count > (1u << 24)) continue;
        const size_t bytes = esz * count;
        const uint8_t* src = p + 8;
        if (bytes > 4) {
            const uint32_t voff = rd32(p + 8, le);
            if (voff + bytes > size) continue;
            src = base + voff;
        }
        Entry e;
        e.group = group;
        e.tag = tag;
        e.type = type;
        e.count = count;
        e.value.assign(src, src + bytes);
        if (!le) {
            const size_t unit = swap_unit(type);
            if (unit > 1)
                for (size_t b = 0; b + unit <= e.value.size(); b += unit) std::reverse(e.value.begin() + b, e.value.begin() + b + unit);
        }
        md.entries.push_back(std::move(e));
    }
}

// One directory's worth of entries, ready to serialize.
struct Dir {
    std::vector<const Entry*> entries;
    uint32_t offset = 0;
    size_t bytes() const { return 2 + entries.size() * 12 + 4; }
};

}  // namespace

Metadata parse_tiff(const uint8_t* data, size_t size) {
    Metadata md;
    if (!data || size < 8) return md;
    const bool le = data[0] == 'I' && data[1] == 'I';
    if (!le && !(data[0] == 'M' && data[1] == 'M')) return md;
    if (rd16(data + 2, le) != 42) return md;
    std::set<uint32_t> seen;
    parse_ifd(data, size, rd32(data + 4, le), le, Group::Image, md, seen, 0);
    md.sort();
    return md;
}

Metadata parse_jpeg(const uint8_t* data, size_t size) {
    Metadata md;
    if (!data || size < 4 || data[0] != 0xFF || data[1] != 0xD8) return md;
    size_t p = 2;
    while (p + 4 <= size) {
        if (data[p] != 0xFF) { ++p; continue; }
        const uint8_t marker = data[p + 1];
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { p += 2; continue; }
        if (marker == 0xD9 || marker == 0xDA) break;
        const size_t len = (static_cast<size_t>(data[p + 2]) << 8) | data[p + 3];
        if (len < 2 || p + 2 + len > size) break;
        if (marker == 0xE1 && len > 10 && std::memcmp(data + p + 4, "Exif\0\0", 6) == 0)
            return parse_tiff(data + p + 10, len - 8);
        p += 2 + len;
    }
    return md;
}

Metadata parse_png(const uint8_t* data, size_t size) {
    Metadata md;
    if (!data || size < 8 || data[0] != 0x89 || std::memcmp(data + 1, "PNG", 3) != 0) return md;
    size_t p = 8;
    while (p + 12 <= size) {
        const uint32_t len = (static_cast<uint32_t>(data[p]) << 24) | (static_cast<uint32_t>(data[p + 1]) << 16) |
                             (static_cast<uint32_t>(data[p + 2]) << 8) | data[p + 3];
        if (p + 12 + static_cast<size_t>(len) > size) break;
        const uint8_t* type = data + p + 4;
        const uint8_t* body = data + p + 8;
        if (std::memcmp(type, "eXIf", 4) == 0) {
            Metadata exif = parse_tiff(body, len);
            md.entries.insert(md.entries.end(), exif.entries.begin(), exif.entries.end());
        } else if (std::memcmp(type, "tEXt", 4) == 0 || std::memcmp(type, "zTXt", 4) == 0 || std::memcmp(type, "iTXt", 4) == 0) {
            size_t q = 0;
            while (q < len && body[q]) ++q;
            const std::string key(reinterpret_cast<const char*>(body), q);
            std::string text;
            if (std::memcmp(type, "tEXt", 4) == 0) {
                if (q + 1 <= len) text.assign(reinterpret_cast<const char*>(body + q + 1), len - q - 1);
            } else if (std::memcmp(type, "zTXt", 4) == 0) {
                if (q + 2 < len) {
                    int outlen = 0;
                    if (char* z = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(body + q + 2), static_cast<int>(len - q - 2), &outlen)) {
                        text.assign(z, z + outlen);
                        std::free(z);
                    }
                }
            } else {   // iTXt: keyword, compression flag and method, language, translated keyword, text
                size_t r = q + 1;
                if (r + 1 >= len) { p += 12 + len; continue; }
                const uint8_t compressed = body[r];
                r += 2;
                while (r < len && body[r]) ++r;   // language tag
                ++r;
                while (r < len && body[r]) ++r;   // translated keyword
                ++r;
                if (r <= len) {
                    if (!compressed) text.assign(reinterpret_cast<const char*>(body + r), len - r);
                    else {
                        int outlen = 0;
                        if (char* z = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(body + r), static_cast<int>(len - r), &outlen)) {
                            text.assign(z, z + outlen);
                            std::free(z);
                        }
                    }
                }
            }
            if (!key.empty()) {
                Entry e;
                e.group = Group::Text;
                e.key = key;
                e.type = kAscii;
                e.value.assign(text.begin(), text.end());
                e.count = static_cast<uint32_t>(e.value.size());
                md.entries.push_back(std::move(e));
            }
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            break;
        }
        p += 12 + len;
    }
    md.sort();
    return md;
}

std::vector<uint8_t> build_tiff(const Metadata& md) {
    Dir image, exif, gps, interop;
    for (const Entry& e : md.entries) {
        if (type_size(e.type) == 0 || e.value.empty()) continue;
        switch (e.group) {
            case Group::Image: image.entries.push_back(&e); break;
            case Group::Exif: exif.entries.push_back(&e); break;
            case Group::GPS: gps.entries.push_back(&e); break;
            case Group::Interop: interop.entries.push_back(&e); break;
            case Group::Text: break;
        }
    }
    if (image.entries.empty() && exif.entries.empty() && gps.entries.empty() && interop.entries.empty()) return {};

    // The pointer entries live in IFD0 (and Interop's in the Exif IFD).
    const size_t image_extra = (exif.entries.empty() && interop.entries.empty() ? 0 : 1) + (gps.entries.empty() ? 0 : 1);
    const size_t exif_extra = interop.entries.empty() ? 0 : 1;
    const size_t image_count = image.entries.size() + image_extra;
    const size_t exif_count = exif.entries.size() + exif_extra;

    image.offset = 8;
    exif.offset = static_cast<uint32_t>(image.offset + 2 + image_count * 12 + 4);
    gps.offset = static_cast<uint32_t>(exif.offset + (exif_count ? 2 + exif_count * 12 + 4 : 0));
    interop.offset = static_cast<uint32_t>(gps.offset + (gps.entries.empty() ? 0 : 2 + gps.entries.size() * 12 + 4));
    uint32_t data_at = static_cast<uint32_t>(interop.offset + (interop.entries.empty() ? 0 : 2 + interop.entries.size() * 12 + 4));

    std::vector<uint8_t> out, pool;
    out.push_back('I');
    out.push_back('I');
    wr16(out, 42);
    wr32(out, image.offset);

    auto put_entry = [&](uint16_t tag, uint16_t type, uint32_t count, const std::vector<uint8_t>& value) {
        wr16(out, tag);
        wr16(out, type);
        wr32(out, count);
        if (value.size() <= 4) {
            for (size_t i = 0; i < 4; ++i) out.push_back(i < value.size() ? value[i] : 0);
        } else {
            wr32(out, data_at + static_cast<uint32_t>(pool.size()));
            pool.insert(pool.end(), value.begin(), value.end());
            if (pool.size() & 1) pool.push_back(0);   // IFD values stay word aligned
        }
    };
    auto put_pointer = [&](uint16_t tag, uint32_t value) {
        wr16(out, tag);
        wr16(out, kLong);
        wr32(out, 1);
        wr32(out, value);
    };

    // Entries within one directory must be in ascending tag order.
    auto write_dir = [&](Dir& d, size_t extra_count, const std::function<void(uint16_t)>& pointers) {
        if (d.entries.empty() && extra_count == 0) return;
        std::stable_sort(d.entries.begin(), d.entries.end(), [](const Entry* a, const Entry* b) { return a->tag < b->tag; });
        wr16(out, static_cast<uint16_t>(d.entries.size() + extra_count));
        for (const Entry* e : d.entries) {
            pointers(e->tag);
            put_entry(e->tag, e->type, e->count, e->value);
        }
        pointers(0xFFFF);
        wr32(out, 0);   // no next directory: the thumbnail is not carried over
    };

    uint16_t written_pointers = 0;
    write_dir(image, image_extra, [&](uint16_t before) {
        if (!exif.entries.empty() || !interop.entries.empty()) {
            if (0x8769 < before && !(written_pointers & 1)) { put_pointer(0x8769, exif.offset); written_pointers |= 1; }
        }
        if (!gps.entries.empty()) {
            if (0x8825 < before && !(written_pointers & 2)) { put_pointer(0x8825, gps.offset); written_pointers |= 2; }
        }
    });
    write_dir(exif, exif_extra, [&](uint16_t before) {
        if (!interop.entries.empty() && 0xA005 < before && !(written_pointers & 4)) { put_pointer(0xA005, interop.offset); written_pointers |= 4; }
    });
    write_dir(gps, 0, [](uint16_t) {});
    write_dir(interop, 0, [](uint16_t) {});

    out.insert(out.end(), pool.begin(), pool.end());
    return out;
}

std::vector<uint8_t> apply_jpeg(const std::vector<uint8_t>& file, const Metadata& md) {
    if (file.size() < 4 || file[0] != 0xFF || file[1] != 0xD8) return file;
    std::vector<uint8_t> app1;
    if (std::vector<uint8_t> tiff = build_tiff(md); !tiff.empty() && tiff.size() + 8 <= 65535) {
        app1.push_back(0xFF);
        app1.push_back(0xE1);
        const size_t len = tiff.size() + 8;
        app1.push_back(static_cast<uint8_t>(len >> 8));
        app1.push_back(static_cast<uint8_t>(len));
        const char* sig = "Exif\0\0";
        app1.insert(app1.end(), sig, sig + 6);
        app1.insert(app1.end(), tiff.begin(), tiff.end());
    }

    std::vector<uint8_t> out;
    out.reserve(file.size() + app1.size());
    out.push_back(0xFF);
    out.push_back(0xD8);
    size_t p = 2;
    bool inserted = false;
    while (p + 4 <= file.size()) {
        if (file[p] != 0xFF) break;
        const uint8_t marker = file[p + 1];
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { out.push_back(file[p]); out.push_back(marker); p += 2; continue; }
        if (marker == 0xD9 || marker == 0xDA) break;
        const size_t len = (static_cast<size_t>(file[p + 2]) << 8) | file[p + 3];
        if (len < 2 || p + 2 + len > file.size()) break;
        const bool is_exif = marker == 0xE1 && len > 8 && std::memcmp(&file[p + 4], "Exif\0\0", 6) == 0;
        if (!is_exif) {
            // Exif belongs before everything but a JFIF header.
            if (!inserted && marker != 0xE0) { out.insert(out.end(), app1.begin(), app1.end()); inserted = true; }
            out.insert(out.end(), file.begin() + p, file.begin() + p + 2 + len);
        }
        p += 2 + len;
    }
    if (!inserted) out.insert(out.end(), app1.begin(), app1.end());
    out.insert(out.end(), file.begin() + p, file.end());
    return out;
}

std::vector<uint8_t> apply_png(const std::vector<uint8_t>& file, const Metadata& md) {
    if (file.size() < 8 || file[0] != 0x89 || std::memcmp(file.data() + 1, "PNG", 3) != 0) return file;
    std::vector<uint8_t> out(file.begin(), file.begin() + 8);
    size_t p = 8;
    bool inserted = false;
    while (p + 12 <= file.size()) {
        const uint32_t len = (static_cast<uint32_t>(file[p]) << 24) | (static_cast<uint32_t>(file[p + 1]) << 16) |
                             (static_cast<uint32_t>(file[p + 2]) << 8) | file[p + 3];
        if (p + 12 + static_cast<size_t>(len) > file.size()) break;
        const uint8_t* type = &file[p + 4];
        if (!is_metadata_chunk(type)) {
            out.insert(out.end(), file.begin() + p, file.begin() + p + 12 + len);
            if (!inserted && std::memcmp(type, "IHDR", 4) == 0) {
                if (std::vector<uint8_t> tiff = build_tiff(md); !tiff.empty()) png_chunk(out, "eXIf", tiff);
                for (const Entry& e : md.entries) {
                    if (e.group != Group::Text || e.key.empty() || e.key.size() > 79) continue;
                    std::vector<uint8_t> body(e.key.begin(), e.key.end());
                    body.push_back(0);
                    body.insert(body.end(), e.value.begin(), e.value.end());
                    png_chunk(out, "tEXt", body);
                }
                inserted = true;
            }
        }
        p += 12 + len;
    }
    out.insert(out.end(), file.begin() + p, file.end());
    return out;
}

}  // namespace firn::meta
