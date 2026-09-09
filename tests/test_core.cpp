// Minimal assert-based tests; no framework dependency yet.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "firn/adjust.h"
#include "firn/effects.h"
#include "firn/commands.h"
#include "firn/document.h"
#include "firn/io.h"
#include "firn/io_psp.h"
#include "firn/mask.h"
#include "firn/raster.h"
#include "firn/text.h"

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

int main() {
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
