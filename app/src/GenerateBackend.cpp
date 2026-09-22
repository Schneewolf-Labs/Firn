// Talking to a stable-diffusion.cpp server over its native async API:
// submit, poll, cancel. Shelling out to curl keeps a program that has no
// HTTP dependency from acquiring one for this; the payloads go through
// files rather than the command line, which a megabyte of base64 would
// overflow on every platform.
#include "GenerateBackend.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

#include "firn/io.h"
#include "firn/json.h"

namespace firn::genhttp {

namespace {

bool have_tool(const char* name) {
#ifdef _WIN32
    const std::string probe_cmd = std::string("where ") + name + " >NUL 2>&1";
#else
    const std::string probe_cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
#endif
    return std::system(probe_cmd.c_str()) == 0;
}

std::string run(const std::string& cmd, size_t cap = 64u << 20) {
#ifdef _WIN32
    FILE* f = _popen(cmd.c_str(), "r");
#else
    FILE* f = popen(cmd.c_str(), "r");
#endif
    if (!f) return {};
    std::string out;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        out.append(buf, n);
        if (out.size() > cap) break;
    }
#ifdef _WIN32
    _pclose(f);
#else
    pclose(f);
#endif
    return out;
}

std::filesystem::path scratch_file(const char* suffix) {
    static std::atomic<uint64_t> counter{0};
    std::random_device rd;
    const std::string name = "firn-gen-" + std::to_string(rd()) + "-" +
                             std::to_string(counter.fetch_add(1)) + suffix;
    return std::filesystem::temp_directory_path() / name;
}

// Everything a request needs written to disk, because a base64 image is far
// past any platform's command line limit.
std::string post_json(const std::string& url, const std::string& body, size_t cap = 64u << 20) {
    const std::filesystem::path in = scratch_file(".json");
    { std::ofstream f(in, std::ios::binary); f.write(body.data(), static_cast<std::streamsize>(body.size())); }
#ifdef _WIN32
    const std::string cmd = "powershell -NoProfile -NonInteractive -Command \"try { "
                            "(Invoke-WebRequest -UseBasicParsing -TimeoutSec 900 -Method Post -ContentType 'application/json' "
                            "-InFile '" + in.string() + "' '" + url + "').Content } catch { $_.Exception.Response }\" 2>NUL";
#else
    const std::string cmd = "curl -sS --max-time 900 -H 'Content-Type: application/json' --data-binary @" +
                            in.string() + " " + url + " 2>/dev/null";
#endif
    const std::string out = run(cmd, cap);
    std::error_code ec;
    std::filesystem::remove(in, ec);
    return out;
}

std::string get(const std::string& url, size_t cap = 64u << 20) {
#ifdef _WIN32
    const std::string cmd = "powershell -NoProfile -NonInteractive -Command \"try { "
                            "(Invoke-WebRequest -UseBasicParsing -TimeoutSec 60 '" + url + "').Content } catch { }\" 2>NUL";
#else
    const std::string cmd = "curl -sS --max-time 60 " + url + " 2>/dev/null";
#endif
    return run(cmd, cap);
}

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const std::vector<uint8_t>& d) {
    std::string out;
    out.reserve((d.size() + 2) / 3 * 4);
    for (size_t i = 0; i < d.size(); i += 3) {
        const uint32_t a = d[i];
        const uint32_t b = i + 1 < d.size() ? d[i + 1] : 0;
        const uint32_t c = i + 2 < d.size() ? d[i + 2] : 0;
        const uint32_t v = (a << 16) | (b << 8) | c;
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(i + 1 < d.size() ? kB64[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < d.size() ? kB64[v & 63] : '=');
    }
    return out;
}

std::vector<uint8_t> unbase64(const std::string& s) {
    int8_t table[256];
    std::fill(std::begin(table), std::end(table), -1);
    for (int i = 0; i < 64; ++i) table[static_cast<uint8_t>(kB64[i])] = static_cast<int8_t>(i);
    std::vector<uint8_t> out;
    out.reserve(s.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (const char c : s) {
        const int8_t v = table[static_cast<uint8_t>(c)];
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF)); }
    }
    return out;
}

// The mask the service wants: white where it should generate, black where
// it must leave the picture alone. Firn's selection is already that way
// round, which is the one piece of luck in this exchange.
std::vector<uint8_t> mask_png(const Mask& m, int w, int h) {
    Image img(w, h, {0, 0, 0, 255});
    if (!m.empty() && m.width() == w && m.height() == h)
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint8_t v = m.at(x, y);
                img.set(x, y, {v, v, v, 255});
            }
    else
        img.fill({255, 255, 255, 255});
    return io::encode_png(img);
}

}  // namespace

bool available() {
#ifdef _WIN32
    return have_tool("powershell.exe") || have_tool("powershell");
#else
    return have_tool("curl");
#endif
}

bool capabilities(const std::string& base, Capabilities* out, std::string* err) {
    if (!out) return false;
    *out = Capabilities{};
    if (!available()) { if (err) *err = "curl is not installed"; return false; }
    const std::string body = get(base + "/sdcpp/v1/capabilities", 4u << 20);
    if (body.empty()) { if (err) *err = "no answer from " + base; return false; }
    json::Value v;
    if (!json::parse(body, v) || !v.is_object()) { if (err) *err = "the address answered, but not with capabilities"; return false; }
    // Any of these means we are talking to something that understands the
    // native API rather than to an unrelated web server.
    if (!v.find("features") && !v.find("defaults") && !v.find("features_by_mode")) {
        if (err) *err = "not a stable-diffusion.cpp server";
        return false;
    }

    // The model block is an object on current servers and was a bare string
    // on older ones.
    if (const json::Value& m = v.get("model"); m.is_object()) {
        out->model = m.get("name").as_string("");
        if (out->model.empty()) out->model = m.get("stem").as_string("");
    } else {
        out->model = m.as_string("");
    }
    if (out->model.empty()) out->model = v.get("current_mode").as_string("ready");

    auto strings = [](const json::Value& a, std::vector<std::string>* dst, const char* key) {
        for (size_t i = 0; i < a.size(); ++i) {
            const json::Value& e = a[i];
            std::string s = e.is_object() ? e.get(key).as_string("") : e.as_string("");
            if (!s.empty()) dst->push_back(std::move(s));
        }
    };
    strings(v.get("samplers"), &out->samplers, "name");
    strings(v.get("schedulers"), &out->schedulers, "name");
    for (size_t i = 0; i < v.get("loras").size(); ++i) {
        const json::Value& e = v.get("loras")[i];
        Capabilities::Lora lora{e.get("name").as_string(""), e.get("path").as_string("")};
        if (lora.path.empty()) lora.path = lora.name;
        if (lora.name.empty()) lora.name = lora.path;
        if (!lora.path.empty()) out->loras.push_back(std::move(lora));
    }

    // The mode-aware fields are the real ones; the top-level trio are
    // documented as deprecated mirrors of whichever mode is current, so they
    // are the fallback rather than the source.
    const json::Value& feat = v.find("features_by_mode") ? v.get("features_by_mode").get("img_gen") : v.get("features");
    auto feature = [&feat](const char* name) {
        // Reported either as a list of names or as an object of flags,
        // depending on the build.
        if (feat.is_object()) return feat.get(name).as_bool(false) || feat.find(name) != nullptr;
        for (size_t i = 0; i < feat.size(); ++i)
            if (feat[i].as_string("") == name) return true;
        return false;
    };
    out->takes_init = feature("init_image");
    out->takes_mask = feature("mask_image");
    out->takes_refs = feature("ref_images");
    out->takes_lora = feature("lora");

    const json::Value& lim = v.get("limits");
    out->min_width = static_cast<int>(lim.get("min_width").as_number(0));
    out->max_width = static_cast<int>(lim.get("max_width").as_number(0));
    out->min_height = static_cast<int>(lim.get("min_height").as_number(0));
    out->max_height = static_cast<int>(lim.get("max_height").as_number(0));
    out->max_batch = static_cast<int>(lim.get("max_batch_count").as_number(0));

    const json::Value& def = v.find("defaults_by_mode") ? v.get("defaults_by_mode").get("img_gen") : v.get("defaults");
    out->width = static_cast<int>(def.get("width").as_number(0));
    out->height = static_cast<int>(def.get("height").as_number(0));
    const json::Value& sp = def.get("sample_params");
    out->steps = static_cast<int>(sp.get("sample_steps").as_number(0));
    out->txt_cfg = static_cast<float>(sp.get("guidance").get("txt_cfg").as_number(0));
    out->sampler = sp.get("sample_method").as_string("");
    out->scheduler = sp.get("scheduler").as_string("");
    return true;
}

std::string probe(const std::string& base, std::string* err) {
    Capabilities c;
    if (!capabilities(base, &c, err)) return {};
    return c.model;
}

gen::Backend backend(const std::string& base) {
    return [base](const gen::Request& req, gen::Progress& p) {
        using namespace std::chrono_literals;
        gen::Result out;
        if (!available()) { out.error = "curl is not installed"; return out; }
        // No picture at all is a text-to-image request, which a unified
        // model answers and an inpainting one does not. It still has to say
        // how big, since there is nothing to take a size from.
        const bool from_nothing = req.init.empty() && req.refs.empty();
        if (from_nothing && (req.width <= 0 || req.height <= 0)) { out.error = "nothing to work from"; return out; }

        const int w = !req.init.empty() ? req.init.width() : req.width > 0 ? req.width : req.refs[0].width();
        const int h = !req.init.empty() ? req.init.height() : req.height > 0 ? req.height : req.refs[0].height();
        json::Value body = json::Value::object();
        // The caller's parameters go through untouched; only the pieces the
        // document owns are filled in here.
        for (const auto& [k, v] : req.params.obj) body.set(k, v);
        if (req.conditioning == gen::Conditioning::Reference) {
            // An instruction model takes the picture as what it is editing,
            // not as noise to work back from, and a mask only crops whatever
            // it decided to imagine. The layer being edited goes first and
            // anything else the caller offered follows, because the order is
            // what the instruction refers to ("put the object from the
            // second picture into the first").
            json::Value refs = json::Value::array();
            if (!req.init.empty()) refs.push(json::Value::string(base64(io::encode_png(req.init))));
            for (const Image& r : req.refs)
                if (!r.empty()) refs.push(json::Value::string(base64(io::encode_png(r))));
            if (refs.size() > 0) body.set("ref_images", std::move(refs));
        } else if (!req.init.empty()) {
            body.set("init_image", json::Value::string(base64(io::encode_png(req.init))));
            if (!req.region.empty()) body.set("mask_image", json::Value::string(base64(mask_png(req.region, w, h))));
        }
        body.set("width", json::Value::number(w));
        body.set("height", json::Value::number(h));
        // A seed of the caller's choosing, otherwise a fresh one every time.
        // The service's own default is a fixed number, which means asking
        // twice gives the same answer back -- so there would be no way to
        // try again, and a seed that happens to suit neither the picture nor
        // the prompt would be stuck that way. One fixed seed on this server
        // returned flat colour where a random one returned the scene.
        if (!req.params.find("seed")) {
            std::random_device rd;
            body.set("seed", json::Value::number(static_cast<double>(rd() & 0x7FFFFFFF)));
        }

        const std::string reply = post_json(base + "/sdcpp/v1/img_gen", json::dump(body), 4u << 20);
        json::Value job;
        if (!json::parse(reply, job) || job.get("id").as_string("").empty()) {
            out.error = reply.empty() ? "no answer from " + base : "the server refused the request";
            if (json::Value e; json::parse(reply, e) && !e.get("error").as_string("").empty())
                out.error = e.get("error").as_string("");
            return out;
        }
        const std::string id = job.get("id").as_string("");
        const std::string url = base + "/sdcpp/v1/jobs/" + id;

        for (;;) {
            if (p.cancel.load() && p.cancellable.load()) {
                post_json(base + "/sdcpp/v1/jobs/" + id + "/cancel", "{}", 1u << 16);
                out.error = "cancelled";
                return out;
            }
            std::this_thread::sleep_for(500ms);
            json::Value st;
            if (!json::parse(get(url), st)) continue;
            const std::string status = st.get("status").as_string("");
            if (status == "generating") {
                // Past the point the service will take a cancel, so stop
                // offering one rather than pretending.
                p.cancellable.store(false);
                p.status.store(gen::Status::Running);
            }
            if (const json::Value& f = st.get("progress"); f.is_number()) p.fraction.store(static_cast<float>(f.num));
            if (status == "failed") { out.error = st.get("error").as_string("the server gave up"); return out; }
            if (status == "cancelled") { out.error = "cancelled"; return out; }
            if (status != "completed") continue;

            const json::Value& images = st.get("result").get("images");
            if (images.size() == 0) { out.error = "the server returned no image"; return out; }
            std::string data = images[0].get("b64_json").as_string("");
            if (data.empty()) data = images[0].as_string("");
            if (const size_t comma = data.find(",\n"); comma != std::string::npos && data.compare(0, 5, "data:") == 0) data = data.substr(comma + 1);
            if (data.compare(0, 5, "data:") == 0)
                if (const size_t comma = data.find(','); comma != std::string::npos) data = data.substr(comma + 1);
            const std::vector<uint8_t> png = unbase64(data);
            std::string err;
            if (auto img = io::load_memory(png.data(), png.size(), &err)) {
                out.image = std::move(*img);
                out.ok = true;
                out.info = st.get("result");
            } else {
                out.error = "the server's image could not be read: " + err;
            }
            return out;
        }
    };
}

}  // namespace firn::genhttp
