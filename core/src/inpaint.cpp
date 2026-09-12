// Content-aware fill by exemplar synthesis (PatchMatch): a randomized
// nearest-neighbor search pairs every patch of the hole with a similar
// patch of known image, then the hole is rebuilt by averaging what those
// matches vote for. Runs coarse to fine so large holes pick up structure
// and not just texture.
#include "firn/inpaint.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "firn/parallel.h"
#include "firn/raster.h"

namespace firn::inpaint {

namespace {

struct Level {
    Image img;
    std::vector<uint8_t> hole;   // 1 where the pixel must be synthesized
    int w = 0, h = 0;
};

inline uint32_t next_random(uint32_t& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

// Half-resolution copy of a level; a pixel is a hole if any of its four
// sources was, so the hole never shrinks away between levels.
Level downscale(const Level& in) {
    Level out;
    out.w = std::max(1, in.w / 2);
    out.h = std::max(1, in.h / 2);
    out.img = Image(out.w, out.h);
    out.hole.assign(static_cast<size_t>(out.w) * out.h, 0);
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            int acc[4] = {0, 0, 0, 0}, n = 0;
            bool hole = false;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = std::min(x * 2 + dx, in.w - 1), sy = std::min(y * 2 + dy, in.h - 1);
                    const size_t si = static_cast<size_t>(sy) * in.w + sx;
                    if (in.hole[si]) { hole = true; continue; }
                    const uint8_t* p = in.img.data() + si * 4;
                    for (int c = 0; c < 4; ++c) acc[c] += p[c];
                    ++n;
                }
            const size_t oi = static_cast<size_t>(y) * out.w + x;
            out.hole[oi] = hole ? 1 : 0;
            uint8_t* d = out.img.data() + oi * 4;
            for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(n ? acc[c] / n : 0);
            if (!n) d[3] = 255;
        }
    return out;
}

// Seeds the hole at the coarsest level: the average of what is known, then
// a few smoothing passes so the search starts from something continuous.
void seed_hole(Level& lv) {
    long long acc[3] = {0, 0, 0};
    long n = 0;
    for (size_t i = 0; i < lv.hole.size(); ++i)
        if (!lv.hole[i]) { const uint8_t* p = lv.img.data() + i * 4; for (int c = 0; c < 3; ++c) acc[c] += p[c]; ++n; }
    const uint8_t mean[3] = {static_cast<uint8_t>(n ? acc[0] / n : 128), static_cast<uint8_t>(n ? acc[1] / n : 128), static_cast<uint8_t>(n ? acc[2] / n : 128)};
    for (size_t i = 0; i < lv.hole.size(); ++i)
        if (lv.hole[i]) { uint8_t* p = lv.img.data() + i * 4; for (int c = 0; c < 3; ++c) p[c] = mean[c]; p[3] = 255; }
    for (int pass = 0; pass < 8; ++pass) {
        Image prev = lv.img;
        for (int y = 0; y < lv.h; ++y)
            for (int x = 0; x < lv.w; ++x) {
                const size_t i = static_cast<size_t>(y) * lv.w + x;
                if (!lv.hole[i]) continue;
                int sum[3] = {0, 0, 0}, cnt = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int px = x + dx, py = y + dy;
                        if (px < 0 || py < 0 || px >= lv.w || py >= lv.h) continue;
                        const uint8_t* q = prev.data() + (static_cast<size_t>(py) * lv.w + px) * 4;
                        for (int c = 0; c < 3; ++c) sum[c] += q[c];
                        ++cnt;
                    }
                uint8_t* p = lv.img.data() + i * 4;
                for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>(sum[c] / cnt);
            }
    }
}

// Where a patch may be taken from: fully inside the image and clear of the
// hole. A summed-area table of the hole makes the test constant time.
struct SourceMap {
    int w = 0, h = 0, r = 0;
    std::vector<int> sat;
    std::vector<uint8_t> ok;
    SourceMap(const Level& lv, int radius) : w(lv.w), h(lv.h), r(radius) {
        sat.assign(static_cast<size_t>(w + 1) * (h + 1), 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                sat[static_cast<size_t>(y + 1) * (w + 1) + x + 1] =
                    lv.hole[static_cast<size_t>(y) * w + x] + sat[static_cast<size_t>(y) * (w + 1) + x + 1] +
                    sat[static_cast<size_t>(y + 1) * (w + 1) + x] - sat[static_cast<size_t>(y) * (w + 1) + x];
        ok.assign(static_cast<size_t>(w) * h, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) ok[static_cast<size_t>(y) * w + x] = valid(x, y) ? 1 : 0;
    }
    bool inside(int x, int y) const { return x - r >= 0 && y - r >= 0 && x + r < w && y + r < h; }
    bool valid(int x, int y) const {
        if (!inside(x, y)) return false;
        const int x0 = x - r, y0 = y - r, x1 = x + r + 1, y1 = y + r + 1;
        const int holes = sat[static_cast<size_t>(y1) * (w + 1) + x1] - sat[static_cast<size_t>(y0) * (w + 1) + x1] -
                          sat[static_cast<size_t>(y1) * (w + 1) + x0] + sat[static_cast<size_t>(y0) * (w + 1) + x0];
        return holes == 0;
    }
    bool is_ok(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h && ok[static_cast<size_t>(y) * w + x]; }
};

// Distance between the patch at a (inside the hole) and a candidate source
// patch at b. Pixels of the target patch that are known image count for far
// more than pixels still being synthesized: without that, a hole seeded flat
// matches any flat region and the fill stays flat forever.
int patch_distance(const Level& lv, int ax, int ay, int bx, int by, int r) {
    const uint8_t* base = lv.img.data();
    float total = 0.0f, weight = 0.0f;
    for (int dy = -r; dy <= r; ++dy) {
        const int ayy = std::clamp(ay + dy, 0, lv.h - 1), byy = by + dy;
        for (int dx = -r; dx <= r; ++dx) {
            const int axx = std::clamp(ax + dx, 0, lv.w - 1), bxx = bx + dx;
            const size_t ai = static_cast<size_t>(ayy) * lv.w + axx;
            const uint8_t* qa = base + ai * 4;
            const uint8_t* qb = base + (static_cast<size_t>(byy) * lv.w + bxx) * 4;
            const float w = lv.hole[ai] ? 0.25f : 1.0f;
            int sq = 0;
            for (int c = 0; c < 3; ++c) { const int d = qa[c] - qb[c]; sq += d * d; }
            total += w * static_cast<float>(sq);
            weight += w;
        }
    }
    if (weight <= 0.0f) return INT32_MAX;
    return static_cast<int>(total / weight);
}

}  // namespace

bool content_aware_fill(Image& img, const Mask& region, const Options& opt) {
    if (img.empty() || region.empty()) return true;
    const int W = img.width(), H = img.height();
    if (region.width() != W || region.height() != H) return true;
    const raster::Rect box = region.bounds();
    if (box.empty()) return true;

    const int r = std::max(1, (std::max(3, opt.patch | 1) - 1) / 2);

    // Work at a bounded resolution; a hole a few thousand pixels across does
    // not need full detail to be filled convincingly.
    const double scale = std::min(1.0, static_cast<double>(opt.max_side) / std::max(W, H));
    const int ww = std::max(1, static_cast<int>(W * scale)), hh = std::max(1, static_cast<int>(H * scale));
    Level fine;
    fine.w = ww;
    fine.h = hh;
    fine.img = scale < 1.0 ? raster::resample(img, ww, hh, raster::Filter::Bilinear) : img;
    fine.hole.assign(static_cast<size_t>(ww) * hh, 0);
    {
        // A pixel is a hole if the mask selects it at all, so a feathered
        // edge is synthesized too and then blended back by its coverage.
        for (int y = 0; y < hh; ++y)
            for (int x = 0; x < ww; ++x) {
                const int sx = std::min(W - 1, static_cast<int>(x / scale)), sy = std::min(H - 1, static_cast<int>(y / scale));
                fine.hole[static_cast<size_t>(y) * ww + x] = region.at(sx, sy) ? 1 : 0;
            }
    }
    long holes = 0;
    for (uint8_t v : fine.hole) holes += v;
    if (!holes) return true;

    // Pyramid, coarse enough that the hole is a handful of patches across.
    std::vector<Level> levels{fine};
    while (levels.back().w > 4 * opt.patch && levels.back().h > 4 * opt.patch && levels.size() < 6)
        levels.push_back(downscale(levels.back()));
    // A level can be so coarse that the hole leaves room for no whole patch
    // of known image. There is nothing to copy from at that size, so drop
    // those levels rather than abandoning the fill.
    while (levels.size() > 1) {
        const SourceMap probe(levels.back(), r);
        if (std::find(probe.ok.begin(), probe.ok.end(), uint8_t{1}) != probe.ok.end()) break;
        levels.pop_back();
    }
    seed_hole(levels.back());

    std::vector<int> nnf;   // two ints per pixel: the source it copies from
    uint32_t rng = opt.seed ? opt.seed : 1u;

    // For the progress fraction: every level's pass count, added up.
    float total_passes = 0.0f, done_passes = 0.0f;
    for (int li = static_cast<int>(levels.size()) - 1; li >= 0; --li)
        total_passes += static_cast<float>(std::max(1, opt.passes + 4 * li));
    if (total_passes <= 0.0f) total_passes = 1.0f;

    for (int li = static_cast<int>(levels.size()) - 1; li >= 0; --li) {
        Level& lv = levels[li];
        const SourceMap src(lv, r);
        // Nothing outside the hole can be used as a source at this size.
        bool any_source = false;
        for (uint8_t v : src.ok) if (v) { any_source = true; break; }
        if (!any_source) continue;

        std::vector<int> next(static_cast<size_t>(lv.w) * lv.h * 2, -1);
        if (nnf.empty()) {
            for (int y = 0; y < lv.h; ++y)
                for (int x = 0; x < lv.w; ++x) {
                    const size_t i = static_cast<size_t>(y) * lv.w + x;
                    if (!lv.hole[i]) continue;
                    for (int tries = 0; tries < 64; ++tries) {
                        const int sx = r + static_cast<int>(next_random(rng) % static_cast<uint32_t>(std::max(1, lv.w - 2 * r)));
                        const int sy = r + static_cast<int>(next_random(rng) % static_cast<uint32_t>(std::max(1, lv.h - 2 * r)));
                        if (src.is_ok(sx, sy)) { next[i * 2] = sx; next[i * 2 + 1] = sy; break; }
                    }
                }
        } else {
            // Carry the previous level's matches over, doubled.
            const Level& prev = levels[li + 1];
            for (int y = 0; y < lv.h; ++y)
                for (int x = 0; x < lv.w; ++x) {
                    const size_t i = static_cast<size_t>(y) * lv.w + x;
                    if (!lv.hole[i]) continue;
                    const int px = std::min(x / 2, prev.w - 1), py = std::min(y / 2, prev.h - 1);
                    const size_t pi = static_cast<size_t>(py) * prev.w + px;
                    int sx = nnf[pi * 2] * 2 + (x & 1), sy = nnf[pi * 2 + 1] * 2 + (y & 1);
                    if (nnf[pi * 2] < 0 || !src.is_ok(sx, sy)) {
                        sx = sy = -1;
                        for (int tries = 0; tries < 32; ++tries) {
                            const int cx = r + static_cast<int>(next_random(rng) % static_cast<uint32_t>(std::max(1, lv.w - 2 * r)));
                            const int cy = r + static_cast<int>(next_random(rng) % static_cast<uint32_t>(std::max(1, lv.h - 2 * r)));
                            if (src.is_ok(cx, cy)) { sx = cx; sy = cy; break; }
                        }
                    }
                    next[i * 2] = sx;
                    next[i * 2 + 1] = sy;
                }
        }
        nnf.swap(next);

        // Coarse levels are tiny and decide the structure of the fill, so
        // they get many more rounds than the expensive fine ones.
        const int passes = std::max(1, opt.passes + 4 * li);
        for (int pass = 0; pass < passes; ++pass) {
            // Progress is reported per pass, which is the only granularity a
            // caller can act on without slowing the search down.
            if (opt.on_progress) {
                done_passes += 1.0f;
                if (!opt.on_progress(std::min(1.0f, done_passes / total_passes))) return false;
            }
            const bool forward = (pass % 2) == 0;
            std::vector<int> dist(static_cast<size_t>(lv.w) * lv.h, INT32_MAX);
            // Search: propagate a neighbor's match, then look randomly
            // around the current one with a shrinking radius.
            for (int k = 0; k < lv.w * lv.h; ++k) {
                const int idx = forward ? k : lv.w * lv.h - 1 - k;
                const int x = idx % lv.w, y = idx / lv.w;
                const size_t i = static_cast<size_t>(idx);
                if (!lv.hole[i] || nnf[i * 2] < 0) continue;
                int bx = nnf[i * 2], by = nnf[i * 2 + 1];
                int best = patch_distance(lv, x, y, bx, by, r);
                const int step = forward ? -1 : 1;
                for (int axis = 0; axis < 2; ++axis) {
                    const int nx = axis == 0 ? x + step : x, ny = axis == 0 ? y : y + step;
                    if (nx < 0 || ny < 0 || nx >= lv.w || ny >= lv.h) continue;
                    const size_t ni = static_cast<size_t>(ny) * lv.w + nx;
                    if (!lv.hole[ni] || nnf[ni * 2] < 0) continue;
                    const int cx = nnf[ni * 2] - (axis == 0 ? step : 0), cy = nnf[ni * 2 + 1] - (axis == 0 ? 0 : step);
                    if (!src.is_ok(cx, cy)) continue;
                    const int d = patch_distance(lv, x, y, cx, cy, r);
                    if (d < best) { best = d; bx = cx; by = cy; }
                }
                for (int radius = std::max(lv.w, lv.h); radius > 1; radius /= 2) {
                    const int cx = bx + static_cast<int>(next_random(rng) % static_cast<uint32_t>(2 * radius + 1)) - radius;
                    const int cy = by + static_cast<int>(next_random(rng) % static_cast<uint32_t>(2 * radius + 1)) - radius;
                    if (!src.is_ok(cx, cy)) continue;
                    const int d = patch_distance(lv, x, y, cx, cy, r);
                    if (d < best) { best = d; bx = cx; by = cy; }
                }
                nnf[i * 2] = bx;
                nnf[i * 2 + 1] = by;
                dist[i] = best;
            }

            // Vote: every patch that covers a hole pixel offers its source's
            // pixel, and the average of those is the new value.
            // Votes are weighted by how well the patch matched, so a patch
            // that fits dominates instead of being averaged away by its
            // neighbors: a plain mean over every overlapping patch is a blur.
            double dsum = 0;
            long dn = 0;
            for (size_t i = 0; i < dist.size(); ++i) if (lv.hole[i] && dist[i] != INT32_MAX) { dsum += dist[i]; ++dn; }
            const float temperature = dn ? std::max(1.0f, static_cast<float>(dsum / dn) * 0.5f) : 1.0f;
            std::vector<float> acc(static_cast<size_t>(lv.w) * lv.h * 3, 0.0f);
            std::vector<float> wsum(static_cast<size_t>(lv.w) * lv.h, 0.0f);
            for (int y = 0; y < lv.h; ++y)
                for (int x = 0; x < lv.w; ++x) {
                    const size_t i = static_cast<size_t>(y) * lv.w + x;
                    if (!lv.hole[i] || nnf[i * 2] < 0) continue;
                    const int sx = nnf[i * 2], sy = nnf[i * 2 + 1];
                    const float pw = std::exp(-static_cast<float>(dist[i]) / temperature);
                    for (int dy = -r; dy <= r; ++dy)
                        for (int dx = -r; dx <= r; ++dx) {
                            const int tx = x + dx, ty = y + dy;
                            if (tx < 0 || ty < 0 || tx >= lv.w || ty >= lv.h) continue;
                            const size_t ti = static_cast<size_t>(ty) * lv.w + tx;
                            if (!lv.hole[ti]) continue;
                            const uint8_t* s = lv.img.data() + (static_cast<size_t>(sy + dy) * lv.w + (sx + dx)) * 4;
                            for (int c = 0; c < 3; ++c) acc[ti * 3 + c] += pw * s[c];
                            wsum[ti] += pw;
                        }
                }
            for (size_t i = 0; i < wsum.size(); ++i) {
                if (!lv.hole[i] || wsum[i] <= 1e-6f) continue;
                uint8_t* d = lv.img.data() + i * 4;
                for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>(std::clamp(acc[i * 3 + c] / wsum[i], 0.0f, 255.0f) + 0.5f);
                d[3] = 255;
            }
            // Averaging overlapping patches is what keeps a synthesized area
            // softer than the picture around it. The last pass of the finest
            // level therefore takes each pixel straight from the patch that
            // matched it, which restores the grain.
            if (li == 0 && pass == passes - 1) {
                const Image voted = lv.img;
                for (size_t i = 0; i < lv.hole.size(); ++i) {
                    if (!lv.hole[i] || nnf[i * 2] < 0) continue;
                    const uint8_t* s = voted.data() + (static_cast<size_t>(nnf[i * 2 + 1]) * lv.w + nnf[i * 2]) * 4;
                    uint8_t* d = lv.img.data() + i * 4;
                    for (int c = 0; c < 3; ++c) d[c] = s[c];
                    d[3] = 255;
                }
            }
        }

        if (li > 0) {
            // Hand the filled hole down to the finer level, keeping its own
            // known pixels.
            Level& finer = levels[li - 1];
            const Image up = raster::resample(lv.img, finer.w, finer.h, raster::Filter::Bilinear);
            for (size_t i = 0; i < finer.hole.size(); ++i)
                if (finer.hole[i]) std::memcpy(finer.img.data() + i * 4, up.data() + i * 4, 4);
        }
    }

    // Back to full resolution, blended by the mask so a feathered selection
    // keeps its soft edge.
    const Image filled = scale < 1.0 ? raster::resample(levels.front().img, W, H, raster::Filter::Bilinear) : levels.front().img;
    for (int y = box.y0; y < box.y1; ++y)
        for (int x = box.x0; x < box.x1; ++x) {
            const float m = region.at(x, y) / 255.0f;
            if (m <= 0.0f) continue;
            uint8_t* d = img.data() + (static_cast<size_t>(y) * W + x) * 4;
            const uint8_t* s = filled.data() + (static_cast<size_t>(y) * W + x) * 4;
            for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>(d[c] + (s[c] - d[c]) * m + 0.5f);
            d[3] = static_cast<uint8_t>(std::max<int>(d[3], static_cast<int>(255 * m)));
        }
    return true;
}

}  // namespace firn::inpaint
