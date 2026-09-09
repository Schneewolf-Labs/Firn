// Loads every native-format sample under WindowsInstall/ (not in git) and
// fails on any file the reader rejects. Passes trivially when the backup is
// absent so CI without the original install still runs.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "firn/io_psp.h"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    const fs::path root = argc > 1 ? argv[1] : fs::path(FIRN_SOURCE_DIR) / "WindowsInstall";
    if (!fs::is_directory(root)) {
        std::puts("psp corpus: WindowsInstall/ not present, skipping");
        return 0;
    }
    int total = 0, failed = 0, warned = 0;
    for (const auto& de : fs::recursive_directory_iterator(root)) {
        if (!de.is_regular_file()) continue;
        std::string ext = de.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        if (ext != ".pspimage" && ext != ".psptube" && ext != ".pspframe" && ext != ".pspselection") continue;
        ++total;
        std::string err;
        std::vector<std::string> warnings;
        auto doc = firn::io::load_psp(de.path().string(), &err, &warnings);
        if (!doc) {
            ++failed;
            std::printf("FAIL %s: %s\n", de.path().string().c_str(), err.c_str());
            continue;
        }
        if (!warnings.empty()) ++warned;

        // Writer/reader consistency: what we write must read back identically.
        std::vector<uint8_t> rewritten = firn::io::save_psp_to_memory(*doc);
        std::string err2;
        auto again = firn::io::load_psp_from_memory(rewritten.data(), rewritten.size(), &err2, nullptr);
        bool same = again && again->width() == doc->width() && again->height() == doc->height() && again->layer_count() == doc->layer_count();
        for (size_t i = 0; same && i < doc->layer_count(); ++i) {
            const firn::Layer& a = doc->layer(i);
            const firn::Layer& b = again->layer(i);
            same = a.name == b.name && a.visible == b.visible && a.blend == b.blend && a.background == b.background &&
                   std::abs(a.opacity - b.opacity) < 0.005f && a.pixels.size_bytes() == b.pixels.size_bytes();
            if (!same) break;
            // Opaque layers are written without alpha; compare colour where alpha matches.
            const uint8_t* pa = a.pixels.data();
            const uint8_t* pb = b.pixels.data();
            for (size_t k = 0; k < a.pixels.size_bytes(); k += 4) {
                const bool alpha_ok = pa[k + 3] == pb[k + 3] || (a.background && pb[k + 3] == 255);
                const bool colour_ok = pa[k + 3] == 0 || (pa[k] == pb[k] && pa[k + 1] == pb[k + 1] && pa[k + 2] == pb[k + 2]);
                if (!alpha_ok || !colour_ok) { same = false; break; }
            }
        }
        if (!same) {
            ++failed;
            std::printf("FAIL roundtrip %s: %s\n", de.path().string().c_str(), err2.c_str());
        }
    }
    std::printf("psp corpus: %d files, %d failed, %d with warnings\n", total, failed, warned);
    return failed ? 1 : 0;
}
