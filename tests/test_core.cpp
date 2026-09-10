// Minimal assert-based tests; no framework dependency yet.
#include <chrono>
#include <cmath>
#include <cstdio>
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
#include "firn/document.h"
#include "firn/icc.h"
#include "firn/io.h"
#include "firn/io_psp.h"
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

    Mask p = mask::polygon(10, 10, {{1, 1}, {8, 1}, {8, 8}, {1, 8}}, false);  // a square
    CHECK(p.at(1, 1) == 255 && p.at(7, 7) == 255 && p.at(8, 8) == 0 && p.at(0, 4) == 0);
    Mask tri = mask::polygon(10, 10, {{0, 0}, {10, 0}, {0, 10}}, true);
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

    hist.run(doc, std::make_unique<LayerPropertiesCommand>(1, doc.props(1), LayerProps{"Renamed", true, 0.5f, BlendMode::Multiply}));
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
    adjust::Lut id = adjust::curve_lut({{0, 0}, {255, 255}});
    CHECK(id[0] == 0 && id[100] == 100 && id[255] == 255);
    adjust::Lut s = adjust::curve_lut({{0, 0}, {64, 32}, {192, 224}, {255, 255}});
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
    Mask line = mask::polyline(20, 10, {{2, 5.5f}, {17, 5.5f}}, 4.0f, true);
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
    V.objects.push_back(vec::make_polygon({{150, 5}, {160, 5}, {160, 10}}, true));  // a plain shape first
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
    auto sp = mask::smooth_polygon({{0, 0}, {10, 0}, {10, 10}, {0, 10}}, 50, true);
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
        x.curves[1] = {{0, 0}, {128, 200}, {255, 255}};
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
    Image a = img; photo::auto_color_balance(a, 100);
    CHECK(std::abs(a.get(8, 8).r - a.get(8, 8).b) < std::abs(img.get(8, 8).r - img.get(8, 8).b));   // less blue cast
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

int main() {
    test_icc();
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
