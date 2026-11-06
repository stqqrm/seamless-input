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

// All callbacks are invoked on a non-GUI thread.
struct NetCallbacks {
    std::function<void(const std::string& msg)> onStatus;
    // 'instance' is the peer's per-run id (also used to identify its audio stream).
    // 'key' is the peer's PUBLIC KEY (64 hex chars); the handshake has already proven
    // that the peer owns the matching private key, so it is safe to use as identity.
    std::function<void(int id, std::uint32_t instance, const std::string& key,
                       const std::string& name, const std::string& addr, bool incoming)> onPeerReady;
    std::function<void(int id)> onPeerLost;
    std::function<void(int id, const std::string& line)> onMessage;
    // UDP discovery beacon from another instance on the LAN.
    std::function<void(const std::string& ip, const std::string& host, bool serverRunning)> onBeacon;
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

    // Scan the subnets of the given (local ip, prefix length) pairs.
    void Scan(const std::vector<std::pair<std::string, int>>& nets);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
