#include "clipshare.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "clipboard.hpp"

namespace fs = std::filesystem;

namespace {

constexpr std::size_t   kChunk    = 32 * 1024;     // raw bytes per CLIPD line
constexpr std::int64_t  kWindow   = 64;            // chunks in flight (2 MiB)
constexpr int           kAckEvery = 16;
constexpr std::uint32_t kMaxItems = 100000;

std::string U8(const fs::path& p)
{
    const auto s = p.u8string();                    // std::string (C++17) or std::u8string (C++20)
    return std::string(s.begin(), s.end());
}

fs::path FromU8(const std::string& s) { return fs::u8path(s); }

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string TextSig(const std::string& t) { return "T|" + std::to_string(std::hash<std::string>{}(t)); }

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

// A file name that is valid on THIS operating system and cannot leave the target folder.
std::string SafeName(std::string n, std::set<std::string>& used)
{
    for (auto& c : n) if (c == '\\') c = '/';
    const auto slash = n.find_last_of('/');
    if (slash != std::string::npos) n.erase(0, slash + 1);

    std::string o;
    for (unsigned char c : n) {
        if (c < 32 || c == 127) continue;
#ifdef _WIN32
        if (std::strchr("<>:\"|?*", c)) c = '_';
#endif
        o += (char)c;
    }
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

    // at most 200 bytes, keeping the extension and never cutting a UTF-8 character in half
    if (o.size() > 200) {
        std::string ext;
        const auto dot = o.find_last_of('.');
        if (dot != std::string::npos && dot > 0 && o.size() - dot <= 20) ext = o.substr(dot);
        std::size_t keep = 200 - ext.size();
        while (keep > 0 && (static_cast<unsigned char>(o[keep]) & 0xC0) == 0x80) --keep;
        o = o.substr(0, keep) + ext;
    }

    // unique inside one transfer (several files may share a name)
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
}

void ClipShare::ForgetSent(int peer)
{
    std::lock_guard<std::mutex> lk(mtx_);
    sentSig_.erase(peer);
}

void ClipShare::ForgetAll()
{
    std::lock_guard<std::mutex> lk(mtx_);
    sentSig_.clear();
}

void ClipShare::PeerLost(int peer)
{
    AbortIn(peer, false);
    std::shared_ptr<Out> o;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = out_.find(peer);
        if (it != out_.end()) o = it->second;           // joined later by SendTo / Shutdown
        sentSig_.erase(peer);
    }
    if (o) {
        o->cancel = true;
        { std::lock_guard<std::mutex> lk(o->m); o->state = 2; if (o->reason.empty()) o->reason = "connection lost"; }
        o->cv.notify_all();
    }
}

// ------------------------------------------------------------------ sending
ClipShare::Prep ClipShare::Prepare(std::vector<Item>& items, std::uint64_t& total, std::string& sig, std::string& info)
{
    const std::uint64_t limit = limit_.load();
    std::vector<fs::path> files;
    if (h_.getFiles(files) && !files.empty()) {
        std::size_t dirs = 0, own = 0;
        std::uint64_t sum = 0;
        std::ostringstream sg;
        sg << "F";
        for (const auto& f : files) {
            if (Inside(f, incoming_)) { ++own; continue; }          // came from another PC: never forwarded
            std::error_code ec;
            if (!fs::is_regular_file(f, ec) || ec) { ++dirs; continue; }
            const std::uint64_t sz = fs::file_size(f, ec);
            if (ec) continue;
            std::error_code ec2;
            const auto mt = fs::last_write_time(f, ec2).time_since_epoch().count();
            Item it;
            it.file = true;
            it.path = f;
            it.name = U8(f.filename());
            it.size = sz;
            sum += sz;
            sg << '|' << U8(f) << '|' << sz << '|' << (long long)mt;
            items.push_back(std::move(it));
        }
        if (items.empty()) {
            if (dirs && !own) info = "only folders are on the clipboard (only files are shared)";
            return Prep::Nothing;
        }
        if (dirs) info = std::to_string(dirs) + " folder(s) skipped (only files are shared)";
        total = sum;
        sig = sg.str();
        return sum > limit ? Prep::TooBig : Prep::Ready;
    }

    std::string text;
    const std::size_t maxText = (std::size_t)std::min<std::uint64_t>(limit, clipboard::kMaxTextBytes);
    if (h_.getText(text, maxText) && !text.empty()) {
        Item it;
        it.file = false;
        it.size = text.size();
        total = text.size();
        sig = TextSig(text);
        it.text = std::move(text);
        items.push_back(std::move(it));
        return Prep::Ready;
    }
    return Prep::Nothing;
}

void ClipShare::SendTo(int peer, const std::string& peerName)
{
    std::shared_ptr<Out> finished;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = out_.find(peer);
        if (it != out_.end()) {
            if (!it->second->done) return;              // still sending the previous clipboard
            finished = it->second;
            out_.erase(it);
        }
    }
    if (finished && finished->th.joinable()) finished->th.join();

    std::vector<Item> items;
    std::uint64_t total = 0;
    std::string sig, info;
    const Prep prep = Prepare(items, total, sig, info);
    if (prep == Prep::Nothing) { if (!info.empty()) Log("Clipboard: " + info); return; }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto s = sentSig_.find(peer);
        if (s != sentSig_.end() && s->second == sig) return;    // the peer already has exactly this
        sentSig_[peer] = sig;
    }
    if (prep == Prep::TooBig) {
        Log("Clipboard not sent to " + peerName + ": " + FormatBytes(total) + " is more than the limit of " +
            FormatBytes(limit_.load()) + " (Network tab)");
        return;
    }
    if (!info.empty()) Log("Clipboard: " + info);

    auto o = std::make_shared<Out>();
    o->peer = peer;
    o->peerName = peerName;
    o->xfer = nextXfer_++;
    o->items = std::move(items);
    o->total = total;
    o->sig = sig;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        out_[peer] = o;
    }
    o->th = std::thread([this, o] { RunOut(o); });
}

void ClipShare::RunOut(std::shared_ptr<Out> o)
{
    // A transfer that did not complete (refused, no answer, error, connection lost) must not be remembered as
    // "the peer already has this", otherwise the same clipboard would never be offered again.
    struct Done {
        ClipShare* self;
        std::shared_ptr<Out> o;
        ~Done()
        {
            if (!o->ok) {
                std::lock_guard<std::mutex> lk(self->mtx_);
                auto s = self->sentSig_.find(o->peer);
                if (s != self->sentSig_.end() && s->second == o->sig) self->sentSig_.erase(s);
            }
            o->done = true;
        }
    } done{this, o};

    const std::string x = std::to_string(o->xfer);
    auto stopped = [&] {
        std::lock_guard<std::mutex> lk(o->m);
        return o->state >= 2 || o->cancel.load();
    };
    auto fail = [&](const std::string& why, bool tellPeer) {
        if (tellPeer) Send(o->peer, "CLIPX " + x);
        Log("Clipboard not sent to " + o->peerName + ": " + why);
    };

    Send(o->peer, "CLIPO " + x + " " + std::to_string(o->items.size()) + " " + std::to_string(o->total));
    {
        std::unique_lock<std::mutex> lk(o->m);
        o->cv.wait_for(lk, std::chrono::seconds(10), [&] { return o->state != 0 || o->cancel.load(); });
        if (o->state == 0) { lk.unlock(); fail("no answer", true); return; }
        if (o->state == 2) { const std::string why = o->reason.empty() ? "refused" : o->reason; lk.unlock(); Log("Clipboard not sent to " + o->peerName + ": " + why); return; }
    }

    // wait until the receiver is less than a window behind
    using Clock = std::chrono::steady_clock;
    auto waitWindow = [&](std::int64_t seq) -> bool {
        std::unique_lock<std::mutex> lk(o->m);
        auto lastProgress = Clock::now();
        std::int64_t lastAck = o->acked;
        while (seq - o->acked > kWindow) {
            if (o->state >= 2 || o->cancel.load()) return false;
            o->cv.wait_for(lk, std::chrono::milliseconds(500));
            if (o->acked != lastAck) { lastAck = o->acked; lastProgress = Clock::now(); }
            else if (Clock::now() - lastProgress > std::chrono::seconds(30)) return false;    // receiver stalled
        }
        return o->state < 2 && !o->cancel.load();
    };

    std::int64_t seq = 0;
    std::vector<char> buf(kChunk);
    for (std::size_t idx = 0; idx < o->items.size(); ++idx) {
        const Item& it = o->items[idx];
        Send(o->peer, "CLIPI " + x + " " + std::to_string(idx) + " " + (it.file ? "F" : "T") + " " +
                      std::to_string(it.size) + " " + (it.file ? clipboard::Base64Encode(it.name) : std::string("-")));

        std::ifstream in;
        if (it.file) {
            in.open(it.path, std::ios::binary);
            if (!in) { fail("cannot read " + it.name, true); return; }
        }
        std::uint64_t sent = 0;
        while (sent < it.size) {
            const std::size_t want = (std::size_t)std::min<std::uint64_t>(kChunk, it.size - sent);
            if (it.file) {
                in.read(buf.data(), (std::streamsize)want);
                if ((std::size_t)in.gcount() != want) { fail(it.name + " changed while it was copied", true); return; }
            } else {
                std::memcpy(buf.data(), it.text.data() + sent, want);
            }
            if (!waitWindow(seq)) {
                if (stopped()) { std::lock_guard<std::mutex> lk(o->m); if (!o->reason.empty()) Log("Clipboard not sent to " + o->peerName + ": " + o->reason); else fail("transfer stopped", true); }
                else fail("receiver stopped answering", true);
                return;
            }
            Send(o->peer, "CLIPD " + x + " " + std::to_string(idx) + " " + std::to_string(seq) + " " +
                          clipboard::Base64Encode(std::string(buf.data(), want)));
            ++seq;
            sent += want;
        }
        Send(o->peer, "CLIPE " + x + " " + std::to_string(idx));
    }
    Send(o->peer, "CLIPF " + x);
    o->ok = true;

    std::string what;
    if (o->items.size() == 1 && !o->items[0].file) what = "text";
    else what = std::to_string(o->items.size()) + (o->items.size() == 1 ? " file" : " files");
    Log("Clipboard sent to " + o->peerName + " (" + what + ", " + FormatBytes(o->total) + ")");
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
    }
    if (!o) return true;

    if (cmd == "CLIPA") {
        { std::lock_guard<std::mutex> lk(o->m); if (o->state == 0) o->state = 1; }
    } else if (cmd == "CLIPN") {
        std::string why, arg;
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
        long long seq = -1;
        is >> seq;
        std::lock_guard<std::mutex> lk(o->m);
        if (seq > o->acked) o->acked = seq;
    } else if (cmd == "CLIPX") {
        std::lock_guard<std::mutex> lk(o->m);
        o->state = 2;
        if (o->reason.empty()) o->reason = "the other PC cancelled";
    }
    o->cv.notify_all();
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
    AbortIn(peer, false);                               // a new offer replaces an unfinished one
    const std::string x = std::to_string(xfer);
    if (!is || nItems == 0 || nItems > kMaxItems) { Send(peer, "CLIPN " + x + " ERROR"); return; }
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
    in->dir = incoming_ / ("t" + x);
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
    if (!is || in.open || idx != in.nextItem || idx >= in.nItems || (kind != "T" && kind != "F")) {
        AbortIn(peer, true, "protocol error");
        return;
    }
    in.declared += size;
    if (in.declared > in.total || in.declared > limit_.load()) { AbortIn(peer, true, "more data than offered"); return; }

    in.itemSize = size;
    in.itemGot = 0;
    in.openFile = kind == "F";
    if (in.openFile) {
        std::string name;
        if (!clipboard::Base64Decode(b64, name)) { AbortIn(peer, true, "bad file name"); return; }
        std::error_code ec;
        fs::create_directories(in.dir, ec);
        in.cur = in.dir / FromU8(SafeName(name, in.used));
        in.f.open(in.cur, std::ios::binary | std::ios::trunc);
        if (!in.f) { AbortIn(peer, true, "cannot write " + U8(in.cur.filename())); return; }
    } else {
        if (size > clipboard::kMaxTextBytes) { AbortIn(peer, true, "text too large"); return; }
        in.text.clear();
        in.text.reserve((std::size_t)size);
    }
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
    if (in.openFile) {
        in.f.write(raw.data(), (std::streamsize)raw.size());
        if (!in.f) { AbortIn(peer, true, "writing to disk failed"); return; }
    } else {
        in.text += raw;
    }
    in.itemGot += raw.size();
    in.got += raw.size();
    ++in.nextSeq;
    if (seq % kAckEvery == kAckEvery - 1) Send(peer, "CLIPK " + std::to_string(xfer) + " " + std::to_string(seq));
}

void ClipShare::OnEnd(int peer, std::uint64_t xfer, std::istream& is)
{
    auto it = in_.find(peer);
    if (it == in_.end() || it->second->xfer != xfer) return;
    In& in = *it->second;
    std::uint32_t idx = 0;
    is >> idx;
    if (!is || !in.open || idx != in.nextItem || in.itemGot != in.itemSize) { AbortIn(peer, true, "incomplete item"); return; }
    if (in.openFile) {
        in.f.close();
        in.files.push_back(in.cur);
    } else {
        in.anyText = true;
    }
    in.open = false;
    ++in.nextItem;
    if (in.nextSeq > 0) Send(peer, "CLIPK " + std::to_string(xfer) + " " + std::to_string(in.nextSeq - 1));
}

void ClipShare::OnFinish(int peer, std::uint64_t xfer)
{
    auto it = in_.find(peer);
    if (it == in_.end() || it->second->xfer != xfer) return;
    In& in = *it->second;
    if (in.open || in.nextItem != in.nItems) { AbortIn(peer, true, "incomplete transfer"); return; }

    bool ok = false;
    std::string what;
    if (!in.files.empty()) {
        ok = h_.setFiles(in.files);
        what = std::to_string(in.files.size()) + (in.files.size() == 1 ? " file" : " files");
    } else if (in.anyText) {
        ok = h_.setText(in.text);
        what = "text";
        if (ok) { std::lock_guard<std::mutex> lk(mtx_); sentSig_[peer] = TextSig(in.text); }   // do not echo it back
    }
    const std::uint64_t got = in.got;
    const std::string from = in.peerName;
    const fs::path keep = in.dir;
    in_.erase(it);                                      // files stay on disk, only the bookkeeping goes

    if (ok) {
        Log("Clipboard received from " + from + " (" + what + ", " + FormatBytes(got) + ")");
        CleanupOldIncoming(keep);
    } else {
        Log("Could not put the received clipboard on this PC's clipboard (Linux: install xclip or wl-clipboard)");
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
