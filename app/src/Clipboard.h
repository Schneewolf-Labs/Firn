#pragma once
// The system clipboard as an image, so copies reach other programs and
// screenshots or images copied elsewhere can be pasted here. Windows uses
// the Win32 clipboard (PNG and DIB formats); Linux goes through wl-copy /
// wl-paste on Wayland or xclip on X11 as image/png; macOS uses osascript.
#include <optional>

#include "firn/image.h"

namespace clipboard {

// Puts the image on the system clipboard. Returns false when no mechanism
// is available (the internal clipboard still works).
bool write_image(const firn::Image& img);
// The clipboard's image, if it holds one this build can read.
std::optional<firn::Image> read_image();
// A short note for the status bar when write_image() fails.
const char* unavailable_reason();

}  // namespace clipboard
