// Vector object state shared by the tools, menus, and dialogs: materials,
// shape/gradient/line/pattern libraries, and the Objects menu operations.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <numeric>

#include "App.h"
#include "firn/io.h"
#include "firn/io_psp.h"

using namespace firn;

namespace {

Color float_color(const float* f) {
    auto c = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return {c(f[0]), c(f[1]), c(f[2]), c(f[3])};
}

// Library folders: Preferences extra dir, FIRN_<KIND>_DIRS, ~/.config/firn/<sub>, the backup.
std::vector<std::filesystem::path> library_dirs(const char* env, const char* sub, const char* backup_sub) {
    namespace fs = std::filesystem;
    std::vector<fs::path> dirs;
    if (const char* extra = std::getenv(env)) {
        std::string s = extra;
        size_t start = 0;
        while (start <= s.size()) {
            const size_t end = s.find(':', start);
            dirs.emplace_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    dirs.emplace_back(fs::path(Config::directory()) / sub);
#ifdef FIRN_SOURCE_DIR
    dirs.emplace_back(fs::path(FIRN_SOURCE_DIR) / "WindowsInstall" / backup_sub);
#endif
    return dirs;
}

template <class F>
void scan_files(const std::vector<std::filesystem::path>& dirs, std::initializer_list<const char*> exts, F&& fn) {
    namespace fs = std::filesystem;
    for (const fs::path& d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) continue;
        for (const auto& de : fs::recursive_directory_iterator(d, fs::directory_options::skip_permission_denied, ec)) {
            if (!de.is_regular_file(ec)) continue;
            std::string ext = de.path().extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bool ok = false;
            for (const char* e : exts) if (ext == e) ok = true;
            if (ok) fn(de.path());
        }
    }
}

// A selected top-level unit: an object, or a group with its members.
struct Unit {
    size_t begin, end;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool has_bounds = false;
};

std::vector<Unit> selected_units(const std::vector<vec::Object>& objs) {
    std::vector<Unit> units;
    for (size_t i = 0; i < objs.size();) {
        const size_t end = vec::group_end(objs, i);
        if (objs[i].selected) {
            Unit u{i, end};
            for (size_t k = i; k < end; ++k) {
                float a, b, c, d;
                if (!objs[k].is_group && vec::outline_bounds(objs[k], &a, &b, &c, &d)) {
                    if (!u.has_bounds) { u.x0 = a; u.y0 = b; u.x1 = c; u.y1 = d; u.has_bounds = true; }
                    u.x0 = std::min(u.x0, a); u.y0 = std::min(u.y0, b); u.x1 = std::max(u.x1, c); u.y1 = std::max(u.y1, d);
                }
            }
            units.push_back(u);
        }
        i = end;
    }
    return units;
}

void translate_unit(std::vector<vec::Object>& objs, const Unit& u, float dx, float dy) {
    for (size_t k = u.begin; k < u.end; ++k) objs[k].translate(dx, dy);
}

}  // namespace

// --- Materials ---------------------------------------------------------------

vec::PaintStyle App::material_style(bool foreground) const {
    const Material& m = foreground ? fg_material : bg_material;
    vec::PaintStyle st;
    if (m.transparent) return st;
    st.color = float_color(foreground ? fg_color : bg_color);
    st.kind = vec::PaintStyle::Kind::Solid;
    if (m.texture_on && m.texture && !m.texture->empty()) {
        st.texture = m.texture;
        st.texture_scale = m.texture_scale;
        st.texture_angle = m.texture_angle;
        st.texture_strength = m.texture_strength;
    }
    if (m.kind == 1) {
        st.kind = vec::PaintStyle::Kind::Gradient;
        st.gradient = m.gradient;
        if (m.gradient_index < 0) {
            // The default gradient runs from the foreground to the background color.
            st.gradient.name = "Foreground-Background";
            st.gradient.colors = {{float_color(fg_color), 0, 50}, {float_color(bg_color), 100, 50}};
            st.gradient.opacities = {{100, 0, 50}, {100, 100, 50}};
        }
        st.gradient.style = static_cast<vec::GradientStyle>(std::clamp(m.gradient_style, 0, 3));
        st.gradient.angle = m.gradient_angle;
        st.gradient.repeats = m.gradient_repeats;
        st.gradient.invert = m.gradient_invert;
    } else if (m.kind == 2 && m.pattern && !m.pattern->empty()) {
        st.kind = vec::PaintStyle::Kind::Pattern;
        st.pattern = m.pattern;
        st.pattern_scale = m.pattern_scale;
        st.pattern_angle = m.pattern_angle;
    }
    return st;
}

// --- Libraries ---------------------------------------------------------------

void App::ensure_shape_library() {
    if (shape_library_loaded) return;
    shape_library_loaded = true;
    scan_files(library_dirs("FIRN_SHAPE_DIRS", "shapes", "Preset Shapes"), {".pspshape"}, [&](const std::filesystem::path& p) {
        std::string err; std::vector<std::string> warnings;
        auto d = io::load_psp(p.string(), &err, &warnings);
        if (!d) return;
        for (size_t i = 0; i < d->layer_count(); ++i) {
            const Layer& L = d->layer(i);
            if (!L.is_vector() || L.objects.empty()) continue;
            shape_library.push_back({p.string(), p.stem().string(), L.objects, d->width(), d->height()});
        }
    });
    std::sort(shape_library.begin(), shape_library.end(), [](const ShapeEntry& a, const ShapeEntry& b) { return a.name < b.name; });
}

void App::ensure_gradients() {
    if (gradients_loaded) return;
    gradients_loaded = true;
    scan_files(library_dirs("FIRN_GRADIENT_DIRS", "gradients", "Gradients"), {".pspgradient", ".grd"}, [&](const std::filesystem::path& p) {
        for (vec::Gradient& g : io::load_gradients(p.string())) gradient_library.push_back(std::move(g));
    });
    std::sort(gradient_library.begin(), gradient_library.end(), [](const vec::Gradient& a, const vec::Gradient& b) { return a.name < b.name; });
}

void App::ensure_line_styles() {
    if (lines_loaded) return;
    lines_loaded = true;
    scan_files(library_dirs("FIRN_LINE_DIRS", "lines", "Styled Lines"), {".pspstyledline"}, [&](const std::filesystem::path& p) {
        if (auto l = io::load_styled_line(p.string())) line_library.push_back({p.string(), std::move(*l)});
    });
    std::sort(line_library.begin(), line_library.end(), [](const LineEntry& a, const LineEntry& b) { return a.line.name < b.line.name; });
}

void App::ensure_patterns() {
    if (patterns_loaded) return;
    patterns_loaded = true;
    scan_files(library_dirs("FIRN_PATTERN_DIRS", "patterns", "Patterns"), {".pspimage", ".png", ".jpg", ".jpeg", ".bmp"}, [&](const std::filesystem::path& p) {
        pattern_library.push_back({p.string(), p.stem().string()});
    });
    std::sort(pattern_library.begin(), pattern_library.end(), [](const PatternEntry& a, const PatternEntry& b) { return a.name < b.name; });
}

void App::ensure_frames() {
    if (frames_loaded) return;
    frames_loaded = true;
    scan_files(library_dirs("FIRN_FRAME_DIRS", "frames", "Picture Frames"), {".pspframe"}, [&](const std::filesystem::path& p) {
        frame_library.push_back({p.string(), p.stem().string()});
    });
    std::sort(frame_library.begin(), frame_library.end(), [](const FrameEntry& a, const FrameEntry& b) { return a.name < b.name; });
}

// --- Layers menu -----------------------------------------------------------------

void App::layer_new_vector() {
    if (!doc) return;
    int n = 1;
    for (size_t i = 0; i < doc->layer_count(); ++i) if (doc->layer(i).is_vector()) ++n;
    run(std::make_unique<AddVectorLayerCommand>("Vector " + std::to_string(n)));
}

void App::layer_convert_to_raster() {
    if (doc && active_layer() >= 0 && doc->layer(active_layer()).is_vector()) run(std::make_unique<ConvertToRasterCommand>(active_layer()));
}

// --- Object creation -----------------------------------------------------------

int App::vector_layer_for_edit(bool create) {
    if (!doc) return -1;
    const int a = active_layer();
    if (a >= 0 && doc->layer(a).is_vector()) return a;
    if (!create) return -1;
    int n = 1;
    for (size_t i = 0; i < doc->layer_count(); ++i) if (doc->layer(i).is_vector()) ++n;
    run(std::make_unique<AddVectorLayerCommand>("Vector " + std::to_string(n)));
    for (int i = static_cast<int>(doc->layer_count()) - 1; i >= 0; --i)
        if (doc->layer(i).is_vector() && doc->layer(i).name == "Vector " + std::to_string(n)) { doc->set_active_layer(i); return i; }
    return -1;
}

void App::apply_object_style(vec::Object& o, bool stroke, bool fill, ImGuiMouseButton button) const {
    // The right button swaps the materials, as in the original.
    const bool swap = button == ImGuiMouseButton_Right;
    o.stroke = stroke ? material_style(!swap) : vec::PaintStyle{};
    o.fill = fill ? material_style(swap) : vec::PaintStyle{};
    o.stroke_width = line_width;
    o.antialias = shape_antialias;
    if (line_index >= 0 && line_index < static_cast<int>(line_library.size())) o.line = line_library[line_index].line;
    else o.line = vec::LineStyle{};
}

std::vector<vec::Object> App::shape_objects(float x0, float y0, float x1, float y1) const {
    const float lx = std::min(x0, x1), rx = std::max(x0, x1), ty = std::min(y0, y1), by = std::max(y0, y1);
    const float cx = (lx + rx) * 0.5f, cy = (ty + by) * 0.5f, hx = (rx - lx) * 0.5f, hy = (by - ty) * 0.5f;
    std::vector<vec::Object> out;
    if (shape_library_index >= 0 && shape_library_index < static_cast<int>(shape_library.size())) {
        // Scale the library shape's outline bounds onto the drag rectangle.
        const ShapeEntry& e = shape_library[shape_library_index];
        float bx0 = 0, by0 = 0, bx1 = 1, by1 = 1;
        bool any = false;
        for (const vec::Object& o : e.objects) {
            float a, b, c, d;
            if (o.is_group || !vec::outline_bounds(o, &a, &b, &c, &d)) continue;
            if (!any) { bx0 = a; by0 = b; bx1 = c; by1 = d; any = true; }
            bx0 = std::min(bx0, a); by0 = std::min(by0, b); bx1 = std::max(bx1, c); by1 = std::max(by1, d);
        }
        const float sx = (rx - lx) / std::max(bx1 - bx0, 1.0f), sy = (by - ty) / std::max(by1 - by0, 1.0f);
        for (vec::Object o : e.objects) {
            o.transform(sx, 0, 0, sy, lx - bx0 * sx, ty - by0 * sy);
            o.name = e.name;
            out.push_back(std::move(o));
        }
        return out;
    }
    auto regular = [&](int sides, float inner, int points) {
        std::vector<std::pair<float, float>> pts;
        const int n = points > 0 ? points * 2 : sides;
        for (int i = 0; i < n; ++i) {
            const float a = -3.14159265f / 2 + 2 * 3.14159265f * i / n;
            const float r = (points > 0 && (i % 2)) ? inner : 1.0f;
            pts.emplace_back(cx + std::cos(a) * hx * r, cy + std::sin(a) * hy * r);
        }
        return vec::make_polygon(pts, true);
    };
    vec::Object o;
    switch (shape_kind) {
        case 1: o = vec::make_rounded_rectangle(lx, ty, rx, by, std::min(shape_radius, std::min(hx, hy))); break;
        case 2: o = vec::make_ellipse(cx, cy, hx, hy); break;
        case 3: o = regular(3, 0, 0); o.name = "Triangle"; break;
        case 4: o = regular(shape_sides, 0, 0); o.name = "Polygon"; break;
        case 5: o = regular(0, star_inner, star_points); o.name = "Star"; break;
        default: o = vec::make_rectangle(lx, ty, rx, by); break;
    }
    out.push_back(std::move(o));
    return out;
}

void App::add_vector_object(vec::Object o, const std::string& name) {
    const int layer = vector_layer_for_edit(true);
    if (layer < 0) return;
    Layer& L = doc->layer(layer);
    std::vector<vec::Object> before = L.objects;
    for (vec::Object& e : L.objects) e.selected = false;
    o.selected = true;
    L.objects.push_back(std::move(o));
    objects_changed(name.c_str(), std::move(before));
}

std::vector<size_t> App::selected_objects() const {
    std::vector<size_t> out;
    const int a = active_layer();
    if (!doc || a < 0 || !doc->layer(a).is_vector()) return out;
    const auto& objs = doc->layer(a).objects;
    for (size_t i = 0; i < objs.size(); ++i) if (objs[i].selected) out.push_back(i);
    return out;
}

void App::select_objects(const std::vector<size_t>& indices, bool add) {
    const int a = active_layer();
    if (!doc || a < 0 || !doc->layer(a).is_vector()) return;
    auto& objs = doc->layer(a).objects;
    if (!add) for (vec::Object& o : objs) o.selected = false;
    for (size_t i : indices) {
        if (i >= objs.size()) continue;
        // Selecting any member selects its outermost group as a whole.
        size_t top = i;
        for (int g = vec::group_of(objs, top); g >= 0; g = vec::group_of(objs, static_cast<size_t>(g))) top = static_cast<size_t>(g);
        for (size_t k = top; k < vec::group_end(objs, top); ++k) objs[k].selected = true;
    }
}

void App::objects_changed(const char* name, std::vector<vec::Object> before) {
    const int layer = vector_layer_for_edit(false);
    if (layer < 0) return;
    doc->rasterize_vector_layer(layer);
    commit(std::make_unique<VectorEditCommand>(layer, name, std::move(before), doc->layer(layer).objects));
}

// --- Objects menu -----------------------------------------------------------

void App::object_align(int how) {
    const int layer = vector_layer_for_edit(false);
    if (layer < 0) return;
    auto& objs = doc->layer(layer).objects;
    std::vector<Unit> units = selected_units(objs);
    units.erase(std::remove_if(units.begin(), units.end(), [](const Unit& u) { return !u.has_bounds; }), units.end());
    if (units.empty()) return;
    const bool canvas = how >= 6;
    if (units.size() < 2 && !canvas) return;
    float x0 = units[0].x0, y0 = units[0].y0, x1 = units[0].x1, y1 = units[0].y1;
    for (const Unit& u : units) { x0 = std::min(x0, u.x0); y0 = std::min(y0, u.y0); x1 = std::max(x1, u.x1); y1 = std::max(y1, u.y1); }
    if (canvas) { x0 = 0; y0 = 0; x1 = static_cast<float>(doc->width()); y1 = static_cast<float>(doc->height()); }
    std::vector<vec::Object> before = objs;
    static const char* names[] = {"Align Top", "Align Bottom", "Align Left", "Align Right", "Align Vertical Center", "Align Horizontal Center",
                                  "Center in Canvas", "Horizontal Center in Canvas", "Vertical Center in Canvas"};
    for (const Unit& u : units) {
        float dx = 0, dy = 0;
        switch (how) {
            case 0: dy = y0 - u.y0; break;
            case 1: dy = y1 - u.y1; break;
            case 2: dx = x0 - u.x0; break;
            case 3: dx = x1 - u.x1; break;
            case 4: dy = (y0 + y1) * 0.5f - (u.y0 + u.y1) * 0.5f; break;
            case 5: dx = (x0 + x1) * 0.5f - (u.x0 + u.x1) * 0.5f; break;
            case 6: dx = (x0 + x1) * 0.5f - (u.x0 + u.x1) * 0.5f; dy = (y0 + y1) * 0.5f - (u.y0 + u.y1) * 0.5f; break;
            case 7: dx = (x0 + x1) * 0.5f - (u.x0 + u.x1) * 0.5f; break;
            default: dy = (y0 + y1) * 0.5f - (u.y0 + u.y1) * 0.5f; break;
        }
        translate_unit(objs, u, dx, dy);
    }
    objects_changed(names[std::clamp(how, 0, 8)], std::move(before));
}

void App::object_distribute(int how) {
    const int layer = vector_layer_for_edit(false);
    if (layer < 0) return;
    auto& objs = doc->layer(layer).objects;
    std::vector<Unit> units = selected_units(objs);
    units.erase(std::remove_if(units.begin(), units.end(), [](const Unit& u) { return !u.has_bounds; }), units.end());
    if (units.size() < 3) return;
    const bool vertical = how <= 2 || how == 6;
    auto key = [&](const Unit& u) {
        switch (how) {
            case 0: case 3: return vertical ? u.y0 : u.x0;
            case 1: case 4: return vertical ? (u.y0 + u.y1) * 0.5f : (u.x0 + u.x1) * 0.5f;
            case 2: case 5: return vertical ? u.y1 : u.x1;
            default: return vertical ? u.y0 : u.x0;
        }
    };
    std::sort(units.begin(), units.end(), [&](const Unit& a, const Unit& b) { return key(a) < key(b); });
    std::vector<vec::Object> before = objs;
    static const char* names[] = {"Distribute Vertical Top", "Distribute Vertical Center", "Distribute Vertical Bottom", "Distribute Horizontal Left",
                                  "Distribute Horizontal Center", "Distribute Horizontal Right", "Space Evenly Vertically", "Space Evenly Horizontally"};
    if (how >= 6) {
        // Equal gaps between the first and last units' extents.
        float total = 0;
        for (const Unit& u : units) total += vertical ? u.y1 - u.y0 : u.x1 - u.x0;
        const float span = vertical ? units.back().y1 - units.front().y0 : units.back().x1 - units.front().x0;
        const float gap = (span - total) / static_cast<float>(units.size() - 1);
        float pos = vertical ? units.front().y0 : units.front().x0;
        for (const Unit& u : units) {
            const float size = vertical ? u.y1 - u.y0 : u.x1 - u.x0;
            const float d = pos - (vertical ? u.y0 : u.x0);
            translate_unit(objs, u, vertical ? 0 : d, vertical ? d : 0);
            pos += size + gap;
        }
    } else {
        const float first = key(units.front()), last = key(units.back());
        for (size_t i = 1; i + 1 < units.size(); ++i) {
            const float target = first + (last - first) * static_cast<float>(i) / static_cast<float>(units.size() - 1);
            const float d = target - key(units[i]);
            translate_unit(objs, units[i], vertical ? 0 : d, vertical ? d : 0);
        }
    }
    objects_changed(names[std::clamp(how, 0, 7)], std::move(before));
}

void App::object_same_size(int how) {
    const int layer = vector_layer_for_edit(false);
    if (layer < 0) return;
    auto& objs = doc->layer(layer).objects;
    std::vector<Unit> units = selected_units(objs);
    units.erase(std::remove_if(units.begin(), units.end(), [](const Unit& u) { return !u.has_bounds; }), units.end());
    if (units.size() < 2) return;
    // The first selected (lowest) unit is the reference, like the original's "last selected".
    const Unit ref = units.front();
    std::vector<vec::Object> before = objs;
    for (size_t i = 1; i < units.size(); ++i) {
        const Unit& u = units[i];
        const float sx = (how == 0) ? 1.0f : (ref.x1 - ref.x0) / std::max(u.x1 - u.x0, 1e-3f);
        const float sy = (how == 1) ? 1.0f : (ref.y1 - ref.y0) / std::max(u.y1 - u.y0, 1e-3f);
        for (size_t k = u.begin; k < u.end; ++k) objs[k].transform(sx, 0, 0, sy, u.x0 - u.x0 * sx, u.y0 - u.y0 * sy);
    }
    static const char* names[] = {"Make Same Height", "Make Same Width", "Make Same Size"};
    objects_changed(names[std::clamp(how, 0, 2)], std::move(before));
}

void App::object_arrange(int delta) {
    const int layer = vector_layer_for_edit(false);
    if (layer < 0 || delta == 0) return;
    auto& objs = doc->layer(layer).objects;
    // Work on top-level units; selected ones move past unselected neighbors.
    std::vector<Unit> units;
    for (size_t i = 0; i < objs.size(); i = vec::group_end(objs, i)) units.push_back({i, vec::group_end(objs, i)});
    std::vector<vec::Object> before = objs;
    std::vector<std::vector<vec::Object>> blocks;
    std::vector<bool> sel;
    for (const Unit& u : units) { blocks.emplace_back(objs.begin() + u.begin, objs.begin() + u.end); sel.push_back(objs[u.begin].selected); }
    const int n = static_cast<int>(blocks.size());
    if (delta >= n || delta <= -n) {
        // To top / bottom: stable partition keeping relative order.
        std::vector<std::vector<vec::Object>> a, b;
        for (int i = 0; i < n; ++i) (sel[i] ? a : b).push_back(std::move(blocks[i]));
        blocks.clear();
        if (delta > 0) { for (auto& x : b) blocks.push_back(std::move(x)); for (auto& x : a) blocks.push_back(std::move(x)); }
        else { for (auto& x : a) blocks.push_back(std::move(x)); for (auto& x : b) blocks.push_back(std::move(x)); }
    } else if (delta > 0) {
        for (int i = n - 2; i >= 0; --i) if (sel[i] && !sel[i + 1]) { std::swap(blocks[i], blocks[i + 1]); sel[i] = false; sel[i + 1] = true; }
    } else {
        for (int i = 1; i < n; ++i) if (sel[i] && !sel[i - 1]) { std::swap(blocks[i], blocks[i - 1]); sel[i] = false; sel[i - 1] = true; }
    }
    objs.clear();
    for (auto& blk : blocks) for (auto& o : blk) objs.push_back(std::move(o));
    const char* name = delta >= n ? "Bring to Top" : delta <= -n ? "Send to Bottom" : delta > 0 ? "Move Up" : "Move Down";
    objects_changed(name, std::move(before));
}

void App::object_group() {
    const int layer = vector_layer_for_edit(false);
    if (layer < 0) return;
    auto& objs = doc->layer(layer).objects;
    std::vector<Unit> units = selected_units(objs);
    if (units.size() < 2) return;
    std::vector<vec::Object> before = objs;
    std::vector<vec::Object> members, rest;
    size_t first = units.front().begin;
    size_t insert_at = 0;
    for (size_t i = 0; i < objs.size(); ++i) {
        const bool in = std::any_of(units.begin(), units.end(), [&](const Unit& u) { return i >= u.begin && i < u.end; });
        if (in) members.push_back(std::move(objs[i]));
        else { if (i < first) insert_at = rest.size() + 1; rest.push_back(std::move(objs[i])); }
    }
    vec::Object g;
    g.name = "Group";
    g.is_group = true;
    g.group_count = static_cast<uint32_t>(units.size());
    g.file_type = 5;
    g.selected = true;
    objs = rest;
    objs.insert(objs.begin() + static_cast<long>(insert_at), g);
    objs.insert(objs.begin() + static_cast<long>(insert_at) + 1, members.begin(), members.end());
    objects_changed("Group", std::move(before));
}

void App::object_ungroup() {
    const int layer = vector_layer_for_edit(false);
    if (layer < 0) return;
    auto& objs = doc->layer(layer).objects;
    std::vector<vec::Object> before = objs;
    bool any = false;
    for (size_t i = 0; i < objs.size();) {
        if (objs[i].is_group && objs[i].selected) { objs.erase(objs.begin() + static_cast<long>(i)); any = true; }
        else ++i;
    }
    if (any) objects_changed("Ungroup", std::move(before));
}

void App::object_delete() {
    const int layer = vector_layer_for_edit(false);
    if (layer < 0) return;
    auto& objs = doc->layer(layer).objects;
    if (selected_objects().empty()) return;
    std::vector<vec::Object> before = objs;
    objs.erase(std::remove_if(objs.begin(), objs.end(), [](const vec::Object& o) { return o.selected; }), objs.end());
    objects_changed("Delete Object", std::move(before));
}

void App::object_select_all() {
    const int layer = vector_layer_for_edit(false);
    if (layer < 0) return;
    for (vec::Object& o : doc->layer(layer).objects) o.selected = true;
}

void App::object_select_none() {
    const int layer = vector_layer_for_edit(false);
    if (layer < 0) return;
    for (vec::Object& o : doc->layer(layer).objects) o.selected = false;
}

void App::object_text_to_curves(bool per_character) {
    const int layer = vector_layer_for_edit(false);
    if (layer < 0) return;
    auto& objs = doc->layer(layer).objects;
    std::vector<vec::Object> before = objs;
    bool any = false;
    for (size_t i = 0; i < objs.size(); ++i) {
        vec::Object& o = objs[i];
        if (!o.is_text || !o.selected) continue;
        any = true;
        if (!per_character) { o.is_text = false; continue; }
        // Split into one object per glyph; the layout is rebuilt so the
        // contour-to-glyph map is known, then placed like the original object.
        std::vector<int> glyphs;
        vec::Object proto = o;
        proto.is_text = false;
        float bx0, by0, bx1, by1;
        vec::outline_bounds(o, &bx0, &by0, &bx1, &by1);
        std::vector<vec::Path> paths = text_paths(o.text, &glyphs);
        // Map the fresh layout onto the existing object's placement (rotation included).
        vec::Object fresh = proto;
        fresh.paths = paths;
        place_text_object(fresh, o.text, 0, 0);
        float fx0, fy0, fx1, fy1;
        vec::outline_bounds(fresh, &fx0, &fy0, &fx1, &fy1);
        std::vector<vec::Object> pieces;
        int cur = -1;
        for (size_t k = 0; k < fresh.paths.size(); ++k) {
            if (k >= glyphs.size() || glyphs[k] != cur) { cur = k < glyphs.size() ? glyphs[k] : -1; pieces.push_back(proto); pieces.back().paths.clear(); }
            pieces.back().paths.push_back(fresh.paths[k]);
        }
        for (size_t k = 0; k < pieces.size(); ++k) {
            pieces[k].translate(bx0 - fx0, by0 - fy0);
            pieces[k].name = o.name + " " + std::to_string(k + 1);
            pieces[k].selected = true;
        }
        objs.erase(objs.begin() + static_cast<long>(i));
        objs.insert(objs.begin() + static_cast<long>(i), pieces.begin(), pieces.end());
        i += pieces.size() - 1;
    }
    if (any) objects_changed(per_character ? "Convert Text to Curves (Characters)" : "Convert Text to Curves", std::move(before));
}

// --- Text objects ---------------------------------------------------------------

std::vector<vec::Path> App::text_paths(const vec::TextInfo& t, std::vector<int>* glyph_ids) const {
    std::shared_ptr<text::Font> font = text_font;
    if (!font || font->info().path != t.font_path) font = text::Font::load(t.font_path);
    if (!font) return {};
    return vec::text_outline_paths(t, *font, nullptr, glyph_ids);
}

void App::place_text_object(vec::Object& o, const vec::TextInfo& t, float x, float y) const {
    o.is_text = true;
    o.text = t;
    o.text.x = x; o.text.y = y;
    std::shared_ptr<text::Font> font = text_font;
    if (!font || font->info().path != t.font_path) font = text::Font::load(t.font_path);
    o.paths = font ? vec::text_outline_paths(o.text, *font, &o.text.baseline) : std::vector<vec::Path>{};
    o.translate(x, y);
    if (t.rotation != 0.0f) {
        float bx0, by0, bx1, by1;
        if (vec::outline_bounds(o, &bx0, &by0, &bx1, &by1)) {
            const float cx = (bx0 + bx1) * 0.5f, cy = (by0 + by1) * 0.5f;
            const float r = t.rotation * 3.14159265f / 180.0f, c = std::cos(r), s = std::sin(r);
            o.transform(c, -s, s, c, cx - (c * cx - s * cy), cy - (s * cx + c * cy));
        }
    }
}
