#pragma once
// Local IPC between the GUI (client) and the daemon (server): a line protocol over TCP on
// 127.0.0.1, authenticated by a random token kept in the user's data dir (owner-only file).
//
// daemon -> GUI                                  GUI -> daemon
//   HELLO <proto> <pid> <version>                  AUTH <token>
//   SB / H / M / G / A / I / P / D / C / SE   ATTACH
//     (one full state snapshot)                    MONITORS <n> <x y w h>...
//   L <text>            activity log line          SCAN | SELECT <ip> | GROUP <pwd>
//   ST <...>            audio levels (4 Hz)        MOVE <id> <x> <y> | AUDIO <id> <s> <m> <mute> <vol>
//                                                  MASTER <mute> <vol> | CONTROL <id> <0|1>
//                                                  STOP | PING
// Text fields are percent-encoded (see Enc); the empty string is "~".
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "state.hpp"

namespace ipc {

inline constexpr unsigned short kPort = 45457;
inline constexpr int kProtocol = 1;

std::string Enc(const std::string& s);
std::string Dec(const std::string& s);
std::vector<std::string> Tokens(const std::string& line);
long long ToInt(const std::string& s, long long def = 0);

std::vector<std::string> SerializeSnapshot(const state::Snapshot& s);   // "SB" ... "SE"
bool ParseSnapshotLine(const std::vector<std::string>& t, state::Snapshot& s);
std::string SerializeStats(const state::Stats& st);
bool ParseStats(const std::vector<std::string>& t, state::Stats& st);

std::filesystem::path TokenPath(const std::filesystem::path& dataDir);
std::string ReadToken(const std::filesystem::path& dataDir);

// Is a daemon of this user answering (and does it accept our token)?
bool Ping(const std::filesystem::path& dataDir, int timeoutMs = 700);
// Asks the daemon to shut down. false if none answered.
bool RequestStop(const std::filesystem::path& dataDir);

class Client {
public:
    struct Handlers {                    // called on the client's reader thread
        std::function<void(const state::Snapshot&)> onSnapshot;
        std::function<void(const std::string& line)> onLog;
        std::function<void(const state::Stats&)> onStats;
        std::function<void()> onDisconnected;
    };

    explicit Client(Handlers h);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Blocking: connect, authenticate, start the reader thread. 'attach' = this is a window
    // (the daemon then streams state and may rescan the LAN periodically).
    bool Connect(const std::filesystem::path& dataDir, bool attach, std::string* err, int timeoutMs = 1500);
    void Close();
    bool Connected() const;
    void Send(const std::string& line);      // any thread

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace ipc
