// asio first: it pulls in winsock2 before anything includes windows.h
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <boost/asio.hpp>

#include "audio.hpp"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <miniaudio.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace asio = boost::asio;
using asio::ip::udp;
using boost::system::error_code;
using Clock = std::chrono::steady_clock;

namespace {

// ---- wire format: 12 byte header + 5 ms of 48 kHz stereo 16-bit PCM (960 bytes)
//   [0..1] magic 'SA'  [2] version  [3] channels  [4..7] sender instance  [8..11] sequence
constexpr int            kRate          = 48000;
constexpr int            kChannels      = 2;
constexpr int            kPacketFrames  = 240;
constexpr std::size_t    kPacketSamples = kPacketFrames * kChannels;
constexpr std::size_t    kPayloadBytes  = kPacketSamples * sizeof(std::int16_t);
constexpr std::size_t    kHeaderBytes   = 12;
constexpr std::uint16_t  kMagic         = 0x5341;
constexpr std::uint8_t   kVersion       = 1;

// ---- receive side jitter buffer (all in samples = frames * channels)
constexpr std::size_t kTargetSamples = 30  * kRate / 1000 * kChannels;   // start playing at 30 ms
constexpr std::size_t kMaxSamples    = 120 * kRate / 1000 * kChannels;   // above this: drop to target
constexpr std::size_t kRingSamples   = 250 * kRate / 1000 * kChannels;

const std::int16_t kZeros[kPacketSamples] = {};

void Put32(unsigned char* p, std::uint32_t v)
{
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

std::uint32_t Get32(const unsigned char* p)
{
    return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) |
           ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
}

struct Peer {
    std::string ip;
    udp::endpoint ep;

    std::vector<std::int16_t> ring = std::vector<std::int16_t>(kRingSamples, 0);
    std::size_t head = 0, count = 0;
    bool primed = false;
    bool haveSeq = false;
    std::uint32_t lastSeq = 0;

    std::uint64_t bytes = 0, statBytes = 0;
    Clock::time_point lastPkt{};
    Clock::time_point statT = Clock::now();
    double kbps = 0;
    float level = 0;
};

void Push(Peer& p, const std::int16_t* s, std::size_t n)
{
    if (n > kRingSamples) { s += n - kRingSamples; n = kRingSamples; }
    if (p.count + n > kRingSamples) {                       // overflow: drop oldest
        const std::size_t drop = p.count + n - kRingSamples;
        p.head = (p.head + drop) % kRingSamples;
        p.count -= drop;
    }
    const std::size_t tail  = (p.head + p.count) % kRingSamples;
    const std::size_t first = std::min(n, kRingSamples - tail);
    std::memcpy(&p.ring[tail], s, first * sizeof(std::int16_t));
    if (n > first) std::memcpy(&p.ring[0], s + first, (n - first) * sizeof(std::int16_t));
    p.count += n;
}

} // namespace

struct AudioEngine::Impl {
    explicit Impl(std::uint32_t s) : self(s) { dests.reserve(64); }

    const std::uint32_t self;

    // ---- network (own io thread, separate sockets for receive and send)
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard{asio::make_work_guard(io)};
    udp::socket sock{io};        // receive only, used by the io thread
    udp::socket sendSock{io};    // send only, used by the capture thread
    std::thread th;
    udp::endpoint from;
    std::array<unsigned char, 2048> rx{};

    // ---- peers
    std::mutex mtx;
    std::map<std::uint32_t, Peer> peers;

    // ---- devices
    ma_device playback;
    ma_device capture;
    bool playbackOn = false;
    bool captureOn  = false;
    bool stopped    = false;

    std::atomic<bool>  sending{false};
    std::atomic<bool>  muted{false};
    std::atomic<float> volume{1.0f};
    std::atomic<float> inLevel{0.0f};

    // capture thread state
    std::array<std::int16_t, kPacketSamples> stage{};
    std::size_t stageCount = 0;
    std::uint32_t seq = 0;
    std::vector<udp::endpoint> dests;

    // ------------------------------------------------------------------ start / stop
    bool Start(std::string* err)
    {
        std::string problems;
        error_code ec;

        sock.open(udp::v4(), ec);
        if (!ec) sock.set_option(asio::socket_base::reuse_address(true), ec);
        if (!ec) sock.bind(udp::endpoint(udp::v4(), kAudioPort), ec);
        if (ec) {
            problems += "cannot receive on UDP " + std::to_string(kAudioPort) + ": " + ec.message() + "; ";
            error_code ig; sock.close(ig);
        }

        sendSock.open(udp::v4(), ec);
        if (ec) problems += "cannot open send socket: " + ec.message() + "; ";

        th = std::thread([this] { io.run(); });
        if (sock.is_open()) asio::post(io, [this] { DoReceive(); });

        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format   = ma_format_f32;
        cfg.playback.channels = kChannels;
        cfg.sampleRate        = kRate;
        cfg.periodSizeInFrames = 480;
        cfg.performanceProfile = ma_performance_profile_low_latency;
        cfg.dataCallback      = &Impl::PlaybackCb;
        cfg.pUserData         = this;
        ma_result r = ma_device_init(nullptr, &cfg, &playback);
        if (r != MA_SUCCESS) {
            problems += std::string("playback device: ") + ma_result_description(r) + "; ";
        } else if ((r = ma_device_start(&playback)) != MA_SUCCESS) {
            ma_device_uninit(&playback);
            problems += std::string("playback start: ") + ma_result_description(r) + "; ";
        } else {
            playbackOn = true;
        }

        if (err) *err = problems;
        return problems.empty();
    }

    void Stop()
    {
        if (stopped) return;
        stopped = true;
        sending = false;
        if (captureOn)  { ma_device_uninit(&capture);  captureOn = false; }
        if (playbackOn) { ma_device_uninit(&playback); playbackOn = false; }
        if (th.joinable()) {
            asio::post(io, [this] { error_code ec; sock.close(ec); });
            guard.reset();
            th.join();
        }
        error_code ec;
        sendSock.close(ec);
    }

    // ------------------------------------------------------------------ peers
    void AddPeer(std::uint32_t inst, const std::string& ip)
    {
        error_code ec;
        auto a = asio::ip::make_address(ip, ec);
        if (ec) return;
        std::lock_guard<std::mutex> lk(mtx);
        Peer& p = peers[inst];
        p.ip = ip;
        p.ep = udp::endpoint(a, kAudioPort);
    }

    void RemovePeer(std::uint32_t inst)
    {
        std::lock_guard<std::mutex> lk(mtx);
        peers.erase(inst);
    }

    // ------------------------------------------------------------------ capture / send
    bool SetSending(bool on, AudioSource src, std::string* err)
    {
        if (captureOn) { ma_device_uninit(&capture); captureOn = false; }
        sending = false;
        inLevel = 0.0f;
        if (!on) return true;

        ma_device_config cfg = ma_device_config_init(
            src == AudioSource::System ? ma_device_type_loopback : ma_device_type_capture);
        cfg.capture.format    = ma_format_s16;
        cfg.capture.channels  = kChannels;
        cfg.sampleRate        = kRate;
        cfg.periodSizeInFrames = kPacketFrames;
        cfg.performanceProfile = ma_performance_profile_low_latency;
        cfg.dataCallback      = &Impl::CaptureCb;
        cfg.pUserData         = this;

        ma_result r = ma_device_init(nullptr, &cfg, &capture);
        if (r != MA_SUCCESS) { if (err) *err = ma_result_description(r); return false; }

        stageCount = 0;
        r = ma_device_start(&capture);
        if (r != MA_SUCCESS) {
            ma_device_uninit(&capture);
            if (err) *err = ma_result_description(r);
            return false;
        }
        captureOn = true;
        sending = true;
        return true;
    }

    static void CaptureCb(ma_device* dev, void*, const void* in, ma_uint32 frames)
    {
        static_cast<Impl*>(dev->pUserData)->OnCapture(static_cast<const std::int16_t*>(in), frames);
    }

    void OnCapture(const std::int16_t* in, ma_uint32 frames)
    {
        if (!sending.load() || !in) return;
        const std::size_t n = (std::size_t)frames * kChannels;

        int peak = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const int a = in[i] < 0 ? -in[i] : in[i];
            if (a > peak) peak = a;
        }
        inLevel = std::max(peak / 32768.0f, inLevel.load() * 0.9f);

        std::size_t pos = 0;
        while (pos < n) {
            const std::size_t take = std::min(n - pos, kPacketSamples - stageCount);
            std::memcpy(stage.data() + stageCount, in + pos, take * sizeof(std::int16_t));
            stageCount += take;
            pos += take;
            if (stageCount == kPacketSamples) {
                SendPacket(stage.data());
                stageCount = 0;
            }
        }
    }

    void SendPacket(const std::int16_t* samples)
    {
        unsigned char pkt[kHeaderBytes + kPayloadBytes];
        pkt[0] = (unsigned char)(kMagic & 0xFF);
        pkt[1] = (unsigned char)(kMagic >> 8);
        pkt[2] = kVersion;
        pkt[3] = (unsigned char)kChannels;
        Put32(pkt + 4, self);
        Put32(pkt + 8, seq++);
        std::memcpy(pkt + kHeaderBytes, samples, kPayloadBytes);

        dests.clear();
        {
            std::lock_guard<std::mutex> lk(mtx);
            for (auto& kv : peers) dests.push_back(kv.second.ep);
        }
        for (auto& ep : dests) {
            error_code ec;
            sendSock.send_to(asio::buffer(pkt, sizeof pkt), ep, 0, ec);   // lost packets are fine
        }
    }

    // ------------------------------------------------------------------ receive
    void DoReceive()
    {
        sock.async_receive_from(asio::buffer(rx), from, [this](error_code ec, std::size_t n) {
            if (ec == asio::error::operation_aborted || !sock.is_open()) return;
            if (!ec) OnPacket(n);
            DoReceive();
        });
    }

    void OnPacket(std::size_t n)
    {
        if (n != kHeaderBytes + kPayloadBytes) return;
        const unsigned char* b = rx.data();
        if (b[0] != (kMagic & 0xFF) || b[1] != (kMagic >> 8) || b[2] != kVersion) return;

        const std::uint32_t inst = Get32(b + 4);
        const std::uint32_t sq   = Get32(b + 8);

        std::int16_t samples[kPacketSamples];
        std::memcpy(samples, b + kHeaderBytes, kPayloadBytes);
        int peak = 0;
        for (std::size_t i = 0; i < kPacketSamples; ++i) {
            const int a = samples[i] < 0 ? -samples[i] : samples[i];
            if (a > peak) peak = a;
        }

        std::lock_guard<std::mutex> lk(mtx);
        auto it = peers.find(inst);
        if (it == peers.end()) return;            // only audio from connected peers
        Peer& p = it->second;

        if (p.haveSeq) {
            const std::int32_t diff = (std::int32_t)(sq - p.lastSeq);
            if (diff <= 0) return;                // duplicate / late packet
            if (diff > 100) {                     // long gap: restart buffering
                p.count = 0;
                p.primed = false;
            } else {
                for (int i = 1; i < diff && i <= 20; ++i)   // keep timing with silence
                    Push(p, kZeros, kPacketSamples);
            }
        }
        p.lastSeq = sq;
        p.haveSeq = true;

        Push(p, samples, kPacketSamples);
        if (p.count > kMaxSamples) {              // too much latency (clock drift): skip ahead
            const std::size_t drop = p.count - kTargetSamples;
            p.head = (p.head + drop) % kRingSamples;
            p.count -= drop;
        }

        p.bytes += n;
        p.lastPkt = Clock::now();
        p.level = std::max(peak / 32768.0f, p.level * 0.97f);
    }

    // ------------------------------------------------------------------ playback / mix
    static void PlaybackCb(ma_device* dev, void* out, const void*, ma_uint32 frames)
    {
        static_cast<Impl*>(dev->pUserData)->Mix(static_cast<float*>(out), frames);
    }

    void Mix(float* o, ma_uint32 frames)
    {
        const std::size_t n = (std::size_t)frames * kChannels;
        std::memset(o, 0, n * sizeof(float));
        if (muted.load()) return;
        const float vol = volume.load() * (1.0f / 32768.0f);

        {
            std::lock_guard<std::mutex> lk(mtx);
            for (auto& kv : peers) {
                Peer& p = kv.second;
                if (!p.primed) {
                    if (p.count >= kTargetSamples) p.primed = true;
                    else continue;
                }
                const std::size_t take = std::min(n, p.count);
                std::size_t idx = p.head;
                for (std::size_t i = 0; i < take; ++i) {
                    o[i] += vol * (float)p.ring[idx];
                    if (++idx == kRingSamples) idx = 0;
                }
                p.head = idx;
                p.count -= take;
                if (take < n) p.primed = false;   // underrun: re-buffer
            }
        }
        for (std::size_t i = 0; i < n; ++i) o[i] = std::clamp(o[i], -1.0f, 1.0f);
    }

    // ------------------------------------------------------------------ stats
    std::vector<AudioPeerStat> Stats()
    {
        std::vector<AudioPeerStat> out;
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lk(mtx);
        for (auto& kv : peers) {
            Peer& p = kv.second;
            const double dt = std::chrono::duration<double>(now - p.statT).count();
            if (dt >= 0.5) {
                p.kbps = (double)(p.bytes - p.statBytes) * 8.0 / 1000.0 / dt;
                p.statBytes = p.bytes;
                p.statT = now;
            }
            const bool recv = p.lastPkt != Clock::time_point{} &&
                              now - p.lastPkt < std::chrono::seconds(1);
            out.push_back({kv.first, p.ip, recv ? p.kbps : 0.0, recv ? p.level : 0.0f, recv});
        }
        return out;
    }
};

// ---------------------------------------------------------------------------- facade
AudioEngine::AudioEngine(std::uint32_t myInstance) : d_(new Impl(myInstance)) {}
AudioEngine::~AudioEngine() { Stop(); }

bool AudioEngine::Start(std::string* err)                 { return d_->Start(err); }
void AudioEngine::Stop()                                  { d_->Stop(); }
void AudioEngine::AddPeer(std::uint32_t i, const std::string& ip) { d_->AddPeer(i, ip); }
void AudioEngine::RemovePeer(std::uint32_t i)             { d_->RemovePeer(i); }
bool AudioEngine::SetSending(bool on, AudioSource s, std::string* e) { return d_->SetSending(on, s, e); }
void AudioEngine::SetMuted(bool m)                        { d_->muted = m; }
void AudioEngine::SetVolume(float v)                      { d_->volume = std::clamp(v, 0.0f, 1.5f); }
std::vector<AudioPeerStat> AudioEngine::Stats()           { return d_->Stats(); }
float AudioEngine::InputLevel() const                     { return d_->inLevel.load(); }

bool AudioEngine::SystemAudioSupported()
{
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}
