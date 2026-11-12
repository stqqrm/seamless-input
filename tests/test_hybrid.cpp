// Test of the hybrid mouse transport (UDP motion, TCP keys/buttons) with the real input engine, a fake backend
// and a simulated network (delay, jitter, loss, UDP <-> TCP flapping).
// Build and run (Linux):  g++ -std=c++17 -O1 -I src -pthread tests/test_hybrid.cpp src/input/input.cpp -o test_hybrid && ./test_hybrid
// Set V=1 for details about failures.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "input/input.hpp"
#include "input/input_backend.hpp"

using Clock = std::chrono::steady_clock;

struct Fake : InputBackend {
    BackendSink sink;
    std::mutex m;
    std::vector<std::string> log;
    int cx = 0, cy = 0;
    const char* Name() const override { return "fake"; }
    bool Start(BackendSink s, std::string*) override { sink = std::move(s); return true; }
    void Stop() override {}
    bool BeginForwarding(int, int, std::string*) override { return true; }
    void EndForwarding(int, int) override {}
    void Rec(const std::string& s) { std::lock_guard<std::mutex> lk(m); log.push_back(s); }
    void InjectMoveTo(int x, int y) override { cx = x; cy = y; Rec("M " + std::to_string(x) + " " + std::to_string(y)); }
    void InjectButton(int b, bool d) override { Rec("B " + std::to_string(b) + " " + std::to_string(d) + " @" + std::to_string(cx) + "," + std::to_string(cy)); }
    void InjectWheel(int dx, int dy) override { Rec("W " + std::to_string(dx) + " " + std::to_string(dy)); }
    void InjectKey(int c, bool d) override { Rec("K " + std::to_string(c) + " " + std::to_string(d)); }
};
static std::vector<Fake*> g_fakes;
std::unique_ptr<InputBackend> MakeEvdevBackend() { auto f = std::make_unique<Fake>(); g_fakes.push_back(f.get()); return f; }
std::unique_ptr<InputBackend> MakeX11Backend(std::string*) { return MakeEvdevBackend(); }

// A wire with delay, jitter, loss; TCP keeps its order, UDP does not.
struct Wire {
    std::mutex m; std::condition_variable cv; bool stop = false;
    struct It { Clock::time_point due; std::uint64_t n; std::function<void()> fn; bool operator<(const It& o) const { return due != o.due ? due > o.due : n > o.n; } };
    std::vector<It> heap; std::uint64_t n = 0; Clock::time_point lastTcp{};
    std::thread th; std::mt19937 rng{12345};
    double tcpMs = 1, udpMs = 1, jitterMs = 0, loss = 0;
    std::atomic<int> udpSent{0}, udpDropped{0}, tcpLines{0}, tcpMotion{0};
    Wire() { th = std::thread([this] { Run(); }); }
    ~Wire() { { std::lock_guard<std::mutex> lk(m); stop = true; } cv.notify_all(); th.join(); }
    void Push(Clock::time_point due, std::function<void()> fn) {
        std::lock_guard<std::mutex> lk(m); heap.push_back({due, n++, std::move(fn)}); std::push_heap(heap.begin(), heap.end()); cv.notify_all();
    }
    void Tcp(std::function<void()> fn) {
        std::lock_guard<std::mutex> lk(m);
        auto due = std::max(Clock::now() + std::chrono::microseconds((int)(tcpMs * 1000)), lastTcp);
        lastTcp = due; heap.push_back({due, n++, std::move(fn)}); std::push_heap(heap.begin(), heap.end()); cv.notify_all();
    }
    bool Lose() { std::lock_guard<std::mutex> lk(m); return std::uniform_real_distribution<>(0, 1)(rng) < loss; }
    double Jit() { std::lock_guard<std::mutex> lk(m); return jitterMs > 0 ? std::uniform_real_distribution<>(0, jitterMs)(rng) : 0; }
    void Run() {
        std::unique_lock<std::mutex> lk(m);
        while (!stop) {
            if (heap.empty()) { cv.wait(lk); continue; }
            if (heap.front().due > Clock::now()) { cv.wait_until(lk, heap.front().due); continue; }
            std::pop_heap(heap.begin(), heap.end()); auto f = std::move(heap.back().fn); heap.pop_back();
            lk.unlock(); f(); lk.lock();
        }
    }
};

struct Result { bool ok = true; std::string why; };
static void Fail(Result& r, const std::string& w) { if (r.ok) { r.ok = false; r.why = w; } }

// scenario: returns pass/fail
static Result Run(const char* name, double tcpMs, double udpMs, double jitterMs, double loss, bool udpUsable, bool flapUsable)
{
    Result res;
    g_fakes.clear();
    Wire w; w.tcpMs = tcpMs; w.udpMs = udpMs; w.jitterMs = jitterMs; w.loss = loss;
    std::atomic<bool> usable{udpUsable};
    InputEngine *A = nullptr, *B = nullptr;
    auto logA = [](const std::string& s) { if (getenv("V")) std::printf("  [A] %s\n", s.c_str()); };
    auto logB = [](const std::string& s) { if (getenv("V")) std::printf("  [B] %s\n", s.c_str()); };
    InputEngine a([&](int, const std::string& line) {
        if (line[0] == 'M' || line[0] == 'W') w.tcpMotion++;
        w.tcpLines++;
        w.Tcp([&B, line] { B->HandleMessage(1, line); });
    }, logA);
    InputEngine b([&](int, const std::string& line) { w.Tcp([&A, line] { A->HandleMessage(1, line); }); }, logB);
    A = &a; B = &b;
    a.SetUdp([&](int, std::uint64_t seq, const MotionPacket& p) {
                 w.udpSent++;
                 if (w.Lose()) { w.udpDropped++; return true; }
                 w.Push(Clock::now() + std::chrono::microseconds((int)((w.udpMs + w.Jit()) * 1000)), [&B, seq, p] { B->HandleMotion(1, seq, p); });
                 return true;
             },
             [&](int) { return usable.load(); });
    std::vector<InputRect> mon{{0, 0, 1920, 1080}};
    a.SetLocalMonitors(mon); b.SetLocalMonitors(mon);
    std::string e1, e2;
    a.Start(&e1); b.Start(&e2);
    Fake* fa = g_fakes[0]; Fake* fb = g_fakes[1];
    a.SetPeer(1, mon, 1920, 0); a.SetPeerAccepts(1, true);
    b.SetPeer(1, mon, -1920, 0); b.SetPeerAllowed(1, true);

    fa->sink.onLocalMotion(1919, 540, false);        // A's pointer pushes against the right edge: control moves to B
    for (int i = 0; i < 200; ++i) { { std::lock_guard<std::mutex> lk(fb->m); if (!fb->log.empty()) break; } std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    int startX, startY;
    { std::lock_guard<std::mutex> lk(fb->m); if (fb->log.empty()) { Fail(res, "B never became controlled"); return res; }
      std::sscanf(fb->log[0].c_str(), "M %d %d", &startX, &startY); fb->log.clear(); }

    // the script: moves, a click (must land where it was aimed), ctrl+scroll, more moves, a drag-click
    std::mt19937 rng(7);
    long ex = startX, ey = startY;           // where B's pointer must be
    std::vector<std::pair<std::string, std::pair<int,int>>> expectClicks;   // label -> position at click
    auto Move = [&](int dx, int dy) { fa->sink.onForwarded({InputKind::Move, dx, dy}); ex += dx; ey += dy; };
    auto Sleep = [&](int us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); };
    for (int round = 0; round < 6; ++round) {
        for (int i = 0; i < 60; ++i) { Move(1 + (int)(rng() % 3), (int)(rng() % 3)); Sleep(800 + rng() % 600); if (flapUsable && i % 20 == 10) usable = !usable; }
        fa->sink.onForwarded({InputKind::Button, 0, 1});                       // click down right after moving
        expectClicks.push_back({"down", {(int)ex, (int)ey}});
        for (int i = 0; i < 5; ++i) { Move(2, 1); Sleep(700); }                // drag a bit
        fa->sink.onForwarded({InputKind::Button, 0, 0});
        expectClicks.push_back({"up", {(int)ex, (int)ey}});
        Sleep(3000);
        fa->sink.onForwarded({InputKind::Key, 29, 1});                         // Ctrl down
        for (int i = 0; i < 20; ++i) { fa->sink.onForwarded({InputKind::Wheel, 0, 120}); Sleep(500 + rng() % 500); }
        fa->sink.onForwarded({InputKind::Key, 29, 0});                         // Ctrl up
        for (int i = 0; i < 10; ++i) { Move(1, 0); Sleep(700); }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    std::vector<std::string> L; { std::lock_guard<std::mutex> lk(fb->m); L = fb->log; }
    // 1. clicks at the right place
    size_t ci = 0; int cx = startX, cy = startY; long totW = 0;
    bool ctrl = false; int wheelsOutsideCtrl = 0, wheelsInside = 0; int badClick = 0;
    for (auto& s : L) {
        int x, y, c, d, wx, wy;
        if (std::sscanf(s.c_str(), "M %d %d", &x, &y) == 2) { cx = x; cy = y; }
        else if (std::sscanf(s.c_str(), "B %d %d @%d,%d", &c, &d, &x, &y) == 4) {
            if (ci < expectClicks.size()) { auto& e = expectClicks[ci++]; if (x != e.second.first || y != e.second.second) { badClick++; if (getenv("V")) std::printf("  click %s expected %d,%d got %d,%d\\n", e.first.c_str(), e.second.first, e.second.second, x, y); } }
        }
        else if (std::sscanf(s.c_str(), "K %d %d", &c, &d) == 2 && c == 29) ctrl = d;
        else if (std::sscanf(s.c_str(), "W %d %d", &wx, &wy) == 2) { totW += wy; (ctrl ? wheelsInside : wheelsOutsideCtrl)++; }
    }
    if (ci != expectClicks.size()) Fail(res, "clicks seen " + std::to_string(ci) + " of " + std::to_string(expectClicks.size()));
    if (badClick) Fail(res, std::to_string(badClick) + " clicks landed in the wrong place");
    if (wheelsOutsideCtrl) Fail(res, std::to_string(wheelsOutsideCtrl) + " wheel events outside Ctrl-down/up");
    if (totW != 6 * 20 * 120) Fail(res, "wheel total " + std::to_string(totW) + " != " + std::to_string(6 * 20 * 120));
    if (cx != ex || cy != ey) Fail(res, "final pointer " + std::to_string(cx) + "," + std::to_string(cy) + " != " + std::to_string(ex) + "," + std::to_string(ey));
    std::printf("%-44s %s  (udp sent %d dropped %d | tcp lines %d, of which motion %d | wheel events %d)%s%s\n", name, res.ok ? "PASS" : "FAIL",
                w.udpSent.load(), w.udpDropped.load(), w.tcpLines.load(), w.tcpMotion.load(), wheelsInside, res.ok ? "" : "  -> ", res.why.c_str());
    a.Stop(); b.Stop();
    return res;
}

int main()
{
    setenv("SEAMLESS_INPUT_BACKEND", "evdev", 1);
    bool all = true;
    all &= Run("TCP only (UDP unusable)",             1, 1, 0, 0,    false, false).ok;
    all &= Run("UDP, LAN (1 ms, no loss)",            1, 1, 0.3, 0,  true,  false).ok;
    all &= Run("UDP faster than TCP by 8 ms",         9, 1, 0.5, 0,  true,  false).ok;
    all &= Run("UDP slower than TCP by 8 ms",         1, 9, 0.5, 0,  true,  false).ok;
    all &= Run("UDP Wi-Fi: 5% loss, 6 ms jitter",     3, 3, 6, 0.05, true,  false).ok;
    all &= Run("UDP bad Wi-Fi: 20% loss, 10 ms jitter", 3, 3, 10, 0.20, true, false).ok;
    all &= Run("UDP flapping to TCP and back",        2, 2, 1, 0.02, true,  true).ok;
    std::printf("%s\n", all ? "ALL PASS" : "SOME FAILED");
    return all ? 0 : 1;
}
