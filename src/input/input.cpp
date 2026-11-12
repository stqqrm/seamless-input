#include "input/input.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

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
// Mouse motion and wheel ticks are not sent one by one: everything that arrives within one tick is merged into
// ONE "M dx dy" and ONE "W dx dy" line (sum of the deltas), so a burst of 20 wheel ticks becomes one big scroll.
// The tick is set PER PEER (Devices tab, updates per second). The first event after a pause goes out at once.
// Each peer has its own queue and its own clock: a slow device never holds back another one, and nothing needs
// to be synchronised between them (a peer only acts on what it has received).
constexpr int kDefaultRateHz = 250;
constexpr int kMinRateHz     = 10;
constexpr int kMaxRateHz     = 1000;
constexpr size_t kMaxQueue   = 256;     // per peer; when exceeded the OLDEST motion/wheel entries are dropped first
constexpr auto kRxWait       = std::chrono::milliseconds(250);  // longest a datagram waits for the key/button events before it (TCP cannot lose them, only delay)

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
    struct Out {
        int peer = 0;
        InputEvent ev;
        std::string raw;
        bool isRaw = false;
        // hybrid transport
        bool isFlush = false;               // not a line: a UDP datagram with these running totals
        std::uint64_t seq = 0;
        MotionPacket snap;
        bool sync = false;                  // key/button line that carries its place in the UDP stream
        unsigned epoch = 0, k = 0;
        unsigned long long need = 0;        // sequence number of the newest datagram whose totals are included below
        std::array<std::int32_t, 4> tot{};  // the exact motion totals at the moment of the event
    };

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

    // ---- hybrid transport, receiving side (mu held). Pointer motion arrives as UDP datagrams with running
    // totals, keys and buttons as TCP lines. A key/button line carries the exact totals of the moment it happened:
    // before it is applied the pointer is brought to exactly there, so a click lands where it was aimed even if
    // datagrams were lost or are late (those are then ignored, their totals are older). The other way round a
    // datagram says how many key/button events must be applied before it ('tcpCount'): motion that happened
    // after Ctrl went down is not applied before Ctrl. Waiting for TCP is safe: TCP loses nothing, it only delays.
    struct RxPkt { std::uint64_t seq; MotionPacket p; Clock::time_point t; };
    struct Rx {
        bool have = false;                  // an EPOCH line has been seen
        std::uint16_t epoch = 0;
        std::uint32_t tcpApplied = 0;       // key/button events of this epoch applied so far
        std::uint64_t udpApplied = 0;       // newest datagram sequence number whose totals are applied
        std::array<std::int32_t, 4> base{}; // the totals applied so far
        std::deque<RxPkt> pend;             // datagrams not applied yet (waiting for key/button events), by sequence number
    };
    std::map<int, Rx> rx;
    std::condition_variable rxcv;
    bool rxStop = false;
    std::thread rxThread;

    // Moves the pointer / wheel by what the totals in 'p' add to 'base' (and brings 'base' up to date).
    void ApplyTotalsLocked(int id, std::array<std::int32_t, 4>& base, const std::int32_t cur[4])
    {
        std::int32_t d[4];
        for (int i = 0; i < 4; ++i) {
            d[i] = (std::int32_t)((std::uint32_t)cur[i] - (std::uint32_t)base[i]);
            base[i] = cur[i];
        }
        if (d[0] || d[1]) DispatchLocked(id, 'M', d[0], d[1]);
        if (d[2] || d[3]) DispatchLocked(id, 'W', d[2], d[3]);
    }

    // Exact totals from a TCP line (key/button, EPOCH): newer than anything applied from datagrams, so apply them
    // and forget the datagrams they make obsolete.
    void SyncTotalsLocked(int id, Rx& r, std::uint64_t need, const std::int32_t cur[4])
    {
        if (need <= r.udpApplied) return;           // the datagrams already brought it there
        ApplyTotalsLocked(id, r.base, cur);
        r.udpApplied = need;
        while (!r.pend.empty() && r.pend.front().seq <= need) r.pend.pop_front();
    }

    void RxPumpLocked(int id, Rx& r)
    {
        const auto now = Clock::now();
        while (!r.pend.empty()) {
            const RxPkt& k = r.pend.front();
            if (k.p.tcpCount > r.tcpApplied && now - k.t <= kRxWait) break;
            const std::int32_t cur[4] = {k.p.mx, k.p.my, k.p.wx, k.p.wy};
            r.udpApplied = k.seq;
            ApplyTotalsLocked(id, r.base, cur);
            r.pend.pop_front();
        }
    }

    void RxLoop()
    {
        std::unique_lock<std::mutex> lk(mu);
        while (!rxStop) {
            bool any = false;
            for (auto& kv : rx) if (!kv.second.pend.empty()) any = true;
            if (any) rxcv.wait_for(lk, std::chrono::milliseconds(20));
            else     rxcv.wait(lk);
            if (rxStop) break;
            for (auto& kv : rx) RxPumpLocked(kv.first, kv.second);
        }
    }

    // ---- outgoing queues, one per peer (keeps hook callbacks fast and coalesces mouse motion)
    struct PeerQ {
        std::deque<Out> q;
        Clock::time_point next{};       // earliest time the next paced batch may go out
        bool urgent = false;            // a click / key / control line is queued: do not wait for the tick
        int hz = kDefaultRateHz;
        // ---- hybrid transport. While 'udp' is set, pointer motion and wheel are not queued: they add up in
        // 'tot' and the sender thread sends the totals as a UDP datagram at every tick. Keys and buttons stay in
        // the queue (TCP); a datagram with the totals up to that moment is put in front of each of them, and the
        // line says which datagram it follows, so a click lands where it was aimed (see MotionPacket).
        bool udp = false;
        std::uint16_t epoch = 0;                    // restarts the totals: a new control session, or a switch of transport
        std::array<std::int32_t, 4> tot{}, sentTot{};   // running totals: pointer x y, wheel x y
        std::uint32_t ordCount = 0;                 // key/button lines queued in this epoch
        std::uint64_t seq = 0;                      // last datagram sequence number (grows for the whole connection)
        std::uint64_t epochFirstSeq = 1;
        int repeatLeft = 0;                         // copies of the last totals still to send once the motion has stopped
        Clock::time_point repeatAt{};
    };
    std::mutex qmu;
    std::condition_variable qcv;
    std::map<int, PeerQ> pq;
    bool qstop = false;
    std::thread sender;
    UdpSendFn udpSend;                  // set before Start()
    UdpUsableFn udpUsable;

    static void AddWrap(std::int32_t& t, int d) { t = (std::int32_t)((std::uint32_t)t + (std::uint32_t)d); }

    // qmu held. A datagram with the totals as they are now. UDP can lose it, and when the motion stops nothing
    // would ever make up for that: so after the last one, the same totals are sent again shortly after
    // (twice, see the sender loop). A datagram that adds nothing is harmless to the receiver.
    Out MakeFlushLocked(int peer, PeerQ& p, bool repeat = false)
    {
        if (repeat) --p.repeatLeft;
        else        p.repeatLeft = 2;
        p.repeatAt = Clock::now() + std::chrono::milliseconds(repeat ? 60 : 25);
        Out f;
        f.peer = peer;
        f.isFlush = true;
        f.seq = ++p.seq;
        f.snap.epoch = p.epoch;
        f.snap.tcpCount = p.ordCount;
        f.snap.mx = p.tot[0]; f.snap.my = p.tot[1]; f.snap.wx = p.tot[2]; f.snap.wy = p.tot[3];
        p.sentTot = p.tot;
        return f;
    }

    // qmu held. Starts a new epoch (totals from zero) and tells the peer, on TCP, in order with everything else.
    // 'carry': the old epoch used UDP, so the line also carries its final totals: whatever datagram was lost or
    // is still on its way, the peer ends up exactly where we are (a switch from UDP to TCP).
    void NewEpochLocked(int peer, PeerQ& p, bool carry)
    {
        std::string line;
        if (carry && p.udp) {
            line = " " + std::to_string(p.tot[0]) + " " + std::to_string(p.tot[1]) + " " + std::to_string(p.tot[2]) + " " +
                   std::to_string(p.tot[3]) + " " + std::to_string((unsigned long long)++p.seq);
        }
        p.udp = udpSend && udpUsable && udpUsable(peer);
        ++p.epoch;
        p.tot.fill(0);
        p.sentTot.fill(0);
        p.ordCount = 0;
        p.epochFirstSeq = p.seq + 1;
        p.repeatLeft = 0;
        Out o; o.peer = peer; o.isRaw = true; o.raw = "EPOCH " + std::to_string((unsigned)p.epoch) + line;
        p.q.push_back(std::move(o));
        p.urgent = true;
    }

    // ----------------------------------------------------------------- sender
    void Push(int peer, const InputEvent& ev)
    {
        std::lock_guard<std::mutex> lk(qmu);
        PeerQ& p = pq[peer];
        if ((udpSend && udpUsable && udpUsable(peer)) != p.udp) NewEpochLocked(peer, p, true);
        const bool motion = ev.kind == InputKind::Move || ev.kind == InputKind::Wheel;
        Out o; o.peer = peer; o.ev = ev;
        if (motion && p.udp) {
            if (ev.kind == InputKind::Move) { AddWrap(p.tot[0], ev.a); AddWrap(p.tot[1], ev.b); }
            else                            { AddWrap(p.tot[2], ev.a); AddWrap(p.tot[3], ev.b); }
            qcv.notify_one();               // the sender thread sends the totals at the next tick
            return;
        }
        if (motion) {
            // Merge into the newest queued entry of the same kind, as long as nothing order-sensitive (a button,
            // a key, a control line) was queued after it. Moves and wheel ticks may pass each other: within
            // one tick that makes no difference.
            for (auto it = p.q.rbegin(); it != p.q.rend(); ++it) {
                if (it->isRaw || it->isFlush) break;
                if (it->ev.kind == ev.kind) { it->ev.a += ev.a; it->ev.b += ev.b; return; }
                if (it->ev.kind != InputKind::Move && it->ev.kind != InputKind::Wheel) break;
            }
        } else {
            p.urgent = true;                // click / key: goes out now, after the motion queued before it
            if (p.udp) {                    // its place in the UDP stream, and the exact totals at this moment
                o.sync = true;
                o.epoch = p.epoch;
                o.k = ++p.ordCount;
                o.need = ++p.seq;           // a stamp in the datagram sequence: older datagrams carry older totals
                o.tot = p.tot;
            }
        }
        p.q.push_back(std::move(o));
        TrimLocked(p);
        qcv.notify_one();
    }

    void PushRaw(int peer, std::string line)
    {
        std::lock_guard<std::mutex> lk(qmu);
        PeerQ& p = pq[peer];
        const bool enter = line.compare(0, 6, "ENTER ") == 0;
        Out o; o.peer = peer; o.raw = std::move(line); o.isRaw = true;
        p.q.push_back(std::move(o));
        p.urgent = true;
        if (enter) NewEpochLocked(peer, p, false);      // every control session starts a fresh set of totals
        TrimLocked(p);
        qcv.notify_one();
    }

    // Bounded queue: only ever drops pointer motion / wheel (the newest state matters), never keys or buttons.
    static void TrimLocked(PeerQ& p)
    {
        while (p.q.size() > kMaxQueue) {
            auto it = std::find_if(p.q.begin(), p.q.end(), [](const Out& o) {
                return !o.isRaw && !o.isFlush && (o.ev.kind == InputKind::Move || o.ev.kind == InputKind::Wheel);
            });
            if (it == p.q.end()) break;
            p.q.erase(it);
        }
    }

    static std::string Format(const Out& o)
    {
        if (o.isRaw) return o.raw;
        char b[160];
        switch (o.ev.kind) {
        case InputKind::Move:   std::snprintf(b, sizeof b, "M %d %d", o.ev.a, o.ev.b); break;
        case InputKind::Wheel:  std::snprintf(b, sizeof b, "W %d %d", o.ev.a, o.ev.b); break;
        case InputKind::Button:
        case InputKind::Key: {
            const char c = o.ev.kind == InputKind::Button ? 'B' : 'K';
            if (o.sync) std::snprintf(b, sizeof b, "%c %d %d %u %u %llu %d %d %d %d", c, o.ev.a, o.ev.b, o.epoch, o.k, o.need,
                                      o.tot[0], o.tot[1], o.tot[2], o.tot[3]);
            else        std::snprintf(b, sizeof b, "%c %d %d", c, o.ev.a, o.ev.b);
            break;
        }
        }
        return b;
    }

    void SenderLoop()
    {
        std::unique_lock<std::mutex> lk(qmu);
        for (;;) {
            std::vector<std::pair<int, std::deque<Out>>> ready;
            auto wake = Clock::time_point::max();
            bool any = false;
            const auto now = Clock::now();
            for (auto& [id, p] : pq) {
                const bool dirty = p.udp && p.tot != p.sentTot;     // motion waiting to go out as a datagram
                const bool repeating = p.udp && !dirty && p.repeatLeft > 0;
                if (p.q.empty() && !dirty && !repeating) continue;
                any = true;
                if (p.q.empty() && !dirty && now < p.repeatAt) {    // only a repeat is pending, and not yet
                    wake = std::min(wake, p.repeatAt);
                    continue;
                }
                if (qstop || p.urgent || now >= p.next) {           // on stop everything left (RELEASE / DENY) goes out
                    ready.emplace_back(id, std::deque<Out>());
                    ready.back().second.swap(p.q);
                    if (p.udp && p.tot != p.sentTot) ready.back().second.push_back(MakeFlushLocked(id, p));
                    else if (repeating && now >= p.repeatAt) ready.back().second.push_back(MakeFlushLocked(id, p, true));
                    p.urgent = false;
                    p.next = now + std::chrono::microseconds(1000000 / std::clamp(p.hz, kMinRateHz, kMaxRateHz));
                } else {
                    wake = std::min(wake, p.next);                  // motion that arrives until then is merged
                }
            }
            if (!ready.empty()) {
                lk.unlock();
                for (auto& [id, batch] : ready)
                    for (const auto& o : batch) {
                        if (o.isFlush) { if (udpSend) udpSend(id, o.seq, o.snap); }
                        else send(id, Format(o));
                    }
                lk.lock();
                continue;
            }
            if (!any) {
                if (qstop) break;
                qcv.wait(lk);
            } else {
                qcv.wait_until(lk, wake);
            }
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
    d_->rxStop = false;
    d_->rxThread = std::thread([this] { d_->RxLoop(); });
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
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        d_->rxStop = true;
    }
    d_->rxcv.notify_all();
    if (d_->rxThread.joinable()) d_->rxThread.join();
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

void InputEngine::SetUdp(UdpSendFn send, UdpUsableFn usable)
{
    std::lock_guard<std::mutex> lk(d_->qmu);
    d_->udpSend = std::move(send);
    d_->udpUsable = std::move(usable);
}

void InputEngine::HandleMotion(int id, std::uint64_t seq, const MotionPacket& m)
{
    std::lock_guard<std::mutex> lk(d_->mu);
    if (!d_->started || d_->mode != Mode::Controlled || d_->peerId != id) return;
    auto it = d_->rx.find(id);
    if (it == d_->rx.end() || !it->second.have) return;     // before its EPOCH line a datagram means nothing
    auto& r = it->second;
    if (m.epoch != r.epoch) return;                          // an epoch that is over: its totals were handed over by TCP
    if (seq <= r.udpApplied) return;                         // applied already (or older than what was)
    auto pos = std::find_if(r.pend.begin(), r.pend.end(), [seq](const Impl::RxPkt& k) { return k.seq >= seq; });
    if (pos != r.pend.end() && pos->seq == seq) return;      // duplicate
    r.pend.insert(pos, Impl::RxPkt{seq, m, Clock::now()});   // datagrams may overtake each other: keep them in order
    d_->RxPumpLocked(id, r);
    if (!r.pend.empty()) d_->rxcv.notify_all();
}

void InputEngine::SetPeerRate(int id, int hz)
{
    std::lock_guard<std::mutex> lk(d_->qmu);
    d_->pq[id].hz = std::clamp(hz, kMinRateHz, kMaxRateHz);
}

void InputEngine::RemovePeer(int id)
{
    {
        std::lock_guard<std::mutex> qlk(d_->qmu);
        d_->pq.erase(id);               // nothing can be sent to it any more
    }
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->peers.erase(id);
    d_->rx.erase(id);
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
        unsigned ep = 0, k = 0;
        unsigned long long need = 0;
        std::int32_t tot[4] = {0, 0, 0, 0};
        const int n = std::sscanf(s + 2, "%d %d %u %u %llu %d %d %d %d", &a, &b, &ep, &k, &need, &tot[0], &tot[1], &tot[2], &tot[3]);
        if (n < 2) return true;
        std::lock_guard<std::mutex> lk(d_->mu);
        if (!d_->started) return true;
        if ((s[0] == 'B' || s[0] == 'K') && n == 9) {
            // With UDP motion the line says where the pointer is at this moment: bring it there first.
            auto it = d_->rx.find(id);
            if (it != d_->rx.end() && it->second.have && it->second.epoch == (std::uint16_t)ep) {
                auto& r = it->second;
                d_->SyncTotalsLocked(id, r, need, tot);
                d_->DispatchLocked(id, s[0], a, b);
                if (k > r.tcpApplied) r.tcpApplied = k;
                d_->RxPumpLocked(id, r);            // datagrams that were waiting for this event
                if (!r.pend.empty()) d_->rxcv.notify_all();
                return true;
            }
        }
        d_->DispatchLocked(id, s[0], a, b);
        return true;
    }

    if (line.rfind("EPOCH ", 0) == 0) {         // totals restart: a new control session, or a switch UDP <-> TCP
        unsigned e = 0;
        unsigned long long need = 0;
        std::int32_t tot[4] = {0, 0, 0, 0};
        const int n = std::sscanf(s + 6, "%u %d %d %d %d %llu", &e, &tot[0], &tot[1], &tot[2], &tot[3], &need);
        if (n < 1) return true;
        std::lock_guard<std::mutex> lk(d_->mu);
        auto& r = d_->rx[id];
        if (r.have && n == 6) d_->SyncTotalsLocked(id, r, need, tot);    // the old epoch's final totals
        r = Impl::Rx{};
        r.have = true;
        r.epoch = (std::uint16_t)e;
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
        d_->rx.erase(id);                   // UDP state of an earlier session; the EPOCH line that follows starts a new one
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
