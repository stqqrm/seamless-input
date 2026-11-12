// Test of the encrypted UDP layer: round trip, replay window, forged datagrams, wrong keys, fallback.
// Build and run (Linux, Boost headers needed):
//   gcc -c -I src src/monocypher/monocypher.c -o monocypher.o
//   g++ -std=c++17 -O1 -I src -pthread tests/test_udp.cpp src/inputudp.cpp monocypher.o -o test_udp && ./test_udp
#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include "inputudp.hpp"
namespace asio = boost::asio;
using asio::ip::udp;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL: %s\n", msg); ++fails; } else std::printf("ok:   %s\n", msg); } while (0)
static void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
int main()
{
    std::uint8_t k1[32], k2[32], other[32];
    for (int i = 0; i < 32; ++i) { k1[i] = (std::uint8_t)(i + 1); k2[i] = (std::uint8_t)(100 + i); other[i] = (std::uint8_t)(200 + i); }
    InputUdp A(1, 46001), B(2, 46002);
    std::mutex m; std::vector<std::pair<std::uint64_t, MotionPacket>> got;
    B.onMotion = [&](int, std::uint64_t seq, const MotionPacket& p) { std::lock_guard<std::mutex> lk(m); got.push_back({seq, p}); };
    std::string e;
    CHECK(A.Start(&e) && B.Start(&e), "both sockets open");
    // A's tx key is B's rx key and the other way round, like after a real handshake
    A.AddPeer(10, 2, "127.0.0.1", k1, k2, 46002);
    B.AddPeer(20, 1, "127.0.0.1", k2, k1, 46001);
    CHECK(!A.Usable(10), "not usable before any probe was exchanged");
    for (int i = 0; i < 40 && !(A.Usable(10) && B.Usable(20)); ++i) Sleep(50);
    CHECK(A.Usable(10) && B.Usable(20), "usable in both directions after the probes");

    MotionPacket p; p.epoch = 7; p.tcpCount = 3; p.mx = -5; p.my = 123456; p.wx = 240; p.wy = -120;
    A.Send(10, 100, p); Sleep(100);
    { std::lock_guard<std::mutex> lk(m);
      CHECK(got.size() == 1 && got[0].first == 100 && got[0].second.epoch == 7 && got[0].second.tcpCount == 3 && got[0].second.mx == -5 &&
            got[0].second.my == 123456 && got[0].second.wx == 240 && got[0].second.wy == -120, "motion arrives intact (all fields)"); }
    A.Send(10, 98, p); Sleep(80);      // older, but inside the window: out of order delivery is fine
    A.Send(10, 98, p); Sleep(80);      // the same datagram again: a replay
    A.Send(10, 100, p); Sleep(80);     // the first one again
    { std::lock_guard<std::mutex> lk(m); CHECK(got.size() == 2 && got[1].first == 98, "out-of-order accepted once, replays dropped"); }
    A.Send(10, 100 + 70, p); Sleep(80);
    A.Send(10, 100 + 3, p); Sleep(80); // now more than 64 behind the newest: too old
    { std::lock_guard<std::mutex> lk(m); CHECK(got.size() == 3, "far behind the window: dropped"); }

    // forged / damaged datagrams
    asio::io_context io; udp::socket s(io); s.open(udp::v4());
    udp::endpoint to(asio::ip::make_address("127.0.0.1"), 46002);
    std::uint8_t junk[55]; std::memset(junk, 0x41, sizeof junk);
    s.send_to(asio::buffer(junk, sizeof junk), to);
    std::uint8_t hdr[55] = {'S', 'I', 1, 0, 1, 0, 0, 0, 0xff, 0, 0, 0, 0, 0, 0, 0};    // right header, no valid MAC
    s.send_to(asio::buffer(hdr, sizeof hdr), to);
    Sleep(100);
    { std::lock_guard<std::mutex> lk(m); CHECK(got.size() == 3, "garbage and forged datagrams change nothing"); }

    // a stranger with its own keys
    InputUdp C(3, 46003); C.Start(&e);
    C.AddPeer(30, 2, "127.0.0.1", other, other, 46002);
    for (int i = 0; i < 6; ++i) { C.Send(30, 1000 + i, p); Sleep(20); }
    { std::lock_guard<std::mutex> lk(m); CHECK(got.size() == 3, "a sender with other keys is ignored"); }
    CHECK(!B.Usable(99), "unknown peer is not usable");

    // the peer's keys are wrong on one side: the other never becomes usable
    InputUdp D(4, 46004), E(5, 46005); D.Start(&e); E.Start(&e);
    D.AddPeer(1, 5, "127.0.0.1", k1, k2, 46005);
    E.AddPeer(1, 4, "127.0.0.1", other, other, 46004);
    Sleep(800);
    CHECK(!D.Usable(1) && !E.Usable(1), "mismatching keys: UDP never becomes usable (TCP fallback)");

    // silence: usable only while the peer keeps confirming
    B.RemovePeer(20);
    Sleep(1300);
    CHECK(!A.Usable(10), "usable ends about a second after the peer stops answering");
    std::printf(fails ? "SOME FAILED\n" : "ALL PASS\n");
    return fails ? 1 : 0;
}
