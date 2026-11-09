#include "clipshare.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>

#ifndef _WIN32
  #include <unistd.h>
#endif

#include "clipboard.hpp"

namespace fs = std::filesystem;

namespace {

constexpr std::size_t   kChunk      = 32 * 1024;     // raw bytes per CLIPD line
constexpr std::int64_t  kWindow     = 64;            // chunks in flight (2 MiB)
constexpr std::int64_t  kItemWindow = 256;           // items in flight
constexpr int           kAckEvery   = 16;            // receiver acknowledges every 16th chunk ...
constexpr std::uint32_t kItemAck    = 64;            // ... and every 64th item
constexpr std::size_t   kNameBytes  = 200;           // longest single file / folder name we create
constexpr auto          kPlanTtl    = std::chrono::seconds(5);   // a finished folder walk may be shared this long
#ifdef _WIN32
constexpr std::size_t   kWinPath    = 240;           // MAX_PATH is 260; keep some room (counted in UTF-8 bytes: safe)
#endif

std::string U8(const fs::path& p)
{
    const auto s = p.u8string();                    // std::string (C++17) or std::u8string (C++20)
    return std::string(s.begin(), s.end());
}

std::string G8(const fs::path& p)                   // same with '/' separators
{
    const auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

fs::path FromU8(const std::string& s) { return fs::u8path(s); }

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string Plural(std::uint64_t n, const char* one, const char* many)
{
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string Describe(std::uint64_t files, std::uint64_t dirs)
{
    std::string s;
    if (files || !dirs) s = Plural(files, "file", "files");
    if (dirs) { if (!s.empty()) s += ", "; s += Plural(dirs, "folder", "folders"); }
    return s;
}

// Same value on every OS (std::hash is not), because the receiver recomputes it for text.
class Hasher {
public:
    void add(const std::string& s) { for (unsigned char c : s) step(c); step('|'); }
    void add(std::uint64_t v) { add(std::to_string(v)); }
    void add(long long v) { add(std::to_string(v)); }
    std::string hex() const
    {
        char b[24];
        std::snprintf(b, sizeof b, "%016llx", (unsigned long long)h_);
        return b;
    }
private:
    void step(unsigned char c) { h_ ^= c; h_ *= 1099511628211ull; }
    std::uint64_t h_ = 1469598103934665603ull;
};

std::string TextSig(const std::string& t)
{
    // raw bytes, no separator: identical to the old Fnv of the text
    std::uint64_t v = 1469598103934665603ull;
    for (unsigned char c : t) { v ^= c; v *= 1099511628211ull; }
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return std::string("T|") + b;
}

// true when 'p' is inside (or is) 'dir'
bool Inside(const fs::path& p, const fs::path& dir)
{
    std::error_code ec;
    const fs::path a = fs::weakly_canonical(p, ec);
    if (ec) return false;
    const fs::path b = fs::weakly_canonical(dir, ec);
    if (ec) return false;
    const fs::path rel = a.lexically_relative(b);
    return !rel.empty() && *rel.begin() != "..";
}

bool Readable(const fs::path& p)
{
#ifdef _WIN32
    (void)p;
    return true;
#else
    return ::access(p.c_str(), R_OK) == 0;
#endif
}

// Invalid UTF-8 (a Linux name in an old encoding) would make fs::u8path throw on Windows: replace it.
std::string ValidUtf8(const std::string& s)
{
    std::string o;
    o.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        std::size_t len = 0;
        if (c < 0x80) len = 1;
        else if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c >= 0xE0 && c <= 0xEF) len = 3;
        else if (c >= 0xF0 && c <= 0xF4) len = 4;
        bool ok = len != 0 && i + len <= s.size();
        for (std::size_t k = 1; ok && k < len; ++k) ok = ((unsigned char)s[i + k] & 0xC0) == 0x80;
        if (ok && len >= 3) {
            const unsigned char c1 = (unsigned char)s[i + 1];
            if (len == 3 && ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0))) ok = false;   // overlong / surrogate
            if (len == 4 && ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90))) ok = false;   // overlong / > U+10FFFF
        }
        if (ok) { o.append(s, i, len); i += len; }
        else    { o += '_'; ++i; }
    }
    return o;
}

// at most maxBytes, keeping the extension and never cutting a UTF-8 character in half
std::string Truncate(const std::string& o, std::size_t maxBytes)
{
    if (o.size() <= maxBytes) return o;
    std::string ext;
    const auto dot = o.find_last_of('.');
    if (dot != std::string::npos && dot > 0 && o.size() - dot <= 20 && o.size() - dot < maxBytes) ext = o.substr(dot);
    std::size_t keep = maxBytes - ext.size();
    while (keep > 0 && ((unsigned char)o[keep] & 0xC0) == 0x80) --keep;
    return o.substr(0, keep) + ext;
}

// unique inside one folder, ignoring case (Windows and macOS file systems do): "a.txt", "a (2).txt", ...
std::string Dedupe(const std::string& o, std::set<std::string>& used)
{
    std::string name = o;
    if (used.count(Lower(name))) {
        std::string stem = o, ext;
        const auto dot = o.find_last_of('.');
        if (dot != std::string::npos && dot > 0) { stem = o.substr(0, dot); ext = o.substr(dot); }
        for (int i = 2;; ++i) {
            name = stem + " (" + std::to_string(i) + ")" + ext;
            if (!used.count(Lower(name))) break;
        }
    }
    used.insert(Lower(name));
    return name;
}

// ONE file or folder name that is valid on THIS operating system, at most 'maxBytes' long.
// ('/' never reaches this function: the caller splits paths first)
std::string SafeComponent(const std::string& raw, std::size_t maxBytes, std::set<std::string>& used)
{
    const std::string n = ValidUtf8(raw);
    std::string o;
    for (unsigned char c : n) {
        if (c < 32 || c == 127) continue;
#ifdef _WIN32
        if (std::strchr("<>:\"|?*\\/", c)) c = '_';
#else
        if (c == '/') c = '_';
#endif
        o += (char)c;
    }
    const std::size_t room = maxBytes > 16 ? maxBytes - 12 : maxBytes;      // leaves space for " (99)" and "_"
    o = Truncate(o, room);
#ifdef _WIN32
    while (!o.empty() && (o.back() == '.' || o.back() == ' ')) o.pop_back();
    {
        const std::string stem = Lower(o.substr(0, o.find('.')));
        static const char* reserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5",
                                         "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4",
                                         "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
        for (const char* r : reserved) if (stem == r) { o = "_" + o; break; }
    }
#endif
    if (o.empty() || o == "." || o == "..") o = "file";
    return Dedupe(o, used);
}

} // namespace

std::string ClipShare::FormatBytes(std::uint64_t b)
{
    static const char* u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    char buf[48];
    if (i == 0) std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b);
    else        std::snprintf(buf, sizeof buf, "%.2f %s", v, u[i]);
    return buf;
}

ClipShare::ClipShare(fs::path incomingDir, Hooks hooks) : incoming_(std::move(incomingDir)), h_(std::move(hooks))
{
    nextXfer_ = (std::uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
    if (!h_.getFiles) h_.getFiles = [](std::vector<fs::path>& f) { return clipboard::GetFiles(f); };
    if (!h_.getText)  h_.getText  = [](std::string& t, std::size_t m) { return clipboard::GetText(t, m); };
    if (!h_.setFiles) h_.setFiles = [](const std::vector<fs::path>& f) { return clipboard::SetFiles(f); };
    if (!h_.setText)  h_.setText  = [](const std::string& t) { return clipboard::SetText(t); };

    std::error_code ec;
    fs::remove_all(incoming_, ec);                      // leftovers of an earlier run
    fs::create_directories(incoming_, ec);
}

ClipShare::~ClipShare() { Shutdown(); }

void ClipShare::SetLimit(std::uint64_t bytes)
{
    limit_ = std::max(kMinLimit, std::min(kMaxLimit, bytes));
}

void ClipShare::Shutdown()
{
    std::vector<std::shared_ptr<Out>> outs;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& kv : out_) outs.push_back(kv.second);
        out_.clear();
    }
    for (auto& o : outs) {
        o->cancel = true;
        { std::lock_guard<std::mutex> lk(o->m); o->state = 2; }
        o->cv.notify_all();
        if (o->th.joinable()) o->th.join();
    }
    for (auto& kv : in_) {
        if (kv.second->f.is_open()) kv.second->f.close();
    }
    in_.clear();
    std::lock_guard<std::mutex> lk(scanMtx_);
    scans_.clear();
}

void ClipShare::PeerLost(int peer)
{
    AbortIn(peer, false);
    std::shared_ptr<Out> o;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = out_.find(peer);
        if (it != out_.end()) o = it->second;           // joined later by SendTo / Shutdown
    }
    if (o) {
        o->cancel = true;
        { std::lock_guard<std::mutex> lk(o->m); o->state = 2; if (o->reason.empty()) o->reason = "connection lost"; }
        o->cv.notify_all();
    }
}

// ------------------------------------------------------------------ sending
// Core thread: only reads WHAT is on the clipboard (a list of paths, or text). Nothing is walked here.
bool ClipShare::ReadClipboard(Raw& raw, std::string& info)
{
    const std::uint64_t limit = limit_.load();
    std::vector<fs::path> files;
    if (h_.getFiles(files) && !files.empty()) {
        std::string key = "F";
        for (const auto& f : files) {
            if (Inside(f, incoming_)) continue;                 // came from another PC: never forwarded
            raw.paths.push_back(f);
            key += '\n';
            key += U8(f);
        }
        if (raw.paths.empty()) { info = "these files came from another PC and are not sent back"; return false; }
        raw.files = true;
        raw.key = std::move(key);
        return true;
    }

    std::string text;
    const std::size_t maxText = (std::size_t)std::min<std::uint64_t>(limit, clipboard::kMaxTextBytes);
    if (h_.getText(text, maxText) && !text.empty()) {
        raw.text = std::move(text);
        raw.key = TextSig(raw.text);
        return true;
    }
    const std::string why = clipboard::LastNote();          // why the file list could not be used
    info = "nothing to send" + (why.empty() ? std::string() : " (" + why + ")");
    return false;
}

// Worker thread: walk every file and folder on the clipboard, adding up the sizes. Stops at the first
// sign that the limit (bytes or items) is passed and says so in the plan; the caller logs it.
ClipShare::PlanPtr ClipShare::Scan(const Raw& raw, std::uint64_t limit, const std::atomic<bool>& cancel)
{
    auto plan = std::make_shared<Plan>();

    if (!raw.files) {
        Item it;
        it.kind = Kind::Text;
        it.size = raw.text.size();
        it.text = raw.text;
        plan->total = it.size;
        plan->sig = TextSig(raw.text);
        plan->what = "text";
        plan->items.push_back(std::move(it));
        plan->status = Status::Ready;
        return plan;
    }

    Hasher hs;
    hs.add(std::string("F"));
    std::set<std::string> topUsed;
    std::uint64_t sum = 0, nFiles = 0, nDirs = 0, linkSkipped = 0, special = 0, unreadable = 0;
    std::string topName;
    bool stop = false;

    auto overflow = [&](Status st) {
        plan->status = st;
        stop = true;
        const std::string who = raw.paths.size() == 1 ? "'" + topName + "'"
                                                      : "the selection (stopped at '" + topName + "')";
        if (st == Status::TooBig)
            plan->message = who + " is larger than the limit of " + FormatBytes(limit) + " (counted " +
                            Plural(plan->items.size(), "item", "items") + ", at least " + FormatBytes(sum) +
                            "); raise the limit in the Network tab";
        else
            plan->message = who + " has more than " + std::to_string(kMaxItems) + " files and folders";
    };

    auto push = [&](Kind k, std::string rel, const fs::path& p, std::uint64_t size, long long mtime) -> bool {
        if (plan->items.size() >= kMaxItems) { overflow(Status::TooMany); return false; }
        if (k == Kind::File) {
            sum += size;
            if (sum > limit) { overflow(Status::TooBig); return false; }
            ++nFiles;
            hs.add(std::string("F")); hs.add(rel); hs.add(size); hs.add(mtime);
        } else {
            ++nDirs;
            hs.add(std::string("D")); hs.add(rel);
        }
        Item it;
        it.kind = k;
        it.rel = std::move(rel);
        it.path = p;
        it.size = size;
        plan->items.push_back(std::move(it));
        return true;
    };

    for (const auto& top : raw.paths) {
        if (cancel.load()) { plan->status = Status::Cancelled; return plan; }
        std::error_code ec1, ec2;
        const bool isDir = fs::is_directory(top, ec1);
        const bool isFile = !isDir && fs::is_regular_file(top, ec2);
        if (!isDir && !isFile) { ++special; continue; }         // gone, or not a plain file / folder

        topName = U8(top.filename());
        if (topName.empty()) topName = U8(top.parent_path().filename());   // "/home/me/dir/"
        if (topName.empty()) { ++special; continue; }                      // a drive or "/" itself
        const std::string name = Dedupe(topName, topUsed);

        if (isFile) {
            std::error_code e;
            const std::uint64_t size = fs::file_size(top, e);
            if (e || !Readable(top)) { ++unreadable; continue; }
            std::error_code me;
            const auto t = fs::last_write_time(top, me);
            if (!push(Kind::File, name, top, size, me ? 0 : (long long)t.time_since_epoch().count())) break;
            continue;
        }

        if (!push(Kind::Dir, name, top, 0, 0)) break;
        std::error_code ie;
        // skip_permission_denied: folders we may not enter are left out; folder links are not followed
        fs::recursive_directory_iterator it(top, fs::directory_options::skip_permission_denied, ie);
        if (ie) { ++unreadable; continue; }
        std::uint64_t seen = 0;
        while (it != fs::recursive_directory_iterator()) {
            if ((++seen & 255) == 0 && cancel.load()) { plan->status = Status::Cancelled; return plan; }
            const fs::directory_entry& e = *it;
            const std::string rel = name + "/" + G8(e.path().lexically_relative(top));

            std::error_code se;
            bool entryFile = false, entryDir = false;
            if (e.is_symlink(se)) {
                std::error_code te;
                const auto st = fs::status(e.path(), te);           // follows the link
                if (!te && fs::is_regular_file(st)) entryFile = true;
                else ++linkSkipped;                                 // link to a folder (or broken)
            } else if (e.is_directory(se)) {
                entryDir = true;
            } else if (e.is_regular_file(se)) {
                entryFile = true;
            } else {
                ++special;                                          // socket, pipe, device
            }

            if (entryDir) {
                if (!push(Kind::Dir, rel, e.path(), 0, 0)) break;
            } else if (entryFile) {
                std::error_code fe, me;
                const std::uint64_t size = fs::file_size(e.path(), fe);
                const auto t = fs::last_write_time(e.path(), me);
                if (fe || !Readable(e.path())) ++unreadable;
                else if (!push(Kind::File, rel, e.path(), size, me ? 0 : (long long)t.time_since_epoch().count())) break;
            }

            std::error_code ne;
            it.increment(ne);
            if (ne) { ++unreadable; break; }                        // the iterator is unusable after an error
        }
        if (stop) break;
    }
    if (stop) return plan;                                          // TooBig / TooMany: message is set

    std::vector<std::string> notes;
    if (linkSkipped) notes.push_back(Plural(linkSkipped, "link to a folder", "links to folders") + " skipped");
    if (special)     notes.push_back(Plural(special, "entry", "entries") + " skipped (not plain files or folders)");
    if (unreadable)  notes.push_back(Plural(unreadable, "entry", "entries") + " could not be read and " + (unreadable == 1 ? "was" : "were") + " skipped");
    for (const auto& n : notes) { if (!plan->note.empty()) plan->note += "; "; plan->note += n; }

    if (plan->items.empty()) {
        plan->status = Status::Nothing;
        plan->message = plan->note.empty() ? "nothing to send" : plan->note;
        plan->note.clear();
        return plan;
    }
    plan->total = sum;
    plan->sig = "F|" + hs.hex();
    plan->what = Describe(nFiles, nDirs);
    plan->status = Status::Ready;
    return plan;
}

// One walk per clipboard, however many PCs ask for it at the same time.
ClipShare::PlanPtr ClipShare::GetPlan(Out& o)
{
    const std::uint64_t limit = limit_.load();
    const std::string key = std::to_string(limit) + "|" + o.raw.key;

    for (int attempt = 0; attempt < 3; ++attempt) {
        std::shared_future<PlanPtr> fut;
        std::shared_ptr<std::promise<PlanPtr>> prom;
        {
            std::lock_guard<std::mutex> lk(scanMtx_);
            const auto now = std::chrono::steady_clock::now();
            for (auto it = scans_.begin(); it != scans_.end();) {
                if (it->finished && now - it->at > kPlanTtl) it = scans_.erase(it);
                else ++it;
            }
            for (auto& e : scans_) if (e.key == key) { fut = e.fut; break; }
            if (!fut.valid()) {
                prom = std::make_shared<std::promise<PlanPtr>>();
                fut = prom->get_future().share();
                scans_.push_back(ScanEntry{key, fut, now, false});
            }
        }

        if (prom) {                                             // we are the one who walks
            PlanPtr plan;
            try {
                plan = Scan(o.raw, limit, o.cancel);
            } catch (const std::exception& ex) {
                auto p = std::make_shared<Plan>();
                p->status = Status::Nothing;
                p->message = std::string("could not read the clipboard files: ") + ex.what();
                plan = p;
            }
            if (!plan->note.empty()) Log("Clipboard: " + plan->note);
            const bool cancelled = plan->status == Status::Cancelled;
            {
                std::lock_guard<std::mutex> lk(scanMtx_);
                for (auto it = scans_.begin(); it != scans_.end(); ++it) {
                    if (it->key != key) continue;
                    if (cancelled) scans_.erase(it);            // nobody may reuse an unfinished walk
                    else { it->finished = true; it->at = std::chrono::steady_clock::now(); }
                    break;
                }
            }
            prom->set_value(plan);
            if (cancelled) return nullptr;
            return plan;
        }

        while (fut.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
            if (o.cancel.load()) return nullptr;
        }
        PlanPtr plan = fut.get();
        if (plan->status != Status::Cancelled) return plan;
        if (o.cancel.load()) return nullptr;
        // the PC that was walking for us went away: walk ourselves (its entry is gone)
    }
    return nullptr;
}

void ClipShare::SendTo(int peer, const std::string& peerName)
{
    std::shared_ptr<Out> finished;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = out_.find(peer);
        if (it != out_.end()) {
            if (!it->second->done) {                    // still sending the previous clipboard
                Log("Clipboard not sent to " + peerName + " yet: the previous clipboard is still being sent");
                return;
            }
            finished = it->second;
            out_.erase(it);
        }
    }
    if (finished && finished->th.joinable()) finished->th.join();

    Raw raw;
    std::string info;
    if (!ReadClipboard(raw, info)) { if (!info.empty()) Log("Clipboard: " + info); return; }

    auto o = std::make_shared<Out>();
    o->peer = peer;
    o->peerName = peerName;
    o->xfer = nextXfer_++;
    o->raw = std::move(raw);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        out_[peer] = o;
    }
    try {
        o->th = std::thread([this, o] { RunOut(o); });
    } catch (const std::exception& ex) {
        std::lock_guard<std::mutex> lk(mtx_);
        out_.erase(peer);
        Log(std::string("Clipboard not sent to ") + peerName + ": could not start the transfer (" + ex.what() + ")");
    }
}

void ClipShare::RunOut(std::shared_ptr<Out> o)
{
    struct Done {
        std::shared_ptr<Out> o;
        ~Done() { o->done = true; }
    } done{o};

    const PlanPtr plan = GetPlan(*o);
    if (!plan) return;                                          // cancelled while walking
    if (plan->status == Status::Nothing) { Log("Clipboard: " + plan->message); return; }
    if (plan->status != Status::Ready) { Log("Clipboard not sent to " + o->peerName + ": " + plan->message); return; }

    const std::string x = std::to_string(o->xfer);
    bool stalled = false;
    auto stopped = [&] {
        std::lock_guard<std::mutex> lk(o->m);
        return o->state >= 2 || o->cancel.load();
    };
    auto fail = [&](const std::string& why, bool tellPeer) {
        if (tellPeer) Send(o->peer, "CLIPX " + x);
        Log("Clipboard not sent to " + o->peerName + ": " + why);
    };
    // a wait ended because the transfer was stopped (by the other PC, a lost connection) or stalled
    auto stopLog = [&] {
        if (stalled) { fail("receiver stopped answering", true); return; }
        std::string why;
        { std::lock_guard<std::mutex> lk(o->m); why = o->reason; }
        if (!why.empty()) Log("Clipboard not sent to " + o->peerName + ": " + why);
        else fail("transfer stopped", true);
    };

    Send(o->peer, "CLIPO " + x + " " + std::to_string(plan->items.size()) + " " + std::to_string(plan->total) + " " + plan->sig);
    {
        std::unique_lock<std::mutex> lk(o->m);
        o->cv.wait_for(lk, std::chrono::seconds(10), [&] { return o->state != 0 || o->cancel.load(); });
        if (o->state == 0) { lk.unlock(); fail("no answer (is the other PC running the same version?)", true); return; }
        if (o->state == 3) { o->ok = true; return; }          // the other PC's clipboard already holds exactly this
        if (o->state == 2) { const std::string why = o->reason.empty() ? "refused" : o->reason; lk.unlock(); Log("Clipboard not sent to " + o->peerName + ": " + why); return; }
    }
    if (plan->total >= (16ull << 20) || plan->items.size() > 500)
        Log("Sending " + plan->what + " (" + FormatBytes(plan->total) + ") to " + o->peerName + " ...");

    using Clock = std::chrono::steady_clock;
    // wait until 'ready' holds (evaluated under the lock); gives up when the transfer is stopped or the
    // receiver makes no progress for 30 s
    auto waitFor = [&](const std::function<bool()>& ready) -> bool {
        std::unique_lock<std::mutex> lk(o->m);
        auto lastProgress = Clock::now();
        std::int64_t lastAck = o->acked, lastItems = o->itemsAcked;
        while (!ready()) {
            if (o->state >= 2 || o->cancel.load()) return false;
            o->cv.wait_for(lk, std::chrono::milliseconds(500));
            if (o->acked != lastAck || o->itemsAcked != lastItems) { lastAck = o->acked; lastItems = o->itemsAcked; lastProgress = Clock::now(); }
            else if (Clock::now() - lastProgress > std::chrono::seconds(30)) { stalled = true; return false; }
        }
        return o->state < 2 && !o->cancel.load();
    };

    std::int64_t seq = 0;
    std::vector<char> buf(kChunk);
    for (std::size_t idx = 0; idx < plan->items.size(); ++idx) {
        const Item& it = plan->items[idx];
        if (idx > 0 && !waitFor([&] { return (std::int64_t)idx - o->itemsAcked <= kItemWindow; })) { stopLog(); return; }

        const char* kind = it.kind == Kind::Text ? "T" : it.kind == Kind::File ? "F" : "D";
        Send(o->peer, "CLIPI " + x + " " + std::to_string(idx) + " " + kind + " " + std::to_string(it.size) + " " +
                      (it.kind == Kind::Text ? std::string("-") : clipboard::Base64Encode(it.rel)));

        std::ifstream in;
        if (it.kind == Kind::File) {
            in.open(it.path, std::ios::binary);
            if (!in) { fail("cannot read " + it.rel, true); return; }
        }
        std::uint64_t sent = 0;
        while (sent < it.size) {
            const std::size_t want = (std::size_t)std::min<std::uint64_t>(kChunk, it.size - sent);
            if (it.kind == Kind::File) {
                in.read(buf.data(), (std::streamsize)want);
                if ((std::size_t)in.gcount() != want) { fail(it.rel + " changed while it was copied", true); return; }
            } else {
                std::memcpy(buf.data(), it.text.data() + sent, want);
            }
            if (!waitFor([&] { return seq - o->acked <= kWindow; })) { stopLog(); return; }
            Send(o->peer, "CLIPD " + x + " " + std::to_string(idx) + " " + std::to_string(seq) + " " +
                          clipboard::Base64Encode(std::string(buf.data(), want)));
            ++seq;
            sent += want;
        }
        // a file that grew meanwhile would arrive cut off: refuse instead
        if (it.kind == Kind::File && in.peek() != std::ifstream::traits_type::eof()) { fail(it.rel + " changed while it was copied", true); return; }
        Send(o->peer, "CLIPE " + x + " " + std::to_string(idx));
        if (stopped()) { stopLog(); return; }
    }
    Log("Clipboard sent to " + o->peerName + " (" + plan->what + ", " + FormatBytes(plan->total) + "); waiting for it to confirm");
    Send(o->peer, "CLIPF " + x);
    o->ok = true;
}

// ------------------------------------------------------------------ receiving
bool ClipShare::HandleLine(int peer, const std::string& peerName, bool mayWrite, const std::string& line)
{
    if (line.compare(0, 4, "CLIP") != 0) return false;
    std::istringstream is(line);
    std::string cmd;
    std::uint64_t xfer = 0;
    is >> cmd >> xfer;
    if (!is) return true;                               // malformed: swallowed

    if (cmd == "CLIPO") { OnOffer(peer, peerName, mayWrite, xfer, is); return true; }
    if (cmd == "CLIPI") { OnItem(peer, xfer, is); return true; }
    if (cmd == "CLIPD") { OnData(peer, xfer, is); return true; }
    if (cmd == "CLIPE") { OnEnd(peer, xfer, is); return true; }
    if (cmd == "CLIPF") { OnFinish(peer, xfer); return true; }

    // answers for a transfer WE are sending
    std::shared_ptr<Out> o;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = out_.find(peer);
        if (it != out_.end() && it->second->xfer == xfer) o = it->second;
    }
    if (cmd == "CLIPX") {
        auto it = in_.find(peer);
        if (it != in_.end() && it->second->xfer == xfer) AbortIn(peer, false);   // the sender gave up
        if (o && o->done && o->ok) {
            // The transfer had finished, but the other PC could not put it on its clipboard.
            Log("Clipboard sent to " + o->peerName + " arrived, but that PC could not put it on its clipboard - it is sent again at the next crossing");
            return true;
        }
    }
    if (!o) return true;

    if (cmd == "CLIPA") {
        { std::lock_guard<std::mutex> lk(o->m); if (o->state == 0) o->state = 1; }
    } else if (cmd == "CLIPS") {
        { std::lock_guard<std::mutex> lk(o->m); if (o->state == 0) o->state = 3; }
    } else if (cmd == "CLIPR") {
        o->delivered = true;
        Log("Clipboard delivered to " + o->peerName + ": it is on that PC's clipboard, paste it there");
        return true;
    } else if (cmd == "CLIPN") {
        std::string why;
        is >> why;
        std::uint64_t lim = 0;
        if (why == "LIMIT") { is >> lim; why = "larger than the limit set on that PC (" + FormatBytes(lim) + ")"; }
        else if (why == "DENIED") why = "that PC does not accept clipboard data from this one";
        else if (why == "DISK") why = "not enough free disk space on that PC";
        else why = "refused";
        std::lock_guard<std::mutex> lk(o->m);
        o->state = 2;
        o->reason = why;
    } else if (cmd == "CLIPK") {
        long long seq = -1, items = -1;
        is >> seq;
        if (!(is >> items)) items = -1;
        std::lock_guard<std::mutex> lk(o->m);
        if (seq > o->acked) o->acked = seq;
        if (items > o->itemsAcked) o->itemsAcked = items;
    } else if (cmd == "CLIPX") {
        std::lock_guard<std::mutex> lk(o->m);
        o->state = 2;
        if (o->reason.empty()) o->reason = "the other PC cancelled (an older version cannot receive folders: update both PCs)";
    }
    o->cv.notify_all();
    return true;
}

bool ClipShare::AlreadyHas(int peer, const std::string& sig)
{
    if (sig.size() < 3) return false;
    if (sig[0] == 'T') {                                // text: compare with what is on the clipboard right now
        std::string cur;
        return h_.getText(cur, (std::size_t)clipboard::kMaxTextBytes) && !cur.empty() && TextSig(cur) == sig;
    }
    // files and folders: the same set we received last time, and our clipboard still points to exactly those
    const auto g = got_.find(peer);
    if (g == got_.end() || g->second.sig != sig || g->second.files.empty()) return false;
    std::vector<fs::path> cur;
    if (!h_.getFiles(cur) || cur.size() != g->second.files.size()) return false;
    for (const auto& want : g->second.files) {
        std::error_code ec;
        if (!fs::exists(want, ec) || ec) return false;      // deleted meanwhile: it has to be sent again
        bool found = false;
        for (const auto& c : cur) {
            std::error_code ec2;
            if (fs::equivalent(want, c, ec2) && !ec2) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

void ClipShare::AbortIn(int peer, bool notify, const std::string& why)
{
    auto it = in_.find(peer);
    if (it == in_.end()) return;
    In& in = *it->second;
    if (in.f.is_open()) in.f.close();
    std::error_code ec;
    if (!in.dir.empty()) fs::remove_all(in.dir, ec);
    if (notify) Send(peer, "CLIPX " + std::to_string(in.xfer));
    if (!why.empty()) Log("Clipboard from " + in.peerName + " dropped: " + why);
    in_.erase(it);
}

void ClipShare::OnOffer(int peer, const std::string& peerName, bool mayWrite, std::uint64_t xfer, std::istream& is)
{
    std::uint32_t nItems = 0;
    std::uint64_t total = 0;
    is >> nItems >> total;
    const bool parsed = static_cast<bool>(is);
    std::string sig;
    if (parsed) is >> sig;                              // older versions send no signature: that is fine
    AbortIn(peer, false);                               // a new offer replaces an unfinished one
    const std::string x = std::to_string(xfer);
    if (!parsed || nItems == 0 || nItems > kMaxItems) {
        Send(peer, "CLIPN " + x + " ERROR");
        Log("Clipboard from " + peerName + " refused: malformed offer");
        return;
    }
    if (!mayWrite) {
        Send(peer, "CLIPN " + x + " DENIED");
        Log("Clipboard from " + peerName + " refused: turn on \"Share my clipboard\" for that PC here (or allow it to control this PC)");
        return;
    }
    const std::uint64_t limit = limit_.load();
    if (total > limit) {
        Send(peer, "CLIPN " + x + " LIMIT " + std::to_string(limit));
        Log("Clipboard from " + peerName + " refused: " + FormatBytes(total) + " is more than the limit of " + FormatBytes(limit));
        return;
    }
    if (!sig.empty() && AlreadyHas(peer, sig)) { Send(peer, "CLIPS " + x); return; }
    std::error_code ec;
    fs::create_directories(incoming_, ec);
    const auto sp = fs::space(incoming_, ec);
    if (!ec && sp.available < total + (16ull << 20)) {
        Send(peer, "CLIPN " + x + " DISK");
        Log("Clipboard from " + peerName + " refused: not enough free disk space for " + FormatBytes(total));
        return;
    }
    auto in = std::make_unique<In>();
    in->xfer = xfer;
    in->total = total;
    in->nItems = nItems;
    in->peerName = peerName;
    in->sig = sig;
    in->dir = incoming_ / ("t" + std::to_string(++dirSeq_));       // short: Windows paths are limited to 260 characters
    in->dirMap[""] = "";
    in_[peer] = std::move(in);
    Send(peer, "CLIPA " + x);
}

void ClipShare::OnItem(int peer, std::uint64_t xfer, std::istream& is)
{
    auto it = in_.find(peer);
    if (it == in_.end() || it->second->xfer != xfer) return;
    In& in = *it->second;
    std::uint32_t idx = 0;
    std::string kind, b64;
    std::uint64_t size = 0;
    is >> idx >> kind >> size >> b64;
    if (!is || in.open || idx != in.nextItem || idx >= in.nItems || (kind != "T" && kind != "F" && kind != "D")) {
        AbortIn(peer, true, "protocol error");
        return;
    }
    if (kind == "D") size = 0;
    in.declared += size;
    if (in.declared > in.total || in.declared > limit_.load()) { AbortIn(peer, true, "more data than offered"); return; }

    in.itemSize = size;
    in.itemGot = 0;

    if (kind == "T") {
        if (size > clipboard::kMaxTextBytes) { AbortIn(peer, true, "text too large"); return; }
        in.cur = Kind::Text;
        in.text.clear();
        in.text.reserve((std::size_t)size);
        in.open = true;
        return;
    }

    std::string rel;
    if (!clipboard::Base64Decode(b64, rel) || rel.empty()) { AbortIn(peer, true, "bad file name"); return; }
    const bool isDir = kind == "D";

    // "a/b/c.txt": the folder "a/b" must have been announced before (the sender sends folders first)
    std::string parent, leaf = rel;
    const auto slash = rel.find_last_of('/');
    if (slash != std::string::npos) { parent = rel.substr(0, slash); leaf = rel.substr(slash + 1); }
    const auto pit = in.dirMap.find(parent);
    if (leaf.empty() || pit == in.dirMap.end()) { AbortIn(peer, true, "protocol error (unknown folder)"); return; }
    const std::string myParent = pit->second;
    if (isDir && in.dirMap.count(rel)) { AbortIn(peer, true, "protocol error (folder sent twice)"); return; }

    std::size_t room = kNameBytes;
#ifdef _WIN32
    {
        const std::size_t base = U8(in.dir).size() + 1 + (myParent.empty() ? 0 : myParent.size() + 1);
        std::size_t left = base + 8 >= kWinPath ? 0 : kWinPath - base;
        if (isDir && left > 24) left -= 20;                                 // leave room for something inside it
        room = std::min(room, left);
        if (room < 8) { AbortIn(peer, true, "path too long for Windows: " + leaf); return; }
    }
#endif
    const std::string name = SafeComponent(leaf, room, in.used[myParent]);
    const std::string myRel = myParent.empty() ? name : myParent + "/" + name;

    std::error_code ec;
    fs::create_directories(in.dir, ec);
    if (isDir) {
        fs::create_directories(in.dir / FromU8(myRel), ec);
        if (ec) { AbortIn(peer, true, "cannot create folder " + name); return; }
        in.dirMap[rel] = myRel;
        in.cur = Kind::Dir;
        ++in.nDirs;
    } else {
        in.curPath = in.dir / FromU8(myRel);
        in.f.open(in.curPath, std::ios::binary | std::ios::trunc);
        if (!in.f) { AbortIn(peer, true, "cannot write " + U8(in.curPath.filename())); return; }
        in.cur = Kind::File;
        ++in.nFiles;
    }
    if (myParent.empty()) in.tops.push_back(in.dir / FromU8(myRel));
    in.open = true;
}

void ClipShare::OnData(int peer, std::uint64_t xfer, std::istream& is)
{
    auto it = in_.find(peer);
    if (it == in_.end() || it->second->xfer != xfer) return;
    In& in = *it->second;
    std::uint32_t idx = 0;
    long long seq = -1;
    std::string b64;
    is >> idx >> seq >> b64;
    if (!is || !in.open || idx != in.nextItem || seq != in.nextSeq) { AbortIn(peer, true, "protocol error"); return; }
    std::string raw;
    if (!clipboard::Base64Decode(b64, raw) || raw.size() > kChunk || in.itemGot + raw.size() > in.itemSize) {
        AbortIn(peer, true, "bad data");
        return;
    }
    if (in.cur == Kind::File) {
        in.f.write(raw.data(), (std::streamsize)raw.size());
        if (!in.f) { AbortIn(peer, true, "writing to disk failed"); return; }
    } else if (in.cur == Kind::Text) {
        in.text += raw;
    }
    in.itemGot += raw.size();
    in.got += raw.size();
    ++in.nextSeq;
    if (seq % kAckEvery == kAckEvery - 1)
        Send(peer, "CLIPK " + std::to_string(xfer) + " " + std::to_string(seq) + " " + std::to_string(in.nextItem));
}

void ClipShare::OnEnd(int peer, std::uint64_t xfer, std::istream& is)
{
    auto it = in_.find(peer);
    if (it == in_.end() || it->second->xfer != xfer) return;
    In& in = *it->second;
    std::uint32_t idx = 0;
    is >> idx;
    if (!is || !in.open || idx != in.nextItem || in.itemGot != in.itemSize) { AbortIn(peer, true, "incomplete item"); return; }
    if (in.cur == Kind::File) in.f.close();
    else if (in.cur == Kind::Text) in.anyText = true;
    in.open = false;
    ++in.nextItem;
    if (in.nextItem % kItemAck == 0)                    // keeps a flood of tiny files / folders in check
        Send(peer, "CLIPK " + std::to_string(xfer) + " " + std::to_string(in.nextSeq - 1) + " " + std::to_string(in.nextItem));
}

void ClipShare::OnFinish(int peer, std::uint64_t xfer)
{
    auto it = in_.find(peer);
    if (it == in_.end() || it->second->xfer != xfer) return;
    In& in = *it->second;
    if (in.open || in.nextItem != in.nItems) { AbortIn(peer, true, "incomplete transfer"); return; }

    bool ok = false;
    std::string what;
    if (!in.tops.empty()) {
        ok = h_.setFiles(in.tops);
        what = Describe(in.nFiles, in.nDirs);
        if (ok) got_[peer] = Got{in.sig, in.tops};
    } else if (in.anyText) {
        ok = h_.setText(in.text);
        what = "text";
    }
    const std::uint64_t got = in.got;
    const std::string from = in.peerName;
    const fs::path keep = in.dir;
    in_.erase(it);                                      // files stay on disk, only the bookkeeping goes

    if (ok) {
        Log("Clipboard received from " + from + " (" + what + ", " + FormatBytes(got) + ")");
        Send(peer, "CLIPR " + std::to_string(xfer));    // so the sender can say it was delivered
        CleanupOldIncoming(keep);
    } else {
        const std::string why = clipboard::LastNote();
        Log("Could not put the received clipboard on this PC's clipboard" + (why.empty() ? std::string(" (Linux: install xclip or wl-clipboard)") : ": " + why));
        Send(peer, "CLIPX " + std::to_string(xfer));        // so the sender offers it again
    }
}

// Only the newest received set is needed: it is what the clipboard points to.
void ClipShare::CleanupOldIncoming(const fs::path& keep)
{
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(incoming_, ec)) {
        if (e.path() == keep) continue;
        // never remove the folder of a transfer that is still arriving from another peer
        bool active = false;
        for (const auto& kv : in_) if (kv.second->dir == e.path()) active = true;
        if (!active) { std::error_code ec2; fs::remove_all(e.path(), ec2); }
    }
}
