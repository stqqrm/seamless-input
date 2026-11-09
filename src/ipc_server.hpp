#pragma once
// IPC server of the daemon (see ipc.hpp). Runs on Core's thread.
#include <filesystem>
#include <memory>
#include <set>
#include <string>

#include "core.hpp"

namespace ipc {

class Session;

class Server {
public:
    Server(Core& core, std::filesystem::path dataDir);
    ~Server();

    // Binds 127.0.0.1:kPort and writes the token file. Doing this FIRST makes the port the
    // single-instance lock: false + AddressInUse() means another daemon is already running.
    bool Start(std::string* err);
    bool AddressInUse() const { return inUse_; }
    void Stop();     // call while Core is still running

private:
    friend class Session;
    void Accept();
    void Pump();
    void BroadcastSnapshot();
    void BroadcastStats();
    void BroadcastClipActivity();
    void Attach(const std::shared_ptr<Session>& s);
    void Remove(const std::shared_ptr<Session>& s);
    int AttachedCount() const;

    Core& core_;
    std::filesystem::path dir_;
    std::string token_;
    bool inUse_ = false;
    bool started_ = false;
    bool dirty_ = false;
    int pumpTicks_ = 0;
    bool telemetryShown_ = false;        // the last CT line sent had transfers in it
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::steady_timer pump_;
    std::set<std::shared_ptr<Session>> sessions_;
    bool stopped_ = false;
};

} // namespace ipc
