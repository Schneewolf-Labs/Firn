// Minimal assert-based tests; no framework dependency yet.
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "psp9/commands.h"
#include "psp9/document.h"
#include "psp9/io.h"

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

using namespace psp9;

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

int main() {
    test_invert_undo_redo();
    test_layer_add_remove();
    test_composite_over();
    test_png_roundtrip();
    std::puts("core tests passed");
    return 0;
}
