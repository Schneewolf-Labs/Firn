#include "Clipboard.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "firn/io.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace clipboard {

namespace {

std::vector<uint8_t> encode_png(const firn::Image& img) {
    return firn::io::encode_png(img);
}

#if !defined(_WIN32)
// Runs a command, feeding `input` to its stdin and collecting its stdout.
[[maybe_unused]] bool run(const std::string& cmd, const std::vector<uint8_t>* input, std::vector<uint8_t>* output) {
    if (input) {
        FILE* f = popen((cmd + " >/dev/null 2>&1").c_str(), "w");
        if (!f) return false;
        const size_t n = fwrite(input->data(), 1, input->size(), f);
        return pclose(f) == 0 && n == input->size();
    }
    FILE* f = popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) output->insert(output->end(), buf, buf + n);
    return pclose(f) == 0 && !output->empty();
}

#if !defined(__APPLE__)
bool have(const char* tool) {
    return std::system((std::string("command -v ") + tool + " >/dev/null 2>&1").c_str()) == 0;
}

bool wayland() { const char* w = std::getenv("WAYLAND_DISPLAY"); return w && *w; }
#endif
#endif

}  // namespace

#if defined(_WIN32)

bool write_image(const firn::Image& img) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    // PNG for programs that take it, plus a 32-bit DIB for everything else.
    const std::vector<uint8_t> png = encode_png(img);
    if (!png.empty()) {
        const UINT fmt = RegisterClipboardFormatA("PNG");
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, png.size());
        if (h) { std::memcpy(GlobalLock(h), png.data(), png.size()); GlobalUnlock(h); SetClipboardData(fmt, h); }
    }
    const int w = img.width(), hgt = img.height();
    const size_t bytes = sizeof(BITMAPV5HEADER) + static_cast<size_t>(w) * hgt * 4;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        auto* p = static_cast<uint8_t*>(GlobalLock(h));
        BITMAPV5HEADER hdr{};
        hdr.bV5Size = sizeof(hdr); hdr.bV5Width = w; hdr.bV5Height = hgt; hdr.bV5Planes = 1; hdr.bV5BitCount = 32;
        hdr.bV5Compression = BI_BITFIELDS; hdr.bV5RedMask = 0x00FF0000; hdr.bV5GreenMask = 0x0000FF00; hdr.bV5BlueMask = 0x000000FF; hdr.bV5AlphaMask = 0xFF000000;
        hdr.bV5CSType = 0x73524742 /* sRGB */; hdr.bV5Intent = LCS_GM_IMAGES;
        std::memcpy(p, &hdr, sizeof(hdr));
        uint8_t* d = p + sizeof(hdr);
        for (int y = 0; y < hgt; ++y) {
            const uint8_t* s = img.data() + static_cast<size_t>(hgt - 1 - y) * w * 4;  // bottom-up
            for (int x = 0; x < w; ++x) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; d += 4; s += 4; }
        }
        GlobalUnlock(h);
        SetClipboardData(CF_DIBV5, h);
    }
    CloseClipboard();
    return true;
}

std::optional<firn::Image> read_image() {
    if (!OpenClipboard(nullptr)) return std::nullopt;
    std::optional<firn::Image> out;
    const UINT png_fmt = RegisterClipboardFormatA("PNG");
    if (IsClipboardFormatAvailable(png_fmt)) {
        if (HANDLE h = GetClipboardData(png_fmt)) {
            const auto* p = static_cast<const uint8_t*>(GlobalLock(h));
            out = firn::io::load_memory(p, GlobalSize(h));
            GlobalUnlock(h);
        }
    }
    if (!out && IsClipboardFormatAvailable(CF_DIB)) {
        if (HANDLE h = GetClipboardData(CF_DIB)) {
            const auto* hdr = static_cast<const BITMAPINFOHEADER*>(GlobalLock(h));
            const int w = hdr->biWidth, hgt = std::abs(hdr->biHeight);
            const bool top_down = hdr->biHeight < 0;
            const int bpp = hdr->biBitCount;
            if ((bpp == 32 || bpp == 24) && (hdr->biCompression == BI_RGB || hdr->biCompression == BI_BITFIELDS) && w > 0 && hgt > 0) {
                const uint8_t* px = reinterpret_cast<const uint8_t*>(hdr) + hdr->biSize + (hdr->biCompression == BI_BITFIELDS ? 12 : 0);
                const size_t stride = (static_cast<size_t>(w) * bpp / 8 + 3) / 4 * 4;
                firn::Image img(w, hgt);
                bool any_alpha = false;
                for (int y = 0; y < hgt; ++y) {
                    const uint8_t* row = px + static_cast<size_t>(top_down ? y : hgt - 1 - y) * stride;
                    uint8_t* d = img.data() + static_cast<size_t>(y) * w * 4;
                    for (int x = 0; x < w; ++x) {
                        d[0] = row[2]; d[1] = row[1]; d[2] = row[0]; d[3] = bpp == 32 ? row[3] : 255;
                        if (bpp == 32 && row[3]) any_alpha = true;
                        row += bpp / 8; d += 4;
                    }
                }
                if (bpp == 32 && !any_alpha) for (size_t i = 3; i < img.size_bytes(); i += 4) img.data()[i] = 255;  // alpha channel unused
                out = std::move(img);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

const char* unavailable_reason() { return "the clipboard could not be opened"; }

#elif defined(__APPLE__)

bool write_image(const firn::Image& img) {
    const std::vector<uint8_t> png = encode_png(img);
    const std::string tmp = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/firn-clipboard.png";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    return run("osascript -e 'set the clipboard to (read (POSIX file \"" + tmp + "\") as «class PNGf»)'", nullptr, nullptr) || true;
}

std::optional<firn::Image> read_image() {
    // osascript prints «data PNGf...hex...»; decode the hex payload.
    std::vector<uint8_t> out;
    if (!run("osascript -e 'the clipboard as «class PNGf»'", nullptr, &out)) return std::nullopt;
    std::string s(out.begin(), out.end());
    const size_t a = s.find("PNGf");
    if (a == std::string::npos) return std::nullopt;
    std::vector<uint8_t> png;
    for (size_t i = a + 4; i + 1 < s.size(); i += 2) {
        if (!isxdigit(static_cast<unsigned char>(s[i])) || !isxdigit(static_cast<unsigned char>(s[i + 1]))) break;
        png.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
    }
    return png.empty() ? std::nullopt : firn::io::load_memory(png.data(), png.size());
}

const char* unavailable_reason() { return "osascript failed"; }

#else

bool write_image(const firn::Image& img) {
    const std::vector<uint8_t> png = encode_png(img);
    if (png.empty()) return false;
    if (wayland() && have("wl-copy")) return run("wl-copy --type image/png", &png, nullptr);
    if (have("xclip")) return run("xclip -selection clipboard -t image/png -i", &png, nullptr);
    return false;
}

std::optional<firn::Image> read_image() {
    std::vector<uint8_t> out;
    bool ok = false;
    if (wayland() && have("wl-paste")) ok = run("wl-paste --type image/png", nullptr, &out);
    if (!ok && have("xclip")) { out.clear(); ok = run("xclip -selection clipboard -t image/png -o", nullptr, &out); }
    if (!ok || out.empty()) return std::nullopt;
    return firn::io::load_memory(out.data(), out.size());
}

const char* unavailable_reason() { return "install xclip (X11) or wl-clipboard (Wayland) to share images with other programs"; }

#endif

}  // namespace clipboard
