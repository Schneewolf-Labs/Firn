#pragma once
// Asks GitHub whether there is a newer release. It only ever reports; it
// never downloads or replaces anything, because an unsigned binary that
// rewrites itself can break someone's installation, and opening the
// releases page costs them one click.
//
// No HTTP library: this shells out to curl, or PowerShell on Windows, the
// same way printing uses `lp` and the macOS clipboard uses `osascript`.
// Every failure is silent by design. Someone editing a picture should
// never see an error because a server was unreachable.
#include <atomic>
#include <future>
#include <string>

namespace firn::update {

struct Result {
    bool checked = false;     // the request finished, whatever it said
    bool newer = false;       // a release later than this build exists
    std::string version;      // "0.5.0", without the tag's leading v
    std::string url;          // where to read about it
    std::string notes;        // that release's notes, possibly empty
};

// True when `candidate` is a later version than `current`. Both may carry a
// leading "v". Anything unparseable is treated as not newer.
bool is_newer(const std::string& candidate, const std::string& current);

// Runs the request on a worker. The future is the only way to read it, so
// there is nothing shared to race over.
std::future<Result> check_async(const std::string& current_version);

// Whether a check can run at all: no curl and no PowerShell means no check.
bool available();

}  // namespace firn::update
