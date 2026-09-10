#pragma once
// Row-band parallelism for the pixel loops that dominate on large images.
// `fn(y0, y1)` handles rows [y0, y1); small jobs run inline on the caller.
#include <algorithm>
#include <thread>
#include <vector>

namespace firn::parallel {

inline int worker_count() {
    const unsigned n = std::thread::hardware_concurrency();
    return static_cast<int>(std::clamp(n == 0 ? 4u : n, 1u, 16u));
}

template <class F>
void rows(int height, size_t work_per_row, F&& fn) {
    const int workers = worker_count();
    // Below ~2 M pixel-operations the thread start-up costs more than it saves.
    if (height <= 1 || workers <= 1 || static_cast<size_t>(height) * work_per_row < (1u << 21)) {
        fn(0, height);
        return;
    }
    const int bands = std::min(workers, height);
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(bands) - 1);
    for (int b = 1; b < bands; ++b) {
        const int y0 = height * b / bands, y1 = height * (b + 1) / bands;
        threads.emplace_back([&fn, y0, y1] { fn(y0, y1); });
    }
    fn(0, height / bands);
    for (std::thread& t : threads) t.join();
}

}  // namespace firn::parallel
