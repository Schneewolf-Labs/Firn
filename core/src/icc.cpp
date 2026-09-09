#include "firn/icc.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace firn::icc {

namespace {

uint32_t be32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
float s15f16(const uint8_t* p) { return static_cast<int32_t>(be32(p)) / 65536.0f; }

void put32(std::vector<uint8_t>& o, uint32_t v) { o.push_back(static_cast<uint8_t>(v >> 24)); o.push_back(static_cast<uint8_t>(v >> 16)); o.push_back(static_cast<uint8_t>(v >> 8)); o.push_back(static_cast<uint8_t>(v)); }
void put16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(static_cast<uint8_t>(v >> 8)); o.push_back(static_cast<uint8_t>(v)); }
void puts15(std::vector<uint8_t>& o, float v) { put32(o, static_cast<uint32_t>(static_cast<int32_t>(std::lround(v * 65536.0f)))); }
void put_tag(std::vector<uint8_t>& o, const char* sig) { o.insert(o.end(), sig, sig + 4); }

bool invert3(const float m[9], float out[9]) {
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::abs(det) < 1e-12) return false;
    const double inv = 1.0 / det;
    out[0] = static_cast<float>((e * i - f * h) * inv); out[1] = static_cast<float>((c * h - b * i) * inv); out[2] = static_cast<float>((b * f - c * e) * inv);
    out[3] = static_cast<float>((f * g - d * i) * inv); out[4] = static_cast<float>((a * i - c * g) * inv); out[5] = static_cast<float>((c * d - a * f) * inv);
    out[6] = static_cast<float>((d * h - e * g) * inv); out[7] = static_cast<float>((b * g - a * h) * inv); out[8] = static_cast<float>((a * e - b * d) * inv);
    return true;
}

// D50-adapted RGB -> XYZ matrix from xy primaries and a D65 white point
// (Bradford), the way ICC profiles for these spaces are built.
void matrix_from_primaries(float rx, float ry, float gx, float gy, float bx, float by, bool d65_white, float out[9]) {
    const float wx = d65_white ? 0.3127f : 0.3457f, wy = d65_white ? 0.3290f : 0.3585f;
    float P[9] = {rx / ry, gx / gy, bx / by, 1, 1, 1, (1 - rx - ry) / ry, (1 - gx - gy) / gy, (1 - bx - by) / by};
    float Pinv[9];
    invert3(P, Pinv);
    const float W[3] = {wx / wy, 1.0f, (1 - wx - wy) / wy};
    float S[3];
    for (int r = 0; r < 3; ++r) S[r] = Pinv[r * 3] * W[0] + Pinv[r * 3 + 1] * W[1] + Pinv[r * 3 + 2] * W[2];
    float M[9];
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) M[r * 3 + c] = P[r * 3 + c] * S[c];
    if (!d65_white) { std::memcpy(out, M, sizeof(M)); return; }
    // Bradford adaptation D65 -> D50.
    const float B[9] = {1.0478112f, 0.0228866f, -0.0501270f, 0.0295424f, 0.9904844f, -0.0170491f, -0.0092345f, 0.0150436f, 0.7521316f};
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) out[r * 3 + c] = B[r * 3] * M[c] + B[r * 3 + 1] * M[3 + c] + B[r * 3 + 2] * M[6 + c];
}

Curve srgb_curve() {
    Curve c;
    c.kind = Curve::Kind::Parametric;
    c.para_type = 3;
    c.p[0] = 2.4f; c.p[1] = 1.0f / 1.055f; c.p[2] = 0.055f / 1.055f; c.p[3] = 1.0f / 12.92f; c.p[4] = 0.04045f;
    return c;
}
Curve gamma_curve(float g) { Curve c; c.kind = Curve::Kind::Gamma; c.gamma = g; return c; }

}  // namespace

float Curve::apply(float v) const {
    v = std::clamp(v, 0.0f, 1.0f);
    switch (kind) {
        case Kind::Identity: return v;
        case Kind::Gamma: return std::pow(v, gamma);
        case Kind::Table: {
            if (table.size() < 2) return v;
            const float x = v * (table.size() - 1);
            const size_t i = std::min(table.size() - 2, static_cast<size_t>(x));
            const float t = x - i;
            return table[i] * (1 - t) + table[i + 1] * t;
        }
        case Kind::Parametric: {
            const float g = p[0], a = p[1], b = p[2], c = p[3], d = p[4], e = p[5], f = p[6];
            switch (para_type) {
                case 0: return std::pow(v, g);
                case 1: return v >= -b / a ? std::pow(a * v + b, g) : 0.0f;
                case 2: return v >= -b / a ? std::pow(a * v + b, g) + c : c;
                case 3: return v >= d ? std::pow(a * v + b, g) : c * v;
                default: return v >= d ? std::pow(a * v + b, g) + e : c * v + f;
            }
        }
    }
    return v;
}

float Curve::inverse(float y) const {
    y = std::clamp(y, 0.0f, 1.0f);
    switch (kind) {
        case Kind::Identity: return y;
        case Kind::Gamma: return std::pow(y, 1.0f / std::max(gamma, 1e-4f));
        case Kind::Parametric:
            if (para_type == 3 && p[1] > 0) { const float lin_limit = p[3] * p[4]; return y <= lin_limit ? y / std::max(p[3], 1e-6f) : (std::pow(y, 1.0f / p[0]) - p[2]) / p[1]; }
            [[fallthrough]];
        case Kind::Table: {
            // Monotonic: bisect.
            float lo = 0.0f, hi = 1.0f;
            for (int i = 0; i < 24; ++i) { const float mid = (lo + hi) * 0.5f; if (apply(mid) < y) lo = mid; else hi = mid; }
            return (lo + hi) * 0.5f;
        }
    }
    return y;
}

bool Profile::is_srgb() const {
    if (!matrix_trc) return false;
    const Profile s = srgb();
    for (int i = 0; i < 9; ++i) if (std::abs(to_xyz[i] - s.to_xyz[i]) > 0.01f) return false;
    for (float v : {0.1f, 0.5f, 0.9f}) if (std::abs(trc[0].apply(v) - s.trc[0].apply(v)) > 0.01f) return false;
    return true;
}

Profile parse(const std::vector<uint8_t>& bytes) { return parse(bytes.data(), bytes.size()); }

Profile parse(const uint8_t* d, size_t n) {
    Profile p;
    if (n < 132 || std::memcmp(d + 36, "acsp", 4) != 0) return p;
    p.color_space.assign(reinterpret_cast<const char*>(d + 16), 4);
    const uint32_t count = be32(d + 128);
    if (n < 132 + static_cast<size_t>(count) * 12) return p;
    bool have_xyz[3] = {false, false, false}, have_trc[3] = {false, false, false};
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* t = d + 132 + i * 12;
        const std::string sig(reinterpret_cast<const char*>(t), 4);
        const uint32_t off = be32(t + 4), len = be32(t + 8);
        if (off + len > n || len < 8) continue;
        const uint8_t* e = d + off;
        const std::string type(reinterpret_cast<const char*>(e), 4);
        auto chan = [&](const std::string& s) { return s[0] == 'r' ? 0 : s[0] == 'g' ? 1 : 2; };
        if ((sig == "rXYZ" || sig == "gXYZ" || sig == "bXYZ") && type == "XYZ " && len >= 20) {
            const int c = chan(sig);
            p.to_xyz[c] = s15f16(e + 8); p.to_xyz[3 + c] = s15f16(e + 12); p.to_xyz[6 + c] = s15f16(e + 16);
            have_xyz[c] = true;
        } else if (sig == "rTRC" || sig == "gTRC" || sig == "bTRC") {
            const int c = chan(sig);
            Curve& cv = p.trc[c];
            if (type == "curv" && len >= 12) {
                const uint32_t cnt = be32(e + 8);
                if (cnt == 0) cv.kind = Curve::Kind::Identity;
                else if (cnt == 1 && len >= 14) { cv.kind = Curve::Kind::Gamma; cv.gamma = be16(e + 12) / 256.0f; }
                else if (len >= 12 + cnt * 2) { cv.kind = Curve::Kind::Table; cv.table.resize(cnt); for (uint32_t k = 0; k < cnt; ++k) cv.table[k] = be16(e + 12 + k * 2) / 65535.0f; }
                have_trc[c] = true;
            } else if (type == "para" && len >= 16) {
                cv.kind = Curve::Kind::Parametric;
                cv.para_type = be16(e + 8);
                const int np[] = {1, 3, 4, 5, 7};
                const int k = np[std::clamp(cv.para_type, 0, 4)];
                for (int i = 0; i < k && len >= 12 + static_cast<uint32_t>(i + 1) * 4; ++i) cv.p[i] = s15f16(e + 12 + i * 4);
                have_trc[c] = true;
            }
        } else if (sig == "desc") {
            if (type == "desc" && len >= 12) { const uint32_t l = be32(e + 8); if (l > 0 && 12 + l <= len) p.description.assign(reinterpret_cast<const char*>(e + 12), l - 1); }
            else if (type == "mluc" && len >= 28) {
                const uint32_t l = be32(e + 20), o = be32(e + 24);
                if (o + l <= len) for (uint32_t k = 0; k + 1 < l; k += 2) { const uint16_t ch = be16(e + o + k); if (ch) p.description += ch < 128 ? static_cast<char>(ch) : '?'; }
            }
        }
    }
    p.valid = true;
    p.matrix_trc = p.color_space == "RGB " && have_xyz[0] && have_xyz[1] && have_xyz[2] && have_trc[0] && have_trc[1] && have_trc[2];
    return p;
}

Profile srgb() {
    Profile p; p.valid = p.matrix_trc = true; p.description = "sRGB IEC61966-2.1"; p.color_space = "RGB ";
    matrix_from_primaries(0.64f, 0.33f, 0.30f, 0.60f, 0.15f, 0.06f, true, p.to_xyz);
    for (Curve& c : p.trc) c = srgb_curve();
    return p;
}

Profile adobe_rgb() {
    Profile p; p.valid = p.matrix_trc = true; p.description = "Adobe RGB (1998)"; p.color_space = "RGB ";
    matrix_from_primaries(0.64f, 0.33f, 0.21f, 0.71f, 0.15f, 0.06f, true, p.to_xyz);
    for (Curve& c : p.trc) c = gamma_curve(563.0f / 256.0f);
    return p;
}

Profile prophoto_rgb() {
    Profile p; p.valid = p.matrix_trc = true; p.description = "ProPhoto RGB"; p.color_space = "RGB ";
    matrix_from_primaries(0.7347f, 0.2653f, 0.1596f, 0.8404f, 0.0366f, 0.0001f, false, p.to_xyz);
    for (Curve& c : p.trc) c = gamma_curve(1.8f);
    return p;
}

std::vector<uint8_t> encode(const Profile& p, const std::string& description) {
    // Tags: desc, wtpt, rXYZ, gXYZ, bXYZ, rTRC, gTRC, bTRC, cprt.
    struct Tag { const char* sig; std::vector<uint8_t> data; };
    std::vector<Tag> tags;
    auto desc_tag = [](const std::string& text) { std::vector<uint8_t> o; put_tag(o, "desc"); put32(o, 0); put32(o, static_cast<uint32_t>(text.size() + 1)); o.insert(o.end(), text.begin(), text.end()); o.push_back(0); o.resize(o.size() + 78, 0); return o; };
    auto xyz_tag = [](float x, float y, float z) { std::vector<uint8_t> o; put_tag(o, "XYZ "); put32(o, 0); puts15(o, x); puts15(o, y); puts15(o, z); return o; };
    auto curve_tag = [](const Curve& c) {
        std::vector<uint8_t> o;
        if (c.kind == Curve::Kind::Parametric) { put_tag(o, "para"); put32(o, 0); put16(o, static_cast<uint16_t>(c.para_type)); put16(o, 0); const int np[] = {1, 3, 4, 5, 7}; for (int i = 0; i < np[std::clamp(c.para_type, 0, 4)]; ++i) puts15(o, c.p[i]); }
        else if (c.kind == Curve::Kind::Gamma) { put_tag(o, "curv"); put32(o, 0); put32(o, 1); put16(o, static_cast<uint16_t>(std::lround(c.gamma * 256.0f))); }
        else { put_tag(o, "curv"); put32(o, 0); const int n = 1024; put32(o, n); for (int i = 0; i < n; ++i) put16(o, static_cast<uint16_t>(std::lround(c.apply(i / static_cast<float>(n - 1)) * 65535.0f))); }
        return o;
    };
    tags.push_back({"desc", desc_tag(description)});
    tags.push_back({"wtpt", xyz_tag(0.9642f, 1.0f, 0.8249f)});
    tags.push_back({"rXYZ", xyz_tag(p.to_xyz[0], p.to_xyz[3], p.to_xyz[6])});
    tags.push_back({"gXYZ", xyz_tag(p.to_xyz[1], p.to_xyz[4], p.to_xyz[7])});
    tags.push_back({"bXYZ", xyz_tag(p.to_xyz[2], p.to_xyz[5], p.to_xyz[8])});
    tags.push_back({"rTRC", curve_tag(p.trc[0])});
    tags.push_back({"gTRC", curve_tag(p.trc[1])});
    tags.push_back({"bTRC", curve_tag(p.trc[2])});
    { std::vector<uint8_t> o; put_tag(o, "text"); put32(o, 0); const char* t = "No copyright, use freely"; o.insert(o.end(), t, t + std::strlen(t) + 1); tags.push_back({"cprt", o}); }
    std::vector<uint8_t> out(128, 0);
    std::memcpy(out.data() + 4, "firn", 4);
    out[8] = 2; out[9] = 0x10;                      // version 2.1
    std::memcpy(out.data() + 12, "mntr", 4);
    std::memcpy(out.data() + 16, "RGB ", 4);
    std::memcpy(out.data() + 20, "XYZ ", 4);
    std::memcpy(out.data() + 36, "acsp", 4);
    { std::vector<uint8_t> w; puts15(w, 0.9642f); puts15(w, 1.0f); puts15(w, 0.8249f); std::memcpy(out.data() + 68, w.data(), 12); }
    put32(out, static_cast<uint32_t>(tags.size()));
    size_t offset = 128 + 4 + tags.size() * 12;
    std::vector<uint8_t> body;
    for (const Tag& t : tags) {
        put_tag(out, t.sig); put32(out, static_cast<uint32_t>(offset + body.size())); put32(out, static_cast<uint32_t>(t.data.size()));
        body.insert(body.end(), t.data.begin(), t.data.end());
        while (body.size() % 4) body.push_back(0);
    }
    out.insert(out.end(), body.begin(), body.end());
    const uint32_t size = static_cast<uint32_t>(out.size());
    out[0] = static_cast<uint8_t>(size >> 24); out[1] = static_cast<uint8_t>(size >> 16); out[2] = static_cast<uint8_t>(size >> 8); out[3] = static_cast<uint8_t>(size);
    return out;
}

Transform::Transform(const Profile& from, const Profile& to) {
    identity = !from.matrix_trc || !to.matrix_trc;
    for (int i = 0; i < 9; ++i) m_[i] = (i % 4 == 0) ? 1.0f : 0.0f;
    if (identity) return;
    float inv[9];
    if (!invert3(to.to_xyz, inv)) { identity = true; return; }
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) m_[r * 3 + c] = inv[r * 3] * from.to_xyz[c] + inv[r * 3 + 1] * from.to_xyz[3 + c] + inv[r * 3 + 2] * from.to_xyz[6 + c];
    for (int c = 0; c < 3; ++c) { lin_[c].resize(4096); for (int i = 0; i < 4096; ++i) lin_[c][static_cast<size_t>(i)] = from.trc[c].apply(i / 4095.0f); }
    enc_.resize(4096);
    for (int i = 0; i < 4096; ++i) enc_[static_cast<size_t>(i)] = static_cast<uint16_t>(std::lround(std::clamp(to.trc[0].inverse(i / 4095.0f), 0.0f, 1.0f) * 65535.0f));
    identity = false;
}

void Transform::apply_rect(Image& img, int x0, int y0, int x1, int y1) const {
    if (identity) return;
    for (int y = std::max(0, y0); y < std::min(img.height(), y1); ++y)
        for (int x = std::max(0, x0); x < std::min(img.width(), x1); ++x) {
            uint8_t* p = img.data() + (static_cast<size_t>(y) * img.width() + x) * 4;
            const float r = lin_[0][static_cast<size_t>(p[0]) * 4095 / 255], g = lin_[1][static_cast<size_t>(p[1]) * 4095 / 255], b = lin_[2][static_cast<size_t>(p[2]) * 4095 / 255];
            const float o[3] = {m_[0] * r + m_[1] * g + m_[2] * b, m_[3] * r + m_[4] * g + m_[5] * b, m_[6] * r + m_[7] * g + m_[8] * b};
            for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>((enc_[static_cast<size_t>(std::clamp(o[c], 0.0f, 1.0f) * 4095.0f + 0.5f)] + 128) / 257);
        }
}

void Transform::apply(Image& img) const { apply_rect(img, 0, 0, img.width(), img.height()); }

void Transform::apply(Image16& img) const {
    if (identity) return;
    uint16_t* p = img.data();
    for (size_t i = 0; i < img.size(); i += 4) {
        const float r = lin_[0][p[i] >> 4], g = lin_[1][p[i + 1] >> 4], b = lin_[2][p[i + 2] >> 4];
        const float o[3] = {m_[0] * r + m_[1] * g + m_[2] * b, m_[3] * r + m_[4] * g + m_[5] * b, m_[6] * r + m_[7] * g + m_[8] * b};
        for (int c = 0; c < 3; ++c) p[i + c] = enc_[static_cast<size_t>(std::clamp(o[c], 0.0f, 1.0f) * 4095.0f + 0.5f)];
    }
}

}  // namespace firn::icc
