// asio first: it pulls in winsock2 before anything includes windows.h
#include <boost/asio.hpp>

#include "inputudp.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "monocypher/monocypher.h"

namespace asio = boost::asio;
using asio::ip::udp;
using error_code = boost::system::error_code;
using Clock = std::chrono::steady_clock;

namespace {

// Datagram: 16 bytes header (authenticated, not hidden) | 23 bytes payload (encrypted) | 16 bytes MAC
//   header:  'S' 'I' version type(0 motion, 1 probe) instance(u32) seq(u64)       all little endian
//   payload: flags(u8, bit 0: "I receive your datagrams") epoch(u16) tcpCount(u32) mx my wx wy (i32 each)
constexpr std::size_t kHeader  = 16;
constexpr std::size_t kPayload = 23;
constexpr std::size_t kMac     = 16;
constexpr std::size_t kPacket  = kHeader + kPayload + kMac;
constexpr std::uint8_t kVersion = 1;
constexpr std::uint8_t kTypeMotion = 0, kTypeProbe = 1;
constexpr auto kProbeEvery = std::chrono::milliseconds(250);
constexpr auto kAlive      = std::chrono::milliseconds(1000);   // "usable" while the peer confirmed us this recently

void Put32(std::uint8_t* p, std::uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (std::uint8_t)(v >> (8 * i)); }
void Put64(std::uint8_t* p, std::uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (std::uint8_t)(v >> (8 * i)); }
std::uint32_t Get32(const std::uint8_t* p) { std::uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= (std::uint32_t)p[i] << (8 * i); return v; }
std::uint64_t Get64(const std::uint8_t* p) { std::uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= (std::uint64_t)p[i] << (8 * i); return v; }

// Nonce = sequence number, plus the type so that motion and probes (separate counters) never share one.
void MakeNonce(std::uint8_t type, std::uint64_t seq, std::uint8_t n[24])
{
    std::memset(n, 0, 24);
    Put64(n, seq);
    n[8] = type;
}

void DeriveKey(std::uint8_t out[32], const std::uint8_t in[32])
{
    static const char kLabel[] = "si-input-v1";
    crypto_blake2b_keyed(out, 32, in, 32, reinterpret_cast<const std::uint8_t*>(kLabel), sizeof kLabel - 1);
}

} // namespace

struct InputUdp::Impl {
    struct Peer {
        int id = 0;
        std::uint32_t inst = 0;
        udp::endpoint ep;
        std::uint8_t txKey[32] = {}, rxKey[32] = {};
        bool keyed = false;
        std::uint64_t probeSeq = 0;                 // last probe sequence number we sent
        std::uint64_t lastProbeRx = 0;
        std::uint64_t motionHi = 0, motionSeen = 0;   // anti-replay window: highest sequence number, and which of the 64 before it came
        Clock::time_point lastRx{}, lastAck{};      // last datagram from it / last one saying it hears us
    };

    explicit Impl(InputUdp* o, std::uint32_t inst, unsigned short prt) : owner(o), self(inst), port(prt) {}

    InputUdp* owner;
    const std::uint32_t self;
    const unsigned short port;

    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard{asio::make_work_guard(io)};
    udp::socket sock{io};          // receive only (io thread)
    udp::socket sendSock{io};      // send only, guarded by sendMu (engine thread and probe timer)
    asio::steady_timer timer{io};
    std::thread th;
    std::mutex sendMu;
    udp::endpoint from;
    std::array<std::uint8_t, 512> rx{};
    bool rxOpen = false;
    bool stopped = false;

    mutable std::mutex mtx;
    std::map<std::uint32_t, Peer> peers;            // by instance

    bool Start(std::string* err)
    {
        error_code ec;
        std::string problems;
        sock.open(udp::v4(), ec);
        if (!ec) sock.set_option(asio::socket_base::reuse_address(true), ec);
        if (!ec) sock.bind(udp::endpoint(udp::v4(), port), ec);
        if (ec) {
            problems += "cannot receive on UDP " + std::to_string(port) + ": " + ec.message();
            error_code ig; sock.close(ig);
        } else {
            rxOpen = true;
        }
        error_code ec2;
        sendSock.open(udp::v4(), ec2);
        if (ec2) problems += (problems.empty() ? "" : "; ") + std::string("cannot open the send socket: ") + ec2.message();
        th = std::thread([this] { io.run(); });
        if (rxOpen) asio::post(io, [this] { DoReceive(); });
        asio::post(io, [this] { ArmTimer(); });
        if (err) *err = problems;
        return problems.empty();
    }

    void Stop()
    {
        if (stopped) return;
        stopped = true;
        if (th.joinable()) {
            asio::post(io, [this] { error_code ec; sock.close(ec); timer.cancel(ec); });
            guard.reset();
            th.join();
        }
        error_code ec;
        sendSock.close(ec);
    }

    // ------------------------------------------------------------------ peers
    void AddPeer(int id, std::uint32_t inst, const std::string& ip, const std::uint8_t tx[32], const std::uint8_t rxk[32], unsigned short peerPort)
    {
        error_code ec;
        auto a = asio::ip::make_address(ip, ec);
        if (ec) return;
        std::lock_guard<std::mutex> lk(mtx);
        Peer& p = peers[inst];
        p = Peer{};
        p.id = id;
        p.inst = inst;
        p.ep = udp::endpoint(a, peerPort ? peerPort : port);
        DeriveKey(p.txKey, tx);
        DeriveKey(p.rxKey, rxk);
        p.keyed = true;
    }

    void RemovePeer(int id)
    {
        std::lock_guard<std::mutex> lk(mtx);
        for (auto it = peers.begin(); it != peers.end(); ++it)
            if (it->second.id == id) {
                crypto_wipe(it->second.txKey, 32);
                crypto_wipe(it->second.rxKey, 32);
                peers.erase(it);
                return;
            }
    }

    bool Usable(int id) const
    {
        if (!rxOpen) return false;
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lk(mtx);
        for (const auto& kv : peers)
            if (kv.second.id == id) return kv.second.keyed && now - kv.second.lastAck < kAlive;
        return false;
    }

    // ------------------------------------------------------------------ sending
    // Builds and sends one datagram to 'p'. mtx held by the caller for the key and the flags.
    void SealAndSend(Peer& p, std::uint8_t type, std::uint64_t seq, const MotionPacket& m, bool ack)
    {
        std::uint8_t pkt[kPacket];
        pkt[0] = 'S'; pkt[1] = 'I'; pkt[2] = kVersion; pkt[3] = type;
        Put32(pkt + 4, self);
        Put64(pkt + 8, seq);
        std::uint8_t plain[kPayload];
        plain[0] = ack ? 1 : 0;
        plain[1] = (std::uint8_t)(m.epoch & 0xff); plain[2] = (std::uint8_t)(m.epoch >> 8);
        Put32(plain + 3, m.tcpCount);
        Put32(plain + 7,  (std::uint32_t)m.mx);
        Put32(plain + 11, (std::uint32_t)m.my);
        Put32(plain + 15, (std::uint32_t)m.wx);
        Put32(plain + 19, (std::uint32_t)m.wy);
        std::uint8_t nonce[24];
        MakeNonce(type, seq, nonce);
        crypto_aead_lock(pkt + kHeader, pkt + kHeader + kPayload, p.txKey, nonce, pkt, kHeader, plain, kPayload);
        const udp::endpoint to = p.ep;
        std::lock_guard<std::mutex> sl(sendMu);
        error_code ec;
        sendSock.send_to(asio::buffer(pkt, sizeof pkt), to, 0, ec);    // lost datagrams are fine
    }

    bool Send(int id, std::uint64_t seq, const MotionPacket& m)
    {
        if (!sendSock.is_open()) return false;
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lk(mtx);
        for (auto& kv : peers) {
            Peer& p = kv.second;
            if (p.id != id || !p.keyed) continue;
            SealAndSend(p, kTypeMotion, seq, m, now - p.lastRx < kAlive);
            return true;
        }
        return false;
    }

    void ArmTimer()
    {
        if (stopped) return;
        error_code ec;
        timer.expires_after(kProbeEvery);
        timer.async_wait([this](const error_code& e) {
            if (e || stopped) return;
            Probe();
            ArmTimer();
        });
    }

    void Probe()
    {
        if (!sendSock.is_open()) return;
        const auto now = Clock::now();
        const MotionPacket none;
        std::lock_guard<std::mutex> lk(mtx);
        for (auto& kv : peers) {
            Peer& p = kv.second;
            if (!p.keyed) continue;
            SealAndSend(p, kTypeProbe, ++p.probeSeq, none, now - p.lastRx < kAlive);
        }
    }

    // ------------------------------------------------------------------ receiving
    void DoReceive()
    {
        sock.async_receive_from(asio::buffer(rx), from, [this](const error_code& ec, std::size_t n) {
            if (ec == asio::error::operation_aborted || stopped) return;
            if (!ec) Handle(n);
            DoReceive();                    // also after errors (Windows reports ICMP "port unreachable" here)
        });
    }

    void Handle(std::size_t n)
    {
        if (n != kPacket) return;
        const std::uint8_t* b = rx.data();
        if (b[0] != 'S' || b[1] != 'I' || b[2] != kVersion) return;
        const std::uint8_t type = b[3];
        if (type != kTypeMotion && type != kTypeProbe) return;
        const std::uint32_t inst = Get32(b + 4);
        const std::uint64_t seq = Get64(b + 8);

        MotionPacket m;
        bool ack = false;
        int id = 0;
        bool deliver = false;
        {
            std::lock_guard<std::mutex> lk(mtx);
            auto it = peers.find(inst);
            if (it == peers.end() || !it->second.keyed) return;     // only peers connected over TCP, with keys
            Peer& p = it->second;
            std::uint8_t nonce[24];
            MakeNonce(type, seq, nonce);
            std::uint8_t plain[kPayload];
            if (crypto_aead_unlock(plain, b + kHeader + kPayload, p.rxKey, nonce, b, kHeader, b + kHeader, kPayload) != 0)
                return;                                              // forged or damaged: changes nothing
            // authentic from here on. A datagram may arrive out of order (UDP) but never twice, and not far behind.
            if (type == kTypeMotion) {
                if (seq > p.motionHi) {
                    const std::uint64_t shift = seq - p.motionHi;
                    p.motionSeen = shift >= 64 ? 0 : p.motionSeen << shift;
                    p.motionSeen |= 1;
                    p.motionHi = seq;
                } else {
                    const std::uint64_t diff = p.motionHi - seq;
                    if (diff >= 64 || (p.motionSeen >> diff & 1)) return;      // replay, or too old
                    p.motionSeen |= std::uint64_t(1) << diff;
                }
            } else {
                if (seq <= p.lastProbeRx) return;
                p.lastProbeRx = seq;
            }
            const auto now = Clock::now();
            p.lastRx = now;
            ack = (plain[0] & 1) != 0;
            if (ack) p.lastAck = now;
            if (type == kTypeMotion) {
                m.epoch    = (std::uint16_t)(plain[1] | (plain[2] << 8));
                m.tcpCount = Get32(plain + 3);
                m.mx = (std::int32_t)Get32(plain + 7);
                m.my = (std::int32_t)Get32(plain + 11);
                m.wx = (std::int32_t)Get32(plain + 15);
                m.wy = (std::int32_t)Get32(plain + 19);
                id = p.id;
                deliver = true;
            }
        }
        if (deliver && owner->onMotion) owner->onMotion(id, seq, m);
    }
};

InputUdp::InputUdp(std::uint32_t myInstance, unsigned short port) : d_(std::make_unique<Impl>(this, myInstance, port)) {}
InputUdp::~InputUdp() { d_->Stop(); }
bool InputUdp::Start(std::string* err) { return d_->Start(err); }
void InputUdp::Stop() { d_->Stop(); }
void InputUdp::AddPeer(int id, std::uint32_t instance, const std::string& ip, const std::uint8_t tx[32], const std::uint8_t rx[32],
                       unsigned short peerPort)
{ d_->AddPeer(id, instance, ip, tx, rx, peerPort); }
void InputUdp::RemovePeer(int id) { d_->RemovePeer(id); }
bool InputUdp::Usable(int id) const { return d_->Usable(id); }
bool InputUdp::Send(int id, std::uint64_t seq, const MotionPacket& p) { return d_->Send(id, seq, p); }
