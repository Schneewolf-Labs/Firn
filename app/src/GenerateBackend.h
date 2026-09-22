#pragma once
#include <string>
#include <vector>

#include "firn/generate.h"

// A gen::Backend that talks to a stable-diffusion.cpp server (or anything
// speaking its native async API) over HTTP. Like the update check, it shells
// out to curl or PowerShell rather than bringing an HTTP library into a
// program that has managed without one.
namespace firn::genhttp {

// True when the tool this needs is on the machine.
bool available();

// `base` is the server root, e.g. "http://127.0.0.1:1234". The returned
// backend is safe to call from a worker thread.
gen::Backend backend(const std::string& base);

// What the server says it is and what it will accept. Asked for once when
// the address is set rather than guessed at, so the interface offers the
// samplers, schedulers and LoRAs this build actually has and the sizes this
// model actually takes. Anything the server does not report stays empty, and
// an empty list means "no opinion", not "none".
struct Capabilities {
    std::string model;                        // the loaded model, for the panel to name
    std::vector<std::string> samplers;
    std::vector<std::string> schedulers;
    std::vector<std::string> loras;           // names, as `lora[].name` wants them
    // What the current mode can be given. A model that takes reference
    // images is a unified or instruction model; one that takes a mask can
    // inpaint. The panel enables its controls from these rather than from
    // a list of model names that would go out of date.
    bool takes_init = false, takes_mask = false, takes_refs = false, takes_lora = false;
    int min_width = 0, max_width = 0, min_height = 0, max_height = 0, max_batch = 0;
    // The server's own defaults, which are a better starting point than
    // anything hardcoded here: they follow the loaded model.
    int width = 0, height = 0, steps = 0;
    float txt_cfg = 0.0f;
    std::string sampler, scheduler;
};

// Asks the server what it is. False with `err` set when the address does not
// answer or does not speak this API.
bool capabilities(const std::string& base, Capabilities* out, std::string* err);

// A one-off reachability check: returns the server's model name, or an empty
// string with `err` set. Used by Preferences to say whether the address works.
std::string probe(const std::string& base, std::string* err);

}  // namespace firn::genhttp
