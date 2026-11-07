#include "ipc_server.hpp"

#include <deque>
#include <fstream>
#include <future>

#include "identity.hpp"
#include "ipc.hpp"

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <unistd.h>
#endif

#ifndef SI_VERSION
  #define SI_VERSION "dev"
#endif

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace ipc {

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(Server& srv, tcp::socket s) : srv_(srv), sock_(std::move(s)), buf_(8192), authTimer_(sock_.get_executor()) {}

    void Start()
    {
        auto self = shared_from_this();
        authTimer_.expires_after(std::chrono::seconds(5));
        authTimer_.async_wait([self](const boost::system::error_code& ec) { if (!ec && !self->authed_) self->Close(); });
        Read();
    }

    void Send(const std::string& line)
    {
        if (closed_) return;
        wq_.push_back(line + "\n");
        if (!writing_) Write();
    }

    void Close()
    {
        if (closed_) return;
        closed_ = true;
        boost::system::error_code ig;
        sock_.close(ig);
        authTimer_.cancel();
        srv_.Remove(shared_from_this());
    }

    bool attached = false;

private:
    void Read()
    {
        auto self = shared_from_this();
        asio::async_read_until(sock_, buf_, '\n', [self](const boost::system::error_code& ec, std::size_t) {
            if (ec) { self->Close(); return; }
            std::istream is(&self->buf_);
            std::string line;
            std::getline(is, line);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            self->Handle(line);
            if (!self->closed_) self->Read();
        });
    }

    void Write()
    {
        if (wq_.empty()) { writing_ = false; return; }
        writing_ = true;
        auto self = shared_from_this();
        asio::async_write(sock_, asio::buffer(wq_.front()), [self](const boost::system::error_code& ec, std::size_t) {
            if (ec) { self->Close(); return; }
            self->wq_.pop_front();
            self->Write();
        });
    }

    void Handle(const std::string& line)
    {
        const auto t = Tokens(line);
        if (t.empty()) return;
        Core& c = srv_.core_;

        if (!authed_) {
            if (t[0] == "AUTH" && t.size() == 2 && t[1] == srv_.token_) {
                authed_ = true;
#ifdef _WIN32
                const long pid = (long)GetCurrentProcessId();
#else
                const long pid = (long)getpid();
#endif
                Send("HELLO " + std::to_string(kProtocol) + " " + std::to_string(pid) + " " + Enc(SI_VERSION));
            } else {
                Close();
            }
            return;
        }

        const std::string& cmd = t[0];
        auto I = [&](size_t i) { return i < t.size() ? ToInt(t[i]) : 0; };

        if (cmd == "ATTACH") {
            srv_.Attach(shared_from_this());
            for (const auto& l : srv_.core_.LogHistory()) Send("L " + Enc(l));
            for (const auto& l : SerializeSnapshot(c.GetSnapshot())) Send(l);
        } else if (cmd == "MONITORS") {
            std::vector<InputRect> v;
            const long long n = I(1);
            for (long long i = 0; i < n && 2 + (size_t)i * 4 + 3 < t.size(); ++i) {
                const size_t o = 2 + (size_t)i * 4;
                v.push_back({(int)I(o), (int)I(o + 1), (int)I(o + 2), (int)I(o + 3)});
            }
            c.CmdMonitorsHint(v);
        } else if (cmd == "SCAN") {
            c.CmdScan();
        } else if (cmd == "SELECT" && t.size() >= 2) {
            c.CmdSelect(Dec(t[1]));
        } else if (cmd == "GROUP") {
            c.CmdGroup(t.size() >= 2 ? Dec(t[1]) : std::string());
        } else if (cmd == "MOVE" && t.size() >= 4) {
            c.CmdMove((int)I(1), (double)I(2), (double)I(3));
        } else if (cmd == "AUDIO" && t.size() >= 6) {
            c.CmdPeerAudio((int)I(1), I(2) != 0, I(3) != 0, I(4) != 0, (int)I(5));
        } else if (cmd == "MASTER" && t.size() >= 3) {
            c.CmdMaster(I(1) != 0, (int)I(2));
        } else if (cmd == "CONTROL" && t.size() >= 3) {
            c.CmdControl((int)I(1), I(2) != 0);
        } else if (cmd == "STOP") {
            c.CmdQuit();
        }
        // PING and unknown commands: nothing to do (the connection itself is the answer)
    }

    Server& srv_;
    tcp::socket sock_;
    asio::streambuf buf_;
    asio::steady_timer authTimer_;
    std::deque<std::string> wq_;
    bool writing_ = false, closed_ = false, authed_ = false;
};

Server::Server(Core& core, std::filesystem::path dataDir)
    : core_(core), dir_(std::move(dataDir)), acceptor_(core.Context()), pump_(core.Context())
{
}

Server::~Server() { Stop(); }

bool Server::Start(std::string* err)
{
    const tcp::endpoint ep(asio::ip::address_v4::loopback(), kPort);
    boost::system::error_code ec;
    acceptor_.open(ep.protocol(), ec);
#ifndef _WIN32
    if (!ec) acceptor_.set_option(tcp::acceptor::reuse_address(true), ec);   // (Windows: never - it would allow port hijacking)
#endif
    if (!ec) acceptor_.bind(ep, ec);
    if (!ec) acceptor_.listen(8, ec);
    if (ec) {
        inUse_ = ec == asio::error::address_in_use;
        if (err) *err = ec.message();
        boost::system::error_code ig;
        acceptor_.close(ig);
        return false;
    }

    // Only now (we own the port) publish a new token; a second daemon must not overwrite it.
    token_ = Identity::RandomHex(16);
    std::error_code fec;
    std::filesystem::create_directories(dir_, fec);
    {
        std::ofstream out(TokenPath(dir_), std::ios::trunc);
        out << token_ << "\n";
    }
    std::filesystem::permissions(TokenPath(dir_), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, fec);

    core_.onChanged = [this] { dirty_ = true; };
    const auto prevLog = core_.onLog;      // e.g. the daemon's log file: keep it
    core_.onLog = [this, prevLog](const std::string& line) {
        if (prevLog) prevLog(line);
        const std::string m = "L " + Enc(line);
        for (const auto& s : sessions_) if (s->attached) s->Send(m);
    };
    started_ = true;
    Accept();
    Pump();
    return true;
}

void Server::Accept()
{
    acceptor_.async_accept([this](const boost::system::error_code& ec, tcp::socket sock) {
        if (ec == asio::error::operation_aborted || stopped_) return;
        if (!ec) {
            if (sessions_.size() < 8) {
                auto s = std::make_shared<Session>(*this, std::move(sock));
                sessions_.insert(s);
                s->Start();
            }
        }
        Accept();
    });
}

void Server::Pump()
{
    pump_.expires_after(std::chrono::milliseconds(50));
    pump_.async_wait([this](const boost::system::error_code& ec) {
        if (ec || stopped_) return;
        if (dirty_) { dirty_ = false; BroadcastSnapshot(); }
        if (++pumpTicks_ % 5 == 0) BroadcastStats();
        Pump();
    });
}

int Server::AttachedCount() const
{
    int n = 0;
    for (const auto& s : sessions_) n += s->attached ? 1 : 0;
    return n;
}

void Server::BroadcastSnapshot()
{
    if (AttachedCount() == 0) return;
    const auto lines = SerializeSnapshot(core_.GetSnapshot());
    for (const auto& s : sessions_)
        if (s->attached) for (const auto& l : lines) s->Send(l);
}

void Server::BroadcastStats()
{
    if (AttachedCount() == 0) return;
    const std::string l = SerializeStats(core_.GetStats());
    for (const auto& s : sessions_) if (s->attached) s->Send(l);
}

void Server::Attach(const std::shared_ptr<Session>& s)
{
    s->attached = true;
    core_.SetClientCount(AttachedCount());
}

void Server::Remove(const std::shared_ptr<Session>& s)
{
    sessions_.erase(s);
    core_.SetClientCount(AttachedCount());
}

void Server::Stop()
{
    // A server that never got the port (another daemon owns it) must not touch anything.
    if (!started_ || stopped_) return;
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    asio::post(core_.Context(), [this, done] {
        stopped_ = true;
        boost::system::error_code ig;
        acceptor_.close(ig);
        pump_.cancel();
        const auto copy = sessions_;
        for (const auto& s : copy) s->Close();
        done->set_value();
    });
    if (fut.wait_for(std::chrono::seconds(2)) != std::future_status::ready) stopped_ = true;   // loop not running (never started)
    std::error_code ec;
    std::filesystem::remove(TokenPath(dir_), ec);
}

} // namespace ipc
