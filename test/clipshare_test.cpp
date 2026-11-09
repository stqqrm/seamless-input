// Loopback test of ClipShare: several "PCs", each with its own Core thread, connected by queues.
#include "../src/clipshare.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; std::printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct Node {
    std::string name;
    std::unique_ptr<ClipShare> cs;
    std::mutex qm; std::condition_variable qcv; std::deque<std::function<void()>> q; bool quit = false; std::thread th;
    std::mutex cm; std::vector<fs::path> clipFiles; std::string clipText;          // the "system clipboard"
    std::mutex lm; std::vector<std::string> logs;
    std::function<void(int, const std::string&)> route;

    void Post(std::function<void()> f) { { std::lock_guard<std::mutex> l(qm); q.push_back(std::move(f)); } qcv.notify_one(); }
    void Start(const fs::path& incoming) {
        ClipShare::Hooks h;
        h.send = [this](int p, const std::string& l) { route(p, l); };
        h.log = [this](const std::string& m) { std::lock_guard<std::mutex> l(lm); logs.push_back(m); std::printf("   [%s] %s\n", name.c_str(), m.c_str()); };
        h.getFiles = [this](std::vector<fs::path>& f) { std::lock_guard<std::mutex> l(cm); f = clipFiles; return !f.empty(); };
        h.getText = [this](std::string& t, std::size_t m) { std::lock_guard<std::mutex> l(cm); if (clipText.empty() || clipText.size() > m) return false; t = clipText; return true; };
        h.setFiles = [this](const std::vector<fs::path>& f) { std::lock_guard<std::mutex> l(cm); clipFiles = f; clipText.clear(); return true; };
        h.setText = [this](const std::string& t) { std::lock_guard<std::mutex> l(cm); clipText = t; clipFiles.clear(); return true; };
        cs = std::make_unique<ClipShare>(incoming, h);
        th = std::thread([this] {
            for (;;) {
                std::function<void()> f;
                { std::unique_lock<std::mutex> l(qm); qcv.wait(l, [&] { return quit || !q.empty(); }); if (q.empty()) return; f = std::move(q.front()); q.pop_front(); }
                f();
            }
        });
    }
    void Stop() { { std::lock_guard<std::mutex> l(qm); quit = true; } qcv.notify_all(); if (th.joinable()) th.join(); cs->Shutdown(); }
    bool Saw(const std::string& s) { std::lock_guard<std::mutex> l(lm); for (auto& x : logs) if (x.find(s) != std::string::npos) return true; return false; }
    int Count(const std::string& s) { std::lock_guard<std::mutex> l(lm); int n = 0; for (auto& x : logs) if (x.find(s) != std::string::npos) ++n; return n; }
    void Clear() { std::lock_guard<std::mutex> l(lm); logs.clear(); }
    std::vector<fs::path> Clip() { std::lock_guard<std::mutex> l(cm); return clipFiles; }
};

static bool WaitFor(const std::function<bool()>& c, int ms = 20000) {
    for (int i = 0; i < ms / 10; ++i) { if (c()) return true; std::this_thread::sleep_for(10ms); }
    return c();
}

static std::string Slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); }
static void Write(const fs::path& p, const std::string& d) { fs::create_directories(p.parent_path()); std::ofstream f(p, std::ios::binary); f << d; }

// expected tree of 'src' (links to folders and broken links are not sent) vs what arrived in 'dst'
static void Compare(const fs::path& src, const fs::path& dst, int& files, int& dirs) {
    CHECK(fs::is_directory(dst)); ++dirs;
    std::map<std::string, fs::path> want;
    for (auto& e : fs::directory_iterator(src)) {
        std::error_code ec;
        if (e.is_symlink()) { auto st = fs::status(e.path(), ec); if (ec || !fs::is_regular_file(st)) continue; }
        want[e.path().filename().string()] = e.path();
    }
    std::map<std::string, fs::path> have;
    for (auto& e : fs::directory_iterator(dst)) have[e.path().filename().string()] = e.path();
    CHECK(want.size() == have.size());
    // names may be changed (invalid UTF-8, duplicates differing in case): compare in sorted order of content
    std::vector<fs::path> w, h;
    for (auto& kv : want) w.push_back(kv.second);
    for (auto& kv : have) h.push_back(kv.second);
    auto key = [](const fs::path& p) { std::error_code ec; return fs::is_directory(p, ec) ? std::string("d") : "f" + std::to_string(fs::file_size(p, ec)); };
    std::sort(w.begin(), w.end(), [&](auto& a, auto& b) { return key(a) + a.filename().string() < key(b) + b.filename().string(); });
    std::sort(h.begin(), h.end(), [&](auto& a, auto& b) { return key(a) + a.filename().string() < key(b) + b.filename().string(); });
    for (std::size_t i = 0; i < std::min(w.size(), h.size()); ++i) {
        if (fs::is_directory(w[i])) Compare(w[i], h[i], files, dirs);
        else { CHECK(Slurp(w[i]) == Slurp(h[i])); ++files; }
    }
}

int main() {
    const fs::path root = fs::temp_directory_path() / ("clip_test_" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    std::mt19937 rng(1);
    auto blob = [&](std::size_t n) { std::string s(n, '\0'); for (auto& c : s) c = (char)rng(); return s; };

    // ---------------------------------------------------------------- source tree
    const fs::path src = root / "send" / "My Project";
    Write(src / "a.txt", "hello");
    Write(src / "empty.bin", "");
    Write(src / "big.bin", blob(5 * 1024 * 1024 + 17));
    Write(src / "sub" / "deep" / "deeper" / "x.dat", blob(70000));
    Write(src / "sub" / "ünïcode ✓ name.txt", "unicode");
    Write(src / "sub" / "Name.txt", "upper");
    Write(src / "sub" / "name.txt", "lower");                       // differs only in case
    Write(src / "sub" / "caf\xe9.txt", "latin1 name, not valid UTF-8");
    fs::create_directories(src / "emptydir" / "alsoempty");
    fs::create_symlink(src / "a.txt", src / "link-to-file");
    fs::create_directory_symlink(src / "sub", src / "link-to-dir");  // not followed
    fs::create_symlink(root / "nowhere", src / "broken-link");
    for (int d = 0; d < 30; ++d) for (int f = 0; f < 100; ++f) Write(src / "many" / ("d" + std::to_string(d)) / ("f" + std::to_string(f) + ".txt"), std::to_string(d * 1000 + f));
    Write(root / "send" / "single.txt", "just a file");

    Node S, R1, R2;
    S.name = "S"; R1.name = "R1"; R2.name = "R2";
    S.route = [&](int p, const std::string& l) { Node& r = p == 1 ? R1 : R2; r.Post([&r, l] { r.cs->HandleLine(7, "S", true, l); }); };
    R1.route = [&](int, const std::string& l) { S.Post([&S, l] { S.cs->HandleLine(1, "R1", true, l); }); };
    R2.route = [&](int, const std::string& l) { S.Post([&S, l] { S.cs->HandleLine(2, "R2", true, l); }); };
    S.Start(root / "inS"); R1.Start(root / "inR1"); R2.Start(root / "inR2");

    // ---------------------------------------------------------------- 1 + 2: a folder to two PCs at once
    std::printf("TEST 1/2: folder with odd entries to two PCs at once\n");
    S.clipFiles = {src, root / "send" / "single.txt"};
    S.Post([&] { S.cs->SendTo(1, "R1"); S.cs->SendTo(2, "R2"); });
    CHECK(WaitFor([&] { return S.Count("delivered to") == 2; }));
    CHECK(R1.Saw("Clipboard received from S") && R2.Saw("Clipboard received from S"));
    CHECK(S.Count("Clipboard: ") == 1);                           // the folder walk (and its note) happened ONCE for two PCs
    for (Node* r : {&R1, &R2}) {
        auto clip = r->Clip();
        CHECK(clip.size() == 2);
        if (clip.size() == 2) {
            CHECK(clip[0].filename() == "My Project");
            CHECK(clip[1].filename() == "single.txt");
            CHECK(Slurp(clip[1]) == "just a file");
            int f = 0, d = 0; Compare(src, clip[0], f, d);
            std::printf("   %s: compared %d files, %d folders\n", r->name.c_str(), f, d);
            CHECK(f == 3009 && d == 37);
            const fs::path sub = clip[0] / "sub";
            CHECK(fs::exists(sub / "caf_.txt"));                  // invalid UTF-8 byte replaced, no exception
            {   // names differing only in case both survive, under names that differ even ignoring case
                std::set<std::string> low; int n = 0;
                for (auto& e : fs::directory_iterator(sub)) { std::string f = e.path().filename().string(); for (auto& c : f) c = (char)std::tolower((unsigned char)c); if (f.rfind("name", 0) == 0) { ++n; low.insert(f); } }
                CHECK(n == 2 && low.size() == 2);
            }
            CHECK(fs::exists(clip[0] / "emptydir" / "alsoempty"));
            CHECK(fs::exists(clip[0] / "empty.bin") && fs::file_size(clip[0] / "empty.bin") == 0);
        }
    }

    // ---------------------------------------------------------------- 3: the receiver already has it
    std::printf("TEST 3: same clipboard again -> receiver says it already has it\n");
    S.Clear(); R1.Clear();
    S.Post([&] { S.cs->SendTo(1, "R1"); });
    WaitFor([&] { return false; }, 700);
    CHECK(!S.Saw("Clipboard sent to R1") && !R1.Saw("Clipboard received"));

    // ---------------------------------------------------------------- 4: limit exceeded while walking
    std::printf("TEST 4: limit exceeded -> activity log says so, nothing is sent\n");
    S.cs->SetLimit(1 << 20);
    S.Clear(); R1.Clear();
    S.Post([&] { S.cs->SendTo(1, "R1"); });
    CHECK(WaitFor([&] { return S.Saw("larger than the limit of 1.00 MiB"); }));
    WaitFor([&] { return false; }, 300);
    CHECK(!R1.Saw("received"));
    S.cs->SetLimit(1ull << 30);

    // ---------------------------------------------------------------- 5: too many items
    std::printf("TEST 5: more than %u items\n", ClipShare::kMaxItems);
    {
        const fs::path huge = root / "huge";
        fs::create_directories(huge);
        for (unsigned i = 0; i < ClipShare::kMaxItems + 5; ++i) { std::ofstream(huge / ("f" + std::to_string(i))) << ""; }
        S.clipFiles = {huge};
        S.Clear();
        S.Post([&] { S.cs->SendTo(2, "R2"); });
        CHECK(WaitFor([&] { return S.Saw("files and folders"); }));
    }

    // ---------------------------------------------------------------- 6: text
    std::printf("TEST 6: text\n");
    S.clipFiles.clear(); S.clipText = "hello \xe2\x9c\x93 text\nline2"; S.Clear(); R2.Clear();
    S.Post([&] { S.cs->SendTo(2, "R2"); });
    CHECK(WaitFor([&] { return S.Saw("delivered to R2"); }));
    CHECK(R2.clipText == S.clipText);

    // ---------------------------------------------------------------- 7: receiver refuses (limit smaller on receiver)
    std::printf("TEST 7: receiver has a smaller limit\n");
    R1.cs->SetLimit(2048);
    S.clipFiles = {root / "send" / "My Project" / "big.bin"}; S.clipText.clear(); S.Clear();
    S.Post([&] { S.cs->SendTo(1, "R1"); });
    CHECK(WaitFor([&] { return S.Saw("larger than the limit set on that PC"); }));
    R1.cs->SetLimit(1ull << 30);

    // ---------------------------------------------------------------- 8: a file that changes while it is copied
    std::printf("TEST 8: file grows / shrinks while it is being sent -> refused, never cut off\n");
    for (int mode = 0; mode < 2; ++mode) {
        const fs::path g = root / (mode ? "shrink.bin" : "grow.bin");
        Write(g, blob(48 * 1024 * 1024));
        S.clipFiles = {g}; S.clipText.clear(); S.Clear(); R2.Clear();
        S.Post([&] { S.cs->SendTo(2, "R2"); });
        CHECK(WaitFor([&] { return S.Saw("Sending 1 file"); }));
        if (mode) fs::resize_file(g, 1024); else std::ofstream(g, std::ios::app | std::ios::binary) << "more";
        CHECK(WaitFor([&] { return S.Saw("changed while it was copied"); }));
        WaitFor([&] { return false; }, 300);
        CHECK(!R2.Saw("Clipboard received"));
        CHECK(!fs::exists(root / "inR2" / "t99"));
    }

    // ---------------------------------------------------------------- 9: the connection dies in the middle of a transfer
    std::printf("TEST 9: connection lost mid-transfer\n");
    {
        const fs::path g = root / "lost.bin";
        Write(g, blob(48 * 1024 * 1024));
        S.clipFiles = {g}; S.Clear(); R1.Clear();
        S.Post([&] { S.cs->SendTo(1, "R1"); });
        CHECK(WaitFor([&] { return S.Saw("Sending 1 file"); }));
        S.Post([&] { S.cs->PeerLost(1); });
        R1.Post([&] { R1.cs->PeerLost(7); });
        CHECK(WaitFor([&] { return S.Saw("connection lost"); }));
        // and the next crossing works again
        S.clipFiles = {root / "send" / "single.txt"}; S.Clear(); R1.Clear();
        S.Post([&] { S.cs->SendTo(1, "R1"); });
        CHECK(WaitFor([&] { return S.Saw("delivered to R1"); }));
    }

    S.Stop(); R1.Stop(); R2.Stop();
    fs::remove_all(root);
    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL OK\n", g_fail);
    return g_fail ? 1 : 0;
}
