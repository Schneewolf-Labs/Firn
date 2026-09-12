#pragma once
// One slow operation running off the interface thread, behind a modal that
// shows how far along it is and offers to cancel. The modal is what keeps
// this simple: the document cannot change while the worker reads it, so
// there is no question of what to do if it did.
#include <atomic>
#include <future>
#include <string>

#include "firn/image.h"

struct BackgroundJob {
    // What the main thread does with the result when the worker is done.
    enum class Kind { Fill, Save };

    Kind kind = Kind::Fill;
    std::string name;              // the dialog's title and the history entry
    std::future<bool> done;        // false when the worker failed or was cancelled
    std::atomic<float> progress{0.0f};
    std::atomic<bool> cancel{false};
    bool cancellable = true;       // writing a file is not worth interrupting
    bool opened = false;           // the modal has been opened for this job

    firn::Image result;            // Fill: the worker's buffer, read once `done` is ready
    size_t layer = 0;              // Fill: where it goes
    std::string path, error;       // Save
};
