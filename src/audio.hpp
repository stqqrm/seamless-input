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

    // Start/stop capturing one source on its default device. The two sources run
    // independently and are sent as separate streams, so a device can get the
    // microphone, the system audio, or both. Microphone: the default input device.
    // System audio: Windows (WASAPI loopback of the default output) and Linux
    // (PulseAudio/PipeWire monitor of the default output).
    bool SetCapture(AudioSource src, bool on, std::string* err);

    // Per device (peer). 'send' decides whether OUR microphone / system audio goes
    // to that device at all; mute / volume only affect what we hear FROM it.
    void SetPeerSend(std::uint32_t instance, AudioSource src, bool on);
    void SetPeerMuted(std::uint32_t instance, bool muted);
    void SetPeerVolume(std::uint32_t instance, float volume);   // 0..1.5

    void SetMuted(bool muted);       // master: mute everything received
    void SetVolume(float volume);    // master: 0..1.5, applied to the mixed output

    std::vector<AudioPeerStat> Stats();
    float InputLevel() const;        // 0..1 peak of what we are sending (loudest source)

    static bool SystemAudioSupported();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
