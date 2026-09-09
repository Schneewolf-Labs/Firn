// firn-convert: flatten any supported image (including the native container)
// to PNG/JPEG/BMP/TGA from the command line. Also prints the layer stack.
#include <cstdio>
#include <string>
#include <vector>

#include "firn/blend.h"
#include "firn/io.h"
#include "firn/io_psp.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: firn-convert <input> [output.png]\n");
        return 2;
    }
    std::string err;
    std::vector<std::string> warnings;
    auto doc = firn::io::load_document(argv[1], &err, &warnings);
    if (!doc) {
        std::fprintf(stderr, "%s: %s\n", argv[1], err.c_str());
        return 1;
    }
    std::printf("%s: %dx%d, %zu layer(s)\n", argv[1], doc->width(), doc->height(), doc->layer_count());
    for (size_t i = doc->layer_count(); i-- > 0;) {
        const firn::Layer& L = doc->layer(i);
        std::printf("  [%zu] %-28s %s opacity %3d%%  %s%s\n", i, L.name.c_str(), L.visible ? "shown " : "hidden",
                    static_cast<int>(L.opacity * 100 + 0.5f), firn::blend_mode_name(L.blend), L.background ? "  (background)" : "");
    }
    for (const auto& w : warnings) std::printf("  warning: %s\n", w.c_str());
    if (argc >= 3) {
        if (!firn::io::save(doc->composite(), argv[2], &err)) {
            std::fprintf(stderr, "%s: %s\n", argv[2], err.c_str());
            return 1;
        }
        std::printf("wrote %s\n", argv[2]);
    }
    return 0;
}
