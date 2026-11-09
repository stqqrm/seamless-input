#pragma once
// Clipboard sharing between PCs: text, files and whole folders, across operating systems.
//
// A PC whose clipboard just changed (the user copied something) calls SendToAll(): it reads its own
// clipboard and streams it, at once, to every PC it shares its clipboard with, over the encrypted control
// connection. The peer stores what it receives in its own "incoming" folder (names made valid for ITS
// operating system) and puts it on its clipboard as "copied", so a normal paste in the file manager
// copies it.
//
// The newest copy wins everywhere. Every copy gets a stamp (milliseconds since 1970, never going back,
// plus the id of the PC that made it). A new copy on this PC cancels every upload and download that is
// still running here (the other PCs are told), and a PC that receives an offer with a newer stamp
// cancels its own uploads and downloads as well, then takes the newer clipboard. An offer older than the
// clipboard a PC already holds is refused (OLDER), so two PCs that copy at the same moment end up with
// the same clipboard instead of swapping.
//
// Folders: the sender walks every folder on the clipboard (never following symbolic links to folders),
// adding up the sizes while it goes. The walk stops as soon as the limit (Network tab) or the item
// limit is passed, and the reason is written to the activity log. Empty folders are kept.
// Several peers can be served at the same time; the folder walk is done once and shared.
//
// Wire protocol (inner lines; the Session encrypts every line):
//   CLIPO <xfer> <items> <total bytes> <sig> <stamp> <origin>
//                                                offer                       sender -> receiver
//   CLIPA <xfer>                                 accepted                    receiver -> sender
//   CLIPS <xfer>                                 "my clipboard already holds exactly this"
//   CLIPN <xfer> LIMIT <bytes>|DENIED|DISK|OLDER|ERROR
//                                                refused                     receiver -> sender
//   CLIPI <xfer> <idx> <T|F|D> <size> <b64 name> next item: text, file or folder   sender -> receiver
//         (name = path relative to the clipboard root, '/' separated; a folder is always sent
//          before the things inside it)
//   CLIPD <xfer> <idx> <seq> <b64 data>          up to 32 KiB of the item    sender -> receiver
//   CLIPK <xfer> <seq> <items done>              received up to seq          receiver -> sender (flow control)
//   CLIPE <xfer> <idx>                           item complete               sender -> receiver
//   CLIPF <xfer>                                 everything sent             sender -> receiver
//   CLIPR <xfer>                                 it is on my clipboard now   receiver -> sender (confirmation)
//   CLIPX <xfer>                                 abort / could not use it    either way
//                                                (also: "I copied something newer, stop")
// The sender never has more than 64 chunks (2 MiB) or 256 items in flight, so input events on the same
// connection are delayed by milliseconds at most and memory use stays small.
//
// Threading: SendToAll / HandleLine / PeerLost / Forget* / Activity are called from ONE thread (the Core loop).
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
    static constexpr int kProtocol = 5;
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
        // Called (Core thread) right after received data was put on this PC's clipboard, so the clipboard
        // watcher does not take that write for a copy made by the user.
        std::function<void()> applied;
    };

    struct Target { int peer = 0; std::string name; };

    // What the progress popup shows ("Clipboard telemetry" in the settings).
    struct Transfer {
        bool upload = false;             // false: download
        std::string peer;
        std::uint64_t done = 0, total = 0;   // bytes (items when the transfer has no bytes); total 0: size not known yet
        bool finished = false;           // just completed (shown for a moment at 100 %)
    };

    ClipShare(std::filesystem::path incomingDir, Hooks hooks);
    ~ClipShare();

    void SetLimit(std::uint64_t bytes);                // clamped to [kMinLimit, kMaxLimit]
    std::uint64_t Limit() const { return limit_.load(); }

    // This PC's id in the stamps of its copies (the network instance id); call once before use.
    void SetOrigin(std::uint32_t origin) { origin_ = origin; }

    // The user copied something on this PC. Cancels every upload and download that is running (this
    // clipboard is newer than all of them) and sends the new clipboard to 'targets' at once. The peer
    // says "already have it" when its own clipboard still holds exactly this. With no targets it only
    // does the cancelling.
    void SendToAll(const std::vector<Target>& targets);

    // Uploads and downloads that are running now, plus the ones that finished a moment ago.
    std::vector<Transfer> Activity();

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
        std::shared_ptr<const Raw> raw;        // shared by the uploads to all peers
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
        std::atomic<bool> superseded{false};   // stopped because a newer clipboard exists: tell the receiver (CLIPX)
        std::uint64_t stamp = 0;               // this copy's stamp (the same for every peer)
        std::chrono::steady_clock::time_point started{};
        // progress, for the popup
        std::atomic<int> tel{0};               // 0 not shown, 1 sending, 2 sent / waiting for the confirmation, 3 over
        std::atomic<std::uint64_t> sent{0}, totalUnits{0};
        std::atomic<long long> sentAtMs{0};    // steady clock, when tel became 2
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
        std::chrono::steady_clock::time_point started{};
    };
    struct Stamp {
        std::uint64_t t = 0;
        std::uint32_t origin = 0;
        bool operator<(const Stamp& o) const { return t != o.t ? t < o.t : origin < o.origin; }
    };
    struct Recent { bool upload; std::string peer; std::uint64_t total; std::chrono::steady_clock::time_point at; };
    struct Got { std::string sig; std::vector<std::filesystem::path> files; };   // what we last put on the clipboard
    struct ScanEntry {
        std::string key;
        std::shared_future<PlanPtr> fut;
        std::chrono::steady_clock::time_point at;
        bool finished = false;
    };

    bool ReadClipboard(Raw& raw, std::string& info, bool* echo = nullptr);   // Core thread
    PlanPtr GetPlan(Out& o);                                       // worker thread: walks once, shared
    PlanPtr Scan(const Raw& raw, std::uint64_t limit, const std::atomic<bool>& cancel);
    void RunOut(std::shared_ptr<Out> o);
    void StartOut(const Target& t, const Raw& raw, std::uint64_t stamp);
    // Stops every upload (a newer clipboard exists). Core thread.
    void CancelOuts(const std::string& why);
    // Drops every download; 'except' keeps the one from that peer (-1: none).
    void AbortAllIn(bool notify, const std::string& why, int except = -1);
    void ReapOld();                                                // joins finished cancelled uploads
    std::uint64_t NextStamp();
    void AddRecent(bool upload, const std::string& peer, std::uint64_t total);

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
    std::uint32_t origin_ = 0;
    std::uint64_t lastStamp_ = 0;                      // Core thread only
    Stamp current_;                                    // stamp of the clipboard this PC holds (Core thread only)
    std::string appliedSig_;                           // text we put on the clipboard ourselves (Core thread only)
    std::atomic<std::uint64_t> limit_{kDefaultLimit};
    std::atomic<std::uint64_t> nextXfer_;
    std::uint64_t dirSeq_ = 0;                         // Core thread only: short folder names keep paths short on Windows

    std::mutex mtx_;                                   // out_
    std::map<int, std::shared_ptr<Out>> out_;
    std::vector<std::shared_ptr<Out>> old_;            // cancelled uploads whose thread is still ending
    std::vector<Recent> recent_;
    std::mutex scanMtx_;                               // scans_
    std::vector<ScanEntry> scans_;
    std::map<int, std::unique_ptr<In>> in_;            // Core thread only
    std::map<int, Got> got_;                           // Core thread only
};
