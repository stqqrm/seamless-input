#pragma once
// System clipboard access for the daemon: plain text and lists of files.
//
// Windows : CF_UNICODETEXT and CF_HDROP through a message-only owner window.
// Linux   : external tools, because X11 / Wayland clipboards need a live selection owner:
//             Wayland  wl-paste / wl-copy      (package wl-clipboard)      text + files
//             X11      xclip                                               text + files
//                      xsel                                                text only
//           Files travel as text/uri-list (KDE, LXQt, ...) or x-special/gnome-copied-files (GNOME,
//           Cinnamon, MATE, XFCE, ...), whichever the desktop's file manager pastes.
//           (GNOME on Wayland refuses background clipboard access; there the tools only work while
//            a window of theirs has focus. This is a compositor limitation.)
//
// Call these from ONE thread (the Core loop): the Windows owner window belongs to the thread that
// first used it.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace clipboard {

constexpr std::uint64_t kMaxTextBytes = 256ull * 1024 * 1024;   // text is held in memory: never more than this

std::string Backend();                            // "Windows clipboard", "xclip", ... or "" if none
bool FilesSupported();                            // can this backend read and write file lists?

// Text ('\n' line ends). false: empty, not text, longer than maxBytes, or no backend.
bool GetText(std::string& utf8, std::size_t maxBytes);
bool SetText(const std::string& utf8);

// Files (and folders: the caller decides) currently on the clipboard / put on it as "copied".
bool GetFiles(std::vector<std::filesystem::path>& files);
bool SetFiles(const std::vector<std::filesystem::path>& files);

std::string Base64Encode(const std::string& in);
bool Base64Decode(const std::string& in, std::string& out);

} // namespace clipboard
