#include "core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <future>
#include <sstream>

#include "clipboard.hpp"
#include "monitors.hpp"
#include "paths.hpp"

namespace {

std::string Stamp()
{
    const std::time_t t = std::time(nullptr);
    std::tm lt{};
#if defined(_WIN32)
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &lt);
    return buf;
}

std::vector<InputRect> ReadMonitorCache(const std::filesystem::path& file)
{
    std::vector<InputRect> v;
    std::ifstream in(file);
    InputRect r;
    while (in >> r.x >> r.y >> r.w >> r.h)
        if (r.w > 0 && r.h > 0 && v.size() < 16) v.push_back(r);
    return v;
}

bool SameRects(const std::vector<InputRect>& a, const std::vector<InputRect>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].w != b[i].w || a[i].h != b[i].h) return false;
    return true;
}

} // namespace

Core::Core(std::filesystem::path dataDir) : dir_(std::move(dataDir)), host_(paths::HostName())
{
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);

    ident_ = std::make_unique<Identity>(Identity::LoadOrCreate(dir_, &identNote_));
    store_ = std::make_unique<ConfigStore>(dir_ / "configs");
    trust_ = std::make_unique<TrustStore>(dir_ / "trust.txt");
    trust_->Load();

    groupFile_ = dir_ / "group.txt";
    {
        std::ifstream gi(groupFile_);
        std::string line;
        if (gi && std::getline(gi, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            groupPwd_ = line;
        }
    }
}

Core::~Core() { Stop(); }

void Core::Log(const std::string& msg)
{
    const std::string line = Stamp() + "  " + msg;
    log_.push_back(line);
    while (log_.size() > 300) log_.pop_front();
    if (onLog) onLog(line);
}

// ------------------------------------------------------------------ start / stop
void Core::LoadMonitors()
{
#if defined(__linux__)
    preferHint_ = std::getenv("WAYLAND_DISPLAY") != nullptr;
#endif
    const auto native = monitors::Native();
    const auto cached = ReadMonitorCache(dir_ / "monitors.txt");
    if (!preferHint_ && !native.empty())  { mons_ = native; monsSource_ = "system"; }
    else if (!cached.empty())             { mons_ = cached; monsSource_ = "saved"; }
    else if (!native.empty())             { mons_ = native; monsSource_ = "system"; }
    else                                  { mons_ = {{0, 0, 1920, 1080}}; monsSource_ = "default"; }
}

bool Core::Start(std::string* err)
{
    if (running_) return true;
    (void)err;

    LoadMonitors();
    layout_.SetLocal(mons_);
    local_ = GetLocalAddresses();

    work_ = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(ctx_.get_executor());
    running_ = true;
    loop_ = std::thread([this] { ctx_.run(); });

    // ---- network: callbacks arrive on the asio thread -> marshal to the Core thread
    NetCallbacks cb;
    cb.onStatus = [this](const std::string& s) { Post([this, s] { Log(s); }); };
    cb.onPeerReady = [this](int id, std::uint32_t inst, const std::string& key, const std::string& name,
                            const std::string& addr, bool incoming, const PeerAudioKeys& ak) {
        Post([=] { OnPeerReady(id, inst, key, name, addr, incoming, ak); });
    };
    cb.onPeerLost = [this](int id) {
        if (input_) input_->RemovePeer(id);     // hand input back immediately
        Post([this, id] { OnPeerLost(id); });
    };
    cb.onMessage = [this](int id, const std::string& line) {
        // Keyboard/mouse traffic is injected right here on the network thread: going through
        // the Core loop would add visible lag.
        if (input_ && input_->HandleMessage(id, line)) return;
        Post([this, id, line] { OnMessage(id, line); });
    };
    cb.onBeacon = [this](const BeaconInfo& b) {
        Post([this, b] {
            state::Beacon nb{b.ip, b.host, b.key, b.server, b.sameGroup};
            auto it = disc_.find(b.ip);
            const bool changed = it == disc_.end() || it->second.b.host != nb.host || it->second.b.server != nb.server ||
                                 it->second.b.key != nb.key || it->second.b.sameGroup != nb.sameGroup;
            disc_[b.ip] = {nb, Clock::now()};
            if (changed) Changed();
        });
    };
    cb.onScanHost = [this](const std::string& ip, bool server) {
        Post([this, ip, server] {
            auto& s = scan_[ip];
            s.s.ip = ip;
            s.s.server = server;
            s.seen = Clock::now();
            Changed();
        });
    };
    cb.onHostName = [this](const std::string& ip, const std::string& name) {
        Post([this, ip, name] {
            auto it = scan_.find(ip);
            if (it == scan_.end()) return;
            it->second.s.name = name;
            Changed();
        });
    };
    cb.onScanDone = [this]() { Post([this] { scanning_ = false; Changed(); }); };
    net_ = std::make_unique<Network>(host_, *ident_, std::move(cb));

    audio_ = std::make_unique<AudioEngine>(net_->Instance());
    {
        std::string e;
        if (audio_->Start(&e)) PostLog("Audio ready (UDP " + std::to_string((int)kAudioPort) + ")");
        else                   PostLog("Audio problem: " + e);
    }

    input_ = std::make_unique<InputEngine>(
        [this](int id, const std::string& line) { net_->Send(id, line); },
        [this](const std::string& msg) { Post([this, msg] { Log("Input: " + msg); }); });
    input_->SetLocalMonitors(mons_);
    input_->onCursorToPeer = [this](int id) { Post([this, id] { OnCursorToPeer(id); }); };
    input_->onModeChange = [this](InputEngine::Mode m, int id) {
        Post([this, m, id] { inputMode_ = m; inputPeer_ = id; Changed(); });
    };
    {
        std::string msg;
        const bool started = input_->Start(&msg);
        if (started) PostLog("Keyboard/mouse sharing ready: " + input_->BackendName());
        else         PostLog("Keyboard/mouse sharing unavailable: " + msg);
        if (!msg.empty() && input_->BackendName() != "none") PostLog("Input: " + msg);
        canInject_ = started && input_->CanInject();
        inputNote_ = msg.empty() && !started ? "keyboard/mouse sharing could not start" : msg;
    }

    {
        ClipShare::Hooks h;
        h.send = [this](int id, const std::string& line) { if (net_) net_->Send(id, line); };
        h.log = [this](const std::string& m) { PostLog(m); };
        clip_ = std::make_unique<ClipShare>(dir_ / "incoming", std::move(h));
        std::ifstream f(dir_ / "clipboard.txt");
        std::string k;
        unsigned long long v = 0;
        if (f >> k >> v && k == "limit") clip_->SetLimit(v);
    }

    Post([this] {
        Log("This PC's key: " + Identity::Fingerprint(ident_->PublicHex()) + "   (data: " + dir_.string() + ")");
        if (!identNote_.empty()) Log("Identity: " + identNote_);
        Log("Monitors (" + monsSource_ + "): " + std::to_string(mons_.size()));
        const std::string cb = clipboard::Backend();
        Log(cb.empty() ? "Clipboard sharing unavailable (on Linux install xclip, xsel or wl-clipboard)"
                       : "Clipboard sharing ready: " + cb + (clipboard::FilesSupported() ? " (text and files)" : " (text only)") +
                         ", limit " + ClipShare::FormatBytes(clip_ ? clip_->Limit() : ClipShare::kDefaultLimit));
        Log(groupPwd_.empty()
            ? "Ready. No password set - PCs without a password connect automatically."
            : "Ready. PCs with the same group password connect automatically.");
    });
    net_->SetGroup(groupPwd_);
    net_->Start();

    ScheduleTick();
    return true;
}

void Core::Stop()
{
    if (!running_.exchange(false)) return;

    // Persist whatever is pending, on the Core thread, before the engines go away.
    {
        auto done = std::make_shared<std::promise<void>>();
        Post([this, done] { FlushConfig(); if (clip_) clip_->Shutdown(); done->set_value(); });
        done->get_future().wait_for(std::chrono::seconds(3));
    }
    if (input_) input_->Stop();   // return the keyboard/mouse before anything else shuts down
    if (audio_) audio_->Stop();
    if (net_) net_->Stop();       // joins the asio thread: no more callbacks after this

    work_.reset();
    ctx_.stop();
    if (loop_.joinable()) loop_.join();
}

void Core::ScheduleTick()
{
    tickTimer_ = std::make_unique<boost::asio::steady_timer>(ctx_);
    auto arm = std::make_shared<std::function<void()>>();
    *arm = [this, arm] {
        if (!running_ || !tickTimer_) return;
        tickTimer_->expires_after(std::chrono::milliseconds(250));
        tickTimer_->async_wait([this, arm](const boost::system::error_code& ec) {
            if (ec || !running_) return;
            Tick();
            (*arm)();
        });
    };
    Post([arm] { (*arm)(); });
}

void Core::Tick()
{
    const auto now = Clock::now();
    if (cfgDirty_ && now - dirtyAt_ > std::chrono::milliseconds(600)) FlushConfig();
    if (++ticks_ % 4 != 0) return;

    bool removed = false;
    for (auto it = disc_.begin(); it != disc_.end();) {
        if (now - it->second.seen > std::chrono::seconds(7)) { it = disc_.erase(it); removed = true; }
        else ++it;
    }
    for (auto it = scan_.begin(); it != scan_.end();) {
        if (now - it->second.seen > std::chrono::seconds(400)) { it = scan_.erase(it); removed = true; }
        else ++it;
    }
    if (removed) Changed();
    // Periodic rescan only while a window is open: without one nobody looks at the result.
    if (clients_ > 0 && ++scanTick_ % 120 == 0) StartScan();
}

// ------------------------------------------------------------------ snapshot
state::Snapshot Core::GetSnapshot() const
{
    state::Snapshot s;
    s.host = host_;
    s.pubKey = ident_->PublicHex();
    s.dataDir = dir_.string();
    s.identNote = identNote_;
    s.backend = input_ ? input_->BackendName() : "none";
    s.monsSource = monsSource_;
    s.group = groupPwd_;
    s.clipLimit = clip_ ? clip_->Limit() : ClipShare::kDefaultLimit;
    s.inputCanInject = canInject_;
    s.inputNote = inputNote_;
    s.mons = mons_;
    s.masterMute = masterMute_;
    s.masterVol = masterVol_;
    s.sysCapturing = sysCapturing_;
    s.micCapturing = micCapturing_;
    s.scanning = scanning_;
    s.inputMode = (int)inputMode_;
    s.inputPeer = inputPeer_;

    for (const auto& kv : peers_) {
        state::Peer p = kv.second;
        layout_.PeerPos(p.id, p.ox, p.oy);
        const auto ts = trust_->Get(p.key);
        p.allowed = ts == TrustStore::State::Allowed && canInject_;   // what is in effect, not just what was ticked
        s.peers.push_back(std::move(p));
    }
    for (const auto& kv : disc_) s.disc.push_back(kv.second.b);
    for (const auto& kv : scan_) s.scan.push_back(kv.second.s);
    return s;
}

state::Stats Core::GetStats() const
{
    state::Stats st;
    if (!audio_) return st;
    st.input = audio_->InputLevel();
    for (const auto& a : audio_->Stats()) st.peers.push_back({a.instance, a.receiving, a.kbps, a.level});
    return st;
}

// ------------------------------------------------------------------ monitors
std::string Core::ScreensLine() const
{
    std::string s = "SCREENS " + std::to_string(mons_.size());
    for (const auto& m : mons_)
        s += " " + std::to_string(m.x) + " " + std::to_string(m.y) + " " + std::to_string(m.w) + " " + std::to_string(m.h);
    return s;
}

void Core::CmdMonitorsHint(const std::vector<InputRect>& hint)
{
    // The OS told us itself (Windows, X11): trust that. On Wayland a headless process cannot ask,
    // so the window reports what it sees and the daemon remembers it for the next start.
    std::vector<InputRect> h;
    for (const auto& r : hint) if (r.w > 0 && r.h > 0 && h.size() < 16) h.push_back(r);
    if (h.empty() || SameRects(h, mons_)) return;
    if (!preferHint_ && monsSource_ == "system") return;

    mons_ = h;
    monsSource_ = "window";
    {
        std::ofstream out(dir_ / "monitors.txt", std::ios::trunc);
        for (const auto& r : mons_) out << r.x << " " << r.y << " " << r.w << " " << r.h << "\n";
    }
    layout_.SetLocal(mons_);
    if (input_) {
        input_->Stop();
        input_->SetLocalMonitors(mons_);
        std::string msg;
        const bool started = input_->Start(&msg);
        if (!started) Log("Keyboard/mouse sharing unavailable: " + msg);
        RefreshInputStatus(started, msg);
    }
    Log("Learned this PC's monitor layout from the window (" + std::to_string(mons_.size()) + " monitor(s))");
    if (!peers_.empty() && net_) net_->SetGroup(groupPwd_);    // reconnect: peers learn the new geometry
    Changed();
}

// ------------------------------------------------------------------ commands
void Core::StartScan()
{
    if (scanning_ || !net_) return;
    local_ = GetLocalAddresses();
    std::vector<std::pair<std::string, int>> nets;
    for (const auto& a : local_) nets.emplace_back(a.ip, a.prefix);
    if (nets.empty()) { Log("No network interface to scan"); return; }
    scanning_ = true;
    net_->Scan(nets);
    Changed();
}

void Core::CmdScan() { StartScan(); }

void Core::CmdSelect(const std::string& ip)
{
    if (ip.empty() || !net_) return;
    local_ = GetLocalAddresses();
    bool isLocal = ip.rfind("127.", 0) == 0;
    for (const auto& a : local_) isLocal = isLocal || a.ip == ip;
    if (isLocal) Log("Selected this machine (" + ip + ")");
    else         Log("Selected " + ip + " - trying to connect");
    net_->Select(ip, isLocal);
}

void Core::CmdGroup(const std::string& password)
{
    groupPwd_ = password;
    std::error_code ec;
    if (groupPwd_.empty()) {
        std::filesystem::remove(groupFile_, ec);
    } else {
        std::filesystem::create_directories(groupFile_.parent_path(), ec);
        {
            std::ofstream out(groupFile_, std::ios::trunc);
            out << groupPwd_ << "\n";
        }
        // owner-only: the password lets other PCs join this group
        std::filesystem::permissions(groupFile_, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ec);
    }
    if (net_) net_->SetGroup(groupPwd_);
    Log(groupPwd_.empty() ? "Password removed - reconnecting" : "Password set - reconnecting");
    Changed();
}

void Core::CmdMove(int id, double x, double y)
{
    auto it = peers_.find(id);
    if (it == peers_.end() || !it->second.placed) return;
    layout_.SetPeerPos(id, x, y);
    SendPos(id, x, y);
    BroadcastRelative(id);
    Changed();
}

void Core::CmdPeerAudio(int id, bool share, bool mic, bool mute, int vol)
{
    auto it = peers_.find(id);
    if (it == peers_.end() || !audio_) return;
    state::Peer& p = it->second;
    vol = std::clamp(vol, 0, 150);

    if (share != p.share) {
        p.share = share;
        audio_->SetPeerSend(p.inst, AudioSource::System, share);
        Log((share ? "Sharing audio with " : "Stopped sharing audio with ") + p.name);
    }
    if (mic != p.mic) {
        p.mic = mic;
        audio_->SetPeerSend(p.inst, AudioSource::Microphone, mic);
        Log((mic ? "Sharing microphone with " : "Stopped sharing microphone with ") + p.name);
    }
    if (mute != p.mute) { p.mute = mute; audio_->SetPeerMuted(p.inst, mute); }
    if (vol != p.vol)   { p.vol = vol;   audio_->SetPeerVolume(p.inst, vol / 100.0f); }
    RefreshCapture();
    RecordPeerAudio(p);
    Changed();
}

void Core::CmdMaster(bool mute, int vol)
{
    if (!audio_) return;
    vol = std::clamp(vol, 0, 150);
    masterMute_ = mute;
    masterVol_ = vol;
    audio_->SetMuted(mute);
    audio_->SetVolume(vol / 100.0f);
    MarkCfgDirty();
    Changed();
}

void Core::CmdControl(int id, bool allow)
{
    auto it = peers_.find(id);
    if (it == peers_.end() || !input_) return;
    const state::Peer& p = it->second;
    if (allow && !canInject_) {
        // Accepting would make the other PC capture its keyboard and mouse for a PC that cannot react.
        Log("Cannot allow " + p.name + " to control this PC: " + (inputNote_.empty() ? "input cannot be injected here" : inputNote_));
        Changed();                                              // the window un-ticks the box
        return;
    }
    std::string err;
    if (!trust_->Set(p.key, allow ? TrustStore::State::Allowed : TrustStore::State::Denied, p.name, &err))
        Log("Could not save permissions: " + err);
    input_->SetPeerAllowed(id, allow);
    SendControlState(id);
    Log(allow ? "Allowed " + p.name + " to control this PC" : "Blocked " + p.name + " from controlling this PC");
    Changed();
}

void Core::CmdClearCache()
{
    // Cached / remembered data that can be rebuilt: who may control this PC (decided again on the next
    // connection), the learned monitor layout and the clipboard bookkeeping. Kept: the identity key, the
    // group password, layouts and per-device audio/clipboard settings.
    std::string err;
    if (!trust_->Clear(&err)) Log("Could not save permissions: " + err);
    for (const auto& kv : peers_) {
        if (input_) input_->SetPeerAllowed(kv.first, false);    // also hands control back if it is active
        SendControlState(kv.first);                              // tell the peer: CTRL 0
    }
    std::error_code ec;
    std::filesystem::remove(dir_ / "monitors.txt", ec);
    if (clip_) clip_->ForgetAll();
    Log("Cache cleared: remembered permissions and the monitor cache were removed (devices must be allowed again)");
    Changed();
}

void Core::CmdRegenKeys()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path file = dir_ / "identity.key", bak = dir_ / "identity.key.bak";
    fs::remove(bak, ec);
    fs::rename(file, bak, ec);                                  // LoadOrCreate makes a new key when the file is gone
    if (ec) { Log("Could not replace the key file: " + ec.message()); return; }

    std::string note;
    const Identity fresh = Identity::LoadOrCreate(dir_, &note);
    if (!fs::exists(file, ec) || !note.empty()) {
        fs::remove(file, ec);
        fs::rename(bak, file, ec);                              // put the old key back: never end up without one
        Log("Could not save the new key" + (note.empty() ? std::string() : " (" + note + ")") + ": the old key stays");
        return;
    }
    fs::remove(bak, ec);                                        // the old private key must not linger on disk
    Log("New keys generated, fingerprint " + fresh.Fingerprint() + ". Restarting the background service ...");
    CmdQuit();                                                  // network, beacons and peers all pick up the new key on restart
}

void Core::CmdClipboard(int id, bool share)
{
    auto it = peers_.find(id);
    if (it == peers_.end()) return;
    state::Peer& p = it->second;
    if (p.clip == share) return;
    p.clip = share;
    if (clip_) clip_->ForgetSent(id);   // switching it on sends the current clipboard at the next crossing
    if (share && clipboard::Backend().empty())
        Log("Clipboard sharing needs xclip, xsel or wl-clipboard on this PC");
    else if (share && !clipboard::FilesSupported())
        Log("Only text can be shared on this PC (copying files needs xclip or wl-clipboard)");
    Log((share ? "Sharing clipboard with " : "Stopped sharing clipboard with ") + p.name);
    RecordPeerAudio(p);                 // per-device settings are saved together (audio + clipboard)
    Changed();
}

void Core::SendControlState(int id)
{
    auto it = peers_.find(id);
    if (it == peers_.end() || !net_) return;
    // "Allowed" is the user's decision; whether control can really work is a separate question. A peer is
    // only told it may control this PC when injection works, so it never grabs its input for nothing.
    const bool allowed = trust_->Get(it->second.key) == TrustStore::State::Allowed;
    net_->Send(id, allowed && canInject_ ? "CTRL 1" : "CTRL 0");
}

void Core::RefreshInputStatus(bool started, const std::string& msg)
{
    const bool can = started && input_ && input_->CanInject();
    const bool changed = can != canInject_;
    canInject_ = can;
    inputNote_ = msg.empty() && !started ? "keyboard/mouse sharing could not start" : msg;
    if (changed) for (const auto& kv : peers_) SendControlState(kv.first);
}

// ------------------------------------------------------------------ network results
void Core::OnPeerReady(int id, std::uint32_t inst, const std::string& key, const std::string& name,
                       const std::string& addr, bool incoming, const PeerAudioKeys& audioKeys)
{
    state::Peer& p = peers_[id];
    p.id = id;
    p.inst = inst;
    p.key = key;
    p.name = name;
    p.addr = addr;
    p.incoming = incoming;
    p.placed = false;
    if (clip_) clip_->PeerLost(id);     // a new connection: the peer's clipboard is unknown again

    Log("Connection established with " + name + " (" + addr + "), key " + Identity::Fingerprint(key) + " verified");
    audio_->AddPeer(inst, addr);
    audio_->SetPeerKeys(inst, audioKeys.tx, audioKeys.rx);      // no keys, no audio in either direction
    net_->Send(id, ScreensLine());      // tell the peer about our monitors
    OnSetChanged();                     // a new set of PCs: switch to its config
    // The layout entry appears once the peer's SCREENS line arrives.
    Changed();
}

void Core::OnPeerLost(int id)
{
    auto it = peers_.find(id);
    if (it != peers_.end()) {
        Log("Disconnected from " + it->second.name);
        audio_->RemovePeer(it->second.inst);
        peers_.erase(it);
    }
    if (clip_) clip_->PeerLost(id);
    layout_.RemovePeer(id);             // its virtual window disappears with the connection
    OnSetChanged();
    RefreshCapture();                   // nobody left to hear our audio: stop capturing
    Changed();
}

void Core::OnMessage(int id, const std::string& line)
{
    auto it = peers_.find(id);
    if (it == peers_.end()) return;
    state::Peer& p = it->second;

    if (clip_ && clip_->HandleLine(id, p.name, p.clip || trust_->Get(p.key) == TrustStore::State::Allowed, line)) return;

    if (line.rfind("SCREENS ", 0) == 0) {
        if (p.placed) return;
        std::istringstream is(line.substr(8));
        int n = 0;
        is >> n;
        std::vector<InputRect> mons;
        for (int i = 0; i < n && i < 16; ++i) {
            int x, y, w, h;
            if (!(is >> x >> y >> w >> h)) break;
            if (w > 0 && h > 0) mons.push_back({x, y, w, h});
        }
        if (mons.empty()) mons.push_back({0, 0, 1920, 1080});
        p.mons = mons;
        p.placed = true;

        // The side that accepted the connection decides the initial layout and sends it;
        // the connecting side only places a provisional window until that POS arrives.
        const auto pos = layout_.AddPeer(id, mons, p.incoming);
        input_->SetPeer(id, mons, pos.first, pos.second);
        input_->SetPeerAllowed(id, trust_->Get(p.key) == TrustStore::State::Allowed);
        SendControlState(id);               // the peer only takes over this PC's input if we said CTRL 1
        ApplyStoredLayout(id);              // saved position for this set of PCs (or the default one)
        Changed();
        return;
    }

    if (line == "CTRL 0" || line == "CTRL 1") {
        if (p.placed) input_->SetPeerAccepts(id, line[5] == '1');
        return;
    }

    {   // PEERPOS <key> <x> <y>: where the PC with that key is, relative to THIS PC
        char key[80] = {};
        int rx = 0, ry = 0;
        if (p.placed && std::sscanf(line.c_str(), "PEERPOS %64s %d %d", key, &rx, &ry) == 3) {
            for (auto& kv : peers_) {
                if (kv.first == id || !kv.second.placed || kv.second.key != key) continue;
                layout_.SetPeerPos(kv.first, rx, ry);
                input_->SetPeerOffset(kv.first, rx, ry);
                RecordPeerPos(kv.first, rx, ry);
            }
            Changed();
            return;
        }
    }

    int x, y;
    if (p.placed && std::sscanf(line.c_str(), "POS %d %d", &x, &y) == 2) {
        layout_.SetPeerPos(id, -x, -y);    // mirrored: their view of us is the opposite offset
        input_->SetPeerOffset(id, -x, -y);
        RecordPeerPos(id, -x, -y);
        Changed();
    }
}

// ------------------------------------------------------------------ layout sync
// With 3+ PCs every PC keeps the whole layout in its own frame. When the user drags PC X
// here, the other PCs must learn where X now is relative to THEM (and X where they are),
// otherwise PC2 and PC3 would disagree about their own neighbourhood.
void Core::BroadcastRelative(int movedId)
{
    auto mit = peers_.find(movedId);
    double mx = 0, my = 0;
    if (mit == peers_.end() || mit->second.key.empty() || !layout_.PeerPos(movedId, mx, my)) return;
    for (const auto& kv : peers_) {
        if (kv.first == movedId || !kv.second.placed || kv.second.key.empty()) continue;
        double qx = 0, qy = 0;
        if (!layout_.PeerPos(kv.first, qx, qy)) continue;
        net_->Send(kv.first, "PEERPOS " + mit->second.key + " " +
                   std::to_string(std::llround(mx - qx)) + " " + std::to_string(std::llround(my - qy)));
        net_->Send(movedId, "PEERPOS " + kv.second.key + " " +
                   std::to_string(std::llround(qx - mx)) + " " + std::to_string(std::llround(qy - my)));
    }
}

void Core::SendPos(int id, double x, double y)
{
    input_->SetPeerOffset(id, x, y);
    net_->Send(id, "POS " + std::to_string(std::lround(x)) + " " + std::to_string(std::lround(y)));
    RecordPeerPos(id, x, y);
}

// ------------------------------------------------------------------ saved configs
// The config that belongs to the exact set of connected PCs (by public key) is kept in cfg_.
// Every change goes into cfg_ right away and is written to disk shortly afterwards; when the
// set changes the matching config is loaded.
void Core::UpdateCfgFromState()
{
    cfg_.audio.mute   = masterMute_;
    cfg_.audio.volume = masterVol_;
    for (const auto& kv : peers_) RecordPeerAudio(kv.second, true);
}

void Core::TouchCfg()
{
    cfgDirty_ = true;
    dirtyAt_ = Clock::now();
}

void Core::MarkCfgDirty()
{
    if (!haveCfg_ || loading_) return;
    UpdateCfgFromState();
    TouchCfg();
}

// 'force': also while loading / for the initial fill of a new config (caller handles saving)
void Core::RecordPeerAudio(const state::Peer& p, bool force)
{
    if (!haveCfg_ || p.key.empty() || (loading_ && !force)) return;
    SavedPeer& sp = cfg_.PeerRef(p.key);
    sp.hasAudio = true;
    sp.send = p.share;
    sp.mic = p.mic;
    sp.hasMic = true;
    sp.mute = p.mute;
    sp.volume = p.vol;
    sp.hasClip = true;
    sp.clip = p.clip;
    if (!force) TouchCfg();
}

void Core::RecordPeerPos(int id, double x, double y)
{
    if (!haveCfg_) return;
    auto it = peers_.find(id);
    if (it == peers_.end() || it->second.key.empty()) return;
    SavedPeer& sp = cfg_.PeerRef(it->second.key);
    const std::string& name = it->second.name;
    const bool changed = std::llround(sp.ox) != std::llround(x) || std::llround(sp.oy) != std::llround(y) ||
                         sp.name != name;
    sp.ox = x;
    sp.oy = y;
    sp.name = name;
    sp.hasPos = true;
    if (changed && !loading_) TouchCfg();
}

void Core::FlushConfig()
{
    if (!haveCfg_ || !cfgDirty_) return;
    cfgDirty_ = false;
    cfg_.rev = NowMs();
    std::string err;
    if (!store_->Save(cfg_, &err)) Log("Could not save config: " + err);
}

// Puts the saved position of peer 'id' in the layout (pushed off any overlap) and, when this
// side accepted the connection, tells the peer. Without a saved position the current one is kept.
void Core::ApplyStoredLayout(int id)
{
    auto it = peers_.find(id);
    if (it == peers_.end() || !it->second.placed) return;
    state::Peer& p = it->second;
    double x = 0, y = 0;
    const SavedPeer* sp = haveCfg_ ? cfg_.FindPeer(p.key) : nullptr;
    if (sp && sp->hasPos) {
        const auto pos = layout_.PlacePeer(id, sp->ox, sp->oy);
        x = pos.first;
        y = pos.second;
    } else if (!layout_.PeerPos(id, x, y)) {
        return;
    }
    if (p.incoming) SendPos(id, x, y);            // the accepting side is authoritative
    else { input_->SetPeerOffset(id, x, y); RecordPeerPos(id, x, y); }
}

void Core::PushPeerAudio(const state::Peer& p)
{
    audio_->SetPeerSend(p.inst, AudioSource::System, p.share);
    audio_->SetPeerSend(p.inst, AudioSource::Microphone, p.mic);
    audio_->SetPeerMuted(p.inst, p.mute);
    audio_->SetPeerVolume(p.inst, p.vol / 100.0f);
}

// Audio settings of the config go into the engines.
void Core::ApplyConfigSettings()
{
    const SavedAudio& a = cfg_.audio;

    masterMute_ = a.mute;
    masterVol_ = a.volume;
    audio_->SetMuted(a.mute);
    audio_->SetVolume(a.volume / 100.0f);

    for (auto& kv : peers_) {
        state::Peer& p = kv.second;
        const SavedPeer* sp = cfg_.FindPeer(p.key);
        if (sp && sp->hasAudio) { p.share = sp->send; p.mic = sp->mic; p.mute = sp->mute; p.vol = sp->volume; }
        else {                                                      // old file: one switch and one source for all
            p.share = a.send && a.source == 1;
            p.mic   = a.send && a.source != 1;
            p.mute = false; p.vol = 100;
        }
        p.clip = sp && sp->hasClip && sp->clip;
        PushPeerAudio(p);
    }
    RefreshCapture();
    if (sysCapturing_ || micCapturing_) Log("Audio sharing switched on by the saved config");
}

// Called whenever a PC joins or leaves: save the old config, load (or start) the one for the new set of PCs.
void Core::OnSetChanged()
{
    FlushConfig();

    std::vector<std::string> members{ident_->PublicHex()};
    std::string names = host_;
    for (const auto& kv : peers_) {
        members.push_back(kv.second.key);
        names += " + " + kv.second.name;
    }
    members = ConfigStore::Normalize(members);

    if (members.size() < 2) { haveCfg_ = false; return; }          // alone: nothing to save
    if (haveCfg_ && cfg_.members == members) return;

    loading_ = true;
    SavedConfig loaded;
    const std::string id = ConfigStore::SetId(members);
    if (store_->Load(members, loaded)) {
        cfg_ = std::move(loaded);
        haveCfg_ = true;
        Log("Loaded saved config " + id + " for " + names);
        ApplyConfigSettings();
    } else {
        cfg_ = SavedConfig{};                    // new combination: start from the current settings
        cfg_.members = members;
        haveCfg_ = true;
        UpdateCfgFromState();
        TouchCfg();
        Log("New combination of PCs (" + names + ") - created config " + id);
    }
    cfg_.label = names;

    for (const auto& kv : peers_) ApplyStoredLayout(kv.first);   // peers that already sent SCREENS
    loading_ = false;
}

// ------------------------------------------------------------------ audio capture
// Each source is captured (on its default device) while at least one device has the matching
// switch on: "Share my audio" = system audio, "Share my microphone" = mic.
void Core::RefreshCapture()
{
    bool anySys = false, anyMic = false;
    for (const auto& kv : peers_) { anySys = anySys || kv.second.share; anyMic = anyMic || kv.second.mic; }
    UpdateCapture(AudioSource::System,     anySys, sysCapturing_, "system audio");
    UpdateCapture(AudioSource::Microphone, anyMic, micCapturing_, "microphone");
}

void Core::UpdateCapture(AudioSource src, bool want, bool& capturing, const char* what)
{
    if (want == capturing) return;
    std::string err;
    if (!audio_->SetCapture(src, want, &err)) {
        Log(std::string("Capturing ") + what + " failed: " + err);
        for (auto& kv : peers_) {
            bool& flag = src == AudioSource::System ? kv.second.share : kv.second.mic;
            flag = false;
            audio_->SetPeerSend(kv.second.inst, src, false);
            RecordPeerAudio(kv.second);
        }
        capturing = false;
        Changed();
        return;
    }
    capturing = want;
    Log(std::string(want ? "Capturing " : "Stopped capturing ") + what);
}


// ------------------------------------------------------------------ clipboard sharing
// The work is done by ClipShare (clipshare.hpp). Core decides WHEN: the cursor moved over to a peer
// we share the clipboard with.
void Core::OnCursorToPeer(int id)
{
    auto it = peers_.find(id);
    if (it == peers_.end() || !net_ || !clip_) return;
    const state::Peer& p = it->second;
    if (!p.clip || !p.placed || clipboard::Backend().empty()) return;
    clip_->SendTo(id, p.name);
}

void Core::CmdClipLimit(std::uint64_t bytes)
{
    if (!clip_) return;
    clip_->SetLimit(bytes);
    const std::uint64_t v = clip_->Limit();         // clamped
    {
        std::ofstream f(dir_ / "clipboard.txt", std::ios::trunc);
        if (f) f << "limit " << v << "\n";
        else Log("Could not save the clipboard limit");
    }
    Log("Clipboard limit: " + ClipShare::FormatBytes(v));
    Changed();
}
