#pragma once
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

inline constexpr unsigned short kTcpPort       = 45454;
inline constexpr unsigned short kDiscoveryPort = 45455;

// All callbacks are invoked on a non-GUI thread.
struct NetCallbacks {
    std::function<void(const std::string& msg)> onStatus;
    std::function<void(int id, const std::string& name, const std::string& addr, bool incoming)> onPeerReady;
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
    Network(std::string hostName, NetCallbacks cb);
    ~Network();
    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    void Start();
    void Stop();

    // Own address: start the server. Otherwise connect; if nobody listens, start our own server.
    void Select(const std::string& ip, bool isLocalAddress);
    void Send(int id, const std::string& line);

    // Scan the subnets of the given (local ip, prefix length) pairs.
    void Scan(const std::vector<std::pair<std::string, int>>& nets);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
