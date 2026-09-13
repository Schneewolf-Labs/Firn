// TIFF: the format print and archival photography run on, and the only one
// here that carries 16 bits a channel to other programs without argument.
// Reads baseline files plus what cameras and scanners actually emit -- LZW,
// Deflate and PackBits, strips or tiles, 8 or 16 bits, grey, palette, RGB
// and RGBA. Writes one strip-per-band Deflate image with the horizontal
// predictor, which is what every other writer produces for photographs.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

#include "firn/document.h"
#include "firn/io_psp.h"
#include "firn/metadata.h"
#include "stb/stb_image.h"

extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace firn::io {

namespace {

// TIFF field types, by their numbers in the file.
enum : uint16_t { kByte = 1, kAscii = 2, kShort = 3, kLong = 4, kRational = 5, kUndefined = 7, kSShort = 8, kSLong = 9 };

struct Reader {
    const std::vector<uint8_t>& d;
    bool big = false;
    bool ok(size_t p, size_t n) const { return p + n <= d.size(); }
    uint16_t u16(size_t p) const {
        if (!ok(p, 2)) return 0;
        return big ? static_cast<uint16_t>((d[p] << 8) | d[p + 1]) : static_cast<uint16_t>((d[p + 1] << 8) | d[p]);
    }
    uint32_t u32(size_t p) const {
        if (!ok(p, 4)) return 0;
        return big ? (static_cast<uint32_t>(d[p]) << 24 | d[p + 1] << 16 | d[p + 2] << 8 | d[p + 3])
                   : (static_cast<uint32_t>(d[p + 3]) << 24 | d[p + 2] << 16 | d[p + 1] << 8 | d[p]);
    }
};

size_t type_size(uint16_t t) {
    switch (t) {
        case kByte: case kAscii: case kUndefined: return 1;
        case kShort: case kSShort: return 2;
        case kLong: case kSLong: return 4;
        case kRational: return 8;
        default: return 0;
    }
}

struct Field {
    uint16_t type = 0;
    uint32_t count = 0;
    size_t at = 0;        // where the values are: inline in the entry, or pointed to
};

// One value of a field, widened to 32 bits. Out of range gives `fallback`.
uint32_t value_at(const Reader& r, const Field& f, uint32_t i, uint32_t fallback = 0) {
    if (i >= f.count) return fallback;
    const size_t p = f.at + static_cast<size_t>(i) * type_size(f.type);
    switch (f.type) {
        case kByte: case kAscii: case kUndefined: return r.ok(p, 1) ? r.d[p] : fallback;
        case kShort: case kSShort: return r.u16(p);
        case kLong: case kSLong: return r.u32(p);
        case kRational: return r.u32(p);
        default: return fallback;
    }
}

// TIFF's LZW: 8 to 12 bit codes, MSB first, 256 clears and 257 ends. Its
// codes run one ahead of the classic algorithm, which is the detail that
// makes a naive implementation drift by one entry and produce noise.
bool lzw_decode(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t expect) {
    struct Entry { int prev; uint8_t byte; uint16_t len; };
    std::vector<Entry> table(4096);
    auto reset = [&table]() {
        for (int i = 0; i < 256; ++i) table[static_cast<size_t>(i)] = {-1, static_cast<uint8_t>(i), 1};
    };
    reset();
    int next = 258, width = 9, prev = -1;
    size_t bit = 0;
    out.clear();
    out.reserve(expect);
    std::vector<uint8_t> run;
    while ((bit + static_cast<size_t>(width)) <= n * 8) {
        uint32_t code = 0;
        for (int k = 0; k < width; ++k) {
            const size_t b = bit + static_cast<size_t>(k);
            code = (code << 1) | ((src[b >> 3] >> (7 - (b & 7))) & 1u);
        }
        bit += static_cast<size_t>(width);
        if (code == 257) break;
        if (code == 256) { next = 258; width = 9; prev = -1; continue; }
        if (code > 4095) return false;
        int walk;
        if (static_cast<int>(code) < next && (code < 256 || table[code].len)) {
            walk = static_cast<int>(code);
        } else if (prev >= 0 && next < 4096) {
            // The code the encoder made from the previous string plus its
            // own first byte, which it may use before we have built it.
            walk = prev;
        } else return false;
        run.clear();
        for (int w = walk; w >= 0; w = table[static_cast<size_t>(w)].prev) run.push_back(table[static_cast<size_t>(w)].byte);
        std::reverse(run.begin(), run.end());
        if (static_cast<int>(code) >= next) run.push_back(run.front());
        out.insert(out.end(), run.begin(), run.end());
        if (prev >= 0 && next < 4096) {
            table[static_cast<size_t>(next)] = {prev, run.front(), static_cast<uint16_t>(table[static_cast<size_t>(prev)].len + 1)};
            ++next;
        }
        prev = static_cast<int>(code) < next ? static_cast<int>(code) : prev;
        // Widen one code early, the way TIFF writers emit it.
        if (next + 1 >= (1 << width) && width < 12) ++width;
        if (out.size() > expect * 2 + 64) break;
    }
    return !out.empty();
}

bool pack_bits_decode(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t expect) {
    out.clear();
    out.reserve(expect);
    size_t p = 0;
    while (p < n && out.size() < expect) {
        const int8_t c = static_cast<int8_t>(src[p++]);
        if (c >= 0) {
            const size_t cnt = std::min<size_t>(static_cast<size_t>(c) + 1, n - p);
            out.insert(out.end(), src + p, src + p + cnt);
            p += cnt;
        } else if (c != -128) {
            if (p >= n) break;
            out.insert(out.end(), static_cast<size_t>(1 - c), src[p++]);
        }
    }
    return true;
}

bool inflate_to(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    int len = 0;
    char* z = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(src), static_cast<int>(n), &len);
    if (!z) return false;
    out.assign(z, z + len);
    std::free(z);
    return true;
}

// Undoes horizontal differencing, which is how LZW and Deflate files get
// most of their compression on photographs.
void unpredict(std::vector<uint8_t>& row, int w, int samples, int bits, bool big) {
    if (bits == 8) {
        for (int x = samples; x < w * samples; ++x)
            row[static_cast<size_t>(x)] = static_cast<uint8_t>(row[static_cast<size_t>(x)] + row[static_cast<size_t>(x - samples)]);
    } else if (bits == 16) {
        // Sixteen-bit samples are in the file's byte order, not ours.
        const size_t hi = big ? 0 : 1, lo = big ? 1 : 0;
        auto get = [&row, hi, lo](size_t i) { return static_cast<uint16_t>((row[i * 2 + hi] << 8) | row[i * 2 + lo]); };
        auto put = [&row, hi, lo](size_t i, uint16_t v) { row[i * 2 + hi] = static_cast<uint8_t>(v >> 8); row[i * 2 + lo] = static_cast<uint8_t>(v); };
        for (int x = samples; x < w * samples; ++x)
            put(static_cast<size_t>(x), static_cast<uint16_t>(get(static_cast<size_t>(x)) + get(static_cast<size_t>(x - samples))));
    }
}

}  // namespace

std::unique_ptr<Document> load_tiff_from_memory(const uint8_t* data, size_t size, std::string* err, std::vector<std::string>* warnings) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return std::unique_ptr<Document>(); };
    const std::vector<uint8_t> d(data, data + size);
    if (d.size() < 8) return fail("not a TIFF file");
    Reader r{d};
    if (std::memcmp(d.data(), "MM", 2) == 0) r.big = true;
    else if (std::memcmp(d.data(), "II", 2) != 0) return fail("not a TIFF file");
    if (r.u16(2) != 42) return fail(r.u16(2) == 43 ? "BigTIFF is not supported" : "not a TIFF file");

    const size_t ifd = r.u32(4);
    if (!r.ok(ifd, 2)) return fail("truncated TIFF directory");
    const uint16_t entries = r.u16(ifd);
    std::vector<std::pair<uint16_t, Field>> fields;
    for (uint16_t i = 0; i < entries; ++i) {
        const size_t e = ifd + 2 + static_cast<size_t>(i) * 12;
        if (!r.ok(e, 12)) break;
        const uint16_t tag = r.u16(e), type = r.u16(e + 2);
        const uint32_t count = r.u32(e + 4);
        const size_t sz = type_size(type);
        if (!sz) continue;
        Field fl;
        fl.type = type;
        fl.count = count;
        fl.at = sz * count <= 4 ? e + 8 : r.u32(e + 8);
        fields.emplace_back(tag, fl);
    }
    auto field = [&fields](uint16_t tag) -> const Field* {
        for (const auto& [t, f] : fields) if (t == tag) return &f;
        return nullptr;
    };
    auto scalar = [&](uint16_t tag, uint32_t fallback) {
        const Field* f = field(tag);
        return f ? value_at(r, *f, 0, fallback) : fallback;
    };

    const int w = static_cast<int>(scalar(256, 0)), h = static_cast<int>(scalar(257, 0));
    if (w <= 0 || h <= 0 || w > 65536 || h > 65536) return fail("bad image size");
    const int samples = static_cast<int>(scalar(277, 1));
    const Field* bps_f = field(258);
    const int bits = bps_f ? static_cast<int>(value_at(r, *bps_f, 0, 8)) : 8;
    const uint32_t compression = scalar(259, 1);
    const uint32_t photometric = scalar(262, samples >= 3 ? 2u : 1u);
    const uint32_t planar = scalar(284, 1);
    const uint32_t predictor = scalar(317, 1);
    // 1, 2 and 4 bits are baseline: bilevel scans and small palettes.
    if (bits != 1 && bits != 2 && bits != 4 && bits != 8 && bits != 16) return fail("only 1, 2, 4, 8 and 16 bits per channel are supported");
    if (bits < 8 && samples != 1) return fail("sub-byte samples are only supported for a single channel");
    if (planar != 1) return fail("planar TIFF files are not supported");
    if (samples < 1 || samples > 4) return fail("only 1 to 4 samples per pixel are supported");
    if (photometric == 5) return fail("CMYK files are not supported: convert to RGB first");
    if (photometric == 6) return fail("YCbCr files are not supported");
    // Say which encoding is missing rather than leaving a blank image and a
    // vague complaint: these are whole compression schemes, not damage.
    if (compression == 2 || compression == 3 || compression == 4)
        return fail("CCITT fax compression is not supported; re-save the file with LZW or Deflate");
    if (compression == 6 || compression == 7 || compression == 34892)
        return fail("JPEG-compressed TIFF is not supported; re-save the file with LZW or Deflate");
    if (compression != 1 && compression != 5 && compression != 8 && compression != 32946 && compression != 32773)
        return fail("unsupported TIFF compression " + std::to_string(compression));

    // Strips and tiles are the same job with different rectangles.
    const Field* tile_w_f = field(322);
    const bool tiled = tile_w_f != nullptr;
    const int tile_w = tiled ? static_cast<int>(scalar(322, 0)) : w;
    const int tile_h = tiled ? static_cast<int>(scalar(323, 0)) : static_cast<int>(scalar(278, static_cast<uint32_t>(h)));
    if (tile_w <= 0 || tile_h <= 0) return fail("bad strip or tile size");
    const Field* off_f = field(tiled ? 324 : 273);
    const Field* cnt_f = field(tiled ? 325 : 279);
    if (!off_f || !cnt_f) return fail("TIFF has no image data");
    const int across = tiled ? (w + tile_w - 1) / tile_w : 1;
    const int down = (h + tile_h - 1) / tile_h;
    const size_t blocks = static_cast<size_t>(across) * down;

    const int sample_bytes = std::max(1, bits / 8);
    const size_t block_row = (static_cast<size_t>(tile_w) * samples * bits + 7) / 8;

    auto doc = std::make_unique<Document>(w, h);
    Layer& L = doc->add_layer("Background");
    L.background = samples != 2 && samples != 4;
    uint8_t* px = L.pixels.data();
    std::fill(px, px + static_cast<size_t>(w) * h * 4, 0);

    // A palette image looks up its colours; the map is three shorts a slot.
    const Field* map_f = field(320);

    std::vector<uint8_t> raw, block;
    bool any = false, complained = false;
    for (size_t b = 0; b < blocks && b < off_f->count; ++b) {
        const size_t at = value_at(r, *off_f, static_cast<uint32_t>(b), 0);
        const size_t len = value_at(r, *cnt_f, static_cast<uint32_t>(b), 0);
        if (!r.ok(at, len) || !len) continue;
        const size_t want = block_row * static_cast<size_t>(tile_h);
        bool got = false;
        switch (compression) {
            case 1: block.assign(d.begin() + static_cast<long>(at), d.begin() + static_cast<long>(at + len)); got = true; break;
            case 5: got = lzw_decode(d.data() + at, len, block, want); break;
            case 8: case 32946: got = inflate_to(d.data() + at, len, block); break;
            case 32773: got = pack_bits_decode(d.data() + at, len, block, want); break;
            default: break;
        }
        if (!got) {
            if (!complained && warnings) { warnings->push_back("TIFF: unsupported compression " + std::to_string(compression) + ", parts of the image are blank"); complained = true; }
            continue;
        }
        block.resize(want, 0);
        const int bx = tiled ? static_cast<int>(b % static_cast<size_t>(across)) * tile_w : 0;
        const int by = static_cast<int>(tiled ? (b / static_cast<size_t>(across)) : b) * tile_h;
        for (int ty = 0; ty < tile_h; ++ty) {
            const int y = by + ty;
            if (y >= h) break;
            std::vector<uint8_t> row(block.begin() + static_cast<long>(static_cast<size_t>(ty) * block_row),
                                     block.begin() + static_cast<long>(static_cast<size_t>(ty + 1) * block_row));
            if (predictor == 2 && bits >= 8) unpredict(row, tile_w, samples, bits, r.big);
            for (int tx = 0; tx < tile_w; ++tx) {
                const int x = bx + tx;
                if (x >= w) break;
                // A packed sample, widened to the 0..255 the document holds.
                auto raw_sample = [&](int s) -> uint32_t {
                    if (bits < 8) {
                        const size_t bit = static_cast<size_t>(tx) * samples * bits + static_cast<size_t>(s) * bits;
                        if ((bit + static_cast<size_t>(bits) + 7) / 8 > row.size()) return 0;
                        const uint8_t byte = row[bit / 8];
                        const int shift = 8 - bits - static_cast<int>(bit % 8);
                        return (byte >> shift) & ((1u << bits) - 1u);
                    }
                    const size_t i = (static_cast<size_t>(tx) * samples + static_cast<size_t>(s)) * static_cast<size_t>(sample_bytes);
                    if (i + static_cast<size_t>(sample_bytes) > row.size()) return 0;
                    // The high byte is the 8-bit value, and which byte that
                    // is depends on the file's order, not the machine's.
                    return sample_bytes == 2 ? row[i + (r.big ? 0 : 1)] : row[i];
                };
                auto sample = [&](int s) -> uint8_t {
                    const uint32_t v = raw_sample(s);
                    // A 2-bit 3 has to become 255, not 3, so the value is
                    // stretched rather than shifted.
                    return bits < 8 ? static_cast<uint8_t>(v * 255u / ((1u << bits) - 1u)) : static_cast<uint8_t>(v);
                };
                uint8_t cr = 0, cg = 0, cb = 0, ca = 255;
                if (photometric == 3 && map_f) {
                    // The palette index is the sample itself, not a value
                    // stretched to 0..255.
                    uint32_t idx = raw_sample(0);
                    if (bits == 16) {
                        const size_t pi = static_cast<size_t>(tx) * samples * 2;
                        if (pi + 1 < row.size()) idx = static_cast<uint32_t>((row[pi + (r.big ? 0 : 1)] << 8) | row[pi + (r.big ? 1 : 0)]);
                    }
                    const uint32_t n = map_f->count / 3;
                    cr = static_cast<uint8_t>(value_at(r, *map_f, idx, 0) >> 8);
                    cg = static_cast<uint8_t>(value_at(r, *map_f, n + idx, 0) >> 8);
                    cb = static_cast<uint8_t>(value_at(r, *map_f, 2 * n + idx, 0) >> 8);
                } else if (samples >= 3) {
                    cr = sample(0); cg = sample(1); cb = sample(2);
                    if (samples >= 4) ca = sample(3);
                } else {
                    cr = cg = cb = sample(0);
                    if (photometric == 0) { cr = cg = cb = static_cast<uint8_t>(255 - cr); }
                    if (samples == 2) ca = sample(1);
                }
                uint8_t* o = px + (static_cast<size_t>(y) * w + x) * 4;
                o[0] = cr; o[1] = cg; o[2] = cb; o[3] = ca;
            }
        }
        any = true;
    }
    if (!any) return fail("TIFF image data could not be decoded");
    if (bits == 16 && warnings) warnings->push_back("16-bit TIFF: read at 8 bits per channel");

    // The metadata a TIFF carries in its own directory.
    {
        meta::Metadata md = meta::parse_tiff(d.data(), d.size());
        if (const Field* xf = field(700); xf && xf->count) {
            md.xmp.assign(reinterpret_cast<const char*>(d.data() + xf->at), std::min<size_t>(xf->count, d.size() - xf->at));
            for (meta::Entry& e : meta::parse_xmp(md.xmp)) md.entries.push_back(std::move(e));
            md.sort();
        }
        if (!md.empty()) doc->set_metadata(std::move(md));
    }
    if (const Field* icc = field(34675); icc && icc->count && r.ok(icc->at, icc->count))
        doc->set_icc(std::vector<uint8_t>(d.begin() + static_cast<long>(icc->at), d.begin() + static_cast<long>(icc->at + icc->count)));
    return doc;
}

std::unique_ptr<Document> load_tiff(const std::string& path, std::string* err, std::vector<std::string>* warnings) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot open " + path; return nullptr; }
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return load_tiff_from_memory(d.data(), d.size(), err, warnings);
}

// --- Writing ----------------------------------------------------------------

namespace {

struct Out {
    std::vector<uint8_t> d;
    void u8(uint8_t v) { d.push_back(v); }
    void u16(uint16_t v) { d.push_back(static_cast<uint8_t>(v)); d.push_back(static_cast<uint8_t>(v >> 8)); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) d.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void bytes(const void* p, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(p); d.insert(d.end(), b, b + n); }
    void patch32(size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) d[at + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i)); }
    void align() { if (d.size() & 1) d.push_back(0); }
};

std::vector<uint8_t> zlib_pack(const std::vector<uint8_t>& in) {
    int len = 0;
    unsigned char* z = stbi_zlib_compress(const_cast<unsigned char*>(in.data()), static_cast<int>(in.size()), &len, 8);
    if (!z) return in;
    std::vector<uint8_t> out(z, z + len);
    std::free(z);
    return out;
}

}  // namespace

std::vector<uint8_t> save_tiff_to_memory(const Document& doc) {
    const int w = doc.width(), h = doc.height();
    const Image flat = doc.composite();
    const bool sixteen = doc.bit_depth() == 16;
    const Image16 deep = sixteen ? doc.composite16() : Image16();
    const int bits = sixteen ? 16 : 8;
    bool has_alpha = false;
    for (size_t k = 3; k < flat.size_bytes() && !has_alpha; k += 4) has_alpha = flat.data()[k] != 255;
    const int samples = has_alpha ? 4 : 3;
    const int sample_bytes = bits / 8;
    const size_t row_bytes = static_cast<size_t>(w) * samples * sample_bytes;

    // One strip per band of about 64 KB, which is what the specification
    // recommends and what keeps a reader's buffer small.
    const int rows_per_strip = std::max(1, static_cast<int>(65536 / std::max<size_t>(row_bytes, 1)));
    const int strips = (h + rows_per_strip - 1) / rows_per_strip;

    Out o;
    o.bytes("II", 2);
    o.u16(42);
    const size_t ifd_at = o.d.size();
    o.u32(0);   // patched once the data is behind us

    std::vector<uint32_t> offsets, counts;
    for (int s = 0; s < strips; ++s) {
        const int y0 = s * rows_per_strip, y1 = std::min(h, y0 + rows_per_strip);
        std::vector<uint8_t> band(row_bytes * static_cast<size_t>(y1 - y0));
        for (int y = y0; y < y1; ++y) {
            uint8_t* dst = band.data() + row_bytes * static_cast<size_t>(y - y0);
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(x) * samples * sample_bytes;
                if (bits == 16) {
                    const uint16_t* src = deep.data() + (static_cast<size_t>(y) * w + x) * 4;
                    for (int c = 0; c < samples; ++c) { dst[i + static_cast<size_t>(c) * 2] = static_cast<uint8_t>(src[c]); dst[i + static_cast<size_t>(c) * 2 + 1] = static_cast<uint8_t>(src[c] >> 8); }
                } else {
                    const uint8_t* src = flat.data() + (static_cast<size_t>(y) * w + x) * 4;
                    for (int c = 0; c < samples; ++c) dst[i + static_cast<size_t>(c)] = src[c];
                }
            }
        }
        // Horizontal differencing, undone by the reader's predictor 2. It is
        // where most of a photograph's compression comes from.
        for (int y = y1 - 1; y >= y0; --y) {
            uint8_t* row = band.data() + row_bytes * static_cast<size_t>(y - y0);
            if (bits == 8) {
                for (int x = w * samples - 1; x >= samples; --x)
                    row[static_cast<size_t>(x)] = static_cast<uint8_t>(row[static_cast<size_t>(x)] - row[static_cast<size_t>(x - samples)]);
            } else {
                auto get = [row](size_t i) { return static_cast<uint16_t>(row[i * 2] | (row[i * 2 + 1] << 8)); };
                auto put = [row](size_t i, uint16_t v) { row[i * 2] = static_cast<uint8_t>(v); row[i * 2 + 1] = static_cast<uint8_t>(v >> 8); };
                for (int x = w * samples - 1; x >= samples; --x)
                    put(static_cast<size_t>(x), static_cast<uint16_t>(get(static_cast<size_t>(x)) - get(static_cast<size_t>(x - samples))));
            }
        }
        const std::vector<uint8_t> packed = zlib_pack(band);
        o.align();
        offsets.push_back(static_cast<uint32_t>(o.d.size()));
        counts.push_back(static_cast<uint32_t>(packed.size()));
        o.bytes(packed.data(), packed.size());
    }

    // Values too big for an entry's four bytes live outside the directory.
    struct Entry { uint16_t tag, type; uint32_t count; std::vector<uint8_t> inline_or_pool; bool pooled; };
    std::vector<Entry> es;
    auto add_inline = [&es](uint16_t tag, uint16_t type, uint32_t count, uint32_t v) {
        Entry e{tag, type, count, {}, false};
        e.inline_or_pool.resize(4, 0);
        if (type == kShort) { e.inline_or_pool[0] = static_cast<uint8_t>(v); e.inline_or_pool[1] = static_cast<uint8_t>(v >> 8); }
        else for (int i = 0; i < 4; ++i) e.inline_or_pool[static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
        es.push_back(std::move(e));
    };
    auto add_pool = [&es](uint16_t tag, uint16_t type, uint32_t count, std::vector<uint8_t> data) {
        es.push_back({tag, type, count, std::move(data), true});
    };
    auto shorts = [](const std::vector<uint16_t>& v) {
        std::vector<uint8_t> out;
        for (const uint16_t s : v) { out.push_back(static_cast<uint8_t>(s)); out.push_back(static_cast<uint8_t>(s >> 8)); }
        return out;
    };
    auto longs = [](const std::vector<uint32_t>& v) {
        std::vector<uint8_t> out;
        for (const uint32_t s : v) for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(s >> (8 * i)));
        return out;
    };

    add_inline(256, kLong, 1, static_cast<uint32_t>(w));
    add_inline(257, kLong, 1, static_cast<uint32_t>(h));
    add_pool(258, kShort, static_cast<uint32_t>(samples), shorts(std::vector<uint16_t>(static_cast<size_t>(samples), static_cast<uint16_t>(bits))));
    add_inline(259, kShort, 1, 8);         // Deflate
    add_inline(262, kShort, 1, 2);         // RGB
    if (strips == 1) add_inline(273, kLong, 1, offsets[0]); else add_pool(273, kLong, static_cast<uint32_t>(strips), longs(offsets));
    add_inline(277, kShort, 1, static_cast<uint32_t>(samples));
    add_inline(278, kLong, 1, static_cast<uint32_t>(rows_per_strip));
    if (strips == 1) add_inline(279, kLong, 1, counts[0]); else add_pool(279, kLong, static_cast<uint32_t>(strips), longs(counts));
    add_pool(282, kRational, 1, longs({72, 1}));
    add_pool(283, kRational, 1, longs({72, 1}));
    add_inline(296, kShort, 1, 2);         // resolution in inches
    add_inline(317, kShort, 1, 2);         // horizontal predictor
    // Unassociated alpha: the colour channels are not premultiplied, which
    // is what straight-alpha RGBA means and what Firn holds.
    if (has_alpha) add_inline(338, kShort, 1, 2);
    if (const std::string packet = meta::build_xmp(doc.metadata()); !packet.empty())
        add_pool(700, kByte, static_cast<uint32_t>(packet.size()), std::vector<uint8_t>(packet.begin(), packet.end()));
    if (!doc.icc().empty()) add_pool(34675, kUndefined, static_cast<uint32_t>(doc.icc().size()), doc.icc());

    std::stable_sort(es.begin(), es.end(), [](const Entry& a, const Entry& b) { return a.tag < b.tag; });

    o.align();
    const uint32_t ifd_start = static_cast<uint32_t>(o.d.size());
    o.patch32(ifd_at, ifd_start);
    o.u16(static_cast<uint16_t>(es.size()));
    // The pool sits after the directory and its terminating next-IFD word.
    uint32_t pool_at = ifd_start + 2 + static_cast<uint32_t>(es.size()) * 12 + 4;
    std::vector<uint8_t> pool;
    for (const Entry& e : es) {
        o.u16(e.tag);
        o.u16(e.type);
        o.u32(e.count);
        if (!e.pooled) {
            o.bytes(e.inline_or_pool.data(), 4);
        } else if (e.inline_or_pool.size() <= 4) {
            std::vector<uint8_t> four = e.inline_or_pool;
            four.resize(4, 0);
            o.bytes(four.data(), 4);
        } else {
            o.u32(pool_at + static_cast<uint32_t>(pool.size()));
            pool.insert(pool.end(), e.inline_or_pool.begin(), e.inline_or_pool.end());
            if (pool.size() & 1) pool.push_back(0);
        }
    }
    o.u32(0);   // no next directory
    o.bytes(pool.data(), pool.size());
    return o.d;
}

bool save_tiff(const Document& doc, const std::string& path, std::string* err) {
    const std::vector<uint8_t> d = save_tiff_to_memory(doc);
    std::ofstream f(path, std::ios::binary);
    if (!f || !f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()))) {
        if (err) *err = "cannot write " + path;
        return false;
    }
    return true;
}

}  // namespace firn::io
