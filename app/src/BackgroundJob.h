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
    std::string name;              // the dialog's title and the history entry
    std::future<bool> done;        // false when the worker was cancelled
    std::atomic<float> progress{0.0f};
    std::atomic<bool> cancel{false};
    firn::Image result;            // the worker's buffer; read only once `done` is ready
    size_t layer = 0;
    bool opened = false;           // the modal has been opened for this job
};
