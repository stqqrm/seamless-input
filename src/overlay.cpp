#include "overlay.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#elif defined(SI_HAVE_X11)
  #include <X11/Xatom.h>
  #include <X11/Xlib.h>
  #include <X11/Xutil.h>
  #include <poll.h>
  #include <unistd.h>
#endif

namespace overlay {

namespace {

constexpr int kWidth = 300;          // design pixels (scaled by the monitor's DPI on Windows)
constexpr int kRowHeight = 62;
constexpr int kPad = 8;
constexpr int kSide = 14;
constexpr int kMargin = 16;
constexpr int kBar = 8;
constexpr auto kStale = std::chrono::seconds(4);   // no update from the daemon for this long: hide

// Colours (R, G, B): a dark card that reads well on any desktop, translucent through the window opacity.
struct Rgb { int r, g, b; };
constexpr Rgb kCard{30, 32, 36}, kBorder{74, 78, 88}, kText{236, 238, 242}, kSub{154, 160, 172}, kTrack{62, 66, 76};
constexpr Rgb kUpload{86, 142, 255}, kDownload{72, 200, 132};

std::string Size(std::uint64_t b)
{
    static const char* u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    char buf[48];
    if (i == 0) std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b);
    else        std::snprintf(buf, sizeof buf, "%.1f %s", v, u[i]);
    return buf;
}

std::string Percent(const Row& r)
{
    if (r.unknown) return "...";
    if (!r.total) return "100%";
    return std::to_string((int)std::min<std::uint64_t>(100, r.done * 100 / r.total)) + "%";
}

std::string Info(const Row& r)
{
    std::string s = (r.upload ? "to " : "from ") + r.peers;
    if (r.total >= 1024) s += "   " + Size(r.done) + " of " + Size(r.total);
    return s;
}

// 0..1, or -1 when the bar should move instead (size not known yet)
double Fraction(const Row& r)
{
    if (r.unknown || (!r.total && !r.finished)) return -1;
    return r.total ? std::min(1.0, (double)r.done / (double)r.total) : 1.0;
}

} // namespace

// Shared by the platform code: what to show, written by Set() and read by the popup's thread.
struct Popup::Impl {
    mutable std::mutex m;
    std::vector<Row> rows;
    std::vector<InputRect> mons;
    std::chrono::steady_clock::time_point lastSet{};
    std::string error;
    std::thread th;
    std::atomic<bool> stop{false};

    std::vector<Row> Rows(std::chrono::steady_clock::time_point* last = nullptr)
    {
        std::lock_guard<std::mutex> l(m);
        if (last) *last = lastSet;
        return rows;
    }
    void SetError(const std::string& e) { std::lock_guard<std::mutex> l(m); error = e; }

    void Wake();                     // platform: make the thread look at the new rows
    void Run();                      // platform: the thread
};

// ============================================================================ Windows
#if defined(_WIN32)

namespace {

const wchar_t* kClass = L"SeamlessInputClipboardPopup";
constexpr UINT WM_SET = WM_APP + 1;
constexpr UINT WM_QUIT_NOW = WM_APP + 2;
constexpr UINT_PTR kTimer = 1;

std::wstring Wide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)std::max(n, 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

COLORREF Cr(const Rgb& c) { return RGB(c.r, c.g, c.b); }

// DPI of the monitor (shcore is looked up at run time: not there before Windows 8.1)
double DpiScale(HMONITOR mon)
{
    using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static Fn fn = [] {
        HMODULE h = LoadLibraryW(L"shcore.dll");
        return h ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(h, "GetDpiForMonitor"))) : nullptr;
    }();
    UINT x = 96, y = 96;
    if (fn && SUCCEEDED(fn(mon, 0 /*MDT_EFFECTIVE_DPI*/, &x, &y)) && x) return x / 96.0;
    return 1.0;
}

struct Win {
    Popup::Impl* d = nullptr;
    HWND hwnd = nullptr;
    unsigned step = 0;
    RECT placed{};                  // where the window is now
    double scale = 1.0;
    std::vector<Row> rows;          // what is drawn
};

void HidePopup(Win& w)
{
    KillTimer(w.hwnd, kTimer);
    ShowWindow(w.hwnd, SW_HIDE);
    w.rows.clear();
    w.placed = RECT{};
}

// Top right corner of the work area (without the task bar) of the monitor under the mouse cursor.
void Place(Win& w, bool force)
{
    POINT pt{};
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;
    w.scale = DpiScale(mon);
    const int width = (int)std::lround(kWidth * w.scale);
    const int height = (int)std::lround((kRowHeight * (double)w.rows.size() + 2 * kPad) * w.scale);
    const int margin = (int)std::lround(kMargin * w.scale);
    RECT r{mi.rcWork.right - width - margin, mi.rcWork.top + margin, 0, 0};
    r.right = r.left + width;
    r.bottom = r.top + height;
    if (!force && EqualRect(&r, &w.placed)) return;
    const bool resized = (r.right - r.left) != (w.placed.right - w.placed.left) || (r.bottom - r.top) != (w.placed.bottom - w.placed.top);
    w.placed = r;
    SetWindowPos(w.hwnd, HWND_TOPMOST, r.left, r.top, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (resized) {
        const int rad = (int)std::lround(10 * w.scale);
        SetWindowRgn(w.hwnd, CreateRoundRectRgn(0, 0, width + 1, height + 1, rad, rad), TRUE);   // the system owns the region now
    }
}

void Apply(Win& w)
{
    w.rows = w.d->Rows();
    if (w.rows.empty()) { HidePopup(w); return; }
    const bool wasVisible = IsWindowVisible(w.hwnd) != 0;
    Place(w, true);
    if (!wasVisible) SetTimer(w.hwnd, kTimer, 60, nullptr);
    InvalidateRect(w.hwnd, nullptr, FALSE);
}

void FillRound(HDC dc, const Rgb& c, double x, double y, double wd, double ht, double rad)
{
    HBRUSH b = CreateSolidBrush(Cr(c));
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, (int)std::lround(x), (int)std::lround(y), (int)std::lround(x + wd) + 1, (int)std::lround(y + ht) + 1,
              (int)std::lround(rad), (int)std::lround(rad));
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
}

void Text(HDC dc, const std::string& s, HFONT font, const Rgb& c, RECT rc, UINT flags)
{
    const std::wstring ws = Wide(s);
    HGDIOBJ of = SelectObject(dc, font);
    SetTextColor(dc, Cr(c));
    DrawTextW(dc, ws.c_str(), (int)ws.size(), &rc, flags | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
}

void Paint(Win& w, HDC target, int W, int H)
{
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, W, H);
    HGDIOBJ ob = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);

    HBRUSH bg = CreateSolidBrush(Cr(kCard));
    RECT all{0, 0, W, H};
    FillRect(dc, &all, bg);
    DeleteObject(bg);

    const double s = w.scale;
    HPEN pen = CreatePen(PS_SOLID, 1, Cr(kBorder));
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const int rad = (int)std::lround(10 * s);
    RoundRect(dc, 0, 0, W, H, rad, rad);
    SelectObject(dc, obr);
    SelectObject(dc, op);
    DeleteObject(pen);

    HFONT bold = CreateFontW(-(int)std::lround(13 * s), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT small = CreateFontW(-(int)std::lround(11 * s), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    const double side = kSide * s, barH = kBar * s, rowH = kRowHeight * s;
    const double inner = W - 2 * side;
    for (size_t i = 0; i < w.rows.size(); ++i) {
        const Row& r = w.rows[i];
        const double y = kPad * s + (double)i * rowH;
        const Rgb& col = r.upload ? kUpload : kDownload;

        const int titleTop = (int)std::lround(y + 6 * s);
        const int titleH = (int)std::lround(18 * s);
        Text(dc, r.upload ? "Uploading" : "Downloading", bold, kText, RECT{(int)side, titleTop, W, titleTop + titleH}, DT_LEFT | DT_VCENTER);
        Text(dc, Percent(r), bold, kText, RECT{0, titleTop, (int)(W - side), titleTop + titleH}, DT_RIGHT | DT_VCENTER);

        const double by = y + 6 * s + titleH + 6 * s;
        FillRound(dc, kTrack, side, by, inner, barH, barH);
        const double f = Fraction(r);
        if (f < 0) {                                         // moving segment
            const double seg = inner * 0.3;
            const double pos = std::fmod((double)w.step * 6.0 * s, inner + seg) - seg;
            const double x0 = std::max(0.0, pos), x1 = std::min(inner, pos + seg);
            if (x1 > x0) FillRound(dc, col, side + x0, by, x1 - x0, barH, barH);
        } else if (f > 0) {
            FillRound(dc, col, side, by, std::max(barH, inner * f), barH, barH);
        }

        const int infoTop = (int)std::lround(by + barH + 4 * s);
        Text(dc, Info(r), small, kSub, RECT{(int)side, infoTop, (int)(W - side), infoTop + (int)std::lround(16 * s)},
             DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    }
    DeleteObject(bold);
    DeleteObject(small);

    BitBlt(target, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Win* w = reinterpret_cast<Win*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        break;
    case WM_SET:
        if (w) Apply(*w);
        return 0;
    case WM_QUIT_NOW:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_TIMER:
        if (w && wp == kTimer) {
            ++w->step;
            std::chrono::steady_clock::time_point last;
            w->rows = w->d->Rows(&last);
            if (w->rows.empty() || std::chrono::steady_clock::now() - last > kStale) { HidePopup(*w); return 0; }
            Place(*w, false);                                // the cursor may have gone to another monitor
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (w) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            Paint(*w, dc, rc.right, rc.bottom);
            EndPaint(h, &ps);
            return 0;
        }
        break;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND g_hwndForWake = nullptr;      // set by the thread; read by Wake() (only one popup exists per process)

} // namespace

void Popup::Impl::Wake()
{
    if (g_hwndForWake) PostMessageW(g_hwndForWake, WM_SET, 0, 0);
}

void Popup::Impl::Run()
{
    // physical pixels, like the monitor list (the daemon asked for this already; harmless when repeated)
    using SetCtx = BOOL(WINAPI*)(HANDLE);
    if (HMODULE u = GetModuleHandleW(L"user32.dll"))
        if (auto f = reinterpret_cast<SetCtx>(reinterpret_cast<void*>(GetProcAddress(u, "SetProcessDpiAwarenessContext"))))
            f(reinterpret_cast<HANDLE>(-4));

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    Win w;
    w.d = this;
    w.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, kClass, L"seamless-input clipboard",
                             WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, inst, &w);
    if (!w.hwnd) {
        SetError("could not create the popup window (error " + std::to_string((unsigned long)GetLastError()) + ")");
        return;
    }
    SetLayeredWindowAttributes(w.hwnd, 0, 216, LWA_ALPHA);
    g_hwndForWake = w.hwnd;
    Apply(w);                                                // something may have been set before the window existed

    MSG msg;
    while (!stop && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_hwndForWake = nullptr;
    if (IsWindow(w.hwnd)) DestroyWindow(w.hwnd);
    UnregisterClassW(kClass, inst);
}

// ============================================================================ X11
#elif defined(SI_HAVE_X11)

namespace {

struct X {
    Popup::Impl* d = nullptr;
    Display* dpy = nullptr;
    Window win = 0;
    GC gc = nullptr;
    XFontStruct* bold = nullptr;
    XFontStruct* plain = nullptr;
    unsigned long px[8]{};          // card, border, text, sub, track, upload, download
    int w = 0, h = 0, x = 0, y = 0;
    bool visible = false;
    unsigned step = 0;
    std::vector<Row> rows;
};

enum { CCard, CBorder, CText, CSub, CTrack, CUpload, CDownload };

unsigned long Alloc(Display* dpy, const Rgb& c)
{
    XColor xc{};
    xc.red = (unsigned short)(c.r * 257);
    xc.green = (unsigned short)(c.g * 257);
    xc.blue = (unsigned short)(c.b * 257);
    xc.flags = DoRed | DoGreen | DoBlue;
    if (XAllocColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)), &xc)) return xc.pixel;
    return WhitePixel(dpy, DefaultScreen(dpy));
}

XFontStruct* LoadFont(Display* dpy, bool bold)
{
    const char* names[] = {
        bold ? "-*-helvetica-bold-r-normal--14-*-*-*-*-*-iso8859-1" : "-*-helvetica-medium-r-normal--12-*-*-*-*-*-iso8859-1",
        bold ? "-*-*-bold-r-normal--14-*-*-*-*-*-iso8859-1" : "-*-*-medium-r-normal--12-*-*-*-*-*-iso8859-1",
        "fixed"};
    for (const char* n : names)
        if (XFontStruct* f = XLoadQueryFont(dpy, n)) return f;
    return nullptr;
}

// The core X fonts are Latin-1: anything else would draw as garbage.
std::string Latin(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { o += (char)c; ++i; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {       // two-byte sequences cover U+0080..U+07FF; Latin-1 is up to U+00FF
            const unsigned cp = ((c & 0x1Fu) << 6) | ((unsigned char)s[i + 1] & 0x3Fu);
            o += cp <= 0xFF ? (char)cp : '?';
            i += 2;
        } else {
            o += '?';
            ++i;
            while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) ++i;
        }
    }
    return o;
}

void FillRound(X& x, Drawable dr, unsigned long col, int px, int py, int w, int h)
{
    XSetForeground(x.dpy, x.gc, col);
    if (w <= 0 || h <= 0) return;
    const int r = std::min(h, w);
    if (w <= r) { XFillArc(x.dpy, dr, x.gc, px, py, (unsigned)w, (unsigned)h, 0, 360 * 64); return; }
    XFillArc(x.dpy, dr, x.gc, px, py, (unsigned)r, (unsigned)h, 0, 360 * 64);
    XFillArc(x.dpy, dr, x.gc, px + w - r, py, (unsigned)r, (unsigned)h, 0, 360 * 64);
    XFillRectangle(x.dpy, dr, x.gc, px + r / 2, py, (unsigned)(w - r), (unsigned)h);
}

void DrawStr(X& x, Drawable dr, XFontStruct* f, unsigned long col, int px, int baseline, const std::string& s, int maxW, bool right)
{
    std::string t = Latin(s);
    if (f) {
        XSetFont(x.dpy, x.gc, f->fid);
        while (t.size() > 1 && XTextWidth(f, t.c_str(), (int)t.size()) > maxW) t.pop_back();       // no room: cut
    }
    int tx = px;
    if (right && f) tx = px + maxW - XTextWidth(f, t.c_str(), (int)t.size());
    XSetForeground(x.dpy, x.gc, col);
    XDrawString(x.dpy, dr, x.gc, tx, baseline, t.c_str(), (int)t.size());
}

void Paint(X& x)
{
    Pixmap pm = XCreatePixmap(x.dpy, x.win, (unsigned)x.w, (unsigned)x.h, (unsigned)DefaultDepth(x.dpy, DefaultScreen(x.dpy)));
    XSetForeground(x.dpy, x.gc, x.px[CCard]);
    XFillRectangle(x.dpy, pm, x.gc, 0, 0, (unsigned)x.w, (unsigned)x.h);
    XSetForeground(x.dpy, x.gc, x.px[CBorder]);
    XDrawRectangle(x.dpy, pm, x.gc, 0, 0, (unsigned)x.w - 1, (unsigned)x.h - 1);

    const int inner = x.w - 2 * kSide;
    for (size_t i = 0; i < x.rows.size(); ++i) {
        const Row& r = x.rows[i];
        const int y = kPad + (int)i * kRowHeight;
        const unsigned long col = x.px[r.upload ? CUpload : CDownload];

        DrawStr(x, pm, x.bold, x.px[CText], kSide, y + 18, r.upload ? "Uploading" : "Downloading", inner, false);
        DrawStr(x, pm, x.bold, x.px[CText], kSide, y + 18, Percent(r), inner, true);

        const int by = y + 26;
        FillRound(x, pm, x.px[CTrack], kSide, by, inner, kBar);
        const double f = Fraction(r);
        if (f < 0) {
            const int seg = inner * 3 / 10;
            const int pos = (int)std::fmod((double)x.step * 6.0, (double)(inner + seg)) - seg;
            const int x0 = std::max(0, pos), x1 = std::min(inner, pos + seg);
            if (x1 > x0) FillRound(x, pm, col, kSide + x0, by, x1 - x0, kBar);
        } else if (f > 0) {
            FillRound(x, pm, col, kSide, by, std::max(kBar, (int)(inner * f)), kBar);
        }
        DrawStr(x, pm, x.plain, x.px[CSub], kSide, by + kBar + 14, Info(r), inner, false);
    }
    XCopyArea(x.dpy, pm, x.win, x.gc, 0, 0, (unsigned)x.w, (unsigned)x.h, 0, 0);
    XFreePixmap(x.dpy, pm);
}

// Top right corner of the monitor under the mouse cursor (a little lower than on Windows: panels).
void Place(X& x, bool force)
{
    Window root, child;
    int rx = 0, ry = 0, wx, wy;
    unsigned mask;
    XQueryPointer(x.dpy, DefaultRootWindow(x.dpy), &root, &child, &rx, &ry, &wx, &wy, &mask);
    InputRect mon{0, 0, DisplayWidth(x.dpy, DefaultScreen(x.dpy)), DisplayHeight(x.dpy, DefaultScreen(x.dpy))};
    {
        std::lock_guard<std::mutex> l(x.d->m);
        bool found = false;
        for (const auto& m : x.d->mons)
            if (rx >= m.x && rx < m.x + m.w && ry >= m.y && ry < m.y + m.h) { mon = m; found = true; break; }
        if (!found && !x.d->mons.empty()) mon = x.d->mons.front();
    }
    const int width = kWidth, height = kRowHeight * (int)x.rows.size() + 2 * kPad;
    const int nx = mon.x + mon.w - width - kMargin, ny = mon.y + kMargin + 32;
    if (!force && nx == x.x && ny == x.y && width == x.w && height == x.h) return;
    x.x = nx; x.y = ny; x.w = width; x.h = height;
    XMoveResizeWindow(x.dpy, x.win, nx, ny, (unsigned)width, (unsigned)height);
}

void Hide(X& x)
{
    if (x.visible) XUnmapWindow(x.dpy, x.win);
    x.visible = false;
    x.rows.clear();
    XFlush(x.dpy);
}

void Apply(X& x)
{
    x.rows = x.d->Rows();
    if (x.rows.empty()) { Hide(x); return; }
    Place(x, true);
    if (!x.visible) { XMapRaised(x.dpy, x.win); x.visible = true; }
    else XRaiseWindow(x.dpy, x.win);
    Paint(x);
    XFlush(x.dpy);
}

int g_wakeFd = -1;                  // write end of the wake-up pipe

} // namespace

void Popup::Impl::Wake()
{
    if (g_wakeFd >= 0) { const char c = 1; if (write(g_wakeFd, &c, 1) < 0) {} }
}

void Popup::Impl::Run()
{
    X x;
    x.d = this;
    x.dpy = XOpenDisplay(nullptr);
    if (!x.dpy) {
        SetError("no X display to draw on (Wayland session or no desktop): the popup cannot be shown");
        return;
    }
    const int scr = DefaultScreen(x.dpy);
    XSetWindowAttributes at{};
    at.override_redirect = True;
    at.background_pixel = BlackPixel(x.dpy, scr);
    at.border_pixel = 0;
    at.event_mask = ExposureMask;
    x.w = kWidth;
    x.h = kRowHeight + 2 * kPad;
    x.win = XCreateWindow(x.dpy, RootWindow(x.dpy, scr), 0, 0, (unsigned)x.w, (unsigned)x.h, 0, CopyFromParent, InputOutput,
                          CopyFromParent, CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask, &at);
    x.gc = XCreateGC(x.dpy, x.win, 0, nullptr);
    const Rgb cols[] = {kCard, kBorder, kText, kSub, kTrack, kUpload, kDownload};
    for (int i = 0; i < 7; ++i) x.px[i] = Alloc(x.dpy, cols[i]);
    x.bold = LoadFont(x.dpy, true);
    x.plain = LoadFont(x.dpy, false);

    const Atom opacity = XInternAtom(x.dpy, "_NET_WM_WINDOW_OPACITY", False);     // honoured by compositing window managers
    const unsigned long op = (unsigned long)(0.85 * 0xFFFFFFFFul);
    XChangeProperty(x.dpy, x.win, opacity, XA_CARDINAL, 32, PropModeReplace, reinterpret_cast<unsigned char*>(const_cast<unsigned long*>(&op)), 1);
    const Atom type = XInternAtom(x.dpy, "_NET_WM_WINDOW_TYPE", False);
    const Atom notif = XInternAtom(x.dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
    XChangeProperty(x.dpy, x.win, type, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&notif), 1);

    int pipefd[2];
    if (pipe(pipefd) != 0) { SetError("could not create the popup's wake-up pipe"); XCloseDisplay(x.dpy); return; }
    g_wakeFd = pipefd[1];
    Apply(x);                                                // something may have been set before the window existed

    while (!stop) {
        struct pollfd fds[2] = {{ConnectionNumber(x.dpy), POLLIN, 0}, {pipefd[0], POLLIN, 0}};
        poll(fds, 2, x.visible ? 60 : -1);                   // hidden: sleep until Set() wakes us
        if (stop) break;
        if (fds[1].revents & POLLIN) {
            char buf[64];
            while (read(pipefd[0], buf, sizeof buf) == (ssize_t)sizeof buf) {}
            Apply(x);
        }
        while (XPending(x.dpy)) { XEvent ev; XNextEvent(x.dpy, &ev); }      // Expose: repainted below anyway
        if (x.visible) {
            ++x.step;
            std::chrono::steady_clock::time_point last;
            x.rows = x.d->Rows(&last);
            if (x.rows.empty() || std::chrono::steady_clock::now() - last > kStale) { Hide(x); continue; }
            Place(x, false);
            Paint(x);
            XFlush(x.dpy);
        }
    }
    g_wakeFd = -1;
    close(pipefd[0]);
    close(pipefd[1]);
    if (x.bold) XFreeFont(x.dpy, x.bold);
    if (x.plain) XFreeFont(x.dpy, x.plain);
    XFreeGC(x.dpy, x.gc);
    XDestroyWindow(x.dpy, x.win);
    XCloseDisplay(x.dpy);
}

// ============================================================================ other systems
#else

void Popup::Impl::Wake() {}
void Popup::Impl::Run() { SetError("the clipboard popup is not available on this system"); }

#endif

// ============================================================================ public
Popup::Popup() : d_(new Impl)
{
    d_->th = std::thread([this] { d_->Run(); });
}

Popup::~Popup()
{
    d_->stop = true;
#if defined(_WIN32)
    if (g_hwndForWake) PostMessageW(g_hwndForWake, WM_QUIT_NOW, 0, 0);
#else
    d_->Wake();
#endif
    if (d_->th.joinable()) d_->th.join();
}

void Popup::Set(const std::vector<Row>& rows)
{
    {
        std::lock_guard<std::mutex> l(d_->m);
        d_->rows = rows;
        d_->lastSet = std::chrono::steady_clock::now();
    }
    d_->Wake();
}

void Popup::SetMonitors(const std::vector<InputRect>& mons)
{
    std::lock_guard<std::mutex> l(d_->m);
    d_->mons = mons;
}

std::string Popup::Error() const
{
    std::lock_guard<std::mutex> l(d_->m);
    return d_->error;
}

} // namespace overlay
