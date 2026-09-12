#include "UpdateCheck.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "firn/json.h"

namespace firn::update {

namespace {

constexpr const char* kLatestUrl =
    "https://api.github.com/repos/Schneewolf-Labs/Firn/releases/latest";

// Three numbers out of "v1.2.3" or "1.2". Missing parts are zero, and
// anything that is not a version at all comes back as all zeroes, which
// compares as "not newer" everywhere it is used.
std::array<int, 3> parse_version(const std::string& s) {
    std::array<int, 3> v{0, 0, 0};
    size_t i = 0;
    while (i < s.size() && !std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    for (int part = 0; part < 3 && i < s.size(); ++part) {
        int n = 0;
        bool any = false;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            n = n * 10 + (s[i++] - '0');
            any = true;
            if (n > 1000000) return {0, 0, 0};
        }
        if (!any) break;
        v[static_cast<size_t>(part)] = n;
        if (i < s.size() && s[i] == '.') ++i;
        else break;
    }
    return v;
}

bool have_tool(const char* name) {
#ifdef _WIN32
    const std::string probe = std::string("where ") + name + " >NUL 2>&1";
#else
    const std::string probe = std::string("command -v ") + name + " >/dev/null 2>&1";
#endif
    return std::system(probe.c_str()) == 0;
}

// The request, capped in both time and size. A hostile or broken server
// must not be able to hang the worker or hand back something enormous.
std::string fetch(const std::string& url) {
#ifdef _WIN32
    const std::string cmd =
        "powershell -NoProfile -NonInteractive -Command \"try { "
        "(Invoke-WebRequest -UseBasicParsing -TimeoutSec 10 -Headers @{'User-Agent'='Firn'} '" + url +
        "').Content } catch { }\" 2>NUL";
    FILE* f = _popen(cmd.c_str(), "r");
#else
    const std::string cmd = "curl -fsSL --max-time 10 -A Firn " + url + " 2>/dev/null";
    FILE* f = popen(cmd.c_str(), "r");
#endif
    if (!f) return {};
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        out.append(buf, n);
        if (out.size() > (1u << 20)) break;   // a release document is a few KB
    }
#ifdef _WIN32
    _pclose(f);
#else
    pclose(f);
#endif
    return out;
}

}  // namespace

bool is_newer(const std::string& candidate, const std::string& current) {
    const std::array<int, 3> a = parse_version(candidate), b = parse_version(current);
    if (a == std::array<int, 3>{0, 0, 0}) return false;
    return a > b;
}

bool available() {
#ifdef _WIN32
    return have_tool("powershell.exe") || have_tool("powershell");
#else
    return have_tool("curl");
#endif
}

std::future<Result> check_async(const std::string& current_version) {
    return std::async(std::launch::async, [current = current_version] {
        Result r;
        if (!available()) return r;
        const std::string body = fetch(kLatestUrl);
        if (body.empty()) return r;
        json::Value v;
        if (!json::parse(body, v) || !v.is_object()) return r;
        const std::string tag = v.get("tag_name").as_string("");
        if (tag.empty()) return r;
        r.checked = true;
        r.version = tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag;
        r.url = v.get("html_url").as_string("https://github.com/Schneewolf-Labs/Firn/releases");
        r.notes = v.get("body").as_string("");
        if (r.notes.size() > 4000) r.notes.resize(4000);
        r.newer = is_newer(tag, current);
        return r;
    });
}

}  // namespace firn::update
