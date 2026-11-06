#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "identity.hpp"

inline constexpr unsigned short kTcpPort       = 45454;
inline constexpr unsigned short kDiscoveryPort = 45455;

// What a discovery beacon told us about another seamless-input instance.
struct BeaconInfo {
    std::string ip, host;
    std::string key;                // its PUBLIC KEY (not yet proven: the TCP handshake does that)
    std::uint32_t instance = 0;
    bool server = false;
    bool sameGroup = false;         // it knows our group password
};

// All callbacks are invoked on a non-GUI thread.
struct NetCallbacks {
    std::function<void(const std::string& msg)> onStatus;
    // 'instance' is the peer's per-run id (also used to identify its audio stream).
    // 'key' is the peer's PUBLIC KEY (64 hex chars); the handshake has already proven
    // that the peer owns the matching private key, so it is safe to use as identity.
    // 'viaGroup': accepted because it knows our group password (otherwise: mutual trust).
    std::function<void(int id, std::uint32_t instance, const std::string& key,
                       const std::string& name, const std::string& addr, bool incoming,
                       bool viaGroup)> onPeerReady;
    std::function<void(int id)> onPeerLost;
    // A peer proved its key but is neither in our group nor mutually trusted (yet). It stays
    // connected so the trust handshake can finish, but carries no input/audio.
    // 'theyTrustMe': it has already sent us a signed "I trust you". Called again on changes.
    std::function<void(int id, std::uint32_t instance, const std::string& key,
                       const std::string& name, const std::string& addr, bool incoming,
                       bool theyTrustMe)> onPeerPending;
    std::function<void(int id)> onPeerPendingLost;
    std::function<void(int id, const std::string& line)> onMessage;
    // UDP discovery beacon from another instance on the LAN.
    std::function<void(const BeaconInfo&)> onBeacon;
    // Subnet scan results.
    std::function<void(const std::string& ip, bool serverRunning)> onScanHost;
    std::function<void(const std::string& ip, const std::string& name)> onHostName;
    std::function<void()> onScanDone;
};

class Network {
public:
    Network(std::string hostName, Identity identity, NetCallbacks cb);
    ~Network();
    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    void Start();
    void Stop();

    std::uint32_t Instance() const;   // this process' unique id

    // Own address: start the server. Otherwise connect; if nobody listens, start our own server.
    void Select(const std::string& ip, bool isLocalAddress);
    void Send(int id, const std::string& line);

    // Group password: instances with the same password connect to each other automatically.
    // Empty = no group.
    void SetGroup(const std::string& password);
    // Trust (or stop trusting) the PC with this public key. Trusting makes us connect to it and
    // send it a signed "I trust you"; the link carries input/audio once BOTH sides trust each
    // other (or share the group password). 'ip' is used when no connection exists yet.
    void SetTrust(const std::string& key, bool trusted, const std::string& ip);

    // Scan the subnets of the given (local ip, prefix length) pairs.
    void Scan(const std::vector<std::pair<std::string, int>>& nets);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
