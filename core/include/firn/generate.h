#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "firn/image.h"
#include "firn/json.h"
#include "firn/mask.h"

// Work handed to an image model and brought back into the document.
//
// Nothing here knows what a diffusion model is. A request carries an opaque
// bag of parameters the backend understands, an optional image and region,
// and a disposition saying what the answer means for the document. That is
// the whole contract: model capabilities change far faster than an editor
// should, so the only things named here are the ones Firn itself has to act
// on. Whether the far side is a program on this machine or a service
// somewhere is the backend's business, not this file's.
namespace firn::gen {

// What a finished result does to the document. Three cases rather than one
// per model feature, because these are the only three the document model
// can tell apart.
enum class Disposition {
    IntoRegion,    // composite into the request's region, leaving the rest alone
    NewLayer,      // arrive as a new layer above the one it came from
    ReplaceLayer,  // replace that layer's pixels outright
};

// How the picture is offered to the model, which is a difference between
// families rather than a preference. An inpainting model wants the image as
// something to noise and denoise back; an instruction model wants it as the
// reference it was trained to edit against. Hand an instruction model an
// init image and it stops editing and starts inventing: asked to remove a
// dog from a lawn it returned a different lawn with a different dog, and
// the same request as a reference removed the dog and matched the mowing
// stripes.
enum class Conditioning {
    Init,        // img2img and masked inpainting
    Reference,   // instruction editing (Qwen-Image-Edit, FLUX Kontext)
};

enum class Status { Queued, Running, Done, Failed, Cancelled };

const char* status_name(Status s);
const char* disposition_name(Disposition d);
const char* conditioning_name(Conditioning c);

struct Request {
    std::string name = "Generate";   // history entry and the label a queue shows
    std::string operation;           // what to ask the backend for; the backend defines these
    json::Value params = json::Value::object();   // passed through untouched

    Image init;                      // the picture to work from, when there is one
    // Further pictures for the model to look at, in order. A unified model
    // takes several (Qwen-Image-2.1 takes ten), which is how one layer's
    // subject is put into another layer's scene. Only meaningful alongside
    // Conditioning::Reference: an inpainting model has one hole to fill and
    // nothing to compare it with.
    std::vector<Image> refs;
    Conditioning conditioning = Conditioning::Init;
    Mask region;                     // where it applies; empty means the whole image
    // What size to ask for when there is no picture to take it from. A
    // unified model generates as well as edits, and a request with neither
    // an init image nor a reference is a text-to-image request, which has
    // to say how big the answer should be.
    int width = 0, height = 0;
    Disposition disposition = Disposition::NewLayer;
    float feather = 3.0f;            // softening applied when compositing into a region

    // Where this came from, so a result that arrives late can tell whether
    // the document moved underneath it. A queue may hold several of these
    // at once and an edit does not wait for them.
    uint64_t revision = 0;
    int layer = -1;
};

struct Result {
    bool ok = false;
    Image image;
    std::string error;
    json::Value info = json::Value::object();   // whatever the backend reported back
};

// Live state of one running request. The backend owns `stage` and
// `cancellable`; the queue and the interface own `cancel`.
struct Progress {
    std::atomic<Status> status{Status::Queued};
    std::atomic<bool> cancel{false};
    // Cleared by the backend once it can no longer stop: some services take
    // a cancel only while the work is still queued on their side, and a
    // button that claims otherwise is lying.
    std::atomic<bool> cancellable{true};
    std::atomic<float> fraction{-1.0f};   // negative when the backend cannot say
};

// Runs one request to completion. Called on a worker thread. It should watch
// `p.cancel` and give up when it can, and must not touch the document.
using Backend = std::function<Result(const Request&, Progress&)>;

// One request's slot in the queue, as the interface sees it.
struct Job {
    uint64_t id = 0;
    std::string name;
    Status status = Status::Queued;
    bool cancellable = true;
    float fraction = -1.0f;
    Disposition disposition = Disposition::NewLayer;
    uint64_t revision = 0;
    int layer = -1;
};

// A finished job, waiting for the main thread to put it into the document.
struct Finished {
    uint64_t id = 0;
    Request request;
    Result result;
    Status status = Status::Done;
};

// Requests waiting on a backend, with however many running at once the
// backend can usefully take. Submitting does not block, and the document is
// free to change while work is outstanding: a Request records the revision
// and layer it was built from so the caller can decide what a stale answer
// should do.
class Queue {
public:
    explicit Queue(Backend backend, int concurrency = 1);
    ~Queue();
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    uint64_t submit(Request r);
    // Asks a job to stop. A queued job always stops; a running one only if
    // its backend is still willing, so this reports what it managed.
    bool cancel(uint64_t id);
    void cancel_all();

    std::vector<Job> jobs() const;        // a snapshot, safe to call every frame
    size_t outstanding() const;           // queued plus running

    // Takes everything that has finished since the last call. The caller
    // owns the results and is expected to be the thread that owns the
    // document.
    std::vector<Finished> drain();

    // Waits for everything outstanding to finish. For tests and shutdown.
    void wait();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Composites a generated picture into `dst` through `region`, softened by
// `feather` pixels, so that nothing outside the region moves.
//
// This is not a convenience. A backend that runs the whole frame through an
// encoder and decoder hands back every pixel slightly changed, including the
// ones it was told to leave alone: measured against one such service, the
// area outside the mask moved by 7.5 levels on average and 130 at worst,
// which on a photograph is a visible shift of the entire picture for an edit
// to one corner of it. Compositing here is what keeps an edit local.
void composite_into(Image& dst, const Image& generated, const Mask& region, float feather);

}  // namespace firn::gen
