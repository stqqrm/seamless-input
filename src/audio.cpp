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
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include "monocypher/monocypher.h"
}

namespace asio = boost::asio;
using asio::ip::udp;
using boost::system::error_code;
using Clock = std::chrono::steady_clock;

namespace {

// ---- wire format: 12 byte header + 5 ms of 48 kHz stereo 16-bit PCM (960 bytes) + 16 byte MAC
//   [0..1] magic 'SA'  [2] version  [3] stream (0 microphone, 1 system audio)
//   [4..7] sender instance  [8..11] sequence (counted per stream)
//   The PCM is encrypted with XChaCha20-Poly1305 using the per-connection audio key; the 12 header bytes
//   are authenticated (not hidden). Nonce = stream | sequence, so every packet of a connection is unique
//   and a modified, forged or re-addressed packet fails the MAC and is dropped.
constexpr int            kRate          = 48000;
constexpr int            kChannels      = 2;
constexpr int            kPacketFrames  = 240;
constexpr std::size_t    kPacketSamples = kPacketFrames * kChannels;
constexpr std::size_t    kPayloadBytes  = kPacketSamples * sizeof(std::int16_t);
constexpr std::size_t    kHeaderBytes   = 12;
constexpr std::size_t    kMacBytes      = 16;
constexpr std::size_t    kPacketBytes   = kHeaderBytes + kPayloadBytes + kMacBytes;
constexpr std::uint16_t  kMagic         = 0x5341;
constexpr std::uint8_t   kVersion       = 3;      // 3 = encrypted
constexpr int            kStreams       = 2;    // microphone, system audio

// ---- send side: audio is only sent while there is something to hear.
// A packet is "audible" when its peak is above the threshold (microphone: a noise gate, system audio: just
// above digital silence). After the last audible packet the stream stays open for a short hang time so word
// endings and reverb tails are not cut, and the packets right before the first audible one (pre-roll) are
// sent too so the start of a word is not clipped.
constexpr int kGatePeak[2]   = {200, 24};                                 // [stream] peak of 32768
constexpr int kHangPackets   = 400 / (kPacketFrames * 1000 / kRate);      // 400 ms
constexpr int kPreroll       = 3;                                         // packets (15 ms)

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

// One incoming stream of a peer (its microphone or its system audio) with its own jitter buffer.
struct Stream {
    std::vector<std::int16_t> ring = std::vector<std::int16_t>(kRingSamples, 0);
    std::size_t head = 0, count = 0;
    bool primed = false;
    bool haveSeq = false;
    std::uint32_t lastSeq = 0;
    float level = 0;
};

void MakeNonce(int stream, std::uint32_t seq, std::uint8_t n[24])
{
    std::memset(n, 0, 24);
    n[0] = (std::uint8_t)stream;
    n[1] = (std::uint8_t)(seq);
    n[2] = (std::uint8_t)(seq >> 8);
    n[3] = (std::uint8_t)(seq >> 16);
    n[4] = (std::uint8_t)(seq >> 24);
}

struct Dest {                       // one recipient of an outgoing packet
    udp::endpoint ep;
    std::uint8_t key[32];
};

struct Peer {
    std::string ip;
    udp::endpoint ep;

    bool keyed = false;                       // false: no audio to or from this peer
    std::uint8_t txKey[32] = {}, rxKey[32] = {};

    // per-device settings (guarded by Impl::mtx like everything else here)
    bool  send[kStreams] = {false, false};   // send OUR microphone / system audio to this peer
    bool  muted = false;     // silence what this peer sends us
    float vol   = 1.0f;      // 0..1.5, applied to what this peer sends us

    Stream in[kStreams];

    std::uint64_t bytes = 0, statBytes = 0;
    Clock::time_point lastPkt{};
    Clock::time_point statT = Clock::now();
    double kbps = 0;
};

void Push(Stream& p, const std::int16_t* s, std::size_t n)
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
    explicit Impl(std::uint32_t s) : self(s)
    {
        for (int i = 0; i < kStreams; ++i) { cap[i].owner = this; cap[i].stream = i; cap[i].dests.reserve(64); }
    }

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
    bool playbackOn = false;

    // One capture device per source (index = AudioSource), each with its own packet
    // staging buffer and sequence counter.
    struct Cap {
        Impl* owner = nullptr;
        int stream = 0;
        ma_device dev;
        bool on = false;
        std::atomic<bool>  sending{false};
        std::atomic<float> level{0.0f};
        std::array<std::int16_t, kPacketSamples> stage{};
        std::size_t stageCount = 0;
        std::uint32_t seq = 0;
        int hang = 0;                       // packets still to send after the last audible one (0 = gate closed)
        std::array<std::array<std::int16_t, kPacketSamples>, kPreroll> pre{};   // newest packets while the gate is closed
        int preCount = 0;
        std::vector<Dest> dests;            // scratch for SendPacket, only touched by this source's thread
    };
    Cap cap[kStreams];
#if defined(__linux__)
    // System audio on Linux = the PulseAudio/PipeWire "monitor" of the default output.
    // miniaudio's loopback device type only exists for WASAPI, so we open that monitor
    // source as an ordinary capture device. The context must outlive the device.
    ma_context sysCtx;
    bool sysCtxOn = false;
#endif
    bool stopped    = false;

    std::atomic<bool>  muted{false};
    std::atomic<float> volume{1.0f};

    // ---- virtual microphone: the microphone of the OTHER PCs comes out here, as a real input device, instead
    // of being played through the speakers. Linux: a PulseAudio/PipeWire null sink + a source that remaps its
    // monitor (created with pactl, removed again on exit). Windows: needs a virtual audio cable driver
    // (VB-Cable); we play into its "CABLE Input" and applications record from "CABLE Output".
    ma_context vmCtx;
    bool vmCtxOn = false;
    ma_device vmDev;
    bool vmOn = false;
    std::string vmName;                    // the microphone applications will see
    std::vector<std::string> vmModules;    // Linux: pactl module ids, unloaded in reverse order


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
        for (auto& c : cap) {
            c.sending = false;
            if (c.on) { ma_device_uninit(&c.dev); c.on = false; }
        }
        CloseSysCtx();
        CloseVirtualMic();
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
        auto it = peers.find(inst);
        if (it == peers.end()) return;
        crypto_wipe(it->second.txKey, 32);
        crypto_wipe(it->second.rxKey, 32);
        peers.erase(it);
    }

    void SetPeerKeys(std::uint32_t inst, const std::uint8_t* tx, const std::uint8_t* rx)
    {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = peers.find(inst);
        if (it == peers.end()) return;
        std::memcpy(it->second.txKey, tx, 32);
        std::memcpy(it->second.rxKey, rx, 32);
        it->second.keyed = true;
    }

    void SetPeerSend(std::uint32_t inst, AudioSource src, bool on)
    {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = peers.find(inst);
        if (it != peers.end()) it->second.send[(int)src] = on;
    }

    void SetPeerMuted(std::uint32_t inst, bool m)
    {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = peers.find(inst);
        if (it != peers.end()) it->second.muted = m;
    }

    void SetPeerVolume(std::uint32_t inst, float v)
    {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = peers.find(inst);
        if (it != peers.end()) it->second.vol = std::clamp(v, 0.0f, 1.5f);
    }

    // ------------------------------------------------------------------ virtual microphone
#if defined(__linux__)
    static std::string Sh(const std::string& cmd)
    {
        std::string out;
        FILE* f = popen((cmd + " 2>/dev/null").c_str(), "r");
        if (!f) return out;
        char buf[512];
        while (std::fgets(buf, sizeof buf, f)) out += buf;
        pclose(f);
        return out;
    }

    static bool IsNumber(const std::string& s)
    {
        return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
    }

    static std::string Trim(std::string s)
    {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        return s;
    }

    // modules left behind by a daemon that did not exit cleanly
    static void UnloadStaleVirtualMic()
    {
        const std::string list = Sh("pactl list short modules");
        std::size_t pos = 0;
        while (pos < list.size()) {
            std::size_t e = list.find('\n', pos);
            if (e == std::string::npos) e = list.size();
            const std::string line = list.substr(pos, e - pos);
            pos = e + 1;
            if (line.find("seamless_input_mic") == std::string::npos) continue;
            const std::string id = line.substr(0, line.find('\t'));
            if (IsNumber(id)) Sh("pactl unload-module " + id);
        }
    }
#endif

    void CloseVirtualMic()
    {
        if (vmOn) { ma_device_uninit(&vmDev); vmOn = false; }
        if (vmCtxOn) { ma_context_uninit(&vmCtx); vmCtxOn = false; }
#if defined(__linux__)
        for (auto it = vmModules.rbegin(); it != vmModules.rend(); ++it) Sh("pactl unload-module " + *it);
#endif
        vmModules.clear();
    }

    // Idempotent. true: *info = name of the microphone other applications can pick. false: *info = why not.
    bool EnableVirtualMic(std::string* info)
    {
        if (vmOn) { if (info) *info = vmName; return true; }
        auto fail = [&](const std::string& why) { CloseVirtualMic(); if (info) *info = why; return false; };

        ma_backend backends[1];
        ma_uint32 nBackends = 0;
        const char* wanted = nullptr;
#if defined(__linux__)
        UnloadStaleVirtualMic();
        const std::string sink = Trim(Sh("pactl load-module module-null-sink sink_name=seamless_input_mic "
                                         "rate=48000 channels=2 "
                                         "sink_properties='device.description=\"Seamless Input Microphone (output)\"'"));
        if (!IsNumber(sink))
            return fail("cannot create the virtual microphone: pactl failed (install pulseaudio-utils; PipeWire needs pipewire-pulse)");
        vmModules.push_back(sink);
        const std::string src = Trim(Sh("pactl load-module module-remap-source master=seamless_input_mic.monitor "
                                        "source_name=seamless_input_mic_source "
                                        "source_properties='device.description=\"Seamless Input Microphone\"'"));
        if (!IsNumber(src)) return fail("cannot create the virtual microphone source (module-remap-source)");
        vmModules.push_back(src);
        backends[nBackends++] = ma_backend_pulseaudio;
        wanted = "seamless_input_mic";
        vmName = "Seamless Input Microphone";
#elif defined(_WIN32)
        wanted = "CABLE Input";
        vmName = "CABLE Output";
#else
        return fail("a virtual microphone is not supported on this platform");
#endif
        if (ma_context_init(nBackends ? backends : nullptr, nBackends, nullptr, &vmCtx) != MA_SUCCESS)
            return fail("cannot open the audio system for the virtual microphone");
        vmCtxOn = true;

        ma_device_info id{};
        bool found = false;
        for (int attempt = 0; attempt < 10 && !found; ++attempt) {
            if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(50));   // the new sink needs a moment
            ma_device_info *pb = nullptr, *cp = nullptr;
            ma_uint32 nPb = 0, nCp = 0;
            if (ma_context_get_devices(&vmCtx, &pb, &nPb, &cp, &nCp) != MA_SUCCESS) continue;
            for (ma_uint32 i = 0; i < nPb && !found; ++i) {
#if defined(__linux__)
                found = std::string(pb[i].id.pulse) == wanted;
#else
                std::string n = pb[i].name, w = wanted;
                for (auto& ch : n) ch = (char)std::tolower((unsigned char)ch);
                for (auto& ch : w) ch = (char)std::tolower((unsigned char)ch);
                found = n.find(w) != std::string::npos || n.find("hi-fi cable input") != std::string::npos;
#endif
                if (found) id = pb[i];
            }
        }
        if (!found) {
#if defined(_WIN32)
            return fail("the microphone of the other PC needs a virtual audio cable on this PC: install VB-Cable "
                        "(vb-audio.com/Cable), restart, and pick \"CABLE Output\" as the microphone in your applications");
#else
            return fail("the virtual microphone output was not found");
#endif
        }

        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.pDeviceID = &id.id;
        cfg.playback.format    = ma_format_f32;
        cfg.playback.channels  = kChannels;
        cfg.sampleRate         = kRate;
        cfg.periodSizeInFrames = 480;
        cfg.performanceProfile = ma_performance_profile_low_latency;
        cfg.dataCallback       = &Impl::VMicCb;
        cfg.pUserData          = this;
        ma_result r = ma_device_init(&vmCtx, &cfg, &vmDev);
        if (r != MA_SUCCESS) return fail(std::string("virtual microphone device: ") + ma_result_description(r));
        if ((r = ma_device_start(&vmDev)) != MA_SUCCESS) {
            ma_device_uninit(&vmDev);
            return fail(std::string("virtual microphone start: ") + ma_result_description(r));
        }
        vmOn = true;
        if (info) *info = vmName;
        return true;
    }

    // ------------------------------------------------------------------ capture / send
    bool SetCapture(AudioSource src, bool on, std::string* err)
    {
        const bool system = src == AudioSource::System;
        Cap& c = cap[(int)src];
        if (c.on) { ma_device_uninit(&c.dev); c.on = false; }
        if (system) CloseSysCtx();
        c.sending = false;
        c.level = 0.0f;
        c.hang = 0;
        c.preCount = 0;
        if (!on) return true;

#if defined(__linux__)
        ma_device_id monitorId;
        if (system && !OpenMonitor(&monitorId, err)) return false;
        ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
        if (system) cfg.capture.pDeviceID = &monitorId;    // microphone: default input device
#else
        ma_device_config cfg = ma_device_config_init(
            system ? ma_device_type_loopback : ma_device_type_capture);
#endif
        cfg.capture.format    = ma_format_s16;
        cfg.capture.channels  = kChannels;
        cfg.sampleRate        = kRate;
        cfg.periodSizeInFrames = kPacketFrames;
        cfg.performanceProfile = ma_performance_profile_low_latency;
        cfg.dataCallback      = &Impl::CaptureCb;
        cfg.pUserData         = &c;

#if defined(__linux__)
        ma_context* ctx = (system && sysCtxOn) ? &sysCtx : nullptr;
#else
        ma_context* ctx = nullptr;
#endif
        ma_result r = ma_device_init(ctx, &cfg, &c.dev);
        if (r != MA_SUCCESS) { if (system) CloseSysCtx(); if (err) *err = ma_result_description(r); return false; }

        c.stageCount = 0;
        r = ma_device_start(&c.dev);
        if (r != MA_SUCCESS) {
            ma_device_uninit(&c.dev);
            if (system) CloseSysCtx();
            if (err) *err = ma_result_description(r);
            return false;
        }
        c.on = true;
        c.sending = true;
        return true;
    }

    void CloseSysCtx()
    {
#if defined(__linux__)
        if (sysCtxOn) { ma_context_uninit(&sysCtx); sysCtxOn = false; }
#endif
    }

#if defined(__linux__)
    // Finds the ".monitor" source of the default output (falls back to the first monitor).
    bool OpenMonitor(ma_device_id* out, std::string* err)
    {
        const ma_backend backends[] = {ma_backend_pulseaudio};   // PipeWire answers through pipewire-pulse
        if (ma_context_init(backends, 1, nullptr, &sysCtx) != MA_SUCCESS) {
            if (err) *err = "system audio needs PulseAudio or PipeWire (pipewire-pulse); neither is reachable";
            return false;
        }
        sysCtxOn = true;

        ma_device_info *pb = nullptr, *cap = nullptr;
        ma_uint32 nPb = 0, nCap = 0;
        if (ma_context_get_devices(&sysCtx, &pb, &nPb, &cap, &nCap) != MA_SUCCESS) {
            CloseSysCtx();
            if (err) *err = "could not list audio devices";
            return false;
        }

        std::string defaultSink;
        for (ma_uint32 i = 0; i < nPb; ++i)
            if (pb[i].isDefault) { defaultSink = pb[i].id.pulse; break; }

        const ma_device_info* best = nullptr;
        for (ma_uint32 i = 0; i < nCap; ++i) {
            const std::string name = cap[i].id.pulse;
            const std::string suffix = ".monitor";
            if (name.size() < suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
                continue;
            if (!best) best = &cap[i];
            if (!defaultSink.empty() && name == defaultSink + suffix) { best = &cap[i]; break; }
        }
        if (!best) {
            CloseSysCtx();
            if (err) *err = "no monitor source found (is an audio output active?)";
            return false;
        }
        *out = best->id;
        return true;
    }
#endif

    static void CaptureCb(ma_device* dev, void*, const void* in, ma_uint32 frames)
    {
        Cap* c = static_cast<Cap*>(dev->pUserData);
        c->owner->OnCapture(*c, static_cast<const std::int16_t*>(in), frames);
    }

    void OnCapture(Cap& c, const std::int16_t* in, ma_uint32 frames)
    {
        if (!c.sending.load() || !in) return;
        const std::size_t n = (std::size_t)frames * kChannels;

        int peak = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const int a = in[i] < 0 ? -in[i] : in[i];
            if (a > peak) peak = a;
        }
        c.level = std::max(peak / 32768.0f, c.level.load() * 0.9f);

        std::size_t pos = 0;
        while (pos < n) {
            const std::size_t take = std::min(n - pos, kPacketSamples - c.stageCount);
            std::memcpy(c.stage.data() + c.stageCount, in + pos, take * sizeof(std::int16_t));
            c.stageCount += take;
            pos += take;
            if (c.stageCount == kPacketSamples) {
                GatePacket(c, c.stage.data());
                c.stageCount = 0;
            }
        }
    }

    // Decides whether a finished packet goes out: only audible audio (plus hang time and pre-roll) is sent.
    // The sequence number only counts packets that are really sent, so the receiver sees no gap and
    // simply re-buffers when the sound starts again; with nothing sent it shows the device as idle.
    void GatePacket(Cap& c, const std::int16_t* samples)
    {
        int peak = 0;
        for (std::size_t i = 0; i < kPacketSamples; ++i) {
            const int a = samples[i] < 0 ? -samples[i] : samples[i];
            if (a > peak) peak = a;
        }
        const bool audible = peak >= kGatePeak[c.stream];

        if (audible) {
            if (c.hang == 0) {                                   // gate opens: pre-roll first, oldest first
                for (int i = c.preCount - 1; i >= 0; --i) SendPacket(c, c.pre[(std::size_t)i].data());
                c.preCount = 0;
            }
            c.hang = kHangPackets;
            SendPacket(c, samples);
            return;
        }
        if (c.hang > 0) {                                        // hang time: still open
            --c.hang;
            SendPacket(c, samples);
            return;
        }
        // gate closed: remember the newest packets (index 0 = newest)
        for (int i = kPreroll - 1; i > 0; --i) c.pre[(std::size_t)i] = c.pre[(std::size_t)i - 1];
        std::memcpy(c.pre[0].data(), samples, kPayloadBytes);
        c.preCount = std::min(c.preCount + 1, kPreroll);
    }

    void SendPacket(Cap& c, const std::int16_t* samples)
    {
        unsigned char pkt[kPacketBytes];
        pkt[0] = (unsigned char)(kMagic & 0xFF);
        pkt[1] = (unsigned char)(kMagic >> 8);
        pkt[2] = kVersion;
        pkt[3] = (unsigned char)c.stream;
        Put32(pkt + 4, self);
        const std::uint32_t seq = c.seq++;
        Put32(pkt + 8, seq);

        auto& dests = c.dests;
        dests.clear();
        {
            std::lock_guard<std::mutex> lk(mtx);
            for (auto& kv : peers)
                if (kv.second.send[c.stream] && kv.second.keyed) {          // only devices we share with
                    Dest d;
                    d.ep = kv.second.ep;
                    std::memcpy(d.key, kv.second.txKey, 32);
                    dests.push_back(d);
                }
        }
        std::uint8_t nonce[24];
        MakeNonce(c.stream, seq, nonce);
        for (auto& d : dests) {
            // every peer has its own key, so the ciphertext is made per destination
            crypto_aead_lock(pkt + kHeaderBytes, pkt + kHeaderBytes + kPayloadBytes, d.key, nonce,
                             pkt, kHeaderBytes, reinterpret_cast<const std::uint8_t*>(samples), kPayloadBytes);
            error_code ec;
            sendSock.send_to(asio::buffer(pkt, sizeof pkt), d.ep, 0, ec);   // lost packets are fine
        }
        for (auto& d : dests) crypto_wipe(d.key, 32);
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
        if (n != kPacketBytes) return;
        const unsigned char* b = rx.data();
        if (b[0] != (kMagic & 0xFF) || b[1] != (kMagic >> 8) || b[2] != kVersion) return;
        const int stream = b[3];
        if (stream < 0 || stream >= kStreams) return;

        const std::uint32_t inst = Get32(b + 4);
        const std::uint32_t sq   = Get32(b + 8);

        std::lock_guard<std::mutex> lk(mtx);
        auto it = peers.find(inst);
        if (it == peers.end() || !it->second.keyed) return;   // only audio from connected, keyed peers
        Peer& pe = it->second;

        // Authenticate and decrypt BEFORE touching any state: a forged packet changes nothing.
        std::int16_t samples[kPacketSamples];
        std::uint8_t nonce[24];
        MakeNonce(stream, sq, nonce);
        if (crypto_aead_unlock(reinterpret_cast<std::uint8_t*>(samples), b + kHeaderBytes + kPayloadBytes,
                               pe.rxKey, nonce, b, kHeaderBytes, b + kHeaderBytes, kPayloadBytes) != 0)
            return;
        int peak = 0;
        for (std::size_t i = 0; i < kPacketSamples; ++i) {
            const int a = samples[i] < 0 ? -samples[i] : samples[i];
            if (a > peak) peak = a;
        }
        Stream& p = pe.in[stream];

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

        pe.bytes += n;
        pe.lastPkt = Clock::now();
        p.level = std::max(peak / 32768.0f, p.level * 0.97f);
    }

    // ------------------------------------------------------------------ playback / mix
    static void PlaybackCb(ma_device* dev, void* out, const void*, ma_uint32 frames)
    {
        static_cast<Impl*>(dev->pUserData)->Mix(static_cast<float*>(out), frames);
    }

    // Adds one incoming stream to the output buffer (n samples) and drains it.
    static void Drain(Stream& p, float* o, std::size_t n, float vol)
    {
        if (!p.primed) {
            if (p.count >= kTargetSamples) p.primed = true;
            else return;
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

    // Speakers: the SYSTEM audio of the other PCs. Their microphones never come out here, they go to the
    // virtual microphone (MixMic).
    void Mix(float* o, ma_uint32 frames)
    {
        const std::size_t n = (std::size_t)frames * kChannels;
        std::memset(o, 0, n * sizeof(float));
        if (muted.load()) return;
        const float master = volume.load() * (1.0f / 32768.0f);

        {
            std::lock_guard<std::mutex> lk(mtx);
            for (auto& kv : peers) {
                Peer& pe = kv.second;
                const float vol = pe.muted ? 0.0f : master * pe.vol;   // a muted device is still drained: no stale audio on unmute
                Drain(pe.in[1], o, n, vol);
            }
        }
        for (std::size_t i = 0; i < n; ++i) o[i] = std::clamp(o[i], -1.0f, 1.0f);
    }

    static void VMicCb(ma_device* dev, void* out, const void*, ma_uint32 frames)
    {
        static_cast<Impl*>(dev->pUserData)->MixMic(static_cast<float*>(out), frames);
    }

    // Virtual microphone: the microphones of the other PCs (per-device mute and volume apply, the master
    // output does not: it is not something this PC listens to).
    void MixMic(float* o, ma_uint32 frames)
    {
        const std::size_t n = (std::size_t)frames * kChannels;
        std::memset(o, 0, n * sizeof(float));
        {
            std::lock_guard<std::mutex> lk(mtx);
            for (auto& kv : peers) {
                Peer& pe = kv.second;
                Drain(pe.in[0], o, n, pe.muted ? 0.0f : pe.vol * (1.0f / 32768.0f));
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
            const float lvl = std::max(p.in[0].level, p.in[1].level);
            out.push_back({kv.first, p.ip, recv ? p.kbps : 0.0, recv ? lvl : 0.0f, recv});
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
void AudioEngine::SetPeerKeys(std::uint32_t i, const std::uint8_t tx[32], const std::uint8_t rx[32]) { d_->SetPeerKeys(i, tx, rx); }
void AudioEngine::RemovePeer(std::uint32_t i)             { d_->RemovePeer(i); }
bool AudioEngine::SetCapture(AudioSource s, bool on, std::string* e) { return d_->SetCapture(s, on, e); }
bool AudioEngine::EnableVirtualMic(std::string* info)     { return d_->EnableVirtualMic(info); }
bool AudioEngine::VirtualMicActive() const                { return d_->vmOn; }
void AudioEngine::SetPeerSend(std::uint32_t i, AudioSource s, bool on) { d_->SetPeerSend(i, s, on); }
void AudioEngine::SetPeerMuted(std::uint32_t i, bool m)   { d_->SetPeerMuted(i, m); }
void AudioEngine::SetPeerVolume(std::uint32_t i, float v) { d_->SetPeerVolume(i, v); }
void AudioEngine::SetMuted(bool m)                        { d_->muted = m; }
void AudioEngine::SetVolume(float v)                      { d_->volume = std::clamp(v, 0.0f, 1.5f); }
std::vector<AudioPeerStat> AudioEngine::Stats()           { return d_->Stats(); }
float AudioEngine::InputLevel() const                     { return std::max(d_->cap[0].level.load(), d_->cap[1].level.load()); }

bool AudioEngine::SystemAudioSupported()
{
#if defined(_WIN32) || defined(__linux__)
    return true;      // WASAPI loopback / PulseAudio-PipeWire monitor source
#else
    return false;
#endif
}
