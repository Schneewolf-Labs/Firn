#pragma once
#include <string>

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

// A one-off reachability check: returns the server's model name, or an empty
// string with `err` set. Used by Preferences to say whether the address works.
std::string probe(const std::string& base, std::string* err);

}  // namespace firn::genhttp
