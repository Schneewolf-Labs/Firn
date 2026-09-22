#pragma once
#include <future>
#include <string>
#include <utility>
#include <vector>

#include "GenerateBackend.h"

// Everything the Generate palette holds. Kept out of App.h, which 25
// translation units include, for the same reason the Adjust dialogs keep
// theirs in AdjustState: only the panel and App need any of it.
struct GenerateState {
    // What the server said it was, and the address that answer belongs to.
    // Asked for once when the address changes rather than every frame: the
    // panel offers the samplers, schedulers and LoRAs this build actually
    // has, instead of a hardcoded list that would drift from the server.
    std::string asked_url;
    firn::genhttp::Capabilities caps;
    bool have_caps = false;
    bool asking = false;
    std::string caps_error;
    // The question is asked on a worker: a wrong address makes curl wait for
    // its timeout, and the interface must not wait with it.
    std::future<std::pair<firn::genhttp::Capabilities, std::string>> ask;

    char prompt[4096] = "";
    char negative[1024] = "";
    int width = 1024, height = 1024;
    int steps = 0;             // 0 = whatever the server's default is
    float cfg = 0.0f;          // 0 = the server's default
    int sampler = -1;          // index into caps.samplers, -1 = the default
    int scheduler = -1;
    long long seed = -1;       // -1 = a fresh one every time
    int batch = 1;

    // Per-entry LoRA strength, parallel to caps.loras. Zero means the LoRA
    // is not sent at all, so the list can stay on screen without every
    // entry being applied.
    std::vector<float> lora_strength;

    // Layers offered to the model as further reference images, by index.
    // Validated when the request is built, because the stack can be edited
    // while the panel is open.
    std::vector<int> refs;

    // Until the size is set by hand it follows the image, so a new
    // document does not leave the panel offering 512x512 for a 4000px
    // photograph.
    bool size_touched = false;
};
