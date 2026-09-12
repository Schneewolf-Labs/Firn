// OpenRaster (.ora): a zip holding stack.xml, one PNG per layer, the merged
// image and a thumbnail. Firn's project format: what the native container
// cannot hold (filter layers, styles, masks, 16-bit layers, vector objects,
// adjustment layers, ICC profiles, saved selections) rides in "firn:"
// extension attributes and elements that other readers ignore.
#include "firn/io_psp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <fstream>
#include <iterator>

#include "firn/io.h"
#include "firn/json.h"
#include "firn/raster.h"
#include "firn/raster16.h"
#include "firn/zip.h"

namespace firn::io {

namespace {

// --- A small XML reader: elements and attributes only (text is skipped) ---

struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlNode> children;
    const std::string* attr(const char* key) const {
        for (const auto& a : attrs) if (a.first == key) return &a.second;
        return nullptr;
    }
    std::string attr_or(const char* key, const std::string& def) const { const std::string* v = attr(key); return v ? *v : def; }
    double number(const char* key, double def) const { const std::string* v = attr(key); return v ? std::atof(v->c_str()) : def; }
};

std::string unescape(std::string s) {
    static const std::pair<const char*, const char*> ents[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
    for (const auto& e : ents) {
        size_t p;
        while ((p = s.find(e.first)) != std::string::npos) s.replace(p, std::strlen(e.first), e.second);
    }
    return s;
}

std::string escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

bool parse_xml(const std::string& text, XmlNode& root, std::string* err) {
    std::vector<XmlNode*> stack;
    size_t i = 0;
    const size_t n = text.size();
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    while (i < n) {
        const size_t lt = text.find('<', i);
        if (lt == std::string::npos) break;
        i = lt + 1;
        if (text.compare(i, 3, "!--") == 0) { const size_t e = text.find("-->", i); if (e == std::string::npos) return fail("xml: unterminated comment"); i = e + 3; continue; }
        if (text[i] == '?' || text[i] == '!') { const size_t e = text.find('>', i); if (e == std::string::npos) return fail("xml: unterminated declaration"); i = e + 1; continue; }
        if (text[i] == '/') {
            const size_t e = text.find('>', i);
            if (e == std::string::npos || stack.empty()) return fail("xml: stray closing tag");
            stack.pop_back();
            i = e + 1;
            continue;
        }
        XmlNode node;
        while (i < n && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != '>' && text[i] != '/') node.name += text[i++];
        bool self_closing = false;
        while (i < n) {
            while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
            if (i >= n) return fail("xml: unterminated tag");
            if (text[i] == '/') { self_closing = true; ++i; continue; }
            if (text[i] == '>') { ++i; break; }
            std::string key;
            while (i < n && text[i] != '=' && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != '>' && text[i] != '/') key += text[i++];
            while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
            std::string value;
            if (i < n && text[i] == '=') {
                ++i;
                while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
                if (i >= n) return fail("xml: bad attribute");
                const char q = text[i];
                if (q == '"' || q == '\'') {
                    const size_t e = text.find(q, i + 1);
                    if (e == std::string::npos) return fail("xml: unterminated attribute");
                    value = unescape(text.substr(i + 1, e - i - 1));
                    i = e + 1;
                } else {
                    while (i < n && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != '>' && text[i] != '/') value += text[i++];
                }
            }
            if (!key.empty()) node.attrs.emplace_back(std::move(key), std::move(value));
        }
        XmlNode* parent = stack.empty() ? nullptr : stack.back();
        if (!parent) {
            root = std::move(node);
            if (!self_closing) stack.push_back(&root);
        } else {
            parent->children.push_back(std::move(node));
            if (!self_closing) stack.push_back(&parent->children.back());
        }
    }
    return !root.name.empty();
}

// --- Blend modes <-> SVG compositing operators ---

const char* svg_op(BlendMode m) {
    switch (m) {
        case BlendMode::Multiply: return "svg:multiply";
        case BlendMode::Screen: return "svg:screen";
        case BlendMode::Overlay: return "svg:overlay";
        case BlendMode::Darken: return "svg:darken";
        case BlendMode::Lighten: return "svg:lighten";
        case BlendMode::Dodge: return "svg:color-dodge";
        case BlendMode::Burn: return "svg:color-burn";
        case BlendMode::HardLight: return "svg:hard-light";
        case BlendMode::SoftLight: return "svg:soft-light";
        case BlendMode::Difference: return "svg:difference";
        case BlendMode::Exclusion: return "svg:exclusion";
        case BlendMode::Hue: return "svg:hue";
        case BlendMode::Saturation: return "svg:saturation";
        case BlendMode::Color: return "svg:color";
        case BlendMode::Luminance: return "svg:luminosity";
        default: return "svg:src-over";
    }
}

bool blend_from_svg(const std::string& op, BlendMode& out) {
    static const std::pair<const char*, BlendMode> table[] = {
        {"svg:src-over", BlendMode::Normal}, {"svg:multiply", BlendMode::Multiply}, {"svg:screen", BlendMode::Screen},
        {"svg:overlay", BlendMode::Overlay}, {"svg:darken", BlendMode::Darken}, {"svg:lighten", BlendMode::Lighten},
        {"svg:color-dodge", BlendMode::Dodge}, {"svg:color-burn", BlendMode::Burn}, {"svg:hard-light", BlendMode::HardLight},
        {"svg:soft-light", BlendMode::SoftLight}, {"svg:difference", BlendMode::Difference}, {"svg:exclusion", BlendMode::Exclusion},
        {"svg:hue", BlendMode::Hue}, {"svg:saturation", BlendMode::Saturation}, {"svg:color", BlendMode::Color},
        {"svg:luminosity", BlendMode::Luminance}};
    for (const auto& t : table) if (op == t.first) { out = t.second; return true; }
    return false;
}

bool blend_from_name(const std::string& name, BlendMode& out) {
    for (int i = 0; i < static_cast<int>(BlendMode::Count); ++i)
        if (name == blend_mode_name(static_cast<BlendMode>(i))) { out = static_cast<BlendMode>(i); return true; }
    return false;
}

// --- Writing ---

std::vector<uint8_t> mask_png(const Mask& m) {
    Image img(m.width(), m.height(), {0, 0, 0, 255});
    for (size_t i = 0; i < m.size(); ++i) { uint8_t* p = img.data() + i * 4; p[0] = p[1] = p[2] = m.data()[i]; }
    return encode_png(img);
}

Mask mask_from_image(const Image& img, int w, int h) {
    Mask m(w, h, 0);
    for (int y = 0; y < std::min(h, img.height()); ++y)
        for (int x = 0; x < std::min(w, img.width()); ++x) m.at(x, y) = img.get(x, y).r;
    return m;
}

std::string fmt(double v) { char b[32]; std::snprintf(b, sizeof(b), "%.4g", v); return b; }

struct OraWriter {
    explicit OraWriter(const Document& d) : doc(d) {}
    const Document& doc;
    std::vector<zip::Entry> entries;
    int counter = 0;
    std::string out;
    // Encoding a layer's PNG is the expensive part, so the entries are built
    // on their own threads while the stack XML that names them is written.
    struct Pending {
        std::string name;
        bool store;
        std::shared_future<std::vector<uint8_t>> data;
    };
    std::vector<Pending> pending;
    // A single opaque layer is its own composite: mergedimage.png and the
    // layer entry are then the same picture, encoded once.
    std::shared_future<std::vector<uint8_t>> merged;
    const Layer* merged_layer = nullptr;

    std::string add_file(const std::string& stem, const std::string& ext, std::function<std::vector<uint8_t>()> fn) {
        const std::string name = "data/" + stem + std::to_string(counter++) + "." + ext;
        // PNGs are deflated already; running the zip's deflate over them
        // costs a second on a big project and saves nothing.
        pending.push_back({name, ext == "png", std::async(std::launch::async, std::move(fn)).share()});
        return name;
    }
    std::string add_file(const std::string& stem, const std::string& ext, std::vector<uint8_t> bytes) {
        return add_file(stem, ext, [b = std::move(bytes)]() mutable { return std::move(b); });
    }
    // Reuses bytes another entry is already producing.
    std::string add_shared(const std::string& stem, const std::string& ext, std::shared_future<std::vector<uint8_t>> data) {
        const std::string name = "data/" + stem + std::to_string(counter++) + "." + ext;
        pending.push_back({name, ext == "png", std::move(data)});
        return name;
    }
    void collect() {
        for (Pending& p : pending) entries.push_back({p.name, p.data.get(), p.store});
        pending.clear();
    }

    void common_attrs(const Layer& L) {
        out += " name=\"" + escape(L.name) + "\"";
        out += " opacity=\"" + fmt(L.opacity) + "\"";
        out += std::string(" visibility=\"") + (L.visible ? "visible" : "hidden") + "\"";
        out += std::string(" composite-op=\"") + svg_op(L.blend) + "\"";
        out += std::string(" firn:blend=\"") + blend_mode_name(L.blend) + "\"";
        if (L.has_mask()) {
            const Layer* Lp = &L;
            out += " firn:mask=\"" + add_file("mask", "png", [Lp] { return mask_png(Lp->mask); }) + "\"";
            if (!L.mask_enabled) out += " firn:mask-enabled=\"0\"";
        }
        if (!L.expanded) out += " firn:expanded=\"0\"";
        if (L.clipped) out += " firn:clipped=\"1\"";
        if (L.pass_through) out += " firn:pass-through=\"1\"";
        if (!L.ranges.identity()) out += " firn:ranges=\"" + escape(json::dump(blend_ranges_json(L.ranges))) + "\"";
        if (L.style.any()) out += " firn:style=\"" + escape(json::dump(L.style.to_json())) + "\"";
    }

    void layer(const Layer& L) {
        out += "    <layer";
        common_attrs(L);
        std::string src;
        int ox = 0, oy = 0;
        if (L.is_adjustment() && L.adjustment.is_filter()) {
            const Adjustment& a = L.adjustment;
            json::Value f = json::Value::object();
            f.set("kind", json::Value::number(static_cast<int>(a.kind)));
            f.set("blur_radius", json::Value::number(a.blur_radius)); f.set("average_radius", json::Value::number(a.average_radius));
            f.set("unsharp_radius", json::Value::number(a.unsharp_radius)); f.set("unsharp_strength", json::Value::number(a.unsharp_strength)); f.set("unsharp_clipping", json::Value::number(a.unsharp_clipping));
            out += " firn:type=\"filter\" firn:filter=\"" + escape(json::dump(f)) + "\"";
            out += " firn:adjustment-full=\"" + escape(json::dump(a.to_json())) + "\"";
            src = add_file("layer", "png", encode_png(Image(1, 1, {0, 0, 0, 0})));
        } else if (L.is_adjustment()) {
            out += " firn:type=\"adjustment\" firn:adjustment=\"" + add_file("adjustment", "bin", adjustment_to_bytes(L.adjustment)) + "\"";
            out += " firn:adjustment-full=\"" + escape(json::dump(L.adjustment.to_json())) + "\"";
            src = add_file("layer", "png", encode_png(Image(1, 1, {0, 0, 0, 0})));
        } else {
            // firn:objects is Firn's own encoding and holds the whole model.
            // Projects written before it exists carry firn:vector instead, a
            // blob in the original's shape layout, which the reader still
            // accepts; writing both would nearly double the vector data for
            // no reader outside Firn.
            if (L.is_vector()) out += " firn:type=\"vector\" firn:objects=\"" + add_file("objects", "bin", encode_objects(L.objects)) + "\"";
            if (L.background) out += " firn:background=\"1\"";
            // Only the part of the layer that holds anything is stored, with
            // its offset, the way the other editors write it.
            raster::Rect box = raster::content_bounds(L.pixels).clipped(L.pixels.width(), L.pixels.height());
            if (box.empty()) box = {0, 0, 1, 1};
            ox = box.x0; oy = box.y0;
            const Layer* Lp = &L;
            if (Lp == merged_layer) {
                src = add_shared("layer", "png", merged);
            } else {
                src = add_file("layer", "png", [Lp, box] {
                    return Lp->is_deep() ? encode_png16(raster16::crop(*Lp->deep, box)) : encode_png(raster::crop(Lp->pixels, box));
                });
            }
        }
        out += " src=\"" + src + "\" x=\"" + std::to_string(ox) + "\" y=\"" + std::to_string(oy) + "\"/>\n";
    }

    // Emits layers [from, to) top first; groups become nested stacks.
    void range(size_t from, size_t to, int indent) {
        // Collect top-level items of this range (bottom to top), then emit reversed.
        std::vector<std::pair<size_t, size_t>> items;   // [start, end)
        size_t i = from;
        while (i < to) {
            const Layer& L = doc.layer(i);
            const size_t end = L.type == LayerType::Group ? std::min(doc.group_end(i), to) : i + 1;
            items.emplace_back(i, end);
            i = end;
        }
        const std::string pad(static_cast<size_t>(indent) * 2, ' ');
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            const Layer& L = doc.layer(it->first);
            if (L.type == LayerType::Group) {
                out += pad + "<stack";
                common_attrs(L);
                out += ">\n";
                range(it->first + 1, it->second, indent + 1);
                out += pad + "</stack>\n";
            } else {
                out.resize(out.size());
                std::string save = std::move(out); out.clear();
                layer(L);
                std::string body = std::move(out);
                out = std::move(save);
                // Re-indent the single layer line.
                out += pad + body.substr(4);
            }
        }
    }

    std::vector<uint8_t> build() {
        // The composite and its thumbnail start encoding first, so they run
        // while the layers do.
        const Image flat = doc.composite();
        const float scale = std::min(1.0f, 256.0f / std::max(1, std::max(flat.width(), flat.height())));
        const Image thumb = scale < 1.0f ? raster::resample(flat, std::max(1, static_cast<int>(flat.width() * scale)), std::max(1, static_cast<int>(flat.height() * scale)), raster::Filter::Bilinear) : flat;
        merged = std::async(std::launch::async, [&flat] { return encode_png(flat); }).share();
        auto thumb_job = std::async(std::launch::async, [&thumb] { return encode_png(thumb); });
        if (doc.layer_count() == 1) {
            const Layer& only = doc.layer(0);
            if (only.is_raster() && !only.is_deep() && only.pixels.width() == flat.width() && only.pixels.height() == flat.height() &&
                std::memcmp(only.pixels.data(), flat.data(), flat.size_bytes()) == 0)
                merged_layer = &only;
        }
        entries.push_back({"mimetype", std::vector<uint8_t>(std::begin("image/openraster"), std::end("image/openraster") - 1), true});
        out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        out += "<image version=\"0.0.3\" w=\"" + std::to_string(doc.width()) + "\" h=\"" + std::to_string(doc.height()) + "\" xres=\"72\" yres=\"72\" xmlns:firn=\"https://github.com/Schneewolf-Labs/Firn\">\n";
        out += "  <stack>\n";
        range(0, doc.layer_count(), 2);
        out += "  </stack>\n";
        for (float g : doc.guides_h()) out += "  <firn:guide axis=\"h\" pos=\"" + fmt(g) + "\"/>\n";
        for (float g : doc.guides_v()) out += "  <firn:guide axis=\"v\" pos=\"" + fmt(g) + "\"/>\n";
        for (const Assistant& a : doc.assistants()) {
            const char* kind = a.kind == Assistant::Kind::VanishingPoint ? "vanishing-point" : a.kind == Assistant::Kind::Parallel ? "parallel" : "ruler";
            out += std::string("  <firn:assistant kind=\"") + kind + "\" x0=\"" + fmt(a.x0) + "\" y0=\"" + fmt(a.y0) + "\" x1=\"" + fmt(a.x1) + "\" y1=\"" + fmt(a.y1) + "\"/>\n";
        }
        if (doc.active_layer() >= 0) out += "  <firn:active index=\"" + std::to_string(doc.active_layer()) + "\"/>\n";
        if (doc.has_selection() && doc.selection().any()) {
            const Mask* sel = &doc.selection();
            out += "  <firn:selection src=\"" + add_file("selection", "png", [sel] { return mask_png(*sel); }) + "\"/>\n";
        }
        if (!doc.icc().empty()) out += "  <firn:icc src=\"" + add_file("profile", "icc", doc.icc()) + "\"/>\n";
        if (const std::vector<uint8_t> tiff = meta::build_tiff(doc.metadata()); !tiff.empty())
            out += "  <firn:exif src=\"" + add_file("exif", "tif", tiff) + "\"/>\n";
        for (const meta::Entry& e : doc.metadata().entries)
            if (e.group == meta::Group::Text)
                out += "  <firn:text key=\"" + escape(e.key) + "\" value=\"" + escape(e.text()) + "\"/>\n";
        for (const Document::AlphaChannel& ch : doc.alpha_channels()) {
            const Mask* m = &ch.mask;
            out += "  <firn:channel name=\"" + escape(ch.name) + "\" src=\"" + add_file("channel", "png", [m] { return mask_png(*m); }) + "\"/>\n";
        }
        out += "</image>\n";
        entries.push_back({"stack.xml", std::vector<uint8_t>(out.begin(), out.end()), false});
        collect();   // the layer entries, in the order the XML names them
        entries.push_back({"mergedimage.png", merged.get(), true});
        entries.push_back({"Thumbnails/thumbnail.png", thumb_job.get(), true});
        return zip::write(entries);
    }
};

// --- Reading ---

struct OraReader {
    const zip::Archive& ar;
    Document& doc;
    std::vector<std::string>* warnings;

    std::optional<Image> png(const std::string& src, std::optional<Image16>* deep = nullptr) {
        const std::vector<uint8_t>* bytes = ar.find(src);
        if (!bytes) return std::nullopt;
        if (deep) *deep = load16_memory(bytes->data(), bytes->size());
        return load_memory(bytes->data(), bytes->size());
    }

    void common(Layer& L, const XmlNode& n) {
        L.name = n.attr_or("name", L.name);
        L.opacity = static_cast<float>(std::clamp(n.number("opacity", 1.0), 0.0, 1.0));
        L.visible = n.attr_or("visibility", "visible") != "hidden";
        BlendMode b = BlendMode::Normal;
        if (const std::string* fb = n.attr("firn:blend"); fb && blend_from_name(*fb, b)) L.blend = b;
        else if (const std::string* op = n.attr("composite-op")) {
            if (blend_from_svg(*op, b)) L.blend = b;
            else if (warnings) warnings->push_back("Layer \"" + L.name + "\": blend mode " + *op + " is not supported; using Normal");
        }
        if (const std::string* m = n.attr("firn:mask")) {
            if (auto img = png(*m)) L.mask = mask_from_image(*img, doc.width(), doc.height());
            L.mask_enabled = n.attr_or("firn:mask-enabled", "1") != "0";
        }
        L.expanded = n.attr_or("firn:expanded", "1") != "0";
        L.clipped = n.attr_or("firn:clipped", "0") == "1";
        L.pass_through = n.attr_or("firn:pass-through", "0") == "1";
        if (const std::string* br = n.attr("firn:ranges")) {
            json::Value v;
            if (json::parse(*br, v)) L.ranges = blend_ranges_from_json(v);
        }
        if (const std::string* st = n.attr("firn:style")) {
            json::Value v;
            if (json::parse(*st, v)) L.style = LayerStyle::from_json(v);
        }
    }

    void layer(const XmlNode& n, int depth) {
        Layer& L = doc.add_layer(n.attr_or("name", "Layer"));
        L.depth = depth;
        common(L, n);
        const std::string type = n.attr_or("firn:type", "");
        if (type == "filter" || type == "adjustment") {
            L.type = LayerType::Adjustment;
            L.pixels = Image();
            if (type == "filter") {
                json::Value f;
                if (json::parse(n.attr_or("firn:filter", "{}"), f)) {
                    Adjustment& a = L.adjustment;
                    a.kind = static_cast<Adjustment::Kind>(static_cast<int>(f.get("kind").as_number(100)));
                    a.blur_radius = static_cast<float>(f.get("blur_radius").as_number(a.blur_radius));
                    a.average_radius = static_cast<int>(f.get("average_radius").as_number(a.average_radius));
                    a.unsharp_radius = static_cast<float>(f.get("unsharp_radius").as_number(a.unsharp_radius));
                    a.unsharp_strength = static_cast<int>(f.get("unsharp_strength").as_number(a.unsharp_strength));
                    a.unsharp_clipping = static_cast<int>(f.get("unsharp_clipping").as_number(a.unsharp_clipping));
                }
            } else if (const std::vector<uint8_t>* bytes = ar.find(n.attr_or("firn:adjustment", ""))) {
                adjustment_from_bytes(bytes->data(), bytes->size(), L.adjustment);
            }
            // Written since the project format became lossless: every field,
            // including the ones the active kind does not use.
            if (const std::string* full = n.attr("firn:adjustment-full")) {
                json::Value v;
                if (json::parse(*full, v)) L.adjustment = Adjustment::from_json(v);
            }
            return;
        }
        L.pixels = Image(doc.width(), doc.height(), {0, 0, 0, 0});
        L.background = n.attr_or("firn:background", "0") == "1";
        if (type == "vector") {
            L.type = LayerType::Vector;
            if (!n.attr("firn:expanded")) L.expanded = false;
            const std::vector<uint8_t>* full = ar.find(n.attr_or("firn:objects", ""));
            if (!full || !decode_objects(full->data(), full->size(), L.objects))
                if (const std::vector<uint8_t>* bytes = ar.find(n.attr_or("firn:vector", "")))
                    vector_objects_from_bytes(bytes->data(), bytes->size(), L.objects);
            doc.rasterize_vector_layer(doc.layer_count() - 1);
            return;
        }
        std::optional<Image16> deep;
        std::optional<Image> img = png(n.attr_or("src", ""), &deep);
        if (!img) { if (warnings) warnings->push_back("Layer \"" + L.name + "\": missing image " + n.attr_or("src", "")); return; }
        const int ox = static_cast<int>(n.number("x", 0)), oy = static_cast<int>(n.number("y", 0));
        if (deep && ox == 0 && oy == 0 && deep->width() == doc.width() && deep->height() == doc.height()) { L.set_deep(std::move(*deep)); return; }

        for (int y = 0; y < img->height(); ++y) {
            const int dy = y + oy;
            if (dy < 0 || dy >= doc.height()) continue;
            for (int x = 0; x < img->width(); ++x) {
                const int dx = x + ox;
                if (dx < 0 || dx >= doc.width()) continue;
                L.pixels.set(dx, dy, img->get(x, y));
            }
        }
        if (deep) {
            // A 16-bit layer that needs placing: widen the placed pixels (the file's depth is kept).
            Image16 wide = to_image16(L.pixels);
            for (int y = 0; y < deep->height(); ++y) {
                const int dy = y + oy;
                if (dy < 0 || dy >= doc.height()) continue;
                for (int x = 0; x < deep->width(); ++x) {
                    const int dx = x + ox;
                    if (dx < 0 || dx >= doc.width()) continue;
                    std::memcpy(wide.data() + (static_cast<size_t>(dy) * wide.width() + dx) * 4, deep->data() + (static_cast<size_t>(y) * deep->width() + x) * 4, 8);
                }
            }
            L.set_deep(std::move(wide));
        }
    }

    // Children are listed top first; the stack is built bottom up.
    void stack(const XmlNode& n, int depth) {
        for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) {
            const XmlNode& c = *it;
            if (c.name == "layer") layer(c, depth);
            else if (c.name == "stack") {
                Layer& G = doc.add_layer(c.attr_or("name", "Group"));
                G.type = LayerType::Group;
                G.depth = depth;
                common(G, c);
                stack(c, depth + 1);
            }
        }
    }
};

}  // namespace

bool is_ora_extension(const std::string& path) {
    const auto dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == "ora";
}

std::optional<Image> load_ora_thumbnail(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    zip::Index index;
    if (!zip::open(bytes.data(), bytes.size(), index)) return std::nullopt;
    // The thumbnail first; the merged image is the fallback. Either way only
    // that one entry is inflated, so browsing a folder stays cheap.
    for (const char* name : {"Thumbnails/thumbnail.png", "mergedimage.png"}) {
        const zip::Index::Item* item = index.find(name);
        if (!item) continue;
        std::vector<uint8_t> data;
        if (!zip::extract(bytes.data(), bytes.size(), *item, data)) continue;
        if (auto img = load_memory(data.data(), data.size())) return img;
    }
    return std::nullopt;
}

std::vector<uint8_t> save_ora_to_memory(const Document& doc) {
    OraWriter w{doc};
    return w.build();
}

bool save_ora(const Document& doc, const std::string& path, std::string* err) {
    const std::vector<uint8_t> data = save_ora_to_memory(doc);
    std::ofstream f(path, std::ios::binary);
    if (!f || !f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
        if (err) *err = "cannot write " + path;
        return false;
    }
    return true;
}

std::unique_ptr<Document> load_ora_from_memory(const uint8_t* data, size_t size, std::string* err, std::vector<std::string>* warnings) {
    zip::Archive ar;
    if (!zip::read(data, size, ar, err)) return nullptr;
    const std::vector<uint8_t>* xml = ar.find("stack.xml");
    if (!xml) { if (err) *err = "not an OpenRaster file (no stack.xml)"; return nullptr; }
    XmlNode root;
    if (!parse_xml(std::string(xml->begin(), xml->end()), root, err) || root.name != "image") { if (err && err->empty()) *err = "bad stack.xml"; return nullptr; }
    const int w = static_cast<int>(root.number("w", 0)), h = static_cast<int>(root.number("h", 0));
    if (w <= 0 || h <= 0 || w > 65536 || h > 65536) { if (err) *err = "bad image size in stack.xml"; return nullptr; }
    auto doc = std::make_unique<Document>(w, h);
    OraReader rd{ar, *doc, warnings};
    int active = -1;
    for (const XmlNode& c : root.children) {
        if (c.name == "stack") rd.stack(c, 0);
        else if (c.name == "firn:icc") { if (const std::vector<uint8_t>* b = ar.find(c.attr_or("src", ""))) doc->set_icc(*b); }
        else if (c.name == "firn:exif") {
            if (const std::vector<uint8_t>* b = ar.find(c.attr_or("src", ""))) {
                meta::Metadata md = meta::parse_tiff(b->data(), b->size());
                for (meta::Entry& e : md.entries) doc->metadata().entries.push_back(std::move(e));
                doc->metadata().sort();
            }
        }
        else if (c.name == "firn:text") { doc->metadata().set_text(c.attr_or("key", ""), c.attr_or("value", "")); }
        else if (c.name == "firn:active") { active = static_cast<int>(c.number("index", -1)); }
        else if (c.name == "firn:selection") {
            if (const std::vector<uint8_t>* b = ar.find(c.attr_or("src", ""))) {
                if (auto img = load_memory(b->data(), b->size()))
                    doc->set_selection(mask_from_image(*img, doc->width(), doc->height()));
            }
        }
        else if (c.name == "firn:guide") {
            const float pos = static_cast<float>(c.number("pos", 0));
            (c.attr_or("axis", "h") == "v" ? doc->guides_v() : doc->guides_h()).push_back(pos);
        }
        else if (c.name == "firn:assistant") {
            Assistant a;
            const std::string kind = c.attr_or("kind", "vanishing-point");
            a.kind = kind == "parallel" ? Assistant::Kind::Parallel : kind == "ruler" ? Assistant::Kind::Ruler : Assistant::Kind::VanishingPoint;
            a.x0 = static_cast<float>(c.number("x0", 0)); a.y0 = static_cast<float>(c.number("y0", 0));
            a.x1 = static_cast<float>(c.number("x1", 0)); a.y1 = static_cast<float>(c.number("y1", 0));
            doc->assistants().push_back(a);
        }
        else if (c.name == "firn:channel") { if (auto img = rd.png(c.attr_or("src", ""))) doc->alpha_channels().push_back({c.attr_or("name", "Selection"), mask_from_image(*img, w, h)}); }
    }
    if (doc->layer_count() == 0) {
        // No layers we could read: fall back to the merged image.
        if (auto merged = rd.png("mergedimage.png")) {
            Layer& bg = doc->add_layer("Background");
            bg.background = true;
            bg.pixels = Image(w, h, {255, 255, 255, 255});
            for (int y = 0; y < std::min(h, merged->height()); ++y) for (int x = 0; x < std::min(w, merged->width()); ++x) bg.pixels.set(x, y, merged->get(x, y));
            if (warnings) warnings->push_back("No layers found; loaded the merged image instead");
        } else { if (err) *err = "no readable layers"; return nullptr; }
    }
    doc->set_active_layer(active >= 0 && active < static_cast<int>(doc->layer_count())
                              ? active
                              : static_cast<int>(doc->layer_count()) - 1);
    doc->touch();
    return doc;
}

std::unique_ptr<Document> load_ora(const std::string& path, std::string* err, std::vector<std::string>* warnings) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot open " + path; return nullptr; }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return load_ora_from_memory(bytes.data(), bytes.size(), err, warnings);
}

}  // namespace firn::io
