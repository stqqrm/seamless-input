#include "ipc.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include <boost/asio.hpp>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace ipc {

// ---------------------------------------------------------------- encoding
std::string Enc(const std::string& s)
{
    if (s.empty()) return "~";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == ':' || c == '/' || c == '@' || c == '+') o += (char)c;
        else { char b[4]; std::snprintf(b, sizeof(b), "%%%02X", c); o += b; }
    }
    return o;
}

std::string Dec(const std::string& s)
{
    if (s == "~") return {};
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            o += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else o += s[i];
    }
    return o;
}

std::vector<std::string> Tokens(const std::string& line)
{
    std::vector<std::string> t;
    std::istringstream is(line);
    std::string w;
    while (is >> w) t.push_back(w);
    return t;
}

long long ToInt(const std::string& s, long long def)
{
    if (s.empty()) return def;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    return end && *end == '\0' ? v : def;
}

// ---------------------------------------------------------------- snapshot
std::vector<std::string> SerializeSnapshot(const state::Snapshot& s)
{
    std::vector<std::string> v;
    auto b = [](bool x) { return x ? "1" : "0"; };
    v.push_back("SB");
    v.push_back("H " + Enc(s.host) + " " + Enc(s.pubKey) + " " + Enc(s.dataDir) + " " + Enc(s.identNote) + " " +
                Enc(s.backend) + " " + Enc(s.monsSource) + " " + Enc(s.version));
    for (const auto& m : s.mons)
        v.push_back("M " + std::to_string(m.x) + " " + std::to_string(m.y) + " " + std::to_string(m.w) + " " + std::to_string(m.h));
    v.push_back("G " + Enc(s.group));
    v.push_back("Q " + std::to_string(s.clipLimit) + " " + b(s.clipTelemetry));
    v.push_back(std::string("X ") + b(s.inputCanInject) + " " + Enc(s.inputNote));
    v.push_back(std::string("A ") + b(s.masterMute) + " " + std::to_string(s.masterVol) + " " + b(s.sysCapturing) + " " +
                b(s.micCapturing) + " " + b(s.scanning));
    v.push_back("I " + std::to_string(s.inputMode) + " " + std::to_string(s.inputPeer));
    for (const auto& p : s.peers) {
        std::string l = "P " + std::to_string(p.id) + " " + std::to_string(p.inst) + " " + Enc(p.key) + " " + Enc(p.name) + " " +
                        Enc(p.addr) + " " + b(p.incoming) + " " + b(p.placed) + " " +
                        std::to_string(std::llround(p.ox)) + " " + std::to_string(std::llround(p.oy)) + " " +
                        b(p.share) + " " + b(p.mic) + " " + b(p.mute) + " " + std::to_string(p.vol) + " " +
                        b(p.allowed) + " " + std::to_string(p.mons.size());
        for (const auto& m : p.mons)
            l += " " + std::to_string(m.x) + " " + std::to_string(m.y) + " " + std::to_string(m.w) + " " + std::to_string(m.h);
        l += std::string(" ") + b(p.clip);          // after the monitor list: older readers ignore it
        v.push_back(std::move(l));
    }
    for (const auto& d : s.disc)
        v.push_back("D " + Enc(d.ip) + " " + Enc(d.host) + " " + b(d.server) + " " + Enc(d.key) + " " + b(d.sameGroup));
    for (const auto& c : s.scan)
        v.push_back("C " + Enc(c.ip) + " " + Enc(c.name) + " " + b(c.server));
    v.push_back("SE");
    return v;
}

bool ParseSnapshotLine(const std::vector<std::string>& t, state::Snapshot& s)
{
    if (t.empty()) return false;
    const std::string& k = t[0];
    auto I = [&](size_t i) { return i < t.size() ? ToInt(t[i]) : 0; };
    auto S = [&](size_t i) { return i < t.size() ? Dec(t[i]) : std::string(); };
    if (k == "H" && t.size() >= 7) {
        s.host = S(1); s.pubKey = S(2); s.dataDir = S(3); s.identNote = S(4); s.backend = S(5); s.monsSource = S(6);
        if (t.size() >= 8) s.version = S(7);
    } else if (k == "M" && t.size() >= 5) {
        s.mons.push_back({(int)I(1), (int)I(2), (int)I(3), (int)I(4)});
    } else if (k == "G") {
        s.group = S(1);
    } else if (k == "X") {
        s.inputCanInject = I(1) != 0;
        s.inputNote = S(2);
    } else if (k == "Q") {
        s.clipLimit = t.size() > 1 ? (std::uint64_t)std::strtoull(t[1].c_str(), nullptr, 10) : (1ull << 30);
        s.clipTelemetry = I(2) != 0;
    } else if (k == "A" && t.size() >= 6) {
        s.masterMute = I(1) != 0; s.masterVol = (int)I(2); s.sysCapturing = I(3) != 0; s.micCapturing = I(4) != 0; s.scanning = I(5) != 0;
    } else if (k == "I" && t.size() >= 3) {
        s.inputMode = (int)I(1); s.inputPeer = (int)I(2);
    } else if (k == "P" && t.size() >= 16) {
        state::Peer p;
        p.id = (int)I(1); p.inst = (std::uint32_t)I(2); p.key = S(3); p.name = S(4); p.addr = S(5);
        p.incoming = I(6) != 0; p.placed = I(7) != 0;
        p.ox = (double)I(8); p.oy = (double)I(9);
        p.share = I(10) != 0; p.mic = I(11) != 0; p.mute = I(12) != 0; p.vol = (int)I(13);
        p.allowed = I(14) != 0;
        const long long n = I(15);
        for (long long i = 0; i < n && 16 + (size_t)i * 4 + 3 < t.size(); ++i) {
            const size_t o = 16 + (size_t)i * 4;
            p.mons.push_back({(int)I(o), (int)I(o + 1), (int)I(o + 2), (int)I(o + 3)});
        }
        p.clip = I(16 + (size_t)(n > 0 ? n : 0) * 4) != 0;
        s.peers.push_back(std::move(p));
    } else if (k == "D" && t.size() >= 6) {
        s.disc.push_back({S(1), S(2), S(4), I(3) != 0, I(5) != 0});
    } else if (k == "C" && t.size() >= 4) {
        s.scan.push_back({S(1), S(2), I(3) != 0});
    } else return false;
    return true;
}

std::string SerializeStats(const state::Stats& st)
{
    char b[64];
    std::snprintf(b, sizeof(b), "ST %.3f %zu", st.input, st.peers.size());
    std::string l = b;
    for (const auto& p : st.peers) {
        std::snprintf(b, sizeof(b), " %u %d %.1f %.3f", p.inst, p.receiving ? 1 : 0, p.kbps, p.level);
        l += b;
    }
    return l;
}

bool ParseStats(const std::vector<std::string>& t, state::Stats& st)
{
    if (t.size() < 3 || t[0] != "ST") return false;
    st = {};
    st.input = (float)std::atof(t[1].c_str());
    const long long n = ToInt(t[2]);
    for (long long i = 0; i < n && 3 + (size_t)i * 4 + 3 < t.size() + 0; ++i) {
        const size_t o = 3 + (size_t)i * 4;
        state::PeerAudioStat a;
        a.inst = (std::uint32_t)ToInt(t[o]);
        a.receiving = ToInt(t[o + 1]) != 0;
        a.kbps = std::atof(t[o + 2].c_str());
        a.level = (float)std::atof(t[o + 3].c_str());
        st.peers.push_back(a);
    }
    return true;
}

std::string SerializeClipActivity(const state::ClipActivity& a)
{
    std::string l = "CT " + std::to_string(a.items.size());
    for (const auto& t : a.items)
        l += std::string(" ") + (t.upload ? "1" : "0") + " " + Enc(t.peer) + " " + std::to_string(t.done) + " " +
             std::to_string(t.total) + " " + (t.finished ? "1" : "0");
    return l;
}

bool ParseClipActivity(const std::vector<std::string>& t, state::ClipActivity& a)
{
    if (t.size() < 2 || t[0] != "CT") return false;
    a = {};
    const long long n = ToInt(t[1]);
    for (long long i = 0; i < n && 2 + (size_t)i * 5 + 4 < t.size(); ++i) {
        const size_t o = 2 + (size_t)i * 5;
        state::ClipTransfer c;
        c.upload = ToInt(t[o]) != 0;
        c.peer = Dec(t[o + 1]);
        c.done = (std::uint64_t)std::strtoull(t[o + 2].c_str(), nullptr, 10);
        c.total = (std::uint64_t)std::strtoull(t[o + 3].c_str(), nullptr, 10);
        c.finished = ToInt(t[o + 4]) != 0;
        a.items.push_back(std::move(c));
    }
    return true;
}

// ---------------------------------------------------------------- token
std::filesystem::path TokenPath(const std::filesystem::path& dataDir) { return dataDir / "ipc.token"; }

std::string ReadToken(const std::filesystem::path& dataDir)
{
    std::ifstream in(TokenPath(dataDir));
    std::string t;
    if (in) std::getline(in, t);
    while (!t.empty() && (t.back() == '\r' || t.back() == '\n' || t.back() == ' ')) t.pop_back();
    return t;
}

// ---------------------------------------------------------------- blocking helpers with timeouts
namespace {

bool ConnectLoopback(asio::io_context& io, tcp::socket& s, int timeoutMs)
{
    boost::system::error_code result = asio::error::would_block;
    s.async_connect(tcp::endpoint(asio::ip::address_v4::loopback(), kPort),
                    [&](const boost::system::error_code& e) { result = e; });
    io.restart();
    io.run_for(std::chrono::milliseconds(timeoutMs));
    if (result == asio::error::would_block) {
        boost::system::error_code ig;
        s.close(ig);
        io.restart();
        io.poll();
        return false;
    }
    return !result;
}

// Reads one line (without the newline) with a timeout. Leftover bytes stay in 'buf'.
bool ReadLine(asio::io_context& io, tcp::socket& s, asio::streambuf& buf, int timeoutMs, std::string& out)
{
    boost::system::error_code result = asio::error::would_block;
    asio::async_read_until(s, buf, '\n', [&](const boost::system::error_code& e, std::size_t) { result = e; });
    io.restart();
    io.run_for(std::chrono::milliseconds(timeoutMs));
    if (result == asio::error::would_block) {
        boost::system::error_code ig;
        s.close(ig);
        io.restart();
        io.poll();
        return false;
    }
    if (result) return false;
    std::istream is(&buf);
    std::getline(is, out);
    if (!out.empty() && out.back() == '\r') out.pop_back();
    return true;
}

bool WriteAll(asio::io_context& io, tcp::socket& s, const std::string& data, int timeoutMs)
{
    boost::system::error_code result = asio::error::would_block;
    asio::async_write(s, asio::buffer(data), [&](const boost::system::error_code& e, std::size_t) { result = e; });
    io.restart();
    io.run_for(std::chrono::milliseconds(timeoutMs));
    if (result == asio::error::would_block) {
        boost::system::error_code ig;
        s.close(ig);
        io.restart();
        io.poll();
        return false;
    }
    return !result;
}

} // namespace

// ---------------------------------------------------------------- client
struct Client::Impl {
    Handlers h;
    asio::io_context io;
    tcp::socket sock{io};
    asio::streambuf buf{1 << 20};
    std::thread th;
    std::deque<std::string> wq;
    bool writing = false;
    std::atomic<bool> connected{false};
    std::atomic<bool> closing{false};
    state::Snapshot cur;
    bool inSnap = false;

    void ReadNext()
    {
        asio::async_read_until(sock, buf, '\n', [this](const boost::system::error_code& ec, std::size_t) {
            if (ec) { Lost(); return; }
            std::istream is(&buf);
            std::string line;
            std::getline(is, line);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            Handle(line);
            ReadNext();
        });
    }

    void Handle(const std::string& line)
    {
        if (line.empty()) return;
        if (line[0] == 'L' && line.size() > 2 && line[1] == ' ') {
            if (h.onLog) h.onLog(Dec(line.substr(2)));
            return;
        }
        const auto t = Tokens(line);
        if (t.empty()) return;
        if (t[0] == "SB") { cur = {}; inSnap = true; return; }
        if (t[0] == "SE") { if (inSnap && h.onSnapshot) h.onSnapshot(cur); inSnap = false; return; }
        if (t[0] == "ST") { state::Stats st; if (ParseStats(t, st) && h.onStats) h.onStats(st); return; }
        if (t[0] == "CT") { state::ClipActivity ca; if (ParseClipActivity(t, ca) && h.onClipActivity) h.onClipActivity(ca); return; }
        if (inSnap) ParseSnapshotLine(t, cur);
    }

    void Lost()
    {
        if (connected.exchange(false) && !closing && h.onDisconnected) h.onDisconnected();
    }

    void DoWrite()
    {
        if (wq.empty()) { writing = false; return; }
        writing = true;
        asio::async_write(sock, asio::buffer(wq.front()), [this](const boost::system::error_code& ec, std::size_t) {
            if (ec) { Lost(); return; }
            wq.pop_front();
            DoWrite();
        });
    }
};

Client::Client(Handlers h) : d_(std::make_unique<Impl>()) { d_->h = std::move(h); }
Client::~Client() { Close(); }

bool Client::Connected() const { return d_->connected; }

bool Client::Connect(const std::filesystem::path& dataDir, bool attach, std::string* err, int timeoutMs)
{
    Close();
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        boost::system::error_code ig;
        d_->sock.close(ig);
        return false;
    };
    const std::string token = ReadToken(dataDir);
    if (token.empty()) return fail("no daemon token (daemon not started yet)");

    d_->closing = false;
    d_->sock = tcp::socket(d_->io);
    d_->buf.consume(d_->buf.size());
    if (!ConnectLoopback(d_->io, d_->sock, timeoutMs)) return fail("cannot connect to the daemon");
    if (!WriteAll(d_->io, d_->sock, "AUTH " + token + "\n", timeoutMs)) return fail("daemon did not accept the connection");
    std::string hello;
    if (!ReadLine(d_->io, d_->sock, d_->buf, timeoutMs, hello) || hello.rfind("HELLO ", 0) != 0)
        return fail("daemon refused the token");
    const auto ht = Tokens(hello);
    if (ht.size() < 2 || ToInt(ht[1]) != kProtocol) return fail("daemon speaks a different protocol version - restart it");

    d_->connected = true;
    if (attach) d_->wq.push_back("ATTACH\n");
    d_->th = std::thread([this] {
        d_->io.restart();
        d_->ReadNext();
        if (!d_->wq.empty()) d_->DoWrite();
        d_->io.run();
    });
    return true;
}

void Client::Close()
{
    d_->closing = true;
    if (d_->th.joinable()) {
        asio::post(d_->io, [this] {
            boost::system::error_code ig;
            d_->sock.close(ig);
        });
        d_->th.join();
    } else {
        boost::system::error_code ig;
        d_->sock.close(ig);
    }
    d_->connected = false;
    d_->wq.clear();
    d_->writing = false;
}

void Client::Send(const std::string& line)
{
    if (!d_->connected) return;
    asio::post(d_->io, [this, line] {
        d_->wq.push_back(line + "\n");
        if (!d_->writing) d_->DoWrite();
    });
}

// ---------------------------------------------------------------- one-shot helpers
static bool OneShot(const std::filesystem::path& dataDir, int timeoutMs, const char* command)
{
    const std::string token = ReadToken(dataDir);
    if (token.empty()) return false;
    asio::io_context io;
    tcp::socket s(io);
    asio::streambuf buf(4096);
    if (!ConnectLoopback(io, s, timeoutMs)) return false;
    std::string msg = "AUTH " + token + "\n";
    if (!WriteAll(io, s, msg, timeoutMs)) return false;
    std::string hello;
    if (!ReadLine(io, s, buf, timeoutMs, hello) || hello.rfind("HELLO ", 0) != 0) return false;
    if (command) {
        if (!WriteAll(io, s, std::string(command) + "\n", timeoutMs)) return false;
    }
    boost::system::error_code ig;
    s.close(ig);
    return true;
}

bool Ping(const std::filesystem::path& dataDir, int timeoutMs) { return OneShot(dataDir, timeoutMs, nullptr); }
bool RequestStop(const std::filesystem::path& dataDir) { return OneShot(dataDir, 1500, "STOP"); }

} // namespace ipc
