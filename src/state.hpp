#pragma once
// Plain data that travels between the daemon (Core) and the GUI. No wx, no asio.
#include <cstdint>
#include <string>
#include <vector>

#include "input/input_types.hpp"

namespace state {

struct Peer {
    int id = 0;
    std::uint32_t inst = 0;
    std::string key, name, addr;
    bool incoming = false;
    bool placed = false;                 // its SCREENS line arrived: it has a place in the layout
    double ox = 0, oy = 0;               // top-left of its desktop relative to this PC's top-left
    std::vector<InputRect> mons;
    bool share = false;                  // send my audio (system audio) to this device
    bool mic = false;                    // send my microphone to this device
    bool mute = false;                   // mute what it sends me
    int  vol = 100;                      // 0..100, what I hear from it
    bool allowed = false;                // may it control this PC? (saved per device, off by default)
    bool clip = false;                   // share my clipboard with it when the cursor moves over to it
};

struct Beacon {                          // another instance seen on the LAN (UDP discovery)
    std::string ip, host, key;
    bool server = false;
    bool sameGroup = false;
};

struct Scanned {                         // a host found by the subnet scan
    std::string ip, name;
    bool server = false;
};

struct Snapshot {
    std::string host, pubKey, dataDir, identNote, backend, monsSource;
    std::string group;                   // group password ("" = none)
    std::vector<InputRect> mons;         // this PC's monitors
    bool masterMute = false;
    int  masterVol = 100;
    bool sysCapturing = false, micCapturing = false;
    bool scanning = false;
    int  inputMode = 0;                  // 0 local, 1 forwarding (I control a peer), 2 controlled
    int  inputPeer = -1;
    std::vector<Peer> peers;
    std::vector<Beacon> disc;
    std::vector<Scanned> scan;
};

struct PeerAudioStat {
    std::uint32_t inst = 0;
    bool receiving = false;
    double kbps = 0;
    float level = 0;
};

struct Stats {
    float input = 0;
    std::vector<PeerAudioStat> peers;
};

} // namespace state
