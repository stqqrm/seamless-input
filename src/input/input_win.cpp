#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <bitset>
#include <cmath>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "input/input_backend.hpp"

namespace {

// Marks events we inject ourselves so our own hooks ignore them.
constexpr ULONG_PTR kTag = 0x53454D4CUL;
constexpr UINT kMsgKbdOn  = WM_APP + 1;
constexpr UINT kMsgKbdOff = WM_APP + 2;

// ---- key translation: Windows hook (vk / scancode+extended)  <->  Linux evdev code
// Plain (non-extended) set-1 scancodes 0x01..0x58 are numerically identical to evdev codes.
struct ExtEntry { unsigned char scan; unsigned short code; };
constexpr ExtEntry kExt[] = {
    {0x1C, 96},  {0x1D, 97},  {0x35, 98},  {0x37, 99},  {0x38, 100}, {0x47, 102},
    {0x48, 103}, {0x49, 104}, {0x4B, 105}, {0x4D, 106}, {0x4F, 107}, {0x50, 108},
    {0x51, 109}, {0x52, 110}, {0x53, 111}, {0x5B, 125}, {0x5C, 126}, {0x5D, 127},
};
// Keys whose scancodes are unreliable in the hook: identified by virtual-key.
struct VkEntry { unsigned char vk; unsigned short code; };
constexpr VkEntry kVk[] = {
    {VK_PAUSE, 119},          {VK_NUMLOCK, 69},          {VK_SNAPSHOT, 99},
    {VK_VOLUME_MUTE, 113},    {VK_VOLUME_DOWN, 114},     {VK_VOLUME_UP, 115},
    {VK_MEDIA_NEXT_TRACK, 163}, {VK_MEDIA_PREV_TRACK, 165}, {VK_MEDIA_STOP, 166},
    {VK_MEDIA_PLAY_PAUSE, 164},
};

int ToEvdev(const KBDLLHOOKSTRUCT& k)
{
    for (const auto& e : kVk)
        if (k.vkCode == e.vk) return e.code;
    if (k.flags & LLKHF_EXTENDED) {
        for (const auto& e : kExt)
            if (k.scanCode == e.scan) return e.code;
        return 0;                       // e.g. the fake Shift Windows adds around numpad keys
    }
    return (k.scanCode >= 1 && k.scanCode <= 0x58) ? (int)k.scanCode : 0;
}

class WindowsBackend : public InputBackend {
public:
    const char* Name() const override { return "Windows (SetWindowsHookEx / SendInput)"; }

    bool Start(BackendSink sink, std::string* err) override
    {
        sink_ = std::move(sink);
        self_ = this;
        std::unique_lock<std::mutex> lk(initMu_);
        th_ = std::thread([this] { Run(); });
        initCv_.wait(lk, [this] { return initDone_; });
        if (!hookOk_) {
            if (th_.joinable()) th_.join();
            if (err) *err = "SetWindowsHookEx(WH_MOUSE_LL) failed, error " + std::to_string(initErr_);
            return false;
        }
        return true;
    }

    void Stop() override
    {
        if (th_.joinable()) {
            PostThreadMessageW(tid_, WM_QUIT, 0, 0);
            th_.join();
        }
        if (self_ == this) self_ = nullptr;
    }

    bool BeginForwarding(int parkX, int parkY, std::string*) override
    {
        // Keys that are already down keep working for their owner: their release must pass through.
        held_.reset();
        for (int vk = 8; vk < 255; ++vk)
            if (GetAsyncKeyState(vk) & 0x8000) held_.set((size_t)vk);
        anchorX_ = parkX;
        anchorY_ = parkY;
        SetCursorPos(parkX, parkY);
        fwd_ = true;
        PostThreadMessageW(tid_, kMsgKbdOn, 0, 0);
        return true;
    }

    void EndForwarding(int x, int y) override
    {
        fwd_ = false;
        PostThreadMessageW(tid_, kMsgKbdOff, 0, 0);
        SetCursorPos(x, y);
    }

    // ------------------------------------------------------------ injection
    void InjectMoveTo(int x, int y) override
    {
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = std::max(2, (int)GetSystemMetrics(SM_CXVIRTUALSCREEN));
        const int vh = std::max(2, (int)GetSystemMetrics(SM_CYVIRTUALSCREEN));
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = (LONG)std::lround((x - vx) * 65535.0 / (vw - 1));
        in.mi.dy = (LONG)std::lround((y - vy) * 65535.0 / (vh - 1));
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        in.mi.dwExtraInfo = kTag;
        SendInput(1, &in, sizeof in);
    }

    void InjectButton(int button, bool down) override
    {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwExtraInfo = kTag;
        switch (button) {
        case 0: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN   : MOUSEEVENTF_LEFTUP;   break;
        case 1: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN  : MOUSEEVENTF_RIGHTUP;  break;
        case 2: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        case 3: in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; in.mi.mouseData = XBUTTON1; break;
        case 4: in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; in.mi.mouseData = XBUTTON2; break;
        default: return;
        }
        SendInput(1, &in, sizeof in);
    }

    void InjectWheel(int dx, int dy) override
    {
        INPUT in[2] = {};
        UINT n = 0;
        if (dy) {
            in[n].type = INPUT_MOUSE;
            in[n].mi.dwFlags = MOUSEEVENTF_WHEEL;
            in[n].mi.mouseData = (DWORD)dy;
            in[n].mi.dwExtraInfo = kTag;
            ++n;
        }
        if (dx) {
            in[n].type = INPUT_MOUSE;
            in[n].mi.dwFlags = MOUSEEVENTF_HWHEEL;
            in[n].mi.mouseData = (DWORD)dx;
            in[n].mi.dwExtraInfo = kTag;
            ++n;
        }
        if (n) SendInput(n, in, sizeof(INPUT));
    }

    void InjectKey(int code, bool down) override
    {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.dwExtraInfo = kTag;
        DWORD fl = down ? 0 : KEYEVENTF_KEYUP;
        bool found = false;
        for (const auto& e : kVk)
            if (e.code == code) {
                in.ki.wVk = e.vk;
                if (e.vk == VK_NUMLOCK) fl |= KEYEVENTF_EXTENDEDKEY;
                found = true;
                break;
            }
        if (!found)
            for (const auto& e : kExt)
                if (e.code == code) {
                    in.ki.wScan = e.scan;
                    fl |= KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY;
                    found = true;
                    break;
                }
        if (!found && code >= 1 && code <= 0x58) {
            in.ki.wScan = (WORD)code;
            fl |= KEYEVENTF_SCANCODE;
            found = true;
        }
        if (!found) return;
        in.ki.dwFlags = fl;
        SendInput(1, &in, sizeof in);
    }

private:
    // ------------------------------------------------------------ hook thread
    void Run()
    {
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // create the message queue
        tid_ = GetCurrentThreadId();
        mouse_ = SetWindowsHookExW(WH_MOUSE_LL, &MouseThunk, GetModuleHandleW(nullptr), 0);
        {
            std::lock_guard<std::mutex> lk(initMu_);
            hookOk_ = mouse_ != nullptr;
            initErr_ = hookOk_ ? 0 : GetLastError();
            initDone_ = true;
        }
        initCv_.notify_all();
        if (!mouse_) return;

        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == kMsgKbdOn) {
                // The keyboard hook only exists while input is being forwarded.
                if (!kbd_ && fwd_) kbd_ = SetWindowsHookExW(WH_KEYBOARD_LL, &KbdThunk, GetModuleHandleW(nullptr), 0);
            } else if (msg.message == kMsgKbdOff) {
                if (kbd_) { UnhookWindowsHookEx(kbd_); kbd_ = nullptr; }
            } else {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (kbd_)   { UnhookWindowsHookEx(kbd_);   kbd_ = nullptr; }
        if (mouse_) { UnhookWindowsHookEx(mouse_); mouse_ = nullptr; }
    }

    static LRESULT CALLBACK MouseThunk(int code, WPARAM wp, LPARAM lp)
    {
        return self_ ? self_->OnMouse(code, wp, lp) : CallNextHookEx(nullptr, code, wp, lp);
    }
    static LRESULT CALLBACK KbdThunk(int code, WPARAM wp, LPARAM lp)
    {
        return self_ ? self_->OnKbd(code, wp, lp) : CallNextHookEx(nullptr, code, wp, lp);
    }

    void Emit(InputKind k, int a, int b)
    {
        if (!sink_.onForwarded) return;
        InputEvent e;
        e.kind = k; e.a = a; e.b = b;
        sink_.onForwarded(e);
    }

    LRESULT OnMouse(int code, WPARAM wp, LPARAM lp)
    {
        if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);
        const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);

        // distance since the previous mouse event (also for our own injections, so a jump
        // we caused is not mistaken for the user moving the mouse afterwards)
        int moved = 0;
        if (wp == WM_MOUSEMOVE) {
            moved = std::abs(ms->pt.x - lastPtX_) + std::abs(ms->pt.y - lastPtY_);
            lastPtX_ = ms->pt.x;
            lastPtY_ = ms->pt.y;
        }
        if (ms->dwExtraInfo == kTag) return CallNextHookEx(nullptr, code, wp, lp);   // our own injection

        if (!fwd_ && sink_.onPhysicalInput) {
            if (wp == WM_MOUSEMOVE && !(ms->flags & LLMHF_INJECTED) && moved > 0) sink_.onPhysicalInput(moved);
            else if (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN) sink_.onPhysicalInput(100);
        }

        // wire button number + held-button bookkeeping
        int btn = -1, bit = 0;
        bool down = false;
        switch (wp) {
        case WM_LBUTTONDOWN: btn = 0; bit = 1;  down = true; break;
        case WM_LBUTTONUP:   btn = 0; bit = 1;  break;
        case WM_RBUTTONDOWN: btn = 1; bit = 2;  down = true; break;
        case WM_RBUTTONUP:   btn = 1; bit = 2;  break;
        case WM_MBUTTONDOWN: btn = 2; bit = 4;  down = true; break;
        case WM_MBUTTONUP:   btn = 2; bit = 4;  break;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            btn  = HIWORD(ms->mouseData) == XBUTTON1 ? 3 : 4;
            bit  = btn == 3 ? 8 : 16;
            down = wp == WM_XBUTTONDOWN;
            break;
        default: break;
        }
        if (bit) { if (down) mask_ |= bit; else mask_ &= ~bit; }

        if (!fwd_) {
            if (wp == WM_MOUSEMOVE && sink_.onLocalMotion) {
                sink_.onLocalMotion(ms->pt.x, ms->pt.y, mask_ != 0);
                if (fwd_) return 1;                 // forwarding just began: eat this move
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        }

        // ---- forwarding: everything is swallowed and sent to the peer
        if (wp == WM_MOUSEMOVE) {
            const int dx = ms->pt.x - anchorX_, dy = ms->pt.y - anchorY_;
            if (dx || dy) {
                Emit(InputKind::Move, dx, dy);
                SetCursorPos(anchorX_, anchorY_);   // keep the cursor parked: no screen-edge clamping
            }
            return 1;
        }
        if (btn >= 0) { Emit(InputKind::Button, btn, down ? 1 : 0); return 1; }
        if (wp == WM_MOUSEWHEEL)  { Emit(InputKind::Wheel, 0, (short)HIWORD(ms->mouseData)); return 1; }
        if (wp == WM_MOUSEHWHEEL) { Emit(InputKind::Wheel, (short)HIWORD(ms->mouseData), 0); return 1; }
        return 1;
    }

    LRESULT OnKbd(int code, WPARAM wp, LPARAM lp)
    {
        if (code != HC_ACTION || !fwd_) return CallNextHookEx(nullptr, code, wp, lp);
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (k->dwExtraInfo == kTag) return CallNextHookEx(nullptr, code, wp, lp);

        const bool up = (k->flags & LLKHF_UP) != 0;
        const size_t vk = k->vkCode & 0xFF;
        if (held_[vk]) {                            // pressed before the switch: let its release through
            if (up) { held_.reset(vk); return CallNextHookEx(nullptr, code, wp, lp); }
            return 1;                               // auto-repeat of that key: swallow
        }
        if (const int ev = ToEvdev(*k)) Emit(InputKind::Key, ev, up ? 0 : 1);
        return 1;
    }

    inline static WindowsBackend* self_ = nullptr;

    BackendSink sink_;
    std::thread th_;
    DWORD tid_ = 0;
    HHOOK mouse_ = nullptr, kbd_ = nullptr;
    std::mutex initMu_;
    std::condition_variable initCv_;
    bool initDone_ = false, hookOk_ = false;
    DWORD initErr_ = 0;

    std::atomic<bool> fwd_{false};
    std::atomic<int> mask_{0};
    int lastPtX_ = 0, lastPtY_ = 0;      // hook thread only
    int anchorX_ = 0, anchorY_ = 0;
    std::bitset<256> held_;
};

} // namespace

std::unique_ptr<InputBackend> MakeWindowsBackend() { return std::make_unique<WindowsBackend>(); }

#endif // _WIN32
