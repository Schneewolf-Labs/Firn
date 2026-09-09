// Minimal assert-based tests; no framework dependency yet.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "firn/commands.h"
#include "firn/document.h"
#include "firn/io.h"
#include "firn/io_psp.h"
#include "firn/mask.h"
#include "firn/raster.h"

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
    s.add_point(24, 16);  // many overlapping stamps across the centre
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
    CHECK(c.a == 0 && c.r == 10);  // colour kept, alpha cleared
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
    raster::greyscale(img);
    CHECK(img.get(0, 0).r == 76 && img.get(0, 0).g == 76);
    CHECK(img.get(1, 0).r == 29);

    Image bc(1, 1, {100, 100, 100, 255});
    raster::brightness_contrast(bc, 50, 0);
    CHECK(bc.get(0, 0).r == 150);
    raster::brightness_contrast(bc, 0, -100);  // full negative contrast collapses to grey
    CHECK(bc.get(0, 0).r == 128);

    // Gaussian blur of a solid image is a no-op and preserves alpha.
    Image solid(8, 8, {40, 80, 120, 255});
    raster::gaussian_blur(solid, 2.0f);
    Color c = solid.get(4, 4);
    CHECK(c.r == 40 && c.g == 80 && c.b == 120 && c.a == 255);
    // A transparent neighbour must not bleed black into an opaque pixel's colour.
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
    // Luminance keeps the bottom's hue: a grey top over pure red gives a red-ish result.
    Color l = px(BlendMode::Luminance, {255, 0, 0, 255}, {128, 128, 128, 255});
    CHECK(l.r > l.g && l.g == l.b);
    // Hue of a grey source is undefined (zero saturation) -> result is grey.
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
    // Shrinking averages: black+white -> grey.
    Image avg = raster::resample(two, 1, 1, raster::Filter::Bilinear);
    CHECK(avg.get(0, 0).r >= 127 && avg.get(0, 0).r <= 128);
    // Transparent neighbours don't bleed colour.
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

int main() {
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
