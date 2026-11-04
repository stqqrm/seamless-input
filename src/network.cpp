#include "network.hpp"

#include <boost/asio.hpp>

#include <array>
#include <atomic>
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

#include "localinfo.hpp"

namespace asio = boost::asio;
using asio::ip::tcp;
using asio::ip::udp;
using boost::system::error_code;

namespace {

struct SessionEvents {
    // returns false to reject the session (duplicate peer)
    std::function<bool(int, std::uint32_t, const std::string&, const std::string&, bool)> ready;
    std::function<void(int, const std::string&)> line;
    std::function<void(int, bool)> closed;
};

// One TCP connection, line based protocol:
//   HELLO <instance> <name>
//   POS <x*1000> <y*1000>
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket sock, int id, bool incoming, std::uint32_t myInstance,
            std::string myName, SessionEvents ev)
        : sock_(std::move(sock)), id_(id), incoming_(incoming), myInstance_(myInstance),
          myName_(std::move(myName)), ev_(std::move(ev)), buf_(4096) {}

    void Start()
    {
        error_code ec;
        sock_.set_option(tcp::no_delay(true), ec);
        auto ep = sock_.remote_endpoint(ec);
        if (!ec) addr_ = ep.address().to_string();
        Send("HELLO " + std::to_string(myInstance_) + " " + myName_);
        DoRead();
    }

    const std::string& RemoteAddr() const { return addr_; }

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
        sock_.shutdown(tcp::socket::shutdown_both, ec);
        sock_.close(ec);
        auto self = shared_from_this();
        asio::post(sock_.get_executor(), [self] { self->ev_.closed(self->id_, self->ready_); });
    }

private:
    void DoRead()
    {
        auto self = shared_from_this();
        asio::async_read_until(sock_, buf_, '\n', [self](error_code ec, std::size_t) {
            if (ec) { self->Close(); return; }
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

    void Handle(const std::string& l)
    {
        if (ready_) { ev_.line(id_, l); return; }

        if (l.rfind("HELLO ", 0) != 0) return;   // ignore anything before the handshake
        std::istringstream is(l.substr(6));
        std::uint32_t inst = 0;
        if (!(is >> inst) || inst == 0) return;
        std::string name;
        std::getline(is >> std::ws, name);
        if (name.empty()) name = addr_;

        if (!ev_.ready(id_, inst, name, addr_, incoming_)) { Close(); return; }
        ready_ = true;
    }

    tcp::socket sock_;
    int id_;
    bool incoming_;
    std::uint32_t myInstance_;
    std::string myName_;
    SessionEvents ev_;
    asio::streambuf buf_;
    std::deque<std::string> out_;
    std::string addr_;
    bool ready_  = false;
    bool closed_ = false;
};

} // namespace

struct Network::Impl {
    Impl(std::string h, NetCallbacks c) : host(std::move(h)), cb(std::move(c)) {}

    std::string host;
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
        if (inst && (byInstance.count(inst) || connectingInst.count(inst))) return;

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

    void AddSession(tcp::socket s, bool incoming)
    {
        const int id = nextId++;
        SessionEvents ev;
        ev.ready = [this](int i, std::uint32_t inst, const std::string& name,
                          const std::string& addr, bool inc) -> bool {
            if (stopping) return false;
            if (byInstance.count(inst)) {            // already linked to this peer
                Status("Duplicate connection to " + name + " dropped");
                return false;
            }
            byInstance[inst] = i;
            if (cb.onPeerReady) cb.onPeerReady(i, name, addr, inc);
            return true;
        };
        ev.line = [this](int i, const std::string& l) {
            if (!stopping && cb.onMessage) cb.onMessage(i, l);
        };
        ev.closed = [this](int i, bool wasReady) {
            sessions.erase(i);
            for (auto it = byInstance.begin(); it != byInstance.end();) {
                if (it->second == i) it = byInstance.erase(it); else ++it;
            }
            if (!stopping && wasReady && cb.onPeerLost) cb.onPeerLost(i);
        };
        auto session = std::make_shared<Session>(std::move(s), id, incoming, instance, host, std::move(ev));
        sessions[id] = session;
        session->Start();
    }

    // -------------------------------------------------------------- discovery
    std::string BeaconMsg() const
    {
        return "WXV1 " + std::to_string(instance) + " " + (serverRunning ? "1" : "0") + " " + host;
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

    // A seamless-input instance announced itself. The LOWER instance id connects
    // to the higher one, so each pair gets exactly one connection (full mesh).
    void OnPeerBeacon(const std::string& ip, std::uint32_t id, bool srv)
    {
        if (knownInst.insert(id).second) SendBeaconTo(ip);   // new peer: answer directly
        if (!srv || instance > id) return;
        if (byInstance.count(id) || connectingInst.count(id)) return;

        auto now = std::chrono::steady_clock::now();
        auto it = lastTry.find(id);
        if (it != lastTry.end() && now - it->second < std::chrono::seconds(10)) return;
        lastTry[id] = now;

        Status("Found seamless-input peer at " + ip + " - connecting automatically");
        Connect(ip, id, false);
    }

    void DoReceive()
    {
        udpSock.async_receive_from(asio::buffer(udpBuf), udpFrom, [this](error_code ec, std::size_t n) {
            if (ec == asio::error::operation_aborted || stopping) return;
            if (!ec) {
                std::istringstream is(std::string(udpBuf.data(), n));
                std::string magic, h;
                std::uint32_t id = 0;
                int srv = 0;
                is >> magic >> id >> srv;
                std::getline(is >> std::ws, h);
                if (!is.fail() && magic == "WXV1" && id != instance) {
                    const std::string ip = udpFrom.address().to_string();
                    if (cb.onBeacon) cb.onBeacon(ip, h, srv != 0);
                    OnPeerBeacon(ip, id, srv != 0);
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

Network::Network(std::string hostName, NetCallbacks cb)
    : d_(new Impl(std::move(hostName), std::move(cb))) {}

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
