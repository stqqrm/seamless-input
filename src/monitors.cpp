#include "monitors.hpp"

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#elif defined(SI_HAVE_X11)
  #include <X11/Xlib.h>
  #ifdef SI_HAVE_XRANDR
    #include <X11/extensions/Xrandr.h>
  #endif
#endif

namespace monitors {

#if defined(_WIN32)

static BOOL CALLBACK EnumProc(HMONITOR mon, HDC, LPRECT, LPARAM lp)
{
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        auto* v = reinterpret_cast<std::vector<InputRect>*>(lp);
        const RECT& r = mi.rcMonitor;
        if (r.right > r.left && r.bottom > r.top) v->push_back({r.left, r.top, r.right - r.left, r.bottom - r.top});
    }
    return TRUE;
}

std::vector<InputRect> Native()
{
    // Physical pixels, not DPI-virtualised ones (the GUI's manifest does the same for the window).
    using SetCtx = BOOL(WINAPI*)(HANDLE);
    if (HMODULE u = GetModuleHandleW(L"user32.dll"))
        if (auto f = reinterpret_cast<SetCtx>(reinterpret_cast<void*>(GetProcAddress(u, "SetProcessDpiAwarenessContext"))))
            f(reinterpret_cast<HANDLE>(-4));      // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    std::vector<InputRect> v;
    EnumDisplayMonitors(nullptr, nullptr, EnumProc, reinterpret_cast<LPARAM>(&v));
    return v;
}

#elif defined(SI_HAVE_X11)

std::vector<InputRect> Native()
{
    std::vector<InputRect> v;
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) return v;
#ifdef SI_HAVE_XRANDR
    int n = 0;
    if (XRRMonitorInfo* mi = XRRGetMonitors(dpy, DefaultRootWindow(dpy), True, &n)) {
        for (int i = 0; i < n; ++i)
            if (mi[i].width > 0 && mi[i].height > 0) v.push_back({mi[i].x, mi[i].y, mi[i].width, mi[i].height});
        XRRFreeMonitors(mi);
    }
#endif
    if (v.empty()) {
        const int s = DefaultScreen(dpy);
        v.push_back({0, 0, DisplayWidth(dpy, s), DisplayHeight(dpy, s)});
    }
    XCloseDisplay(dpy);
    return v;
}

#else

std::vector<InputRect> Native() { return {}; }

#endif

} // namespace monitors
