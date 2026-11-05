#include "input/input_backend.hpp"

#if defined(__linux__)

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

#ifndef REL_WHEEL_HI_RES
#define REL_WHEEL_HI_RES 0x0b
#endif
#ifndef REL_HWHEEL_HI_RES
#define REL_HWHEEL_HI_RES 0x0c
#endif

// Wayland gives ordinary clients no way to observe or inject global input, so this
// backend works one level below the compositor:
//   capture    /dev/input/event*  (read passively; EVIOCGRAB while forwarding)
//   injection  /dev/uinput        (a virtual keyboard + an absolute-position mouse)
// It therefore also works on X11 and on the console. Needs the user to be in the
// 'input' group and to have write access to /dev/uinput (see linux-input-setup.sh).
//
// The compositor never reveals where the cursor is, so in local mode the position is
// *estimated* from the raw deltas. Whenever we place the cursor ourselves (absolute
// uinput move) the estimate is exact again. SEAMLESS_INPUT_GAIN scales the estimate
// if your pointer acceleration makes it drift.
namespace {

constexpr int kLongBits = (int)(sizeof(unsigned long) * 8);

template <int N>
struct Bits {
    unsigned long v[(N + kLongBits - 1) / kLongBits] = {};
    bool Test(int b) const { return b >= 0 && b < N && ((v[b / kLongBits] >> (b % kLongBits)) & 1UL) != 0; }
};

constexpr double kTouchpadPxPerMm = 12.0;
constexpr int kAbsMax = 32767;

struct Dev {
    int fd = -1;
    std::string path;
    bool pointer = false, keyboard = false, touchpad = false, hires = false;
    bool grabbed = false;
    // per-frame accumulators
    int relX = 0, relY = 0, wheelV = 0, wheelH = 0;
    double fx = 0, fy = 0;
    // touchpad (single finger = motion)
    int absX = 0, absY = 0, lastX = 0, lastY = 0, fingers = 0, prevFingers = 0;
    bool tool1 = false, tool2 = false, tool3 = false, haveLast = false;
    double kx = 0, ky = 0;

    Dev() = default;
    Dev(const Dev&) = delete;
    Dev& operator=(const Dev&) = delete;
    ~Dev() { if (fd >= 0) close(fd); }
};

void Put(int fd, int type, int code, int value)
{
    input_event e;
    std::memset(&e, 0, sizeof e);
    e.type = (unsigned short)type;
    e.code = (unsigned short)code;
    e.value = value;
    if (write(fd, &e, sizeof e) < 0) { /* device gone: nothing sensible to do */ }
}

int MakeUinput(bool keyboard, const char* name)
{
    const int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    if (keyboard) {
        ioctl(fd, UI_SET_EVBIT, EV_KEY);
        for (int k = 1; k < 0x100; ++k) ioctl(fd, UI_SET_KEYBIT, k);
    } else {
        ioctl(fd, UI_SET_EVBIT, EV_KEY);
        for (int b : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA}) ioctl(fd, UI_SET_KEYBIT, b);
        ioctl(fd, UI_SET_EVBIT, EV_REL);
        for (int r : {REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES}) ioctl(fd, UI_SET_RELBIT, r);
        ioctl(fd, UI_SET_EVBIT, EV_ABS);
        ioctl(fd, UI_SET_ABSBIT, ABS_X);
        ioctl(fd, UI_SET_ABSBIT, ABS_Y);
        for (int axis : {ABS_X, ABS_Y}) {
            uinput_abs_setup a;
            std::memset(&a, 0, sizeof a);
            a.code = (unsigned short)axis;
            a.absinfo.minimum = 0;
            a.absinfo.maximum = kAbsMax;
            a.absinfo.resolution = 0;
            ioctl(fd, UI_ABS_SETUP, &a);
        }
    }
    uinput_setup us;
    std::memset(&us, 0, sizeof us);
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor = 0x5345;
    us.id.product = keyboard ? 1 : 2;
    us.id.version = 1;
    std::strncpy(us.name, name, UINPUT_MAX_NAME_SIZE - 1);
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) { close(fd); return -1; }
    return fd;
}

class EvdevBackend : public InputBackend {
public:
    const char* Name() const override { return "evdev + uinput (Wayland / any session)"; }

    void SetGeometry(const std::vector<InputRect>& m) override
    {
        mons_ = m;
        bounds_ = inputgeom::Bounds(m);
        estX_ = bounds_.x + bounds_.w / 2;
        estY_ = bounds_.y + bounds_.h / 2;
    }

    bool Start(BackendSink sink, std::string* err) override
    {
        sink_ = std::move(sink);
        if (const char* g = std::getenv("SEAMLESS_INPUT_GAIN")) {
            const double v = std::atof(g);
            if (v > 0.05 && v < 20.0) gain_ = v;
        }

        wake_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        inotify_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_ >= 0) inotify_add_watch(inotify_, "/dev/input", IN_CREATE | IN_DELETE);

        uKbd_ = MakeUinput(true,  "seamless-input virtual keyboard");
        uPtr_ = MakeUinput(false, "seamless-input virtual mouse");

        ScanDevices(false);
        int pointers = 0;
        for (const auto& d : devs_) if (d->pointer || d->touchpad) ++pointers;
        if (pointers == 0) {
            std::string m = "no readable mouse found in /dev/input";
            if (denied_) m += " (permission denied: add your user to the 'input' group and log in again, or run linux-input-setup.sh)";
            if (err) *err = m;
            Cleanup();
            return false;
        }
        if (uKbd_ < 0 || uPtr_ < 0) {
            if (err) *err = "cannot open /dev/uinput (load the module and allow access, see linux-input-setup.sh): "
                            "this PC can control others but cannot be controlled";
        }

        run_ = true;
        th_ = std::thread([this] { Loop(); });
        if (uPtr_ >= 0 && !std::getenv("SEAMLESS_INPUT_NO_CALIBRATE")) {
            // The compositor hides the cursor position: centre it once so our estimate is exact.
            Post([this] {
                std::this_thread::sleep_for(std::chrono::milliseconds(400));   // let the compositor pick up the new device
                EmitAbs(estX_, estY_);
            });
        }
        return true;
    }

    void Stop() override
    {
        if (th_.joinable()) {
            run_ = false;
            Signal();
            th_.join();
        }
        Cleanup();
    }

    // Always on the evdev thread (inside onLocalMotion).
    bool BeginForwarding(int, int, std::string* err) override
    {
        ScanDevices(true);                       // also opens the keyboards
        std::string why;
        bool ok = true;
        for (const auto& d : devs_) {            // refuse while something is held: a swallowed release would stick
            Bits<KEY_MAX + 1> held;
            if (ioctl(d->fd, EVIOCGKEY(sizeof held.v), held.v) < 0) continue;
            for (int k = 1; k < 0x100 && ok; ++k)
                if (held.Test(k)) { ok = false; why = "release all keys first"; }
            for (int b = BTN_LEFT; b <= BTN_TASK && ok; ++b)
                if (held.Test(b)) { ok = false; why = "release the mouse buttons first"; }
            if (!ok) break;
        }
        if (ok) {
            for (const auto& d : devs_) {
                if (ioctl(d->fd, EVIOCGRAB, (void*)1) < 0) {
                    ok = false;
                    why = std::string("EVIOCGRAB failed on ") + d->path + ": " + std::strerror(errno);
                    break;
                }
                d->grabbed = true;
            }
        }
        if (!ok) {
            ReleaseGrabs();
            CloseKeyboards();
            if (err) *err = why;
            return false;
        }
        for (const auto& d : devs_) d->relX = d->relY = d->wheelV = d->wheelH = 0;
        fwd_ = true;
        return true;
    }

    void EndForwarding(int x, int y) override
    {
        Post([this, x, y] {
            ReleaseGrabs();
            CloseKeyboards();
            fwd_ = false;
            EmitAbs(x, y);
        });
    }

    // ------------------------------------------------------------ injection
    void InjectMoveTo(int x, int y) override
    {
        std::lock_guard<std::mutex> lk(uMu_);
        EmitAbsLocked(x, y);
    }

    void InjectButton(int button, bool down) override
    {
        static const int codes[5] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA};
        if (button < 0 || button > 4) return;
        std::lock_guard<std::mutex> lk(uMu_);
        if (uPtr_ < 0) return;
        Put(uPtr_, EV_KEY, codes[button], down ? 1 : 0);
        Put(uPtr_, EV_SYN, SYN_REPORT, 0);
    }

    void InjectWheel(int dx, int dy) override
    {
        std::lock_guard<std::mutex> lk(uMu_);
        if (uPtr_ < 0) return;
        if (dy) {
            Put(uPtr_, EV_REL, REL_WHEEL_HI_RES, dy);
            accV_ += dy;
            const int n = accV_ / 120;
            accV_ -= n * 120;
            if (n) Put(uPtr_, EV_REL, REL_WHEEL, n);
        }
        if (dx) {
            Put(uPtr_, EV_REL, REL_HWHEEL_HI_RES, dx);
            accH_ += dx;
            const int n = accH_ / 120;
            accH_ -= n * 120;
            if (n) Put(uPtr_, EV_REL, REL_HWHEEL, n);
        }
        Put(uPtr_, EV_SYN, SYN_REPORT, 0);
    }

    void InjectKey(int code, bool down) override
    {
        if (code < 1 || code > 0xFF) return;
        std::lock_guard<std::mutex> lk(uMu_);
        if (uKbd_ < 0) return;
        if (down && keyDown_.count(code)) return;      // the compositor/clients generate key repeat
        if (down) keyDown_.insert(code); else keyDown_.erase(code);
        Put(uKbd_, EV_KEY, code, down ? 1 : 0);
        Put(uKbd_, EV_SYN, SYN_REPORT, 0);
    }

private:
    // ------------------------------------------------------------ uinput helpers
    void EmitAbs(int x, int y)
    {
        std::lock_guard<std::mutex> lk(uMu_);
        EmitAbsLocked(x, y);
    }

    void EmitAbsLocked(int x, int y)
    {
        estX_ = x;
        estY_ = y;
        if (uPtr_ < 0) return;
        const int ax = (int)std::lround((double)(x - bounds_.x) * kAbsMax / std::max(1, bounds_.w - 1));
        const int ay = (int)std::lround((double)(y - bounds_.y) * kAbsMax / std::max(1, bounds_.h - 1));
        Put(uPtr_, EV_ABS, ABS_X, std::clamp(ax, 0, kAbsMax));
        Put(uPtr_, EV_ABS, ABS_Y, std::clamp(ay, 0, kAbsMax));
        Put(uPtr_, EV_SYN, SYN_REPORT, 0);
    }

    // ------------------------------------------------------------ device handling
    std::unique_ptr<Dev> Probe(const std::string& path, bool wantKeyboards)
    {
        const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { if (errno == EACCES || errno == EPERM) denied_ = true; return nullptr; }

        auto d = std::make_unique<Dev>();
        d->fd = fd;                              // closed by ~Dev on every early return
        d->path = path;

        char name[128] = {};
        ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
        if (!std::strncmp(name, "seamless-input", 14)) return nullptr;     // our own virtual devices

        Bits<EV_MAX + 1> ev;
        Bits<KEY_MAX + 1> key;
        Bits<REL_MAX + 1> rel;
        Bits<ABS_MAX + 1> abs;
        ioctl(fd, EVIOCGBIT(0, sizeof ev.v), ev.v);
        if (ev.Test(EV_KEY)) ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key.v), key.v);
        if (ev.Test(EV_REL)) ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel.v), rel.v);
        if (ev.Test(EV_ABS)) ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs.v), abs.v);

        d->pointer  = rel.Test(REL_X) && rel.Test(REL_Y) && key.Test(BTN_LEFT);
        d->hires    = rel.Test(REL_WHEEL_HI_RES) || rel.Test(REL_HWHEEL_HI_RES);
        d->touchpad = !d->pointer && abs.Test(ABS_X) && abs.Test(ABS_Y) &&
                      key.Test(BTN_TOOL_FINGER) && key.Test(BTN_TOUCH);
        d->keyboard = key.Test(KEY_A) && key.Test(KEY_Z) && key.Test(KEY_ENTER) && key.Test(KEY_SPACE);

        if (d->touchpad) {
            input_absinfo ax, ay;
            std::memset(&ax, 0, sizeof ax);
            std::memset(&ay, 0, sizeof ay);
            ioctl(fd, EVIOCGABS(ABS_X), &ax);
            ioctl(fd, EVIOCGABS(ABS_Y), &ay);
            const double ux = ax.resolution > 0 ? ax.resolution : std::max(1, ax.maximum - ax.minimum) / 90.0;
            const double uy = ay.resolution > 0 ? ay.resolution : std::max(1, ay.maximum - ay.minimum) / 60.0;
            d->kx = kTouchpadPxPerMm / ux;
            d->ky = kTouchpadPxPerMm / uy;
        }

        const bool keep = d->pointer || d->touchpad || (d->keyboard && wantKeyboards);
        if (!keep) return nullptr;
        return d;
    }

    void ScanDevices(bool wantKeyboards)
    {
        std::set<std::string> open;
        for (const auto& d : devs_) open.insert(d->path);

        DIR* dir = opendir("/dev/input");
        if (!dir) return;
        while (const dirent* e = readdir(dir)) {
            if (std::strncmp(e->d_name, "event", 5) != 0) continue;
            const std::string path = std::string("/dev/input/") + e->d_name;
            if (open.count(path)) continue;
            if (auto d = Probe(path, wantKeyboards)) {
                if (fwd_ && !d->grabbed) {        // hot-plugged while forwarding: take it over too
                    if (ioctl(d->fd, EVIOCGRAB, (void*)1) == 0) d->grabbed = true;
                }
                devs_.push_back(std::move(d));
            }
        }
        closedir(dir);
    }

    void ReleaseGrabs()
    {
        for (const auto& d : devs_)
            if (d->grabbed) { ioctl(d->fd, EVIOCGRAB, (void*)0); d->grabbed = false; }
    }

    void CloseKeyboards()
    {
        // keyboards are only kept open while forwarding (nothing reads them in local mode)
        for (auto it = devs_.begin(); it != devs_.end();) {
            if ((*it)->keyboard && !(*it)->pointer && !(*it)->touchpad) it = devs_.erase(it);
            else ++it;
        }
    }

    void Cleanup()
    {
        devs_.clear();
        if (uKbd_ >= 0) { ioctl(uKbd_, UI_DEV_DESTROY); close(uKbd_); uKbd_ = -1; }
        if (uPtr_ >= 0) { ioctl(uPtr_, UI_DEV_DESTROY); close(uPtr_); uPtr_ = -1; }
        if (inotify_ >= 0) { close(inotify_); inotify_ = -1; }
        if (wake_ >= 0)    { close(wake_);    wake_ = -1; }
    }

    // ------------------------------------------------------------ event loop
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
        bool rescan = false;
        auto rescanAt = std::chrono::steady_clock::now();

        while (run_) {
            std::vector<pollfd> fds;
            fds.push_back({wake_, POLLIN, 0});
            fds.push_back({inotify_, POLLIN, 0});
            for (const auto& d : devs_) fds.push_back({d->fd, POLLIN, 0});

            int timeout = -1;
            if (rescan) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    rescanAt - std::chrono::steady_clock::now()).count();
                timeout = (int)std::max<long long>(0, left);
            }
            const int n = poll(fds.data(), (nfds_t)fds.size(), timeout);
            if (n < 0 && errno != EINTR) break;

            if (fds[0].revents & POLLIN) { std::uint64_t v; if (read(wake_, &v, sizeof v) < 0) {} }
            std::vector<std::function<void()>> todo;
            { std::lock_guard<std::mutex> lk(cmdMu_); todo.swap(cmds_); }
            for (auto& f : todo) f();

            if (inotify_ >= 0 && (fds[1].revents & POLLIN)) {
                char buf[4096];
                while (read(inotify_, buf, sizeof buf) > 0) {}
                rescan = true;                                   // let udev apply permissions first
                rescanAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
            }
            if (rescan && std::chrono::steady_clock::now() >= rescanAt) {
                rescan = false;
                ScanDevices(fwd_);
                continue;                                        // devs_ changed: rebuild the poll set
            }

            if (devs_.size() + 2 != fds.size()) continue;        // a command changed the device list: rebuild the poll set

            std::vector<std::string> dead;
            for (size_t i = 0; i < devs_.size() && i + 2 < fds.size(); ++i) {
                const short re = fds[i + 2].revents;
                if (re & (POLLERR | POLLHUP | POLLNVAL)) { dead.push_back(devs_[i]->path); continue; }
                if ((re & POLLIN) && !ReadDev(*devs_[i])) dead.push_back(devs_[i]->path);
            }
            for (const auto& p : dead)
                for (auto it = devs_.begin(); it != devs_.end(); ++it)
                    if ((*it)->path == p) { devs_.erase(it); break; }
        }
        ReleaseGrabs();
    }

    bool ReadDev(Dev& d)
    {
        input_event evs[64];
        for (;;) {
            const ssize_t n = read(d.fd, evs, sizeof evs);
            if (n < 0) return errno == EAGAIN || errno == EINTR;
            if (n == 0) return false;
            const size_t cnt = (size_t)n / sizeof(input_event);
            for (size_t i = 0; i < cnt; ++i) Process(d, evs[i]);
            if ((size_t)n < sizeof evs) return true;
        }
    }

    void Emit(InputKind k, int a, int b)
    {
        if (!sink_.onForwarded) return;
        InputEvent e;
        e.kind = k; e.a = a; e.b = b;
        sink_.onForwarded(e);
    }

    void Process(Dev& d, const input_event& e)
    {
        switch (e.type) {
        case EV_REL:
            switch (e.code) {
            case REL_X: d.relX += e.value; break;
            case REL_Y: d.relY += e.value; break;
            case REL_WHEEL:          if (!d.hires) d.wheelV += e.value * 120; break;
            case REL_WHEEL_HI_RES:   d.wheelV += e.value; break;
            case REL_HWHEEL:         if (!d.hires) d.wheelH += e.value * 120; break;
            case REL_HWHEEL_HI_RES:  d.wheelH += e.value; break;
            default: break;
            }
            break;
        case EV_ABS:
            if (d.touchpad) {
                if (e.code == ABS_X) d.absX = e.value;
                else if (e.code == ABS_Y) d.absY = e.value;
            }
            break;
        case EV_KEY: Key(d, e.code, e.value); break;
        case EV_SYN: if (e.code == SYN_REPORT) Frame(d); break;
        default: break;
        }
    }

    void Key(Dev& d, int code, int value)
    {
        if (code >= BTN_LEFT && code <= BTN_TASK) {                 // mouse buttons
            int b = -1;
            switch (code) {
            case BTN_LEFT: b = 0; break;
            case BTN_RIGHT: b = 1; break;
            case BTN_MIDDLE: b = 2; break;
            case BTN_SIDE: case BTN_BACK: b = 3; break;
            case BTN_EXTRA: case BTN_FORWARD: b = 4; break;
            default: break;
            }
            if (b < 0 || value == 2) return;
            if (value) mask_ |= (1 << b); else mask_ &= ~(1 << b);
            if (fwd_) Emit(InputKind::Button, b, value ? 1 : 0);
            return;
        }
        if (d.touchpad) {                                           // finger count
            if (code == BTN_TOOL_FINGER)         d.tool1 = value != 0;
            else if (code == BTN_TOOL_DOUBLETAP) d.tool2 = value != 0;
            else if (code == BTN_TOOL_TRIPLETAP) d.tool3 = value != 0;
            d.fingers = d.tool3 ? 3 : (d.tool2 ? 2 : (d.tool1 ? 1 : 0));
            return;
        }
        if (fwd_ && code > 0 && code < 0x100 && d.keyboard)         // keyboard keys
            Emit(InputKind::Key, code, value ? 1 : 0);              // value 2 (repeat) -> another "down"
    }

    void Frame(Dev& d)
    {
        double dx = d.relX, dy = d.relY;
        if (d.touchpad) {
            dx = dy = 0;
            if (d.fingers != d.prevFingers) { d.haveLast = false; d.prevFingers = d.fingers; }
            if (d.fingers == 1) {
                if (d.haveLast) { dx = (d.absX - d.lastX) * d.kx; dy = (d.absY - d.lastY) * d.ky; }
                d.lastX = d.absX;
                d.lastY = d.absY;
                d.haveLast = true;
            } else {
                d.haveLast = false;
            }
        }
        d.relX = d.relY = 0;

        d.fx += dx * gain_;
        d.fy += dy * gain_;
        const int ix = (int)d.fx, iy = (int)d.fy;        // keep the fractional part for the next frame
        d.fx -= ix;
        d.fy -= iy;

        if (ix || iy) {
            if (fwd_) {
                Emit(InputKind::Move, ix, iy);
            } else {
                int x = estX_ + ix, y = estY_ + iy;
                if (!mons_.empty()) inputgeom::ClampToAny(mons_, x, y);
                estX_ = x;
                estY_ = y;
                if (sink_.onLocalMotion) sink_.onLocalMotion(x, y, mask_ != 0);
            }
        }
        if (d.wheelV || d.wheelH) {
            if (fwd_) Emit(InputKind::Wheel, d.wheelH, d.wheelV);
            d.wheelV = d.wheelH = 0;
        }
    }

    BackendSink sink_;
    std::vector<InputRect> mons_;
    InputRect bounds_{0, 0, 1920, 1080};
    std::vector<std::unique_ptr<Dev>> devs_;       // evdev thread only (after Start)
    bool denied_ = false;
    double gain_ = 1.0;

    int wake_ = -1, inotify_ = -1;
    std::thread th_;
    std::atomic<std::thread::id> thId_{};
    std::atomic<bool> run_{false};
    std::mutex cmdMu_;
    std::vector<std::function<void()>> cmds_;

    std::atomic<bool> fwd_{false};
    std::atomic<int> mask_{0};
    std::atomic<int> estX_{960}, estY_{540};

    std::mutex uMu_;
    int uKbd_ = -1, uPtr_ = -1;
    int accV_ = 0, accH_ = 0;
    std::set<int> keyDown_;
};

} // namespace

std::unique_ptr<InputBackend> MakeEvdevBackend() { return std::make_unique<EvdevBackend>(); }

#endif // __linux__
