// Firn's own encoding of a vector layer's objects, for the project format.
//
// The native container stores shapes the way the original does, which is
// where .PspImage compatibility comes from and also where it stops: that
// layout has nowhere to put a dash array, a pattern image, a hidden object
// or a fractional point size. OpenRaster is Firn's own file, so it carries
// this instead and loses nothing. Pattern and texture images travel inline
// as PNG, shared between the styles that reference the same one.
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "firn/io.h"
#include "firn/vector.h"

namespace firn::io {

namespace {

constexpr uint32_t kMagic = 0x43455646;   // "FVEC"
constexpr uint32_t kVersion = 1;

struct Out {
    std::vector<uint8_t> b;
    void u8(uint32_t v) { b.push_back(static_cast<uint8_t>(v)); }
    void u16(uint32_t v) { u8(v); u8(v >> 8); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) u8(v >> (8 * i)); }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void f32(float v) { uint32_t t; std::memcpy(&t, &v, 4); u32(t); }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        b.insert(b.end(), s.begin(), s.end());
    }
    void blob(const std::vector<uint8_t>& v) {
        u32(static_cast<uint32_t>(v.size()));
        b.insert(b.end(), v.begin(), v.end());
    }
    void color(const Color& c) { u8(c.r); u8(c.g); u8(c.b); u8(c.a); }
};

struct In {
    const uint8_t* p;
    size_t n, at = 0;
    bool bad = false;
    bool want(size_t k) { if (at + k > n) { bad = true; return false; } return true; }
    uint32_t u8v() { if (!want(1)) return 0; return p[at++]; }
    uint32_t u16v() { const uint32_t a = u8v(); return a | (u8v() << 8); }
    uint32_t u32v() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= u8v() << (8 * i); return v; }
    int32_t i32v() { return static_cast<int32_t>(u32v()); }
    float f32v() { const uint32_t t = u32v(); float v; std::memcpy(&v, &t, 4); return v; }
    std::string str() {
        const uint32_t len = u32v();
        if (bad || !want(len)) return {};
        std::string s(reinterpret_cast<const char*>(p + at), len);
        at += len;
        return s;
    }
    std::vector<uint8_t> blob() {
        const uint32_t len = u32v();
        if (bad || !want(len)) return {};
        std::vector<uint8_t> v(p + at, p + at + len);
        at += len;
        return v;
    }
    Color color() { Color c; c.r = static_cast<uint8_t>(u8v()); c.g = static_cast<uint8_t>(u8v()); c.b = static_cast<uint8_t>(u8v()); c.a = static_cast<uint8_t>(u8v()); return c; }
};

// Images referenced by paint styles, written once each and referred to by
// index. -1 means the style has none.
struct ImagePool {
    std::vector<const Image*> order;
    std::map<const Image*, int> index;
    int add(const std::shared_ptr<const Image>& img) {
        if (!img || img->empty()) return -1;
        const auto it = index.find(img.get());
        if (it != index.end()) return it->second;
        const int i = static_cast<int>(order.size());
        order.push_back(img.get());
        index.emplace(img.get(), i);
        return i;
    }
};

void write_gradient(Out& o, const vec::Gradient& g) {
    o.str(g.name);
    o.u16(static_cast<uint16_t>(g.style));
    o.f32(g.angle);
    o.f32(g.center_x);
    o.f32(g.center_y);
    o.i32(g.repeats);
    o.u8(g.invert ? 1 : 0);
    o.u32(static_cast<uint32_t>(g.colors.size()));
    for (const vec::GradientStop& s : g.colors) { o.color(s.color); o.f32(s.pos); o.f32(s.mid); }
    o.u32(static_cast<uint32_t>(g.opacities.size()));
    for (const vec::OpacityStop& s : g.opacities) { o.f32(s.opacity); o.f32(s.pos); o.f32(s.mid); }
}

void read_gradient(In& i, vec::Gradient& g) {
    g.name = i.str();
    g.style = static_cast<vec::GradientStyle>(i.u16v());
    g.angle = i.f32v();
    g.center_x = i.f32v();
    g.center_y = i.f32v();
    g.repeats = i.i32v();
    g.invert = i.u8v() != 0;
    const uint32_t nc = i.u32v();
    if (i.bad || nc > (1u << 16)) { i.bad = true; return; }
    g.colors.clear();
    for (uint32_t k = 0; k < nc && !i.bad; ++k) {
        vec::GradientStop s;
        s.color = i.color();
        s.pos = i.f32v();
        s.mid = i.f32v();
        g.colors.push_back(s);
    }
    const uint32_t no = i.u32v();
    if (i.bad || no > (1u << 16)) { i.bad = true; return; }
    g.opacities.clear();
    for (uint32_t k = 0; k < no && !i.bad; ++k) {
        vec::OpacityStop s;
        s.opacity = i.f32v();
        s.pos = i.f32v();
        s.mid = i.f32v();
        g.opacities.push_back(s);
    }
}

void write_paint(Out& o, const vec::PaintStyle& s, ImagePool& pool) {
    o.u16(static_cast<uint16_t>(s.kind));
    o.color(s.color);
    write_gradient(o, s.gradient);
    o.i32(pool.add(s.pattern));
    o.f32(s.pattern_scale);
    o.f32(s.pattern_angle);
    o.i32(pool.add(s.texture));
    o.f32(s.texture_scale);
    o.f32(s.texture_angle);
    o.f32(s.texture_strength);
}

void read_paint(In& i, vec::PaintStyle& s, const std::vector<std::shared_ptr<const Image>>& images) {
    s.kind = static_cast<vec::PaintStyle::Kind>(i.u16v());
    s.color = i.color();
    read_gradient(i, s.gradient);
    auto pick = [&](int idx) -> std::shared_ptr<const Image> {
        return idx >= 0 && idx < static_cast<int>(images.size()) ? images[static_cast<size_t>(idx)] : nullptr;
    };
    s.pattern = pick(i.i32v());
    s.pattern_scale = i.f32v();
    s.pattern_angle = i.f32v();
    s.texture = pick(i.i32v());
    s.texture_scale = i.f32v();
    s.texture_angle = i.f32v();
    s.texture_strength = i.f32v();
}

void write_line(Out& o, const vec::LineStyle& l) {
    o.u32(l.first_cap);
    o.u32(l.last_cap);
    o.f32(l.first_w);
    o.f32(l.first_h);
    o.f32(l.last_w);
    o.f32(l.last_h);
    o.f32(l.miter);
    o.u32(static_cast<uint32_t>(l.dashes.size()));
    for (float d : l.dashes) o.f32(d);
    o.u32(l.seg_start_cap);
    o.f32(l.seg_start_w);
    o.f32(l.seg_start_h);
    o.u32(l.seg_end_cap);
    o.f32(l.seg_end_w);
    o.f32(l.seg_end_h);
    o.u32(l.flag_a);
    o.u32(l.flag_b);
    o.u32(l.seg_caps_on);
    o.str(l.name);
}

void read_line(In& i, vec::LineStyle& l) {
    l.first_cap = i.u32v();
    l.last_cap = i.u32v();
    l.first_w = i.f32v();
    l.first_h = i.f32v();
    l.last_w = i.f32v();
    l.last_h = i.f32v();
    l.miter = i.f32v();
    const uint32_t nd = i.u32v();
    if (i.bad || nd > (1u << 16)) { i.bad = true; return; }
    l.dashes.clear();
    for (uint32_t k = 0; k < nd && !i.bad; ++k) l.dashes.push_back(i.f32v());
    l.seg_start_cap = i.u32v();
    l.seg_start_w = i.f32v();
    l.seg_start_h = i.f32v();
    l.seg_end_cap = i.u32v();
    l.seg_end_w = i.f32v();
    l.seg_end_h = i.f32v();
    l.flag_a = i.u32v();
    l.flag_b = i.u32v();
    l.seg_caps_on = i.u32v();
    l.name = i.str();
}

void write_text(Out& o, const vec::TextInfo& t) {
    o.str(t.text);
    o.str(t.font_path);
    o.str(t.font_family);
    o.f32(t.size);
    o.i32(t.align);
    o.f32(t.rotation);
    o.u8(t.antialias ? 1 : 0);
    o.f32(t.x);
    o.f32(t.y);
    o.f32(t.baseline);
}

void read_text(In& i, vec::TextInfo& t) {
    t.text = i.str();
    t.font_path = i.str();
    t.font_family = i.str();
    t.size = i.f32v();
    t.align = i.i32v();
    t.rotation = i.f32v();
    t.antialias = i.u8v() != 0;
    t.x = i.f32v();
    t.y = i.f32v();
    t.baseline = i.f32v();
}

}  // namespace

std::vector<uint8_t> encode_objects(const std::vector<vec::Object>& objects) {
    ImagePool pool;
    Out body;
    body.u32(static_cast<uint32_t>(objects.size()));
    for (const vec::Object& o : objects) {
        body.str(o.name);
        body.u8((o.antialias ? 1u : 0u) | (o.visible ? 2u : 0u) | (o.is_text ? 4u : 0u) | (o.is_group ? 8u : 0u));
        body.u32(o.group_count);
        body.f32(o.stroke_width);
        body.f32(o.miter);
        body.u16(o.file_type);
        body.u32(o.file_a);
        body.u32(o.file_flags);
        body.u32(o.file_c);
        write_paint(body, o.stroke, pool);
        write_paint(body, o.fill, pool);
        write_line(body, o.line);
        write_text(body, o.text);
        body.u32(static_cast<uint32_t>(o.paths.size()));
        for (const vec::Path& p : o.paths) {
            body.u8(p.closed ? 1 : 0);
            body.u32(static_cast<uint32_t>(p.nodes.size()));
            for (const vec::Node& n : p.nodes) {
                body.f32(n.x); body.f32(n.y);
                body.f32(n.in_x); body.f32(n.in_y);
                body.f32(n.out_x); body.f32(n.out_y);
                body.u8(n.flags[0]); body.u8(n.flags[1]); body.u8(n.flags[2]);
            }
        }
        body.blob(o.attr_raw);
        body.blob(o.linestyle_raw);
    }

    // Header, then the shared images, then the objects that index them.
    Out out;
    out.u32(kMagic);
    out.u32(kVersion);
    out.u32(static_cast<uint32_t>(pool.order.size()));
    for (const Image* img : pool.order) out.blob(encode_png(*img));
    out.b.insert(out.b.end(), body.b.begin(), body.b.end());
    return out.b;
}

bool decode_objects(const uint8_t* data, size_t size, std::vector<vec::Object>& out) {
    In in{data, size};
    if (in.u32v() != kMagic) return false;
    if (in.u32v() != kVersion) return false;
    const uint32_t nimg = in.u32v();
    if (in.bad || nimg > 4096) return false;
    std::vector<std::shared_ptr<const Image>> images;
    images.reserve(nimg);
    for (uint32_t i = 0; i < nimg && !in.bad; ++i) {
        const std::vector<uint8_t> png = in.blob();
        std::optional<Image> img = load_memory(png.data(), png.size());
        images.push_back(img ? std::make_shared<const Image>(std::move(*img)) : nullptr);
    }
    const uint32_t nobj = in.u32v();
    if (in.bad || nobj > (1u << 20)) return false;
    std::vector<vec::Object> objects;
    objects.reserve(nobj);
    for (uint32_t i = 0; i < nobj && !in.bad; ++i) {
        vec::Object o;
        o.name = in.str();
        const uint32_t flags = in.u8v();
        o.antialias = (flags & 1) != 0;
        o.visible = (flags & 2) != 0;
        o.is_text = (flags & 4) != 0;
        o.is_group = (flags & 8) != 0;
        o.group_count = in.u32v();
        o.stroke_width = in.f32v();
        o.miter = in.f32v();
        o.file_type = static_cast<uint16_t>(in.u16v());
        o.file_a = in.u32v();
        o.file_flags = in.u32v();
        o.file_c = in.u32v();
        read_paint(in, o.stroke, images);
        read_paint(in, o.fill, images);
        read_line(in, o.line);
        read_text(in, o.text);
        const uint32_t np = in.u32v();
        if (in.bad || np > (1u << 20)) return false;
        for (uint32_t k = 0; k < np && !in.bad; ++k) {
            vec::Path p;
            p.closed = in.u8v() != 0;
            const uint32_t nn = in.u32v();
            if (in.bad || nn > (1u << 22)) return false;
            p.nodes.reserve(nn);
            for (uint32_t j = 0; j < nn && !in.bad; ++j) {
                vec::Node n;
                n.x = in.f32v(); n.y = in.f32v();
                n.in_x = in.f32v(); n.in_y = in.f32v();
                n.out_x = in.f32v(); n.out_y = in.f32v();
                n.flags[0] = static_cast<uint8_t>(in.u8v());
                n.flags[1] = static_cast<uint8_t>(in.u8v());
                n.flags[2] = static_cast<uint8_t>(in.u8v());
                p.nodes.push_back(n);
            }
            o.paths.push_back(std::move(p));
        }
        o.attr_raw = in.blob();
        o.linestyle_raw = in.blob();
        objects.push_back(std::move(o));
    }
    if (in.bad) return false;
    out = std::move(objects);
    return true;
}

}  // namespace firn::io
