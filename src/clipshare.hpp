#pragma once
// Clipboard sharing between PCs: text, files and whole folders, across operating systems.
//
// A PC whose cursor just moved over to a peer calls SendTo(): it reads its own clipboard and, if there
// is something there, streams it to the peer over the encrypted control connection. The peer stores
// what it receives in its own "incoming" folder (names made valid for ITS operating system) and puts
// it on its clipboard as "copied", so a normal paste in the file manager copies it.
//
// Folders: the sender walks every folder on the clipboard (never following symbolic links to folders),
// adding up the sizes while it goes. The walk stops as soon as the limit (Network tab) or the item
// limit is passed, and the reason is written to the activity log. Empty folders are kept.
// Several peers can be served at the same time; the folder walk is done once and shared.
//
// Wire protocol (inner lines; the Session encrypts every line):
//   CLIPO <xfer> <items> <total bytes> <sig>     offer                       sender -> receiver
//   CLIPA <xfer>                                 accepted                    receiver -> sender
//   CLIPS <xfer>                                 "my clipboard already holds exactly this"
//   CLIPN <xfer> LIMIT <bytes>|DENIED|DISK|ERROR refused                     receiver -> sender
//   CLIPI <xfer> <idx> <T|F|D> <size> <b64 name> next item: text, file or folder   sender -> receiver
//         (name = path relative to the clipboard root, '/' separated; a folder is always sent
//          before the things inside it)
//   CLIPD <xfer> <idx> <seq> <b64 data>          up to 32 KiB of the item    sender -> receiver
//   CLIPK <xfer> <seq> <items done>              received up to seq          receiver -> sender (flow control)
//   CLIPE <xfer> <idx>                           item complete               sender -> receiver
//   CLIPF <xfer>                                 everything sent             sender -> receiver
//   CLIPR <xfer>                                 it is on my clipboard now   receiver -> sender (confirmation)
//   CLIPX <xfer>                                 abort / could not use it    either way
// The sender never has more than 64 chunks (2 MiB) or 256 items in flight, so input events on the same
// connection are delayed by milliseconds at most and memory use stays small.
//
// Threading: SendTo / HandleLine / PeerLost / Forget* are called from ONE thread (the Core loop).
// The hooks may be called from any thread. Every outgoing transfer has its own worker thread.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

class ClipShare {
public:
    // Bump when the clipboard wire protocol changes. Shown in the startup log so an old daemon that is
    // still running (it keeps running after an update) can be told apart from the new one.
    static constexpr int kProtocol = 4;
    static constexpr std::uint64_t kDefaultLimit = 1ull << 30;     // 1 GiB
    static constexpr std::uint64_t kMinLimit     = 1ull << 10;     // 1 KiB
    static constexpr std::uint64_t kMaxLimit     = 1ull << 40;     // 1 TiB
    static constexpr std::uint32_t kMaxItems     = 100000;         // files + folders in one transfer

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

    // The cursor moved over to 'peer': send the local clipboard to it (the peer says "already have it"
    // when its own clipboard still holds exactly this).
    void SendTo(int peer, const std::string& peerName);

    // Handles every line starting with "CLIP". 'mayWrite': this peer may put data on our clipboard.
    // Returns true when the line was a clipboard line (even a malformed one).
    bool HandleLine(int peer, const std::string& peerName, bool mayWrite, const std::string& line);

    void PeerLost(int peer);                           // connection gone: stop and clean up
    // The RECEIVER decides whether a transfer is needed, so there is nothing to forget on the sending
    // side any more. Kept so that callers do not change.
    void ForgetSent(int) {}
    void ForgetAll() {}
    void Shutdown();                                   // stops and joins every transfer

    static std::string FormatBytes(std::uint64_t bytes);   // "1.50 GiB"

private:
    enum class Kind { Text, File, Dir };
    enum class Status { Ready, Nothing, TooBig, TooMany, Cancelled };

    struct Item {
        Kind kind = Kind::File;
        std::string rel;                 // name relative to the clipboard root, '/' separated
        std::filesystem::path path;      // on this PC (files only)
        std::string text;
        std::uint64_t size = 0;
    };
    // What was on the clipboard (read on the Core thread, cheap).
    struct Raw {
        bool files = false;
        std::vector<std::filesystem::path> paths;
        std::string text;
        std::string key;                 // identifies this clipboard (for sharing one folder walk)
    };
    // The result of walking the clipboard (done on a worker thread, shared by all peers).
    struct Plan {
        Status status = Status::Nothing;
        std::vector<Item> items;
        std::uint64_t total = 0;
        std::string sig;                 // what is being sent (see AlreadyHas)
        std::string what;                // "3 files, 2 folders" / "text"
        std::string message;             // why it is not sent (TooBig / TooMany / Nothing)
        std::string note;                // logged once: skipped links, unreadable parts, ...
    };
    using PlanPtr = std::shared_ptr<const Plan>;

    struct Out {
        int peer = 0;
        std::uint64_t xfer = 0;
        std::string peerName;
        Raw raw;
        std::atomic<bool> ok{false};     // everything was sent
        std::atomic<bool> delivered{false};   // the other PC confirmed (CLIPR)
        std::mutex m;
        std::condition_variable cv;
        int state = 0;                   // 0 waiting for the answer, 1 accepted, 2 refused / aborted, 3 receiver already has it
        std::string reason;
        std::int64_t acked = -1;         // highest chunk the receiver has
        std::int64_t itemsAcked = 0;     // items the receiver has completed
        std::atomic<bool> cancel{false};
        std::atomic<bool> done{false};
        std::thread th;
    };
    struct In {
        std::uint64_t xfer = 0;
        std::uint64_t total = 0, declared = 0;
        std::uint32_t nItems = 0, nextItem = 0;
        bool open = false;
        Kind cur = Kind::File;
        std::uint64_t itemSize = 0, itemGot = 0;
        std::int64_t nextSeq = 0;
        std::ofstream f;
        std::filesystem::path dir, curPath;
        std::vector<std::filesystem::path> tops;                    // top level files / folders: what goes on the clipboard
        std::map<std::string, std::string> dirMap;                  // sender's folder name -> ours (both relative)
        std::map<std::string, std::set<std::string>> used;          // our folder -> names taken (lower case)
        std::string text;
        bool anyText = false;
        std::uint64_t got = 0;
        std::uint32_t nFiles = 0, nDirs = 0;
        std::string peerName;
        std::string sig;                 // the sender's signature of this clipboard (see AlreadyHas)
    };
    struct Got { std::string sig; std::vector<std::filesystem::path> files; };   // what we last put on the clipboard
    struct ScanEntry {
        std::string key;
        std::shared_future<PlanPtr> fut;
        std::chrono::steady_clock::time_point at;
        bool finished = false;
    };

    bool ReadClipboard(Raw& raw, std::string& info);               // Core thread
    PlanPtr GetPlan(Out& o);                                       // worker thread: walks once, shared
    PlanPtr Scan(const Raw& raw, std::uint64_t limit, const std::atomic<bool>& cancel);
    void RunOut(std::shared_ptr<Out> o);

    void OnOffer(int peer, const std::string& peerName, bool mayWrite, std::uint64_t xfer, std::istream& is);
    void OnItem(int peer, std::uint64_t xfer, std::istream& is);
    void OnData(int peer, std::uint64_t xfer, std::istream& is);
    void OnEnd(int peer, std::uint64_t xfer, std::istream& is);
    void OnFinish(int peer, std::uint64_t xfer);
    void AbortIn(int peer, bool notify, const std::string& why = std::string());
    void CleanupOldIncoming(const std::filesystem::path& keep);
    // The RECEIVER decides whether a transfer is needed: it looks at its own clipboard. (A sender that
    // remembers "already sent" is wrong as soon as the other PC's clipboard changed in the meantime.)
    bool AlreadyHas(int peer, const std::string& sig);

    void Send(int peer, const std::string& line) { if (h_.send) h_.send(peer, line); }
    void Log(const std::string& s) { if (h_.log) h_.log(s); }

    std::filesystem::path incoming_;
    Hooks h_;
    std::atomic<std::uint64_t> limit_{kDefaultLimit};
    std::atomic<std::uint64_t> nextXfer_;
    std::uint64_t dirSeq_ = 0;                         // Core thread only: short folder names keep paths short on Windows

    std::mutex mtx_;                                   // out_
    std::map<int, std::shared_ptr<Out>> out_;
    std::mutex scanMtx_;                               // scans_
    std::vector<ScanEntry> scans_;
    std::map<int, std::unique_ptr<In>> in_;            // Core thread only
    std::map<int, Got> got_;                           // Core thread only
};
