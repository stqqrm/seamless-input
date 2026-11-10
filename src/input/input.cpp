#include "input/input.hpp"

#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "input/input_backend.hpp"

#if defined(_WIN32)
std::unique_ptr<InputBackend> MakeWindowsBackend();
#elif defined(__linux__)
std::unique_ptr<InputBackend> MakeX11Backend(std::string* why);
std::unique_ptr<InputBackend> MakeEvdevBackend();
#endif

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kKeyScrollLock = 70;      // emergency exit while forwarding
constexpr int kInset         = 4;       // px inside the target monitor where the cursor lands
constexpr int kExitBack      = 24;      // px inside our own monitor after control comes back
constexpr int kMaxGap        = 256;     // largest gap between monitors that is still bridged
constexpr int kEdgeBand      = 2;       // px: a cursor this close to an outer edge counts as "at the edge"
                                        // (touchpads decelerate and may stop 1-2 px short of the edge)
constexpr auto kCooldown     = std::chrono::milliseconds(250);
constexpr auto kRetry        = std::chrono::milliseconds(750);
// PC owner priority: the person sitting at a PC always wins over whoever is remote controlling it.
// After the owner touched their mouse/keyboard nobody may (re)take control of that PC for this long.
constexpr auto kOwnerHold    = std::chrono::milliseconds(3000);

std::unique_ptr<InputBackend> CreateBackend(std::string* why)
{
#if defined(_WIN32)
    (void)why;
    return MakeWindowsBackend();
#elif defined(__linux__)
    const char* force = std::getenv("SEAMLESS_INPUT_BACKEND");
    const char* sess  = std::getenv("XDG_SESSION_TYPE");
    std::string pick;
    if (force && *force)                         pick = force;
    else if (sess && !std::strcmp(sess, "wayland")) pick = "evdev";
    else if (sess && !std::strcmp(sess, "x11"))     pick = "x11";
    else if (std::getenv("WAYLAND_DISPLAY"))        pick = "evdev";
    else if (std::getenv("DISPLAY"))                pick = "x11";
    else                                            pick = "evdev";

    if (pick == "x11") return MakeX11Backend(why);
    return MakeEvdevBackend();
#else
    if (why) *why = "keyboard/mouse sharing is not implemented for this platform";
    return nullptr;
#endif
}

} // namespace

struct InputEngine::Impl {
    struct Peer {
        std::vector<InputRect> mons;   // peer desktop coordinates
        int bx = 0, by = 0;            // top-left of the peer's bounding box
        double ox = 0, oy = 0;
        bool placed = false;
        bool allowed = false;          // it may control this PC
        bool accepts = false;          // it said (CTRL 1) that this PC may control it
        bool warned = false;           // already logged that it does not allow remote control
        Clock::time_point holdUntil{}; // its owner is using it: do not take control before this time
        bool holdLogged = false;       // already logged that we are waiting for it
    };
    struct Crossing { int peer = -1; int x = 0, y = 0; };
    struct Out { int peer = 0; InputEvent ev; std::string raw; bool isRaw = false; };

    Impl(InputEngine* s, SendFn sf, LogFn lf) : self(s), send(std::move(sf)), log(std::move(lf)) {}

    InputEngine* self;
    SendFn send;
    LogFn log;
    std::unique_ptr<InputBackend> be;

    std::mutex mu;
    bool started = false;
    Mode mode = Mode::Local;
    int peerId = -1;

    std::vector<InputRect> mons;
    int lbx = 0, lby = 0;
    std::map<int, Peer> peers;

    int cx = 0, cy = 0;                 // Controlled: where our cursor is
    int vx = 0, vy = 0;                 // Forwarding: where the cursor is on the controlled peer (its desktop coords)
    int physAcc = 0;                    // Controlled: motion of the local (real) mouse in the current window
    Clock::time_point physStart{};
    Clock::time_point ownerLast{};      // the last time the REAL mouse / keyboard of this PC was used
    int exitX = 0, exitY = 0;           // Forwarding: where our cursor goes on a forced stop
    std::set<int> keysDown, btnsDown;   // Controlled: what we injected and must release
    Clock::time_point lastSwitch{};
    std::string lastErr;

    // ---- outgoing queue (keeps hook callbacks fast and coalesces mouse motion)
    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<Out> q;
    bool qstop = false;
    std::thread sender;

    // ----------------------------------------------------------------- sender
    void Push(int peer, const InputEvent& ev)
    {
        std::lock_guard<std::mutex> lk(qmu);
        if (!q.empty()) {
            Out& t = q.back();
            if (!t.isRaw && t.peer == peer && t.ev.kind == ev.kind &&
                (ev.kind == InputKind::Move || ev.kind == InputKind::Wheel)) {
                t.ev.a += ev.a;
                t.ev.b += ev.b;
                return;
            }
        }
        Out o; o.peer = peer; o.ev = ev;
        q.push_back(std::move(o));
        qcv.notify_one();
    }

    void PushRaw(int peer, std::string line)
    {
        std::lock_guard<std::mutex> lk(qmu);
        Out o; o.peer = peer; o.raw = std::move(line); o.isRaw = true;
        q.push_back(std::move(o));
        qcv.notify_one();
    }

    static std::string Format(const Out& o)
    {
        if (o.isRaw) return o.raw;
        char b[64];
        switch (o.ev.kind) {
        case InputKind::Move:   std::snprintf(b, sizeof b, "M %d %d", o.ev.a, o.ev.b); break;
        case InputKind::Button: std::snprintf(b, sizeof b, "B %d %d", o.ev.a, o.ev.b); break;
        case InputKind::Wheel:  std::snprintf(b, sizeof b, "W %d %d", o.ev.a, o.ev.b); break;
        case InputKind::Key:    std::snprintf(b, sizeof b, "K %d %d", o.ev.a, o.ev.b); break;
        }
        return b;
    }

    void SenderLoop()
    {
        std::unique_lock<std::mutex> lk(qmu);
        for (;;) {
            qcv.wait(lk, [this] { return qstop || !q.empty(); });
            if (q.empty()) break;               // qstop and nothing left; queued lines (RELEASE/DENY) are still sent
            std::deque<Out> batch;
            batch.swap(q);
            lk.unlock();
            for (const auto& o : batch) send(o.peer, Format(o));
            // ~500 Hz cap: motion that arrives meanwhile is merged in Push()
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            lk.lock();
        }
    }

    // ----------------------------------------------------------------- layout
    // Core of the edge search, in world coordinates (our top-left = 0,0): the pixel (wx, wy)
    // wants to leave in direction (dx, dy). Finds the monitor beyond that edge among all peers
    // (onlyPeer >= 0 restricts to one, excludePeer skips one) and optionally this PC's own
    // monitors (reported as peer -1). out.x/out.y are in the TARGET's desktop coordinates.
    // forControl: only peers that accept control from this PC are candidates.
    bool FindCrossingWorld(int wx, int wy, int dx, int dy, int onlyPeer, int excludePeer,
                           bool forControl, Crossing& out) const
    {
        const auto now = Clock::now();
        int bestGap = INT_MAX;
        auto consider = [&](int pid, int rx, int ry, int rw, int rh, int toX, int toY) {
            int gap, ex, ey;
            if (dx > 0) {
                if (wy < ry || wy >= ry + rh || rx + rw <= wx + 1) return;
                gap = std::max(0, rx - (wx + 1));
                ex  = std::min(rx + rw - 1, std::max(wx + 1, rx) + kInset);
                ey  = wy;
            } else if (dx < 0) {
                if (wy < ry || wy >= ry + rh || rx > wx - 1) return;
                gap = std::max(0, (wx - 1) - (rx + rw - 1));
                ex  = std::max(rx, std::min(wx - 1, rx + rw - 1) - kInset);
                ey  = wy;
            } else if (dy > 0) {
                if (wx < rx || wx >= rx + rw || ry + rh <= wy + 1) return;
                gap = std::max(0, ry - (wy + 1));
                ey  = std::min(ry + rh - 1, std::max(wy + 1, ry) + kInset);
                ex  = wx;
            } else {
                if (wx < rx || wx >= rx + rw || ry > wy - 1) return;
                gap = std::max(0, (wy - 1) - (ry + rh - 1));
                ey  = std::max(ry, std::min(wy - 1, ry + rh - 1) - kInset);
                ex  = wx;
            }
            if (gap > kMaxGap || gap >= bestGap) return;
            bestGap = gap;
            out.peer = pid;
            out.x = ex + toX;
            out.y = ey + toY;
        };

        for (const auto& kv : peers) {
            const Peer& P = kv.second;
            if (!P.placed || (forControl && !P.accepts) || (onlyPeer >= 0 && kv.first != onlyPeer) ||
                kv.first == excludePeer) continue;
            if (forControl && now < P.holdUntil) continue;      // its owner is using it right now
            const int pox = (int)std::lround(P.ox), poy = (int)std::lround(P.oy);
            for (const auto& m : P.mons)
                consider(kv.first, pox + m.x - P.bx, poy + m.y - P.by, m.w, m.h, P.bx - pox, P.by - poy);
        }
        return bestGap != INT_MAX;
    }

    // The cursor of this PC is at (px, py) and wants to leave in direction (dx, dy).
    // Finds the peer monitor that lies beyond that edge. onlyPeer >= 0 restricts
    // the search to one peer (used for "give control back"). forControl: skip peers that do
    // not let this PC control them.
    bool FindCrossing(int px, int py, int dx, int dy, int onlyPeer, bool forControl, Crossing& out) const
    {
        if (!inputgeom::InsideAny(mons, px, py)) return false;
        if (inputgeom::InsideAny(mons, px + dx, py + dy)) return false;   // not an outer edge
        return FindCrossingWorld(px - lbx, py - lby, dx, dy, onlyPeer, -1, forControl, out);
    }

    void Notify() { if (self->onModeChange) self->onModeChange(mode, peerId); }

    void CursorTo(int id) { if (self->onCursorToPeer) self->onCursorToPeer(id); }

    // Walks from (x, y) towards direction d for at most kEdgeBand pixels. If the outer edge of
    // the desktop is reached, (ex, ey) is the last pixel inside and true is returned.
    bool EdgeAhead(int x, int y, const int d[2], int& ex, int& ey) const
    {
        ex = x;
        ey = y;
        for (int k = 0; k <= kEdgeBand; ++k) {
            if (!inputgeom::InsideAny(mons, ex + d[0], ey + d[1])) return true;
            ex += d[0];
            ey += d[1];
        }
        return false;
    }

    // -------------------------------------------------- local -> peer (we control)
    void OnLocalMotion(int x, int y, bool buttonsDown)
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!started || mode != Mode::Local || buttonsDown || peers.empty()) return;
        const auto now = Clock::now();
        if (now - lastSwitch < kCooldown) return;

        // Some pointing devices (precision touchpads, drivers that inject motion) report a point
        // just outside the desktop or stop a pixel or two before the edge: snap to the desktop and
        // accept the last kEdgeBand pixels as the edge, so a slow finger works like a fast mouse.
        inputgeom::ClampToAny(mons, x, y);

        static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& d : dirs) {
            int ex = x, ey = y;
            if (!EdgeAhead(x, y, d, ex, ey)) continue;
            Crossing c;
            if (!FindCrossing(ex, ey, d[0], d[1], -1, true, c)) continue;
            // from here on the edge pixel is the reference point
            x = ex;
            y = ey;

            int parkX = x, parkY = y;
            inputgeom::MonitorCenter(mons, x, y, parkX, parkY);
            std::string err;
            if (!be->BeginForwarding(parkX, parkY, &err)) {
                lastSwitch = now - kCooldown + kRetry;      // retry in ~750 ms
                if (err != lastErr) { lastErr = err; log("Cannot take over input: " + err); }
                return;
            }
            lastErr.clear();
            exitX = x - d[0] * kExitBack;
            exitY = y - d[1] * kExitBack;
            inputgeom::ClampToAny(mons, exitX, exitY);

            mode = Mode::Forwarding;
            peerId = c.peer;
            lastSwitch = now;
            vx = c.x;
            vy = c.y;
            PushRaw(c.peer, "ENTER " + std::to_string(c.x) + " " + std::to_string(c.y));
            Notify();
            CursorTo(c.peer);
            return;
        }

        // Nothing to cross to. If the cursor is pushing against the edge of a PC that does not allow
        // remote control, say so once (otherwise nothing visible happens at all).
        for (const auto& d : dirs) {
            int ex = x, ey = y;
            if (!EdgeAhead(x, y, d, ex, ey)) continue;
            Crossing c;
            if (!FindCrossing(ex, ey, d[0], d[1], -1, false, c)) continue;
            auto it = peers.find(c.peer);
            if (it == peers.end()) continue;
            if (it->second.accepts) {
                if (now < it->second.holdUntil && !it->second.holdLogged) {
                    it->second.holdLogged = true;
                    log("The owner of the PC at this edge is using it - waiting a few seconds before taking control");
                }
                continue;
            }
            if (it->second.warned) continue;
            it->second.warned = true;
            log("The PC at this edge does not allow remote control (turn on \"Allow remote control\" for this PC there)");
            return;
        }
    }

    void OnForwarded(const InputEvent& ev)
    {
        std::lock_guard<std::mutex> lk(mu);
        if (mode != Mode::Forwarding) return;
        if (ev.kind == InputKind::Key && ev.a == kKeyScrollLock && ev.b == 1) {
            log("Scroll Lock pressed - input returned to this PC");
            StopForwardingLocked(true, -1, -1);
            return;
        }
        Push(peerId, ev);
        if (ev.kind == InputKind::Move) TrackVirtualLocked(ev.a, ev.b);
    }

    // We know the layout of every PC (all in OUR frame), so while we control one PC we follow
    // its cursor ourselves. When it runs off the edge towards a THIRD PC, control hops there
    // directly: PC1 -> PC2 -> PC3 works without PC2 having to know about PC3.
    // (Going back to this PC is still decided by the controlled PC: RETURN.)
    void TrackVirtualLocked(int dx, int dy)
    {
        auto it = peers.find(peerId);
        if (it == peers.end() || !it->second.placed) return;
        const Peer& P = it->second;
        const int nx = vx + dx, ny = vy + dy;
        if (inputgeom::InsideAny(P.mons, nx, ny)) { vx = nx; vy = ny; return; }

        int px = nx, py = ny;
        inputgeom::ClampToAny(P.mons, px, py);
        vx = px;
        vy = py;
        const int sx = nx < px ? -1 : (nx > px ? 1 : 0);
        const int sy = ny < py ? -1 : (ny > py ? 1 : 0);

        const int wx = (int)std::lround(P.ox) + px - P.bx;      // the last pixel inside, in our world
        const int wy = (int)std::lround(P.oy) + py - P.by;
        Crossing c;
        const bool found = (sx && FindCrossingWorld(wx, wy, sx, 0, -1, peerId, true, c)) ||
                           (sy && FindCrossingWorld(wx, wy, 0, sy, -1, peerId, true, c));
        if (!found) return;

        const int from = peerId;
        PushRaw(from, "RELEASE");
        PushRaw(c.peer, "ENTER " + std::to_string(c.x) + " " + std::to_string(c.y));
        peerId = c.peer;
        vx = c.x;
        vy = c.y;
        lastSwitch = Clock::now();
        Notify();
        CursorTo(c.peer);
    }

    // x,y < 0 means "use the position we left from".
    void StopForwardingLocked(bool tellPeer, int x, int y)
    {
        if (mode != Mode::Forwarding) return;
        if (x < 0 || y < 0) { x = exitX; y = exitY; }
        else {
            inputgeom::ClampToAny(mons, x, y);
        }
        be->EndForwarding(x, y);
        if (tellPeer) PushRaw(peerId, "RELEASE");
        mode = Mode::Local;
        lastSwitch = Clock::now();
        peerId = -1;
        Notify();
    }

    // ------------------------------------------------- peer -> local (we are controlled)
    void ReleaseInjectedLocked()
    {
        for (int k : keysDown) be->InjectKey(k, false);
        for (int b : btnsDown) be->InjectButton(b, false);
        keysDown.clear();
        btnsDown.clear();
    }

    void StopControlledLocked(bool tellPeer)
    {
        if (mode != Mode::Controlled) return;
        ReleaseInjectedLocked();
        if (tellPeer) PushRaw(peerId, "DENY");
        mode = Mode::Local;
        lastSwitch = Clock::now();
        peerId = -1;
        Notify();
    }

    // The user of THIS PC touched its own mouse or keyboard. They always win: a peer that is controlling
    // this PC loses control at once (TAKE), and nobody can take it again until kOwnerHold has passed
    // without the owner touching anything (see ENTER in HandleMessage).
    void OnPhysicalInput(int weight)
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!started || mode == Mode::Forwarding) { physAcc = 0; return; }   // forwarding: the owner is at the other PC
        const auto now = Clock::now();
        if (mode == Mode::Controlled && now - lastSwitch < std::chrono::milliseconds(350)) return;   // let the hand-over settle
        if (physAcc == 0 || now - physStart > std::chrono::milliseconds(400)) { physStart = now; physAcc = 0; }
        physAcc += weight;
        if (physAcc < 40) return;                                          // ignore bumps and jitter
        physAcc = 0;
        ownerLast = now;
        if (mode != Mode::Controlled) return;

        ReleaseInjectedLocked();
        PushRaw(peerId, "TAKE");
        log("Local mouse/keyboard used - took control back from the other PC");
        mode = Mode::Local;
        lastSwitch = now;
        peerId = -1;
        Notify();
    }

    void RemoteMoveLocked(int id, int dx, int dy)
    {
        if (mode != Mode::Controlled || id != peerId) return;
        const int nx = cx + dx, ny = cy + dy;
        if (inputgeom::InsideAny(mons, nx, ny)) {
            cx = nx; cy = ny;
            be->InjectMoveTo(cx, cy);
            return;
        }

        int px = nx, py = ny;
        inputgeom::ClampToAny(mons, px, py);
        const int sx = nx < px ? -1 : (nx > px ? 1 : 0);    // direction of the overshoot
        const int sy = ny < py ? -1 : (ny > py ? 1 : 0);

        Crossing c;
        const bool back = (sx && FindCrossing(px, py, sx, 0, peerId, false, c)) ||
                          (sy && FindCrossing(px, py, 0, sy, peerId, false, c));
        if (!back) {
            cx = px; cy = py;
            be->InjectMoveTo(cx, cy);
            return;
        }

        PushRaw(peerId, "RETURN " + std::to_string(c.x) + " " + std::to_string(c.y));
        const int controller = peerId;
        ReleaseInjectedLocked();
        int ix = px - (sx ? sx * kExitBack : 0);
        int iy = py - (sy && !sx ? sy * kExitBack : 0);
        inputgeom::ClampToAny(mons, ix, iy);
        be->InjectMoveTo(ix, iy);
        mode = Mode::Local;
        lastSwitch = Clock::now();
        peerId = -1;
        Notify();
        CursorTo(controller);               // the cursor goes back to the PC that was driving us
    }

    void DispatchLocked(int id, char type, int a, int b)
    {
        if (mode != Mode::Controlled || id != peerId) return;
        switch (type) {
        case 'M': RemoteMoveLocked(id, a, b); break;
        case 'B':
            if (b) btnsDown.insert(a); else btnsDown.erase(a);
            be->InjectButton(a, b != 0);
            break;
        case 'W': be->InjectWheel(a, b); break;
        case 'K':
            if (b) keysDown.insert(a); else keysDown.erase(a);
            be->InjectKey(a, b != 0);
            break;
        }
    }
};

// ---------------------------------------------------------------------------
InputEngine::InputEngine(SendFn send, LogFn log)
    : d_(new Impl(this, std::move(send), std::move(log))) {}

InputEngine::~InputEngine() { Stop(); }

void InputEngine::SetLocalMonitors(const std::vector<InputRect>& mons)
{
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->mons = mons;
    const InputRect b = inputgeom::Bounds(mons);
    d_->lbx = b.x;
    d_->lby = b.y;
}

bool InputEngine::Start(std::string* msg)
{
    if (d_->started) return true;
    std::string why;
    d_->be = CreateBackend(&why);
    if (!d_->be) {
        if (msg) *msg = why.empty() ? "no input backend available" : why;
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(d_->mu);
        d_->be->SetGeometry(d_->mons);
    }

    BackendSink sink;
    sink.onLocalMotion = [this](int x, int y, bool b) { d_->OnLocalMotion(x, y, b); };
    sink.onForwarded   = [this](const InputEvent& e) { d_->OnForwarded(e); };
    sink.onPhysicalInput = [this](int w) { d_->OnPhysicalInput(w); };
    sink.onLog = [this](const std::string& m) { if (d_->log) d_->log(m); };

    std::string err;
    if (!d_->be->Start(std::move(sink), &err)) {
        if (msg) *msg = err.empty() ? "input backend failed to start" : err;
        d_->be.reset();
        return false;
    }
    if (msg) *msg = err;     // warning, if any

    d_->qstop = false;
    d_->sender = std::thread([this] { d_->SenderLoop(); });
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->started = true;
    return true;
}

void InputEngine::Stop()
{
    if (!d_->be) return;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        d_->started = false;
        d_->StopForwardingLocked(true, -1, -1);
        d_->StopControlledLocked(true);
    }
    d_->be->Stop();                 // joins the backend thread: no more callbacks after this
    {
        std::lock_guard<std::mutex> lk(d_->qmu);
        d_->qstop = true;
    }
    d_->qcv.notify_all();
    if (d_->sender.joinable()) d_->sender.join();
    d_->be.reset();
}

bool InputEngine::CanInject() const
{
    return d_->be && d_->be->CanInject();
}

std::string InputEngine::BackendName() const
{
    return d_->be ? d_->be->Name() : std::string("none");
}

void InputEngine::SetPeer(int id, const std::vector<InputRect>& mons, double ox, double oy)
{
    std::lock_guard<std::mutex> lk(d_->mu);
    auto& p = d_->peers[id];
    p.mons = mons;
    const InputRect b = inputgeom::Bounds(mons);
    p.bx = b.x;
    p.by = b.y;
    p.ox = ox;
    p.oy = oy;
    p.placed = !mons.empty();
}

void InputEngine::SetPeerOffset(int id, double ox, double oy)
{
    std::lock_guard<std::mutex> lk(d_->mu);
    auto it = d_->peers.find(id);
    if (it == d_->peers.end()) return;
    it->second.ox = ox;
    it->second.oy = oy;
}

void InputEngine::SetPeerAllowed(int id, bool allowed)
{
    std::lock_guard<std::mutex> lk(d_->mu);
    auto it = d_->peers.find(id);
    if (it == d_->peers.end()) return;
    it->second.allowed = allowed;
    if (!allowed && d_->be && d_->mode == Mode::Controlled && d_->peerId == id)
        d_->StopControlledLocked(true);
}

void InputEngine::SetPeerAccepts(int id, bool accepts)
{
    std::lock_guard<std::mutex> lk(d_->mu);
    auto it = d_->peers.find(id);
    if (it == d_->peers.end()) return;
    it->second.accepts = accepts;
    it->second.warned = false;
    // It withdrew its permission while we are driving it: hand the keyboard/mouse back at once.
    if (!accepts && d_->be && d_->mode == Mode::Forwarding && d_->peerId == id)
        d_->StopForwardingLocked(false, -1, -1);
}

void InputEngine::RemovePeer(int id)
{
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->peers.erase(id);
    if (!d_->be || d_->peerId != id) return;
    if (d_->mode == Mode::Forwarding)      d_->StopForwardingLocked(false, -1, -1);
    else if (d_->mode == Mode::Controlled) d_->StopControlledLocked(false);
}

bool InputEngine::HandleMessage(int id, const std::string& line)
{
    if (line.empty()) return false;
    const char* s = line.c_str();
    int a = 0, b = 0;

    // ---- hot path: events from the controller
    if ((s[0] == 'M' || s[0] == 'B' || s[0] == 'W' || s[0] == 'K') && s[1] == ' ') {
        if (std::sscanf(s + 2, "%d %d", &a, &b) != 2) return true;
        std::lock_guard<std::mutex> lk(d_->mu);
        if (d_->started) d_->DispatchLocked(id, s[0], a, b);
        return true;
    }

    if (line.rfind("ENTER ", 0) == 0) {
        if (std::sscanf(s + 6, "%d %d", &a, &b) != 2) return true;
        std::lock_guard<std::mutex> lk(d_->mu);
        auto it = d_->peers.find(id);
        // Who may control this PC is decided per device by 'allowed'; nobody is asked.
        // A PC that cannot inject input must never accept control: the controller would capture its own
        // keyboard and mouse, and nothing would happen here.
        const bool ok = d_->started && d_->be && d_->be->CanInject() && it != d_->peers.end() &&
                        it->second.placed && it->second.allowed && d_->mode == Mode::Local;
        if (ok && d_->ownerLast != Clock::time_point{} && Clock::now() - d_->ownerLast < kOwnerHold) {
            // The owner is using this PC: not a refusal, just "later". TAKE makes the controller give the
            // keyboard/mouse back and wait; it will try again by itself when the owner has been idle.
            d_->PushRaw(id, "TAKE");
            return true;
        }
        if (!ok) {
            d_->PushRaw(id, "DENY");
            return true;
        }
        d_->cx = a;
        d_->cy = b;
        inputgeom::ClampToAny(d_->mons, d_->cx, d_->cy);
        d_->mode = Mode::Controlled;
        d_->peerId = id;
        d_->lastSwitch = Clock::now();
        d_->be->InjectMoveTo(d_->cx, d_->cy);
        d_->Notify();
        return true;
    }

    if (line.rfind("RETURN ", 0) == 0) {
        if (std::sscanf(s + 7, "%d %d", &a, &b) != 2) return true;
        std::lock_guard<std::mutex> lk(d_->mu);
        if (d_->mode == Mode::Forwarding && d_->peerId == id) d_->StopForwardingLocked(false, a, b);
        return true;
    }

    if (line == "TAKE") {      // the controlled PC's owner used their mouse/keyboard: back to this PC, then wait
        std::lock_guard<std::mutex> lk(d_->mu);
        auto it = d_->peers.find(id);
        if (it != d_->peers.end()) { it->second.holdUntil = Clock::now() + kOwnerHold; it->second.holdLogged = false; }
        if (d_->mode == Mode::Forwarding && d_->peerId == id) {
            d_->log("The owner of the other PC is using it - control returned, waiting a few seconds");
            d_->StopForwardingLocked(false, -1, -1);
        }
        return true;
    }

    if (line == "RELEASE") {
        std::lock_guard<std::mutex> lk(d_->mu);
        if (d_->mode == Mode::Controlled && d_->peerId == id) d_->StopControlledLocked(false);
        return true;
    }

    if (line == "DENY") {
        std::lock_guard<std::mutex> lk(d_->mu);
        auto it = d_->peers.find(id);
        if (it != d_->peers.end()) { it->second.accepts = false; it->second.warned = true; }   // no retry until CTRL 1
        if (d_->mode == Mode::Forwarding && d_->peerId == id) {
            d_->log("The other PC does not allow remote control (enable \"Allow remote control\" for this PC there)");
            d_->StopForwardingLocked(false, -1, -1);
        }
        return true;
    }

    return false;
}
