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
    std::function<void(int, const std::string&, const std::string&, bool)> ready;
    std::function<void(int, const std::string&)> line;
    std::function<void(int, bool)> closed;
};

// One TCP connection, line based protocol:
//   HELLO <name>
//   POS <x*1000> <y*1000>
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket sock, int id, bool incoming, std::string myName, SessionEvents ev)
        : sock_(std::move(sock)), id_(id), incoming_(incoming),
          myName_(std::move(myName)), ev_(std::move(ev)), buf_(4096) {}

    void Start()
    {
        error_code ec;
        sock_.set_option(tcp::no_delay(true), ec);
        auto ep = sock_.remote_endpoint(ec);
        if (!ec) addr_ = ep.address().to_string();
        Send("HELLO " + myName_);
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
        if (!ready_) {
            if (l.rfind("HELLO ", 0) == 0) {
                ready_ = true;
                ev_.ready(id_, l.substr(6), addr_, incoming_);
            }
            return;   // ignore anything before the handshake
        }
        ev_.line(id_, l);
    }

    tcp::socket sock_;
    int id_;
    bool incoming_;
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
    std::shared_ptr<tcp::socket> pendingSock;
    std::shared_ptr<asio::steady_timer> pendingTimer;

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
    };
    std::shared_ptr<ScanState> scan;

    std::uint32_t instance = std::random_device{}();
    std::thread th;

    // ---------------------------------------------------------------- helpers
    void Status(const std::string& s) { if (cb.onStatus) cb.onStatus(s); }

    // ----------------------------------------------------------------- server
    void StartServer()
    {
        if (serverRunning) { Status("Server already running"); return; }
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
        Status("Server listening on TCP port " + std::to_string(kTcpPort));
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
    void Connect(const std::string& ip)
    {
        if (pendingSock) { Status("A connection attempt is already in progress"); return; }
        for (auto& kv : sessions)
            if (kv.second->RemoteAddr() == ip) { Status("Already connected to " + ip); return; }

        error_code ec;
        auto addr = asio::ip::make_address(ip, ec);
        if (ec) { Status("Invalid address: " + ip); return; }

        Status("Connecting to " + ip + " ...");
        pendingSock  = std::make_shared<tcp::socket>(io);
        pendingTimer = std::make_shared<asio::steady_timer>(io, std::chrono::seconds(2));
        auto sock  = pendingSock;
        auto timer = pendingTimer;

        timer->async_wait([sock](error_code e) {
            if (!e) { error_code ig; sock->close(ig); }   // timeout -> abort connect
        });

        sock->async_connect(tcp::endpoint(addr, kTcpPort), [this, ip, sock, timer](error_code e) {
            timer->cancel();
            pendingSock.reset();
            pendingTimer.reset();
            if (stopping) return;
            if (!e) {
                Status("Connected to " + ip);
                AddSession(tcp::socket(std::move(*sock)), false);
            } else {
                Status("No server at " + ip + " (" + e.message() + ") - starting our own");
                StartServer();
            }
        });
    }

    void AddSession(tcp::socket s, bool incoming)
    {
        const int id = nextId++;
        SessionEvents ev;
        ev.ready = [this](int i, const std::string& name, const std::string& addr, bool inc) {
            if (!stopping && cb.onPeerReady) cb.onPeerReady(i, name, addr, inc);
        };
        ev.line = [this](int i, const std::string& l) {
            if (!stopping && cb.onMessage) cb.onMessage(i, l);
        };
        ev.closed = [this](int i, bool wasReady) {
            sessions.erase(i);
            if (!stopping && wasReady && cb.onPeerLost) cb.onPeerLost(i);
        };
        auto session = std::make_shared<Session>(std::move(s), id, incoming, host, std::move(ev));
        sessions[id] = session;
        session->Start();
    }

    // -------------------------------------------------------------- discovery
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
        auto msg = std::make_shared<std::string>(
            "WXV1 " + std::to_string(instance) + " " + (serverRunning ? "1" : "0") + " " + host);
        udpSock.async_send_to(asio::buffer(*msg),
                              udp::endpoint(asio::ip::address_v4::broadcast(), kDiscoveryPort),
                              [msg](error_code, std::size_t) {});
        beaconTimer.expires_after(std::chrono::seconds(2));
        beaconTimer.async_wait([this](error_code ec) { if (!ec) SendBeacon(); });
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
                if (!is.fail() && magic == "WXV1" && id != instance && cb.onBeacon)
                    cb.onBeacon(udpFrom.address().to_string(), h, srv != 0);
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
            if (prefix > 30) continue;           // /31, /32: no hosts to scan
            if (prefix < 22) prefix = 24;        // don't scan huge networks
            const std::uint32_t ip   = a.to_uint();
            const std::uint32_t mask = ~std::uint32_t(0) << (32 - prefix);
            const std::uint32_t net  = ip & mask;
            const std::uint32_t bc   = net | ~mask;
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
        Status("Scanning " + std::to_string(st->targets.size()) + " addresses ...");
        PumpScan(st);
    }

    void PumpScan(const std::shared_ptr<ScanState>& st)
    {
        while (!stopping && st->active < 64 && st->next < st->targets.size())
            Probe(st, st->targets[st->next++]);
        if (st->active == 0 && (stopping || st->next >= st->targets.size()))
            FinishScan(st);
    }

    void Probe(const std::shared_ptr<ScanState>& st, tcp::endpoint ep)
    {
        ++st->active;
        auto sock  = std::make_shared<tcp::socket>(io);
        auto timer = std::make_shared<asio::steady_timer>(io, std::chrono::milliseconds(1500));
        st->socks.insert(sock);

        timer->async_wait([sock](error_code e) {
            if (!e) { error_code ig; sock->close(ig); }
        });
        sock->async_connect(ep, [this, st, ep, sock, timer](error_code e) {
            timer->cancel();
            --st->active;
            st->socks.erase(sock);
            if (!stopping) {
                // connected  -> host up, our server is running
                // refused    -> host up, no server
                // anything else (timeout, unreachable) -> not reported here (ARP may still catch it)
                if (!e || e == asio::error::connection_refused)
                    Report(st, ep.address().to_string(), !e);
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
    }

    void FinishScan(const std::shared_ptr<ScanState>& st)
    {
        if (!stopping) {
            // Firewalled hosts drop our SYN but still answered ARP.
            std::set<std::string> inScope;
            for (auto& t : st->targets) inScope.insert(t.address().to_string());
            for (auto& ip : ReadArpTable())
                if (inScope.count(ip) && !st->reported.count(ip)) Report(st, ip, false);

            Status("Scan finished: " + std::to_string(st->reported.size()) + " host(s) found");
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
        { error_code ig; if (scan) for (auto& s : scan->socks) s->close(ig); }
        error_code ec;
        acceptor.close(ec);
        udpSock.close(ec);
        beaconTimer.cancel();
        if (pendingSock)  pendingSock->close(ec);
        if (pendingTimer) pendingTimer->cancel();
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
    asio::post(d_->io, [this] { d_->StartDiscovery(); });
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
        if (isLocalAddress) d_->StartServer();
        else                d_->Connect(ip);
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
