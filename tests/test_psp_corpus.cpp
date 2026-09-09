// Loads every native-format sample under WindowsInstall/ (not in git) and
// fails on any file the reader rejects. Passes trivially when the backup is
// absent so CI without the original install still runs.
#include <algorithm>
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
    }
    std::printf("psp corpus: %d files, %d failed, %d with warnings\n", total, failed, warned);
    return failed ? 1 : 0;
}
