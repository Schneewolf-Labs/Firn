// Minimal assert-based tests; no framework dependency yet.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdlib>
#include <memory>
#include <string>

#include "firn/adjust.h"
#include "firn/effects.h"
#include "firn/json.h"
#include "firn/print.h"
#include "firn/commands.h"
#include <thread>

#include "firn/document.h"
#include "firn/generate.h"
#include "firn/icc.h"
#include "firn/io.h"
#include "firn/inpaint.h"
#include "firn/io_psp.h"
#include "firn/zip.h"
#include "firn/mask.h"
#include "firn/raster.h"
#include "firn/raster16.h"
#include "firn/text.h"
#include "firn/photo.h"
#include "firn/vector.h"

// Scratch files go to the platform's temp directory (no /tmp on Windows).
static std::string tmp_path(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

using namespace firn;

static void test_invert_undo_redo() {
    Document doc(4, 4);
    doc.add_layer("Background").pixels.fill({10, 20, 30, 255});
    CommandStack hist;

    hist.run(doc, std::make_unique<InvertCommand>(0));
    Color c = doc.layer(0).pixels.get(1, 1);
    CHECK(c.r == 245 && c.g == 235 && c.b == 225 && c.a == 255);

    hist.undo(doc);
    c = doc.layer(0).pixels.get(1, 1);
    CHECK(c.r == 10 && c.g == 20 && c.b == 30);
    CHECK(!hist.can_undo() && hist.can_redo());

    hist.redo(doc);
    c = doc.layer(0).pixels.get(3, 3);
    CHECK(c.r == 245);
}

static void test_layer_add_remove() {
    Document doc(2, 2);
    doc.add_layer("Background");
    CommandStack hist;
    hist.run(doc, std::make_unique<AddLayerCommand>("Raster 1"));
    CHECK(doc.layer_count() == 2);
    CHECK(doc.layer(1).name == "Raster 1");
    hist.run(doc, std::make_unique<RemoveLayerCommand>(0));
    CHECK(doc.layer_count() == 1 && doc.layer(0).name == "Raster 1");
    hist.undo(doc);
    CHECK(doc.layer_count() == 2 && doc.layer(0).name == "Background");
    hist.undo(doc);
    CHECK(doc.layer_count() == 1);
}

static void test_composite_over() {
    Document doc(1, 1);
    doc.add_layer("bg").pixels.fill({255, 255, 255, 255});
    Layer& top = doc.add_layer("top");
    top.pixels.fill({0, 0, 0, 255});
    top.opacity = 0.5f;
    Color c = doc.composite().get(0, 0);
    CHECK(c.a == 255);
    CHECK(c.r >= 127 && c.r <= 128);
    top.visible = false;
    CHECK(doc.composite().get(0, 0).r == 255);
}

static void test_png_roundtrip() {
    Image img(3, 2);
    img.set(0, 0, {1, 2, 3, 4});
    img.set(2, 1, {200, 100, 50, 255});
    const char* path = "test_roundtrip.png";
    CHECK(io::save_png(img, path));
    auto back = io::load(path);
    CHECK(back.has_value());
    CHECK(back->width() == 3 && back->height() == 2);
    Color c = back->get(2, 1);
    CHECK(c.r == 200 && c.g == 100 && c.b == 50 && c.a == 255);
    std::remove(path);
}

static void test_stroke_opacity_does_not_build_up() {
    Image base(32, 32, {255, 255, 255, 255});
    raster::Brush b;
    b.size = 10;
    b.hardness = 1.0f;
    b.opacity = 0.5f;
    b.step = 0.1f;
    raster::Stroke s(base, b, {0, 0, 0, 255}, raster::StrokeMode::Paint);
    Image out = base;
    s.add_point(8, 16);
    s.add_point(24, 16);  // many overlapping stamps across the center
    s.add_point(8, 16);   // and back over the same pixels
    raster::Rect r = s.render(out);
    CHECK(!r.empty());
    Color c = out.get(16, 16);
    CHECK(c.r >= 127 && c.r <= 128);  // exactly one 50% application
    CHECK(out.get(0, 0).r == 255);     // untouched
    CHECK(out.get(16, 2).r == 255);    // outside the 5px radius
}

static void test_stroke_erase() {
    Image base(8, 8, {10, 20, 30, 255});
    raster::Brush b;
    b.size = 100;
    b.hardness = 1.0f;
    raster::Stroke s(base, b, {0, 0, 0, 255}, raster::StrokeMode::Erase);
    Image out = base;
    s.add_point(4, 4);
    s.render(out);
    Color c = out.get(4, 4);
    CHECK(c.a == 0 && c.r == 10);  // color kept, alpha cleared
}

static void test_flood_fill() {
    Image img(6, 6, {255, 255, 255, 255});
    for (int i = 0; i < 6; ++i) img.set(3, i, {0, 0, 0, 255});  // vertical wall at x=3
    raster::Rect r = raster::flood_fill(img, 0, 0, {255, 0, 0, 255}, 0);
    CHECK(r.x0 == 0 && r.x1 == 3 && r.y0 == 0 && r.y1 == 6);
    CHECK(img.get(2, 5).r == 255 && img.get(2, 5).g == 0);
    CHECK(img.get(4, 0).g == 255);  // other side of the wall untouched
    CHECK(img.get(3, 0).r == 0);    // wall untouched
    // Tolerance lets it cross a near-match.
    Image img2(4, 1, {100, 100, 100, 255});
    img2.set(2, 0, {110, 100, 100, 255});
    raster::flood_fill(img2, 0, 0, {0, 0, 0, 255}, 5);
    CHECK(img2.get(1, 0).r == 0 && img2.get(2, 0).r == 110);
    raster::flood_fill(img2, 3, 0, {0, 0, 0, 255}, 10);
    CHECK(img2.get(2, 0).r == 0);
}

static void test_adjustments() {
    Image img(2, 1);
    img.set(0, 0, {255, 0, 0, 255});
    img.set(1, 0, {0, 0, 255, 255});
    raster::grayscale(img);
    CHECK(img.get(0, 0).r == 76 && img.get(0, 0).g == 76);
    CHECK(img.get(1, 0).r == 29);

    Image bc(1, 1, {100, 100, 100, 255});
    raster::brightness_contrast(bc, 50, 0);
    CHECK(bc.get(0, 0).r == 150);
    raster::brightness_contrast(bc, 0, -100);  // full negative contrast collapses to gray
    CHECK(bc.get(0, 0).r == 128);

    // Gaussian blur of a solid image is a no-op and preserves alpha.
    Image solid(8, 8, {40, 80, 120, 255});
    raster::gaussian_blur(solid, 2.0f);
    Color c = solid.get(4, 4);
    CHECK(c.r == 40 && c.g == 80 && c.b == 120 && c.a == 255);
    // A transparent neighbor must not bleed black into an opaque pixel's color.
    Image edge(8, 1, {0, 0, 0, 0});
    edge.set(3, 0, {200, 200, 200, 255});
    raster::gaussian_blur(edge, 1.0f);
    CHECK(edge.get(3, 0).r == 200 && edge.get(3, 0).a < 255);
}

static void test_flip_mirror_snapshot() {
    Document doc(2, 2);
    Layer& L = doc.add_layer("bg");
    L.pixels.set(0, 0, {1, 0, 0, 255});
    L.pixels.set(1, 1, {2, 0, 0, 255});
    CommandStack hist;
    hist.run(doc, std::make_unique<FlipCommand>());
    CHECK(doc.layer(0).pixels.get(0, 1).r == 1);
    hist.run(doc, std::make_unique<MirrorCommand>());
    CHECK(doc.layer(0).pixels.get(1, 1).r == 1 && doc.layer(0).pixels.get(0, 0).r == 2);
    hist.undo(doc);
    hist.undo(doc);
    CHECK(doc.layer(0).pixels.get(0, 0).r == 1);

    // Interactive edit: apply live, then record.
    Image before = doc.layer(0).pixels;
    doc.layer(0).pixels.fill({9, 9, 9, 9});
    hist.push_applied(std::make_unique<LayerSnapshotCommand>(0, "Paint Brush", before, doc.layer(0).pixels));
    CHECK(hist.size() == 1 && hist.cursor() == 1);  // redo branch discarded
    hist.undo(doc);
    CHECK(doc.layer(0).pixels.get(0, 0).r == 1);
    hist.redo(doc);
    CHECK(doc.layer(0).pixels.get(0, 0).r == 9);
}

static void test_mask_shapes() {
    Mask r = mask::rectangle(10, 10, 2, 3, 6, 8, false);
    CHECK(r.at(2, 3) == 255 && r.at(5, 7) == 255 && r.at(6, 7) == 0 && r.at(5, 8) == 0);
    raster::Rect b = r.bounds();
    CHECK(b.x0 == 2 && b.y0 == 3 && b.x1 == 6 && b.y1 == 8);
    // Antialiased half-pixel edge gives half coverage.
    Mask ra = mask::rectangle(10, 10, 2.5f, 3, 6, 8, true);
    CHECK(ra.at(2, 3) >= 127 && ra.at(2, 3) <= 128 && ra.at(3, 3) == 255);

    Mask e = mask::ellipse(20, 20, 10, 10, 6, 4, true);
    CHECK(e.at(10, 10) == 255 && e.at(15, 10) > 0 && e.at(17, 10) == 0 && e.at(10, 15) == 0);

    Mask p = mask::polygon(10, 10, {{1.0f, 1.0f}, {8.0f, 1.0f}, {8.0f, 8.0f}, {1.0f, 8.0f}}, false);  // a square
    CHECK(p.at(1, 1) == 255 && p.at(7, 7) == 255 && p.at(8, 8) == 0 && p.at(0, 4) == 0);
    Mask tri = mask::polygon(10, 10, {{0.0f, 0.0f}, {10.0f, 0.0f}, {0.0f, 10.0f}}, true);
    CHECK(tri.at(1, 1) == 255 && tri.at(8, 8) == 0);
    CHECK(tri.at(5, 4) > 0 && tri.at(5, 4) < 255);  // on the diagonal edge
}

static void test_mask_ops() {
    Mask a = mask::rectangle(8, 8, 0, 0, 4, 8, false);
    Mask b = mask::rectangle(8, 8, 2, 0, 6, 8, false);
    Mask u = a; mask::combine(u, b, mask::Combine::Add);
    CHECK(u.at(0, 0) == 255 && u.at(5, 0) == 255 && u.at(6, 0) == 0);
    Mask d = a; mask::combine(d, b, mask::Combine::Subtract);
    CHECK(d.at(1, 0) == 255 && d.at(2, 0) == 0);
    Mask i = a; mask::combine(i, b, mask::Combine::Intersect);
    CHECK(i.at(1, 0) == 0 && i.at(3, 0) == 255 && i.at(4, 0) == 0);
    mask::invert(i);
    CHECK(i.at(1, 0) == 255 && i.at(3, 0) == 0);

    Mask sq = mask::rectangle(12, 12, 4, 4, 8, 8, false);
    Mask ex = sq; mask::expand(ex, 2);
    CHECK(ex.at(2, 5) == 255 && ex.at(1, 5) == 0 && ex.at(2, 2) == 0);  // circular, so no corner
    Mask co = sq; mask::contract(co, 1);
    CHECK(co.at(4, 4) == 0 && co.at(5, 5) == 255 && co.at(6, 6) == 255 && co.at(7, 7) == 0);
    Mask all(12, 12, 255); mask::contract(all, 2);
    CHECK(all.at(0, 5) == 0 && all.at(1, 5) == 0 && all.at(2, 5) == 255);  // shrinks from the image edge
    Mask f = sq; mask::feather(f, 2.0f);
    CHECK(f.at(6, 6) > 200 && f.at(3, 6) > 0 && f.at(3, 6) < 255 && f.at(0, 6) == 0);

    Image img(6, 1, {0, 0, 0, 255});
    img.set(2, 0, {255, 255, 255, 255});
    img.set(5, 0, {0, 0, 0, 255});
    Mask w = mask::magic_wand(img, 0, 0, 0, true);
    CHECK(w.at(1, 0) == 255 && w.at(2, 0) == 0 && w.at(3, 0) == 0);  // blocked by the white pixel
    Mask wg = mask::magic_wand(img, 0, 0, 0, false);
    CHECK(wg.at(3, 0) == 255 && wg.at(5, 0) == 255 && wg.at(2, 0) == 0);
}

static void test_selection_clips_commands() {
    Document doc(4, 1);
    doc.add_layer("bg").pixels.fill({100, 100, 100, 255});
    CommandStack hist;
    hist.run(doc, std::make_unique<SelectionCommand>("Selection", mask::rectangle(4, 1, 1, 0, 3, 1, false)));
    CHECK(doc.has_selection());
    hist.run(doc, std::make_unique<InvertCommand>(0));
    CHECK(doc.layer(0).pixels.get(0, 0).r == 100 && doc.layer(0).pixels.get(1, 0).r == 155 &&
          doc.layer(0).pixels.get(2, 0).r == 155 && doc.layer(0).pixels.get(3, 0).r == 100);
    hist.run(doc, std::make_unique<ClearCommand>(0, Color{0, 0, 0, 0}));
    CHECK(doc.layer(0).pixels.get(1, 0).a == 0 && doc.layer(0).pixels.get(0, 0).a == 255);
    hist.undo(doc); hist.undo(doc); hist.undo(doc);
    CHECK(!doc.has_selection() && doc.layer(0).pixels.get(1, 0).r == 100);

    // Brush and fill clip too.
    Mask sel = mask::rectangle(4, 1, 0, 0, 2, 1, false);
    Image base(4, 1, {0, 0, 0, 255});
    raster::Brush b; b.size = 100; b.hardness = 1;
    raster::Stroke st(base, b, {255, 255, 255, 255}, raster::StrokeMode::Paint, &sel);
    Image out = base;
    st.add_point(2, 0);
    st.render(out);
    CHECK(out.get(1, 0).r == 255 && out.get(2, 0).r == 0);
    Image ff(4, 1, {0, 0, 0, 255});
    raster::flood_fill(ff, 0, 0, {255, 0, 0, 255}, 0, 1.0f, &sel);
    CHECK(ff.get(1, 0).r == 255 && ff.get(2, 0).r == 0);
    CHECK(raster::flood_fill(ff, 3, 0, {255, 0, 0, 255}, 0, 1.0f, &sel).empty());  // seed outside selection

    // Paste as new layer goes above the active layer and undoes cleanly.
    doc.set_active_layer(0);
    hist.run(doc, std::make_unique<PasteLayerCommand>("Raster 1", Image(4, 1, {9, 9, 9, 255})));
    CHECK(doc.layer_count() == 2 && doc.layer(1).name == "Raster 1" && doc.active_layer() == 1);
    hist.undo(doc);
    CHECK(doc.layer_count() == 1 && doc.active_layer() == 0);
}

static void test_save_formats() {
    Image img(4, 4, {200, 100, 50, 255});
    img.set(0, 0, {0, 0, 0, 0});  // transparent pixel -> white in jpg/bmp
    for (const char* name : {"t.jpg", "t.bmp", "t.tga"}) {
        CHECK(io::save(img, name));
        auto back = io::load(name);
        CHECK(back && back->width() == 4);
        std::remove(name);
    }
    CHECK(io::save(img, "t.bmp"));
    auto bmp = io::load("t.bmp");
    Color c = bmp->get(0, 0);
    CHECK(c.r == 255 && c.g == 255 && c.b == 255);
    CHECK(bmp->get(1, 1).r == 200);
    std::remove("t.bmp");
    std::string err;
    CHECK(!io::save(img, "t.xyz", &err) && !err.empty());
}

static void test_blend_modes() {
    auto px = [](BlendMode m, Color bottom, Color top, float op = 1.0f) {
        Document doc(1, 1);
        doc.add_layer("b").pixels.fill(bottom);
        Layer& t = doc.add_layer("t");
        t.pixels.fill(top);
        t.blend = m;
        t.opacity = op;
        return doc.composite().get(0, 0);
    };
    CHECK(px(BlendMode::Multiply, {128, 128, 128, 255}, {128, 128, 128, 255}).r == 64);
    CHECK(px(BlendMode::Screen, {128, 128, 128, 255}, {128, 128, 128, 255}).r == 192);
    CHECK(px(BlendMode::Darken, {50, 200, 0, 255}, {100, 100, 100, 255}).g == 100);
    CHECK(px(BlendMode::Lighten, {50, 200, 0, 255}, {100, 100, 100, 255}).r == 100);
    CHECK(px(BlendMode::Difference, {200, 0, 0, 255}, {50, 0, 0, 255}).r == 150);
    CHECK(px(BlendMode::Exclusion, {255, 0, 0, 255}, {255, 0, 0, 255}).r == 0);
    CHECK(px(BlendMode::Overlay, {0, 0, 0, 255}, {255, 255, 255, 255}).r == 0);
    CHECK(px(BlendMode::HardLight, {0, 0, 0, 255}, {255, 255, 255, 255}).r == 255);
    CHECK(px(BlendMode::Dodge, {128, 128, 128, 255}, {255, 255, 255, 255}).r == 255);
    CHECK(px(BlendMode::Burn, {128, 128, 128, 255}, {0, 0, 0, 255}).r == 0);
    // Luminance keeps the bottom's hue: a gray top over pure red gives a red-ish result.
    Color l = px(BlendMode::Luminance, {255, 0, 0, 255}, {128, 128, 128, 255});
    CHECK(l.r > l.g && l.g == l.b);
    // Hue of a gray source is undefined (zero saturation) -> result is gray.
    Color h = px(BlendMode::Hue, {255, 0, 0, 255}, {128, 128, 128, 255});
    CHECK(h.r == h.g && h.g == h.b);
    // Color: takes the top's hue/sat with the bottom's luminance.
    Color c = px(BlendMode::Color, {128, 128, 128, 255}, {255, 0, 0, 255});
    CHECK(c.r > c.g && c.g == c.b);
    // Opacity and transparent destination.
    CHECK(px(BlendMode::Multiply, {0, 0, 0, 0}, {100, 100, 100, 255}).r == 100);  // over nothing = source
    Color half = px(BlendMode::Normal, {0, 0, 0, 255}, {255, 255, 255, 255}, 0.5f);
    CHECK(half.r >= 127 && half.r <= 128);
    // Dissolve at 50%: about half the pixels of a 64x64 layer show through.
    Document doc(64, 64);
    doc.add_layer("b").pixels.fill({0, 0, 0, 255});
    Layer& t = doc.add_layer("t");
    t.pixels.fill({255, 255, 255, 128});
    t.blend = BlendMode::Dissolve;
    Image out = doc.composite();
    int white = 0;
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) white += out.get(x, y).r == 255;
    CHECK(white > 1600 && white < 2500);
    CHECK(std::string(blend_mode_name(BlendMode::SoftLight)) == "Soft Light");
}

static void test_layer_structure_commands() {
    Document doc(2, 1);
    Layer& bg = doc.add_layer("Background");
    bg.background = true;
    bg.pixels.fill({255, 255, 255, 255});
    Layer& r1 = doc.add_layer("Raster 1");
    r1.pixels.set(0, 0, {0, 0, 0, 255});
    Layer& r2 = doc.add_layer("Raster 2");
    r2.pixels.set(1, 0, {255, 0, 0, 128});
    r2.visible = false;
    CommandStack hist;

    hist.run(doc, std::make_unique<LayerPropertiesCommand>(1, doc.props(1), LayerProps{"Renamed", true, 0.5f, BlendMode::Multiply, false, false, false, {}}));
    CHECK(doc.layer(1).name == "Renamed" && doc.layer(1).opacity == 0.5f && doc.layer(1).blend == BlendMode::Multiply);
    hist.undo(doc);
    CHECK(doc.layer(1).name == "Raster 1" && doc.layer(1).opacity == 1.0f);

    hist.run(doc, std::make_unique<DuplicateLayerCommand>(1));
    CHECK(doc.layer_count() == 4 && doc.layer(2).name == "Copy of Raster 1" && doc.active_layer() == 2);
    CHECK(doc.layer(2).pixels.get(0, 0).r == 0);
    hist.undo(doc);
    CHECK(doc.layer_count() == 3 && doc.active_layer() == 1);

    hist.run(doc, std::make_unique<ArrangeLayerCommand>(1, 2));
    CHECK(doc.layer(2).name == "Raster 1" && doc.layer(1).name == "Raster 2");
    hist.undo(doc);
    CHECK(doc.layer(1).name == "Raster 1");

    hist.run(doc, std::make_unique<MergeLayersCommand>(MergeLayersCommand::Kind::Down, 1));
    CHECK(doc.layer_count() == 2 && doc.layer(0).name == "Background" && doc.layer(0).background);
    CHECK(doc.layer(0).pixels.get(0, 0).r == 0 && doc.layer(0).pixels.get(1, 0).r == 255);
    hist.undo(doc);
    CHECK(doc.layer_count() == 3 && doc.layer(0).pixels.get(0, 0).r == 255);

    hist.run(doc, std::make_unique<MergeLayersCommand>(MergeLayersCommand::Kind::Visible));
    CHECK(doc.layer_count() == 2 && doc.layer(0).name == "Merged" && doc.layer(1).name == "Raster 2" && !doc.layer(1).visible);
    hist.undo(doc);
    CHECK(doc.layer_count() == 3);

    doc.layer(2).visible = true;
    hist.run(doc, std::make_unique<MergeLayersCommand>(MergeLayersCommand::Kind::All));
    CHECK(doc.layer_count() == 1 && doc.layer(0).background);
    Color p = doc.layer(0).pixels.get(1, 0);  // half-red over white, opaque
    CHECK(p.a == 255 && p.r == 255 && p.g >= 127 && p.g <= 128);
    hist.undo(doc);
    CHECK(doc.layer_count() == 3);

    hist.run(doc, std::make_unique<PromoteBackgroundCommand>(0));
    CHECK(!doc.layer(0).background && doc.layer(0).name == "Raster 1");
    hist.undo(doc);
    CHECK(doc.layer(0).background && doc.layer(0).name == "Background");
}

// Builds a minimal version-6 native file: 3x2 canvas, one opaque background
// layer and one 2x1 layer with transparency placed at (1,1), no compression.
static std::vector<uint8_t> make_psp_file() {
    std::vector<uint8_t> f;
    auto u8 = [&](int v) { f.push_back(static_cast<uint8_t>(v)); };
    auto u16 = [&](int v) { u8(v & 255); u8((v >> 8) & 255); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) u8((v >> (8 * i)) & 255); };
    auto i32 = [&](int32_t v) { u32(static_cast<uint32_t>(v)); };
    auto bytes = [&](const std::vector<uint8_t>& b) { f.insert(f.end(), b.begin(), b.end()); };
    auto block = [&](uint16_t id, const std::vector<uint8_t>& payload) {
        f.insert(f.end(), {'~', 'B', 'K', 0});
        u16(id);
        u32(static_cast<uint32_t>(payload.size()));
        bytes(payload);
    };
    auto build = [&](auto&& fn) { std::vector<uint8_t> saved; saved.swap(f); fn(); std::vector<uint8_t> out; out.swap(f); f.swap(saved); return out; };

    const char sig[] = "Paint Shop Pro Image File\n\x1a";
    f.insert(f.end(), sig, sig + 27);
    while (f.size() < 32) u8(0);
    u16(6); u16(0);  // version 6.0

    block(0, build([&] {  // general image attributes
        u32(46); i32(3); i32(2);
        for (int i = 0; i < 8; ++i) u8(0);  // resolution
        u8(0); u16(0 /*none*/); u16(24); u16(1); u32(16777216); u8(0); u32(18); i32(1); u16(2); u32(0);
    }));

    auto channel = [&](int bitmap_type, int channel_type, std::vector<uint8_t> data) {
        return build([&] { block(5, build([&] { u32(16); u32(static_cast<uint32_t>(data.size())); u32(static_cast<uint32_t>(data.size())); u16(bitmap_type); u16(channel_type); bytes(data); })); });
    };
    auto layer = [&](const std::string& name, int32_t rect[4], int32_t saved[4], int opacity, int blend, int visible, std::vector<std::vector<uint8_t>> chans) {
        return build([&] {
            block(4, build([&] {
                std::vector<uint8_t> info = build([&] {
                    u32(0);  // chunk length, patched below
                    u16(static_cast<int>(name.size())); for (char c : name) u8(c);
                    u8(1);
                    for (int i = 0; i < 4; ++i) i32(rect[i]);
                    for (int i = 0; i < 4; ++i) i32(saved[i]);
                    u8(opacity); u8(blend); u8(visible); u8(0); u8(0);
                    for (int i = 0; i < 8; ++i) i32(0);
                    u8(0); u8(0);
                    for (int i = 0; i < 43; ++i) u8(0);  // blend ranges etc.
                });
                const uint32_t len = static_cast<uint32_t>(info.size());
                for (int i = 0; i < 4; ++i) info[i] = static_cast<uint8_t>((len >> (8 * i)) & 255);
                bytes(info);
                u32(8); u16(static_cast<int>(chans.size() > 3 ? 2 : 1)); u16(static_cast<int>(chans.size()));
                for (auto& c : chans) bytes(c);
            }));
        });
    };
    int32_t full[4] = {0, 0, 3, 2}, full_saved[4] = {0, 0, 3, 2};
    int32_t top_rect[4] = {0, 0, 3, 2}, top_saved[4] = {1, 1, 3, 2};
    std::vector<uint8_t> bank = layer("Background", full, full_saved, 255, 0, 1,
                                      {channel(0, 1, {10, 20, 30, 40, 50, 60}), channel(0, 2, {1, 2, 3, 4, 5, 6}), channel(0, 3, {9, 9, 9, 9, 9, 9})});
    std::vector<uint8_t> top = layer("Top", top_rect, top_saved, 128, 7, 1,
                                     {channel(0, 1, {200, 201}), channel(0, 2, {0, 0}), channel(0, 3, {0, 0}), channel(1, 0, {255, 0})});
    bank.insert(bank.end(), top.begin(), top.end());
    block(3, bank);
    return f;
}

static void test_tube_info() {
    std::vector<uint8_t> file = make_psp_file();
    CHECK(!io::load_psp_tube_info(file.data(), file.size()));
    // Append a tube block: chunk 30, u16 0, step 200, 4 columns, 4 rows, 16 cells, random placement, incremental selection.
    const uint8_t tube[] = {'~', 'B', 'K', 0, 11, 0, 30, 0, 0, 0, 30, 0, 0, 0, 0, 0, 200, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 16, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0};
    file.insert(file.end(), tube, tube + sizeof(tube));
    auto t = io::load_psp_tube_info(file.data(), file.size());
    CHECK(t && t->step == 200 && t->columns == 4 && t->rows == 4 && t->total == 16 && t->placement == 1 && t->selection == 2);
    auto doc = io::load_psp_from_memory(file.data(), file.size(), nullptr, nullptr);
    CHECK(doc && doc->layer_count() == 2);  // the extra block does not disturb image loading
}

static void test_psp_reader() {
    std::vector<uint8_t> file = make_psp_file();
    std::string err;
    std::vector<std::string> warnings;
    auto doc = io::load_psp_from_memory(file.data(), file.size(), &err, &warnings);
    if (!doc) std::fprintf(stderr, "load_psp: %s\n", err.c_str());
    CHECK(doc != nullptr);
    CHECK(doc->width() == 3 && doc->height() == 2 && doc->layer_count() == 2);
    CHECK(doc->active_layer() == 1);
    const Layer& bg = doc->layer(0);
    CHECK(bg.name == "Background" && bg.background);
    Color c = bg.pixels.get(2, 1);
    CHECK(c.r == 60 && c.g == 6 && c.b == 9 && c.a == 255);
    const Layer& top = doc->layer(1);
    CHECK(top.name == "Top" && !top.background && top.blend == BlendMode::Multiply);
    CHECK(top.opacity > 0.49f && top.opacity < 0.51f);
    CHECK(top.pixels.get(0, 0).a == 0 && top.pixels.get(1, 0).a == 0);  // outside the saved rect
    CHECK(top.pixels.get(1, 1).r == 200 && top.pixels.get(1, 1).a == 255);
    CHECK(top.pixels.get(2, 1).r == 201 && top.pixels.get(2, 1).a == 0);
    CHECK(warnings.empty());

    file[0] = 'X';
    CHECK(io::load_psp_from_memory(file.data(), file.size(), &err, nullptr) == nullptr && !err.empty());
    CHECK(io::is_psp_extension("a.PspImage") && io::is_psp_extension("b.psptube") && !io::is_psp_extension("c.png"));
}

static void test_geometry() {
    // Resample: solid stays solid in every filter; 2x nearest duplicates pixels.
    Image solid(5, 3, {10, 200, 30, 255});
    for (auto f : {raster::Filter::Nearest, raster::Filter::Bilinear, raster::Filter::Bicubic}) {
        Image r = raster::resample(solid, 9, 7, f);
        CHECK(r.width() == 9 && r.height() == 7);
        Color c = r.get(4, 3);
        CHECK(c.r == 10 && c.g == 200 && c.b == 30 && c.a == 255);
        Image d = raster::resample(solid, 2, 1, f);
        CHECK(d.get(1, 0).g == 200);
    }
    Image two(2, 1);
    two.set(0, 0, {0, 0, 0, 255});
    two.set(1, 0, {255, 255, 255, 255});
    Image n = raster::resample(two, 4, 1, raster::Filter::Nearest);
    CHECK(n.get(1, 0).r == 0 && n.get(2, 0).r == 255);
    Image b = raster::resample(two, 4, 1, raster::Filter::Bilinear);
    CHECK(b.get(1, 0).r > 0 && b.get(1, 0).r < 128 && b.get(2, 0).r > 128 && b.get(2, 0).r < 255);
    // Shrinking averages: black+white -> gray.
    Image avg = raster::resample(two, 1, 1, raster::Filter::Bilinear);
    CHECK(avg.get(0, 0).r >= 127 && avg.get(0, 0).r <= 128);
    // Transparent neighbors don't bleed color.
    Image edge(2, 1);
    edge.set(0, 0, {255, 0, 0, 255});
    edge.set(1, 0, {0, 0, 0, 0});
    Image e = raster::resample(edge, 1, 1, raster::Filter::Bilinear);
    CHECK(e.get(0, 0).r == 255 && e.get(0, 0).a >= 127 && e.get(0, 0).a <= 128);

    // Crop, including a rect partly outside.
    Image img(4, 4);
    img.set(1, 1, {7, 0, 0, 255});
    Image c = raster::crop(img, {1, 1, 3, 3});
    CHECK(c.width() == 2 && c.get(0, 0).r == 7);
    Image c2 = raster::crop(img, {-1, -1, 2, 2});
    CHECK(c2.width() == 3 && c2.get(0, 0).a == 0 && c2.get(2, 2).r == 7);

    // Quarter rotations.
    Image r(3, 2);
    r.set(0, 0, {1, 0, 0, 255});  // top-left
    r.set(2, 1, {2, 0, 0, 255});  // bottom-right
    Image cw = raster::rotate_quarter(r, 1);
    CHECK(cw.width() == 2 && cw.height() == 3 && cw.get(1, 0).r == 1 && cw.get(0, 2).r == 2);
    Image ccw = raster::rotate_quarter(r, -1);
    CHECK(ccw.get(0, 2).r == 1 && ccw.get(1, 0).r == 2);
    Image half = raster::rotate_quarter(r, 2);
    CHECK(half.get(2, 1).r == 1 && half.get(0, 0).r == 2);
    // Free rotation by 90 lands on the same result within rounding.
    Image free = raster::rotate(r, 90.0f);
    CHECK(free.width() == 2 && free.height() == 3 && free.get(1, 0).r == 1 && free.get(0, 2).r == 2);
    int rw, rh;
    raster::rotated_size(100, 50, 45.0f, &rw, &rh);
    CHECK(rw == 107 && rh == 107);

    // Commands with undo, on a two-layer document with a selection.
    Document doc(4, 4);
    Layer& bg = doc.add_layer("Background");
    bg.background = true;
    bg.pixels.fill({255, 255, 255, 255});
    Layer& top = doc.add_layer("Top");
    top.pixels.set(3, 3, {9, 9, 9, 255});
    doc.set_selection(mask::rectangle(4, 4, 2, 2, 4, 4, false));
    CommandStack hist;
    hist.run(doc, std::make_unique<CropCommand>(raster::Rect{2, 2, 4, 4}));
    CHECK(doc.width() == 2 && doc.layer_count() == 2 && doc.layer(1).pixels.get(1, 1).r == 9);
    CHECK(doc.selection().at(0, 0) == 255);
    hist.undo(doc);
    CHECK(doc.width() == 4 && doc.layer(1).pixels.get(3, 3).r == 9 && doc.selection().at(0, 0) == 0);

    hist.run(doc, std::make_unique<ResizeCommand>(8, 8, raster::Filter::Nearest));
    CHECK(doc.width() == 8 && doc.layer(1).pixels.get(7, 7).r == 9 && doc.layer(1).pixels.get(5, 5).a == 0);
    CHECK(doc.selection().at(7, 7) == 255 && doc.selection().at(1, 1) == 0);
    hist.undo(doc);
    CHECK(doc.width() == 4);

    hist.run(doc, std::make_unique<CanvasSizeCommand>(6, 6, 1, 1, Color{0, 0, 255, 255}));
    CHECK(doc.width() == 6);
    CHECK(doc.layer(0).pixels.get(0, 0).b == 255 && doc.layer(0).pixels.get(0, 0).a == 255);  // background padded blue
    CHECK(doc.layer(0).pixels.get(1, 1).r == 255);
    CHECK(doc.layer(1).pixels.get(0, 0).a == 0 && doc.layer(1).pixels.get(4, 4).r == 9);       // top padded transparent
    CHECK(doc.selection().at(3, 3) == 255 && doc.selection().at(0, 0) == 0);
    hist.undo(doc);

    hist.run(doc, std::make_unique<RotateCommand>(90.0f, Color{0, 0, 0, 255}));
    CHECK(doc.width() == 4 && doc.layer(1).pixels.get(0, 3).r == 9);
    hist.undo(doc);
    hist.run(doc, std::make_unique<RotateCommand>(45.0f, Color{0, 255, 0, 255}));
    CHECK(doc.width() == 6 && doc.height() == 6);
    CHECK(doc.layer(0).pixels.get(0, 0).g == 255 && doc.layer(0).pixels.get(0, 0).a == 255);  // corner filled
    CHECK(doc.layer(1).pixels.get(0, 0).a == 0);
    hist.undo(doc);
    CHECK(doc.width() == 4 && doc.layer(1).pixels.get(3, 3).r == 9 && doc.layer(0).background);
}

static void test_psp_writer_roundtrip() {
    Document doc(5, 4);
    Layer& bg = doc.add_layer("Background");
    bg.background = true;
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 5; ++x) bg.pixels.set(x, y, {static_cast<uint8_t>(x * 50), static_cast<uint8_t>(y * 60), 7, 255});
    Layer& top = doc.add_layer("Semi");
    top.pixels.set(2, 1, {255, 0, 0, 128});
    top.pixels.set(3, 2, {0, 255, 0, 255});
    top.opacity = 0.6f;
    top.blend = BlendMode::Screen;
    top.visible = false;
    Layer& empty = doc.add_layer("Empty");
    (void)empty;
    doc.set_active_layer(1);

    std::vector<uint8_t> file = io::save_psp_to_memory(doc);
    std::string err;
    std::vector<std::string> warnings;
    auto back = io::load_psp_from_memory(file.data(), file.size(), &err, &warnings);
    if (!back) std::fprintf(stderr, "roundtrip: %s\n", err.c_str());
    CHECK(back != nullptr && warnings.empty());
    CHECK(back->width() == 5 && back->height() == 4 && back->layer_count() == 3 && back->active_layer() == 1);
    CHECK(back->layer(0).name == "Background" && back->layer(0).background);
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 5; ++x) {
        Color a = bg.pixels.get(x, y), b = back->layer(0).pixels.get(x, y);
        CHECK(a.r == b.r && a.g == b.g && a.b == b.b && b.a == 255);
    }
    const Layer& t = back->layer(1);
    CHECK(t.name == "Semi" && !t.background && !t.visible && t.blend == BlendMode::Screen);
    CHECK(t.opacity > 0.59f && t.opacity < 0.61f);
    Color p = t.pixels.get(2, 1);
    CHECK(p.r == 255 && p.g == 0 && p.a == 128);
    CHECK(t.pixels.get(3, 2).g == 255 && t.pixels.get(0, 0).a == 0 && t.pixels.get(4, 3).a == 0);
    CHECK(back->layer(2).name == "Empty" && back->layer(2).pixels.get(0, 0).a == 0);

    // Groups and masks survive a round trip; a masked raster layer comes back
    // as a masked raster layer even though the file stores a group.
    Document g(4, 2);
    g.add_layer("Background").background = true;
    Layer& grp = g.add_layer("Group A");
    grp.type = LayerType::Group; grp.pixels = Image(); grp.opacity = 0.5f; grp.blend = BlendMode::Multiply;
    Layer& m1 = g.add_layer("Member 1"); m1.depth = 1; m1.pixels.fill({10, 20, 30, 200});
    Layer& m2 = g.add_layer("Member 2"); m2.depth = 1; m2.visible = false;
    Mask gm(4, 2, 255); gm.at(0, 0) = 0;
    g.layer(1).mask = gm;
    Layer& top2 = g.add_layer("Masked top");
    Mask tm(4, 2, 255); tm.at(3, 1) = 7;
    top2.mask = tm; top2.mask_enabled = false; top2.pixels.fill({1, 2, 3, 255});
    std::vector<uint8_t> gf = io::save_psp_to_memory(g);
    auto gb = io::load_psp_from_memory(gf.data(), gf.size(), &err, &warnings);
    CHECK(gb != nullptr);
    CHECK(gb->layer_count() == 5);
    CHECK(gb->layer(1).type == LayerType::Group && gb->layer(1).name == "Group A" && gb->layer(1).blend == BlendMode::Multiply);
    CHECK(gb->layer(1).has_mask() && gb->layer(1).mask.at(0, 0) == 0 && gb->layer(1).mask.at(1, 0) == 255);
    CHECK(gb->layer(2).depth == 1 && gb->layer(3).depth == 1 && !gb->layer(3).visible && gb->group_end(1) == 4);
    CHECK(gb->layer(2).pixels.get(1, 1).a == 200);
    CHECK(gb->layer(4).is_raster() && gb->layer(4).depth == 0 && gb->layer(4).has_mask() && gb->layer(4).mask.at(3, 1) == 7 && !gb->layer(4).mask_enabled);
    CHECK(gb->layer(4).name == "Masked top" && gb->layer(4).pixels.get(0, 0).r == 1);

    // Alpha channels (saved selections) round-trip with their names.
    Document a(6, 3);
    a.add_layer("bg").background = true;
    Mask sel = mask::rectangle(6, 3, 1, 0, 4, 2, false);
    a.alpha_channels().push_back({"Selection #1", sel});
    a.alpha_channels().push_back({"Second", mask::rectangle(6, 3, 0, 0, 6, 1, false)});
    std::vector<uint8_t> af = io::save_psp_to_memory(a);
    auto ab = io::load_psp_from_memory(af.data(), af.size(), &err, nullptr);
    CHECK(ab && ab->alpha_channels().size() == 2);
    CHECK(ab->alpha_channels()[0].name == "Selection #1" && ab->alpha_channels()[1].name == "Second");
    CHECK(ab->alpha_channels()[0].mask.at(1, 0) == 255 && ab->alpha_channels()[0].mask.at(0, 0) == 0 && ab->alpha_channels()[0].mask.at(4, 1) == 0);
    CHECK(ab->alpha_channels()[1].mask.at(5, 0) == 255 && ab->alpha_channels()[1].mask.at(5, 1) == 0);
}

static void test_adjust_module() {
    // HSL round trip.
    for (Color c : {Color{255, 0, 0, 255}, Color{10, 200, 90, 255}, Color{128, 128, 128, 255}, Color{0, 0, 255, 255}}) {
        adjust::HSL h = adjust::rgb_to_hsl(c.r, c.g, c.b);
        uint8_t r, g, b;
        adjust::hsl_to_rgb(h, &r, &g, &b);
        CHECK(std::abs(r - c.r) <= 1 && std::abs(g - c.g) <= 1 && std::abs(b - c.b) <= 1);
    }
    CHECK(adjust::rgb_to_hsl(255, 0, 0).h == 0.0f && adjust::rgb_to_hsl(0, 255, 0).h == 120.0f);

    // Colorize keeps lightness; a gray becomes the requested hue.
    Image img(1, 1, {100, 100, 100, 255});
    adjust::colorize(img, 0, 255);
    Color c = img.get(0, 0);
    CHECK(c.r > c.g && c.g == c.b && c.g == 0);  // pure red at lightness 100/255 -> r=200
    CHECK(c.r == 200);
    // HSL: hue shift 120 turns red green; +100 lightness is white; -100 saturation is gray.
    Image red(1, 1, {255, 0, 0, 255});
    adjust::hsl_adjust(red, 120, 0, 0);
    CHECK(red.get(0, 0).g == 255 && red.get(0, 0).r == 0);
    adjust::hsl_adjust(red, 0, 0, 100);
    CHECK(red.get(0, 0).r == 255 && red.get(0, 0).b == 255);
    Image red2(1, 1, {255, 0, 0, 255});
    adjust::hsl_adjust(red2, 0, -100, 0);
    CHECK(red2.get(0, 0).r == red2.get(0, 0).g);

    // LUTs.
    adjust::Lut lv = adjust::levels_lut(64, 1.0f, 192, 0, 255);
    CHECK(lv[64] == 0 && lv[192] == 255 && lv[128] >= 127 && lv[128] <= 128);
    adjust::Lut ga = adjust::gamma_lut(2.0f);
    CHECK(ga[0] == 0 && ga[255] == 255 && ga[64] > 64);
    CHECK(adjust::threshold_lut(128)[127] == 0 && adjust::threshold_lut(128)[128] == 255);
    adjust::Lut po = adjust::posterize_lut(2);
    CHECK(po[0] == 0 && po[127] == 0 && po[128] == 255);
    adjust::Lut so = adjust::solarize_lut(128);
    CHECK(so[100] == 100 && so[200] == 55);
    adjust::Lut bc = adjust::brightness_contrast_lut(50, 0);
    CHECK(bc[100] == 150);
    adjust::Lut id = adjust::curve_lut({{0.0f, 0.0f}, {255.0f, 255.0f}});
    CHECK(id[0] == 0 && id[100] == 100 && id[255] == 255);
    adjust::Lut s = adjust::curve_lut({{0.0f, 0.0f}, {64.0f, 32.0f}, {192.0f, 224.0f}, {255.0f, 255.0f}});
    CHECK(s[64] == 32 && s[192] == 224 && s[128] > 100 && s[128] < 156);
    for (int i = 1; i < 256; ++i) CHECK(s[i] >= s[i - 1]);  // monotone

    // Channel mixer: swap red and blue; monochrome uses row 0.
    Image mix(1, 1, {200, 50, 10, 255});
    adjust::ChannelMix m;
    m.mix[0][0] = 0; m.mix[0][2] = 100; m.mix[2][2] = 0; m.mix[2][0] = 100;
    adjust::channel_mixer(mix, m);
    CHECK(mix.get(0, 0).r == 10 && mix.get(0, 0).b == 200 && mix.get(0, 0).g == 50);
    adjust::ChannelMix mono;
    mono.monochrome = true;
    mono.mix[0][0] = 100; mono.mix[0][1] = 0; mono.mix[0][2] = 0;
    Image mono_img(1, 1, {200, 50, 10, 255});
    adjust::channel_mixer(mono_img, mono);
    CHECK(mono_img.get(0, 0).r == 200 && mono_img.get(0, 0).g == 200 && mono_img.get(0, 0).b == 200);

    // Histogram ops on a low-contrast image.
    Image lowc(2, 1);
    lowc.set(0, 0, {100, 100, 100, 255});
    lowc.set(1, 0, {150, 150, 150, 255});
    Image st = lowc;
    adjust::histogram_stretch(st);
    CHECK(st.get(0, 0).r == 0 && st.get(1, 0).r == 255);
    Image ac = lowc;
    adjust::auto_contrast(ac, 0.0f);
    CHECK(ac.get(0, 0).r == 0 && ac.get(1, 0).r == 255);
    Image eq = lowc;
    adjust::histogram_equalize(eq);
    CHECK(eq.get(1, 0).r == 255 && eq.get(0, 0).r < eq.get(1, 0).r);
    CHECK(adjust::histogram_luma(lowc)[100] == 1 && adjust::histogram_luma(lowc)[150] == 1);
}

static void test_effects() {
    // Solid images are fixed points of the local filters.
    for (auto fn : {effects::sharpen, effects::sharpen_more, effects::blur_more, effects::soften, effects::soften_more,
                    effects::enhance_edges, effects::enhance_edges_more, effects::erode, effects::dilate}) {
        Image s(5, 5, {40, 90, 140, 255});
        fn(s);
        Color c = s.get(2, 2);
        CHECK(c.r == 40 && c.g == 90 && c.b == 140 && c.a == 255);
    }
    // Edge detection: flat is black, a step edge lights up.
    Image flat(5, 5, {100, 100, 100, 255});
    effects::find_edges(flat);
    CHECK(flat.get(2, 2).r == 0);
    Image step(6, 3, {0, 0, 0, 255});
    for (int y = 0; y < 3; ++y) for (int x = 3; x < 6; ++x) step.set(x, y, {255, 255, 255, 255});
    Image e = step;
    effects::find_edges(e);
    CHECK(e.get(2, 1).r > 200 && e.get(0, 1).r == 0);
    // Emboss is gray and mid-gray on flat areas.
    Image em(5, 5, {200, 50, 50, 255});
    effects::emboss(em);
    CHECK(em.get(2, 2).r == 128 && em.get(2, 2).g == 128);
    // Erode/dilate on a single bright pixel.
    Image dot(5, 5, {0, 0, 0, 255});
    dot.set(2, 2, {255, 255, 255, 255});
    Image di = dot; effects::dilate(di);
    CHECK(di.get(1, 1).r == 255 && di.get(0, 0).r == 0);
    Image er = dot; effects::erode(er);
    CHECK(er.get(2, 2).r == 0);
    // Median removes a lone outlier.
    Image md = dot; effects::median(md, 1);
    CHECK(md.get(2, 2).r == 0);
    // Sharpen increases the contrast of a step.
    Image sh = step; effects::sharpen(sh);
    CHECK(sh.get(2, 1).r == 0 && sh.get(3, 1).r == 255);
    Image soft(6, 3, {0, 0, 0, 255});
    for (int y = 0; y < 3; ++y) for (int x = 3; x < 6; ++x) soft.set(x, y, {200, 200, 200, 255});
    Image um = soft; effects::unsharp_mask(um, 1.0f, 100, 0);
    CHECK(um.get(3, 1).r > 200 && um.get(2, 1).r == 0);
    // Mosaic averages blocks.
    Image mo(4, 2);
    for (int x = 0; x < 4; ++x) { mo.set(x, 0, {0, 0, 0, 255}); mo.set(x, 1, {200, 200, 200, 255}); }
    effects::mosaic(mo, 2, 2);
    CHECK(mo.get(0, 0).r == 100 && mo.get(3, 1).r == 100);
    // Motion blur along x smears a dot; transparent surroundings keep color.
    Image mb(7, 1, {0, 0, 0, 0});
    mb.set(3, 0, {255, 0, 0, 255});
    effects::motion_blur(mb, 180.0f, 3);  // samples to the left: pixels 3,4,5 see it
    CHECK(mb.get(4, 0).a > 0 && mb.get(4, 0).r == 255 && mb.get(0, 0).a == 0);
    // Noise changes pixels, monochrome keeps them gray, amplitude bounded.
    Image nz(16, 16, {128, 128, 128, 255});
    effects::add_noise(nz, 20, false, true, 7);
    int changed = 0;
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) {
        Color c = nz.get(x, y);
        CHECK(c.r == c.g && c.g == c.b && c.r >= 128 - 51 && c.r <= 128 + 51);
        changed += c.r != 128;
    }
    CHECK(changed > 200);
    Image nz2(16, 16, {128, 128, 128, 255});
    effects::add_noise(nz2, 20, true, false, 7);
    CHECK(nz2.get(3, 3).r != nz2.get(3, 3).g || nz2.get(5, 5).r != nz2.get(5, 5).b);
    // Drop shadow: opaque square on transparent gets shadow below-right.
    Image ds(8, 8, {0, 0, 0, 0});
    for (int y = 1; y < 4; ++y) for (int x = 1; x < 4; ++x) ds.set(x, y, {255, 255, 255, 255});
    effects::drop_shadow(ds, 2, 2, 1.0f, 0.0f, {0, 0, 0, 255});
    CHECK(ds.get(2, 2).r == 255 && ds.get(2, 2).a == 255);  // original kept
    CHECK(ds.get(5, 5).a == 255 && ds.get(5, 5).r == 0);    // shadow
    CHECK(ds.get(7, 7).a == 0);
}

static void test_stroke_modes() {
    // Clone: copies from an offset in the source.
    Image src(8, 1, {0, 0, 0, 255});
    src.set(5, 0, {255, 0, 0, 255});
    Image base(8, 1, {0, 0, 0, 255});
    raster::Brush b; b.size = 1.5f; b.hardness = 1.0f;
    raster::Stroke st(base, b, {}, raster::StrokeMode::Clone);
    st.set_clone_source(&src, 4, 0);  // dest 1 <- src 5
    Image out = base;
    st.add_point(1.5f, 0.5f);
    st.render(out);
    CHECK(out.get(1, 0).r == 255 && out.get(2, 0).r == 0);
    // Filter: half-coverage blends halfway towards the filtered color.
    Image fb(3, 1, {100, 100, 100, 255});
    raster::Brush wide; wide.size = 100; wide.hardness = 1; wide.opacity = 0.5f;
    raster::Stroke sf(fb, wide, {}, raster::StrokeMode::Filter);
    sf.set_filter([](Color c) { return Color{200, c.g, c.b, c.a}; });
    Image fo = fb;
    sf.add_point(1, 0);
    sf.render(fo);
    CHECK(fo.get(1, 0).r == 150 && fo.get(1, 0).g == 100);
    // Airbrush accumulates with repeated stamps but caps at 1.
    Image ab(3, 1, {0, 0, 0, 255});
    raster::Brush air; air.size = 100; air.hardness = 1; air.accumulate = true; air.flow = 0.25f;
    raster::Stroke sa(ab, air, {255, 255, 255, 255}, raster::StrokeMode::Paint);
    Image ao = ab;
    sa.stamp_at(1, 0); sa.render(ao);
    CHECK(ao.get(1, 0).r >= 63 && ao.get(1, 0).r <= 65);
    sa.stamp_at(1, 0); sa.render(ao);
    CHECK(ao.get(1, 0).r >= 127 && ao.get(1, 0).r <= 128);
    for (int i = 0; i < 10; ++i) sa.stamp_at(1, 0);
    sa.render(ao);
    CHECK(ao.get(1, 0).r == 255);
    // Shift.
    Image sh(3, 1, {0, 0, 0, 0});
    sh.set(0, 0, {9, 9, 9, 255});
    Image moved = raster::shifted(sh, 2, 0);
    CHECK(moved.get(2, 0).r == 9 && moved.get(0, 0).a == 0);
}

static void test_text_and_polyline() {
    Mask line = mask::polyline(20, 10, {{2.0f, 5.5f}, {17.0f, 5.5f}}, 4.0f, true);
    CHECK(line.at(10, 5) == 255 && line.at(10, 4) == 255 && line.at(10, 6) == 255);
    CHECK(line.at(10, 2) == 0 && line.at(0, 5) == 255 && line.at(19, 5) == 0);  // round cap reaches x=0
    CHECK(line.at(10, 3) > 0 && line.at(10, 3) < 255);  // antialiased edge

    const char* candidates[] = {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf"};
    std::shared_ptr<text::Font> font;
    for (const char* c : candidates) if ((font = text::Font::load(c))) break;
    if (!font) { std::puts("text: no system font found, skipping"); return; }
    CHECK(!font->info().family.empty());
    text::Font::Layout lay;
    Image img = font->render("Hi\nthere", 32.0f, {255, 0, 0, 255}, true, text::Font::Align::Left, 1.0f, 0.0f, &lay);
    CHECK(img.width() > 20 && img.height() > 50 && lay.baseline > 0 && lay.baseline < img.height());
    int opaque = 0, partial = 0;
    for (int y = 0; y < img.height(); ++y) for (int x = 0; x < img.width(); ++x) {
        const Color c = img.get(x, y);
        CHECK(c.r == 255 && c.g == 0);
        opaque += c.a == 255;
        partial += c.a > 0 && c.a < 255;
    }
    CHECK(opaque > 50 && partial > 20);
    Image hard = font->render("Hi", 32.0f, {0, 0, 0, 255}, false, text::Font::Align::Left);
    for (int y = 0; y < hard.height(); ++y) for (int x = 0; x < hard.width(); ++x) CHECK(hard.get(x, y).a == 0 || hard.get(x, y).a == 255);
    // Center alignment shifts the short line right.
    Image left = font->render("I\nMMMM", 24.0f, {0, 0, 0, 255}, true, text::Font::Align::Left);
    Image center = font->render("I\nMMMM", 24.0f, {0, 0, 0, 255}, true, text::Font::Align::Center);
    auto first_ink_x = [](const Image& im, int row) { for (int x = 0; x < im.width(); ++x) if (im.get(x, row).a > 128) return x; return -1; };
    const int row = 12;
    CHECK(first_ink_x(center, row) > first_ink_x(left, row));
    CHECK(text::Font::load("/nonexistent.ttf") == nullptr);
}

static void test_adjust_round2() {
    // Color balance: midtone red pushes a gray towards red; preserve keeps lightness.
    Image g(1, 1, {128, 128, 128, 255});
    adjust::ColorBalance cb;
    cb.midtones[0] = 60;
    adjust::color_balance(g, cb);
    Color c = g.get(0, 0);
    CHECK(c.r > c.g && c.g == c.b);
    adjust::HSL before = adjust::rgb_to_hsl(128, 128, 128), after = adjust::rgb_to_hsl(c.r, c.g, c.b);
    CHECK(std::abs(before.l - after.l) < 0.02f);
    Image g2(1, 1, {128, 128, 128, 255});
    cb.preserve_luminosity = false;
    adjust::color_balance(g2, cb);
    CHECK(g2.get(0, 0).r > 128 && g2.get(0, 0).g == 128);
    // Sepia at 100 tints, at 0 is identity.
    Image s0(1, 1, {50, 100, 150, 255}), s1 = s0;
    adjust::sepia(s0, 0);
    adjust::sepia(s1, 100);
    CHECK(s0.get(0, 0).b == 150 && s1.get(0, 0).r > s1.get(0, 0).b);
    // Hue map: shifting the red band by 120 turns pure red green; blue untouched.
    Image hm(2, 1);
    hm.set(0, 0, {255, 0, 0, 255});
    hm.set(1, 0, {0, 0, 255, 255});
    adjust::HueMap map;
    map.shift[0] = 120;
    adjust::hue_map(hm, map);
    CHECK(hm.get(0, 0).g == 255 && hm.get(0, 0).r == 0 && hm.get(1, 0).b == 255);
    // Area filter sees the base image.
    Image base(3, 1, {0, 0, 0, 255});
    base.set(2, 0, {255, 255, 255, 255});
    raster::Brush b; b.size = 1.5f; b.hardness = 1;
    raster::Stroke st(base, b, {}, raster::StrokeMode::Filter);
    st.set_area_filter([](const Image& im, int x, int) { return im.get(std::min(x + 1, im.width() - 1), 0); });
    Image out = base;
    st.add_point(1.5f, 0.5f);
    st.render(out);
    CHECK(out.get(1, 0).r == 255 && out.get(0, 0).r == 0);
}

static void test_effects_round2() {
    // Zero-strength distortions are identities (up to rounding at the edge).
    Image g(9, 9);
    for (int y = 0; y < 9; ++y) for (int x = 0; x < 9; ++x) g.set(x, y, {static_cast<uint8_t>(x * 28), static_cast<uint8_t>(y * 28), 0, 255});
    Image w = g; effects::wave(w, 0, 10, 0, 10);
    Image p = g; effects::pinch(p, 0);
    Image t = g; effects::twirl(t, 0);
    for (int y = 1; y < 8; ++y) for (int x = 1; x < 8; ++x) {
        CHECK(w.get(x, y).r == g.get(x, y).r && p.get(x, y).r == g.get(x, y).r && t.get(x, y).r == g.get(x, y).r);
    }
    // Wave shifts rows; twirl moves an off-center pixel around.
    Image wv = g; effects::wave(wv, 2, 4, 0, 0);
    bool moved = false;
    for (int y = 0; y < 9; ++y) moved |= wv.get(4, y).r != g.get(4, y).r;
    CHECK(moved);
    Image tw = g; effects::twirl(tw, 90);
    CHECK(tw.get(4, 4).r == g.get(4, 4).r && tw.get(6, 4).r != g.get(6, 4).r);
    // Pinch pulls edge color towards the center.
    Image pi = g; effects::pinch(pi, 80);
    CHECK(pi.get(6, 4).r > g.get(6, 4).r);
    // Buttonize: top-left edge lighter, bottom-right darker (transparent edge).
    Image bt(20, 20, {100, 100, 100, 255});
    effects::buttonize(bt, 4, 1.0f, {0, 0, 0, 255}, true);
    CHECK(bt.get(0, 10).r > 100 && bt.get(19, 10).r < 100 && bt.get(10, 10).r == 100);
    Image bs(20, 20, {100, 100, 100, 255});
    effects::buttonize(bs, 4, 1.0f, {200, 0, 0, 255}, false);
    CHECK(bs.get(0, 10).r > 100 && bs.get(10, 10).r == 100);
    // Inner bevel on an opaque square in a transparent layer: lit from the left
    // brightens the left edge and darkens the right edge; center untouched.
    Image bv(30, 30, {0, 0, 0, 0});
    for (int y = 5; y < 25; ++y) for (int x = 5; x < 25; ++x) bv.set(x, y, {128, 128, 128, 255});
    effects::inner_bevel(bv, nullptr, 5, 180.0f, 1.0f, 1.0f);
    CHECK(bv.get(6, 15).r > 128 && bv.get(23, 15).r < 128 && bv.get(15, 15).r == 128);
    CHECK(bv.get(2, 2).a == 0);
    // Cutout with a selection region: darkens near the top-left inside edge, not the center.
    Image co(30, 30, {200, 200, 200, 255});
    Mask sel = mask::rectangle(30, 30, 5, 5, 25, 25, false);
    effects::cutout(co, sel.data(), 3, 3, 1.0f, 0.0f, {0, 0, 0, 255});
    CHECK(co.get(6, 6).r < 50 && co.get(15, 15).r == 200 && co.get(2, 2).r == 200);
}

static void test_more_shapes() {
    Mask rr = mask::rounded_rectangle(40, 40, 5, 5, 35, 35, 10, false);
    CHECK(rr.at(20, 20) == 255 && rr.at(20, 6) == 255);   // inside and mid-edge
    CHECK(rr.at(5, 5) == 0 && rr.at(34, 34) == 0);         // corners cut off
    CHECK(rr.at(2, 20) == 0);
    Mask tri = mask::regular_polygon(40, 40, 20, 20, 15, 15, 3, 0.0f, false);
    CHECK(tri.at(20, 10) == 255 && tri.at(20, 26) == 255 && tri.at(20, 30) == 0);  // apex at top, base at y=27.5
    CHECK(tri.at(8, 10) == 0 && tri.at(32, 10) == 0);
    Mask hex = mask::regular_polygon(40, 40, 20, 20, 15, 15, 6, 0.0f, true);
    CHECK(hex.at(20, 20) == 255 && hex.at(20, 6) == 255 && hex.at(6, 20) == 0);
    Mask st = mask::star(40, 40, 20, 20, 18, 18, 5, 0.4f, 0.0f, false);
    CHECK(st.at(20, 4) == 255 && st.at(20, 20) == 255);     // top point and center
    CHECK(st.at(4, 4) == 0 && st.at(20, 36) == 0);           // between points below
}

static void test_groups_and_masks() {
    Document doc(2, 1);
    Layer& bg = doc.add_layer("Background");
    bg.background = true;
    bg.pixels.fill({255, 255, 255, 255});
    Layer& r1 = doc.add_layer("Raster 1");
    r1.pixels.fill({0, 0, 0, 255});
    CommandStack hist;
    doc.set_active_layer(1);

    // Group the top layer; the group composites its member.
    hist.run(doc, std::make_unique<NewLayerGroupCommand>(1));
    CHECK(doc.layer_count() == 3 && doc.layer(1).type == LayerType::Group && doc.layer(2).depth == 1);
    CHECK(doc.group_end(1) == 3 && doc.parent_group(2) == 1 && doc.parent_group(0) == -1);
    CHECK(doc.composite().get(0, 0).r == 0);
    // Group opacity applies to the whole group; hidden group hides members.
    doc.layer(1).opacity = 0.5f;
    CHECK(doc.composite().get(0, 0).r >= 127 && doc.composite().get(0, 0).r <= 128);
    doc.layer(1).opacity = 1.0f;
    doc.layer(1).visible = false;
    CHECK(doc.composite().get(0, 0).r == 255);
    doc.layer(1).visible = true;
    // Group mask hides half.
    Mask m(2, 1);
    m.at(0, 0) = 0; m.at(1, 0) = 255;
    hist.run(doc, std::make_unique<SetMaskCommand>(1, "Mask", m));
    CHECK(doc.composite().get(0, 0).r == 255 && doc.composite().get(1, 0).r == 0);
    doc.layer(1).mask_enabled = false;
    CHECK(doc.composite().get(0, 0).r == 0);
    doc.layer(1).mask_enabled = true;
    hist.undo(doc);
    CHECK(!doc.layer(1).has_mask());
    hist.redo(doc);

    // Add a layer inside the group above the member; duplicate the group block.
    doc.set_active_layer(2);
    hist.run(doc, std::make_unique<AddLayerCommand>("Raster 2"));
    CHECK(doc.layer_count() == 4 && doc.layer(3).name == "Raster 2" && doc.layer(3).depth == 1 && doc.group_end(1) == 4);
    hist.run(doc, std::make_unique<DuplicateLayerCommand>(1));
    CHECK(doc.layer_count() == 7 && doc.layer(4).type == LayerType::Group && doc.layer(4).name == "Copy of Group" && doc.layer(6).depth == 1);
    hist.undo(doc);
    CHECK(doc.layer_count() == 4);
    // Arrange: move the group block below the background.
    hist.run(doc, std::make_unique<ArrangeLayerCommand>(1, -1));
    CHECK(doc.layer(0).type == LayerType::Group && doc.layer(2).name == "Raster 2" && doc.layer(3).name == "Background");
    hist.undo(doc);
    CHECK(doc.layer(0).name == "Background");
    // Remove the group removes its members; ungroup promotes them.
    hist.run(doc, std::make_unique<RemoveLayerCommand>(1));
    CHECK(doc.layer_count() == 1);
    hist.undo(doc);
    CHECK(doc.layer_count() == 4);
    hist.run(doc, std::make_unique<UngroupCommand>(1));
    CHECK(doc.layer_count() == 3 && doc.layer(1).depth == 0 && doc.layer(2).depth == 0 && doc.layer(1).is_raster());
    hist.undo(doc);
    CHECK(doc.layer_count() == 4 && doc.layer(1).type == LayerType::Group);
    // Geometry keeps masks: crop to the right half keeps the shown pixel.
    hist.run(doc, std::make_unique<CropCommand>(raster::Rect{1, 0, 2, 1}));
    CHECK(doc.width() == 1 && doc.layer(1).mask.at(0, 0) == 255 && doc.composite().get(0, 0).r == 0);
    hist.undo(doc);
    // Pixel commands ignore groups.
    hist.run(doc, std::make_unique<InvertCommand>(1));
    CHECK(doc.layer(1).pixels.empty());
}

static void test_square_brush() {
    Image base(21, 21, {0, 0, 0, 255});
    raster::Brush b; b.size = 10; b.hardness = 1; b.square = true;
    raster::Stroke st(base, b, {255, 255, 255, 255}, raster::StrokeMode::Paint);
    Image out = base;
    st.add_point(10.5f, 10.5f);
    st.render(out);
    CHECK(out.get(6, 6).r == 255 && out.get(14, 14).r == 255);  // corners of the square are painted
    CHECK(out.get(4, 10).r == 0 && out.get(10, 4).r == 0);
    raster::Brush r = b; r.square = false;
    raster::Stroke sr(base, r, {255, 255, 255, 255}, raster::StrokeMode::Paint);
    Image ro = base;
    sr.add_point(10.5f, 10.5f);
    sr.render(ro);
    CHECK(ro.get(6, 6).r == 0);  // round brush misses the corner
}

static void test_effects_round3() {
    Image g(21, 21);
    for (int y = 0; y < 21; ++y) for (int x = 0; x < 21; ++x) g.set(x, y, {static_cast<uint8_t>(x * 12), static_cast<uint8_t>(y * 12), 0, 255});
    Image r = g; effects::ripple(r, 0, 10);
    Image s0 = g; effects::spherize(s0, 0);
    Image l0 = g; effects::lens_distortion(l0, 0);
    CHECK(r.get(5, 5).r == g.get(5, 5).r && s0.get(5, 5).r == g.get(5, 5).r && l0.get(5, 5).r == g.get(5, 5).r);
    Image rp = g; effects::ripple(rp, 3, 6);
    bool moved = false;
    for (int x = 0; x < 21; ++x) moved |= rp.get(x, 10).r != g.get(x, 10).r;
    CHECK(moved);
    Image sp = g; effects::spherize(sp, 100);
    CHECK(sp.get(10, 10).r == g.get(10, 10).r && sp.get(14, 10).r < g.get(14, 10).r);  // bulge magnifies the center
    Image le = g; effects::lens_distortion(le, 100);
    CHECK(le.get(10, 10).r == g.get(10, 10).r && le.get(18, 10).r >= g.get(18, 10).r);   // barrel pulls the edge inward
    // Halftone: white stays paper, black becomes solid ink.
    Image ht(16, 16, {255, 255, 255, 255});
    effects::halftone(ht, 4, 0, {0, 0, 0, 255}, {255, 255, 255, 255});
    CHECK(ht.get(8, 8).r == 255);
    Image hb(16, 16, {0, 0, 0, 255});
    effects::halftone(hb, 4, 0, {0, 0, 0, 255}, {255, 255, 255, 255});
    CHECK(hb.get(8, 8).r == 0);
    // Chrome is gray and periodic.
    Image ch(4, 1);
    for (int x = 0; x < 4; ++x) ch.set(x, 0, {static_cast<uint8_t>(x * 85), static_cast<uint8_t>(x * 85), static_cast<uint8_t>(x * 85), 255});
    effects::chrome(ch, 2, 1.0f);
    CHECK(ch.get(0, 0).r == ch.get(0, 0).g && ch.get(0, 0).r == 0);
    CHECK(ch.get(1, 0).r > 100);
    // Outer bevel adds opaque rim pixels around an opaque square.
    Image ob(30, 30, {0, 0, 0, 0});
    for (int y = 10; y < 20; ++y) for (int x = 10; x < 20; ++x) ob.set(x, y, {128, 128, 128, 255});
    effects::outer_bevel(ob, nullptr, 4, 135.0f, 1.0f, {200, 200, 200, 255});
    CHECK(ob.get(8, 15).a == 255 && ob.get(21, 15).a == 255 && ob.get(3, 15).a == 0);
    CHECK(ob.get(8, 15).r > ob.get(21, 15).r);  // lit from the top-left
    CHECK(ob.get(15, 15).r == 128);
}

static void test_composite_region_and_speed() {
    Document doc(64, 48);
    Layer& bg = doc.add_layer("bg"); bg.background = true;
    for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x) bg.pixels.set(x, y, {static_cast<uint8_t>(x * 4), static_cast<uint8_t>(y * 5), 30, 255});
    Layer& half = doc.add_layer("half"); half.opacity = 0.5f; half.blend = BlendMode::Multiply;
    for (int y = 10; y < 30; ++y) for (int x = 10; x < 40; ++x) half.pixels.set(x, y, {200, 100, 50, 200});
    Layer& top = doc.add_layer("top");
    for (int y = 20; y < 40; ++y) for (int x = 30; x < 60; ++x) top.pixels.set(x, y, {0, 0, 255, static_cast<uint8_t>(x * 4)});
    const Image full = doc.composite();
    Image partial = full;
    // Scribble on the cache, then ask for one region back: it must match the full composite there.
    for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x) partial.set(x, y, {1, 2, 3, 4});
    doc.composite_into(partial, {15, 15, 50, 35});
    for (int y = 15; y < 35; ++y) for (int x = 15; x < 50; ++x) {
        Color a = full.get(x, y), b = partial.get(x, y);
        CHECK(a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a);
    }
    CHECK(partial.get(0, 0).r == 1);  // outside untouched
    // Dirty rect tracking.
    doc.take_dirty();
    doc.touch({5, 5, 10, 10});
    doc.touch({8, 8, 20, 12});
    raster::Rect d = doc.take_dirty();
    CHECK(d.x0 == 5 && d.y0 == 5 && d.x1 == 20 && d.y1 == 12);
    CHECK(doc.take_dirty().empty());
    doc.touch();
    CHECK(doc.take_dirty().x1 == 64);

    // Fast path agrees with the general path within rounding.
    Document f(3, 1);
    f.add_layer("a").pixels.fill({10, 20, 30, 255});
    Layer& b2 = f.add_layer("b");
    b2.pixels.set(0, 0, {200, 100, 0, 255});
    b2.pixels.set(1, 0, {200, 100, 0, 128});
    b2.pixels.set(2, 0, {200, 100, 0, 0});
    Image fc = f.composite();
    CHECK(fc.get(0, 0).r == 200 && fc.get(2, 0).r == 10);
    CHECK(fc.get(1, 0).r >= 104 && fc.get(1, 0).r <= 106);

    // Timing report (not asserted): a 12 MP three-layer composite.
    Document big(4000, 3000);
    big.add_layer("bg").pixels.fill({100, 100, 100, 255});
    big.add_layer("a").pixels.fill({50, 60, 70, 128});
    Layer& c = big.add_layer("c"); c.blend = BlendMode::Overlay; c.pixels.fill({200, 20, 20, 90});
    auto t0 = std::chrono::steady_clock::now();
    Image bc = big.composite();
    auto t1 = std::chrono::steady_clock::now();
    big.composite_into(bc, {1000, 1000, 1200, 1200});
    auto t2 = std::chrono::steady_clock::now();
    std::printf("composite 4000x3000x3 layers: %.0f ms full, %.1f ms for a 200x200 region\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count());
}

static void test_photo_fixes() {
    // Fade correction stretches a low-contrast pair and boosts saturation.
    Image faded(2, 1);
    faded.set(0, 0, {110, 100, 100, 255});
    faded.set(1, 0, {150, 140, 140, 255});
    adjust::fade_correction(faded, 100);
    CHECK(faded.get(0, 0).r < 80 && faded.get(1, 0).r > 200 && faded.get(0, 0).g < faded.get(0, 0).r);
    Image same(2, 1, {120, 120, 120, 255});
    Image before = same;
    adjust::fade_correction(same, 0);
    CHECK(same.get(0, 0).r == before.get(0, 0).r);
    // Red-eye: a red pupil turns dark grey; skin tones outside the circle and
    // non-red pixels inside are untouched.
    Image eye(20, 20, {220, 180, 160, 255});
    for (int y = 6; y < 14; ++y) for (int x = 6; x < 14; ++x) eye.set(x, y, {230, 40, 40, 255});
    eye.set(10, 10, {40, 200, 40, 255});
    adjust::red_eye(eye, 10, 10, 5, 1.0f);
    CHECK(eye.get(9, 9).r <= 60 && eye.get(9, 9).g == 40);
    CHECK(eye.get(10, 10).g == 200 && eye.get(10, 10).r == 40);
    CHECK(eye.get(2, 2).r == 220);
    Image skin(20, 20, {220, 180, 160, 255});
    adjust::red_eye(skin, 10, 10, 5, 1.0f);
    CHECK(skin.get(10, 10).r > 200);  // mild redness of skin barely changes
}

static void test_brush_tip() {
    // A tip: black left half, white right half -> only the left half paints.
    Image tip_img(4, 4, {255, 255, 255, 255});
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 2; ++x) tip_img.set(x, y, {0, 0, 0, 255});
    auto tip = raster::BrushTip::from_image(tip_img);
    CHECK(tip->width == 4 && tip->coverage[0] == 1.0f && tip->coverage[3] == 0.0f);
    Image base(40, 40, {0, 0, 0, 255});
    raster::Brush b; b.size = 20; b.tip = tip;
    raster::Stroke st(base, b, {255, 255, 255, 255}, raster::StrokeMode::Paint);
    Image out = base;
    st.add_point(20, 20);
    st.render(out);
    CHECK(out.get(13, 20).r == 255 && out.get(27, 20).r == 0);   // left half painted, right half not
    CHECK(out.get(20, 5).r == 0 && out.get(5, 20).r == 0);        // outside the 20 px tip
}

static void test_kaleidoscope_sunburst() {
    Image g(41, 41);
    for (int y = 0; y < 41; ++y) for (int x = 0; x < 41; ++x) g.set(x, y, {static_cast<uint8_t>(x * 6), static_cast<uint8_t>(y * 6), 0, 255});
    Image k = g;
    effects::kaleidoscope(k, 4, 0.0f, 100.0f);
    // Mirror symmetry across the horizontal axis through the center.
    CHECK(std::abs(k.get(30, 15).r - k.get(30, 25).r) <= 2 && std::abs(k.get(30, 15).g - k.get(30, 25).g) <= 2);
    Image s(41, 41, {0, 0, 0, 255});
    effects::sunburst(s, 0.5f, 0.5f, 1.0f, 0, 0.0f, {255, 255, 255, 255});
    CHECK(s.get(20, 20).r > 200 && s.get(0, 0).r < 30);  // bright at the source, dark in the corner
    Image r(41, 41, {0, 0, 0, 255});
    effects::sunburst(r, 0.5f, 0.5f, 0.0f, 8, 1.0f, {255, 255, 255, 255});
    int lit = 0;
    for (int x = 0; x < 41; ++x) lit += r.get(x, 20).r > 0 || r.get(20, x).r > 0;
    CHECK(lit > 0);
}

static void test_brush_texture() {
    // 2x1 texture: left pixel black (no paint), right pixel white (full).
    Image tex(2, 1);
    tex.set(0, 0, {0, 0, 0, 255});
    tex.set(1, 0, {255, 255, 255, 255});
    Image base(8, 4, {0, 0, 0, 255});
    raster::Brush b; b.size = 100; b.hardness = 1; b.texture = raster::BrushTip::texture_from_image(tex); b.texture_strength = 1.0f;
    raster::Stroke st(base, b, {255, 255, 255, 255}, raster::StrokeMode::Paint);
    Image out = base;
    st.add_point(4, 2);
    st.render(out);
    CHECK(out.get(0, 0).r == 0 && out.get(1, 0).r == 255 && out.get(2, 3).r == 0 && out.get(3, 3).r == 255);
    b.texture_strength = 0.5f;
    raster::Stroke half(base, b, {255, 255, 255, 255}, raster::StrokeMode::Paint);
    Image ho = base;
    half.add_point(4, 2);
    half.render(ho);
    CHECK(ho.get(0, 0).r >= 127 && ho.get(0, 0).r <= 128 && ho.get(1, 0).r == 255);

    // Back to pixels for the Texture effect's bump map: the full 0..255 range,
    // not coverage truncated to 0 and 1 (which left that effect flat).
    tex.set(0, 0, {128, 128, 128, 255});
    const Image back = raster::BrushTip::texture_from_image(tex)->to_image();
    CHECK(back.width() == 2 && back.height() == 1);
    CHECK(back.get(0, 0).r == 128 && back.get(0, 0).g == 128 && back.get(0, 0).a == 255);
    CHECK(back.get(1, 0).r == 255);
}

static void test_history_limit() {
    Document doc(1, 1);
    doc.add_layer("bg").pixels.fill({0, 0, 0, 255});
    CommandStack hist;
    hist.set_limit(3);
    for (int i = 0; i < 5; ++i) hist.run(doc, std::make_unique<InvertCommand>(0));
    CHECK(hist.size() == 3 && hist.cursor() == 3);
    hist.undo(doc); hist.undo(doc); hist.undo(doc);
    CHECK(!hist.can_undo() && doc.layer(0).pixels.get(0, 0).r == 0);  // 5 inverts, 3 undone -> 2 applied -> original
}

// Gradient::at_point applies the style, angle, centre, repeats and invert,
// and is what both painting and the material dialog's preview go through.
static void test_gradient_at_point() {
    vec::Gradient g;
    g.colors = {{{0, 0, 0, 255}, 0, 50}, {{255, 255, 255, 255}, 100, 50}};
    g.opacities = {{100, 0, 50}, {100, 100, 50}};
    // Linear at 0 degrees runs up the box: dark at the bottom, light at the top.
    CHECK(g.at_point(50, 2, 0, 0, 100, 100).r > g.at_point(50, 98, 0, 0, 100, 100).r);
    // Constant across a row.
    CHECK(g.at_point(5, 50, 0, 0, 100, 100).r == g.at_point(95, 50, 0, 0, 100, 100).r);
    // Turned 90 degrees it runs across instead.
    g.angle = 90;
    CHECK(g.at_point(5, 50, 0, 0, 100, 100).r != g.at_point(95, 50, 0, 0, 100, 100).r);
    // cos(90 degrees) is not exactly zero in floating point, so allow a step of one.
    CHECK(std::abs(g.at_point(50, 5, 0, 0, 100, 100).r - g.at_point(50, 95, 0, 0, 100, 100).r) <= 1);
    // Radial: the centre and the rim differ, and opposite rim points match.
    g.angle = 0;
    g.style = vec::GradientStyle::Radial;
    CHECK(g.at_point(50, 50, 0, 0, 100, 100).r != g.at_point(2, 2, 0, 0, 100, 100).r);
    CHECK(std::abs(g.at_point(2, 50, 0, 0, 100, 100).r - g.at_point(98, 50, 0, 0, 100, 100).r) <= 1);
    // Invert swaps the ends; repeats bring the start colour back inside.
    g.style = vec::GradientStyle::Linear;
    const int top = g.at_point(50, 2, 0, 0, 100, 100).r;
    g.invert = true;
    CHECK(std::abs(g.at_point(50, 2, 0, 0, 100, 100).r - (255 - top)) <= 1);
    g.invert = false;
    g.repeats = 1;
    CHECK(g.at_point(50, 51, 0, 0, 100, 100).r > g.at_point(50, 55, 0, 0, 100, 100).r);
}

static void test_vector_core() {
    // Rectangle fill and stroke rasterize where expected.
    vec::Object r = vec::make_rectangle(4, 4, 16, 12);
    r.fill.kind = vec::PaintStyle::Kind::Solid; r.fill.color = {0, 0, 255, 255};
    r.stroke.kind = vec::PaintStyle::Kind::Solid; r.stroke.color = {255, 0, 0, 255}; r.stroke_width = 2;
    r.antialias = false;
    Image img(24, 20, {0, 0, 0, 0});
    vec::rasterize({r}, img);
    CHECK(img.get(10, 8).b == 255 && img.get(10, 8).r == 0);   // inside: fill
    CHECK(img.get(4, 8).r == 255);                              // on the edge: stroke
    CHECK(img.get(1, 1).a == 0);
    float x0, y0, x1, y1;
    r.bounds(&x0, &y0, &x1, &y1);
    CHECK(x0 == 4 && y0 == 4 && x1 == 16 && y1 == 12);
    r.translate(2, 3);
    r.bounds(&x0, &y0, &x1, &y1);
    CHECK(x0 == 6 && y0 == 7);
    // Ellipse flattening stays within its bounds and is smooth (many points).
    vec::Object e = vec::make_ellipse(20, 20, 10, 5);
    auto pts = vec::flatten(e.paths[0]);
    CHECK(pts.size() > 20);
    for (auto& p : pts) CHECK(p.first >= 9.9f && p.first <= 30.1f && p.second >= 14.9f && p.second <= 25.1f);
    // Gradient: linear top-to-bottom at angle 0 from black to white; midpoint honored.
    vec::Gradient g;
    CHECK(g.at(0).r == 0 && g.at(1).r == 255 && g.at(0.5f).r >= 127 && g.at(0.5f).r <= 128);
    g.colors[0].mid = 25;
    CHECK(g.at(0.25f).r >= 127 && g.at(0.25f).r <= 128);
    g.colors[0].mid = 50;
    g.opacities[0].opacity = 0;
    CHECK(g.at(0).a == 0 && g.at(1).a == 255);
    vec::Object gr = vec::make_rectangle(0, 0, 10, 100);
    gr.fill.kind = vec::PaintStyle::Kind::Gradient;
    gr.fill.gradient = vec::Gradient{};
    gr.fill.gradient.angle = 180;  // first stop at the top
    gr.antialias = false;
    Image gi(10, 100, {0, 0, 0, 0});
    vec::rasterize({gr}, gi);
    CHECK(gi.get(5, 2).r < 20 && gi.get(5, 97).r > 235 && gi.get(5, 50).r > 110 && gi.get(5, 50).r < 145);
    // Even-odd: a square with a square hole.
    vec::Object hole = vec::make_rectangle(0, 0, 20, 20);
    vec::Object inner = vec::make_rectangle(5, 5, 15, 15);
    hole.paths.push_back(inner.paths[0]);
    hole.fill.kind = vec::PaintStyle::Kind::Solid; hole.antialias = false;
    Image hi(20, 20, {0, 0, 0, 0});
    vec::rasterize({hole}, hi);
    CHECK(hi.get(2, 2).a == 255 && hi.get(10, 10).a == 0);
    // Document: vector layer, edit command, convert to raster.
    Document doc(24, 20);
    doc.add_layer("bg").background = true;
    CommandStack hist;
    hist.run(doc, std::make_unique<AddVectorLayerCommand>("Vector 1"));
    CHECK(doc.layer_count() == 2 && doc.layer(1).is_vector() && doc.active_layer() == 1);
    std::vector<vec::Object> objs{r};
    hist.run(doc, std::make_unique<VectorEditCommand>(1, "Add Rectangle", objs));
    CHECK(doc.layer(1).objects.size() == 1 && doc.layer(1).pixels.get(11, 10).b == 255);
    hist.undo(doc);
    CHECK(doc.layer(1).objects.empty() && doc.layer(1).pixels.get(11, 10).a == 0);
    hist.redo(doc);
    hist.run(doc, std::make_unique<ConvertToRasterCommand>(1));
    CHECK(doc.layer(1).is_raster() && doc.layer(1).objects.empty() && doc.layer(1).pixels.get(11, 10).b == 255);
    hist.undo(doc);
    CHECK(doc.layer(1).is_vector() && doc.layer(1).objects.size() == 1);
}

static void test_text_objects_survive_native_save() {
    const auto fonts = text::list_fonts();
    if (fonts.empty()) { std::printf("  (no fonts installed; text shape test skipped)\n"); return; }
    std::shared_ptr<text::Font> font;
    std::string family;
    for (const auto& f : fonts) if (f.style == "Regular" && (font = text::Font::load(f.path))) { family = f.family + "  Regular"; break; }
    if (!font) return;
    Document doc(200, 100);
    doc.add_layer("Background").background = true;
    Layer& V = doc.add_layer("Vector");
    V.type = LayerType::Vector;
    vec::Object o;
    o.name = "Firn";
    o.is_text = true;
    o.text.text = "Firn";
    o.text.font_path = font->info().path;
    o.text.font_family = family;
    o.text.size = 36; o.text.align = 1; o.text.antialias = true;
    o.fill.kind = vec::PaintStyle::Kind::Solid; o.fill.color = {10, 20, 30, 255};
    o.paths = vec::text_outline_paths(o.text, *font, &o.text.baseline);
    o.translate(20, 30);   // moves the insert point along with the outlines
    CHECK(o.text.x == 20 && o.text.y == 30);
    CHECK(!o.paths.empty() && o.text.baseline > 0);
    V.objects.push_back(vec::make_polygon({{150.0f, 5.0f}, {160.0f, 5.0f}, {160.0f, 10.0f}}, true));  // a plain shape first
    V.objects.push_back(o);
    doc.rasterize_vector_layer(1);
    const std::string tmp = tmp_path("firn_test_text.pspimage");
    CHECK(io::save_psp(doc, tmp, nullptr));
    std::string err; std::vector<std::string> warnings;
    auto rt = io::load_psp(tmp, &err, &warnings);
    std::remove(tmp.c_str());
    CHECK(rt && rt->layer_count() == 2 && rt->layer(1).is_vector() && rt->layer(1).objects.size() == 2);
    const vec::Object& b = rt->layer(1).objects[1];
    CHECK(!rt->layer(1).objects[0].is_text && b.is_text && b.text.text == "Firn" && b.text.font_family.rfind(family.substr(0, family.find("  ")), 0) == 0);
    CHECK(b.text.size == 36 && b.text.align == 1 && b.text.antialias && b.fill.color.b == 30);
    // The outlines come back at the same place.
    float ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;
    CHECK(vec::outline_bounds(o, &ax0, &ay0, &ax1, &ay1) && vec::outline_bounds(b, &bx0, &by0, &bx1, &by1));
    CHECK(std::abs(ax0 - bx0) < 1.5f && std::abs(ay0 - by0) < 1.5f && std::abs(ax1 - bx1) < 1.5f);
    // Rotated about its center, as the app does: the matrix carries it.
    vec::Object rot = o;
    {
        const float cx = (ax0 + ax1) * 0.5f, cy = (ay0 + ay1) * 0.5f, r = 25.0f * 3.14159265f / 180.0f, c = std::cos(r), sn = std::sin(r);
        rot.transform(c, -sn, sn, c, cx - (c * cx - sn * cy), cy - (sn * cx + c * cy));
    }
    CHECK(std::abs(rot.text.rotation - 25.0f) < 0.01f);
    doc.layer(1).objects[1] = rot;
    CHECK(io::save_psp(doc, tmp, nullptr));
    auto rt2 = io::load_psp(tmp, &err, &warnings);
    std::remove(tmp.c_str());
    CHECK(rt2 && rt2->layer(1).objects.size() == 2);
    const vec::Object& rb = rt2->layer(1).objects[1];
    float rx0, ry0, rx1, ry1, qx0, qy0, qx1, qy1;
    CHECK(vec::outline_bounds(rot, &rx0, &ry0, &rx1, &ry1) && vec::outline_bounds(rb, &qx0, &qy0, &qx1, &qy1));
    CHECK(rb.is_text && std::abs(rb.text.rotation - 25.0f) < 0.5f);
    CHECK(std::abs(rx0 - qx0) < 2.0f && std::abs(ry0 - qy0) < 2.0f && std::abs(rx1 - qx1) < 2.0f && std::abs(ry1 - qy1) < 2.0f);
}

static void test_zip() {
    std::vector<zip::Entry> entries;
    entries.push_back({"mimetype", std::vector<uint8_t>{'a', 'b', 'c'}, true});
    std::vector<uint8_t> big(100000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>((i * 7) % 13);
    entries.push_back({"data/big.bin", big, false});
    entries.push_back({"empty", {}, false});
    const std::vector<uint8_t> bytes = zip::write(entries);
    CHECK(bytes.size() < big.size() / 2);   // deflated
    zip::Archive ar;
    std::string err;
    CHECK(zip::read(bytes.data(), bytes.size(), ar, &err));
    CHECK(ar.files.size() == 3 && ar.find("mimetype") && *ar.find("mimetype") == std::vector<uint8_t>({'a', 'b', 'c'}));
    CHECK(ar.find("data/big.bin") && *ar.find("data/big.bin") == big);
    CHECK(ar.find("empty") && ar.find("empty")->empty());
    // The index reads names without inflating anything; extract does one entry.
    zip::Index index;
    CHECK(zip::open(bytes.data(), bytes.size(), index, &err) && index.items.size() == 3);
    CHECK(index.find("data/big.bin") && index.find("data/big.bin")->usize == big.size());
    CHECK(!index.find("nope"));
    std::vector<uint8_t> one;
    CHECK(zip::extract(bytes.data(), bytes.size(), *index.find("data/big.bin"), one, &err) && one == big);
    // The mimetype entry is stored first and uncompressed, as OpenRaster requires.
    CHECK(std::memcmp(bytes.data() + 30, "mimetype", 8) == 0 && bytes[8] == 0 && std::memcmp(bytes.data() + 38, "abc", 3) == 0);
}

// The classic format's Firn stash names layers by index and by name; the
// index is only trusted when the name agrees, so a file whose layers moved
// cannot restore a filter or a style onto the wrong layer.
// Layers palette drag and drop: a whole group travels with its members and
// a layer dropped on a group member joins the group.
// Edge Preserving Smooth flattens noise inside an area but leaves the step
// between two areas intact.
// One Step Photo Fix should add punch without blocking up the shadows or
// draining the color, which is what it used to do: the contrast stretch
// collapsed everything below its clip point onto pure black, and clarify
// then scaled those pixels by zero.
// Content-aware fill rebuilds a hole from the rest of the picture. On a
// regular texture the right answer is known, so the fill should land on it
// almost exactly; a hole so large that no whole patch of known image is
// left must still be filled rather than abandoned.
// Edge-directed enlargement interpolates along an edge rather than across
// it, so on hard-edged artwork enlarged several times it lands closer to
// the true shape than bicubic. On a photograph it is close to bicubic;
// this pins the case it exists for.
static void test_edge_directed_resample() {
    auto shapes = [](int n) {
        Image im(n, n, {255, 255, 255, 255});
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double fx = (x + 0.5) / n, fy = (y + 0.5) / n;
                bool ink = std::hypot(fx - 0.35, fy - 0.35) < 0.22;
                ink |= std::abs((fy - 0.15) - 0.9 * (fx - 0.1)) < 0.018 && fx > 0.45;
                if (ink) im.set(x, y, {20, 40, 160, 255});
            }
        return im;
    };
    auto error = [](const Image& a, const Image& b) {
        double se = 0;
        size_t n = 0;
        for (size_t i = 0; i < a.size_bytes(); i += 4)
            for (int c = 0; c < 3; ++c) { const double d = a.data()[i + c] - b.data()[i + c]; se += d * d; ++n; }
        return se / n;
    };
    const Image small = shapes(96), truth = shapes(384);
    const Image cubic = raster::resample(small, 384, 384, raster::Filter::Bicubic);
    const Image edged = raster::resample(small, 384, 384, raster::Filter::EdgeDirected);
    CHECK(edged.width() == 384 && edged.height() == 384);
    CHECK(error(edged, truth) < error(cubic, truth));
    // Every filter keeps a flat field exactly flat: the weights are
    // normalized, so nothing overshoots or drifts.
    {
        Image flat(64, 64, {70, 130, 190, 255});
        for (raster::Filter f : {raster::Filter::Nearest, raster::Filter::Bilinear, raster::Filter::Bicubic,
                                 raster::Filter::Lanczos, raster::Filter::Mitchell, raster::Filter::EdgeDirected, raster::Filter::Smart}) {
            const Image up = raster::resample(flat, 100, 100, f);
            const Image down = raster::resample(flat, 30, 30, f);
            CHECK(up.get(50, 50).r == 70 && up.get(50, 50).g == 130 && up.get(50, 50).b == 190);
            CHECK(down.get(15, 15).r == 70 && down.get(15, 15).g == 130 && down.get(15, 15).b == 190);
        }
    }
    // Lanczos keeps more detail than bilinear through a reduction.
    {
        Image detail(128, 128);
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x) {
                const uint8_t v = static_cast<uint8_t>(128 + 90 * std::sin(x * 0.35) * std::cos(y * 0.27));
                detail.set(x, y, {v, v, v, 255});
            }
        auto variance = [](const Image& im) {
            double sum = 0, sum2 = 0;
            size_t n = 0;
            for (size_t i = 0; i < im.size_bytes(); i += 4) { sum += im.data()[i]; sum2 += static_cast<double>(im.data()[i]) * im.data()[i]; ++n; }
            return sum2 / n - (sum / n) * (sum / n);
        };
        CHECK(variance(raster::resample(detail, 64, 64, raster::Filter::Lanczos)) >
              variance(raster::resample(detail, 64, 64, raster::Filter::Bilinear)));
    }
    // Smart picks per resize: Lanczos down and for a modest enlargement,
    // edge directed once it is past a doubling.
    CHECK(std::memcmp(raster::resample(truth, 96, 96, raster::Filter::Smart).data(),
                      raster::resample(truth, 96, 96, raster::Filter::Lanczos).data(), 96 * 96 * 4) == 0);
    CHECK(std::memcmp(raster::resample(small, 140, 140, raster::Filter::Smart).data(),
                      raster::resample(small, 140, 140, raster::Filter::Lanczos).data(), 140 * 140 * 4) == 0);
    CHECK(std::memcmp(raster::resample(small, 384, 384, raster::Filter::Smart).data(), edged.data(), edged.size_bytes()) == 0);
    // Shrinking is left to the area average, so it matches bicubic exactly.
    const Image down_e = raster::resample(truth, 96, 96, raster::Filter::EdgeDirected);
    const Image down_c = raster::resample(truth, 96, 96, raster::Filter::Bicubic);
    CHECK(std::memcmp(down_e.data(), down_c.data(), down_c.size_bytes()) == 0);
}

static void test_content_aware_fill() {
    const int W = 192, H = 192;
    Image truth(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const bool cell = ((x / 16) + (y / 16)) % 2;
            const bool stripe = ((x + y) / 4) % 2;
            truth.set(x, y, {static_cast<uint8_t>(cell ? 200 : 60), static_cast<uint8_t>(stripe ? 180 : 90),
                             static_cast<uint8_t>(120 + ((x * 5 + y * 3) % 40)), 255});
        }
    auto fill_and_score = [&](int hole) {
        Image img = truth;
        Mask m(W, H, 0);
        const int x0 = (W - hole) / 2, y0 = (H - hole) / 2;
        for (int y = y0; y < y0 + hole; ++y)
            for (int x = x0; x < x0 + hole; ++x) { m.at(x, y) = 255; img.set(x, y, {255, 0, 255, 255}); }
        inpaint::content_aware_fill(img, m);
        double se = 0;
        int n = 0, magenta = 0;
        for (int y = y0; y < y0 + hole; ++y)
            for (int x = x0; x < x0 + hole; ++x) {
                const Color a = img.get(x, y), b = truth.get(x, y);
                if (a.r > 250 && a.g < 5 && a.b > 250) ++magenta;
                se += (a.r - b.r) * (a.r - b.r) + (a.g - b.g) * (a.g - b.g) + (a.b - b.b) * (a.b - b.b);
                n += 3;
            }
        CHECK(magenta == 0);   // nothing of the hole was left untouched
        return se / n;
    };
    CHECK(fill_and_score(32) < 50.0);    // a small hole comes back essentially exact
    CHECK(fill_and_score(64) < 200.0);
    // A half-covered selection lands halfway: the synthesized texture mixed
    // with what was there, rather than replacing it outright.
    Image img = truth;
    Mask m(W, H, 0);
    for (int y = 80; y < 112; ++y)
        for (int x = 80; x < 112; ++x) { m.at(x, y) = 128; img.set(x, y, {255, 0, 255, 255}); }
    inpaint::content_aware_fill(img, m);
    const Color mid = img.get(96, 96), want = truth.get(96, 96);
    CHECK(std::abs(mid.g - (0 + want.g) / 2) < 40);          // green moved from 0 toward the texture
    CHECK(mid.r < 255 && mid.r > want.r);                    // red came down from magenta but not all the way
    // Nothing outside the selection moves.
    CHECK(img.get(10, 10).r == truth.get(10, 10).r && img.get(180, 180).b == truth.get(180, 180).b);
}

static void test_one_step_photo_fix() {
    // A photo-like image: a bright colorful half and a dark half that still
    // holds detail.
    Image img(64, 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const int detail = ((x / 4 + y / 4) % 3) * 4;          // texture worth keeping
            if (y < 32) img.set(x, y, {static_cast<uint8_t>(170 + detail), static_cast<uint8_t>(90 + detail), static_cast<uint8_t>(60 + detail), 255});
            else img.set(x, y, {static_cast<uint8_t>(10 + detail), static_cast<uint8_t>(14 + detail), static_cast<uint8_t>(22 + detail), 255});
        }
    auto black_fraction = [](const Image& im) {
        int n = 0;
        for (size_t i = 0; i < im.size_bytes(); i += 4)
            if (im.data()[i] <= 1 && im.data()[i + 1] <= 1 && im.data()[i + 2] <= 1) ++n;
        return static_cast<double>(n) / (im.size_bytes() / 4);
    };
    auto mean_saturation = [](const Image& im) {
        double s = 0;
        size_t n = 0;
        for (size_t i = 0; i < im.size_bytes(); i += 4) {
            const int mx = std::max({im.data()[i], im.data()[i + 1], im.data()[i + 2]});
            const int mn = std::min({im.data()[i], im.data()[i + 1], im.data()[i + 2]});
            s += mx ? static_cast<double>(mx - mn) / mx : 0.0;
            ++n;
        }
        return s / n;
    };
    auto shadow_levels = [](const Image& im) {   // distinct values left in the dark half
        std::array<bool, 256> seen{};
        seen.fill(false);
        for (int y = 32; y < 64; ++y) for (int x = 0; x < 64; ++x) seen[im.get(x, y).b] = true;
        int n = 0;
        for (bool v : seen) if (v) ++n;
        return n;
    };
    const double sat_before = mean_saturation(img);
    const int levels_before = shadow_levels(img);
    Image after = img;
    photo::one_step_photo_fix(after);
    CHECK(black_fraction(after) < 0.01);                  // the shadows are not crushed flat
    CHECK(shadow_levels(after) >= levels_before - 1);     // and still hold their detail
    CHECK(mean_saturation(after) > sat_before);           // a saturation enhancement raises saturation
    // Clarify on its own must not invent pure black either.
    Image cl = img;
    photo::clarify(cl, 5);
    CHECK(black_fraction(cl) <= black_fraction(img));
}

static void test_edge_preserving_smooth() {
    Image img(40, 40);
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 40; ++x) {
            const int base = x < 20 ? 60 : 200;
            const int n = ((x * 7 + y * 13) % 11) - 5;   // +/-5 of noise
            const uint8_t v = static_cast<uint8_t>(std::clamp(base + n, 0, 255));
            img.set(x, y, {v, v, v, 255});
        }
    auto spread = [](const Image& im, int x0, int x1) {
        int lo = 255, hi = 0;
        for (int y = 10; y < 30; ++y)
            for (int x = x0; x < x1; ++x) { lo = std::min<int>(lo, im.get(x, y).r); hi = std::max<int>(hi, im.get(x, y).r); }
        return hi - lo;
    };
    const int before = spread(img, 4, 16);
    Image out = img;
    photo::edge_preserving_smooth(out, 50);
    CHECK(spread(out, 4, 16) < before / 2);                       // the noise inside the flat area is gone
    CHECK(out.get(24, 20).r - out.get(15, 20).r > 100);           // the step between the areas survives
    CHECK(out.get(5, 5).a == 255);
}

static void test_move_layer() {
    Document doc(8, 8);
    doc.add_layer("Background");          // 0
    Layer& g = doc.add_layer("Group");    // 1
    g.type = LayerType::Group;
    doc.add_layer("Inner A").depth = 1;   // 2
    doc.add_layer("Inner B").depth = 1;   // 3
    doc.add_layer("Loose");               // 4
    CommandStack stack;

    // Drop "Loose" onto "Inner A": it lands above it, inside the group.
    stack.run(doc, std::make_unique<MoveLayerCommand>(4, 3, 1));
    CHECK(doc.layer(3).name == "Loose" && doc.layer(3).depth == 1);
    CHECK(doc.group_end(1) == 5 && doc.layer_count() == 5);
    stack.undo(doc);
    CHECK(doc.layer(4).name == "Loose" && doc.layer(4).depth == 0 && doc.group_end(1) == 4);
    stack.redo(doc);
    CHECK(doc.layer(3).name == "Loose");

    // Drop the group onto "Background": the group and all three members move.
    stack.run(doc, std::make_unique<MoveLayerCommand>(1, 1, 0));
    CHECK(doc.layer(0).name == "Background");
    stack.run(doc, std::make_unique<MoveLayerCommand>(1, 0, 0));
    CHECK(doc.layer(0).name == "Group" && doc.layer(1).depth == 1 && doc.layer(4).name == "Background");
    CHECK(doc.group_end(0) == 4);
    stack.undo(doc);
    CHECK(doc.layer(0).name == "Background");

    // A group cannot land inside itself: the stack is left alone.
    const size_t n = doc.layer_count();
    stack.run(doc, std::make_unique<MoveLayerCommand>(1, 3, 1));
    CHECK(doc.layer_count() == n && doc.layer(1).name == "Group" && doc.layer(1).depth == 0);
}

static void test_firn_stash_resolution() {
    Document doc(24, 16);
    doc.add_layer("Background").background = true;
    doc.layer(0).pixels.fill({10, 20, 30, 255});
    Layer& blur = doc.add_layer("Blur");
    blur.type = LayerType::Adjustment;
    blur.adjustment.kind = Adjustment::Kind::GaussianBlur;
    blur.adjustment.blur_radius = 4.0f;
    Layer& top = doc.add_layer("Top");
    top.pixels = Image(24, 16, {0, 0, 0, 0});
    top.style.drop_shadow = true;
    std::vector<uint8_t> bytes = io::save_psp_to_memory(doc);

    // Patch the stash's indices out of range, keeping the byte length: the
    // reader must fall back to the names (what a reordered file looks like).
    auto patch = [&](std::vector<uint8_t>& data, const std::string& from, const std::string& to) {
        CHECK(from.size() == to.size());
        const auto it = std::search(data.begin(), data.end(), from.begin(), from.end());
        CHECK(it != data.end());
        std::copy(to.begin(), to.end(), it);
    };
    std::vector<uint8_t> moved = bytes;
    patch(moved, "\"layer\":1", "\"layer\":7");
    patch(moved, "\"layer\":2", "\"layer\":8");
    std::string err;
    std::vector<std::string> warnings;
    auto back = io::load_psp_from_memory(moved.data(), moved.size(), &err, &warnings);
    CHECK(back && back->layer_count() == 3 && warnings.empty());
    CHECK(back->layer(1).name == "Blur" && back->layer(1).is_adjustment() && std::abs(back->layer(1).adjustment.blur_radius - 4.0f) < 1e-4f);
    CHECK(back->layer(2).name == "Top" && back->layer(2).style.drop_shadow);
    CHECK(!back->layer(0).is_adjustment() && !back->layer(0).style.any());

    // A name that no longer exists: the entry is dropped with a warning and
    // the placeholder stays an ordinary layer, rather than landing anywhere.
    std::vector<uint8_t> gone = moved;
    patch(gone, "\"name\":\"Blur\"", "\"name\":\"Blue\"");
    warnings.clear();
    auto back2 = io::load_psp_from_memory(gone.data(), gone.size(), &err, &warnings);
    CHECK(back2 && back2->layer_count() == 3 && warnings.size() == 1);
    CHECK(back2->layer(1).name == "Blur" && !back2->layer(1).is_adjustment());
    CHECK(back2->layer(2).style.drop_shadow);   // the entry that still matches is kept
}

static void test_openraster() {
    // A document using everything: background, a 16-bit layer, a masked
    // group with two members, a vector layer, an adjustment layer, a filter
    // layer, a style, a saved selection and a profile.
    Document doc(32, 24);
    Layer& bg = doc.add_layer("Background");
    bg.background = true;
    bg.pixels.fill({200, 100, 50, 255});
    Layer& deep = doc.add_layer("Deep");
    { Image16 d(32, 24); for (size_t i = 0; i < d.size(); i += 4) { d.data()[i] = 1000; d.data()[i + 1] = 2000; d.data()[i + 2] = 3000; d.data()[i + 3] = 40000; } deep.set_deep(std::move(d)); }
    deep.blend = BlendMode::Multiply;
    deep.opacity = 0.5f;
    Layer& group = doc.add_layer("Group");
    group.type = LayerType::Group;
    group.mask = mask::rectangle(32, 24, 0, 0, 16, 24, false);
    Layer& m1 = doc.add_layer("Member 1");
    m1.depth = 1;
    m1.pixels = Image(32, 24, {0, 0, 0, 0});
    m1.pixels.set(3, 3, {10, 20, 30, 255});
    m1.visible = false;
    m1.blend = BlendMode::Dissolve;   // no SVG operator: restored from firn:blend
    Layer& m2 = doc.add_layer("Member 2");
    m2.depth = 1;
    m2.pixels = Image(32, 24, {0, 0, 0, 0});
    m2.style.drop_shadow = true;
    Layer& vec_layer = doc.add_layer("Shapes");
    vec_layer.type = LayerType::Vector;
    { vec::Object rect = vec::make_rectangle(4, 4, 16, 12); rect.fill.kind = vec::PaintStyle::Kind::Solid; rect.fill.color = {0, 255, 0, 255}; vec_layer.objects.push_back(rect); }
    vec_layer.pixels = Image(32, 24, {0, 0, 0, 0});
    doc.rasterize_vector_layer(5);
    Layer& adj = doc.add_layer("Invert");
    adj.type = LayerType::Adjustment;
    adj.adjustment.kind = Adjustment::Kind::Invert;
    Layer& filt = doc.add_layer("Blur");
    filt.type = LayerType::Adjustment;
    filt.adjustment.kind = Adjustment::Kind::GaussianBlur;
    filt.adjustment.blur_radius = 2.5f;
    doc.alpha_channels().push_back({"Saved 1", mask::rectangle(32, 24, 2, 2, 10, 10, false)});
    doc.guides_h().push_back(6.5f);
    doc.guides_v().push_back(11.0f);
    doc.guides_v().push_back(20.0f);
    { Assistant a; a.kind = Assistant::Kind::VanishingPoint; a.x0 = 30; a.y0 = 12; doc.assistants().push_back(a); }
    { Assistant a; a.kind = Assistant::Kind::Ruler; a.x0 = 1; a.y0 = 2; a.x1 = 3; a.y1 = 4; doc.assistants().push_back(a); }
    doc.set_icc(std::vector<uint8_t>{1, 2, 3, 4, 5});

    const std::vector<uint8_t> bytes = io::save_ora_to_memory(doc);
    std::string err;
    std::vector<std::string> warnings;
    auto back = io::load_ora_from_memory(bytes.data(), bytes.size(), &err, &warnings);
    CHECK(back && err.empty());
    CHECK(back->width() == 32 && back->height() == 24 && back->layer_count() == 8);
    CHECK(back->layer(0).background && back->layer(0).pixels.get(5, 5).r == 200);
    CHECK(back->layer(1).is_deep() && back->layer(1).deep->data()[3] == 40000 && back->layer(1).blend == BlendMode::Multiply && std::abs(back->layer(1).opacity - 0.5f) < 1e-3f);
    CHECK(back->layer(2).type == LayerType::Group && back->layer(2).has_mask() && back->layer(2).mask.at(3, 3) == 255 && back->layer(2).mask.at(20, 3) == 0);
    CHECK(back->layer(3).depth == 1 && !back->layer(3).visible && back->layer(3).blend == BlendMode::Dissolve && back->layer(3).pixels.get(3, 3).b == 30);
    CHECK(back->layer(4).depth == 1 && back->layer(4).style.drop_shadow);
    CHECK(back->layer(5).is_vector() && back->layer(5).objects.size() == 1 && back->layer(5).pixels.get(8, 8).a == 255);
    CHECK(back->layer(6).is_adjustment() && back->layer(6).adjustment.kind == Adjustment::Kind::Invert);
    CHECK(back->layer(7).is_adjustment() && back->layer(7).adjustment.kind == Adjustment::Kind::GaussianBlur && std::abs(back->layer(7).adjustment.blur_radius - 2.5f) < 1e-4f);
    CHECK(back->alpha_channels().size() == 1 && back->alpha_channels()[0].name == "Saved 1" && back->alpha_channels()[0].mask.at(5, 5) == 255);
    CHECK(back->icc() == std::vector<uint8_t>({1, 2, 3, 4, 5}));
    CHECK(back->guides_h().size() == 1 && std::abs(back->guides_h()[0] - 6.5f) < 1e-3f);
    CHECK(back->guides_v().size() == 2 && back->guides_v()[1] == 20.0f);
    CHECK(back->assistants().size() == 2 && back->assistants()[0].kind == Assistant::Kind::VanishingPoint && back->assistants()[0].x0 == 30);
    CHECK(back->assistants()[1].kind == Assistant::Kind::Ruler && back->assistants()[1].y1 == 4);
    // A layer is stored as just its content box plus an offset, and a 16-bit
    // one still comes back at 16 bits.
    {
        Document small(200, 200);
        small.add_layer("Background").pixels.fill({0, 0, 0, 255});
        Layer& dot = small.add_layer("Dot");
        Image16 d(200, 200);
        for (size_t k = 0; k < d.size(); ++k) d.data()[k] = 0;
        for (int y = 100; y < 110; ++y)
            for (int x = 150; x < 160; ++x) {
                uint16_t* px = d.data() + (static_cast<size_t>(y) * 200 + x) * 4;
                px[0] = 65535; px[1] = 300; px[2] = 700; px[3] = 65535;
            }
        dot.set_deep(std::move(d));
        const std::vector<uint8_t> ob = io::save_ora_to_memory(small);
        zip::Archive sar;
        CHECK(zip::read(ob.data(), ob.size(), sar, &err));
        std::string sxml;
        { const std::vector<uint8_t>* x2 = sar.find("stack.xml"); CHECK(x2); sxml.assign(x2->begin(), x2->end()); }
        CHECK(sxml.find("x=\"150\" y=\"100\"") != std::string::npos);   // stored at its offset
        auto sback = io::load_ora_from_memory(ob.data(), ob.size(), &err, nullptr);
        CHECK(sback && sback->layer_count() == 2 && sback->layer(1).is_deep());
        const uint16_t* got = sback->layer(1).deep->data() + (static_cast<size_t>(104) * 200 + 154) * 4;
        CHECK(got[0] == 65535 && got[1] == 300 && got[3] == 65535);
        CHECK(sback->layer(1).deep->data()[3] == 0);   // outside the box stays clear
    }

    // The thumbnail is readable on its own, without the layers.
    { const std::string tmp = tmp_path("firn_test_thumb.ora");
      std::ofstream f(tmp, std::ios::binary);
      f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      f.close();
      auto thumb = io::load_ora_thumbnail(tmp);
      CHECK(thumb && thumb->width() == 32 && thumb->height() == 24);
      std::remove(tmp.c_str()); }
    CHECK(back->group_end(2) == 5);
    // The composites agree.
    const Image a = doc.composite(), b = back->composite();
    int diff = 0;
    for (size_t i = 0; i < a.size_bytes(); ++i) diff = std::max(diff, std::abs(a.data()[i] - b.data()[i]));
    CHECK(diff <= 1);

    // A file as another editor writes it: offsets, hidden layers, SVG operators, nested stacks.
    Image tile(4, 4, {255, 0, 0, 255});
    std::vector<zip::Entry> entries;
    entries.push_back({"mimetype", std::vector<uint8_t>(std::begin("image/openraster"), std::end("image/openraster") - 1), true});
    const std::string xml =
        "<?xml version='1.0' encoding='UTF-8'?>\n"
        "<image version=\"0.0.1\" w=\"10\" h=\"8\">\n"
        "  <stack>\n"
        "    <layer name=\"Top &amp; tile\" src=\"data/t.png\" x=\"6\" y=\"5\" opacity=\"0.25\" visibility=\"hidden\" composite-op=\"svg:multiply\"/>\n"
        "    <stack name=\"Folder\" opacity=\"1.0\">\n"
        "      <layer name=\"Inner\" src=\"data/t.png\" x=\"-2\" y=\"-2\" composite-op=\"svg:src-over\" selected=\"true\"/>\n"
        "    </stack>\n"
        "    <layer name=\"Base\" src=\"data/t.png\" x=\"0\" y=\"0\" composite-op=\"svg:plus\"/>\n"
        "  </stack>\n"
        "</image>\n";
    entries.push_back({"stack.xml", std::vector<uint8_t>(xml.begin(), xml.end()), false});
    entries.push_back({"data/t.png", io::encode_png(tile), false});
    const std::vector<uint8_t> foreign = zip::write(entries);
    warnings.clear();
    auto f = io::load_ora_from_memory(foreign.data(), foreign.size(), &err, &warnings);
    CHECK(f && f->layer_count() == 4);
    CHECK(f->layer(0).name == "Base" && f->layer(0).blend == BlendMode::Normal && warnings.size() == 1);   // svg:plus unsupported
    CHECK(f->layer(1).type == LayerType::Group && f->layer(1).name == "Folder");
    CHECK(f->layer(2).depth == 1 && f->layer(2).name == "Inner" && f->layer(2).pixels.get(1, 1).r == 255 && f->layer(2).pixels.get(2, 2).a == 0);   // placed at -2,-2
    CHECK(f->layer(3).name == "Top & tile" && !f->layer(3).visible && f->layer(3).blend == BlendMode::Multiply && std::abs(f->layer(3).opacity - 0.25f) < 1e-3f);
    CHECK(f->layer(3).pixels.get(6, 5).r == 255 && f->layer(3).pixels.get(5, 5).a == 0 && f->layer(3).pixels.get(9, 7).r == 255);
}

static void test_layer_styles() {
    // A white square on a transparent layer over a gray background, with a
    // drop shadow and a red stroke.
    Document doc(60, 60);
    doc.add_layer("Background").pixels.fill({128, 128, 128, 255});
    Layer& top = doc.add_layer("Square");
    top.pixels = Image(60, 60, {0, 0, 0, 0});
    for (int y = 20; y < 40; ++y) for (int x = 20; x < 40; ++x) top.pixels.set(x, y, {255, 255, 255, 255});
    LayerStyle st;
    st.drop_shadow = true; st.shadow_offset_x = 6; st.shadow_offset_y = 6; st.shadow_blur = 1.0f; st.shadow_opacity = 1.0f;
    st.stroke = true; st.stroke_width = 2; st.stroke_color = {255, 0, 0, 255};
    CommandStack stack;
    stack.run(doc, std::make_unique<SetLayerStyleCommand>(1, LayerStyle(), st));
    Image c = doc.composite();
    CHECK(c.get(30, 30).r == 255);                                   // the square itself
    CHECK(c.get(41, 30).r == 255 && c.get(41, 30).g == 0);           // stroke just outside the edge
    CHECK(c.get(44, 44).r < 60);                                     // shadow below and right, outside the stroke
    CHECK(c.get(5, 5).r == 128);                                     // untouched background
    stack.undo(doc);
    CHECK(doc.composite().get(41, 30).r == 128);
    stack.redo(doc);
    // Partial recomposite of a rect matches the full composite.
    doc.take_dirty();
    doc.layer(1).pixels.set(25, 25, {0, 0, 0, 255});
    doc.touch({25, 25, 26, 26});
    const raster::Rect dirty = doc.take_dirty();
    CHECK(dirty.x1 - dirty.x0 > 10);
    Image part = c;
    doc.composite_into(part, dirty);
    const Image full = doc.composite();
    CHECK(std::memcmp(part.data(), full.data(), full.size_bytes()) == 0);
    // A style on a group works from the group's composite, not its members.
    {
        Document g(60, 60);
        g.add_layer("Background").pixels.fill({128, 128, 128, 255});
        Layer& grp = g.add_layer("Group");
        grp.type = LayerType::Group;
        grp.style.stroke = true;
        grp.style.stroke_width = 2;
        grp.style.stroke_color = {255, 0, 0, 255};
        Layer& left = g.add_layer("Left");
        left.depth = 1;
        left.pixels = Image(60, 60, {0, 0, 0, 0});
        for (int y = 20; y < 40; ++y) for (int x = 20; x < 30; ++x) left.pixels.set(x, y, {255, 255, 255, 255});
        Layer& right = g.add_layer("Right");
        right.depth = 1;
        right.pixels = Image(60, 60, {0, 0, 0, 0});
        for (int y = 20; y < 40; ++y) for (int x = 30; x < 40; ++x) right.pixels.set(x, y, {255, 255, 255, 255});
        const Image gc = g.composite();
        CHECK(gc.get(30, 30).r == 255 && gc.get(30, 30).g == 255);   // the seam between the members is not stroked
        CHECK(gc.get(41, 30).r == 255 && gc.get(41, 30).g == 0);     // the group's outline is
        CHECK(gc.get(5, 5).r == 128);
        // Partial recomposite matches the whole.
        g.take_dirty();
        g.layer(2).pixels.set(25, 25, {0, 0, 0, 255});
        g.touch({25, 25, 26, 26});
        Image part = gc;
        g.composite_into(part, g.take_dirty());
        const Image whole = g.composite();
        CHECK(std::memcmp(part.data(), whole.data(), whole.size_bytes()) == 0);
    }

    // Styles survive the native format through the stash.
    const std::vector<uint8_t> bytes = io::save_psp_to_memory(doc);
    std::string err;
    auto back = io::load_psp_from_memory(bytes.data(), bytes.size(), &err, nullptr);
    CHECK(back && back->layer_count() == 2 && back->layer(1).style == st);
    // JSON round trip is exact.
    CHECK(LayerStyle::from_json(st.to_json()) == st);
}

static void test_filter_layers() {
    // A blur filter layer softens the edge of what lies below it.
    Document doc(40, 40);
    Layer& bg = doc.add_layer("Background");
    bg.background = true;
    for (int y = 0; y < 40; ++y) for (int x = 0; x < 40; ++x) bg.pixels.set(x, y, x < 20 ? Color{255, 255, 255, 255} : Color{0, 0, 0, 255});
    Adjustment a;
    a.kind = Adjustment::Kind::GaussianBlur;
    a.blur_radius = 3.0f;
    CommandStack stack;
    stack.run(doc, std::make_unique<AddAdjustmentLayerCommand>("Blur", a));
    CHECK(doc.layer_count() == 2 && doc.layer(1).is_adjustment() && doc.layer(1).adjustment.is_filter());
    Image full = doc.composite();
    CHECK(full.get(19, 20).r > 60 && full.get(19, 20).r < 200);    // the edge is soft
    CHECK(full.get(2, 20).r == 255 && full.get(38, 20).r == 0);     // far from it, untouched
    // An edit below the filter dirties a rect grown by the filter's reach,
    // and recompositing only that rect reproduces the full composite.
    doc.take_dirty();
    for (int y = 8; y < 12; ++y) for (int x = 8; x < 12; ++x) doc.layer(0).pixels.set(x, y, {0, 0, 0, 255});
    doc.touch({8, 8, 12, 12});
    const raster::Rect dirty = doc.take_dirty();
    CHECK(dirty.x0 < 8 && dirty.y0 < 8 && dirty.x1 > 12 && dirty.y1 > 12);
    Image part = full;
    doc.composite_into(part, dirty);
    const Image again = doc.composite();
    CHECK(std::memcmp(part.data(), again.data(), again.size_bytes()) == 0);
    CHECK(again.get(10, 10).r < 200);   // the black square shows through the blur
    // A filter layer's mask rides through the classic format too, wrapped
    // the way a masked raster layer is.
    {
        Document md(40, 40);
        md.add_layer("Background").pixels.fill({90, 90, 90, 255});
        Layer& mf = md.add_layer("Masked Blur");
        mf.type = LayerType::Adjustment;
        mf.adjustment.kind = Adjustment::Kind::GaussianBlur;
        mf.adjustment.blur_radius = 3.0f;
        mf.mask = mask::rectangle(40, 40, 0, 0, 20, 40, false);
        const std::vector<uint8_t> mb = io::save_psp_to_memory(md);
        std::string merr;
        auto mback = io::load_psp_from_memory(mb.data(), mb.size(), &merr, nullptr);
        CHECK(mback && mback->layer_count() == 2);
        CHECK(mback->layer(1).is_adjustment() && mback->layer(1).adjustment.kind == Adjustment::Kind::GaussianBlur);
        CHECK(mback->layer(1).has_mask() && mback->layer(1).mask.at(5, 5) == 255 && mback->layer(1).mask.at(35, 5) == 0);
    }

    // The native format keeps the filter layer (as a placeholder plus the stash).
    const std::vector<uint8_t> bytes = io::save_psp_to_memory(doc);
    std::string err;
    auto back = io::load_psp_from_memory(bytes.data(), bytes.size(), &err, nullptr);
    CHECK(back && back->layer_count() == 2);
    CHECK(back->layer(1).is_adjustment() && back->layer(1).adjustment.kind == Adjustment::Kind::GaussianBlur);
    CHECK(std::abs(back->layer(1).adjustment.blur_radius - 3.0f) < 1e-4f);
    const Image rt = back->composite();
    CHECK(std::abs(rt.get(19, 20).r - again.get(19, 20).r) <= 2);
}

static void test_compound_and_mask_warp() {
    // A compound entry runs its parts in order and undoes them together.
    Document doc(20, 20);
    doc.add_layer("Background").pixels.fill({0, 0, 0, 255});
    CommandStack stack;
    Image after = doc.layer(0).pixels;
    after.set(3, 3, {255, 0, 0, 255});
    Mask sel = mask::rectangle(20, 20, 2, 2, 8, 8, false);
    std::vector<std::unique_ptr<Command>> parts;
    parts.push_back(std::make_unique<LayerSnapshotCommand>(0, "Deform", doc.layer(0).pixels, after));
    parts.push_back(std::make_unique<SelectionCommand>("Deform", sel));
    stack.run(doc, std::make_unique<CompoundCommand>("Deform", std::move(parts)));
    CHECK(doc.layer(0).pixels.get(3, 3).r == 255 && doc.selection().at(4, 4) == 255);
    stack.undo(doc);
    CHECK(doc.layer(0).pixels.get(3, 3).r == 0 && !doc.has_selection());
    stack.redo(doc);
    CHECK(doc.layer(0).pixels.get(3, 3).r == 255 && doc.selection().at(4, 4) == 255);
    // Warping a mask by a translation moves it.
    const float H[9] = {1, 0, 5, 0, 1, 0, 0, 0, 1};
    Mask moved = mask::warp(sel, H);
    CHECK(moved.at(12, 4) == 255 && moved.at(3, 4) == 0);
}

static void test_foreground_select() {
    // A noisy red disk on a noisy blue field: one scribble across the disk
    // and one on the field pick out the disk, edge included.
    const int W = 80, H = 80;
    Image img(W, H, {0, 0, 0, 255});
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const bool disk = (x - 40) * (x - 40) + (y - 40) * (y - 40) < 22 * 22;
            const uint8_t n = static_cast<uint8_t>((x * 7 + y * 13) % 30);
            img.set(x, y, disk ? Color{static_cast<uint8_t>(200 + n / 2), n, n, 255} : Color{n, n, static_cast<uint8_t>(180 + n), 255});
        }
    Mask fg(W, H, 0), bg(W, H, 0);
    for (int x = 30; x < 50; ++x) fg.at(x, 40) = 255;
    for (int y = 5; y < 75; ++y) bg.at(5, y) = 255;
    Mask sel = mask::foreground_select(img, fg, bg, Mask());
    CHECK(sel.width() == W && sel.height() == H);
    CHECK(sel.at(40, 25) == 255 && sel.at(52, 52) == 255);   // inside the disk, away from the scribble
    CHECK(sel.at(10, 10) == 0 && sel.at(70, 40) == 0);       // field
    CHECK(sel.at(40, 66) == 0 && sel.at(40, 60) == 255);     // edge lands within a pixel or two of the true rim
    // No background marks: the image border stands in.
    Mask sel2 = mask::foreground_select(img, fg, Mask(), Mask());
    CHECK(sel2.at(40, 25) == 255 && sel2.at(10, 10) == 0);
    // A rough selection as the region: everything outside it is background.
    Mask region = mask::rectangle(W, H, 0, 0, 40, 80, false);
    Mask sel3 = mask::foreground_select(img, fg, Mask(), region);
    CHECK(sel3.at(30, 40) == 255 && sel3.at(50, 40) == 0);
}

static void test_symmetry() {
    // Both: a stamp near one corner lands in all four quadrants of a 64x64 image.
    Image img(64, 64, {0, 0, 0, 0});
    raster::Brush b; b.size = 6; b.hardness = 1.0f;
    raster::Stroke st(img, b, {255, 0, 0, 255}, raster::StrokeMode::Paint);
    raster::Symmetry sym; sym.mode = raster::Symmetry::Mode::Both; sym.cx = 32; sym.cy = 32;
    st.set_symmetry(sym);
    st.add_point(10, 10);
    Image out = img;
    st.render(out);
    CHECK(out.get(10, 10).a == 255 && out.get(54, 10).a == 255 && out.get(10, 54).a == 255 && out.get(54, 54).a == 255);
    CHECK(out.get(32, 32).a == 0);
    // Rotational, 4 copies: (10, 32) maps onto the other three compass points.
    Image img2(64, 64, {0, 0, 0, 0});
    raster::Stroke st2(img2, b, {255, 0, 0, 255}, raster::StrokeMode::Paint);
    sym.mode = raster::Symmetry::Mode::Rotational; sym.count = 4;
    st2.set_symmetry(sym);
    st2.add_point(10, 32);
    Image out2 = img2;
    st2.render(out2);
    CHECK(out2.get(10, 32).a == 255 && out2.get(32, 10).a == 255 && out2.get(54, 32).a == 255 && out2.get(32, 54).a == 255);
    CHECK(out2.get(10, 10).a == 0);
    // Kaleidoscope doubles the copies; the ones on the mirror line coincide.
    raster::Symmetry kal; kal.mode = raster::Symmetry::Mode::Kaleidoscope; kal.cx = 32; kal.cy = 32; kal.count = 6;
    CHECK(kal.points(10, 20).size() == 12);
}

static void test_heal_and_color_to_alpha() {
    // Heal: a bright source patch onto a dark target keeps the target's tone at the rim.
    Image target(40, 40, Color{40, 40, 40, 255});
    Image src(40, 40, Color{200, 200, 200, 255});
    for (int y = 0; y < 40; ++y) for (int x = 0; x < 40; ++x) if ((x + y) % 4 == 0) src.set(x, y, {220, 220, 220, 255});  // texture
    std::vector<float> region(40 * 40, 0.0f);
    for (int y = 12; y < 28; ++y) for (int x = 12; x < 28; ++x) region[static_cast<size_t>(y) * 40 + x] = 1.0f;
    Image healed = target;
    raster::heal(healed, src, 0, 0, region, raster::Rect{10, 10, 30, 30});
    CHECK(healed.get(20, 20).r < 90 && healed.get(20, 20).r >= 30);   // pulled down to the target's darkness
    CHECK(healed.get(5, 5).r == 40);                                    // outside the region untouched
    Image cloned = target;
    raster::Brush b; b.size = 10; b.hardness = 1;
    raster::Stroke st(target, b, {0, 0, 0, 255}, raster::StrokeMode::Heal);
    st.set_clone_source(&src, 0, 0);
    st.add_point(20, 20);
    st.render(cloned);
    CHECK(cloned.get(20, 20).r < 90);
    // Color to alpha: white to transparency on a gray ramp.
    Image img(3, 1, Color{255, 255, 255, 255});
    img.set(1, 0, {128, 128, 128, 255});
    img.set(2, 0, {255, 0, 0, 255});
    raster::color_to_alpha(img, {255, 255, 255, 255});
    CHECK(img.get(0, 0).a == 0);
    CHECK(img.get(1, 0).a >= 126 && img.get(1, 0).a <= 129 && img.get(1, 0).r <= 2);   // half gray = black at half alpha
    CHECK(img.get(2, 0).a == 255 && img.get(2, 0).r == 255 && img.get(2, 0).g == 0);
}

static void test_stroke_pressure() {
    Image base(64, 64, Color{0, 0, 0, 0});
    raster::Brush b;
    b.size = 20.0f; b.hardness = 1.0f;
    raster::Stroke full(base, b, {255, 0, 0, 255}, raster::StrokeMode::Paint);
    full.set_pressure_response(true, true);
    full.add_point(32, 32, 1.0f);
    Image out_full = base;
    full.render(out_full);
    raster::Stroke light(base, b, {255, 0, 0, 255}, raster::StrokeMode::Paint);
    light.set_pressure_response(true, true);
    light.add_point(32, 32, 0.5f);
    Image out_light = base;
    light.render(out_light);
    // Full pressure reaches radius 10; half pressure stops near radius 5 and paints at half coverage.
    CHECK(out_full.get(41, 32).a == 255 && out_light.get(41, 32).a == 0);
    CHECK(out_light.get(33, 32).a > 100 && out_light.get(33, 32).a < 160);
    raster::Stroke none(base, b, {255, 0, 0, 255}, raster::StrokeMode::Paint);   // pressure ignored by default
    none.add_point(32, 32, 0.2f);
    Image out_none = base;
    none.render(out_none);
    CHECK(out_none.get(41, 32).a == 255);
}

static void test_webp_roundtrip() {
    Image img(9, 7);
    for (int y = 0; y < 7; ++y) for (int x = 0; x < 9; ++x) img.set(x, y, {static_cast<uint8_t>(x * 28), static_cast<uint8_t>(y * 36), 77, static_cast<uint8_t>(x == 4 ? 128 : 255)});
    const std::string path = tmp_path("firn_test.webp");
    std::string err;
    CHECK(io::save(img, path, &err, 100));   // lossless
    auto back = io::load(path, &err);
    CHECK(back && back->width() == 9 && back->height() == 7);
    if (back) { bool same = true; for (size_t i = 0; i < img.size_bytes(); ++i) same = same && img.data()[i] == back->data()[i]; CHECK(same); }
    CHECK(io::save(img, path, &err, 80));    // lossy still opens with the same size
    auto lossy = io::load(path, &err);
    CHECK(lossy && lossy->width() == 9 && lossy->get(4, 0).a > 100);
    auto doc = io::load_document(path, &err, nullptr);
    CHECK(doc && doc->width() == 9);
    // An ICC profile rides in the container's ICCP chunk.
    const std::vector<uint8_t> prof = icc::encode(icc::adobe_rgb(), "Adobe RGB (1998)");
    CHECK(io::embed_icc(path, prof, &err));
    CHECK(io::read_icc(path) == prof);
    auto tagged = io::load(path, &err);
    CHECK(tagged && tagged->width() == 9);
    std::remove(path.c_str());
}

static void test_psd_import() {
    // A 4x2 RGB 8-bit PSD: a group holding one layer, then a plain layer on top, raw channels.
    std::vector<uint8_t> d;
    auto u8 = [&](int v) { d.push_back(static_cast<uint8_t>(v)); };
    auto u16 = [&](int v) { u8(v >> 8); u8(v & 255); };
    auto u32 = [&](uint32_t v) { u8(v >> 24); u8((v >> 16) & 255); u8((v >> 8) & 255); u8(v & 255); };
    auto str = [&](const char* s) { for (const char* p = s; *p; ++p) u8(*p); };
    str("8BPS"); u16(1); for (int i = 0; i < 6; ++i) u8(0); u16(3); u32(2); u32(4); u16(8); u16(3);
    u32(0); u32(0);  // color mode data, image resources
    const size_t lm_at = d.size(); u32(0);   // layer and mask info length (patched)
    const size_t li_at = d.size(); u32(0);   // layer info length (patched)
    u16(3);  // three records: divider, member, group
    struct Rec { const char* name; int section; int x0, y0, x1, y1; int nch; };
    const Rec recs[3] = {{"</Layer group>", 3, 0, 0, 0, 0, 4}, {"inner", 0, 1, 0, 3, 2, 5}, {"Group A", 1, 0, 0, 0, 0, 4}};
    for (const Rec& r : recs) {
        u32(r.y0); u32(r.x0); u32(r.y1); u32(r.x1);
        u16(r.nch);
        const int w = r.x1 - r.x0, h = r.y1 - r.y0;
        const int ids[5] = {-1, 0, 1, 2, -2};
        for (int k = 0; k < r.nch; ++k) { u16(static_cast<uint16_t>(ids[k])); u32(2 + static_cast<uint32_t>(ids[k] == -2 ? 2 * 1 : w * h)); }
        str("8BIM"); str(r.section == 0 ? "mul " : "norm"); u8(r.section == 0 ? 128 : 255); u8(0); u8(0); u8(0);
        const size_t extra_at = d.size(); u32(0);
        if (r.section == 0) { u32(20); u32(0); u32(1); u32(1); u32(3); u8(0); u8(0); u16(0); }  // mask rect y0 0,x0 1,y1 1,x1 3, default 0
        else u32(0);
        u32(0);  // blending ranges
        const size_t nlen = std::strlen(r.name); u8(static_cast<int>(nlen)); str(r.name);
        for (size_t p = nlen + 1; p % 4; ++p) u8(0);
        if (r.section) { str("8BIM"); str("lsct"); u32(4); u32(static_cast<uint32_t>(r.section)); }
        const uint32_t extra_len = static_cast<uint32_t>(d.size() - extra_at - 4);
        d[extra_at] = extra_len >> 24; d[extra_at + 1] = (extra_len >> 16) & 255; d[extra_at + 2] = (extra_len >> 8) & 255; d[extra_at + 3] = extra_len & 255;
    }
    // Channel data in record order: divider (empty), inner (2x2: alpha, r, g, b, mask 2x1), group (empty).
    for (int k = 0; k < 4; ++k) u16(0);
    u16(0); for (int i = 0; i < 4; ++i) u8(255);           // alpha
    u16(0); u8(200); u8(200); u8(10); u8(10);               // red
    u16(0); u8(20); u8(20); u8(220); u8(220);               // green
    u16(0); for (int i = 0; i < 4; ++i) u8(40);             // blue
    u16(0); u8(255); u8(0);                                  // mask row: show, hide
    for (int k = 0; k < 4; ++k) u16(0);
    const uint32_t li_len = static_cast<uint32_t>(d.size() - li_at - 4);
    d[li_at] = li_len >> 24; d[li_at + 1] = (li_len >> 16) & 255; d[li_at + 2] = (li_len >> 8) & 255; d[li_at + 3] = li_len & 255;
    const uint32_t lm_len = static_cast<uint32_t>(d.size() - lm_at - 4);
    d[lm_at] = lm_len >> 24; d[lm_at + 1] = (lm_len >> 16) & 255; d[lm_at + 2] = (lm_len >> 8) & 255; d[lm_at + 3] = lm_len & 255;
    u16(0); for (int i = 0; i < 3 * 8; ++i) u8(99);  // merged image, unused
    const std::string tmp = tmp_path("firn_test.psd");
    { std::ofstream f(tmp, std::ios::binary); f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size())); }
    std::string err; std::vector<std::string> warnings;
    auto doc = io::load_document(tmp, &err, &warnings);
    std::remove(tmp.c_str());
    CHECK(doc != nullptr);
    if (!doc) { std::printf("  psd: %s\n", err.c_str()); return; }
    CHECK(doc->width() == 4 && doc->height() == 2 && doc->layer_count() == 2);
    CHECK(doc->layer(0).type == LayerType::Group && doc->layer(0).name == "Group A" && doc->layer(0).depth == 0);
    const Layer& L = doc->layer(1);
    CHECK(L.name == "inner" && L.depth == 1 && L.blend == BlendMode::Multiply && std::abs(L.opacity - 128.0f / 255.0f) < 0.01f);
    CHECK(L.pixels.get(1, 0).r == 200 && L.pixels.get(2, 1).g == 220 && L.pixels.get(1, 0).a == 255 && L.pixels.get(0, 0).a == 0);
    CHECK(L.has_mask() && L.mask.at(1, 0) == 255 && L.mask.at(2, 0) == 0 && L.mask.at(0, 1) == 0);
}

static void test_snapshot_crop_and_undo_budget() {
    Document doc(40, 30);
    Layer& L = doc.add_layer("L");
    L.pixels = Image(40, 30, Color{10, 20, 30, 255});
    Image before = L.pixels;
    Image after = before;
    after.set(5, 7, {200, 0, 0, 255});
    after.set(9, 12, {0, 200, 0, 255});
    auto cmd = std::make_unique<LayerSnapshotCommand>(0, "Dab", before, after);
    CHECK(cmd->rect().x0 == 5 && cmd->rect().y0 == 7 && cmd->rect().x1 == 10 && cmd->rect().y1 == 13);
    CHECK(cmd->memory_bytes() == 2u * 5 * 6 * 4);   // two 5x6 crops
    CommandStack stack;
    stack.run(doc, std::move(cmd));
    CHECK(doc.layer(0).pixels.get(5, 7).r == 200 && doc.layer(0).pixels.get(9, 12).g == 200 && doc.layer(0).pixels.get(0, 0).b == 30);
    stack.undo(doc);
    CHECK(doc.layer(0).pixels.get(5, 7).r == 10 && doc.layer(0).pixels.get(9, 12).g == 20);
    stack.redo(doc);
    CHECK(doc.layer(0).pixels.get(5, 7).r == 200);
    // Memory budget: the oldest applied entries go first, the newest stays.
    for (int i = 0; i < 4; ++i) {
        Image b = doc.layer(0).pixels, a = b;
        a.set(i, 0, {static_cast<uint8_t>(100 + i), 0, 0, 255});
        stack.run(doc, std::make_unique<LayerSnapshotCommand>(0, "Dab", b, a));
    }
    CHECK(stack.size() == 5 && stack.memory_bytes() > 0);
    stack.set_memory_limit(1);
    CHECK(stack.size() == 1 && stack.can_undo());
    stack.undo(doc);
    CHECK(doc.layer(0).pixels.get(3, 0).r == 200 || doc.layer(0).pixels.get(3, 0).r == 10);
}

static void test_selection_modify_ops() {
    // Specks and holes: a 1-px speck and a 1-px hole in a 20x20 mask.
    Mask m(20, 20, 0);
    for (int y = 4; y < 16; ++y) for (int x = 4; x < 16; ++x) m.at(x, y) = 255;
    m.at(10, 10) = 0;   // hole
    m.at(1, 1) = 255;   // speck
    mask::remove_specks_and_holes(m, 2, 2);
    CHECK(m.at(1, 1) == 0 && m.at(10, 10) == 255 && m.at(4, 4) == 255 && m.at(0, 0) == 0);
    // Unfeather and inside/outside feather.
    Mask f = m;
    mask::feather_inside(f, 2.0f);
    CHECK(f.at(2, 2) == 0 && f.at(4, 4) < 255 && f.at(10, 10) == 255);
    Mask g = m;
    mask::feather_outside(g, 2.0f);
    CHECK(g.at(3, 10) > 0 && g.at(4, 4) == 255);
    mask::unfeather(g);
    CHECK(g.at(3, 10) == 255 || g.at(3, 10) == 0);
    Mask s = m;
    mask::smooth(s, 2, false);
    CHECK(s.at(10, 10) == 255 && s.at(0, 0) == 0);
    Mask aa = m;
    mask::shape_antialias(aa, true, true);
    CHECK(aa.at(4, 4) < 255 && aa.at(10, 10) == 255);
    // Color range and select similar.
    Image img(4, 1, Color{255, 0, 0, 255});
    img.set(1, 0, {250, 5, 0, 255});
    img.set(2, 0, {0, 0, 255, 255});
    img.set(3, 0, {128, 0, 0, 255});
    Mask cr = mask::select_color_range(img, {255, 0, 0, 255}, 10, 0);
    CHECK(cr.at(0, 0) == 255 && cr.at(1, 0) == 255 && cr.at(2, 0) == 0 && cr.at(3, 0) == 0);
    Mask seed(4, 1, 0);
    seed.at(0, 0) = 255;
    Mask sim = mask::select_similar(img, seed, 16);
    CHECK(sim.at(0, 0) == 255 && sim.at(1, 0) == 255 && sim.at(2, 0) == 0 && sim.at(3, 0) == 0);
    // Edge helpers: a vertical edge at x = 5 in a 12x12 image.
    Image e(12, 12, Color{0, 0, 0, 255});
    for (int y = 0; y < 12; ++y) for (int x = 5; x < 12; ++x) e.set(x, y, {255, 255, 255, 255});
    auto edges = mask::edge_map(e);
    auto snapped = mask::seek_edge(edges, 12, 12, 2.0f, 6.0f, 4);
    CHECK(std::abs(snapped.first - 5.0f) < 1.5f);
    auto path = mask::edge_path(edges, 12, 12, {4.5f, 1.5f}, {4.5f, 10.5f});
    CHECK(path.size() >= 9 && std::abs(path[path.size() / 2].first - 4.5f) < 1.5f);
    auto sp = mask::smooth_polygon({{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f}, {0.0f, 10.0f}}, 50, true);
    CHECK(sp.size() == 4 && sp[0].first > 0.0f);
    // Matting.
    Image mt(2, 1, Color{0, 0, 0, 0});
    mt.set(0, 0, {128, 0, 0, 128});   // red over black at 50%
    raster::remove_matte(mt, {0, 0, 0, 255});
    CHECK(mt.get(0, 0).r == 255 && mt.get(0, 0).a == 128);
    Image df(3, 1, Color{0, 200, 0, 255});
    df.set(1, 0, {255, 255, 255, 100});
    df.set(2, 0, {0, 0, 0, 0});
    raster::defringe(df, 1);
    CHECK(df.get(1, 0).g == 200 && df.get(1, 0).a == 100 && df.get(2, 0).a == 0);
}

static void test_material_texture_and_gradient_file() {
    // A checker texture halves the paint where it is black.
    auto tex = std::make_shared<Image>(2, 1, Color{255, 255, 255, 255});
    tex->set(1, 0, {0, 0, 0, 255});
    vec::PaintStyle st;
    st.kind = vec::PaintStyle::Kind::Solid;
    st.color = {200, 0, 0, 255};
    st.texture = tex;
    CHECK(vec::texture_factor(st, 0.5f, 0.5f, 0, 0) == 1.0f && vec::texture_factor(st, 1.5f, 0.5f, 0, 0) == 0.0f);
    st.texture_strength = 0.5f;
    CHECK(std::abs(vec::texture_factor(st, 1.5f, 0.5f, 0, 0) - 0.5f) < 1e-5f);
    Image out(2, 1, Color{0, 0, 0, 0});
    std::vector<uint8_t> cov(2, 255);
    vec::paint(out, cov, 2, 1, st, 0, 0, 2, 1);
    CHECK(out.get(0, 0).a == 255 && out.get(1, 0).a == 128);

    // Gradient files round-trip through the writer.
    vec::Gradient g;
    g.name = "Firn test";
    g.colors = {{{10, 20, 30, 255}, 0, 50}, {{255, 128, 0, 255}, 40, 25}, {{0, 0, 255, 255}, 100, 50}};
    g.opacities = {{100, 0, 50}, {30, 100, 50}};
    const std::string path = tmp_path("firn_test.PspGradient");
    CHECK(io::save_gradients({g}, path));
    auto back = io::load_gradients(path);
    std::remove(path.c_str());
    CHECK(back.size() == 1 && back[0].name == "Firn test" && back[0].colors.size() == 3 && back[0].opacities.size() == 2);
    CHECK(back[0].colors[1].color.r == 255 && back[0].colors[1].color.g == 128 && std::abs(back[0].colors[1].pos - 40.0f) < 0.1f && std::abs(back[0].colors[1].mid - 25.0f) < 0.5f);
    CHECK(std::abs(back[0].opacities[1].opacity - 30.0f) < 0.5f);
}

static void test_vector_roundtrip() {
    Document d(40, 30);
    d.add_layer("bg").background = true;
    Layer& v = d.add_layer("Vector 1");
    v.type = LayerType::Vector;
    vec::Object rect = vec::make_rectangle(2, 2, 20, 12);
    rect.stroke.kind = vec::PaintStyle::Kind::Solid; rect.stroke.color = {10, 20, 30, 255}; rect.stroke_width = 3;
    rect.fill.kind = vec::PaintStyle::Kind::Gradient;
    rect.fill.gradient.colors = {{{255, 0, 0, 255}, 0, 50}, {{0, 0, 255, 255}, 100, 40}};
    rect.fill.gradient.style = vec::GradientStyle::Radial;
    rect.fill.gradient.angle = 45; rect.fill.gradient.center_x = 30; rect.fill.gradient.center_y = 60;
    vec::Object ell = vec::make_ellipse(25, 20, 8, 6);
    ell.fill.kind = vec::PaintStyle::Kind::Solid; ell.fill.color = {0, 255, 0, 255}; ell.antialias = false;
    v.objects = {rect, ell};
    d.rasterize_vector_layer(1);
    std::vector<uint8_t> file = io::save_psp_to_memory(d);
    std::string err;
    std::vector<std::string> warnings;
    auto back = io::load_psp_from_memory(file.data(), file.size(), &err, &warnings);
    CHECK(back && warnings.empty() && back->layer_count() == 2 && back->layer(1).is_vector());
    const auto& objs = back->layer(1).objects;
    CHECK(objs.size() == 2 && objs[0].name == "Rectangle" && objs[1].name == "Ellipse");
    CHECK(objs[0].paths.size() == 1 && objs[0].paths[0].nodes.size() == 4 && objs[0].paths[0].closed);
    CHECK(objs[0].paths[0].nodes[0].x == 2 && objs[0].paths[0].nodes[0].y == 12);
    CHECK(objs[0].stroke.kind == vec::PaintStyle::Kind::Solid && objs[0].stroke.color.g == 20 && objs[0].stroke_width == 3);
    CHECK(objs[0].fill.kind == vec::PaintStyle::Kind::Gradient && objs[0].fill.gradient.style == vec::GradientStyle::Radial);
    CHECK(objs[0].fill.gradient.opacities.size() == 2 && objs[0].fill.gradient.repeats == 0);
    CHECK(objs[0].fill.gradient.colors.size() == 2 && objs[0].fill.gradient.colors[1].color.b == 255 && objs[0].fill.gradient.colors[1].mid == 40);
    CHECK(objs[0].fill.gradient.angle == 45 && objs[0].fill.gradient.center_x == 30 && objs[0].fill.gradient.center_y == 60);
    CHECK(objs[1].fill.color.g == 255 && !objs[1].antialias && objs[1].paths[0].nodes[1].in_y != objs[1].paths[0].nodes[1].y);
    // The rendered cache matches the original rendering.
    CHECK(back->layer(1).pixels.get(25, 20).g == 255 && back->layer(1).pixels.get(3, 3).a > 0);

    // Sample libraries, when the backup is present.
    auto shapes = io::load_preset_shapes(std::string(FIRN_SOURCE_DIR) + "/WindowsInstall/Preset Shapes/Star 1.pspshape");
    if (!shapes.empty()) CHECK(shapes[0].paths[0].nodes.size() == 8 && shapes[0].stroke.kind == vec::PaintStyle::Kind::Solid);
    auto grads = io::load_gradients(std::string(FIRN_SOURCE_DIR) + "/WindowsInstall/Gradients/Black-white.PspGradient");
    if (!grads.empty()) {
        CHECK(grads[0].name == "Black-white" && grads[0].colors.size() == 3);
        CHECK(grads[0].colors[0].color.r == 0 && grads[0].colors[2].color.r == 255 && grads[0].colors[2].pos == 100);
    }
    // Every shipped gradient file parses to sane stops (name padding varies between files).
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        int files = 0;
        for (const auto& de : fs::recursive_directory_iterator(std::string(FIRN_SOURCE_DIR) + "/WindowsInstall/Gradients", fs::directory_options::skip_permission_denied, ec)) {
            if (!de.is_regular_file(ec) || de.path().extension() != ".PspGradient") continue;
            ++files;
            const auto gs = io::load_gradients(de.path().string());
            CHECK(gs.size() == 1 && gs[0].colors.size() >= 2 && !gs[0].opacities.empty());
            for (const auto& c : gs[0].colors) CHECK(c.pos >= 0 && c.pos <= 100 && c.mid >= 1 && c.mid <= 99);
            for (const auto& o : gs[0].opacities) CHECK(o.pos >= 0 && o.pos <= 100 && o.opacity >= 0 && o.opacity <= 100);
        }
        (void)files;
    }
    auto line = io::load_styled_line(std::string(FIRN_SOURCE_DIR) + "/WindowsInstall/Styled Lines/Dashed Lines/Dashed.PspStyledLine");
    if (line) {
        CHECK(line->name == "Dashed" && line->dashes.size() == 2 && line->dashes[0] == 24 && line->dashes[1] == 12);
        CHECK(line->miter == 2.0f && line->first_cap == 0);
        // A dashed stroke covers less than a solid one, and caps add to it.
        vec::Object solid;
        vec::Path path;
        vec::Node n0; n0.x = n0.in_x = n0.out_x = 10; n0.y = n0.in_y = n0.out_y = 10; n0.flags[0] = 1;
        vec::Node n1 = n0; n1.x = n1.in_x = n1.out_x = 90; n1.flags[0] = 0;
        path.nodes = {n0, n1};
        path.closed = false;
        solid.paths.push_back(path);
        solid.fill.kind = vec::PaintStyle::Kind::None;
        solid.stroke.kind = vec::PaintStyle::Kind::Solid; solid.stroke.color = {0, 0, 0, 255};
        solid.stroke_width = 2;
        vec::Object dashed = solid; dashed.line = *line;
        vec::Object capped = solid; capped.line.last_cap = 4; capped.line.last_w = 4; capped.line.last_h = 4;
        auto covered = [](const vec::Object& o) {
            Image img(100, 20);
            vec::rasterize({o}, img);
            long n = 0;
            for (int y = 0; y < 20; ++y) for (int x = 0; x < 100; ++x) n += img.get(x, y).a;
            return n;
        };
        const long a = covered(solid), b = covered(dashed), c = covered(capped);
        CHECK(b < a && b > 0);
        CHECK(c > a);
        // Styled line files round-trip through the writer.
        const std::string tmp = tmp_path("firn_test_line.PspStyledLine");
        CHECK(io::save_styled_line(*line, tmp));
        auto back = io::load_styled_line(tmp);
        CHECK(back && back->dashes == line->dashes && back->first_w == line->first_w && back->miter == line->miter);
        std::remove(tmp.c_str());
    }
}

// A fresh object must serialize with the same attribute and line style
// bytes the original writes for its own defaults: the original hangs on a
// line style block whose sizes sit at the wrong offsets.
static void test_vector_default_bytes() {
    Document doc(64, 64);
    Layer& L = doc.add_layer("Vector 1");
    L.type = LayerType::Vector;
    L.pixels = Image(64, 64, {0, 0, 0, 0});
    vec::Object o = vec::make_rectangle(10, 10, 40, 30);
    o.stroke.kind = vec::PaintStyle::Kind::Solid;
    o.fill.kind = vec::PaintStyle::Kind::Solid;
    L.objects.push_back(o);
    const std::string tmp = tmp_path("firn_test_defaults.pspimage");
    CHECK(io::save_psp(doc, tmp, nullptr));
    std::ifstream f(tmp, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::remove(tmp.c_str());
    // Attribute chunk: length 60, flags 1 1 1, width 1.0, then the cap records
    // (01 00 1.0 1.0) x2, 00, miter 10.0 as in the shipped preset shapes.
    const std::vector<uint8_t> attr = {0x3c, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f,
                                       0, 0, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f,
                                       0, 0, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f,
                                       0, 0, 0, 0, 0, 0, 0, 0x24, 0x40};
    CHECK(std::search(d.begin(), d.end(), attr.begin(), attr.end()) != d.end());
    // Line style chunk (45): u16 0, f64 1.0, f64 1.0, u16 0, f64 1.0, f64 1.0, 5 x 00.
    const std::vector<uint8_t> line = {0x2d, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f,
                                       0, 0, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f, 0, 0, 0, 0, 0};
    CHECK(line.size() == 45);
    CHECK(std::search(d.begin(), d.end(), line.begin(), line.end()) != d.end());
}

static void test_vector_queries() {
    using namespace vec;
    Object r = make_rectangle(10, 10, 50, 30);
    r.fill.kind = PaintStyle::Kind::Solid;
    r.stroke.kind = PaintStyle::Kind::None;
    float x0, y0, x1, y1;
    CHECK(outline_bounds(r, &x0, &y0, &x1, &y1) && x0 == 10 && y0 == 10 && x1 == 50 && y1 == 30);
    CHECK(hit_test(r, 30, 20, 1));          // inside a filled shape
    CHECK(!hit_test(r, 60, 20, 1));         // outside
    r.fill.kind = PaintStyle::Kind::None;
    r.stroke.kind = PaintStyle::Kind::Solid;
    r.stroke_width = 2;
    CHECK(!hit_test(r, 30, 20, 1));         // hollow: the interior no longer hits
    CHECK(hit_test(r, 30, 10.5f, 1));       // but the outline does
    // Groups: a group object precedes its members; nested groups count as one member.
    std::vector<Object> objs;
    Object g; g.is_group = true; g.group_count = 2;
    Object inner; inner.is_group = true; inner.group_count = 1;
    objs.push_back(g); objs.push_back(make_rectangle(0, 0, 1, 1)); objs.push_back(inner); objs.push_back(make_rectangle(0, 0, 2, 2)); objs.push_back(make_rectangle(5, 5, 6, 6));
    CHECK(group_end(objs, 0) == 4);
    CHECK(group_end(objs, 2) == 4);
    CHECK(group_end(objs, 4) == 5);
    CHECK(group_of(objs, 1) == 0);
    CHECK(group_of(objs, 3) == 2);          // innermost
    CHECK(group_of(objs, 4) == -1);
    // Text outlines: glyph contours are closed cubic paths tagged by glyph.
    const auto fonts = text::list_fonts();
    if (!fonts.empty()) {
        auto font = text::Font::load(fonts[0].path);
        std::vector<int> ids;
        const auto contours = font->outlines("Ab", 40, text::Font::Align::Left, 1.0f, 0.0f, nullptr, &ids);
        CHECK(contours.size() >= 2 && ids.size() == contours.size());
        CHECK(ids.front() == 0 && ids.back() == 1);
        for (const auto& c : contours) CHECK(c.size() >= 3);
    }
}

static void test_adjustment_layers() {
    Document doc(8, 8);
    Layer& base = doc.add_layer("Background");
    base.pixels = Image(8, 8, {100, 100, 100, 255});
    doc.set_active_layer(0);
    CommandStack stack;
    Adjustment a;
    a.kind = Adjustment::Kind::Invert;
    stack.run(doc, std::make_unique<AddAdjustmentLayerCommand>("Invert", a));
    CHECK(doc.layer_count() == 2 && doc.layer(1).is_adjustment());
    Image c = doc.composite();
    CHECK(c.get(3, 3).r == 155 && c.get(3, 3).a == 255);
    // Opacity and mask scale the effect.
    doc.layer(1).opacity = 0.5f; doc.touch();
    c = doc.composite();
    CHECK(std::abs(static_cast<int>(c.get(3, 3).r) - 128) <= 1);
    doc.layer(1).opacity = 1.0f;
    doc.layer(1).mask = Mask(8, 8, 0);
    doc.layer(1).mask.at(0, 0) = 255;
    doc.touch();
    c = doc.composite();
    CHECK(c.get(0, 0).r == 155 && c.get(3, 3).r == 100);
    doc.layer(1).mask = Mask();
    // Editing through the command, undo restores.
    Adjustment b = a;
    b.kind = Adjustment::Kind::BrightnessContrast;
    b.brightness = 50;
    stack.run(doc, std::make_unique<SetAdjustmentCommand>(1, a, b));
    CHECK(doc.layer(1).adjustment.kind == Adjustment::Kind::BrightnessContrast);
    CHECK(doc.composite().get(3, 3).r > 100);
    stack.undo(doc);
    CHECK(doc.composite().get(3, 3).r == 155);
    stack.undo(doc);
    CHECK(doc.layer_count() == 1);
    // Every kind round-trips through the native format with its parameters.
    using K = Adjustment::Kind;
    for (K k : {K::Levels, K::Curves, K::BrightnessContrast, K::ColorBalance, K::HSL, K::ChannelMixer, K::Invert, K::Threshold, K::Posterize}) {
        Document d(8, 8);
        d.add_layer("Background").pixels = Image(8, 8, {100, 100, 100, 255});
        Layer& L = d.add_layer(Adjustment::kind_name(k));
        L.type = LayerType::Adjustment;
        Adjustment& x = L.adjustment;
        x.kind = k;
        x.brightness = -20; x.contrast = 30;
        x.levels[0].gamma = 1.5f; x.levels[0].in_low = 10; x.levels[2].out_high = 200;
        x.curves[1] = {{0.0f, 0.0f}, {128.0f, 200.0f}, {255.0f, 255.0f}};
        x.color_balance.midtones[0] = 40; x.color_balance.shadows[2] = -15; x.color_balance.preserve_luminosity = false;
        x.hue = 30; x.saturation = -10; x.lightness = 5; x.hsl_ranges[2][3] = 77;
        x.mixer.mix[0][1] = 50; x.mixer.constant[2] = -10; x.mixer.monochrome = true;
        x.threshold = 90; x.posterize = 4;
        L.mask = Mask(8, 8, 128);
        const std::string tmp = tmp_path("firn_test_adj.pspimage");
        CHECK(io::save_psp(d, tmp, nullptr));
        std::string err; std::vector<std::string> warnings;
        auto back = io::load_psp(tmp, &err, &warnings);
        std::remove(tmp.c_str());
        CHECK(back && back->layer_count() == 2 && back->layer(1).is_adjustment());
        const Adjustment& y = back->layer(1).adjustment;
        CHECK(y.kind == k);
        CHECK(back->layer(1).has_mask() && back->layer(1).mask.at(2, 2) == 128);
        switch (k) {
            case K::BrightnessContrast: CHECK(y.brightness == -20 && y.contrast == 30); break;
            case K::Levels: CHECK(y.levels[0].gamma == 1.5f && y.levels[0].in_low == 10 && y.levels[2].out_high == 200); break;
            case K::Curves: CHECK(y.curves[1].size() == 3 && y.curves[1][1].second == 200); break;
            case K::ColorBalance: CHECK(y.color_balance.midtones[0] == 40 && y.color_balance.shadows[2] == -15 && !y.color_balance.preserve_luminosity); break;
            case K::HSL: CHECK(y.hue == 30 && y.saturation == -10 && y.lightness == 5 && y.hsl_ranges[2][3] == 77); break;
            case K::ChannelMixer: CHECK(y.mixer.mix[0][1] == 50 && y.mixer.constant[2] == -10 && y.mixer.monochrome); break;
            case K::Threshold: CHECK(y.threshold == 90); break;
            case K::Posterize: CHECK(y.posterize == 4); break;
            default: break;
        }
        // The rendering survives too.
        Image before = d.composite(), after = back->composite();
        CHECK(before.get(3, 3).r == after.get(3, 3).r);
    }
}

static void test_warp() {
    // A homography through four corners maps them exactly; a pure translation warps pixels intact.
    raster::Quad from, to;
    const float fx[4] = {0, 10, 10, 0}, fy[4] = {0, 0, 10, 10};
    const float tx[4] = {5, 25, 30, 2}, ty[4] = {3, 4, 20, 22};
    for (int i = 0; i < 4; ++i) { from.x[i] = fx[i]; from.y[i] = fy[i]; to.x[i] = tx[i]; to.y[i] = ty[i]; }
    float H[9];
    CHECK(raster::homography(from, to, H));
    for (int i = 0; i < 4; ++i) {
        float ox, oy;
        raster::apply_homography(H, fx[i], fy[i], &ox, &oy);
        CHECK(std::abs(ox - tx[i]) < 1e-3f && std::abs(oy - ty[i]) < 1e-3f);
    }
    float inv[9];
    CHECK(raster::invert3(H, inv));
    float bx, by;
    raster::apply_homography(inv, tx[2], ty[2], &bx, &by);
    CHECK(std::abs(bx - 10) < 1e-3f && std::abs(by - 10) < 1e-3f);
    Image img(20, 20, {0, 0, 0, 0});
    for (int y = 2; y < 6; ++y) for (int x = 3; x < 8; ++x) img.set(x, y, {200, 100, 50, 255});
    const raster::Rect cb = raster::content_bounds(img);
    CHECK(cb.x0 == 3 && cb.y0 == 2 && cb.x1 == 8 && cb.y1 == 6);
    const float T[9] = {1, 0, 4, 0, 1, 6, 0, 0, 1};
    Image moved = raster::warp(img, T, 20, 20);
    CHECK(moved.get(7, 8).r == 200 && moved.get(7, 8).a == 255 && moved.get(3, 2).a == 0);
    // The command warps every layer and can crop.
    Document doc(20, 20);
    doc.add_layer("a").pixels = img;
    doc.add_layer("b").pixels = img;
    CommandStack stack;
    stack.run(doc, std::make_unique<WarpLayersCommand>("Warp", T, -1, raster::Rect{4, 6, 20, 20}, Color{0, 0, 0, 255}));
    CHECK(doc.width() == 16 && doc.height() == 14);
    CHECK(doc.layer(0).pixels.get(3, 2).r == 200 && doc.layer(1).pixels.get(3, 2).r == 200);
    stack.undo(doc);
    CHECK(doc.width() == 20 && doc.layer(1).pixels.get(3, 2).r == 200 && doc.layer(1).pixels.get(7, 8).a == 0);
}

static void test_color_ops() {
    Image img(8, 8, {255, 0, 0, 255});
    for (int x = 0; x < 4; ++x) for (int y = 0; y < 8; ++y) img.set(x, y, {0, 0, 255, 255});
    img.set(7, 7, {10, 200, 30, 0});   // transparent pixels do not count
    CHECK(raster::count_colors(img) == 2);
    auto pal = raster::median_cut_palette(img, 2);
    CHECK(pal.size() == 2);
    Image q = img;
    raster::apply_palette(q, pal, false);
    CHECK(raster::count_colors(q) == 2 && q.get(0, 0).b > 200 && q.get(7, 0).r > 200);
    Image mono(4, 1, {128, 128, 128, 255});
    raster::to_monochrome(mono, true);
    int white = 0;
    for (int x = 0; x < 4; ++x) { CHECK(mono.get(x, 0).r == 0 || mono.get(x, 0).r == 255); if (mono.get(x, 0).r == 255) ++white; }
    CHECK(white == 2);   // dithered mid-gray alternates
    // Split and combine are inverses for RGB and CMYK, and close for HSL.
    Image src(3, 2);
    src.set(0, 0, {10, 20, 30, 255}); src.set(1, 0, {200, 100, 50, 255}); src.set(2, 0, {0, 255, 0, 255});
    src.set(0, 1, {255, 255, 255, 255}); src.set(1, 1, {0, 0, 0, 255}); src.set(2, 1, {123, 45, 67, 255});
    for (int mode : {0, 1, 2}) {
        auto planes = raster::split_channels(src, mode);
        CHECK(planes.size() == (mode == 2 ? 4u : 3u));
        Image back = raster::combine_channels(planes, mode);
        for (int y = 0; y < 2; ++y) for (int x = 0; x < 3; ++x) {
            const Color a = src.get(x, y), b = back.get(x, y);
            const int tol = mode == 1 ? 6 : 2;
            CHECK(std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol);
        }
    }
    Image a(2, 1, {100, 100, 100, 255}), b(2, 1, {200, 50, 0, 255});
    Image sum = raster::arithmetic(a, b, raster::ArithOp::Add, 1, 0, true, 0);
    CHECK(sum.get(0, 0).r == 255 && sum.get(0, 0).g == 150 && sum.get(0, 0).b == 100);
    Image wrapped = raster::arithmetic(a, b, raster::ArithOp::Add, 1, 0, false, 0);
    CHECK(wrapped.get(0, 0).r == 44);
    Image diff = raster::arithmetic(a, b, raster::ArithOp::Difference, 2, 10, true, 1);
    CHECK(diff.get(0, 0).r == 60 && diff.get(0, 0).g == 100);
    const std::string tmp = tmp_path("firn_test.PspPalette");
    CHECK(io::save_palette(pal, tmp));
    auto loaded = io::load_palette(tmp);
    std::remove(tmp.c_str());
    CHECK(loaded.size() == pal.size() && loaded[0].r == pal[0].r && loaded[1].b == pal[1].b);
    const auto shipped = io::load_palette(std::string(FIRN_SOURCE_DIR) + "/WindowsInstall/Palettes/Safety.PspPalette");
    CHECK(shipped.empty() || shipped.size() == 256);
}

static void test_photo_fix_suite() {
    // A dark, blue-tinted, low-contrast image.
    Image img(16, 16);
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) img.set(x, y, {static_cast<uint8_t>(40 + x * 2), static_cast<uint8_t>(50 + x * 2), static_cast<uint8_t>(90 + x * 2), 255});
    // Cast removal is opt-in (the original's factory preset leaves it off).
    Image a = img; photo::auto_color_balance(a, 100, 6500, true);
    CHECK(std::abs(a.get(8, 8).r - a.get(8, 8).b) < std::abs(img.get(8, 8).r - img.get(8, 8).b));   // less blue cast
    Image keep = img; photo::auto_color_balance(keep, 100, 6500, false);
    CHECK(keep.get(8, 8).b - keep.get(8, 8).r == img.get(8, 8).b - img.get(8, 8).r);                // colors left alone
    Image c = img; photo::auto_contrast_enhance(c, 1, 0, 1);
    CHECK(c.get(15, 0).r - c.get(0, 0).r > img.get(15, 0).r - img.get(0, 0).r);                      // more contrast
    Image sat = img; photo::auto_saturation(sat, 2, 2, false);
    CHECK(sat.get(8, 8).b - sat.get(8, 8).r > img.get(8, 8).b - img.get(8, 8).r);                    // more saturated
    Image ff = img; photo::fill_flash(ff, 100);
    CHECK(ff.get(0, 0).r > img.get(0, 0).r);                                                       // shadows lifted
    Image bl(2, 1, {240, 240, 240, 255}); photo::backlighting(bl, 100);
    CHECK(bl.get(0, 0).r < 240);                                                                   // highlights lowered
    Image bw(2, 1); bw.set(0, 0, {20, 20, 20, 255}); bw.set(1, 0, {200, 200, 200, 255});
    photo::black_white_points(bw, {20, 20, 20, 255}, {200, 200, 200, 255}, {0, 0, 0, 255}, {255, 255, 255, 255});
    CHECK(bw.get(0, 0).r == 0 && bw.get(1, 0).r == 255);
    Image ha = img; photo::histogram_adjust(ha, 0, 0, 1.0f, 0, 0);
    CHECK(ha.get(15, 0).r >= img.get(15, 0).r);
    // Salt and pepper: a lone white pixel on a gray field disappears; the field survives.
    Image sp(9, 9, {100, 100, 100, 255}); sp.set(4, 4, {255, 255, 255, 255});
    photo::salt_and_pepper(sp, 3, 20, true, false);
    CHECK(sp.get(4, 4).r < 130 && sp.get(0, 0).r == 100);
    Image nr = sp; nr.set(2, 2, {130, 100, 100, 255}); photo::noise_removal(nr, 80, 100, 0);
    CHECK(std::abs(nr.get(2, 2).r - 100) < 30);
    Image ja(8, 8, {100, 100, 100, 255}); for (int x = 4; x < 8; ++x) for (int y = 0; y < 8; ++y) ja.set(x, y, {200, 200, 200, 255});
    photo::jpeg_artifact_removal(ja, 1, 0);
    CHECK(ja.get(0, 0).r == 100 && ja.get(7, 7).r == 200);                                          // an edge stays an edge
    Image ca(9, 9, {50, 60, 70, 255}); photo::chromatic_aberration(ca, 2.0f, -2.0f);
    CHECK(ca.get(4, 4).r == 50 && ca.get(4, 4).g == 60 && ca.get(4, 4).b == 70);                     // center untouched
    Image cl = img; photo::clarify(cl, 3);
    CHECK(cl.get(8, 8).a == 255);
    Image osf = img; photo::one_step_photo_fix(osf);
    CHECK(osf.get(8, 8).a == 255 && osf.get(15, 0).r != img.get(15, 0).r);
}

static void test_mesh_and_displace() {
    Image img(16, 16, {0, 0, 0, 0});
    for (int y = 4; y < 12; ++y) for (int x = 4; x < 12; ++x) img.set(x, y, {200, 100, 50, 255});
    // Identity mesh reproduces the image.
    std::vector<std::pair<float, float>> nodes;
    for (int r = 0; r <= 2; ++r) for (int c = 0; c <= 2; ++c) nodes.emplace_back(8.0f * c, 8.0f * r);
    Image same = raster::mesh_warp(img, 2, 2, nodes);
    CHECK(same.get(6, 6).r == 200 && same.get(2, 2).a == 0 && same.get(11, 11).a == 255);
    // Moving the center node right shifts content near it.
    nodes[4] = {12.0f, 8.0f};
    Image moved = raster::mesh_warp(img, 2, 2, nodes);
    CHECK(moved.get(13, 8).a == 255 && moved.get(4, 8).a == 0);
    // A constant displacement is a translation.
    std::vector<float> dx(256, 2.0f), dy(256, 0.0f);
    Image shifted = raster::displace(img, dx, dy);
    CHECK(shifted.get(2, 6).r == 200 && shifted.get(11, 6).a == 0);
    // Scratch fill bridges a dark line with the colors beside it.
    Image scratch(20, 20, {100, 100, 100, 255});
    for (int x = 0; x < 20; ++x) scratch.set(x, 10, {0, 0, 0, 255});
    raster::scratch_fill(scratch, 0, 10.5f, 20, 10.5f, 3);
    CHECK(scratch.get(10, 10).r > 90);
}

static void test_geo_effects() {
    Image img(32, 32, {30, 60, 90, 255});
    for (int y = 0; y < 32; ++y) for (int x = 0; x < 16; ++x) img.set(x, y, {200, 100, 50, 255});
    // Offset with wrap moves the left half to the right.
    Image o = img; effects::offset(o, 16, 0, {0, {0, 0, 0, 255}});
    CHECK(o.get(20, 5).r == 200 && o.get(4, 5).r == 30);
    Image ot = img; effects::offset(ot, 16, 0, {3, {0, 0, 0, 255}});
    CHECK(ot.get(4, 5).a == 0 && ot.get(20, 5).r == 200);
    // Mirror across the vertical center line copies the left half.
    Image m = img; effects::rotating_mirror(m, 90.0f, 50, 50, {1, {0, 0, 0, 255}});
    CHECK(m.get(24, 5).r == 200 || m.get(8, 5).r == 30);
    // Polar to rectangular and back stays plausible (center color survives).
    Image pc = img; effects::polar_coordinates(pc, true, {1, {0, 0, 0, 255}});
    CHECK(pc.get(16, 16).a == 255);
    // Each effect leaves size and alpha intact on an opaque image.
    auto ok = [](const Image& i) { for (int y = 0; y < i.height(); y += 7) for (int x = 0; x < i.width(); x += 7) if (i.get(x, y).a != 255) return false; return i.width() == 32 && i.height() == 32; };
    Image e;
    e = img; effects::curlicues(e, 2, 2, 50, 50); CHECK(ok(e));
    e = img; effects::displacement_map(e, img, 10, false, 0, {1, {0, 0, 0, 255}}); CHECK(ok(e));
    e = img; effects::spiky_halo(e, 60, 8, 20, 0); CHECK(ok(e));
    e = img; effects::warp(e, 50, 50, 50, 60); CHECK(ok(e));
    e = img; effects::wind(e, true, 40); CHECK(ok(e) && e.get(20, 5).r >= 30);
    e = img; effects::cylinder(e, false, 60); CHECK(ok(e));
    e = img; effects::perspective(e, false, 40, {1, {0, 0, 0, 255}}); CHECK(ok(e));
    e = img; effects::skew(e, false, 20, {1, {0, 0, 0, 255}}); CHECK(ok(e));
    e = img; effects::feedback(e, 60, 4, 50, 50, false); CHECK(ok(e));
    e = img; effects::pattern(e, 0, 50, 50, 25, 0); CHECK(ok(e));
    e = img; effects::seamless_tiling(e, 0, 0, 50); CHECK(ok(e));
    e = img; effects::circle(e, {3, {0, 0, 0, 255}}); CHECK(e.get(16, 16).a == 255 && e.get(0, 0).a == 0);
    e = img; effects::pentagon(e, {3, {0, 0, 0, 255}}); CHECK(e.get(16, 16).a == 255 && e.get(0, 31).a == 0);
    e = img; effects::page_curl(e, 3, 50, 50, 2, {255, 255, 255, 255}, {0, 255, 0, 255}, false);
    CHECK(e.get(31, 31).g == 255 && e.get(31, 31).r == 0 && e.get(2, 2).r == 200);   // corner shows the fill, far corner untouched
}

static void test_art_effects() {
    Image img(40, 40, {30, 60, 90, 255});
    for (int y = 0; y < 40; ++y) for (int x = 0; x < 20; ++x) img.set(x, y, {200, 100, 50, 255});
    auto ok = [](const Image& i) { for (int y = 0; y < i.height(); y += 5) for (int x = 0; x < i.width(); x += 5) if (i.get(x, y).a != 255) return false; return i.width() == 40 && i.height() == 40; };
    const Color white{255, 255, 255, 255};
    Image e;
    e = img; effects::aged_newspaper(e, 60); CHECK(ok(e));
    e = img; effects::balls_and_bubbles(e, 10, 4, 12, 80, true, white, 1); CHECK(ok(e));
    e = img; effects::balls_and_bubbles(e, 10, 4, 12, 80, false, white, 1); CHECK(ok(e));
    e = img; effects::colored_edges(e, 30, 0, white); CHECK(ok(e) && e.get(20, 20).r > e.get(5, 20).r);   // the edge lights up
    e = img; effects::colored_foil(e, 2, 40, white, 315); CHECK(ok(e));
    e = img; effects::contours(e, 30, 0, 4, {255, 0, 0, 255}); CHECK(ok(e));
    e = img; effects::enamel(e, 2, 40, 50, 315, white); CHECK(ok(e));
    e = img; effects::glowing_edges(e, 50, 50); CHECK(ok(e) && e.get(5, 20).r < 30 && e.get(20, 20).r > 0);
    e = img; effects::hot_wax(e, {200, 180, 120, 255}); CHECK(ok(e));
    e = img; effects::magnifying_lens(e, 50, 50, 40, 60, 50); CHECK(ok(e));
    e = img; effects::neon_glow(e, 50, 100); CHECK(ok(e));
    e = img; effects::topography(e, 8, 6, 315, white); CHECK(ok(e));
    effects::Light L[1]; L[0].on = true; L[0].x = 50; L[0].y = 50; L[0].cone = 360;
    e = img; effects::lights(e, L, 1, 60); CHECK(ok(e) && e.get(21, 20).b >= e.get(39, 39).b);
    e = img; effects::blinds(e, 8, 60, false, true, {0, 0, 0, 255}); CHECK(ok(e));
    e = img; effects::leather(e, false, 40, 315, 1, 0, {120, 80, 40, 255}, 3); CHECK(ok(e));
    e = img; effects::leather(e, true, 40, 315, 1, 0, {120, 80, 40, 255}, 3); CHECK(ok(e));
    e = img; effects::fur(e, 0, 50, 8, 30, 5); CHECK(ok(e));
    e = img; effects::mosaic_antique(e, 4, 4, 0, 30, 20, 30); CHECK(ok(e));
    e = img; effects::mosaic_glass(e, 4, 4, 50, 20, 30); CHECK(ok(e));
    e = img; effects::polished_stone(e, 2, 40, 315, 30, white); CHECK(ok(e));
    e = img; effects::sandstone(e, 2, 40, 315, white, 2); CHECK(ok(e));
    e = img; effects::sculpture(e, 20, 30, 315, white); CHECK(ok(e));
    e = img; effects::soft_plastic(e, 2, 40, 50, 315, white); CHECK(ok(e));
    e = img; effects::straw_wall(e, 1, 40, 50, 315, white, 6); CHECK(ok(e));
    e = img; effects::texture(e, Image(), 100, 20, 30, 315, white); CHECK(ok(e));
    e = img; effects::tiles(e, 0, 10, 20, 20, 30, 315, white); CHECK(ok(e));
    e = img; effects::tiles(e, 1, 10, 20, 20, 30, 315, white); CHECK(ok(e));
    e = img; effects::weave(e, 3, 4, 70, {0, 0, 0, 255}, white, true); CHECK(ok(e));
    e = img; effects::black_pencil(e, 40, 100); CHECK(ok(e) && e.get(5, 20).r > 100);   // flat areas stay light
    e = img; effects::brush_strokes(e, 12, 50, 4, 70, 8); CHECK(ok(e));
    e = img; effects::charcoal(e, 40, 100); CHECK(ok(e));
    e = img; effects::colored_chalk(e, 40, 70); CHECK(ok(e));
    e = img; effects::colored_pencil(e, 40, 70); CHECK(ok(e));
    e = img; effects::pencil(e, 100, 0, {0, 0, 0, 255}); CHECK(ok(e) && e.get(5, 20).r == 255 && e.get(20, 20).r < 255);
    const float ident[25] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    e = img; effects::user_defined_filter(e, ident, 1, 0); CHECK(e.get(5, 5).r == 200 && e.get(30, 30).b == 90);
    float box[25]; for (float& k : box) k = 1.0f;
    e = img; effects::user_defined_filter(e, box, 25, 0); CHECK(e.get(20, 20).r > 90 && e.get(20, 20).r < 160);
}

static void test_json() {
    json::Value v;
    std::string err;
    CHECK(json::parse(R"({"a": 1, "b": [true, "x\ny", -2.5, null], "c": {"d": "e"}})", v, &err));
    CHECK(v.get("a").as_number() == 1 && v.get("b")[0].as_bool() && v.get("b")[1].as_string() == "x\ny" && v.get("b")[2].as_number() == -2.5);
    CHECK(v.get("c.d").as_string() == "e" && v.get("missing.key").is_null() && v.get("b").size() == 4);
    CHECK(json::Value::string("True").as_bool() && !json::Value::string("False").as_bool(true));
    CHECK(json::dump(v) == R"({"a":1,"b":[true,"x\ny",-2.5,null],"c":{"d":"e"}})");
    CHECK(!json::parse("{\"a\": }", v, &err) && !err.empty());
    json::Value o = json::Value::object();
    o.set("w", json::Value::number(3.5)); o.set("w", json::Value::number(4));
    CHECK(o.size() == 1 && o.get("w").as_number() == 4);
}

static void test_print() {
    Image img(40, 30, {200, 100, 50, 255});
    print::PageSetup ps;
    ps.title = "test (page)";
    const std::string tmp = tmp_path("firn_test_print.pdf");
    CHECK(print::write_pdf(img, ps, tmp));
    std::ifstream f(tmp, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::remove(tmp.c_str());
    CHECK(data.rfind("%PDF-1.4", 0) == 0 && data.find("/DCTDecode") != std::string::npos && data.find("%%EOF") != std::string::npos);
    CHECK(data.find("/Width 40 /Height 30") != std::string::npos && data.find("\xFF\xD8") != std::string::npos);
}

static void test_16bit() {
    Image16 d(4, 4, 1000, 30000, 65535, 65535);
    Image e = to_image8(d);
    CHECK(e.get(0, 0).r == 4 && e.get(0, 0).g == 117 && e.get(0, 0).b == 255);
    Image16 back = to_image16(e);
    CHECK(back.data()[2] == 65535 && back.data()[0] == 4 * 257);
    // LUT precision beyond 8 bits survives.
    raster16::brightness_contrast(d, 0, 0);
    CHECK(d.data()[0] == 1000);
    raster16::levels(d, 0, 1.0f, 255, 0, 255);
    CHECK(std::abs(d.data()[0] - 1000) <= 1);
    raster16::invert(d);
    CHECK(std::abs(d.data()[0] - 64535) <= 1);
    // Document depth, commands and undo.
    Document doc(8, 8);
    doc.add_layer("Background").pixels = Image(8, 8, {100, 100, 100, 255});
    doc.set_active_layer(0);
    CHECK(doc.bit_depth() == 8);
    doc.set_bit_depth(16);
    CHECK(doc.bit_depth() == 16 && doc.layer(0).is_deep() && doc.layer(0).deep->data()[0] == 25700);
    CommandStack stack;
    stack.run(doc, std::make_unique<AdjustCommand>(0, "Levels", [](Image& i) { adjust::apply_lut(i, adjust::levels_lut(0, 1.0f, 200, 0, 255)); }, [](Image16& i) { raster16::levels(i, 0, 1.0f, 200, 0, 255); }));
    CHECK(doc.layer(0).is_deep() && doc.layer(0).deep->data()[0] > 25700 && doc.layer(0).pixels.get(0, 0).r > 100);
    stack.run(doc, std::make_unique<AdjustCommand>(0, "Emboss", [](Image& i) { effects::emboss(i); }));   // 8-bit only
    CHECK(!doc.layer(0).is_deep() && doc.bit_depth() == 8);
    stack.undo(doc);
    CHECK(doc.layer(0).is_deep());
    stack.run(doc, std::make_unique<ResizeCommand>(4, 4, raster::Filter::Bilinear));
    CHECK(doc.layer(0).is_deep() && doc.layer(0).deep->width() == 4);
    stack.run(doc, std::make_unique<RotateCommand>(90.0f, Color{0, 0, 0, 255}));
    CHECK(doc.layer(0).is_deep());
    // Native and PNG round trips keep 16-bit values.
    Image16 fine(6, 5);
    for (size_t i = 0; i < fine.size(); i += 4) { fine.data()[i] = static_cast<uint16_t>(i * 37 % 65536); fine.data()[i + 1] = 12345; fine.data()[i + 2] = 60000; fine.data()[i + 3] = 65535; }
    Document d16(6, 5);
    Layer& L = d16.add_layer("Background");
    L.background = true;
    L.set_deep(fine);
    // A second layer with a 16-bit transparency mask that no 8-bit value hits.
    Image16 soft = fine;
    for (size_t i = 3; i < soft.size(); i += 4) soft.data()[i] = static_cast<uint16_t>(1000 + i);
    d16.add_layer("Soft").set_deep(soft);
    const std::string tmp = tmp_path("firn_test_16.pspimage");
    CHECK(io::save_psp(d16, tmp, nullptr));
    std::string err; std::vector<std::string> warnings;
    auto rt = io::load_psp(tmp, &err, &warnings);
    {
        std::ifstream f(tmp, std::ios::binary);
        std::vector<uint8_t> head(36);
        f.read(reinterpret_cast<char*>(head.data()), 36);
        CHECK(head[32] == 8 && head[33] == 0);  // 48-bit files carry the version 8 label
    }
    std::remove(tmp.c_str());
    CHECK(rt && rt->bit_depth() == 16 && rt->layer(0).is_deep() && rt->layer_count() == 2);
    CHECK(rt->layer(0).deep->data()[1] == 12345 && rt->layer(0).deep->data()[2] == 60000 && rt->layer(0).deep->data()[4 * 7] == fine.data()[4 * 7]);
    CHECK(rt->layer(1).is_deep() && rt->layer(1).deep->data()[3] == 1003 && rt->layer(1).deep->data()[4 * 7 + 3] == soft.data()[4 * 7 + 3]);
    CHECK(rt->layer(1).pixels.get(0, 0).a == (1003 + 128) / 257);
    const std::string png = tmp_path("firn_test_16.png");
    CHECK(io::save_png16(fine, png));
    auto p16 = io::load16(png);
    CHECK(p16 && p16->width() == 6 && p16->data()[1] == 12345 && p16->data()[2] == 60000);
    auto dpng = io::load_document(png, &err, &warnings);
    std::remove(png.c_str());
    CHECK(dpng && dpng->bit_depth() == 16);
    CHECK(!io::load16(std::string(FIRN_SOURCE_DIR) + "/README.md"));
}

static void test_icc() {
    const icc::Profile s = icc::srgb(), a = icc::adobe_rgb(), pp = icc::prophoto_rgb();
    CHECK(s.is_srgb() && !a.is_srgb() && !pp.is_srgb());
    // sRGB curve round trip and the D50 matrix rows sum to the white point.
    CHECK(std::abs(s.trc[0].inverse(s.trc[0].apply(0.37f)) - 0.37f) < 1e-3f);
    CHECK(std::abs(s.to_xyz[3] + s.to_xyz[4] + s.to_xyz[5] - 1.0f) < 0.01f);
    // Encode and parse back.
    const auto bytes = icc::encode(a, "Adobe RGB (1998)");
    const icc::Profile back = icc::parse(bytes);
    CHECK(back.valid && back.matrix_trc && back.description == "Adobe RGB (1998)");
    for (int i = 0; i < 9; ++i) CHECK(std::abs(back.to_xyz[i] - a.to_xyz[i]) < 1e-3f);
    CHECK(std::abs(back.trc[1].apply(0.5f) - a.trc[1].apply(0.5f)) < 1e-3f);
    // Adobe RGB green is outside sRGB: converting clips to a very saturated green; gray stays gray.
    Image img(2, 1); img.set(0, 0, {0, 255, 0, 255}); img.set(1, 0, {128, 128, 128, 255});
    icc::Transform t(a, s);
    CHECK(!t.identity);
    t.apply(img);
    CHECK(img.get(0, 0).g == 255 && img.get(0, 0).r < 40 && img.get(1, 0).r == img.get(1, 0).g && std::abs(img.get(1, 0).r - 128) <= 3);
    icc::Transform id(s, s);
    Image same(1, 1); same.set(0, 0, {200, 100, 50, 255});
    id.apply(same);
    CHECK(same.get(0, 0).r == 200 && same.get(0, 0).g == 100);
    // Profiles travel through PNG and JPEG files.
    const std::string png = tmp_path("firn_test_icc.png"), jpg = tmp_path("firn_test_icc.jpg");
    CHECK(io::save_png(img, png) && io::embed_icc(png, bytes));
    const auto pb = io::read_icc(png);
    CHECK(pb.size() == bytes.size() && std::equal(pb.begin(), pb.end(), bytes.begin()));
    auto reloaded = io::load(png);
    CHECK(reloaded && reloaded->get(0, 0).g == 255);
    CHECK(io::save(img, jpg) && io::embed_icc(jpg, bytes));
    const auto jb = io::read_icc(jpg);
    CHECK(jb.size() == bytes.size() && std::equal(jb.begin(), jb.end(), bytes.begin()));
    CHECK(io::load(jpg).has_value());
    std::string err; std::vector<std::string> warnings;
    auto doc = io::load_document(png, &err, &warnings);
    CHECK(doc && doc->icc().size() == bytes.size());
    std::remove(png.c_str()); std::remove(jpg.c_str());
}

// Every parser that reads a file has to survive a broken one. These are
// not hypothetical inputs: a truncated download, a half-written file after
// a crash, or a file from another program all land here, and the reader is
// the only thing between them and the rest of the program.
//
// The heap-use-after-free this pins down was real: load_psp_stored_composite
// kept a pointer to a block inside the vector a range-for was walking, and
// that vector was a temporary, so the pointer dangled as soon as the loop
// ended. It only showed up under a sanitizer, which is why this suite is
// worth running that way.
static void test_parsers_survive_broken_files() {
    std::string err;
    std::vector<std::string> warnings;

    // A real file of each kind to cut up.
    Document doc(24, 16);
    Layer& bg = doc.add_layer("Background");
    bg.background = true;
    bg.pixels.fill({120, 90, 60, 255});
    Layer& top = doc.add_layer("Top");
    top.pixels = Image(24, 16, {0, 0, 0, 0});
    top.pixels.set(4, 4, {255, 0, 0, 255});
    const std::vector<uint8_t> tiff_file = io::save_tiff_to_memory(doc);
    const std::vector<uint8_t> psd_file = io::save_psd_to_memory(doc, nullptr);
    const std::vector<uint8_t> psp = io::save_psp_to_memory(doc);
    const std::vector<uint8_t> ora = io::save_ora_to_memory(doc);
    const std::vector<uint8_t> png = io::encode_png(bg.pixels);
    CHECK(!psp.empty() && !ora.empty() && !png.empty());

    auto poke_every_parser = [&](const uint8_t* d, size_t n) {
        (void)io::load_psp_from_memory(d, n, &err, &warnings);
        (void)io::load_ora_from_memory(d, n, &err, &warnings);
        (void)io::load_psp_stored_composite(d, n);
        (void)io::load_psd_from_memory(d, n, &err, &warnings);
        (void)io::load_tiff_from_memory(d, n, &err, &warnings);
        (void)meta::parse_jpeg(d, n);
        (void)meta::parse_png(d, n);
        (void)meta::parse_tiff(d, n);
        // XMP is XML out of a file, so the scanner sees whatever is there:
        // unterminated tags, quotes that never close, markup in the middle
        // of a value. Building it back must not walk off the end either.
        {
            meta::Metadata md;
            md.xmp.assign(reinterpret_cast<const char*>(d), n);
            md.entries = meta::parse_xmp(md.xmp);
            (void)meta::build_xmp(md);
            for (meta::Entry& e : md.entries) e.set_text("changed");
            (void)meta::build_xmp(md);
            for (const meta::Entry& e : std::vector<meta::Entry>(md.entries)) md.remove_xmp(e.key);
            md.remove_private();
        }
        std::vector<vec::Object> objs;
        (void)io::decode_objects(d, n, objs);
        (void)io::vector_objects_from_bytes(d, n, objs);
        zip::Archive ar;
        (void)zip::read(d, n, ar, &err);
        (void)io::load_memory(d, n, &err);
    };

    // Truncated at every length, which is where offset and length fields
    // point past the end.
    for (const std::vector<uint8_t>* src : {&psp, &ora, &png, &tiff_file, &psd_file}) {
        for (size_t cut = 0; cut <= src->size(); cut += std::max<size_t>(1, src->size() / 12))
            poke_every_parser(src->data(), cut);
        poke_every_parser(src->data(), src->size() - 1);
    }
    // Nothing at all, and one byte.
    poke_every_parser(nullptr, 0);
    { const uint8_t one = 0x89; poke_every_parser(&one, 1); }

    // Corruption inside the headers, where the lengths live. Deterministic
    // so a failure can be reproduced from the seed alone.
    uint32_t rng = 20260912u;
    auto next = [&rng] { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
    for (const std::vector<uint8_t>* src : {&psp, &ora, &png}) {
        for (int i = 0; i < 40; ++i) {
            std::vector<uint8_t> d = *src;
            const size_t span = std::min<size_t>(d.size(), 512);
            d[next() % span] = static_cast<uint8_t>(next());
            poke_every_parser(d.data(), d.size());
        }
    }
    // A valid signature in front of noise: the shape that gets furthest in
    // before something goes wrong.
    for (const std::vector<uint8_t>* src : {&psp, &ora, &png}) {
        std::vector<uint8_t> d = *src;
        for (size_t k = std::min<size_t>(d.size(), 32); k < d.size(); ++k) d[k] = static_cast<uint8_t>(next());
        poke_every_parser(d.data(), d.size());
    }

    // A file must not be able to ask for an unreasonable buffer. One flipped
    // byte in the composite's size field used to make this allocate 2.3 GB
    // for a 20 KB file, which is a denial of service on opening a bad
    // download. The reader refuses instead.
    {
        std::vector<uint8_t> huge = psp;
        // Walk the size fields and try each as an absurd dimension.
        int refused = 0, attempts = 0;
        for (size_t at = 0; at + 4 <= std::min<size_t>(huge.size(), 600); ++at) {
            huge = psp;
            huge[at] = 0xBA;
            huge[at + 1] = 0xBA;
            ++attempts;
            // Not a crash and not a gigabyte: the only outcomes allowed are
            // an image of a sane size, or nothing.
            const std::optional<Image> got = io::load_psp_stored_composite(huge.data(), huge.size());
            if (!got) ++refused;
            else CHECK(static_cast<size_t>(got->width()) * got->height() <= (size_t(1) << 28));
        }
        CHECK(attempts > 100 && refused > 0);
    }

    // Getting here without a crash is the test. A good file must still read.
    auto back = io::load_psp_from_memory(psp.data(), psp.size(), &err, &warnings);
    CHECK(back && back->layer_count() == 2);
    auto back2 = io::load_ora_from_memory(ora.data(), ora.size(), &err, &warnings);
    CHECK(back2 && back2->layer_count() == 2);
    CHECK(io::load_psp_stored_composite(psp.data(), psp.size()).has_value());
}

// Box blur: a running sum, so the cost does not grow with the radius. The
// window always holds 2r+1 samples with out-of-range ones clamped to the
// edge, which is what pins the result: these expectations are the averages
// that definition produces, and they caught the off-by-one the first
// running-sum attempt had.
static void test_box_blur() {
    // A single bright pixel in a flat field, radius 1: the 3x3 average.
    {
        Image img(5, 5, {0, 0, 0, 255});
        img.set(2, 2, {90, 90, 90, 255});
        raster::box_blur(img, 1);
        CHECK(img.get(2, 2).r == 10);   // 90 / 9
        CHECK(img.get(1, 2).r == 10 && img.get(3, 2).r == 10);
        CHECK(img.get(0, 2).r == 0);    // outside the 3x3 window
        CHECK(img.get(2, 2).a == 255);
    }
    // A flat image stays exactly flat at any radius: no drift from the
    // running sum, and edge clamping keeps the window full.
    for (int r : {1, 3, 9}) {
        Image flat(9, 7, {37, 200, 5, 255});
        raster::box_blur(flat, r);
        for (int y = 0; y < 7; ++y)
            for (int x = 0; x < 9; ++x)
                CHECK(flat.get(x, y).r == 37 && flat.get(x, y).g == 200 && flat.get(x, y).b == 5 && flat.get(x, y).a == 255);
    }
    // A radius wider than the picture averages the whole thing.
    {
        Image img(2, 1, {0, 0, 0, 255});
        img.set(0, 0, {100, 0, 0, 255});
        img.set(1, 0, {0, 0, 0, 255});
        raster::box_blur(img, 50);
        // Every window is dominated by the clamped edges, so the two pixels
        // keep their own side's colour rather than both becoming 50.
        CHECK(img.get(0, 0).r > img.get(1, 0).r);
    }
    // A single row and a single column, where the other axis is all clamp.
    {
        Image row(4, 1, {0, 0, 0, 255});
        row.set(0, 0, {255, 255, 255, 255});
        raster::box_blur(row, 1);
        CHECK(row.get(0, 0).r == 170);   // (255 + 255 + 0) / 3, the left edge repeated
        CHECK(row.get(1, 0).r == 85);    // (255 + 0 + 0) / 3
        CHECK(row.get(3, 0).r == 0);
    }
    // Radius zero leaves the image alone.
    {
        Image img(3, 3, {1, 2, 3, 4});
        const Image before = img;
        raster::box_blur(img, 0);
        CHECK(std::memcmp(img.data(), before.data(), img.size_bytes()) == 0);
    }
}

// Content-aware fill reports how far along it is and stops when asked, so
// the interface can run it on a worker thread and still cancel it.
static void test_inpaint_progress_and_cancel() {
    // A checkerboard with the hole painted flat red, so a finished fill is
    // visibly different from the input: the fill reproduces a regular
    // texture exactly, which is why the hole cannot start as that texture.
    auto make = [] {
        Image img(96, 96);
        for (int y = 0; y < 96; ++y)
            for (int x = 0; x < 96; ++x) {
                const uint8_t v = static_cast<uint8_t>(((x / 8 + y / 8) % 2) ? 220 : 40);
                img.set(x, y, {v, v, v, 255});
            }
        for (int y = 36; y < 60; ++y)
            for (int x = 36; x < 60; ++x) img.set(x, y, {255, 0, 0, 255});
        return img;
    };
    const Mask hole = mask::rectangle(96, 96, 36, 36, 60, 60, false);

    // Progress arrives in order, inside 0..1, and reaches the end.
    Image img = make();
    std::vector<float> seen;
    inpaint::Options opt;
    opt.max_side = 96;
    opt.on_progress = [&seen](float p) { seen.push_back(p); return true; };
    CHECK(inpaint::content_aware_fill(img, hole, opt));
    CHECK(!seen.empty());
    for (size_t i = 0; i < seen.size(); ++i) {
        CHECK(seen[i] >= 0.0f && seen[i] <= 1.0f);
        if (i) CHECK(seen[i] >= seen[i - 1]);
    }
    CHECK(seen.back() > 0.9f);
    const Image done = img;

    // Stopping early leaves the image exactly as it was, so a cancelled fill
    // has nothing to undo.
    Image other = make();
    const Image untouched = other;
    int calls = 0;
    inpaint::Options stop;
    stop.max_side = 96;
    stop.on_progress = [&calls](float) { return ++calls < 2; };
    CHECK(!inpaint::content_aware_fill(other, hole, stop));
    CHECK(other.size_bytes() == untouched.size_bytes());
    CHECK(std::memcmp(other.data(), untouched.data(), other.size_bytes()) == 0);
    // And the run that was allowed to finish did change something.
    CHECK(std::memcmp(done.data(), untouched.data(), done.size_bytes()) != 0);

    // Without a callback it still works, which is the path every other
    // caller takes.
    Image plain = make();
    inpaint::Options none;
    none.max_side = 96;
    CHECK(inpaint::content_aware_fill(plain, hole, none));
    CHECK(std::memcmp(plain.data(), untouched.data(), plain.size_bytes()) != 0);
}

// Gradient Map: a pixel's lightness picks a colour along a gradient. It is
// Firn's own adjustment, so the native container stores it as a placeholder
// layer plus the stash rather than as one of the original's own blocks.
static void test_gradient_map() {
    Adjustment a;
    a.kind = Adjustment::Kind::GradientMap;
    a.gradient.colors = {{{255, 0, 0, 255}, 0, 50}, {{0, 0, 255, 255}, 100, 50}};
    CHECK(a.is_firn_only() && !a.is_filter());
    CHECK(std::string(Adjustment::kind_name(a.kind)) == "Gradient Map");

    Image img(3, 1);
    img.set(0, 0, {0, 0, 0, 255});          // black maps to the first stop
    img.set(1, 0, {255, 255, 255, 255});    // white to the last
    img.set(2, 0, {0, 0, 0, 0});            // a clear pixel keeps its alpha
    a.apply(img);
    CHECK(img.get(0, 0).r == 255 && img.get(0, 0).b == 0);
    CHECK(img.get(1, 0).b == 255 && img.get(1, 0).r == 0);
    CHECK(img.get(2, 0).a == 0);

    // As an adjustment layer it recolours what is below it.
    Document doc(2, 1);
    Layer& base = doc.add_layer("Base");
    base.pixels.set(0, 0, {0, 0, 0, 255});
    base.pixels.set(1, 0, {255, 255, 255, 255});
    Layer& gm = doc.add_layer("Map");
    gm.type = LayerType::Adjustment;
    gm.adjustment = a;
    doc.touch();
    const Image flat = doc.composite();
    CHECK(flat.get(0, 0).r == 255 && flat.get(1, 0).b == 255);

    // The whole adjustment, gradient included, survives both formats.
    std::string err;
    std::vector<std::string> warnings;
    const std::vector<uint8_t> ora = io::save_ora_to_memory(doc);
    auto back = io::load_ora_from_memory(ora.data(), ora.size(), &err, &warnings);
    CHECK(back && back->layer_count() == 2);
    CHECK(back->layer(1).is_adjustment() && back->layer(1).adjustment.kind == Adjustment::Kind::GradientMap);
    CHECK(back->layer(1).adjustment.gradient.colors.size() == 2);
    CHECK(back->layer(1).adjustment.gradient.colors[0].color.r == 255);
    CHECK(back->layer(1).adjustment.gradient.colors[1].color.b == 255);

    const std::vector<uint8_t> psp = io::save_psp_to_memory(doc);
    auto p2 = io::load_psp_from_memory(psp.data(), psp.size(), &err, &warnings);
    CHECK(p2 && p2->layer_count() == 2);
    CHECK(p2->layer(1).is_adjustment() && p2->layer(1).adjustment.kind == Adjustment::Kind::GradientMap);
    CHECK(p2->layer(1).adjustment.gradient.colors.size() == 2 && p2->layer(1).adjustment.gradient.colors[1].color.b == 255);
}

// Locking a layer's transparency: the clear parts stay clear, so painting
// and fills only touch pixels that are already there. This is the
// original's "transparency protected", and it travels in its own field of
// the native layer info rather than in Firn's stash.
static void test_lock_transparency() {
    Document doc(4, 1);
    Layer& L = doc.add_layer("L");
    L.pixels = Image(4, 1, {0, 0, 0, 0});
    L.pixels.set(0, 0, {10, 20, 30, 255});
    L.pixels.set(1, 0, {40, 50, 60, 128});   // partly there: still paintable

    // Unlocked, a fill covers the whole layer.
    CommandStack hist;
    hist.run(doc, std::make_unique<AdjustCommand>(0, "Fill", [](Image& i) { i.fill({200, 0, 0, 255}); }));
    CHECK(doc.layer(0).pixels.get(3, 0).a == 255 && doc.layer(0).pixels.get(3, 0).r == 200);
    hist.undo(doc);
    CHECK(doc.layer(0).pixels.get(3, 0).a == 0);

    // Locked, the same fill leaves the clear pixels exactly as they were.
    doc.layer(0).lock_alpha = true;
    hist.run(doc, std::make_unique<AdjustCommand>(0, "Fill", [](Image& i) { i.fill({200, 0, 0, 255}); }));
    const Image& px = doc.layer(0).pixels;
    CHECK(px.get(0, 0).r == 200 && px.get(0, 0).a == 255);   // a pixel that existed is filled
    CHECK(px.get(1, 0).r == 200);                             // a partly there pixel too
    CHECK(px.get(3, 0).a == 0);                               // a clear one is untouched
    CHECK(px.get(2, 0).a == 0);
    hist.undo(doc);
    CHECK(doc.layer(0).pixels.get(0, 0).r == 10);

    // It is a layer property, so it is undoable like the rest of them.
    const LayerProps before = doc.props(0);
    LayerProps after = before;
    after.lock_alpha = false;
    hist.run(doc, std::make_unique<LayerPropertiesCommand>(0, before, after));
    CHECK(!doc.layer(0).lock_alpha);
    hist.undo(doc);
    CHECK(doc.layer(0).lock_alpha);

    // The native container has a field for it, so it round trips there
    // rather than riding in the Firn stash.
    std::string err;
    std::vector<std::string> warnings;
    const std::vector<uint8_t> psp = io::save_psp_to_memory(doc);
    auto back = io::load_psp_from_memory(psp.data(), psp.size(), &err, &warnings);
    CHECK(back && back->layer_count() == 1 && back->layer(0).lock_alpha);
    // And the project format keeps it too.
    const std::vector<uint8_t> ora = io::save_ora_to_memory(doc);
    auto b2 = io::load_ora_from_memory(ora.data(), ora.size(), &err, &warnings);
    CHECK(b2 && b2->layer_count() == 1 && b2->layer(0).lock_alpha);
}

// Pass-through groups: the members composite straight onto what is below
// the group, so an adjustment or filter layer inside it reaches the whole
// image rather than only its siblings.
static void test_pass_through_groups() {
    auto build = [](bool pass) {
        auto doc = std::make_unique<Document>(4, 1);
        Layer& bottom = doc->add_layer("Bottom");
        bottom.pixels = Image(4, 1, {200, 200, 200, 255});
        Layer& g = doc->add_layer("Group");
        g.type = LayerType::Group;
        g.pass_through = pass;
        Layer& inner = doc->add_layer("Inner");
        inner.depth = 1;
        inner.pixels = Image(4, 1, {0, 0, 0, 0});
        inner.pixels.set(0, 0, {50, 50, 50, 255});
        Layer& inv = doc->add_layer("Invert");
        inv.depth = 1;
        inv.type = LayerType::Adjustment;
        inv.adjustment.kind = Adjustment::Kind::Invert;
        doc->touch();
        return doc;
    };

    // Isolated, the adjustment stops at the group's own members.
    auto iso = build(false);
    Image f = iso->composite();
    CHECK(f.get(0, 0).r == 205);   // the member was inverted
    CHECK(f.get(2, 0).r == 200);   // what is below the group was not

    // Pass-through, it reaches the layer below the group as well.
    auto pt = build(true);
    f = pt->composite();
    CHECK(f.get(0, 0).r == 205);
    CHECK(f.get(2, 0).r == 55);

    // The group's opacity mixes the change back over the original.
    auto half = build(true);
    half->layer(1).opacity = 0.5f;
    half->touch();
    f = half->composite();
    CHECK(std::abs(f.get(2, 0).r - 128) <= 2);

    // Its mask says where the change lands.
    auto masked = build(true);
    masked->layer(1).mask = mask::rectangle(4, 1, 0, 0, 2, 1, false);
    masked->touch();
    f = masked->composite();
    CHECK(f.get(1, 0).r == 55 && f.get(3, 0).r == 200);

    // A hidden group changes nothing.
    auto hidden = build(true);
    hidden->layer(1).visible = false;
    hidden->touch();
    f = hidden->composite();
    CHECK(f.get(2, 0).r == 200 && f.get(0, 0).r == 200);

    // A filter inside one has to see what is below the group, not an empty
    // buffer, which is the part that is easy to get wrong.
    Document blur(8, 1);
    Layer& base = blur.add_layer("Base");
    base.pixels = Image(8, 1, {0, 0, 0, 255});
    for (int x = 4; x < 8; ++x) base.pixels.set(x, 0, {255, 255, 255, 255});
    Layer& bg = blur.add_layer("G");
    bg.type = LayerType::Group;
    bg.pass_through = true;
    Layer& bl = blur.add_layer("Blur");
    bl.depth = 1;
    bl.type = LayerType::Adjustment;
    bl.adjustment.kind = Adjustment::Kind::GaussianBlur;
    bl.adjustment.blur_radius = 2.0f;
    blur.touch();
    const Image b = blur.composite();
    CHECK(b.get(3, 0).r > 20 && b.get(3, 0).r < 235);   // a ramp, not the hard edge
    CHECK(b.get(4, 0).r > 20 && b.get(4, 0).r < 235);

    // Both formats keep the flag.
    std::string err;
    std::vector<std::string> warnings;
    const std::vector<uint8_t> ora = io::save_ora_to_memory(*pt);
    auto back = io::load_ora_from_memory(ora.data(), ora.size(), &err, &warnings);
    CHECK(back && back->layer_count() == 4 && back->layer(1).pass_through);
    const std::vector<uint8_t> psp = io::save_psp_to_memory(*pt);
    auto p2 = io::load_psp_from_memory(psp.data(), psp.size(), &err, &warnings);
    CHECK(p2 && p2->layer_count() == 4 && p2->layer(1).pass_through);
}

// Blend ranges: a layer limited to a range of its own tones, or of the
// tones beneath it, without painting a mask.
static void test_blend_ranges() {
    // A black-to-white ramp under a flat red layer.
    Document doc(256, 1);
    Layer& base = doc.add_layer("Base");
    for (int x = 0; x < 256; ++x) base.pixels.set(x, 0, {static_cast<uint8_t>(x), static_cast<uint8_t>(x), static_cast<uint8_t>(x), 255});
    Layer& top = doc.add_layer("Top");
    top.pixels = Image(256, 1, {255, 0, 0, 255});

    CHECK(top.ranges.identity());
    Image f = doc.composite();
    CHECK(f.get(10, 0).r == 255 && f.get(200, 0).r == 255);   // nothing limited yet

    // Hide the layer where what is under it is dark, with a hard edge.
    top.ranges.under.low0 = 64;
    top.ranges.under.low1 = 64;
    doc.touch();
    f = doc.composite();
    CHECK(f.get(10, 0).r == 10 && f.get(10, 0).g == 10);      // the base shows through
    CHECK(f.get(200, 0).r == 255 && f.get(200, 0).g == 0);    // the red still covers

    // Splitting the two stops ramps it in instead.
    top.ranges.under.low1 = 192;
    doc.touch();
    f = doc.composite();
    const int mid = f.get(128, 0).r;
    CHECK(mid > 128 && mid < 255 && f.get(128, 0).g > 0 && f.get(128, 0).g < 128);

    // The layer's own tones, rather than what is below.
    top.ranges = BlendRanges{};
    for (int x = 0; x < 256; ++x) top.pixels.set(x, 0, {static_cast<uint8_t>(x), static_cast<uint8_t>(x), static_cast<uint8_t>(x), 255});
    top.ranges.source.high1 = 128;
    top.ranges.source.high0 = 128;
    doc.touch();
    f = doc.composite();
    CHECK(f.get(60, 0).r == 60);     // kept: the layer's own value is under the stop
    CHECK(f.get(200, 0).r == 200);   // dropped, and the base happens to match

    // One channel instead of lightness.
    Document d2(2, 1);
    Layer& b2 = d2.add_layer("B");
    b2.pixels.set(0, 0, {0, 0, 0, 255});
    b2.pixels.set(1, 0, {0, 0, 255, 255});
    Layer& t2 = d2.add_layer("T");
    t2.pixels = Image(2, 1, {255, 255, 0, 255});
    t2.ranges.channel = BlendRanges::Channel::Blue;
    t2.ranges.under.low0 = 128;
    t2.ranges.under.low1 = 128;
    d2.touch();
    Image g = d2.composite();
    CHECK(g.get(0, 0).r == 0);       // no blue underneath, so the layer is hidden
    CHECK(g.get(1, 0).r == 255);     // blue underneath, so it shows

    // A range composes with the layer's mask rather than replacing it.
    t2.mask = mask::rectangle(2, 1, 0, 0, 1, 1, false);
    d2.touch();
    g = d2.composite();
    CHECK(g.get(1, 0).r == 0);       // the mask hides the pixel the range allowed

    // Both formats carry it.
    std::string err;
    std::vector<std::string> warnings;
    const std::vector<uint8_t> ora = io::save_ora_to_memory(doc);
    auto back = io::load_ora_from_memory(ora.data(), ora.size(), &err, &warnings);
    CHECK(back && back->layer_count() == 2);
    CHECK(back->layer(1).ranges.source.high1 == 128 && back->layer(1).ranges.source.high0 == 128);
    CHECK(back->layer(1).ranges.under.identity());

    Document d3(4, 1);
    d3.add_layer("Under").pixels.fill({0, 0, 0, 255});
    Layer& t3 = d3.add_layer("Ranged");
    t3.pixels = Image(4, 1, {200, 100, 50, 255});
    t3.ranges.channel = BlendRanges::Channel::Green;
    t3.ranges.under.low0 = 10;
    t3.ranges.under.low1 = 20;
    t3.ranges.source.high1 = 200;
    t3.ranges.source.high0 = 250;
    const std::vector<uint8_t> psp = io::save_psp_to_memory(d3);
    auto p3 = io::load_psp_from_memory(psp.data(), psp.size(), &err, &warnings);
    CHECK(p3 && p3->layer_count() == 2);
    const BlendRanges& r = p3->layer(1).ranges;
    CHECK(r.channel == BlendRanges::Channel::Green);
    CHECK(r.under.low0 == 10 && r.under.low1 == 20);
    CHECK(r.source.high1 == 200 && r.source.high0 == 250);
}

// A layer style is measured in pixels, so resizing the image has to resize
// the style with it. Otherwise halving a picture leaves a full-size drop
// shadow on it, twice as heavy against the artwork as the one drawn.
static void test_layer_style_scales_with_the_image() {
    Document doc(400, 300);
    doc.add_layer("Background").background = true;
    Layer& L = doc.add_layer("Shape");
    L.pixels = Image(400, 300, {0, 0, 0, 0});
    for (int y = 100; y < 200; ++y)
        for (int x = 100; x < 300; ++x) L.pixels.set(x, y, {20, 60, 200, 255});
    L.style.drop_shadow = true;
    L.style.shadow_offset_x = 20; L.style.shadow_offset_y = 16; L.style.shadow_blur = 8;
    L.style.outer_glow = true; L.style.glow_size = 12;
    L.style.inner_glow = true; L.style.inner_glow_size = 6;
    L.style.stroke = true; L.style.stroke_width = 4;
    L.style.bevel = true; L.style.bevel_size = 10; L.style.bevel_depth = 2.0f; L.style.bevel_angle = 315.0f;
    L.style.shadow_opacity = 0.6f;
    doc.touch();

    CommandStack hist;
    hist.run(doc, std::make_unique<ResizeCommand>(200, 150, raster::Filter::Bilinear));
    const LayerStyle& s = doc.layer(1).style;
    CHECK(std::abs(s.shadow_offset_x - 10.0f) < 0.01f);
    CHECK(std::abs(s.shadow_offset_y - 8.0f) < 0.01f);
    CHECK(std::abs(s.shadow_blur - 4.0f) < 0.01f);
    CHECK(std::abs(s.glow_size - 6.0f) < 0.01f);
    CHECK(std::abs(s.inner_glow_size - 3.0f) < 0.01f);
    CHECK(s.stroke_width == 2);
    CHECK(std::abs(s.bevel_size - 5.0f) < 0.01f);
    // A ratio, a direction and an opacity are not lengths.
    CHECK(std::abs(s.bevel_depth - 2.0f) < 0.01f);
    CHECK(std::abs(s.bevel_angle - 315.0f) < 0.01f);
    CHECK(std::abs(s.shadow_opacity - 0.6f) < 0.01f);

    // Undo puts the original sizes back, not scaled-up approximations.
    hist.undo(doc);
    const LayerStyle& u = doc.layer(1).style;
    CHECK(std::abs(u.shadow_offset_x - 20.0f) < 0.01f && std::abs(u.shadow_blur - 8.0f) < 0.01f);
    CHECK(u.stroke_width == 4);

    // Enlarging scales the other way, and each axis follows its own factor.
    CommandStack h2;
    h2.run(doc, std::make_unique<ResizeCommand>(800, 300, raster::Filter::Bilinear));
    const LayerStyle& w = doc.layer(1).style;
    CHECK(std::abs(w.shadow_offset_x - 40.0f) < 0.01f);   // 2x wider
    CHECK(std::abs(w.shadow_offset_y - 16.0f) < 0.01f);   // same height

    // A stroke must not disappear on a big reduction: a hairline is a line.
    Document tiny(1000, 1000);
    tiny.add_layer("Background").background = true;
    Layer& t = tiny.add_layer("S");
    t.pixels = Image(1000, 1000, {0, 0, 0, 0});
    t.style.stroke = true;
    t.style.stroke_width = 3;
    CommandStack h3;
    h3.run(tiny, std::make_unique<ResizeCommand>(20, 20, raster::Filter::Bilinear));
    CHECK(tiny.layer(1).style.stroke_width >= 1);
}

// Clipping masks: a layer marked clipped shows only where the layer below
// it does, and the whole unit then blends with that layer's own opacity,
// blend mode and mask.
static void test_clipping_masks() {
    Document doc(20, 10);
    Layer& base = doc.add_layer("Base");
    base.pixels = Image(20, 10, {0, 0, 0, 0});
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 10; ++x) base.pixels.set(x, y, {255, 0, 0, 255});
    Layer& top = doc.add_layer("Top");
    top.pixels = Image(20, 10, {0, 0, 255, 255});

    // Unclipped, the top layer covers everything.
    Image flat = doc.composite();
    CHECK(flat.get(5, 5).b == 255 && flat.get(15, 5).b == 255 && flat.get(15, 5).a == 255);

    top.clipped = true;
    doc.touch();
    flat = doc.composite();
    CHECK(flat.get(5, 5).b == 255 && flat.get(5, 5).a == 255);   // over the base it shows
    CHECK(flat.get(15, 5).a == 0);                                // past the base, nothing
    CHECK(doc.clip_end(0) == 2);

    // The base's opacity applies to the whole unit, not only to the base.
    base.opacity = 0.5f;
    doc.touch();
    flat = doc.composite();
    CHECK(flat.get(5, 5).b == 255 && std::abs(flat.get(5, 5).a - 128) <= 1);
    base.opacity = 1.0f;

    // A clipped layer's own opacity still blends it against the base.
    top.opacity = 0.5f;
    doc.touch();
    flat = doc.composite();
    CHECK(std::abs(flat.get(5, 5).r - 128) <= 2 && std::abs(flat.get(5, 5).b - 128) <= 2 && flat.get(5, 5).a == 255);
    top.opacity = 1.0f;

    // The base's mask shapes the unit, so the clipped layer stops where the
    // mask hides the base rather than at the base's raw alpha.
    base.mask = mask::rectangle(20, 10, 0, 0, 5, 10, false);
    doc.touch();
    flat = doc.composite();
    CHECK(flat.get(2, 5).a == 255 && flat.get(8, 5).a == 0);
    base.mask = Mask();

    // Several layers clip to one base, and an unclipped layer ends the run.
    Layer& third = doc.add_layer("Third");
    third.pixels = Image(20, 10, {0, 255, 0, 255});
    third.clipped = true;
    Layer& free_layer = doc.add_layer("Free");
    free_layer.pixels = Image(20, 10, {0, 0, 0, 0});
    for (int y = 0; y < 10; ++y) free_layer.pixels.set(18, y, {255, 255, 0, 255});
    doc.touch();
    CHECK(doc.clip_end(0) == 3);
    flat = doc.composite();
    CHECK(flat.get(5, 5).g == 255 && flat.get(5, 5).a == 255);    // the top of the run wins
    CHECK(flat.get(15, 5).a == 0);                                 // still held to the base
    CHECK(flat.get(18, 5).r == 255 && flat.get(18, 5).g == 255);   // the free layer is unaffected

    // A clipped adjustment layer changes only what it is clipped to.
    Document d2(4, 2);
    Layer& b2 = d2.add_layer("B");
    b2.pixels = Image(4, 2, {0, 0, 0, 0});
    b2.pixels.set(0, 0, {200, 200, 200, 255});
    Layer& inv = d2.add_layer("Invert");
    inv.type = LayerType::Adjustment;
    inv.adjustment.kind = Adjustment::Kind::Invert;
    inv.clipped = true;
    Layer& over = d2.add_layer("Over");
    over.pixels = Image(4, 2, {0, 0, 0, 0});
    over.pixels.set(3, 0, {200, 200, 200, 255});
    d2.touch();
    const Image f2 = d2.composite();
    CHECK(f2.get(0, 0).r == 55);    // the clipped adjustment inverted its base
    CHECK(f2.get(3, 0).r == 200);   // and left the layer above it alone

    // Without the clip, the adjustment reaches everything below it, which is
    // the behaviour that has to keep working.
    inv.clipped = false;
    d2.touch();
    const Image f3 = d2.composite();
    CHECK(f3.get(0, 0).r == 55 && f3.get(3, 0).r == 200);

    // Clipping is Firn's own, so the native container carries it in the stash
    // rather than in a block the original would read.
    std::string err;
    std::vector<std::string> warnings;
    const std::vector<uint8_t> psp = io::save_psp_to_memory(doc);
    auto back = io::load_psp_from_memory(psp.data(), psp.size(), &err, &warnings);
    CHECK(back && back->layer_count() == 4);
    CHECK(back->layer(1).clipped && back->layer(2).clipped && !back->layer(3).clipped);
}

// The project format is meant to be lossless: everything the document model
// holds comes back. This sets each field to something that is not its
// default, so a field the writer forgets shows up as a failure rather than
// as a default that happens to match. Editor-only state (Layer::floating,
// Object::selected) is deliberately not saved and not checked here.
static void test_openraster_lossless() {
    Document doc(40, 30);
    Layer& bg = doc.add_layer("Back");
    bg.background = true;
    bg.pixels.fill({10, 20, 30, 255});
    LayerStyle& s0 = bg.style;
    s0.drop_shadow = true; s0.shadow_color = {1, 2, 3, 255};
    s0.shadow_opacity = 0.4f; s0.shadow_offset_x = 7; s0.shadow_offset_y = -3; s0.shadow_blur = 9;
    s0.outer_glow = true; s0.glow_color = {4, 5, 6, 255}; s0.glow_size = 11; s0.glow_opacity = 0.3f;
    s0.inner_glow = true; s0.inner_glow_color = {7, 8, 9, 255}; s0.inner_glow_size = 12; s0.inner_glow_opacity = 0.2f;
    s0.stroke = true; s0.stroke_color = {10, 11, 12, 255}; s0.stroke_width = 6; s0.stroke_opacity = 0.6f;
    s0.bevel = true; s0.bevel_size = 4; s0.bevel_depth = 2.5f; s0.bevel_angle = 200;

    Layer& grp = doc.add_layer("Grp");
    grp.type = LayerType::Group;
    grp.expanded = false;
    grp.mask = mask::rectangle(40, 30, 0, 0, 20, 30, false);
    grp.mask_enabled = false;
    grp.opacity = 0.25f;
    grp.blend = BlendMode::Dissolve;   // no SVG operator: restored from firn:blend
    grp.pass_through = true;

    Layer& mem = doc.add_layer("Mem");
    mem.depth = 1;
    mem.pixels = Image(40, 30, {0, 0, 0, 0});
    mem.pixels.set(5, 5, {99, 88, 77, 255});
    mem.visible = false;
    mem.clipped = true;
    mem.ranges.channel = BlendRanges::Channel::Blue;
    mem.ranges.source.low0 = 12;
    mem.ranges.source.low1 = 34;
    mem.ranges.under.high1 = 200;
    mem.ranges.under.high0 = 220;

    Layer& deep = doc.add_layer("Deep");
    deep.depth = 1;
    { Image16 d(40, 30); for (size_t i = 0; i < d.size(); i += 4) { d.data()[i] = 1234; d.data()[i + 1] = 4321; d.data()[i + 2] = 999; d.data()[i + 3] = 65535; } deep.set_deep(std::move(d)); }

    // An adjustment layer whose kind uses only some of its fields: the rest
    // are values a person set and switched away from, and must survive.
    Layer& adj = doc.add_layer("Adj");
    adj.type = LayerType::Adjustment;
    Adjustment& a0 = adj.adjustment;
    a0.kind = Adjustment::Kind::Levels;
    a0.levels[0] = {1.4f, 10, 240, 5, 250};
    a0.levels[1] = {0.8f, 3, 200, 1, 199};
    a0.brightness = 12; a0.contrast = -7;
    a0.hue = 30; a0.saturation = -20; a0.lightness = 15;
    a0.colorize = true; a0.colorize_hue = 40; a0.colorize_saturation = 60;
    a0.threshold = 77; a0.posterize = 3;
    a0.curves[0] = {{0.0f, 0.0f}, {100.0f, 150.0f}, {255.0f, 255.0f}};
    a0.hsl_ranges[2][3] = 42;
    a0.color_balance.midtones[1] = -15;
    a0.color_balance.preserve_luminosity = false;
    a0.mixer.mix[1][2] = 40.5f;
    a0.mixer.constant[0] = -3.5f;
    a0.mixer.monochrome = true;

    Layer& filt = doc.add_layer("Filt");
    filt.type = LayerType::Adjustment;
    filt.adjustment.kind = Adjustment::Kind::UnsharpMask;
    filt.adjustment.unsharp_radius = 3.5f;
    filt.adjustment.unsharp_strength = 140;
    filt.adjustment.unsharp_clipping = 4;
    filt.adjustment.blur_radius = 6.5f;      // another kind's field, still carried

    doc.alpha_channels().push_back({"Saved", mask::rectangle(40, 30, 2, 2, 10, 10, false)});
    doc.guides_h().push_back(6.5f);
    doc.guides_v().push_back(11.25f);
    { Assistant as; as.kind = Assistant::Kind::Parallel; as.x0 = 1; as.y0 = 2; as.x1 = 3; as.y1 = 4; doc.assistants().push_back(as); }
    doc.set_icc(std::vector<uint8_t>{1, 2, 3, 4, 5});
    doc.metadata().set(meta::Group::Image, 0x013B, "Someone");
    doc.metadata().set_text("Note", "hello");
    doc.set_selection(mask::rectangle(40, 30, 4, 4, 14, 14, false));
    doc.set_active_layer(2);

    const std::vector<uint8_t> bytes = io::save_ora_to_memory(doc);
    std::string err;
    std::vector<std::string> warnings;
    auto b = io::load_ora_from_memory(bytes.data(), bytes.size(), &err, &warnings);
    CHECK(b && err.empty() && warnings.empty());
    CHECK(b->layer_count() == 6 && b->width() == 40 && b->height() == 30);
    CHECK(b->layer(0).name == "Back" && b->layer(4).name == "Adj");

    // Document state.
    CHECK(b->active_layer() == 2);
    CHECK(b->has_selection() && b->selection().at(8, 8) == 255 && b->selection().at(30, 20) == 0);
    CHECK(b->alpha_channels().size() == 1 && b->alpha_channels()[0].name == "Saved" && b->alpha_channels()[0].mask.at(5, 5) == 255);
    CHECK(b->guides_h().size() == 1 && std::abs(b->guides_h()[0] - 6.5f) < 0.01f);
    CHECK(b->guides_v().size() == 1 && std::abs(b->guides_v()[0] - 11.25f) < 0.01f);
    CHECK(b->assistants().size() == 1 && b->assistants()[0].kind == Assistant::Kind::Parallel && b->assistants()[0].y1 == 4);
    CHECK(b->icc() == std::vector<uint8_t>({1, 2, 3, 4, 5}));
    CHECK(b->metadata().find(meta::Group::Image, 0x013B) && b->metadata().find_text("Note"));
    CHECK(b->bit_depth() == 16);

    // Layer state.
    const Layer& L0 = b->layer(0);
    CHECK(L0.background && L0.pixels.get(1, 1).r == 10);
    const LayerStyle& s = L0.style;
    CHECK(s.drop_shadow && s.shadow_color.r == 1 && std::abs(s.shadow_opacity - 0.4f) < 0.01f);
    CHECK(std::abs(s.shadow_offset_x - 7) < 0.01f && std::abs(s.shadow_offset_y + 3) < 0.01f && std::abs(s.shadow_blur - 9) < 0.01f);
    CHECK(s.outer_glow && s.glow_color.g == 5 && std::abs(s.glow_size - 11) < 0.01f && std::abs(s.glow_opacity - 0.3f) < 0.01f);
    CHECK(s.inner_glow && s.inner_glow_color.b == 9 && std::abs(s.inner_glow_size - 12) < 0.01f && std::abs(s.inner_glow_opacity - 0.2f) < 0.01f);
    CHECK(s.stroke && s.stroke_color.r == 10 && s.stroke_width == 6 && std::abs(s.stroke_opacity - 0.6f) < 0.01f);
    CHECK(s.bevel && std::abs(s.bevel_size - 4) < 0.01f && std::abs(s.bevel_depth - 2.5f) < 0.01f && std::abs(s.bevel_angle - 200) < 0.01f);

    const Layer& G = b->layer(1);
    CHECK(G.type == LayerType::Group && !G.expanded && G.pass_through);
    CHECK(G.has_mask() && G.mask.at(3, 3) == 255 && G.mask.at(30, 3) == 0 && !G.mask_enabled);
    CHECK(std::abs(G.opacity - 0.25f) < 0.01f && G.blend == BlendMode::Dissolve);

    const Layer& M = b->layer(2);
    CHECK(M.depth == 1 && !M.visible && M.pixels.get(5, 5).r == 99 && M.clipped);
    CHECK(M.ranges.channel == BlendRanges::Channel::Blue);
    CHECK(M.ranges.source.low0 == 12 && M.ranges.source.low1 == 34);
    CHECK(M.ranges.under.high1 == 200 && M.ranges.under.high0 == 220);

    const Layer& D = b->layer(3);
    CHECK(D.depth == 1 && D.is_deep() && D.deep->data()[0] == 1234 && D.deep->data()[1] == 4321 && D.deep->data()[2] == 999);

    // Adjustment layers keep every field, not only the active kind's.
    const Adjustment& a = b->layer(4).adjustment;
    CHECK(b->layer(4).is_adjustment() && a.kind == Adjustment::Kind::Levels);
    CHECK(std::abs(a.levels[0].gamma - 1.4f) < 0.001f && a.levels[0].in_low == 10 && a.levels[0].out_high == 250);
    CHECK(std::abs(a.levels[1].gamma - 0.8f) < 0.001f && a.levels[1].in_high == 200);
    CHECK(a.brightness == 12 && a.contrast == -7);
    CHECK(a.hue == 30 && a.saturation == -20 && a.lightness == 15);
    CHECK(a.colorize && a.colorize_hue == 40 && a.colorize_saturation == 60);
    CHECK(a.threshold == 77 && a.posterize == 3);
    CHECK(a.curves[0].size() == 3 && std::abs(a.curves[0][1].second - 150) < 0.01f);
    CHECK(a.hsl_ranges[2][3] == 42);
    CHECK(a.color_balance.midtones[1] == -15 && !a.color_balance.preserve_luminosity);
    CHECK(std::abs(a.mixer.mix[1][2] - 40.5f) < 0.01f && std::abs(a.mixer.constant[0] + 3.5f) < 0.01f && a.mixer.monochrome);

    const Adjustment& f = b->layer(5).adjustment;
    CHECK(f.kind == Adjustment::Kind::UnsharpMask && f.is_filter());
    CHECK(std::abs(f.unsharp_radius - 3.5f) < 0.01f && f.unsharp_strength == 140 && f.unsharp_clipping == 4);
    CHECK(std::abs(f.blur_radius - 6.5f) < 0.01f);
}

// The project format must be lossless: every field of the document model
// comes back exactly. The native container cannot carry all of it, which is
// what test_psp_vector_compat below pins down.
// Break, join and reverse: the structural half of node editing. What they
// must not do is move the curve, so each one is checked by flattening the
// path before and after and comparing the outline it draws.
// Firn read picture tubes long before it could write one. A tube is the
// native format plus one block saying how the image divides into cells.
// A saved photo carries a thumbnail of what it now shows. The one a file
// arrived with is not reused: after a crop it would still show what was cut
// away, which is a real way for detail to leak out of a picture.
// XMP is where a photo manager keeps the title, caption, keywords,
// copyright and rating. Firn used to drop the whole packet on save while
// Exif came through whole, which is what made the loss easy to miss.
// Firn could read a Photoshop file and not write one, so anyone handed a
// PSD could edit it and had no way to hand it back. What the format is
// fussy about is lengths: a field padded to the wrong thing, or a length
// that counts its own padding, and Photoshop and GIMP both call the whole
// file corrupt rather than skipping the layer.
// TIFF is what print and archival photography run on, and the only format
// here that hands 16 bits a channel to another program without argument.
// Work handed to an image model. The queue and the compositing are the
// parts Firn owns; the backend here is a fake, so none of this needs a
// server, a model, or a network.
static void test_generate_queue() {
    using namespace std::chrono_literals;

    // A backend that returns a flat colour and, like a real one, hands back
    // a whole frame rather than only the part it was asked to change.
    auto flat_backend = [](Color c) {
        return [c](const gen::Request& r, gen::Progress& p) {
            gen::Result out;
            p.fraction.store(0.5f);
            out.image = Image(r.init.width(), r.init.height(), c);
            out.ok = true;
            return out;
        };
    };

    // Jobs run, come back in submission order, and report themselves.
    {
        gen::Queue q(flat_backend({10, 20, 30, 255}), 1);
        std::vector<uint64_t> ids;
        for (int i = 0; i < 4; ++i) {
            gen::Request r;
            r.name = "Job " + std::to_string(i);
            r.init = Image(8, 8, {0, 0, 0, 255});
            r.revision = static_cast<uint64_t>(i);
            r.layer = i;
            ids.push_back(q.submit(std::move(r)));
        }
        CHECK(ids[0] == 1 && ids[3] == 4);
        q.wait();
        std::vector<gen::Finished> done = q.drain();
        CHECK(done.size() == 4);
        CHECK(q.drain().empty());                     // draining takes them away
        CHECK(q.outstanding() == 0);
        bool ordered = true, carried = true;
        for (size_t i = 0; i < done.size(); ++i) {
            if (done[i].id != ids[i]) ordered = false;
            if (done[i].status != gen::Status::Done || !done[i].result.ok) ordered = false;
            // The request comes back with it, so a late answer can still say
            // which layer and which revision it was built from.
            if (done[i].request.revision != i || done[i].request.layer != static_cast<int>(i)) carried = false;
        }
        CHECK(ordered);
        CHECK(carried);
    }

    // A backend that fails, and one that is not there at all.
    {
        gen::Queue q([](const gen::Request&, gen::Progress&) {
            gen::Result r;
            r.error = "the far side said no";
            return r;
        }, 1);
        gen::Request r;
        r.init = Image(4, 4, {0, 0, 0, 255});
        q.submit(std::move(r));
        q.wait();
        const std::vector<gen::Finished> done = q.drain();
        CHECK(done.size() == 1 && done[0].status == gen::Status::Failed);
        CHECK(done[0].result.error == "the far side said no");
    }
    {
        gen::Queue q(nullptr, 1);
        q.submit(gen::Request{});
        q.wait();
        const std::vector<gen::Finished> done = q.drain();
        CHECK(done.size() == 1 && done[0].status == gen::Status::Failed);
    }

    // Cancelling. A queued job always stops; a running one only when its
    // backend is still willing to be stopped, and the queue reports which.
    {
        std::atomic<bool> release{false};
        std::atomic<int> started{0};
        gen::Queue q([&release, &started](const gen::Request&, gen::Progress& p) {
            started.fetch_add(1);
            p.cancellable.store(false);          // past the point of no return
            while (!release.load()) std::this_thread::sleep_for(1ms);
            gen::Result r;
            r.ok = !p.cancel.load();
            r.image = Image(4, 4, {1, 2, 3, 255});
            return r;
        }, 1);
        const uint64_t first = q.submit(gen::Request{});
        while (started.load() == 0) std::this_thread::sleep_for(1ms);
        const uint64_t queued = q.submit(gen::Request{});
        CHECK(q.cancel(queued));                 // still waiting, so it stops
        CHECK(!q.cancel(first));                 // running and not cancellable
        CHECK(!q.cancel(9999));                  // no such job
        const std::vector<gen::Job> snapshot = q.jobs();
        CHECK(snapshot.size() == 2);
        CHECK(snapshot[0].status == gen::Status::Running && !snapshot[0].cancellable);
        CHECK(snapshot[1].status == gen::Status::Queued);
        release.store(true);
        q.wait();
        const std::vector<gen::Finished> done = q.drain();
        CHECK(done.size() == 2);
        // The cancelled one never ran, so the backend saw only the first.
        CHECK(started.load() == 1);
        bool saw_cancelled = false;
        for (const gen::Finished& f : done) if (f.status == gen::Status::Cancelled) saw_cancelled = true;
        CHECK(saw_cancelled);
    }

    // Compositing is the part that keeps an edit local. Measured against a
    // real service, the area outside the mask came back 7.5 levels different
    // on average and 130 at worst; the fake backend here does the same thing
    // by handing back a whole flat frame.
    {
        Image original(64, 48);
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x)
                original.set(x, y, {static_cast<uint8_t>(x * 4), static_cast<uint8_t>(y * 5), 90, 255});
        const Image generated(64, 48, {255, 0, 0, 255});
        Mask region(64, 48, 0);
        for (int y = 10; y < 30; ++y)
            for (int x = 10; x < 40; ++x) region.at(x, y) = 255;

        Image dst = original;
        gen::composite_into(dst, generated, region, 0.0f);
        bool inside_taken = true, outside_kept = true;
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x) {
                const Color d = dst.get(x, y), o = original.get(x, y);
                if (region.at(x, y) == 255) {
                    if (d.r != 255 || d.g != 0 || d.b != 0) inside_taken = false;
                } else if (d.r != o.r || d.g != o.g || d.b != o.b) outside_kept = false;
            }
        CHECK(inside_taken);
        CHECK(outside_kept);      // the whole point: nothing outside moves

        // Feathering softens the seam without reaching past the region's own
        // spread, and still leaves the far side of the picture alone.
        Image soft = original;
        gen::composite_into(soft, generated, region, 4.0f);
        CHECK(soft.get(25, 20).r == 255);                       // deep inside
        const Color edge = soft.get(41, 20);
        CHECK(edge.r > original.get(41, 20).r && edge.r < 255);  // partly blended
        CHECK(soft.get(60, 45).r == original.get(60, 45).r);     // far corner untouched

        // A backend working at its own resolution is brought back to the
        // layer's size rather than pasted at the wrong scale.
        Image half = original;
        gen::composite_into(half, Image(32, 24, {0, 255, 0, 255}), region, 0.0f);
        CHECK(half.get(25, 20).g == 255);
        CHECK(half.get(60, 45).r == original.get(60, 45).r);

        // An empty region means the whole frame, which is the one case where
        // a backend's own output can be taken entire.
        Image all = original;
        gen::composite_into(all, generated, Mask(), 0.0f);
        CHECK(all.get(0, 0).r == 255 && all.get(63, 47).r == 255);
    }

    // Several at once, which is the reason a queue exists rather than one
    // slot: the interface stays responsive and the document is free to move.
    {
        gen::Queue q(flat_backend({7, 7, 7, 255}), 3);
        for (int i = 0; i < 9; ++i) {
            gen::Request r;
            r.init = Image(4, 4, {0, 0, 0, 255});
            q.submit(std::move(r));
        }
        q.wait();
        CHECK(q.drain().size() == 9);
    }
}

static void test_tiff() {
    Document d(40, 24);
    Layer& bg = d.add_layer("Background");
    bg.background = true;
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 40; ++x)
            bg.pixels.set(x, y, {static_cast<uint8_t>(x * 6), static_cast<uint8_t>(y * 10), 128, 255});
    meta::Metadata md;
    md.set(meta::Group::Image, 0x010F, "A Camera Co");
    d.set_metadata(std::move(md));

    const std::string path = tmp_path("firn_test.tif");
    std::string err;
    std::vector<std::string> warn;
    CHECK(io::save_tiff(d, path, &err));

    auto back = io::load_tiff(path, &err, &warn);
    CHECK(back != nullptr);
    if (!back) return;
    CHECK(back->width() == 40 && back->height() == 24);
    // Deflate with the horizontal predictor is lossless, so every pixel must
    // come back exactly, not nearly.
    bool exact = true;
    for (int y = 0; y < 24 && exact; ++y)
        for (int x = 0; x < 40; ++x) {
            const Color a = bg.pixels.get(x, y), b = back->layer(0).pixels.get(x, y);
            if (a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a) { exact = false; break; }
        }
    CHECK(exact);

    // The header, checked on the bytes: little-endian, 42, and a directory
    // whose entries are in ascending tag order, which readers rely on.
    const std::vector<uint8_t> bytes = io::save_tiff_to_memory(d);
    CHECK(bytes.size() > 16 && bytes[0] == 'I' && bytes[1] == 'I');
    auto le16 = [&bytes](size_t o) { return static_cast<uint16_t>(bytes[o] | (bytes[o + 1] << 8)); };
    auto le32 = [&bytes](size_t o) {
        return static_cast<uint32_t>(bytes[o] | (bytes[o + 1] << 8) | (bytes[o + 2] << 16) | (static_cast<uint32_t>(bytes[o + 3]) << 24));
    };
    CHECK(le16(2) == 42);
    const size_t ifd = le32(4);
    CHECK(ifd + 2 < bytes.size());
    const uint16_t n = le16(ifd);
    CHECK(n >= 10);
    uint16_t last = 0;
    bool ascending = true, saw_predictor = false, saw_compression = false;
    for (uint16_t i = 0; i < n; ++i) {
        const size_t e = ifd + 2 + static_cast<size_t>(i) * 12;
        const uint16_t tag = le16(e);
        if (tag < last) ascending = false;
        last = tag;
        if (tag == 259) { saw_compression = le16(e + 8) == 8; }
        if (tag == 317) { saw_predictor = le16(e + 8) == 2; }
    }
    CHECK(ascending);
    CHECK(saw_compression);   // Deflate
    CHECK(saw_predictor);     // horizontal differencing
    CHECK(le32(ifd + 2 + static_cast<size_t>(n) * 12) == 0);   // no second directory

    // Transparency picks up a fourth sample and says it is unassociated,
    // which is what straight alpha means.
    Document t(8, 8);
    Layer& tl = t.add_layer("Layer");
    tl.pixels = Image(8, 8, {10, 20, 30, 128});
    const std::vector<uint8_t> with_alpha = io::save_tiff_to_memory(t);
    bool has_extra = false, four = false;
    {
        const size_t i2 = static_cast<size_t>(with_alpha[4] | (with_alpha[5] << 8) | (with_alpha[6] << 16) | (static_cast<uint32_t>(with_alpha[7]) << 24));
        const uint16_t n2 = static_cast<uint16_t>(with_alpha[i2] | (with_alpha[i2 + 1] << 8));
        for (uint16_t i = 0; i < n2; ++i) {
            const size_t e = i2 + 2 + static_cast<size_t>(i) * 12;
            const uint16_t tag = static_cast<uint16_t>(with_alpha[e] | (with_alpha[e + 1] << 8));
            const uint16_t v = static_cast<uint16_t>(with_alpha[e + 8] | (with_alpha[e + 9] << 8));
            if (tag == 338 && v == 2) has_extra = true;
            if (tag == 277 && v == 4) four = true;
        }
    }
    CHECK(has_extra && four);

    // 16 bits a channel, which is the reason to reach for TIFF at all.
    Document deep(16, 12);
    Layer& dl = deep.add_layer("Deep");
    dl.background = true;
    dl.pixels.fill({100, 150, 200, 255});
    Image16 wide(16, 12);
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 16; ++x) {
            uint16_t* p = wide.data() + (static_cast<size_t>(y) * 16 + x) * 4;
            p[0] = static_cast<uint16_t>(x * 4000); p[1] = static_cast<uint16_t>(y * 5000); p[2] = 30000; p[3] = 65535;
        }
    dl.set_deep(std::move(wide));
    CHECK(deep.bit_depth() == 16);
    const std::string deep_path = tmp_path("firn_test16.tif");
    CHECK(io::save_tiff(deep, deep_path, &err));
    {
        std::ifstream df(deep_path, std::ios::binary);
        const std::vector<uint8_t> b((std::istreambuf_iterator<char>(df)), std::istreambuf_iterator<char>());
        const size_t i2 = static_cast<size_t>(b[4] | (b[5] << 8) | (b[6] << 16) | (static_cast<uint32_t>(b[7]) << 24));
        const uint16_t n2 = static_cast<uint16_t>(b[i2] | (b[i2 + 1] << 8));
        bool sixteen = false;
        for (uint16_t i = 0; i < n2; ++i) {
            const size_t e = i2 + 2 + static_cast<size_t>(i) * 12;
            if (static_cast<uint16_t>(b[e] | (b[e + 1] << 8)) != 258) continue;
            const size_t at = static_cast<size_t>(b[e + 8] | (b[e + 9] << 8) | (b[e + 10] << 16) | (static_cast<uint32_t>(b[e + 11]) << 24));
            sixteen = at + 1 < b.size() && static_cast<uint16_t>(b[at] | (b[at + 1] << 8)) == 16;
        }
        CHECK(sixteen);
    }
    // It still reads back, at the eight bits the document model shows.
    auto deep_back = io::load_tiff(deep_path, &err, &warn);
    CHECK(deep_back != nullptr);
    if (deep_back) CHECK(deep_back->layer(0).pixels.get(2, 2).g == static_cast<uint8_t>((2 * 5000) >> 8));
    std::remove(path.c_str());
    std::remove(deep_path.c_str());
}

static void test_psd_writer() {
    Document d(48, 36);
    Layer& bg = d.add_layer("Background");
    bg.background = true;
    bg.pixels.fill({200, 60, 40, 255});

    Layer& mid = d.add_layer("Middle");
    mid.pixels = Image(48, 36, {0, 0, 0, 0});
    for (int y = 4; y < 20; ++y)
        for (int x = 4; x < 24; ++x) mid.pixels.set(x, y, {40, 90, 200, 255});
    mid.blend = BlendMode::Multiply;
    mid.opacity = 0.6f;
    mid.mask = Mask(48, 36);
    std::fill(mid.mask.data(), mid.mask.data() + mid.mask.size(), 128);

    Layer& grp = d.add_layer("A group");
    grp.type = LayerType::Group;
    grp.pixels = Image();
    Layer& inner = d.add_layer("Inside");
    inner.pixels = Image(48, 36, {0, 0, 0, 0});
    inner.pixels.set(30, 30, {10, 220, 70, 255});
    inner.depth = 1;
    inner.blend = BlendMode::Screen;
    inner.visible = false;

    const std::string path = tmp_path("firn_test_write.psd");
    std::string err;
    std::vector<std::string> warn;
    CHECK(io::save_psd(d, path, &err, &warn));

    auto back = io::load_psd(path, &err, &warn);
    CHECK(back != nullptr);
    if (!back) return;
    CHECK(back->width() == 48 && back->height() == 36);
    CHECK(back->layer_count() == 4);

    const Layer& r_bg = back->layer(0);
    CHECK(r_bg.name == "Background" && r_bg.background);
    const Layer& r_mid = back->layer(1);
    CHECK(r_mid.name == "Middle");
    CHECK(r_mid.blend == BlendMode::Multiply);
    CHECK(std::abs(r_mid.opacity - 0.6f) < 0.01f);
    CHECK(r_mid.has_mask() && r_mid.mask.data()[0] == 128);
    CHECK(r_mid.pixels.get(10, 10).b == 200 && r_mid.pixels.get(40, 30).a == 0);
    const Layer& r_grp = back->layer(2);
    CHECK(r_grp.type == LayerType::Group && r_grp.name == "A group");
    const Layer& r_in = back->layer(3);
    CHECK(r_in.name == "Inside" && r_in.depth == 1);
    CHECK(r_in.blend == BlendMode::Screen && !r_in.visible);
    CHECK(r_in.pixels.get(30, 30).g == 220);

    // The structural rules the format is unforgiving about, checked on the
    // bytes rather than trusting our own reader, which is lenient enough to
    // have accepted the first broken version of this writer.
    const std::vector<uint8_t> bytes = io::save_psd_to_memory(d, nullptr);
    CHECK(bytes.size() > 64 && std::memcmp(bytes.data(), "8BPS", 4) == 0);
    auto be32 = [&bytes](size_t o) {
        return (static_cast<uint32_t>(bytes[o]) << 24) | (static_cast<uint32_t>(bytes[o + 1]) << 16) |
               (static_cast<uint32_t>(bytes[o + 2]) << 8) | bytes[o + 3];
    };
    auto be16 = [&bytes](size_t o) { return static_cast<uint16_t>((bytes[o] << 8) | bytes[o + 1]); };
    CHECK(be16(4) == 1);                                  // version 1, not PSB
    CHECK(be32(14) == 36 && be32(18) == 48);              // height then width
    CHECK(be16(22) == 8 && be16(24) == 3);                // 8-bit RGB
    size_t p = 26;
    p += 4 + be32(p);                                     // colour mode data
    p += 4 + be32(p);                                     // image resources
    const uint32_t lm_len = be32(p);
    const size_t lm_begin = p + 4;
    CHECK(lm_begin + lm_len < bytes.size());              // the merged image follows it
    p = lm_begin;
    const uint32_t li_len = be32(p);
    const size_t li_begin = p + 4;
    CHECK(li_begin + li_len <= lm_begin + lm_len);        // layer info fits its parent
    CHECK((li_len & 1) == 0);                             // and is padded even
    // Five records for four layers: a group is a divider below its members
    // and the group record above them, which is the reverse of how Firn
    // keeps it. Negative promises the merged image carries real alpha.
    CHECK(static_cast<int16_t>(be16(li_begin)) == -5);

    // Every layer record's name field must be a multiple of four bytes long,
    // counting its own length byte. Getting this wrong is what made the first
    // files this wrote unreadable everywhere but here.
    p = li_begin + 2;
    for (int i = 0; i < 5; ++i) {
        p += 16;
        const uint16_t nch = be16(p);
        p += 2 + static_cast<size_t>(nch) * 6;
        CHECK(std::memcmp(bytes.data() + p, "8BIM", 4) == 0);
        p += 12;
        const uint32_t extra = be32(p);
        const size_t extra_end = p + 4 + extra;
        p += 4;
        p += 4 + be32(p);                                 // mask data
        p += 4 + be32(p);                                 // blending ranges
        const size_t name_at = p;
        const uint8_t nlen = bytes[p];
        p += ((static_cast<size_t>(nlen) + 1 + 3) / 4) * 4;
        CHECK((p - name_at) % 4 == 0);
        CHECK(p <= extra_end);                            // the name stayed inside its record
        p = extra_end;
    }
    std::remove(path.c_str());
}

static void test_xmp() {
    const std::string packet =
        "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "  <rdf:Description rdf:about=\"\"\n"
        "    xmlns:dc=\"http://purl.org/dc/elements/1.1/\"\n"
        "    xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n"
        "    xmlns:photoshop=\"http://ns.adobe.com/photoshop/1.0/\"\n"
        "    xmp:Rating=\"4\"\n"
        "    photoshop:City=\"Reykjavik\">\n"
        "   <dc:title>\n"
        "    <rdf:Alt>\n"
        "     <rdf:li xml:lang=\"x-default\">A dog in the grass</rdf:li>\n"
        "    </rdf:Alt>\n"
        "   </dc:title>\n"
        "   <dc:subject>\n"
        "    <rdf:Bag>\n"
        "     <rdf:li>dog</rdf:li>\n"
        "     <rdf:li>summer</rdf:li>\n"
        "    </rdf:Bag>\n"
        "   </dc:subject>\n"
        "   <dc:rights>Copyright 2026 &amp; all that</dc:rights>\n"
        "   <mwg-rs:Regions rdf:parseType=\"Resource\">\n"
        "    <mwg-rs:AppliedToDimensions stDim:w=\"5712\"/>\n"
        "   </mwg-rs:Regions>\n"
        "  </rdf:Description>\n"
        " </rdf:RDF>\n"
        "</x:xmpmeta>\n<?xpacket end=\"w\"?>";

    const std::vector<meta::Entry> props = meta::parse_xmp(packet);
    auto value_of = [&props](const char* key) {
        for (const meta::Entry& e : props) if (e.key == key) return e.text();
        return std::string("(missing)");
    };
    CHECK(value_of("dc:title") == "A dog in the grass");        // an rdf:Alt
    CHECK(value_of("dc:subject") == "dog; summer");             // an rdf:Bag
    CHECK(value_of("dc:rights") == "Copyright 2026 & all that");  // entity decoded
    CHECK(value_of("xmp:Rating") == "4");                       // an attribute
    CHECK(value_of("photoshop:City") == "Reykjavik");
    CHECK(value_of("stDim:w") == "5712");
    // Structure, not content: neither the RDF scaffolding nor a property
    // whose value is itself markup becomes an entry.
    CHECK(value_of("mwg-rs:Regions") == "(missing)");
    CHECK(value_of("rdf:Description") == "(missing)");
    CHECK(value_of("x:xmpmeta") == "(missing)");

    meta::Metadata md;
    md.xmp = packet;
    md.entries = props;
    // Nothing edited: the packet must come back exactly as it went in.
    CHECK(meta::build_xmp(md) == packet);

    // Each kind of value edited in place.
    CHECK(md.set_xmp("dc:title", "A very good dog"));
    CHECK(md.set_xmp("dc:subject", "dog; winter; snow"));
    CHECK(md.set_xmp("xmp:Rating", "5"));
    CHECK(md.set_xmp("dc:description", "Taken on a walk"));   // not in the packet yet
    const std::string edited = meta::build_xmp(md);
    const std::vector<meta::Entry> back = meta::parse_xmp(edited);
    auto back_of = [&back](const char* key) {
        for (const meta::Entry& e : back) if (e.key == key) return e.text();
        return std::string("(missing)");
    };
    CHECK(back_of("dc:title") == "A very good dog");
    CHECK(back_of("dc:subject") == "dog; winter; snow");
    CHECK(back_of("xmp:Rating") == "5");
    CHECK(back_of("dc:description") == "Taken on a walk");
    CHECK(back_of("dc:rights") == "Copyright 2026 & all that");   // untouched
    CHECK(back_of("photoshop:City") == "Reykjavik");
    CHECK(edited.find("mwg-rs:Regions") != std::string::npos);    // structure survives
    CHECK(edited.find("<?xpacket end") != std::string::npos);

    // Stripping private metadata has to reach into the packet: leaving the
    // Exif GPS out but the XMP city in would not be stripping anything.
    meta::Metadata priv = md;
    priv.set(meta::Group::GPS, 0x0001, "N");
    priv.remove_private();
    CHECK(!priv.find(meta::Group::GPS, 0x0001));
    CHECK(!priv.find_xmp("photoshop:City"));
    CHECK(priv.xmp.find("photoshop:City") == std::string::npos);
    CHECK(priv.find_xmp("dc:title"));                             // not private
    CHECK(meta::parse_xmp(meta::build_xmp(priv)).size() > 0);

    // The project formats have to carry it too, or saving a photo as a
    // project and back would lose what saving it as a JPEG now keeps.
    Image img(24, 18, {200, 180, 160, 255});
    Document d(24, 18);
    Layer& b = d.add_layer("Background");
    b.background = true;
    b.pixels = img;
    meta::Metadata file_md;
    file_md.xmp = packet;
    file_md.entries = props;
    file_md.set(meta::Group::Image, 0x010F, "A Camera Co");
    d.set_metadata(file_md);
    std::string err;
    for (const char* ext : {"jpg", "png", "ora", "pspimage"}) {
        const std::string path = tmp_path((std::string("firn_test_xmp.") + ext).c_str());
        CHECK(io::save_document(d, path, &err, 92));
        meta::Metadata got;
        if (std::string(ext) == "jpg" || std::string(ext) == "png") {
            got = io::read_metadata(path);
        } else {
            std::vector<std::string> warn;
            auto back = io::load_document(path, &err, &warn);
            CHECK(back != nullptr);
            if (back) got = back->metadata();
        }
        CHECK(got.xmp == packet);                                  // byte for byte
        CHECK(got.find_xmp("dc:title") && got.find_xmp("dc:title")->text() == "A dog in the grass");
        CHECK(got.find(meta::Group::Image, 0x010F));               // Exif still there too
        std::remove(path.c_str());
    }
}

static void test_exif_thumbnail() {
    Image img(400, 300, {0, 0, 0, 255});
    for (int y = 0; y < 300; ++y)
        for (int x = 0; x < 400; ++x)
            img.set(x, y, {static_cast<uint8_t>(x * 255 / 399), static_cast<uint8_t>(y * 255 / 299), 90, 255});

    const std::vector<uint8_t> thumb = io::exif_thumbnail(img);
    CHECK(!thumb.empty() && thumb.size() < 60000);
    CHECK(thumb[0] == 0xFF && thumb[1] == 0xD8);       // a JPEG

    meta::Metadata md;
    md.set(meta::Group::Image, 0x010F, "A Camera Co");
    md.set(meta::Group::Exif, 0x829A, "1/125");

    // The thumbnail is IFD1, so parsing the block back must still see the
    // real entries and must not mistake the thumbnail for one of them.
    const std::vector<uint8_t> tiff = meta::build_tiff(md, thumb);
    CHECK(!tiff.empty());
    const meta::Metadata back = meta::parse_tiff(tiff.data(), tiff.size());
    CHECK(back.find(meta::Group::Image, 0x010F) && back.find(meta::Group::Image, 0x010F)->text() == "A Camera Co");
    CHECK(back.find(meta::Group::Exif, 0x829A));
    CHECK(back.size() == md.size());

    // IFD0's next-directory pointer has to lead to IFD1, and the offset it
    // names has to land on the thumbnail's own JPEG signature.
    auto u32 = [&tiff](size_t o) { return static_cast<uint32_t>(tiff[o] | tiff[o + 1] << 8 | tiff[o + 2] << 16 | tiff[o + 3] << 24); };
    auto u16 = [&tiff](size_t o) { return static_cast<uint16_t>(tiff[o] | tiff[o + 1] << 8); };
    const uint32_t ifd0 = u32(4);
    const uint32_t ifd1 = u32(ifd0 + 2 + static_cast<size_t>(u16(ifd0)) * 12);
    CHECK(ifd1 > 0 && ifd1 + 2 < tiff.size());
    CHECK(u16(ifd1) == 3);
    uint32_t at = 0, len = 0;
    for (int i = 0; i < 3; ++i) {
        const size_t e = ifd1 + 2 + static_cast<size_t>(i) * 12;
        if (u16(e) == 0x0201) at = u32(e + 8);
        if (u16(e) == 0x0202) len = u32(e + 8);
    }
    CHECK(at > 0 && len == thumb.size() && at + len <= tiff.size());
    CHECK(tiff[at] == 0xFF && tiff[at + 1] == 0xD8);
    CHECK(std::equal(thumb.begin(), thumb.end(), tiff.begin() + at));

    // Through a real file, and gone again when the document has no metadata.
    Document d(400, 300);
    Layer& b = d.add_layer("Background");
    b.background = true;
    b.pixels = img;
    d.set_metadata(md);
    const std::string jpg = tmp_path("firn_test_thumb.jpg");
    std::string err;
    CHECK(io::save_document(d, jpg, &err, 90));
    const meta::Metadata reread = io::read_metadata(jpg);
    CHECK(reread.find(meta::Group::Image, 0x010F));
    std::ifstream jf(jpg, std::ios::binary);
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(jf)), std::istreambuf_iterator<char>());
    // The Exif segment, wherever the encoder's own headers left room for it,
    // carries the thumbnail bytes.
    size_t app1 = 2;
    while (app1 + 4 < file.size() && !(file[app1] == 0xFF && file[app1 + 1] == 0xE1))
        app1 += 2 + (static_cast<size_t>(file[app1 + 2]) << 8 | file[app1 + 3]);
    CHECK(app1 + 4 < file.size() && file[app1 + 1] == 0xE1);
    const size_t app1_end = app1 + 2 + (static_cast<size_t>(file[app1 + 2]) << 8 | file[app1 + 3]);
    CHECK(app1_end <= file.size());
    const auto seg_begin = file.begin() + static_cast<long>(app1), seg_end = file.begin() + static_cast<long>(app1_end);
    CHECK(std::search(seg_begin, seg_end, thumb.begin(), thumb.end()) != seg_end);
    std::remove(jpg.c_str());

    // A thumbnail too big for the 64 KB segment is left out, not truncated.
    CHECK(meta::build_tiff(md, std::vector<uint8_t>(70000, 0xAB)) == meta::build_tiff(md));
    CHECK(io::exif_thumbnail(Image(4, 4, {0, 0, 0, 255})).empty());
}

static void test_picture_tube_export() {
    Document d(64, 48);
    Layer& b = d.add_layer("Background");
    b.background = true;
    b.pixels.fill({255, 255, 255, 0});
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 64; ++x)
            b.pixels.set(x, y, {static_cast<uint8_t>(x / 16 * 60), static_cast<uint8_t>(y / 16 * 80), 40, 255});

    io::TubeInfo t;
    t.step = 20; t.columns = 4; t.rows = 3; t.total = 12; t.placement = 2; t.selection = 3;
    const std::string path = tmp_path("firn_test.psptube");
    std::string err;
    CHECK(io::save_psp_tube(d, t, path, &err));

    const std::optional<io::TubeInfo> got = io::load_psp_tube_info(path);
    CHECK(got.has_value());
    CHECK(got->step == 20 && got->columns == 4 && got->rows == 3);
    CHECK(got->total == 12 && got->placement == 2 && got->selection == 3);

    // The pixels have to survive too, or the tube has no cells to stamp.
    std::vector<std::string> warn;
    auto back = io::load_document(path, &err, &warn);
    CHECK(back && back->width() == 64 && back->height() == 48);
    auto same = [](Color a, Color c) { return a.r == c.r && a.g == c.g && a.b == c.b && a.a == c.a; };
    CHECK(same(back->composite().get(8, 8), b.pixels.get(8, 8)));
    CHECK(same(back->composite().get(56, 40), b.pixels.get(56, 40)));

    // A grid the image does not divide by would give cells of uneven size.
    io::TubeInfo bad = t;
    bad.columns = 5;
    CHECK(!io::save_psp_tube(d, bad, tmp_path("firn_test_bad.psptube"), &err));
    CHECK(err.find("divide evenly") != std::string::npos);
    bad = t; bad.rows = 0;
    CHECK(!io::save_psp_tube(d, bad, tmp_path("firn_test_bad.psptube"), &err));
    std::remove(path.c_str());
}

static void test_path_editing() {
    // The points a path flattens to, rounded and sorted, so the same outline
    // walked from a different node or in the other direction compares equal.
    auto outline = [](const vec::Path& p) {
        std::vector<std::pair<int, int>> pts;
        for (const auto& q : vec::flatten(p)) pts.emplace_back(static_cast<int>(std::lround(q.first * 16)), static_cast<int>(std::lround(q.second * 16)));
        std::sort(pts.begin(), pts.end());
        // A path repeats its start, and breaking one repeats the break node,
        // so compare the set of points an outline covers, not the walk.
        pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
        return pts;
    };
    auto near_same = [](const std::vector<std::pair<int, int>>& a, const std::vector<std::pair<int, int>>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::abs(a[i].first - b[i].first) > 1 || std::abs(a[i].second - b[i].second) > 1) return false;
        return true;
    };

    // Reversing an ellipse draws the same ellipse, backwards.
    {
        vec::Object e = vec::make_ellipse(50, 40, 30, 20);
        const auto before = outline(e.paths[0]);
        vec::reverse_path(e.paths[0]);
        CHECK(near_same(outline(e.paths[0]), before));
        CHECK(e.paths[0].nodes.front().flags[0] & 1);
        CHECK(e.paths[0].nodes.back().flags[1] & 0x80);
        vec::reverse_path(e.paths[0]);
        CHECK(near_same(outline(e.paths[0]), before));
    }

    // Breaking a closed path opens it without changing what it draws.
    {
        vec::Object e = vec::make_ellipse(50, 40, 30, 20);
        const auto before = outline(e.paths[0]);
        CHECK(vec::break_path(e, 0, 2));
        CHECK(e.paths.size() == 1 && !e.paths[0].closed);
        CHECK(e.paths[0].nodes.size() == 5);           // four nodes plus the repeated one
        CHECK(!(e.paths[0].nodes.back().flags[1] & 0x80));
        CHECK(near_same(outline(e.paths[0]), before));
        // Breaking again splits the open path in two.
        CHECK(vec::break_path(e, 0, 2));
        CHECK(e.paths.size() == 2);
        CHECK(e.paths[0].nodes.size() == 3 && e.paths[1].nodes.size() == 3);
        CHECK(!vec::break_path(e, 0, 0));              // an end has nothing to break
        CHECK(!vec::break_path(e, 0, 2));              // nor does the far end
        CHECK(!vec::break_path(e, 9, 1));
    }

    // Joining puts the two halves back together as one path.
    {
        vec::Object e = vec::make_ellipse(50, 40, 30, 20);
        const auto whole = outline(e.paths[0]);
        CHECK(vec::break_path(e, 0, 2));
        CHECK(vec::break_path(e, 0, 2));
        CHECK(vec::join_paths(e, 0, 1));
        CHECK(e.paths.size() == 1 && e.paths[0].nodes.size() == 5);
        CHECK(near_same(outline(e.paths[0]), whole));
        CHECK(!vec::join_paths(e, 0, 0));
        CHECK(!vec::join_paths(e, 0, 7));
        e.paths[0].closed = true;
        vec::Path spare; spare.closed = false; spare.nodes = e.paths[0].nodes;
        e.paths.push_back(spare);
        CHECK(!vec::join_paths(e, 0, 1));              // a closed path cannot be joined
    }

    // Two separate strokes meeting at a point join at whichever ends are
    // nearest, reversing as needed, and the result is one continuous run.
    {
        vec::Object o;
        o.paths.push_back(vec::make_polygon({{0.0f, 0.0f}, {10.0f, 0.0f}, {20.0f, 0.0f}}, false).paths[0]);
        o.paths.push_back(vec::make_polygon({{40.0f, 0.0f}, {30.0f, 0.0f}, {20.0f, 0.0f}}, false).paths[0]);
        CHECK(vec::join_paths(o, 0, 1));
        CHECK(o.paths.size() == 1);
        const vec::Path& p = o.paths[0];
        CHECK(p.nodes.size() == 5);
        for (size_t i = 0; i + 1 < p.nodes.size(); ++i) CHECK(p.nodes[i].x < p.nodes[i + 1].x);
        CHECK(p.nodes.front().x == 0.0f && p.nodes.back().x == 40.0f);
    }
}

static void test_openraster_vectors() {
    Document doc(64, 48);
    doc.add_layer("Background").background = true;
    Layer& L = doc.add_layer("Shapes");
    L.type = LayerType::Vector;

    // A curved open path with real Bezier handles, a dashed stroke and caps.
    {
        vec::Object o;
        o.name = "Curve";
        vec::Path p;
        p.closed = false;
        vec::Node a; a.x = 4; a.y = 4; a.in_x = 2; a.in_y = 3; a.out_x = 9; a.out_y = 7;
        vec::Node b; b.x = 20; b.y = 30; b.in_x = 15; b.in_y = 22; b.out_x = 24; b.out_y = 33;
        b.flags[0] = 1; b.flags[1] = 0x40; b.flags[2] = 3;
        p.nodes = {a, b};
        o.paths.push_back(p);
        o.stroke.kind = vec::PaintStyle::Kind::Solid;
        o.stroke.color = {12, 34, 56, 255};
        o.stroke_width = 3.5f;
        o.miter = 7.5f;
        o.line.dashes = {4.0f, 2.0f};
        o.line.first_cap = 3;
        o.line.last_cap = 1;
        o.line.name = "Dashed";
        o.antialias = false;
        o.visible = false;
        o.attr_raw = std::vector<uint8_t>(56, 0xAB);
        L.objects.push_back(o);
    }
    // A closed shape filled with a fully specified gradient.
    {
        vec::Object o = vec::make_ellipse(30, 20, 10, 6);
        o.name = "Blob";
        o.fill.kind = vec::PaintStyle::Kind::Gradient;
        o.fill.gradient.name = "Sunset";
        o.fill.gradient.style = vec::GradientStyle::Radial;
        o.fill.gradient.angle = 77.5f;
        o.fill.gradient.center_x = 35;
        o.fill.gradient.repeats = 3;
        o.fill.gradient.invert = true;
        o.fill.gradient.colors = {{{255, 0, 0, 255}, 0, 40}, {{0, 0, 255, 255}, 100, 50}};
        o.fill.gradient.opacities = {{100, 0, 50}, {30, 100, 55}};
        o.stroke.kind = vec::PaintStyle::Kind::None;
        L.objects.push_back(o);
    }
    // A pattern fill and a stroke texture: images, which the original's shape
    // layout has nowhere to put.
    {
        vec::Object o = vec::make_rectangle(40, 30, 60, 44);
        o.name = "Patterned";
        o.fill.kind = vec::PaintStyle::Kind::Pattern;
        o.fill.pattern = std::make_shared<Image>(4, 4, Color{7, 8, 9, 255});
        o.fill.pattern_scale = 2.5f;
        o.fill.pattern_angle = 30.0f;
        o.stroke.kind = vec::PaintStyle::Kind::Solid;
        o.stroke.texture = std::make_shared<Image>(2, 2, Color{200, 200, 200, 255});
        o.stroke.texture_scale = 1.5f;
        o.stroke.texture_strength = 0.5f;
        L.objects.push_back(o);
    }
    // A group of two rectangles.
    {
        vec::Object g;
        g.name = "Pair";
        g.is_group = true;
        g.group_count = 2;
        L.objects.push_back(g);
        vec::Object r1 = vec::make_rectangle(2, 2, 8, 8);
        r1.name = "One";
        r1.fill.kind = vec::PaintStyle::Kind::Solid;
        r1.fill.color = {0, 255, 0, 255};
        L.objects.push_back(r1);
        vec::Object r2 = vec::make_rectangle(10, 2, 16, 8);
        r2.name = "Two";
        L.objects.push_back(r2);
    }
    // Text, which must stay editable text rather than becoming outlines.
    {
        vec::Object o;
        o.name = "Label";
        o.is_text = true;
        o.text.text = "Firn";
        o.text.font_family = "DejaVu Sans";
        o.text.font_path = "/usr/share/fonts/x.ttf";
        o.text.size = 17.5f;     // fractional: a whole-number field would round it
        o.text.align = 2;
        o.text.rotation = 15.0f;
        o.text.x = 6;
        o.text.y = 40;
        o.text.baseline = 13.5f;
        o.text.antialias = false;
        o.fill.kind = vec::PaintStyle::Kind::Solid;
        o.fill.color = {9, 9, 9, 255};
        L.objects.push_back(o);
    }
    L.pixels = Image(64, 48, {0, 0, 0, 0});
    doc.rasterize_vector_layer(1);

    const std::vector<uint8_t> bytes = io::save_ora_to_memory(doc);
    std::string err;
    std::vector<std::string> warnings;
    auto back = io::load_ora_from_memory(bytes.data(), bytes.size(), &err, &warnings);
    CHECK(back && err.empty() && back->layer_count() == 2);
    const Layer& R = back->layer(1);
    CHECK(R.is_vector() && R.objects.size() == 7);

    const vec::Object& curve = R.objects[0];
    CHECK(curve.name == "Curve");
    CHECK(curve.paths.size() == 1 && !curve.paths[0].closed && curve.paths[0].nodes.size() == 2);
    CHECK(std::abs(curve.paths[0].nodes[1].x - 20.0f) < 0.01f && std::abs(curve.paths[0].nodes[1].y - 30.0f) < 0.01f);
    CHECK(std::abs(curve.paths[0].nodes[0].out_x - 9.0f) < 0.01f && std::abs(curve.paths[0].nodes[1].in_y - 22.0f) < 0.01f);
    CHECK(curve.paths[0].nodes[1].flags[1] == 0x40 && curve.paths[0].nodes[1].flags[2] == 3);
    CHECK(curve.stroke.kind == vec::PaintStyle::Kind::Solid && curve.stroke.color.r == 12 && curve.stroke.color.b == 56);
    CHECK(std::abs(curve.stroke_width - 3.5f) < 0.01f && std::abs(curve.miter - 7.5f) < 0.01f);
    CHECK(curve.line.first_cap == 3 && curve.line.last_cap == 1);
    CHECK(curve.line.dashes.size() == 2 && std::abs(curve.line.dashes[0] - 4.0f) < 0.01f);
    CHECK(curve.line.name == "Dashed");
    CHECK(!curve.antialias);
    CHECK(!curve.visible);
    CHECK(curve.attr_raw.size() == 56 && curve.attr_raw[0] == 0xAB);

    const vec::Object& blob = R.objects[1];
    CHECK(blob.name == "Blob" && blob.paths.size() == 1 && blob.paths[0].closed);
    CHECK(blob.fill.kind == vec::PaintStyle::Kind::Gradient);
    const vec::Gradient& g = blob.fill.gradient;
    CHECK(g.name == "Sunset" && g.style == vec::GradientStyle::Radial);
    CHECK(std::abs(g.angle - 77.5f) < 0.01f && std::abs(g.center_x - 35.0f) < 0.01f);
    CHECK(g.repeats == 3 && g.invert);
    CHECK(g.colors.size() == 2 && g.colors[0].color.r == 255 && std::abs(g.colors[0].mid - 40.0f) < 0.01f);
    CHECK(g.opacities.size() == 2 && std::abs(g.opacities[1].opacity - 30.0f) < 0.01f && std::abs(g.opacities[1].mid - 55.0f) < 0.01f);
    CHECK(blob.stroke.kind == vec::PaintStyle::Kind::None);

    const vec::Object& pat = R.objects[2];
    CHECK(pat.fill.kind == vec::PaintStyle::Kind::Pattern);
    CHECK(pat.fill.pattern && pat.fill.pattern->width() == 4 && pat.fill.pattern->get(1, 1).r == 7);
    CHECK(std::abs(pat.fill.pattern_scale - 2.5f) < 0.01f && std::abs(pat.fill.pattern_angle - 30.0f) < 0.01f);
    CHECK(pat.stroke.texture && pat.stroke.texture->width() == 2);
    CHECK(std::abs(pat.stroke.texture_scale - 1.5f) < 0.01f && std::abs(pat.stroke.texture_strength - 0.5f) < 0.01f);

    CHECK(R.objects[3].is_group && R.objects[3].group_count == 2 && R.objects[3].name == "Pair");
    CHECK(vec::group_end(R.objects, 3) == 6);
    CHECK(R.objects[4].name == "One" && R.objects[4].fill.color.g == 255);
    CHECK(R.objects[5].name == "Two");

    const vec::Object& label = R.objects[6];
    CHECK(label.is_text && label.text.text == "Firn");
    CHECK(std::abs(label.text.size - 17.5f) < 0.01f);
    CHECK(label.text.align == 2 && !label.text.antialias);
    CHECK(std::abs(label.text.rotation - 15.0f) < 0.01f);
    CHECK(std::abs(label.text.baseline - 13.5f) < 0.01f);
    CHECK(label.text.font_path == "/usr/share/fonts/x.ttf" && label.text.font_family == "DejaVu Sans");
    CHECK(label.fill.color.r == 9);

    // The rendered cache comes back, so a project opens without re-rasterizing.
    CHECK(R.pixels.width() == 64 && R.pixels.height() == 48);

    // Projects Firn wrote before it had its own object encoding carry only
    // the native blob. The reader must still take that path, so the complete
    // decoder has to reject those bytes rather than half-read them.
    const std::vector<uint8_t> old_blob = io::vector_objects_to_bytes(L.objects);
    std::vector<vec::Object> ignored;
    CHECK(!io::decode_objects(old_blob.data(), old_blob.size(), ignored));
    std::vector<vec::Object> from_old;
    CHECK(io::vector_objects_from_bytes(old_blob.data(), old_blob.size(), from_old));
    CHECK(from_old.size() == 7 && from_old[0].name == "Curve");
    // And the new encoding refuses anything that is not its own.
    CHECK(!io::decode_objects(reinterpret_cast<const uint8_t*>("not a vector blob"), 17, ignored));
    CHECK(!io::decode_objects(nullptr, 0, ignored));
}

// The native container is what the original reads, so its shape layout is
// fixed and cannot hold everything Firn's model does. This pins what it does
// carry and what it drops, so a change to the writer that breaks the
// original's reader, or that quietly starts losing more, shows up here.
static void test_psp_vector_compat() {
    Document doc(32, 24);
    doc.add_layer("Background").background = true;
    Layer& L = doc.add_layer("Shapes");
    L.type = LayerType::Vector;
    vec::Object o = vec::make_rectangle(3, 3, 20, 18);
    o.name = "Box";
    o.stroke.kind = vec::PaintStyle::Kind::Solid;
    o.stroke.color = {11, 22, 33, 255};
    o.stroke_width = 2.5f;
    o.line.first_cap = 3;
    o.line.last_cap = 1;
    o.fill.kind = vec::PaintStyle::Kind::Gradient;
    o.fill.gradient.style = vec::GradientStyle::Sunburst;
    o.fill.gradient.colors = {{{255, 0, 0, 255}, 0, 50}, {{0, 0, 255, 255}, 100, 50}};
    // The parts the original's layout has no room for.
    o.line.dashes = {4.0f, 2.0f};
    o.visible = false;
    o.fill.pattern = std::make_shared<Image>(4, 4, Color{7, 8, 9, 255});
    L.objects.push_back(o);
    vec::Object t;
    t.is_text = true;
    t.text.text = "Hi";
    t.text.font_family = "DejaVu Sans";
    t.text.size = 17.5f;
    t.fill.kind = vec::PaintStyle::Kind::Solid;
    L.objects.push_back(t);
    L.pixels = Image(32, 24, {0, 0, 0, 0});
    doc.rasterize_vector_layer(1);

    const std::vector<uint8_t> bytes = io::save_psp_to_memory(doc);
    std::string err;
    std::vector<std::string> warnings;
    auto back = io::load_psp_from_memory(bytes.data(), bytes.size(), &err, &warnings);
    CHECK(back && err.empty() && back->layer_count() == 2);
    const Layer& R = back->layer(1);
    CHECK(R.is_vector() && R.objects.size() == 2);
    const vec::Object& b = R.objects[0];
    // What the original's shape blocks do carry.
    CHECK(b.name == "Box");
    CHECK(b.paths.size() == 1 && b.paths[0].closed && b.paths[0].nodes.size() == 4);
    CHECK(b.stroke.kind == vec::PaintStyle::Kind::Solid && b.stroke.color.g == 22);
    CHECK(std::abs(b.stroke_width - 2.5f) < 0.01f);
    CHECK(b.line.first_cap == 3 && b.line.last_cap == 1);
    CHECK(b.fill.kind == vec::PaintStyle::Kind::Gradient && b.fill.gradient.style == vec::GradientStyle::Sunburst);
    CHECK(b.fill.gradient.colors.size() == 2 && b.fill.gradient.colors[1].color.b == 255);
    CHECK(R.objects[1].is_text && R.objects[1].text.text == "Hi");
    // What it cannot. These are the reason .ora is the project format. If one
    // of them starts passing, the native writer learned something new and this
    // test should say so rather than keep pretending it did not.
    CHECK(b.line.dashes.empty());          // no room for a dash array
    CHECK(b.visible);                      // per-object visibility is not stored
    CHECK(!b.fill.pattern);                // a pattern is an image
    CHECK(std::abs(R.objects[1].text.size - 18.0f) < 0.01f);   // point size is a whole number
}

static void test_metadata() {
    meta::Metadata md;
    CHECK(md.empty());
    CHECK(md.set(meta::Group::Image, meta::tag_for_name(meta::Group::Image, "Artist"), "Nobody in particular"));
    CHECK(md.set(meta::Group::Image, 0x0110, "6"));                      // Orientation, a SHORT
    CHECK(md.set(meta::Group::Exif, 0x829A, "1/250"));                   // ExposureTime, a RATIONAL
    CHECK(md.set(meta::Group::Exif, 0x829D, "2.8"));                     // FNumber
    CHECK(md.set(meta::Group::GPS, 0x0001, "N"));
    CHECK(md.set_text("Comment", "a text chunk"));
    CHECK(!md.set(meta::Group::Image, 0x4321, "no such tag"));
    CHECK(md.find(meta::Group::Image, 0x013B)->text() == "Nobody in particular");
    CHECK(md.find(meta::Group::Image, 0x013B)->name() == std::string("Artist"));
    CHECK(md.find(meta::Group::Exif, 0x829A)->text() == "1/250");
    CHECK(md.find(meta::Group::Exif, 0x829D)->text() == "2.8");
    CHECK(md.find_text("Comment")->text() == "a text chunk");

    // Everything survives the TIFF block, in its own directory.
    const std::vector<uint8_t> tiff = meta::build_tiff(md);
    CHECK(!tiff.empty() && tiff[0] == 'I' && tiff[1] == 'I');
    const meta::Metadata back = meta::parse_tiff(tiff.data(), tiff.size());
    CHECK(back.find(meta::Group::Image, 0x013B) && back.find(meta::Group::Image, 0x013B)->text() == "Nobody in particular");
    CHECK(back.find(meta::Group::Image, 0x0110)->text() == "6");
    CHECK(back.find(meta::Group::Exif, 0x829A)->text() == "1/250");
    CHECK(back.find(meta::Group::Exif, 0x829D)->text() == "2.8");
    CHECK(back.find(meta::Group::GPS, 0x0001)->text() == "N");

    // And through real files, both ways.
    Image img(4, 4, {10, 20, 30, 255});
    const std::string png = tmp_path("firn_test_meta.png"), jpg = tmp_path("firn_test_meta.jpg");
    CHECK(io::save_png(img, png) && io::embed_metadata(png, md));
    const meta::Metadata from_png = io::read_metadata(png);
    CHECK(from_png.find(meta::Group::Image, 0x013B)->text() == "Nobody in particular");
    CHECK(from_png.find_text("Comment") && from_png.find_text("Comment")->text() == "a text chunk");
    CHECK(io::load(png).has_value());
    CHECK(io::save(img, jpg) && io::embed_metadata(jpg, md));
    const meta::Metadata from_jpg = io::read_metadata(jpg);
    CHECK(from_jpg.find(meta::Group::Exif, 0x829D)->text() == "2.8");
    CHECK(from_jpg.find(meta::Group::GPS, 0x0001)->text() == "N");
    CHECK(io::load(jpg).has_value());

    // A document carries it, and saving writes it out again.
    std::string err;
    std::vector<std::string> warn;
    auto doc = io::load_document(png, &err, &warn);
    CHECK(doc && doc->metadata().find(meta::Group::Image, 0x013B));
    const std::string out = tmp_path("firn_test_meta_out.jpg"), ora = tmp_path("firn_test_meta.ora");
    CHECK(io::save_document(*doc, out, &err));
    CHECK(io::read_metadata(out).find(meta::Group::Image, 0x013B));
    // The project format keeps it too.
    CHECK(io::save_document(*doc, ora, &err));
    auto reopened = io::load_document(ora, &err, &warn);
    CHECK(reopened && reopened->metadata().find(meta::Group::Exif, 0x829A));
    CHECK(reopened->metadata().find_text("Comment"));

    // The native container has nowhere of its own for metadata, so it rides
    // in the Firn stash. This was the last thing Firn held that a save to
    // that format used to drop.
    {
        Document d(16, 12);
        Layer& b = d.add_layer("Background");
        b.background = true;
        b.pixels.fill({90, 110, 130, 255});
        d.metadata().set(meta::Group::Image, 0x013B, "A Photographer");
        d.metadata().set(meta::Group::Exif, 0x829A, "1/250");
        d.metadata().set(meta::Group::GPS, 0x0001, "N");
        d.metadata().set_text("Comment", "a text note");
        const std::vector<uint8_t> native = io::save_psp_to_memory(d);
        std::vector<std::string> w2;
        auto back2 = io::load_psp_from_memory(native.data(), native.size(), &err, &w2);
        CHECK(back2 && back2->layer_count() == 1);
        const meta::Metadata& got = back2->metadata();
        CHECK(got.find(meta::Group::Image, 0x013B) && got.find(meta::Group::Image, 0x013B)->text() == "A Photographer");
        CHECK(got.find(meta::Group::Exif, 0x829A) && got.find(meta::Group::Exif, 0x829A)->text() == "1/250");
        CHECK(got.find(meta::Group::GPS, 0x0001) && got.find(meta::Group::GPS, 0x0001)->text() == "N");
        CHECK(got.find_text("Comment") && got.find_text("Comment")->text() == "a text note");
        // An image with no metadata must not gain a stash for nothing.
        Document plain(8, 8);
        plain.add_layer("Background").background = true;
        const std::vector<uint8_t> bare = io::save_psp_to_memory(plain);
        auto back3 = io::load_psp_from_memory(bare.data(), bare.size(), &err, &w2);
        CHECK(back3 && back3->metadata().empty());
    }

    // Stripping what identifies the photographer and the place.
    meta::Metadata priv = md;
    priv.remove_private();
    CHECK(!priv.find(meta::Group::GPS, 0x0001) && priv.find(meta::Group::Image, 0x013B));
    // An untouched entry keeps its exact bytes; only edits rewrite a value.
    meta::Metadata edited = back;
    CHECK(edited.set(meta::Group::Image, 0x013B, "Someone else"));
    CHECK(edited.find(meta::Group::Image, 0x013B)->text() == "Someone else");
    CHECK(edited.find(meta::Group::Exif, 0x829A)->value == back.find(meta::Group::Exif, 0x829A)->value);
    std::remove(png.c_str()); std::remove(jpg.c_str()); std::remove(out.c_str()); std::remove(ora.c_str());
}

int main() {
    test_icc();
    test_parsers_survive_broken_files();
    test_box_blur();
    test_inpaint_progress_and_cancel();
    test_gradient_map();
    test_lock_transparency();
    test_pass_through_groups();
    test_blend_ranges();
    test_layer_style_scales_with_the_image();
    test_clipping_masks();
    test_openraster_lossless();
    test_generate_queue();
    test_tiff();
    test_psd_writer();
    test_xmp();
    test_exif_thumbnail();
    test_picture_tube_export();
    test_path_editing();
    test_openraster_vectors();
    test_psp_vector_compat();
    test_metadata();
    test_16bit();
    test_print();
    test_json();
    test_art_effects();
    test_geo_effects();
    test_mesh_and_displace();
    test_photo_fix_suite();
    test_color_ops();
    test_warp();
    test_adjustment_layers();
    test_vector_default_bytes();
    test_vector_queries();
    test_vector_roundtrip();
    test_material_texture_and_gradient_file();
    test_selection_modify_ops();
    test_snapshot_crop_and_undo_budget();
    test_text_objects_survive_native_save();
    test_psd_import();
    test_webp_roundtrip();
    test_stroke_pressure();
    test_heal_and_color_to_alpha();
    test_symmetry();
    test_foreground_select();
    test_compound_and_mask_warp();
    test_filter_layers();
    test_layer_styles();
    test_zip();
    test_openraster();
    test_firn_stash_resolution();
    test_move_layer();
    test_edge_preserving_smooth();
    test_one_step_photo_fix();
    test_content_aware_fill();
    test_edge_directed_resample();
    test_gradient_at_point();
    test_vector_core();
    test_history_limit();
    test_brush_texture();
    test_kaleidoscope_sunburst();
    test_brush_tip();
    test_tube_info();
    test_photo_fixes();
    test_composite_region_and_speed();
    test_effects_round3();
    test_square_brush();
    test_groups_and_masks();
    test_more_shapes();
    test_effects_round2();
    test_adjust_round2();
    test_text_and_polyline();
    test_stroke_modes();
    test_effects();
    test_adjust_module();
    test_psp_writer_roundtrip();
    test_geometry();
    test_psp_reader();
    test_blend_modes();
    test_layer_structure_commands();
    test_save_formats();
    test_mask_shapes();
    test_mask_ops();
    test_selection_clips_commands();
    test_stroke_opacity_does_not_build_up();
    test_stroke_erase();
    test_flood_fill();
    test_adjustments();
    test_flip_mirror_snapshot();
    test_invert_undo_redo();
    test_layer_add_remove();
    test_composite_over();
    test_png_roundtrip();
    std::puts("core tests passed");
    return 0;
}
