#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

inline constexpr unsigned short kAudioPort = 45456;   // UDP

enum class AudioSource { Microphone, System };

struct AudioPeerStat {
    std::uint32_t instance;
    std::string ip;
    double kbps;       // incoming data rate
    float level;       // 0..1 recent peak of the incoming audio
    bool receiving;    // packets arrived within the last second
};

// Sends this PC's audio to all registered peers (UDP) and mixes the audio
// received from them into the default playback device.
class AudioEngine {
public:
    explicit AudioEngine(std::uint32_t myInstance);
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Opens the UDP sockets and the playback device. On failure returns false
    // and describes what went wrong (the engine keeps whatever part works).
    bool Start(std::string* err);
    void Stop();

    // Peers come from the TCP mesh: audio is only accepted from / sent to these.
    void AddPeer(std::uint32_t instance, const std::string& ip);
    void RemovePeer(std::uint32_t instance);

    // Start/stop capturing and sending. Microphone works everywhere; System
    // (WASAPI loopback) only on Windows.
    bool SetSending(bool on, AudioSource src, std::string* err);

    void SetMuted(bool muted);       // mute everything received
    void SetVolume(float volume);    // 0..1.5, applied to the mixed output

    std::vector<AudioPeerStat> Stats();
    float InputLevel() const;        // 0..1 peak of what we are sending

    static bool SystemAudioSupported();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
