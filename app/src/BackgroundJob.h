#pragma once
// One slow operation running off the interface thread, behind a modal that
// shows how far along it is and offers to cancel. The modal is what keeps
// this simple: the document cannot change while the worker reads it, so
// there is no question of what to do if it did.
#include <atomic>
#include <future>
#include <string>

#include <memory>
#include <vector>

#include "firn/document.h"
#include "firn/image.h"

struct BackgroundJob {
    // What the main thread does with the result when the worker is done.
    enum class Kind { Fill, NewLayer, Upscale, Save, Open };

    Kind kind = Kind::Fill;
    std::string name;              // the dialog's title and the history entry
    std::future<bool> done;        // false when the worker failed or was cancelled
    std::atomic<float> progress{0.0f};
    std::atomic<bool> cancel{false};
    bool cancellable = true;       // writing a file is not worth interrupting
    bool opened = false;           // the modal has been opened for this job

    firn::Image result;            // Fill: the worker's buffer, read once `done` is ready
    size_t layer = 0;              // Fill: where it goes
    std::string layer_name;        // NewLayer: what to call the layer it arrives as
    std::vector<firn::Image> upscaled;      // Upscale: the enlarged layers, by layer index
    int upscale_w = 0, upscale_h = 0;       // Upscale: the size they came back at
    std::string path, error;       // Save and Open
    std::unique_ptr<firn::Document> loaded;      // Open: the worker's document
    std::vector<std::string> warnings;           // Open
};
