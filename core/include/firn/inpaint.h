#pragma once
#include <cstdint>
#include <functional>

#include "firn/image.h"
#include "firn/mask.h"

// Content-aware fill: replaces a region with content synthesized from the
// rest of the picture, so an unwanted object can be selected and removed.
// Exemplar synthesis over a resolution pyramid (the PatchMatch approach):
// every patch inside the hole repeatedly looks for the most similar patch
// in the untouched part of the image, and the hole is rebuilt from those
// matches. No model or training data is involved.
namespace firn::inpaint {

struct Options {
    int patch = 7;        // odd; the patch size matched against the image
    int passes = 6;       // search and vote rounds per pyramid level
    int max_side = 2048;  // work at this resolution at most, then scale back up
    uint32_t seed = 1;
    // Called from whatever thread runs the fill, with how far along it is in
    // 0..1. Returning false stops early and leaves the image untouched, so a
    // caller can put this on a worker thread and still cancel it.
    std::function<bool(float)> on_progress;
};

// Fills wherever `region` is non-zero, reading the rest of `img` for
// material. Partial mask values feather the result back over the original.
// Returns false when `on_progress` asked it to stop, in which case `img` is
// left as it was.
bool content_aware_fill(Image& img, const Mask& region, const Options& opt = {});

}  // namespace firn::inpaint
