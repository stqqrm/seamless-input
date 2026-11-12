#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "input/motion.hpp"

inline constexpr unsigned short kInputUdpPort = 45457;   // UDP

// Carries pointer motion and wheel between two connected PCs over UDP (everything else stays on TCP).
// UDP so that a lost packet never holds up the ones behind it (Wi-Fi), with the same safety as the TCP link:
//  - the keys come from the TCP handshake of this very connection (derived from the audio keys with their own
//    label, so they are never equal to the TCP or the audio keys) and change with every connection;
//  - every datagram is sealed with XChaCha20-Poly1305, its 16-byte header (sender, sequence number) is
//    authenticated; the nonce is the sequence number, which only ever grows, older ones are dropped;
//  - datagrams are only accepted from a peer that is connected over TCP and has keys.
// Each side also sends a small probe every quarter of a second saying whether it hears the other side. UDP is
// "usable" towards a peer only while that peer confirms it receives ours; otherwise everything goes over TCP.
class InputUdp {
public:
    // port: the UDP port to listen on (and, by default, the port of the peers). Only tests use another one.
    explicit InputUdp(std::uint32_t myInstance, unsigned short port = kInputUdpPort);
    ~InputUdp();
    InputUdp(const InputUdp&) = delete;
    InputUdp& operator=(const InputUdp&) = delete;

    bool Start(std::string* err);       // false: UDP cannot be used (the sockets), TCP carries everything
    void Stop();

    // Peers come from the TCP mesh. 'tx'/'rx' are the per-connection audio keys (PeerAudioKeys).
    void AddPeer(int id, std::uint32_t instance, const std::string& ip, const std::uint8_t tx[32], const std::uint8_t rx[32],
                 unsigned short peerPort = 0);   // 0: the port this side listens on
    void RemovePeer(int id);

    bool Usable(int id) const;          // may motion go to this peer over UDP right now?
    bool Send(int id, std::uint64_t seq, const MotionPacket& p);   // false: not sent

    std::function<void(int id, std::uint64_t seq, const MotionPacket&)> onMotion;   // io thread
    std::function<void(const std::string&)> onLog;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
