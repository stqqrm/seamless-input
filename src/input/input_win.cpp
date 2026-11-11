#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "input/input_backend.hpp"

namespace {

// Marks events we inject ourselves so our own hooks ignore them.
constexpr ULONG_PTR kTag = 0x53454D4CUL;
constexpr UINT kMsgKbdOn  = WM_APP + 1;
constexpr UINT kMsgKbdOff = WM_APP + 2;
constexpr UINT kMsgTask   = WM_APP + 3;   // lParam: heap-allocated std::function<void()> to run on the hook thread
constexpr unsigned kDeskCheckTicks = 4;   // check for a desktop switch every 4th edge poll (~120 ms)
constexpr UINT kPollMs    = 30;           // edge polling while input is local (see PollEdge)

// ---- key translation: Windows hook (vk / scancode+extended)  <->  Linux evdev code
// Plain (non-extended) set-1 scancodes 0x01..0x58 are numerically identical to evdev codes.
struct ExtEntry { unsigned char scan; unsigned short code; };
constexpr ExtEntry kExt[] = {
    {0x1C, 96},  {0x1D, 97},  {0x35, 98},  {0x36, 54},  {0x37, 99},  {0x38, 100}, {0x47, 102},
    {0x48, 103}, {0x49, 104}, {0x4B, 105}, {0x4D, 106}, {0x4F, 107}, {0x50, 108},
    {0x51, 109}, {0x52, 110}, {0x53, 111}, {0x5B, 125}, {0x5C, 126}, {0x5D, 127},
};
// Note 0x36 = Right Shift: the low-level hook reports it WITH LLKHF_EXTENDED on many systems,
// so it must be in this table or it is dropped (the fake 0x2A Shift around numpad keys stays dropped).
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

// Administrator (elevated) or SYSTEM: only then do hooks and SendInput reach elevated windows.
bool IsPrivileged()
{
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el{};
    DWORD n = 0;
    const bool ok = GetTokenInformation(tok, TokenElevation, &el, sizeof el, &n) && el.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

std::wstring DesktopName(HDESK d)
{
    wchar_t buf[256] = {};
    DWORD need = 0;
    if (!d || !GetUserObjectInformationW(d, UOI_NAME, buf, sizeof buf, &need)) return {};
    return buf;
}

class WindowsBackend : public InputBackend {
public:
    const char* Name() const override { return "Windows (SetWindowsHookEx / SendInput)"; }

    ~WindowsBackend() override { ShowCursorAgain(); }

    void SetGeometry(const std::vector<InputRect>& monitors) override { mons_ = monitors; }

    bool Start(BackendSink sink, std::string* err) override
    {
        sink_ = std::move(sink);
        self_ = this;
        RestoreSystemCursors();             // a previous run that crashed may have left them invisible
        std::unique_lock<std::mutex> lk(initMu_);
        th_ = std::thread([this] { Run(); });
        initCv_.wait(lk, [this] { return initDone_; });
        if (!hookOk_) {
            if (th_.joinable()) th_.join();
            if (err) *err = "SetWindowsHookEx(WH_MOUSE_LL) failed, error " + std::to_string(initErr_);
            return false;
        }
        ntSend_ = nullptr;
        if (!std::getenv("SI_NO_NT"))       // SI_NO_NT=1: always use the documented SendInput
            if (HMODULE w = LoadLibraryW(L"win32u.dll"))
                ntSend_ = reinterpret_cast<NtSendFn>(reinterpret_cast<void*>(GetProcAddress(w, "NtUserSendInput")));
        StartWorkers();
        if (err && !IsPrivileged())
            *err = "running without administrator rights: Task Manager, other elevated windows and UAC prompts cannot be "
                   "controlled. Start it through the Windows service (the window does this by itself: allow its one "
                   "UAC prompt, and keep the program in a folder only administrators can write to).";
        return true;
    }

    void Stop() override
    {
        StopWorkers();
        if (th_.joinable()) {
            PostThreadMessageW(tid_, WM_QUIT, 0, 0);
            th_.join();
        }
        if (self_ == this) self_ = nullptr;
        ShowCursorAgain();
    }

    bool BeginForwarding(int parkX, int parkY, std::string*) override
    {
        // Keys that are already down keep working for their owner: their release must pass through.
        held_.reset();
        for (int vk = 8; vk < 255; ++vk)
            if (GetAsyncKeyState(vk) & 0x8000) held_.set((size_t)vk);
        anchorX_ = parkX;
        anchorY_ = parkY;
        HideCursor();
        SetCursorPos(parkX, parkY);
        fwd_ = true;
        PostThreadMessageW(tid_, kMsgKbdOn, 0, 0);
        return true;
    }

    void EndForwarding(int x, int y) override
    {
        fwd_ = false;
        PostThreadMessageW(tid_, kMsgKbdOff, 0, 0);
        // cursor and cursor scheme belong to the desktop the hook thread is on (it follows the UAC desktop)
        RunOnHookThread([this, x, y] { SetCursorPos(x, y); ShowCursorAgain(); });
    }

    // ------------------------------------------------------------ injection (any thread)
    // Injection runs on TWO worker threads of its own, one for the mouse and one for the keyboard, so a slow
    // mouse (a virtual machine) never delays a key press, and the hook thread stays free to answer the
    // low-level hooks in time (Windows drops a hook that answers too slowly). Each worker injects everything it
    // has waiting with ONE call (NtUserSendInput directly when this Windows has it, else SendInput).
    //
    //  - consecutive moves collapse into the latest position, consecutive wheel ticks add up;
    //  - buttons and wheel are ORDERED against keys (Ctrl held while scrolling or clicking): such an operation
    //    waits until every key / button queued before it, on the other thread, has been injected. Plain moves
    //    never wait for anything;
    //  - when injecting is slow, the mouse worker waits a few times as long as one injection took before the
    //    next one, so the system is never fed faster than it can digest: what arrives meanwhile is merged.
    void InjectMoveTo(int x, int y) override { Enqueue(QOp::Move, x, y); }
    void InjectButton(int button, bool down) override { Enqueue(QOp::Button, button, down ? 1 : 0); }
    void InjectWheel(int dx, int dy) override { Enqueue(QOp::Wheel, dx, dy); }
    void InjectKey(int code, bool down) override { Enqueue(QOp::Key, code, down ? 1 : 0); }

    void AddMove(std::vector<INPUT>& v, int x, int y)
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
        v.push_back(in);
    }

    void AddButton(std::vector<INPUT>& v, int button, bool down)
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
        v.push_back(in);
    }

    void AddWheel(std::vector<INPUT>& v, int dx, int dy)
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
        for (UINT i = 0; i < n; ++i) v.push_back(in[i]);
    }

    void AddKey(std::vector<INPUT>& v, int code, bool down)
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
        v.push_back(in);
    }

private:
    // SendInput returns 0 when Windows refuses the input. The usual reason is UIPI: the window that has the
    // focus (Task Manager, an admin console, a UAC prompt) is more privileged than this process. Say so once
    // in a while instead of failing silently.
    void Send(UINT n, INPUT* in)
    {
        const int cb = (int)sizeof(INPUT);
        NtSendFn nt = ntSend_.load();
        UINT sent = nt ? nt(n, in, cb) : SendInput(n, in, cb);
        if (sent == n) { ntBad_ = 0; return; }
        if (sent == 0 && nt) {
            // The direct call can be refused where the documented one is not: retry once through SendInput (nothing
            // was injected, so nothing is doubled) and stop using the direct call if that keeps being the case.
            sent = SendInput(n, in, cb);
            if (sent == n) { if (++ntBad_ >= 3) ntSend_ = nullptr; return; }
        }
        const DWORD e = GetLastError();
        const ULONGLONG now = GetTickCount64();
        ULONGLONG prev = lastSendWarn_.load();
        if (now - prev < 5000 || !sink_.onLog || !lastSendWarn_.compare_exchange_strong(prev, now)) return;
        sink_.onLog("Windows refused injected input (error " + std::to_string(e) + "). " +
                    (IsPrivileged() ? "Desktop: " + Narrow(DeskNameCopy()) + "."
                                    : "This daemon has no administrator rights, so elevated windows (Task Manager, UAC) "
                                      "ignore it: it must be started by the Windows service (restart the window and allow "
                                      "its UAC prompt)."));
    }
    static std::string Narrow(const std::wstring& w)
    {
        std::string o;
        for (wchar_t c : w) o += c < 128 ? (char)c : '?';
        return o;
    }

    // ------------------------------------------------------------ injection queues and workers
    enum class QOp : unsigned char { Move, Button, Wheel, Key };
    struct QItem { QOp op; int a, b; std::uint64_t seq; };     // seq 0: a move (not ordered against anything)

    void Enqueue(QOp op, int a, int b)
    {
        std::lock_guard<std::mutex> lk(qMu_);
        if (workersStop_) return;
        switch (op) {
        case QOp::Move:
            for (auto it = mq_.rbegin(); it != mq_.rend(); ++it) {
                if (it->op == QOp::Button) break;                       // the click must land where it was aimed
                if (it->op == QOp::Move) { it->a = a; it->b = b; return; }   // absolute target: the latest wins
            }
            mq_.push_back({op, a, b, 0});
            break;
        case QOp::Wheel:
            for (auto it = mq_.rbegin(); it != mq_.rend(); ++it) {
                if (it->op == QOp::Button) break;
                if (it->op == QOp::Wheel) {
                    if (it->seq == lastOrdered_) { it->a += a; it->b += b; return; }   // 20 ticks -> one big scroll
                    break;                                              // a key was queued after it: keep the order
                }
            }
            mq_.push_back({op, a, b, ++seq_});
            pendBtn_.push_back(seq_);
            lastOrdered_ = seq_;
            break;
        case QOp::Button:
            mq_.push_back({op, a, b, ++seq_});
            pendBtn_.push_back(seq_);
            lastOrdered_ = seq_;
            mouseUrgent_ = true;                                        // a click never waits for pacing
            break;
        case QOp::Key:
            kq_.push_back({op, a, b, ++seq_});
            pendKey_.push_back(seq_);
            lastOrdered_ = seq_;
            break;
        }
        qCv_.notify_all();
    }

    // qMu_ held. True when everything of the OTHER kind that was queued before 'seq' has been injected.
    bool OrderedReadyLocked(bool keyboard, std::uint64_t seq) const
    {
        const auto& other = keyboard ? pendBtn_ : pendKey_;
        return other.empty() || other.front() > seq || (keyboard ? mouseGone_ : kbdGone_);
    }

    // Injects 'buf' with one call, then reports the ordered operations in it as done.
    void FlushBuf(std::vector<INPUT>& buf, std::vector<std::uint64_t>& done, bool keyboard)
    {
        if (!buf.empty()) { Send((UINT)buf.size(), buf.data()); buf.clear(); }
        if (done.empty()) return;
        std::lock_guard<std::mutex> lk(qMu_);
        auto& pend = keyboard ? pendKey_ : pendBtn_;
        for (std::uint64_t d : done) {
            auto it = std::find(pend.begin(), pend.end(), d);
            if (it != pend.end()) pend.erase(it);
        }
        done.clear();
        qCv_.notify_all();
    }

    // Before an ordered operation: inject what is collected so far (the other thread may be waiting for it), then
    // wait until everything of the other kind queued earlier has been injected. No deadlock is possible: the
    // oldest pending ordered operation overall never has anything to wait for.
    void WaitOrdered(std::vector<INPUT>& buf, std::vector<std::uint64_t>& done, bool keyboard, std::uint64_t seq)
    {
        {
            std::lock_guard<std::mutex> lk(qMu_);
            if (OrderedReadyLocked(keyboard, seq)) return;
        }
        FlushBuf(buf, done, keyboard);
        std::unique_lock<std::mutex> lk(qMu_);
        qCv_.wait(lk, [&] { return OrderedReadyLocked(keyboard, seq); });
    }

    // Worker threads only: injected input goes to the desktop that has the input (UAC prompt, lock screen). The
    // hook thread notices the switch (FollowInputDesktop) and bumps deskGen_; each worker then follows.
    void FollowDesktopWorker(HDESK& mine, std::wstring& name, unsigned& seen)
    {
        seen = deskGen_.load();
        HDESK in = OpenInputDesktop(0, FALSE, GENERIC_ALL);
        if (!in) in = OpenInputDesktop(0, FALSE, DESKTOP_CREATEWINDOW | DESKTOP_HOOKCONTROL | DESKTOP_READOBJECTS |
                                                     DESKTOP_WRITEOBJECTS | DESKTOP_SWITCHDESKTOP);
        if (!in) return;
        const std::wstring n = DesktopName(in);
        if (n.empty() || n == name) { CloseDesktop(in); return; }
        if (!SetThreadDesktop(in)) { CloseDesktop(in); return; }
        if (mine) CloseDesktop(mine);
        mine = in;
        name = n;
    }

    void MouseWorker()
    {
        using Clock = std::chrono::steady_clock;
        HDESK desk = nullptr;
        std::wstring dname;
        unsigned seen = 0;
        FollowDesktopWorker(desk, dname, seen);
        std::vector<INPUT> buf;
        std::vector<std::uint64_t> done;
        Clock::time_point last{};
        std::chrono::microseconds cost{0};              // smoothed time of ONE injected event

        std::unique_lock<std::mutex> lk(qMu_);
        for (;;) {
            qCv_.wait(lk, [this] { return workersStop_ || !mq_.empty(); });
            if (mq_.empty()) break;                     // stopped and nothing left

            // Adaptive flood control: injecting is slow (a VM) -> wait a few times as long as one event took.
            auto gap = cost * 3;
            if (gap < std::chrono::milliseconds(4)) gap = std::chrono::microseconds(0);
            if (gap > std::chrono::milliseconds(40)) gap = std::chrono::milliseconds(40);
            if (gap.count() > 0 && !mouseUrgent_ && !workersStop_)
                qCv_.wait_until(lk, last + gap, [this] { return workersStop_ || mouseUrgent_; });

            std::deque<QItem> batch;
            batch.swap(mq_);
            mouseUrgent_ = false;
            lk.unlock();

            if (deskGen_.load() != seen) FollowDesktopWorker(desk, dname, seen);
            const auto t0 = Clock::now();
            for (const QItem& i : batch) {
                if (i.seq) WaitOrdered(buf, done, false, i.seq);
                switch (i.op) {
                case QOp::Move:   AddMove(buf, i.a, i.b); break;
                case QOp::Button: AddButton(buf, i.a, i.b != 0); done.push_back(i.seq); break;
                case QOp::Wheel:  AddWheel(buf, i.a, i.b); done.push_back(i.seq); break;
                case QOp::Key:    break;
                }
            }
            FlushBuf(buf, done, false);
            last = Clock::now();
            cost = (cost * 3 + std::chrono::duration_cast<std::chrono::microseconds>(last - t0) / (int)batch.size()) / 4;
            lk.lock();
        }
        mouseGone_ = true;
        qCv_.notify_all();
    }

    void KbdWorker()
    {
        HDESK desk = nullptr;
        std::wstring dname;
        unsigned seen = 0;
        FollowDesktopWorker(desk, dname, seen);
        std::vector<INPUT> buf;
        std::vector<std::uint64_t> done;

        std::unique_lock<std::mutex> lk(qMu_);
        for (;;) {
            qCv_.wait(lk, [this] { return workersStop_ || !kq_.empty(); });
            if (kq_.empty()) break;
            std::deque<QItem> batch;
            batch.swap(kq_);
            lk.unlock();

            if (deskGen_.load() != seen) FollowDesktopWorker(desk, dname, seen);
            for (const QItem& i : batch) {
                WaitOrdered(buf, done, true, i.seq);
                AddKey(buf, i.a, i.b != 0);
                done.push_back(i.seq);
            }
            FlushBuf(buf, done, true);
            lk.lock();
        }
        kbdGone_ = true;
        qCv_.notify_all();
    }

    void StartWorkers()
    {
        {
            std::lock_guard<std::mutex> lk(qMu_);
            workersStop_ = false;
            mouseGone_ = kbdGone_ = false;
            mq_.clear(); kq_.clear(); pendBtn_.clear(); pendKey_.clear();
        }
        mouseTh_ = std::thread([this] { MouseWorker(); });
        kbdTh_   = std::thread([this] { KbdWorker(); });
    }

    void StopWorkers()
    {
        {
            std::lock_guard<std::mutex> lk(qMu_);
            workersStop_ = true;
            qCv_.notify_all();
        }
        if (mouseTh_.joinable()) mouseTh_.join();
        if (kbdTh_.joinable())   kbdTh_.join();
    }

    // ------------------------------------------------------------ hook-thread plumbing
    void RunOnHookThread(std::function<void()> fn)
    {
        if (GetCurrentThreadId() == tid_) { fn(); return; }
        auto* task = new std::function<void()>(std::move(fn));
        if (tid_ == 0 || !PostThreadMessageW(tid_, kMsgTask, 0, reinterpret_cast<LPARAM>(task))) {
            (*task)();                      // hook thread is gone (shutting down): do it here
            delete task;
        }
    }

    void InstallHooks()
    {
        if (!mouse_) mouse_ = SetWindowsHookExW(WH_MOUSE_LL, &MouseThunk, GetModuleHandleW(nullptr), 0);
        if (!kbd_)   kbd_   = SetWindowsHookExW(WH_KEYBOARD_LL, &KbdThunk, GetModuleHandleW(nullptr), 0);
    }

    void RemoveHooks()
    {
        if (kbd_)   { UnhookWindowsHookEx(kbd_);   kbd_ = nullptr; }
        if (mouse_) { UnhookWindowsHookEx(mouse_); mouse_ = nullptr; }
    }

    // Hook thread only. Hooks, windows, the cursor and injected input belong to the DESKTOP of the thread that
    // made them. The UAC prompt, Ctrl+Alt+Del and the lock screen are shown on another desktop ("Winlogon"),
    // which a normal user process cannot even open. So: watch which desktop receives input, and when it changes,
    // move this thread (hooks + cursor overlay) there. Needs SYSTEM (the service worker); without it
    // OpenInputDesktop simply fails on the secure desktop and nothing changes.
    bool FollowInputDesktop()
    {
        HDESK in = OpenInputDesktop(0, FALSE, GENERIC_ALL);
        if (!in) in = OpenInputDesktop(0, FALSE, DESKTOP_CREATEWINDOW | DESKTOP_HOOKCONTROL | DESKTOP_READOBJECTS |
                                                     DESKTOP_WRITEOBJECTS | DESKTOP_SWITCHDESKTOP);
        if (!in) return false;              // not allowed, or the desktop is switching right now
        const std::wstring name = DesktopName(in);
        if (name.empty() || name == deskName_ || name == deskFailed_) { CloseDesktop(in); return false; }

        // SetThreadDesktop fails while the thread still owns hooks or windows on the old desktop
        RemoveHooks();
        HideOverlay();
        if (overlay_) { DestroyWindow(overlay_); overlay_ = nullptr; }
        if (!SetThreadDesktop(in)) {
            deskFailed_ = name;             // do not retry every 120 ms
            CloseDesktop(in);
            InstallHooks();
            if (fwd_) ShowOverlay();
            return false;
        }
        if (deskOwned_) CloseDesktop(deskOwned_);
        deskOwned_ = in;
        deskName_ = name;
        deskFailed_.clear();
        { std::lock_guard<std::mutex> g(deskNameMu_); deskNameShared_ = name; }
        ++deskGen_;                         // the injection workers follow (see FollowDesktopWorker)
        InstallHooks();
        if (fwd_) ShowOverlay();
        return true;
    }

    // ------------------------------------------------------------ cursor visibility
    // ShowCursor() only affects the calling thread's windows. The reliable way to hide the
    // pointer everywhere is to replace every system cursor with a fully transparent one and
    // to reload the user's cursor scheme afterwards.
    static void RestoreSystemCursors() { SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0); }

    void HideCursor()
    {
        if (hidden_.exchange(true)) return;
        static const DWORD ids[] = {
            32512 /*arrow*/, 32513 /*I-beam*/, 32514 /*wait*/, 32515 /*cross*/, 32516 /*up arrow*/,
            32642 /*size NW-SE*/, 32643 /*size NE-SW*/, 32644 /*size W-E*/, 32645 /*size N-S*/,
            32646 /*size all*/, 32648 /*no*/, 32649 /*hand*/, 32650 /*app starting*/, 32651 /*help*/,
            32631 /*pen*/, 32671 /*location pin*/, 32672 /*person*/,
        };
        BYTE andMask[32 * 4];
        BYTE xorMask[32 * 4];
        std::memset(andMask, 0xFF, sizeof andMask);     // AND 1 + XOR 0 = transparent
        std::memset(xorMask, 0x00, sizeof xorMask);
        for (DWORD id : ids) {
            // SetSystemCursor takes ownership of (and destroys) the handle: one per id
            HCURSOR c = CreateCursor(GetModuleHandleW(nullptr), 0, 0, 32, 32, andMask, xorMask);
            if (c) SetSystemCursor(c, id);
        }
    }

    void ShowCursorAgain()
    {
        if (!hidden_.exchange(false)) return;
        RestoreSystemCursors();
    }

    // ------------------------------------------------------------ cursor overlay
    // Replacing the system cursors is not enough: many applications (browsers, editors,
    // games, anything with a custom cursor) set their own cursor and keep showing it. So
    // while input is forwarded, an invisible, topmost window covers the whole desktop and
    // answers WM_SETCURSOR with "no cursor". It never takes focus, and the pointer is
    // parked on it, so nothing underneath gets to choose the cursor shape. Hook thread only.
    static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
    {
        switch (m) {
        case WM_SETCURSOR:    SetCursor(nullptr); return TRUE;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        default:              return DefWindowProcW(h, m, wp, lp);
        }
    }

    void ShowOverlay()
    {
        if (!overlay_) {
            WNDCLASSW wc{};
            wc.lpfnWndProc   = &OverlayProc;
            wc.hInstance     = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"SeamlessInputCursorOverlay";
            wc.hCursor       = nullptr;
            RegisterClassW(&wc);            // fails harmlessly when already registered
            overlay_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                                       wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1,
                                       nullptr, nullptr, wc.hInstance, nullptr);
            if (!overlay_) return;
            // alpha 1/255: invisible, but (unlike a colour key) the window still receives the pointer
            SetLayeredWindowAttributes(overlay_, 0, 1, LWA_ALPHA);
        }
        SetWindowPos(overlay_, HWND_TOPMOST,
                     GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                     GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetCursor(nullptr);
        // ShowCursor() is per thread and applies over this thread's windows - and the overlay
        // (ours) covers the whole desktop, so this hides the pointer everywhere as well.
        if (!cursorCounted_) { ShowCursor(FALSE); cursorCounted_ = true; }
        // Other topmost windows (taskbar, menus, full-screen apps) can climb above the overlay
        // and bring the pointer back; re-assert it a few times a second while forwarding.
        overlayTimer_ = SetTimer(nullptr, overlayTimer_, 150, nullptr);
        SetCursorPos(anchorX_, anchorY_);   // makes Windows re-evaluate the cursor under the pointer
    }

    void Reassert()
    {
        if (!overlay_ || !fwd_) return;
        SetWindowPos(overlay_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetCursor(nullptr);
        POINT p{};
        if (GetCursorPos(&p) && (p.x != anchorX_ || p.y != anchorY_))
            SetCursorPos(anchorX_, anchorY_);   // also re-triggers WM_SETCURSOR
    }

    void HideOverlay()
    {
        if (overlayTimer_) { KillTimer(nullptr, overlayTimer_); overlayTimer_ = 0; }
        if (cursorCounted_) { ShowCursor(TRUE); cursorCounted_ = false; }
        if (overlay_) ShowWindow(overlay_, SW_HIDE);
    }

    // ------------------------------------------------------------ hook thread
    void Run()
    {
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // create the message queue
        tid_ = GetCurrentThreadId();
        deskName_ = DesktopName(GetThreadDesktop(tid_));
        FollowInputDesktop();               // start on whichever desktop has the input (not always "Default")
        InstallHooks();
        {
            std::lock_guard<std::mutex> lk(initMu_);
            hookOk_ = mouse_ != nullptr;
            initErr_ = hookOk_ ? 0 : GetLastError();
            initDone_ = true;
        }
        initCv_.notify_all();
        if (!mouse_) return;
        // The keyboard hook stays installed: while input is local it only reports that the owner is typing
        // (owner priority), while forwarding it swallows and forwards the keys.

        // A touchpad stops delivering mouse events the moment the finger rests: if the one event
        // that reached the screen edge was not enough to switch (or the driver reported a point the
        // hook could not use), nothing would ever look at the edge again. Poll it.
        pollTimer_ = SetTimer(nullptr, 0, kPollMs, nullptr);

        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == kMsgKbdOn) {
                if (!kbd_) kbd_ = SetWindowsHookExW(WH_KEYBOARD_LL, &KbdThunk, GetModuleHandleW(nullptr), 0);
                if (fwd_) ShowOverlay();
            } else if (msg.message == kMsgKbdOff) {
                HideOverlay();
            } else if (msg.message == WM_TIMER && msg.hwnd == nullptr) {
                if (msg.wParam == pollTimer_) {
                    if (++deskTick_ % kDeskCheckTicks == 0) FollowInputDesktop();
                    PollEdge();
                } else {
                    Reassert();
                }
            } else if (msg.message == kMsgTask && msg.hwnd == nullptr) {
                auto* task = reinterpret_cast<std::function<void()>*>(msg.lParam);
                if (task) { (*task)(); delete task; }
            } else {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (pollTimer_) { KillTimer(nullptr, pollTimer_); pollTimer_ = 0; }
        RemoveHooks();
        if (overlay_) { DestroyWindow(overlay_); overlay_ = nullptr; }
        if (deskOwned_) { CloseDesktop(deskOwned_); deskOwned_ = nullptr; }
    }

    // Real state of the mouse buttons (also those of a touchpad). The hook's own bookkeeping can get
    // stuck when a button event is swallowed by another hook; the system's state cannot.
    static bool AnyButtonDown()
    {
        for (int vk : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2})
            if (GetAsyncKeyState(vk) & 0x8000) return true;
        return false;
    }

    // True when (x, y) is within 'band' px of an outer edge of the desktop.
    bool NearOuterEdge(int x, int y, int band) const
    {
        if (mons_.empty()) return false;
        static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& d : dirs)
            for (int k = 0; k <= band; ++k)
                if (!inputgeom::InsideAny(mons_, x + d[0] * (k + 1), y + d[1] * (k + 1))) return true;
        return false;
    }

    // Hook thread, ~33 Hz while input is local: if the cursor sits at the edge, tell the engine
    // (it decides whether there is a PC beyond it). Motion events from the hook do the same, this
    // just makes sure a cursor that arrived and stopped is not forgotten.
    void PollEdge()
    {
        if (fwd_ || !sink_.onLocalMotion) return;
        POINT p{};
        if (!GetCursorPos(&p)) return;
        if (!NearOuterEdge(p.x, p.y, 3)) return;
        sink_.onLocalMotion(p.x, p.y, AnyButtonDown());
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
                sink_.onLocalMotion(ms->pt.x, ms->pt.y, mask_ != 0 && AnyButtonDown());
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
        if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (k->dwExtraInfo == kTag) return CallNextHookEx(nullptr, code, wp, lp);
        if (!fwd_) {
            if (!(k->flags & (LLKHF_UP | LLKHF_INJECTED)) && sink_.onPhysicalInput) sink_.onPhysicalInput(100);
            return CallNextHookEx(nullptr, code, wp, lp);
        }

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
    HWND overlay_ = nullptr;             // hook thread only
    bool cursorCounted_ = false;         // hook thread only: ShowCursor(FALSE) is in effect
    UINT_PTR overlayTimer_ = 0;          // hook thread only
    UINT_PTR pollTimer_ = 0;             // hook thread only
    HDESK deskOwned_ = nullptr;          // hook thread only: the input desktop handle this thread is attached to
    std::wstring deskName_, deskFailed_; // hook thread only
    unsigned deskTick_ = 0;              // hook thread only
    std::atomic<ULONGLONG> lastSendWarn_{0};
    using NtSendFn = UINT(WINAPI*)(UINT, LPINPUT, int);
    std::atomic<NtSendFn> ntSend_{nullptr};   // win32u!NtUserSendInput, resolved in Start() (null: use SendInput)
    std::atomic<int> ntBad_{0};
    std::mutex deskNameMu_;
    std::wstring deskNameShared_;             // copy of deskName_ for the log message written by the workers
    std::wstring DeskNameCopy() { std::lock_guard<std::mutex> g(deskNameMu_); return deskNameShared_; }
    std::atomic<unsigned> deskGen_{0};        // bumped by the hook thread when the input desktop changed
    std::vector<InputRect> mons_;        // set before Start(), read on the hook thread
    std::mutex initMu_;
    std::condition_variable initCv_;
    bool initDone_ = false, hookOk_ = false;
    DWORD initErr_ = 0;

    std::mutex qMu_;                     // guards everything below up to workersStop_
    std::condition_variable qCv_;
    std::deque<QItem> mq_, kq_;          // mouse queue (moves, buttons, wheel) and keyboard queue
    std::deque<std::uint64_t> pendBtn_, pendKey_;   // ordered operations not injected yet (button/wheel, key), ascending
    std::uint64_t seq_ = 0, lastOrdered_ = 0;
    bool mouseUrgent_ = false, mouseGone_ = false, kbdGone_ = false, workersStop_ = true;
    std::thread mouseTh_, kbdTh_;

    std::atomic<bool> fwd_{false};
    std::atomic<bool> hidden_{false};
    std::atomic<int> mask_{0};
    int lastPtX_ = 0, lastPtY_ = 0;      // hook thread only
    int anchorX_ = 0, anchorY_ = 0;
    std::bitset<256> held_;
};

} // namespace

std::unique_ptr<InputBackend> MakeWindowsBackend() { return std::make_unique<WindowsBackend>(); }

#endif // _WIN32
