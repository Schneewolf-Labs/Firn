// Loads every native-format sample under WindowsInstall/ (not in git) and
// fails on any file the reader rejects. Passes trivially when the backup is
// absent so CI without the original install still runs.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    int total = 0, failed = 0, warned = 0, compared = 0, mismatched = 0;
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

        // Fidelity: our composite against the one the original stored (when
        // it is channel data, not a JPEG). Mean channel error over opaque
        // pixels should be tiny; report the worst files.
        {
            std::ifstream f(de.path(), std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (auto stored = firn::io::load_psp_stored_composite(bytes.data(), bytes.size())) {
                const firn::Image ours = doc->composite();
                if (stored->width() == ours.width() && stored->height() == ours.height()) {
                    double sum = 0; long n = 0;
                    for (size_t k = 0; k < ours.size_bytes(); k += 4) {
                        const uint8_t* a = ours.data() + k; const uint8_t* b = stored->data() + k;
                        if (a[3] == 0 && b[3] == 0) continue;
                        // Compare color over a white background so alpha differences count too.
                        for (int c = 0; c < 3; ++c) {
                            const int ac = (a[c] * a[3] + 255 * (255 - a[3])) / 255, bc = (b[c] * b[3] + 255 * (255 - b[3])) / 255;
                            sum += std::abs(ac - bc); ++n;
                        }
                    }
                    const double mean = n ? sum / n : 0.0;
                    ++compared;
                    if (mean > 2.0) { ++mismatched; std::printf("MISMATCH %.2f %s\n", mean, de.path().string().c_str()); }
                }
            }
        }

        // Writer/reader consistency: what we write must read back identically.
        std::vector<uint8_t> rewritten = firn::io::save_psp_to_memory(*doc);
        std::string err2;
        auto again = firn::io::load_psp_from_memory(rewritten.data(), rewritten.size(), &err2, nullptr);
        bool same = again && again->width() == doc->width() && again->height() == doc->height() && again->layer_count() == doc->layer_count() &&
                    again->alpha_channels().size() == doc->alpha_channels().size();
        for (size_t i = 0; same && i < doc->layer_count(); ++i) {
            const firn::Layer& a = doc->layer(i);
            const firn::Layer& b = again->layer(i);
            same = a.name == b.name && a.visible == b.visible && a.blend == b.blend && a.background == b.background &&
                   a.type == b.type && a.depth == b.depth && a.has_mask() == b.has_mask() && a.mask_enabled == b.mask_enabled &&
                   std::abs(a.opacity - b.opacity) < 0.005f && a.pixels.size_bytes() == b.pixels.size_bytes();
            if (same && a.has_mask()) same = std::equal(a.mask.data(), a.mask.data() + a.mask.size(), b.mask.data());
            if (!same) break;
            // Opaque layers are written without alpha; compare color where alpha matches.
            const uint8_t* pa = a.pixels.data();
            const uint8_t* pb = b.pixels.data();
            for (size_t k = 0; k < a.pixels.size_bytes(); k += 4) {
                const bool alpha_ok = pa[k + 3] == pb[k + 3] || (a.background && pb[k + 3] == 255);
                const bool color_ok = pa[k + 3] == 0 || (pa[k] == pb[k] && pa[k + 1] == pb[k + 1] && pa[k + 2] == pb[k + 2]);
                if (!alpha_ok || !color_ok) { same = false; break; }
            }
        }
        if (!same) {
            ++failed;
            std::printf("FAIL roundtrip %s: %s\n", de.path().string().c_str(), err2.c_str());
        }
    }
    std::printf("psp corpus: %d files, %d failed, %d with warnings; composite compared for %d, %d beyond tolerance\n",
                total, failed, warned, compared, mismatched);
    return (failed || mismatched) ? 1 : 0;
}
