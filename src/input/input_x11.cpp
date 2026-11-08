#include "input/input_backend.hpp"

#if defined(__linux__)

#if defined(SI_HAVE_X11)

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <bitset>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

namespace {

class X11Backend : public InputBackend {
public:
    const char* Name() const override { return "X11 (XInput2 / XGrab / XTest)"; }
    bool CanInject() const override { return xtest_; }

    bool Start(BackendSink sink, std::string* err) override
    {
        sink_ = std::move(sink);

        dpy_ = XOpenDisplay(nullptr);
        if (!dpy_) { if (err) *err = "cannot open the X display"; return false; }
        root_ = DefaultRootWindow(dpy_);

        int ev = 0, er = 0;
        if (!XQueryExtension(dpy_, "XInputExtension", &xiOp_, &ev, &er)) {
            if (err) *err = "the X server has no XInput extension";
            return Fail();
        }
        int maj = 2, min = 0;
        if (XIQueryVersion(dpy_, &maj, &min) == BadRequest) {
            if (err) *err = "XInput 2 is required";
            return Fail();
        }

        // Raw motion = "the pointer moved", delivered without any grab.
        unsigned char mask[XIMaskLen(XI_LASTEVENT)] = {};
        XISetMask(mask, XI_RawMotion);
        XISetMask(mask, XI_RawButtonPress);
        XIEventMask em;
        em.deviceid = XIAllMasterDevices;
        em.mask_len = (int)sizeof mask;
        em.mask = mask;
        XISelectEvents(dpy_, root_, &em, 1);

        // The XTEST pointer is what OUR injection (and xdotool & co.) comes from: motion from
        // it must not count as "the user moved the mouse".
        {
            int n = 0;
            XIDeviceInfo* info = XIQueryDevice(dpy_, XIAllDevices, &n);
            for (int i = 0; info && i < n; ++i)
                if (info[i].use == XISlavePointer && info[i].name && std::strstr(info[i].name, "XTEST"))
                    xtestIds_.insert(info[i].deviceid);
            if (info) XIFreeDeviceInfo(info);
        }

        Bool supported = False;
        XkbSetDetectableAutoRepeat(dpy_, True, &supported);   // repeats arrive as presses only

        Pixmap pm = XCreatePixmap(dpy_, root_, 1, 1, 1);
        XColor black;
        black.pixel = 0; black.red = black.green = black.blue = 0; black.flags = 0; black.pad = 0;
        blank_ = XCreatePixmapCursor(dpy_, pm, pm, &black, &black, 0, 0);
        XFreePixmap(dpy_, pm);

        inj_ = XOpenDisplay(nullptr);
        if (inj_) {
            int a, b, c, d;
            xtest_ = XTestQueryExtension(inj_, &a, &b, &c, &d) != 0;
        }
        if (!xtest_ && err) *err = "XTEST extension unavailable: this PC can control others but cannot be controlled";

        wake_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        run_ = true;
        th_ = std::thread([this] { Loop(); });
        return true;
    }

    void Stop() override
    {
        if (th_.joinable()) {
            run_ = false;
            Signal();
            th_.join();
        }
        if (wake_ >= 0) { close(wake_); wake_ = -1; }
        if (inj_) { XCloseDisplay(inj_); inj_ = nullptr; }
        if (dpy_) { XCloseDisplay(dpy_); dpy_ = nullptr; }
    }

    // Called on the X thread from inside onLocalMotion.
    bool BeginForwarding(int parkX, int parkY, std::string* err) override
    {
        if (std::this_thread::get_id() != thId_.load()) { if (err) *err = "internal error: not on the X thread"; return false; }
        int r = XGrabPointer(dpy_, root_, False,
                             PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
                             GrabModeAsync, GrabModeAsync, None, blank_, CurrentTime);
        if (r != GrabSuccess) {
            if (err) *err = "pointer grab refused (a menu or another program holds the grab)";
            return false;
        }
        r = XGrabKeyboard(dpy_, root_, False, GrabModeAsync, GrabModeAsync, CurrentTime);
        if (r != GrabSuccess) {
            XUngrabPointer(dpy_, CurrentTime);
            XFlush(dpy_);
            if (err) *err = "keyboard grab refused (a menu or another program holds the grab)";
            return false;
        }
        parkX_ = lastX_ = parkX;
        parkY_ = lastY_ = parkY;
        XWarpPointer(dpy_, None, root_, 0, 0, 0, 0, parkX, parkY);
        XFlush(dpy_);
        warping_ = true;          // ignore stale motion until the warp is acknowledged
        fwd_ = true;
        return true;
    }

    void EndForwarding(int x, int y) override
    {
        Post([this, x, y] {
            if (fwd_) {
                XUngrabKeyboard(dpy_, CurrentTime);
                XUngrabPointer(dpy_, CurrentTime);
            }
            fwd_ = false;
            warping_ = false;
            XWarpPointer(dpy_, None, root_, 0, 0, 0, 0, x, y);
            XFlush(dpy_);
        });
    }

    // ------------------------------------------------------------ injection
    void InjectMoveTo(int x, int y) override
    {
        std::lock_guard<std::mutex> lk(injMu_);
        if (!xtest_) return;
        XTestFakeMotionEvent(inj_, -1, x, y, CurrentTime);
        XFlush(inj_);
    }

    void InjectButton(int button, bool down) override
    {
        static const unsigned map[5] = {1, 3, 2, 8, 9};      // wire -> X button
        if (button < 0 || button > 4) return;
        std::lock_guard<std::mutex> lk(injMu_);
        if (!xtest_) return;
        XTestFakeButtonEvent(inj_, map[button], down ? True : False, CurrentTime);
        XFlush(inj_);
    }

    void InjectWheel(int dx, int dy) override
    {
        std::lock_guard<std::mutex> lk(injMu_);
        if (!xtest_) return;
        accX_ += dx;
        accY_ += dy;
        while (accY_ >=  120) { Click(4); accY_ -= 120; }
        while (accY_ <= -120) { Click(5); accY_ += 120; }
        while (accX_ >=  120) { Click(7); accX_ -= 120; }
        while (accX_ <= -120) { Click(6); accX_ += 120; }
        XFlush(inj_);
    }

    void InjectKey(int code, bool down) override
    {
        const unsigned kc = (unsigned)code + 8;                // evdev -> X keycode
        if (kc < 8 || kc > 255) return;
        std::lock_guard<std::mutex> lk(injMu_);
        if (!xtest_) return;
        if (down && keyDown_[kc]) return;                      // the X server does its own auto-repeat
        keyDown_[kc] = down;
        XTestFakeKeyEvent(inj_, kc, down ? True : False, CurrentTime);
        XFlush(inj_);
    }

private:
    bool Fail()
    {
        if (dpy_) { XCloseDisplay(dpy_); dpy_ = nullptr; }
        return false;
    }

    void Click(unsigned b)
    {
        XTestFakeButtonEvent(inj_, b, True, CurrentTime);
        XTestFakeButtonEvent(inj_, b, False, CurrentTime);
    }

    // ------------------------------------------------------------ X thread
    void Signal() { if (wake_ >= 0) { std::uint64_t one = 1; if (write(wake_, &one, sizeof one) < 0) {} } }

    void Post(std::function<void()> fn)
    {
        if (std::this_thread::get_id() == thId_.load()) { fn(); return; }
        { std::lock_guard<std::mutex> lk(cmdMu_); cmds_.push_back(std::move(fn)); }
        Signal();
    }

    void Loop()
    {
        thId_.store(std::this_thread::get_id());
        pollfd fds[2];
        fds[0].fd = ConnectionNumber(dpy_); fds[0].events = POLLIN; fds[0].revents = 0;
        fds[1].fd = wake_;                  fds[1].events = POLLIN; fds[1].revents = 0;

        while (run_) {
            while (XPending(dpy_)) {
                XEvent ev;
                XNextEvent(dpy_, &ev);
                Handle(ev);
            }
            std::vector<std::function<void()>> todo;
            { std::lock_guard<std::mutex> lk(cmdMu_); todo.swap(cmds_); }
            for (auto& f : todo) f();
            XFlush(dpy_);
            if (!run_) break;
            if (poll(fds, 2, -1) < 0) continue;
            if (fds[1].revents & POLLIN) { std::uint64_t v; if (read(wake_, &v, sizeof v) < 0) {} }
        }
        if (fwd_) {
            XUngrabKeyboard(dpy_, CurrentTime);
            XUngrabPointer(dpy_, CurrentTime);
            XFlush(dpy_);
            fwd_ = false;
        }
    }

    void Handle(XEvent& ev)
    {
        switch (ev.type) {
        case GenericEvent:
            if (ev.xcookie.extension == xiOp_ && XGetEventData(dpy_, &ev.xcookie)) {
                if (!fwd_) {
                    const auto* re = static_cast<const XIRawEvent*>(ev.xcookie.data);
                    const bool injected = re && xtestIds_.count(re->sourceid) != 0;
                    if (ev.xcookie.evtype == XI_RawMotion) {
                        LocalMotion();
                        if (!injected && sink_.onPhysicalInput) sink_.onPhysicalInput(RawWeight(re));
                    } else if (ev.xcookie.evtype == XI_RawButtonPress) {
                        if (!injected && sink_.onPhysicalInput) sink_.onPhysicalInput(100);
                    }
                }
                XFreeEventData(dpy_, &ev.xcookie);
            }
            break;
        case MotionNotify:
            if (fwd_) FwdMotion(ev.xmotion.x_root, ev.xmotion.y_root);
            break;
        case ButtonPress:
        case ButtonRelease:
            if (fwd_) FwdButton(ev.xbutton.button, ev.type == ButtonPress);
            break;
        case KeyPress:
        case KeyRelease:
            if (fwd_ && ev.xkey.keycode >= 9) {
                InputEvent e;
                e.kind = InputKind::Key;
                e.a = (int)ev.xkey.keycode - 8;
                e.b = ev.type == KeyPress ? 1 : 0;
                if (sink_.onForwarded) sink_.onForwarded(e);
            }
            break;
        default: break;
        }
    }

    // |dx| + |dy| of a raw motion event (valuator 0 = x, 1 = y), at least 1.
    static int RawWeight(const XIRawEvent* re)
    {
        if (!re) return 1;
        double dx = 0, dy = 0;
        const double* v = re->raw_values;
        for (int i = 0; i < re->valuators.mask_len * 8; ++i) {
            if (!XIMaskIsSet(re->valuators.mask, i)) continue;
            const double val = *v++;
            if (i == 0) dx = val; else if (i == 1) dy = val;
        }
        return std::max(1, (int)std::lround(std::fabs(dx) + std::fabs(dy)));
    }

    void LocalMotion()
    {
        Window r, c;
        int rx, ry, wx, wy;
        unsigned mask = 0;
        if (!XQueryPointer(dpy_, root_, &r, &c, &rx, &ry, &wx, &wy, &mask)) return;
        const bool btn = (mask & (Button1Mask | Button2Mask | Button3Mask | Button4Mask | Button5Mask)) != 0;
        if (sink_.onLocalMotion) sink_.onLocalMotion(rx, ry, btn);
    }

    void FwdMotion(int x, int y)
    {
        if (warping_) {                         // stale events queued before our warp
            if (x == parkX_ && y == parkY_) { warping_ = false; lastX_ = x; lastY_ = y; }
            return;
        }
        const int dx = x - lastX_, dy = y - lastY_;
        lastX_ = x;
        lastY_ = y;
        if ((dx || dy) && sink_.onForwarded) {
            InputEvent e;
            e.kind = InputKind::Move; e.a = dx; e.b = dy;
            sink_.onForwarded(e);
        }
        // Re-centre before the pointer reaches a screen edge (where it would clamp).
        if (std::abs(x - parkX_) > 200 || std::abs(y - parkY_) > 200) {
            XWarpPointer(dpy_, None, root_, 0, 0, 0, 0, parkX_, parkY_);
            XFlush(dpy_);
            warping_ = true;
        }
    }

    void FwdButton(unsigned b, bool down)
    {
        if (!sink_.onForwarded) return;
        InputEvent e;
        switch (b) {
        case 1: e.kind = InputKind::Button; e.a = 0; e.b = down; break;
        case 2: e.kind = InputKind::Button; e.a = 2; e.b = down; break;
        case 3: e.kind = InputKind::Button; e.a = 1; e.b = down; break;
        case 8: e.kind = InputKind::Button; e.a = 3; e.b = down; break;
        case 9: e.kind = InputKind::Button; e.a = 4; e.b = down; break;
        case 4: if (!down) return; e.kind = InputKind::Wheel; e.a = 0;    e.b = 120;  break;
        case 5: if (!down) return; e.kind = InputKind::Wheel; e.a = 0;    e.b = -120; break;
        case 6: if (!down) return; e.kind = InputKind::Wheel; e.a = -120; e.b = 0;    break;
        case 7: if (!down) return; e.kind = InputKind::Wheel; e.a = 120;  e.b = 0;    break;
        default: return;
        }
        sink_.onForwarded(e);
    }

    BackendSink sink_;
    Display* dpy_ = nullptr;      // X thread only
    Display* inj_ = nullptr;      // injection, guarded by injMu_
    Window root_ = 0;
    int xiOp_ = 0;
    Cursor blank_ = 0;
    int wake_ = -1;
    std::thread th_;
    std::atomic<std::thread::id> thId_{};
    std::atomic<bool> run_{false};

    std::mutex cmdMu_;
    std::vector<std::function<void()>> cmds_;

    std::set<int> xtestIds_;                  // slave pointer ids of the XTEST device
    bool fwd_ = false, warping_ = false;      // X thread only
    int parkX_ = 0, parkY_ = 0, lastX_ = 0, lastY_ = 0;

    std::mutex injMu_;
    bool xtest_ = false;
    int accX_ = 0, accY_ = 0;
    std::bitset<256> keyDown_;
};

} // namespace

std::unique_ptr<InputBackend> MakeX11Backend(std::string*) { return std::make_unique<X11Backend>(); }

#else  // built without X11 development files

std::unique_ptr<InputBackend> MakeX11Backend(std::string* why)
{
    if (why) *why = "this build has no X11 support (libX11/libXi/libXtst were not found); use the evdev backend";
    return nullptr;
}

#endif // SI_HAVE_X11
#endif // __linux__
