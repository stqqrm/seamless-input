#pragma once
// Plain-text clipboard access for the daemon. Text only (UTF-8, '\n' line ends on the wire).
//
// Windows : CF_UNICODETEXT through a message-only owner window.
// Linux   : external tools, because X11 / Wayland clipboards need a live selection owner:
//             Wayland  wl-paste / wl-copy      (package wl-clipboard)
//             X11      xclip, or xsel
//           (GNOME on Wayland refuses background clipboard access; there the X11 tools only
//            work while an XWayland window has focus. This is a compositor limitation.)
//
// Call GetText/SetText from ONE thread (the Core loop): the Windows owner window belongs to the
// thread that first used it.
#include <string>

namespace clipboard {

constexpr std::size_t kMaxBytes = 512 * 1024;      // larger clipboards are not shared

std::string Backend();                            // "Windows clipboard", "xclip", ... or "" if none
bool GetText(std::string& utf8);                  // false: empty, not text, too large, or no backend
bool SetText(const std::string& utf8);

std::string Base64Encode(const std::string& in);
bool Base64Decode(const std::string& in, std::string& out);

} // namespace clipboard
