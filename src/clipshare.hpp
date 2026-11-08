#pragma once
// Clipboard sharing between PCs: text and files (single or several), across operating systems.
//
// A PC whose cursor just moved over to a peer calls SendTo(): it reads its own clipboard and, if
// there is something new, streams it to the peer over the encrypted control connection. The peer
// stores received files in its own "incoming" folder (names made valid for ITS operating system) and
// puts them on its clipboard as "copied", so a normal paste in the file manager copies them.
//
// Wire protocol (inner lines; the Session encrypts every line):
//   CLIPO <xfer> <items> <total bytes>           offer                       sender -> receiver
//   CLIPA <xfer>                                 accepted                    receiver -> sender
//   CLIPN <xfer> LIMIT <bytes>|DENIED|DISK|ERROR refused                     receiver -> sender
//   CLIPI <xfer> <idx> <T|F> <size> <b64 name>   next item (text or file)    sender -> receiver
//   CLIPD <xfer> <idx> <seq> <b64 data>          up to 32 KiB of the item    sender -> receiver
//   CLIPK <xfer> <seq>                           received up to seq          receiver -> sender (flow control)
//   CLIPE <xfer> <idx>                           item complete               sender -> receiver
//   CLIPF <xfer>                                 everything sent             sender -> receiver
//   CLIPX <xfer>                                 abort                       either way
// The sender never has more than 64 chunks (2 MiB) in flight, so input events on the same
// connection are delayed by milliseconds at most and memory use stays small.
//
// Threading: SendTo / HandleLine / PeerLost / Forget* are called from ONE thread (the Core loop).
// The hooks may be called from any thread.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

class ClipShare {
public:
    static constexpr std::uint64_t kDefaultLimit = 1ull << 30;     // 1 GiB
    static constexpr std::uint64_t kMinLimit     = 1ull << 10;     // 1 KiB
    static constexpr std::uint64_t kMaxLimit     = 1ull << 40;     // 1 TiB

    struct Hooks {
        std::function<void(int peer, const std::string& line)> send;   // any thread
        std::function<void(const std::string& message)> log;           // any thread
        // Clipboard access. Empty = the system clipboard (clipboard.hpp); tests replace them.
        std::function<bool(std::vector<std::filesystem::path>&)> getFiles;
        std::function<bool(std::string&, std::size_t)> getText;
        std::function<bool(const std::vector<std::filesystem::path>&)> setFiles;
        std::function<bool(const std::string&)> setText;
    };

    ClipShare(std::filesystem::path incomingDir, Hooks hooks);
    ~ClipShare();

    void SetLimit(std::uint64_t bytes);                // clamped to [kMinLimit, kMaxLimit]
    std::uint64_t Limit() const { return limit_.load(); }

    // The cursor moved over to 'peer': send the local clipboard if it is new to that peer.
    void SendTo(int peer, const std::string& peerName);

    // Handles every line starting with "CLIP". 'mayWrite': this peer may put data on our clipboard.
    // Returns true when the line was a clipboard line (even a malformed one).
    bool HandleLine(int peer, const std::string& peerName, bool mayWrite, const std::string& line);

    void PeerLost(int peer);                           // connection gone: stop and clean up
    void ForgetSent(int peer);                         // the peer's clipboard is unknown again
    void ForgetAll();
    void Shutdown();                                   // stops and joins every transfer

    static std::string FormatBytes(std::uint64_t bytes);   // "1.50 GiB"

private:
    struct Item {
        bool file = false;
        std::string name;
        std::filesystem::path path;
        std::string text;
        std::uint64_t size = 0;
    };
    struct Out {
        int peer = 0;
        std::uint64_t xfer = 0;
        std::string peerName;
        std::vector<Item> items;
        std::uint64_t total = 0;
        std::mutex m;
        std::condition_variable cv;
        int state = 0;                   // 0 waiting for the answer, 1 accepted, 2 refused / aborted
        std::string reason;
        std::int64_t acked = -1;
        std::atomic<bool> cancel{false};
        std::atomic<bool> done{false};
        std::thread th;
    };
    struct In {
        std::uint64_t xfer = 0;
        std::uint64_t total = 0, declared = 0;
        std::uint32_t nItems = 0, nextItem = 0;
        bool open = false, openFile = false;
        std::uint64_t itemSize = 0, itemGot = 0;
        std::int64_t nextSeq = 0;
        std::ofstream f;
        std::filesystem::path dir, cur;
        std::vector<std::filesystem::path> files;
        std::string text;
        bool anyText = false;
        std::set<std::string> used;
        std::uint64_t got = 0;
        std::string peerName;
    };
    enum class Prep { Nothing, Ready, TooBig };

    Prep Prepare(std::vector<Item>& items, std::uint64_t& total, std::string& sig, std::string& info);
    void RunOut(std::shared_ptr<Out> o);

    void OnOffer(int peer, const std::string& peerName, bool mayWrite, std::uint64_t xfer, std::istream& is);
    void OnItem(int peer, std::uint64_t xfer, std::istream& is);
    void OnData(int peer, std::uint64_t xfer, std::istream& is);
    void OnEnd(int peer, std::uint64_t xfer, std::istream& is);
    void OnFinish(int peer, std::uint64_t xfer);
    void AbortIn(int peer, bool notify, const std::string& why = std::string());
    void CleanupOldIncoming(const std::filesystem::path& keep);

    void Send(int peer, const std::string& line) { if (h_.send) h_.send(peer, line); }
    void Log(const std::string& s) { if (h_.log) h_.log(s); }

    std::filesystem::path incoming_;
    Hooks h_;
    std::atomic<std::uint64_t> limit_{kDefaultLimit};
    std::atomic<std::uint64_t> nextXfer_;

    std::mutex mtx_;                                   // out_, sentSig_
    std::map<int, std::shared_ptr<Out>> out_;
    std::map<int, std::string> sentSig_;
    std::map<int, std::unique_ptr<In>> in_;            // Core thread only
};
