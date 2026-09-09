#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "firn/image.h"

// Color management for RGB matrix/TRC ICC profiles (sRGB, Adobe RGB,
// ProPhoto, camera and monitor profiles): parsing, a few built-in
// profiles, and conversions between profiles. LUT-based (CMYK) profiles
// are recognized but not converted.
namespace firn::icc {

struct Curve {
    enum class Kind { Identity, Gamma, Table, Parametric };
    Kind kind = Kind::Identity;
    float gamma = 1.0f;
    std::vector<float> table;        // 0..1 samples, evenly spaced
    int para_type = 0;               // ICC parametricCurveType function 0..4
    float p[7] = {1, 0, 0, 0, 0, 0, 0};   // g, a, b, c, d, e, f
    float apply(float v) const;      // encoded -> linear
    float inverse(float v) const;    // linear -> encoded (numeric for tables)
};

struct Profile {
    bool valid = false;
    bool matrix_trc = false;         // RGB with rXYZ/gXYZ/bXYZ and TRCs
    std::string description;
    std::string color_space;         // "RGB ", "CMYK", "GRAY", ...
    float to_xyz[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};   // linear RGB -> XYZ (D50 PCS), row-major
    Curve trc[3];
    bool is_srgb() const;            // close enough to sRGB to skip conversion
};

Profile parse(const std::vector<uint8_t>& bytes);
Profile parse(const uint8_t* data, size_t size);
// Built-in profiles and their ICC bytes (v2 matrix/TRC).
Profile srgb();
Profile adobe_rgb();
Profile prophoto_rgb();
std::vector<uint8_t> encode(const Profile& p, const std::string& description);

// Pixel conversion between two matrix/TRC profiles (alpha untouched).
struct Transform {
    Transform(const Profile& from, const Profile& to);
    bool identity = true;
    void apply(Image& img) const;
    void apply(Image16& img) const;
    void apply_rect(Image& img, int x0, int y0, int x1, int y1) const;
private:
    std::vector<float> lin_[3];      // 4096-entry encoded -> linear
    std::vector<uint16_t> enc_;      // 4096-entry linear -> encoded (16-bit)
    float m_[9];                     // from linear RGB -> to linear RGB
};

}  // namespace firn::icc
