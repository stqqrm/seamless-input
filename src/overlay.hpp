#pragma once
// The clipboard telemetry popup ("Clipboard telemetry" in the settings), drawn by the daemon itself so it
// works whether or not the window is open.
//
// A small semi-transparent window with a progress bar ("Uploading" / "Downloading"). It sits in the top
// right corner of the monitor the mouse cursor is on, follows the cursor to another monitor, never takes
// the focus and (Windows) lets mouse clicks through.
//
//   Windows : a layered Win32 window drawn with GDI, on its own thread with its own message loop.
//   X11     : an override-redirect window drawn with Xlib, opacity through _NET_WM_WINDOW_OPACITY (needs a
//             compositing window manager to be translucent; without one it is simply opaque).
//   Wayland : not possible for a normal program (no always-on-top windows); Error() says so.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "input/input_types.hpp"

namespace overlay {

struct Row {
    bool upload = true;              // false: download
    std::string peers;               // UTF-8, comma separated
    std::uint64_t done = 0, total = 0;
    bool unknown = false;            // size not known yet: moving bar instead of a percentage
    bool finished = false;
};

class Popup {
public:
    struct Impl;                     // platform details (overlay.cpp)

    Popup();
    ~Popup();
    Popup(const Popup&) = delete;
    Popup& operator=(const Popup&) = delete;

    // Replaces what is shown; an empty list hides the popup. Thread-safe, returns at once. The popup also
    // hides by itself when nothing has been set for a few seconds.
    void Set(const std::vector<Row>& rows);

    // This PC's monitors in desktop coordinates (used on X11 to find the monitor under the cursor).
    void SetMonitors(const std::vector<InputRect>& mons);

    // Empty while all is well; otherwise why the popup cannot be shown here (no display, Wayland, ...).
    std::string Error() const;

private:
    std::unique_ptr<Impl> d_;
};

} // namespace overlay
