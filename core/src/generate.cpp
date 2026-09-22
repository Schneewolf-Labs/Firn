#include "firn/generate.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "firn/parallel.h"
#include "firn/raster.h"

namespace firn::gen {

const char* status_name(Status s) {
    switch (s) {
        case Status::Queued: return "queued";
        case Status::Running: return "running";
        case Status::Done: return "done";
        case Status::Failed: return "failed";
        case Status::Cancelled: return "cancelled";
    }
    return "";
}

const char* disposition_name(Disposition d) {
    switch (d) {
        case Disposition::IntoRegion: return "into_region";
        case Disposition::NewLayer: return "new_layer";
        case Disposition::ReplaceLayer: return "replace_layer";
    }
    return "";
}

const char* conditioning_name(Conditioning c) {
    switch (c) {
        case Conditioning::Init: return "init";
        case Conditioning::Reference: return "reference";
    }
    return "";
}

// A square window centred on the region: square because that is what these
// models are happiest with, centred so the thing being repaired is not up
// against an edge with no context on one side.
Window context_window(const Mask& region, int layer_w, int layer_h, int target, float context) {
    Window w;
    if (layer_w <= 0 || layer_h <= 0) return w;
    if (target <= 0) target = 1024;
    // No selection is not a small selection: it means the whole layer, and
    // there is no window to choose.
    const raster::Rect b = region.empty() ? raster::Rect{} : region.bounds();
    if (b.empty()) { w.box = {0, 0, layer_w, layer_h}; }
    else {
        const int bw = b.x1 - b.x0, bh = b.y1 - b.y0;
        // Enough room for context around the selection; and if that still
        // fits inside the model's working size, take the whole of it, since
        // the context is free and the repair stays at full resolution.
        int side = static_cast<int>(std::lround(std::max(bw, bh) * context));
        if (side <= target) side = target;
        side = std::min(side, std::min(layer_w, layer_h));
        side = std::max(side, 8);
        // Centred, then pushed back inside the layer rather than clipped, so
        // a selection near an edge still gets a full-sized window.
        int x0 = (b.x0 + b.x1) / 2 - side / 2, y0 = (b.y0 + b.y1) / 2 - side / 2;
        x0 = std::clamp(x0, 0, layer_w - side);
        y0 = std::clamp(y0, 0, layer_h - side);
        w.box = {x0, y0, x0 + side, y0 + side};
    }
    const int bw = w.box.x1 - w.box.x0, bh = w.box.y1 - w.box.y0;
    // Sent at the model's size at most, and on its grid: a request that does
    // not line up with the latent grid is rounded somewhere unseen.
    const double shrink = std::min(1.0, static_cast<double>(target) / std::max(bw, bh));
    auto grid = [](double v) { return std::max(64, static_cast<int>(std::lround(v / 64.0)) * 64); };
    w.width = grid(bw * shrink);
    w.height = grid(bh * shrink);
    return w;
}

void match_surroundings(Image& answer, const Image& original, const Mask& region, raster::Rect window) {
    if (answer.empty() || answer.width() != original.width() || answer.height() != original.height()) return;
    if (window.empty()) window = {0, 0, answer.width(), answer.height()};
    window = window.clipped(answer.width(), answer.height());
    if (window.empty()) return;
    const bool have_region = !region.empty() && region.width() == answer.width() && region.height() == answer.height();

    double sum[3] = {0, 0, 0};
    long long n = 0;
    for (int y = window.y0; y < window.y1; ++y)
        for (int x = window.x0; x < window.x1; ++x) {
            // Only where the two pictures are meant to agree.
            if (have_region && region.at(x, y) > 8) continue;
            const Color a = answer.get(x, y), o = original.get(x, y);
            sum[0] += static_cast<double>(a.r) - o.r;
            sum[1] += static_cast<double>(a.g) - o.g;
            sum[2] += static_cast<double>(a.b) - o.b;
            ++n;
        }
    // Too little agreement left to measure anything from.
    if (n < 256) return;
    const double dr = sum[0] / n, dg = sum[1] / n, db = sum[2] / n;
    if (std::abs(dr) < 0.5 && std::abs(dg) < 0.5 && std::abs(db) < 0.5) return;
    auto fix = [](int v, double d) { return static_cast<uint8_t>(std::clamp(v - d, 0.0, 255.0)); };
    for (int y = window.y0; y < window.y1; ++y)
        for (int x = window.x0; x < window.x1; ++x) {
            Color c = answer.get(x, y);
            answer.set(x, y, {fix(c.r, dr), fix(c.g, dg), fix(c.b, db), c.a});
        }
}

void composite_into(Image& dst, const Image& generated, const Mask& region, float feather) {
    if (dst.empty() || generated.empty()) return;
    const int w = dst.width(), h = dst.height();
    // A region that is empty or the wrong size means "all of it", which is
    // the one case where the backend's frame can be taken whole.
    const bool whole = region.empty() || region.width() != w || region.height() != h;
    Mask soft;
    if (!whole) {
        soft = region;
        if (feather > 0.0f) mask::feather(soft, feather);
    }
    // A backend is free to work at whatever size suits it; the result is
    // brought back to the layer's size before any of it is kept.
    Image rescaled;
    if (generated.width() != w || generated.height() != h) rescaled = raster::resample(generated, w, h, raster::Filter::Bilinear);
    const Image& src = rescaled.empty() ? generated : rescaled;
    parallel::rows(h, static_cast<size_t>(w) * 4, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                const int a = whole ? 255 : soft.at(x, y);
                if (a == 0) continue;
                const Color s = src.get(x, y);
                if (a == 255) { dst.set(x, y, s); continue; }
                const Color d = dst.get(x, y);
                const int inv = 255 - a;
                dst.set(x, y, Color{static_cast<uint8_t>((s.r * a + d.r * inv + 127) / 255),
                                    static_cast<uint8_t>((s.g * a + d.g * inv + 127) / 255),
                                    static_cast<uint8_t>((s.b * a + d.b * inv + 127) / 255),
                                    static_cast<uint8_t>((s.a * a + d.a * inv + 127) / 255)});
            }
    });
}

// --- Queue ------------------------------------------------------------------

struct Queue::Impl {
    Backend backend;
    int concurrency = 1;

    mutable std::mutex m;
    std::condition_variable work, idle;
    bool stopping = false;

    uint64_t next_id = 1;
    struct Entry {
        uint64_t id = 0;
        Request request;
        std::shared_ptr<Progress> progress;
    };
    std::deque<Entry> waiting;
    std::map<uint64_t, Entry> running;
    std::vector<Finished> finished;
    std::vector<std::thread> workers;

    void worker() {
        for (;;) {
            Entry e;
            {
                std::unique_lock<std::mutex> lock(m);
                work.wait(lock, [this] { return stopping || !waiting.empty(); });
                if (stopping && waiting.empty()) return;
                e = std::move(waiting.front());
                waiting.pop_front();
                // A job cancelled while it sat in the queue never starts.
                if (e.progress->cancel.load()) {
                    e.progress->status.store(Status::Cancelled);
                    finished.push_back({e.id, std::move(e.request), Result{}, Status::Cancelled});
                    idle.notify_all();
                    continue;
                }
                e.progress->status.store(Status::Running);
                running[e.id] = e;
            }
            Result r;
            Status end = Status::Done;
            if (backend) {
                r = backend(e.request, *e.progress);
                if (e.progress->cancel.load() && !r.ok) end = Status::Cancelled;
                else if (!r.ok) end = Status::Failed;
            } else {
                r.error = "no backend";
                end = Status::Failed;
            }
            {
                std::lock_guard<std::mutex> lock(m);
                running.erase(e.id);
                e.progress->status.store(end);
                finished.push_back({e.id, std::move(e.request), std::move(r), end});
                idle.notify_all();
            }
        }
    }
};

Queue::Queue(Backend backend, int concurrency) : impl_(std::make_unique<Impl>()) {
    impl_->backend = std::move(backend);
    impl_->concurrency = std::max(1, concurrency);
    for (int i = 0; i < impl_->concurrency; ++i) impl_->workers.emplace_back([this] { impl_->worker(); });
}

Queue::~Queue() {
    {
        std::lock_guard<std::mutex> lock(impl_->m);
        impl_->stopping = true;
        for (auto& [id, e] : impl_->running) e.progress->cancel.store(true);
        for (auto& e : impl_->waiting) e.progress->cancel.store(true);
    }
    impl_->work.notify_all();
    for (std::thread& t : impl_->workers) if (t.joinable()) t.join();
}

uint64_t Queue::submit(Request r) {
    uint64_t id;
    {
        std::lock_guard<std::mutex> lock(impl_->m);
        id = impl_->next_id++;
        impl_->waiting.push_back({id, std::move(r), std::make_shared<Progress>()});
    }
    impl_->work.notify_one();
    return id;
}

bool Queue::cancel(uint64_t id) {
    std::lock_guard<std::mutex> lock(impl_->m);
    for (auto& e : impl_->waiting)
        if (e.id == id) { e.progress->cancel.store(true); return true; }
    const auto it = impl_->running.find(id);
    if (it == impl_->running.end()) return false;
    it->second.progress->cancel.store(true);
    // Whether a running job actually stops is the backend's to say.
    return it->second.progress->cancellable.load();
}

void Queue::cancel_all() {
    std::lock_guard<std::mutex> lock(impl_->m);
    for (auto& e : impl_->waiting) e.progress->cancel.store(true);
    for (auto& [id, e] : impl_->running) e.progress->cancel.store(true);
}

std::vector<Job> Queue::jobs() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    std::vector<Job> out;
    out.reserve(impl_->waiting.size() + impl_->running.size());
    auto add = [&out](const Impl::Entry& e) {
        Job j;
        j.id = e.id;
        j.name = e.request.name;
        j.status = e.progress->status.load();
        j.cancellable = e.progress->cancellable.load();
        j.fraction = e.progress->fraction.load();
        j.disposition = e.request.disposition;
        j.revision = e.request.revision;
        j.layer = e.request.layer;
        out.push_back(std::move(j));
    };
    for (const auto& [id, e] : impl_->running) add(e);
    for (const auto& e : impl_->waiting) add(e);
    std::stable_sort(out.begin(), out.end(), [](const Job& a, const Job& b) { return a.id < b.id; });
    return out;
}

size_t Queue::outstanding() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    return impl_->waiting.size() + impl_->running.size();
}

std::vector<Finished> Queue::drain() {
    std::lock_guard<std::mutex> lock(impl_->m);
    std::vector<Finished> out;
    out.swap(impl_->finished);
    return out;
}

void Queue::wait() {
    std::unique_lock<std::mutex> lock(impl_->m);
    impl_->idle.wait(lock, [this] { return impl_->waiting.empty() && impl_->running.empty(); });
}

}  // namespace firn::gen
