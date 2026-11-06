#include "network.hpp"

#include <boost/asio.hpp>

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "identity.hpp"
#include "localinfo.hpp"

namespace asio = boost::asio;
using asio::ip::tcp;
using asio::ip::udp;
using boost::system::error_code;

namespace {

struct SessionEvents {
    // (session id, peer instance, peer public key, peer name, peer address, incoming, viaGroup)
    // returns false to reject the session
    std::function<bool(int, std::uint32_t, const std::string&, const std::string&,
                       const std::string&, bool, bool)> ready;
    // authenticated but not accepted (yet); (..., theyTrustMe)
    std::function<void(int, std::uint32_t, const std::string&, const std::string&,
                       const std::string&, bool, bool)> pending;
    // key proven: returns false when another session to the same peer wins (this one is dropped)
    std::function<bool(int, std::uint32_t, const std::string&, bool)> authed;
    std::function<void(int, const std::string&)> line;
    std::function<void(int, bool wasReady, bool wasPending)> closed;
    std::function<std::string()> groupSecret;               // empty: no group
    std::function<bool(const std::string&)> trusted;        // do WE trust this key?
};

// One TCP connection, line based protocol:
//   HELLO <instance> <public key hex> <nonce hex> <name>
//   AUTH <signature hex> [<group proof>]
//                             proves ownership of the key announced in HELLO:
//                             sign("si-auth-v1|<my key>|<your key>|<your nonce>")
//                             group proof = H(group secret, my key, your key, your nonce): shows
//                             we know the same group password without revealing it
//   TRUST <0|1> <signature hex>   "I (do not) trust you", signed with the key and bound to
//                             both nonces, so it cannot be replayed into another session
//   (a peer is ACCEPTED when its group proof matches, or when both sides sent TRUST 1;
//    only accepted peers get:)
//   SCREENS ..., POS ..., and the input protocol (see input.hpp)
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket sock, int id, bool incoming, std::uint32_t myInstance,
            std::string myName, const Identity& ident, SessionEvents ev)
        : sock_(std::move(sock)), id_(id), incoming_(incoming), myInstance_(myInstance),
          myName_(std::move(myName)), ident_(ident), ev_(std::move(ev)), buf_(4096),
          myNonce_(Identity::RandomHex(16)), hb_(sock_.get_executor()) {}

    void Start()
    {
        error_code ec;
        sock_.set_option(tcp::no_delay(true), ec);
        auto ep = sock_.remote_endpoint(ec);
        if (!ec) addr_ = ep.address().to_string();
        Send("HELLO " + std::to_string(myInstance_) + " " + ident_.PublicHex() + " " + myNonce_ + " " + myName_);
        lastRx_ = std::chrono::steady_clock::now();
        Heartbeat();
        DoRead();
    }

    const std::string& RemoteAddr() const { return addr_; }
    bool Authed() const { return authed_ && !closed_; }
    const std::string& PeerKey() const { return peerKey_; }
    std::uint32_t PeerInstance() const { return peerInst_; }
    bool Incoming() const { return incoming_; }
    bool Ready() const { return ready_; }
    // The one who opened the TCP connection (identical on both ends)
    std::uint32_t Initiator(std::uint32_t myInst) const { return incoming_ ? peerInst_ : myInst; }

    // our trust list or the group changed
    void OnPolicyChanged()
    {
        if (!Authed()) return;
        SendTrust();
        Evaluate();
    }

    void Send(const std::string& line)
    {
        if (closed_) return;
        const bool idle = out_.empty();
        out_.push_back(line + "\n");
        if (idle) DoWrite();
    }

    void Close()
    {
        if (closed_) return;
        closed_ = true;
        error_code ec;
        hb_.cancel(ec);
        sock_.shutdown(tcp::socket::shutdown_both, ec);
        sock_.close(ec);
        auto self = shared_from_this();
        asio::post(sock_.get_executor(), [self] { self->ev_.closed(self->id_, self->ready_, self->authed_ && !self->ready_); });
    }

private:
    // Liveness: a PC that went to sleep (or lost the network) never closes the TCP link, so
    // without this the controller would keep forwarding mouse/keyboard into the void and the
    // user would be stuck. Both sides send PING every second; silence for 5 s drops the link,
    // which hands the keyboard/mouse back to this PC (see OnPeerLost -> RemovePeer).
    void Heartbeat()
    {
        auto self = shared_from_this();
        error_code ec;
        hb_.expires_after(std::chrono::seconds(1), ec);
        hb_.async_wait([self](error_code e) {
            if (e || self->closed_) return;
            if (std::chrono::steady_clock::now() - self->lastRx_ > std::chrono::seconds(5)) {
                self->Close();
                return;
            }
            if (self->gotHello_) self->Send("PING");
            self->Heartbeat();
        });
    }

    void DoRead()
    {
        auto self = shared_from_this();
        asio::async_read_until(sock_, buf_, '\n', [self](error_code ec, std::size_t) {
            if (ec) { self->Close(); return; }
            self->lastRx_ = std::chrono::steady_clock::now();
            std::istream is(&self->buf_);
            std::string l;
            std::getline(is, l);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            self->Handle(l);
            if (!self->closed_) self->DoRead();
        });
    }

    void DoWrite()
    {
        auto self = shared_from_this();
        asio::async_write(sock_, asio::buffer(out_.front()), [self](error_code ec, std::size_t) {
            if (ec) { self->Close(); return; }
            self->out_.pop_front();
            if (!self->out_.empty()) self->DoWrite();
        });
    }

    static std::string AuthMsg(const std::string& signerKey, const std::string& verifierKey,
                               const std::string& verifierNonce)
    {
        return "si-auth-v1|" + signerKey + "|" + verifierKey + "|" + verifierNonce;
    }

    static bool IsNonce(const std::string& s)
    {
        if (s.size() != 32) return false;
        for (char c : s) if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        return true;
    }

    static std::string GroupProof(const std::string& secret, const std::string& signerKey,
                                  const std::string& verifierKey, const std::string& verifierNonce)
    {
        return Identity::HashHex("si-group-v1|" + secret + "|" + signerKey + "|" + verifierKey + "|" +
                                 verifierNonce, 16);
    }

    static std::string TrustMsg(const std::string& signerKey, const std::string& verifierKey,
                                const std::string& signerNonce, const std::string& verifierNonce, bool v)
    {
        return "si-trust-v1|" + signerKey + "|" + verifierKey + "|" + signerNonce + "|" + verifierNonce +
               "|" + (v ? "1" : "0");
    }

    void SendTrust()
    {
        const bool t = ev_.trusted(peerKey_);
        if (t == sentTrust_ && sentOnce_) return;
        if (!t && !sentOnce_) { sentTrust_ = false; return; }   // nothing to retract
        sentTrust_ = t;
        sentOnce_ = true;
        Send(std::string("TRUST ") + (t ? "1 " : "0 ") +
             ident_.Sign(TrustMsg(ident_.PublicHex(), peerKey_, myNonce_, peerNonce_, t)));
    }

    void HandleTrust(const std::string& rest)
    {
        std::istringstream is(rest);
        int v = -1;
        std::string sig;
        if (!(is >> v >> sig) || (v != 0 && v != 1)) return;
        if (!Identity::Verify(peerKey_, TrustMsg(peerKey_, ident_.PublicHex(), peerNonce_, myNonce_, v == 1), sig))
            return;                                 // not signed by that peer for this session
        theyTrust_ = v == 1;
        Evaluate();
    }

    // Decide what this session may do: accepted -> normal peer; otherwise it waits.
    void Evaluate()
    {
        if (!authed_ || closed_) return;
        const bool accepted = groupOk_ || (theyTrust_ && ev_.trusted(peerKey_));
        if (accepted) {
            if (!ready_) {
                if (!ev_.ready(id_, peerInst_, peerKey_, peerName_, addr_, incoming_, groupOk_)) { Close(); return; }
                ready_ = true;
            }
        } else if (ready_) {
            Close();                                // trust withdrawn / group changed
        } else {
            ev_.pending(id_, peerInst_, peerKey_, peerName_, addr_, incoming_, theyTrust_);
        }
    }

    void Handle(const std::string& l)
    {
        if (l == "PING") return;                    // liveness only; receiving it is what counts
        if (authed_) {
            if (l.rfind("TRUST ", 0) == 0) { HandleTrust(l.substr(6)); return; }
            if (ready_) ev_.line(id_, l);           // pending peers may only talk about trust
            return;
        }

        if (!gotHello_) {
            if (l.rfind("HELLO ", 0) != 0) return;   // ignore anything before the handshake
            std::istringstream is(l.substr(6));
            std::uint32_t inst = 0;
            std::string key, nonce;
            if (!(is >> inst >> key >> nonce) || inst == 0 ||
                !Identity::IsValidPublicHex(key) || !IsNonce(nonce)) { Close(); return; }
            if (key == ident_.PublicHex()) { Close(); return; }   // that is us
            std::string name;
            std::getline(is >> std::ws, name);
            if (name.empty()) name = addr_;

            peerInst_ = inst; peerKey_ = key; peerNonce_ = nonce; peerName_ = name;
            gotHello_ = true;
            // Prove that we own our key: sign the challenge the peer just gave us.
            std::string auth = "AUTH " + ident_.Sign(AuthMsg(ident_.PublicHex(), peerKey_, peerNonce_));
            const std::string secret = ev_.groupSecret();
            if (!secret.empty()) auth += " " + GroupProof(secret, ident_.PublicHex(), peerKey_, peerNonce_);
            Send(auth);
            return;
        }

        if (l.rfind("AUTH ", 0) != 0) return;
        std::istringstream is(l.substr(5));
        std::string sig, proof;
        is >> sig >> proof;
        if (!Identity::Verify(peerKey_, AuthMsg(peerKey_, ident_.PublicHex(), myNonce_), sig)) {
            Close();                                // peer does not own the key it claims
            return;
        }
        const std::string secret = ev_.groupSecret();
        groupOk_ = !secret.empty() && !proof.empty() &&
                   proof == GroupProof(secret, peerKey_, ident_.PublicHex(), myNonce_);
        authed_ = true;
        if (!ev_.authed(id_, peerInst_, peerKey_, incoming_)) { Close(); return; }
        SendTrust();
        Evaluate();
    }

    tcp::socket sock_;
    int id_;
    bool incoming_;
    std::uint32_t myInstance_;
    std::string myName_;
    const Identity& ident_;
    SessionEvents ev_;
    asio::streambuf buf_;
    std::string myNonce_;
    std::string peerKey_, peerNonce_, peerName_;
    std::uint32_t peerInst_ = 0;
    bool gotHello_ = false;
    std::deque<std::string> out_;
    std::string addr_;
    bool authed_ = false;       // peer proved its key
    bool groupOk_ = false;      // peer knows our group password
    bool theyTrust_ = false;    // peer sent a valid signed "TRUST 1"
    bool sentTrust_ = false, sentOnce_ = false;
    bool ready_  = false;       // accepted: carries input / audio
    bool closed_ = false;
    std::chrono::steady_clock::time_point lastRx_;
    asio::steady_timer hb_;
};

} // namespace

struct Network::Impl {
    Impl(std::string h, Identity i, NetCallbacks c) : host(std::move(h)), ident(std::move(i)), cb(std::move(c)) {}

    std::string host;
    Identity ident;
    NetCallbacks cb;

    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard{asio::make_work_guard(io)};
    tcp::acceptor acceptor{io};
    udp::socket udpSock{io};
    asio::steady_timer beaconTimer{io};
    udp::endpoint udpFrom;
    std::array<char, 512> udpBuf{};

    std::map<int, std::shared_ptr<Session>> sessions;
    std::map<std::uint32_t, int> byInstance;            // peer instance -> session id
    std::set<std::uint32_t> connectingInst;
    std::set<std::string> connectingIp;
    std::set<std::shared_ptr<tcp::socket>> connSocks;
    std::set<std::uint32_t> knownInst;                  // peers whose beacon we have seen
    std::map<std::uint32_t, std::chrono::steady_clock::time_point> lastTry;

    std::string groupSecret;                            // derived from the group password; empty = none
    std::set<std::string> trusted;                      // public keys WE trust

    int nextId = 1;
    bool serverRunning = false;
    bool stopping = false;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);

    struct ScanState {
        std::vector<tcp::endpoint> targets;
        std::set<std::string> reported;
        std::set<std::shared_ptr<tcp::socket>> socks;
        std::size_t next = 0;
        int active = 0;
        int open = 0, refused = 0, silent = 0, other = 0;
        std::string firstOtherErr;
    };
    std::shared_ptr<ScanState> scan;

    std::uint32_t instance = [] {
        std::uint32_t v = 0;
        while (v == 0) v = std::random_device{}();
        return v;
    }();
    std::thread th;

    // ---------------------------------------------------------------- helpers
    void Status(const std::string& s) { if (cb.onStatus) cb.onStatus(s); }

    // ----------------------------------------------------------------- server
    void StartServer()
    {
        if (serverRunning) return;
        error_code ec;
        tcp::endpoint ep(tcp::v4(), kTcpPort);
        acceptor.open(ep.protocol(), ec);
        if (!ec) acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
        if (!ec) acceptor.bind(ep, ec);
        if (!ec) acceptor.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            error_code ig;
            acceptor.close(ig);
            Status("Cannot start server: " + ec.message());
            return;
        }
        serverRunning = true;
        Status("Listening on TCP port " + std::to_string(kTcpPort));
        DoAccept();
    }

    void DoAccept()
    {
        acceptor.async_accept([this](error_code ec, tcp::socket s) {
            if (ec == asio::error::operation_aborted || stopping) return;
            if (!ec) AddSession(std::move(s), true);
            if (acceptor.is_open()) DoAccept();
        });
    }

    // ----------------------------------------------------------------- client
    // inst == 0 for a manual connect (peer id unknown until HELLO).
    void Connect(const std::string& ip, std::uint32_t inst, bool manual)
    {
        if (connectingIp.count(ip)) { if (manual) Status("Already connecting to " + ip); return; }
        for (auto& kv : sessions)
            if (kv.second->RemoteAddr() == ip) { if (manual) Status("Already connected to " + ip); return; }
        if (inst && (HasSessionFor(inst) || connectingInst.count(inst))) return;

        error_code ec;
        auto addr = asio::ip::make_address(ip, ec);
        if (ec) { Status("Invalid address: " + ip); return; }

        Status("Connecting to " + ip + " ...");
        connectingIp.insert(ip);
        if (inst) connectingInst.insert(inst);

        auto sock  = std::make_shared<tcp::socket>(io);
        auto timer = std::make_shared<asio::steady_timer>(io, std::chrono::seconds(3));
        connSocks.insert(sock);

        timer->async_wait([sock](error_code e) {
            if (!e) { error_code ig; sock->close(ig); }   // timeout -> abort connect
        });

        sock->async_connect(tcp::endpoint(addr, kTcpPort), [this, ip, inst, sock, timer](error_code e) {
            timer->cancel();
            connSocks.erase(sock);
            connectingIp.erase(ip);
            if (inst) connectingInst.erase(inst);
            if (stopping) return;
            if (!e) {
                Status("Connected to " + ip);
                AddSession(tcp::socket(std::move(*sock)), false);
            } else {
                Status("Cannot connect to " + ip + " (" + e.message() + ")");
            }
        });
    }

    // any authenticated session to this peer instance (accepted or still pending)?
    bool HasSessionFor(std::uint32_t inst) const
    {
        for (auto& kv : sessions)
            if (kv.second->Authed() && kv.second->PeerInstance() == inst) return true;
        return false;
    }

    // A session just proved its peer's key. At most one session per peer survives; both ends
    // pick the same one (the one opened by the lower instance id), so they cannot both drop.
    bool OnAuthed(int id, std::uint32_t inst, const std::string& key, bool incoming)
    {
        if (stopping) return false;
        const std::uint32_t mine = incoming ? inst : instance;
        for (auto& kv : sessions) {
            if (kv.first == id) continue;
            auto& o = kv.second;
            if (!o->Authed() || o->PeerKey() != key) continue;
            if (o->PeerInstance() != inst) { o->Close(); continue; }   // peer restarted: old link is dead
            if (mine < o->Initiator(instance)) o->Close();
            else { Status("Duplicate connection to " + key.substr(0, 8) + " dropped"); return false; }
        }
        return true;
    }

    void AddSession(tcp::socket s, bool incoming)
    {
        const int id = nextId++;
        SessionEvents ev;
        ev.ready = [this](int i, std::uint32_t inst, const std::string& key, const std::string& name,
                          const std::string& addr, bool inc, bool viaGroup) -> bool {
            if (stopping) return false;
            byInstance[inst] = i;
            if (cb.onPeerReady) cb.onPeerReady(i, inst, key, name, addr, inc, viaGroup);
            return true;
        };
        ev.pending = [this](int i, std::uint32_t inst, const std::string& key, const std::string& name,
                            const std::string& addr, bool inc, bool theyTrust) {
            if (!stopping && cb.onPeerPending) cb.onPeerPending(i, inst, key, name, addr, inc, theyTrust);
        };
        ev.authed = [this](int i, std::uint32_t inst, const std::string& key, bool inc) {
            return OnAuthed(i, inst, key, inc);
        };
        ev.line = [this](int i, const std::string& l) {
            if (!stopping && cb.onMessage) cb.onMessage(i, l);
        };
        ev.closed = [this](int i, bool wasReady, bool wasPending) {
            sessions.erase(i);
            for (auto it = byInstance.begin(); it != byInstance.end();) {
                if (it->second == i) it = byInstance.erase(it); else ++it;
            }
            if (stopping) return;
            if (wasReady && cb.onPeerLost) cb.onPeerLost(i);
            if (wasPending && cb.onPeerPendingLost) cb.onPeerPendingLost(i);
        };
        ev.groupSecret = [this] { return groupSecret; };
        ev.trusted = [this](const std::string& k) { return trusted.count(k) > 0; };
        auto session = std::make_shared<Session>(std::move(s), id, incoming, instance, host, ident, std::move(ev));
        sessions[id] = session;
        session->Start();
    }

    // -------------------------------------------------------------- discovery
    // The tag lets peers of the same group recognise each other without a connection; it is
    // derived from the (slow-hashed) group secret and says nothing to anyone else.
    static std::string BeaconTag(const std::string& secret, std::uint32_t inst, const std::string& key)
    {
        if (secret.empty()) return "-";
        return Identity::HashHex("si-gbeacon-v1|" + secret + "|" + std::to_string(inst) + "|" + key, 8);
    }

    std::string BeaconMsg() const
    {
        return "WXV2 " + std::to_string(instance) + " " + (serverRunning ? "1" : "0") + " " +
               ident.PublicHex() + " " + BeaconTag(groupSecret, instance, ident.PublicHex()) + " " + host;
    }

    void StartDiscovery()
    {
        error_code ec;
        udpSock.open(udp::v4(), ec);
        if (!ec) udpSock.set_option(asio::socket_base::reuse_address(true), ec);
        if (!ec) udpSock.set_option(asio::socket_base::broadcast(true), ec);
        if (!ec) udpSock.bind(udp::endpoint(udp::v4(), kDiscoveryPort), ec);
        if (ec) { Status("LAN discovery unavailable: " + ec.message()); return; }
        DoReceive();
        SendBeacon();
    }

    void SendBeacon()
    {
        if (stopping || !udpSock.is_open()) return;
        auto msg = std::make_shared<std::string>(BeaconMsg());
        udpSock.async_send_to(asio::buffer(*msg),
                              udp::endpoint(asio::ip::address_v4::broadcast(), kDiscoveryPort),
                              [msg](error_code, std::size_t) {});
        beaconTimer.expires_after(std::chrono::seconds(2));
        beaconTimer.async_wait([this](error_code ec) { if (!ec) SendBeacon(); });
    }

    // Direct beacon to one host (works across subnets / where broadcast is blocked).
    void SendBeaconTo(const std::string& ip)
    {
        if (stopping || !udpSock.is_open()) return;
        error_code ec;
        auto a = asio::ip::make_address(ip, ec);
        if (ec) return;
        auto msg = std::make_shared<std::string>(BeaconMsg());
        udpSock.async_send_to(asio::buffer(*msg), udp::endpoint(a, kDiscoveryPort),
                              [msg](error_code, std::size_t) {});
    }

    // A seamless-input instance announced itself. We connect to it when it is in our group
    // (the LOWER instance id connects, so each pair gets one connection) or when we trust its key
    // (then we always connect: it may not trust us yet and would never come to us).
    void OnPeerBeacon(const BeaconInfo& b)
    {
        if (knownInst.insert(b.instance).second) SendBeaconTo(b.ip);   // new peer: answer directly
        const bool trustedPeer = trusted.count(b.key) > 0;
        if (!b.server || !(b.sameGroup || trustedPeer)) return;
        if (!trustedPeer && instance > b.instance) return;
        if (HasSessionFor(b.instance) || connectingInst.count(b.instance)) return;

        auto now = std::chrono::steady_clock::now();
        auto it = lastTry.find(b.instance);
        if (it != lastTry.end() && now - it->second < std::chrono::seconds(10)) return;
        lastTry[b.instance] = now;

        Status("Found seamless-input peer at " + b.ip + (b.sameGroup ? " (same group)" : " (trusted)") +
               " - connecting");
        Connect(b.ip, b.instance, false);
    }

    void ApplyGroup(const std::string& secret)
    {
        groupSecret = secret;
        lastTry.clear();
        knownInst.clear();                // answer every peer's next beacon again
        auto copy = sessions;             // links made under the old group are re-judged by reconnecting
        for (auto& kv : copy) kv.second->Close();
        SendBeacon();
    }

    void ApplyTrust(const std::string& key, bool on, const std::string& ip)
    {
        if (on) trusted.insert(key); else trusted.erase(key);
        bool found = false;
        auto copy = sessions;
        for (auto& kv : copy)
            if (kv.second->Authed() && kv.second->PeerKey() == key) { kv.second->OnPolicyChanged(); found = true; }
        if (on && !found && !ip.empty()) Connect(ip, 0, true);
    }

    void DoReceive()
    {
        udpSock.async_receive_from(asio::buffer(udpBuf), udpFrom, [this](error_code ec, std::size_t n) {
            if (ec == asio::error::operation_aborted || stopping) return;
            if (!ec) {
                std::istringstream is(std::string(udpBuf.data(), n));
                std::string magic, key, tag, h;
                std::uint32_t id = 0;
                int srv = 0;
                is >> magic >> id >> srv >> key >> tag;
                std::getline(is >> std::ws, h);
                if (!is.fail() && magic == "WXV2" && id != instance && key != ident.PublicHex() &&
                    Identity::IsValidPublicHex(key)) {
                    BeaconInfo b;
                    b.ip = udpFrom.address().to_string();
                    b.host = h;
                    b.key = key;
                    b.instance = id;
                    b.server = srv != 0;
                    b.sameGroup = !groupSecret.empty() && tag == BeaconTag(groupSecret, id, key);
                    if (cb.onBeacon) cb.onBeacon(b);
                    OnPeerBeacon(b);
                }
            }
            DoReceive();
        });
    }

    // ------------------------------------------------------------------- scan
    void StartScan(const std::vector<std::pair<std::string, int>>& nets)
    {
        if (scan || stopping) return;
        auto st = std::make_shared<ScanState>();

        std::set<std::string> own;
        for (auto& n : nets) own.insert(n.first);

        std::set<std::uint32_t> seen;
        for (auto& n : nets) {
            error_code ec;
            auto a = asio::ip::make_address_v4(n.first, ec);
            if (ec) continue;
            int prefix = n.second;
            if (prefix > 30) { Status("Skipping " + n.first + "/" + std::to_string(prefix) + " (too small)"); continue; }
            if (prefix < 16) prefix = 16;        // allow up to a /16 (65k addresses)
            const std::uint32_t ip   = a.to_uint();
            const std::uint32_t mask = ~std::uint32_t(0) << (32 - prefix);
            const std::uint32_t net  = ip & mask;
            const std::uint32_t bc   = net | ~mask;
            Status("Scanning " + asio::ip::address_v4(net).to_string() + "/" +
                   std::to_string(prefix) + " (from " + n.first + ")");
            for (std::uint32_t h = net + 1; h < bc; ++h) {
                asio::ip::address_v4 ha(h);
                if (own.count(ha.to_string()) || !seen.insert(h).second) continue;
                st->targets.emplace_back(ha, kTcpPort);
            }
        }

        if (st->targets.empty()) {
            Status("Nothing to scan");
            if (cb.onScanDone) cb.onScanDone();
            return;
        }
        scan = st;
        Status("Probing " + std::to_string(st->targets.size()) + " addresses ...");
        PumpScan(st);
    }

    void PumpScan(const std::shared_ptr<ScanState>& st)
    {
        while (!stopping && st->active < 256 && st->next < st->targets.size())
            Probe(st, st->targets[st->next++]);
        if (st->active == 0 && (stopping || st->next >= st->targets.size()))
            FinishScan(st);
    }

    void Probe(const std::shared_ptr<ScanState>& st, tcp::endpoint ep)
    {
        ++st->active;
        auto sock  = std::make_shared<tcp::socket>(io);
        auto timer = std::make_shared<asio::steady_timer>(io, std::chrono::milliseconds(600));
        st->socks.insert(sock);

        timer->async_wait([sock](error_code e) {
            if (!e) { error_code ig; sock->close(ig); }
        });
        sock->async_connect(ep, [this, st, ep, sock, timer](error_code e) {
            timer->cancel();
            --st->active;
            st->socks.erase(sock);
            if (!stopping) {
                const std::string ip = ep.address().to_string();
                if (!e)                                        { ++st->open;    Report(st, ip, true);  }
                else if (e == asio::error::connection_refused) { ++st->refused; Report(st, ip, false); }
                else if (e == asio::error::operation_aborted ||
                         e == asio::error::host_unreachable ||
                         e == asio::error::timed_out)          { ++st->silent; }
                else {
                    ++st->other;
                    if (st->firstOtherErr.empty()) st->firstOtherErr = e.message();
                }
            }
            error_code ig;
            sock->close(ig);
            PumpScan(st);
        });
    }

    void Report(const std::shared_ptr<ScanState>& st, const std::string& ip, bool server)
    {
        st->reported.insert(ip);
        if (cb.onScanHost) cb.onScanHost(ip, server);
        SendBeaconTo(ip);   // if it runs seamless-input it will answer and the mesh forms
    }

    void FinishScan(const std::shared_ptr<ScanState>& st)
    {
        if (!stopping) {
            // Firewalled hosts drop our SYN but still answered ARP.
            std::set<std::string> inScope;
            for (auto& t : st->targets) inScope.insert(t.address().to_string());
            int viaArp = 0;
            for (auto& ip : ReadArpTable())
                if (inScope.count(ip) && !st->reported.count(ip)) { Report(st, ip, false); ++viaArp; }

            std::string msg = "Scan finished: " + std::to_string(st->reported.size()) + " host(s) found ("
                + std::to_string(st->open) + " with server, "
                + std::to_string(st->refused) + " online, "
                + std::to_string(viaArp) + " via ARP, "
                + std::to_string(st->silent) + " silent)";
            if (st->other)
                msg += ", " + std::to_string(st->other) + " errors (" + st->firstOtherErr + ")";
            Status(msg);

            ResolveNames({st->reported.begin(), st->reported.end()});
            if (cb.onScanDone) cb.onScanDone();
        }
        scan.reset();
    }

    // Reverse DNS can block for seconds, so it runs on its own thread and is
    // never joined; it only touches the 'alive' flag and a copy of the callback.
    void ResolveNames(std::vector<std::string> ips)
    {
        if (ips.empty() || !cb.onHostName) return;
        auto flag = alive;
        auto fn   = cb.onHostName;
        std::thread([ips = std::move(ips), flag, fn] {
            asio::io_context ctx;
            tcp::resolver r(ctx);
            for (const auto& ip : ips) {
                if (!*flag) return;
                error_code ec;
                auto a = asio::ip::make_address(ip, ec);
                if (ec) continue;
                auto res = r.resolve(tcp::endpoint(a, kTcpPort), ec);
                if (ec || res.empty()) continue;
                std::string n = res.begin()->host_name();
                if (!n.empty() && n != ip && *flag) fn(ip, n);
            }
        }).detach();
    }

    // --------------------------------------------------------------- shutdown
    void Shutdown()
    {
        stopping = true;
        *alive = false;
        error_code ec;
        if (scan) for (auto& s : scan->socks) s->close(ec);
        for (auto& s : connSocks) s->close(ec);
        acceptor.close(ec);
        udpSock.close(ec);
        beaconTimer.cancel();
        auto copy = sessions;
        for (auto& kv : copy) kv.second->Close();
    }
};

Network::Network(std::string hostName, Identity identity, NetCallbacks cb)
    : d_(new Impl(std::move(hostName), std::move(identity), std::move(cb))) {}

Network::~Network() { Stop(); }

void Network::Start()
{
    if (d_->th.joinable()) return;
    d_->th = std::thread([this] { d_->io.run(); });
    asio::post(d_->io, [this] {
        d_->StartServer();      // every instance always listens
        d_->StartDiscovery();
    });
}

void Network::Stop()
{
    if (!d_->th.joinable()) return;
    asio::post(d_->io, [this] { d_->Shutdown(); });
    d_->guard.reset();
    d_->th.join();
}

void Network::Select(const std::string& ip, bool isLocalAddress)
{
    asio::post(d_->io, [this, ip, isLocalAddress] {
        if (isLocalAddress) {
            d_->StartServer();   // no-op if already listening
            d_->Status("This machine is already listening");
        } else {
            d_->Connect(ip, 0, true);
        }
    });
}

void Network::SetGroup(const std::string& password)
{
    asio::post(d_->io, [this, password] { d_->ApplyGroup(Identity::GroupSecret(password)); });
}

void Network::SetTrust(const std::string& key, bool trusted, const std::string& ip)
{
    asio::post(d_->io, [this, key, trusted, ip] { d_->ApplyTrust(key, trusted, ip); });
}

void Network::Send(int id, const std::string& line)
{
    asio::post(d_->io, [this, id, line] {
        auto it = d_->sessions.find(id);
        if (it != d_->sessions.end()) it->second->Send(line);
    });
}

void Network::Scan(const std::vector<std::pair<std::string, int>>& nets)
{
    asio::post(d_->io, [this, nets] { d_->StartScan(nets); });
}

std::uint32_t Network::Instance() const { return d_->instance; }
