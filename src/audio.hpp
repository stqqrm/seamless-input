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

// Sends this PC's audio to all registered peers (UDP) and mixes the system audio
// received from them into the default playback device. Audio is only sent while it is audible
// (silence is not transmitted). Received microphones go to a virtual microphone.
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

    // Per-connection keys (see PeerAudioKeys in network.hpp). Audio is encrypted and authenticated with
    // them; a peer without keys gets no audio and none is accepted from it.
    void SetPeerKeys(std::uint32_t instance, const std::uint8_t tx[32], const std::uint8_t rx[32]);

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

    // The microphone of the OTHER PCs is not played through the speakers: it is forwarded to a virtual
    // microphone that applications on this PC can select as an input device.
    //   Linux    a PulseAudio/PipeWire virtual source "Seamless Input Microphone" (needs pactl)
    //   Windows  plays into a virtual audio cable ("CABLE Input" of VB-Cable); applications use "CABLE Output"
    // Idempotent. true: *info is the name applications will see. false: *info says why it is not available
    // (until it works, microphones received from other PCs are silently dropped).
    bool EnableVirtualMic(std::string* info);
    bool VirtualMicActive() const;

    static bool SystemAudioSupported();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
