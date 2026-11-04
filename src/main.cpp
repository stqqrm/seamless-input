#include <wx/wx.h>
#include <wx/dcbuffer.h>
#include <wx/listctrl.h>
#include <wx/timer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "localinfo.hpp"
#include "network.hpp"

// ============================================================================
// Canvas: central square = this window, draggable squares = virtual windows
// of connected peers. Positions are stored in "square units" relative to the
// centre so they are independent of either side's window size.
// ============================================================================
class Canvas : public wxPanel {
public:
    static constexpr int kSquare = 110;

    struct VWindow {
        int id;
        wxString name, addr;
        double ux, uy;
    };

    std::function<void(int, double, double)> onPeerMoved;

    Canvas(wxWindow* parent, const wxString& host)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(420, 260)), host_(host)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(420, 240));
        Bind(wxEVT_PAINT, &Canvas::OnPaint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Refresh(); e.Skip(); });
        Bind(wxEVT_LEFT_DOWN, &Canvas::OnDown, this);
        Bind(wxEVT_LEFT_UP, &Canvas::OnUp, this);
        Bind(wxEVT_MOTION, &Canvas::OnMove, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { dragId_ = -1; });
    }

    size_t PeerCount() const { return peers_.size(); }

    void AddPeer(int id, const wxString& name, const wxString& addr, double ux, double uy)
    {
        peers_.push_back({id, name, addr, ux, uy});
        Refresh();
    }

    void RemovePeer(int id)
    {
        peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                                    [id](const VWindow& v) { return v.id == id; }),
                     peers_.end());
        if (dragId_ == id) dragId_ = -1;
        Refresh();
    }

    // Used for remote updates: does not fire onPeerMoved.
    void SetPeerPos(int id, double ux, double uy)
    {
        for (auto& v : peers_)
            if (v.id == id) { v.ux = ux; v.uy = uy; }
        Refresh();
    }

private:
    wxPoint Centre() const { wxSize s = GetClientSize(); return wxPoint(s.x / 2, s.y / 2); }
    static wxRect SquareAt(wxPoint c) { return wxRect(c.x - kSquare / 2, c.y - kSquare / 2, kSquare, kSquare); }

    wxRect PeerRect(const VWindow& v) const
    {
        wxPoint c = Centre();
        return SquareAt(wxPoint(c.x + (int)std::lround(v.ux * kSquare),
                                c.y + (int)std::lround(v.uy * kSquare)));
    }

    void DrawLine2(wxDC& dc, const wxString& text, const wxRect& r, int y)
    {
        wxString t = wxControl::Ellipsize(text, dc, wxELLIPSIZE_END, r.width - 8);
        wxSize ext = dc.GetTextExtent(t);
        dc.DrawText(t, r.x + (r.width - ext.x) / 2, r.y + y);
    }

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(wxColour(28, 30, 34)));
        dc.Clear();

        wxSize s = GetClientSize();
        dc.SetPen(wxPen(wxColour(42, 45, 52)));
        for (int x = 0; x < s.x; x += 32) dc.DrawLine(x, 0, x, s.y);
        for (int y = 0; y < s.y; y += 32) dc.DrawLine(0, y, s.x, y);

        // Current window (always shown)
        wxRect cr = SquareAt(Centre());
        dc.SetPen(wxPen(wxColour(120, 170, 255), 2));
        dc.SetBrush(wxBrush(wxColour(52, 120, 246)));
        dc.DrawRectangle(cr);
        dc.SetTextForeground(*wxWHITE);
        DrawLine2(dc, "This window", cr, kSquare / 2 - 18);
        DrawLine2(dc, host_, cr, kSquare / 2 + 2);

        // Virtual windows (only exist once a connection is established)
        for (const auto& v : peers_) {
            wxRect r = PeerRect(v);
            dc.SetPen(wxPen(wxColour(255, 170, 60), 2, wxPENSTYLE_SHORT_DASH));
            dc.SetBrush(wxBrush(wxColour(64, 54, 38)));
            dc.DrawRectangle(r);
            dc.SetTextForeground(wxColour(255, 214, 150));
            DrawLine2(dc, v.name, r, kSquare / 2 - 18);
            DrawLine2(dc, v.addr, r, kSquare / 2 + 2);
        }

        if (peers_.empty()) {
            dc.SetTextForeground(wxColour(140, 146, 158));
            wxString hint = "Select a target below to connect (or start a server)";
            wxSize ext = dc.GetTextExtent(hint);
            dc.DrawText(hint, (s.x - ext.x) / 2, cr.GetBottom() + 14);
        }
    }

    void OnDown(wxMouseEvent& e)
    {
        for (size_t i = peers_.size(); i-- > 0;) {
            wxRect r = PeerRect(peers_[i]);
            if (r.Contains(e.GetPosition())) {
                dragId_ = peers_[i].id;
                dragOff_ = e.GetPosition() - wxPoint(r.x + r.width / 2, r.y + r.height / 2);
                std::rotate(peers_.begin() + i, peers_.begin() + i + 1, peers_.end());  // bring to front
                if (!HasCapture()) CaptureMouse();
                return;
            }
        }
    }

    void OnUp(wxMouseEvent&)
    {
        if (HasCapture()) ReleaseMouse();
        dragId_ = -1;
    }

    void OnMove(wxMouseEvent& e)
    {
        if (dragId_ >= 0 && e.LeftIsDown()) {
            wxSize s = GetClientSize();
            wxPoint c = e.GetPosition() - dragOff_;
            c.x = std::clamp(c.x, 0, s.x);
            c.y = std::clamp(c.y, 0, s.y);
            wxPoint o = Centre();
            for (auto& v : peers_) {
                if (v.id != dragId_) continue;
                v.ux = double(c.x - o.x) / kSquare;
                v.uy = double(c.y - o.y) / kSquare;
                if (onPeerMoved) onPeerMoved(v.id, v.ux, v.uy);
            }
            Refresh();
            return;
        }
        bool over = false;
        for (const auto& v : peers_) over = over || PeerRect(v).Contains(e.GetPosition());
        SetCursor(over ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
    }

    wxString host_;
    std::vector<VWindow> peers_;
    int dragId_ = -1;
    wxPoint dragOff_;
};

// ============================================================================
// Main window
// ============================================================================
class MainFrame : public wxFrame {
public:
    MainFrame()
        : wxFrame(nullptr, wxID_ANY, "seamless-input", wxDefaultPosition, wxSize(900, 700)),
          host_(wxGetHostName())
    {
        SetTitle("seamless-input - " + host_);
        local_ = GetLocalAddresses();

        auto* root = new wxBoxSizer(wxVERTICAL);

        // ---- upper part: canvas
        canvas_ = new Canvas(this, host_);
        root->Add(canvas_, 3, wxEXPAND);

        // ---- header
        auto* header = new wxStaticText(this, wxID_ANY,
            wxString::Format("Host name: %s     TCP %d / UDP %d",
                             host_, (int)kTcpPort, (int)kDiscoveryPort));
        root->Add(header, 0, wxALL, 8);

        // ---- lower part: list + log
        auto* bottom = new wxBoxSizer(wxHORIZONTAL);

        auto* left = new wxBoxSizer(wxVERTICAL);
        searchLbl_ = new wxStaticText(this, wxID_ANY, "");
        left->Add(searchLbl_, 0, wxBOTTOM, 4);

        list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxLC_REPORT | wxLC_SINGLE_SEL);
        list_->InsertColumn(0, "Name", wxLIST_FORMAT_LEFT, 200);
        list_->InsertColumn(1, "Address", wxLIST_FORMAT_LEFT, 130);
        list_->InsertColumn(2, "Status", wxLIST_FORMAT_LEFT, 140);
        left->Add(list_, 1, wxEXPAND);

        auto* row = new wxBoxSizer(wxHORIZONTAL);
        target_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
        target_->SetHint("target IP");
        auto* go = new wxButton(this, wxID_ANY, "Go");
        scanBtn_ = new wxButton(this, wxID_ANY, "Scan LAN");
        row->Add(target_, 1, wxRIGHT, 6);
        row->Add(go, 0, wxRIGHT, 6);
        row->Add(scanBtn_, 0);
        left->Add(row, 0, wxEXPAND | wxTOP, 6);

        log_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                              wxTE_MULTILINE | wxTE_READONLY);

        bottom->Add(left, 1, wxEXPAND | wxLEFT | wxBOTTOM, 8);
        bottom->Add(log_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        root->Add(bottom, 2, wxEXPAND);
        SetSizer(root);

        // ---- events
        list_->Bind(wxEVT_LIST_ITEM_SELECTED, &MainFrame::OnListSelected, this);
        go->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Activate(target_->GetValue().ToStdString()); });
        target_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { Activate(target_->GetValue().ToStdString()); });
        scanBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StartScan(); });
        Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);

        timer_.SetOwner(this, kTimerId);
        Bind(wxEVT_TIMER, &MainFrame::OnTimer, this, kTimerId);
        timer_.Start(1000);

        animTimer_.SetOwner(this, kAnimTimerId);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { ++animStep_; UpdateSearchLabel(); }, kAnimTimerId);

        canvas_->onPeerMoved = [this](int id, double x, double y) { SendPos(id, x, y); };

        // ---- network (callbacks arrive on the asio thread -> marshal to GUI)
        NetCallbacks cb;
        cb.onStatus = [this](const std::string& s) {
            CallAfter([this, s] { Log(wxString::FromUTF8(s)); });
        };
        cb.onPeerReady = [this](int id, const std::string& name, const std::string& addr, bool incoming) {
            CallAfter([this, id, name, addr, incoming] {
                OnPeerReady(id, wxString::FromUTF8(name), wxString::FromUTF8(addr), incoming);
            });
        };
        cb.onPeerLost = [this](int id) { CallAfter([this, id] { OnPeerLost(id); }); };
        cb.onMessage = [this](int id, const std::string& line) {
            CallAfter([this, id, line] { OnMessage(id, line); });
        };
        cb.onBeacon = [this](const std::string& ip, const std::string& host, bool server) {
            CallAfter([this, ip, host, server] { OnBeacon(ip, host, server); });
        };
        cb.onScanHost = [this](const std::string& ip, bool server) {
            CallAfter([this, ip, server] { OnScanHost(ip, server); });
        };
        cb.onHostName = [this](const std::string& ip, const std::string& name) {
            CallAfter([this, ip, name] { OnHostName(ip, wxString::FromUTF8(name)); });
        };
        cb.onScanDone = [this]() { CallAfter([this] { OnScanDone(); }); };
        net_ = std::make_unique<Network>(host_.ToStdString(), std::move(cb));

        RebuildList();
        Log("Ready. Click an entry in the list to connect / start a server.");
        net_->Start();
        StartScan();
        Centre();
    }

private:
    static constexpr int kTimerId     = 9000;
    static constexpr int kAnimTimerId = 9001;

    struct Entry { wxString name; std::string ip; wxString status; bool local; };
    struct Discovered {
        std::string host;
        bool server;
        std::chrono::steady_clock::time_point seen;
    };
    struct Scanned {
        wxString name;
        bool server = false;
        std::chrono::steady_clock::time_point seen;
    };

    // ---------------------------------------------------------------- actions
    void Activate(const std::string& ip)
    {
        if (ip.empty()) return;
        bool isLocal = ip.rfind("127.", 0) == 0;
        for (const auto& a : local_) isLocal = isLocal || a.ip == ip;

        selectedIp_ = ip;
        if (isLocal) Log("Selected this machine (" + wxString::FromUTF8(ip) + ") - starting server");
        else         Log("Selected " + wxString::FromUTF8(ip) + " - trying to connect");
        net_->Select(ip, isLocal);
    }

    void OnListSelected(wxListEvent& e)
    {
        if (rebuilding_) return;
        long i = e.GetIndex();
        if (i < 0 || i >= (long)entries_.size()) return;
        target_->SetValue(wxString::FromUTF8(entries_[i].ip));
        Activate(entries_[i].ip);
    }

    void SendPos(int id, double x, double y)
    {
        net_->Send(id, "POS " + std::to_string(std::lround(x * 1000)) + " " +
                           std::to_string(std::lround(y * 1000)));
    }

    // ------------------------------------------------------------------- scan
    void StartScan()
    {
        if (scanning_) return;
        std::vector<std::pair<std::string, int>> nets;
        for (const auto& a : local_) nets.emplace_back(a.ip, a.prefix);
        if (nets.empty()) { Log("No network interface to scan"); return; }
        scanning_ = true;
        scanBtn_->Enable(false);
        scanBtn_->SetLabel("Scanning...");
        animStep_ = 0;
        UpdateSearchLabel();
        animTimer_.Start(300);
        net_->Scan(nets);
    }

    void UpdateSearchLabel()
    {
        const int n = animStep_ % 3 + 1;
        wxString dots = wxString("...").Left(n);
        searchLbl_->SetLabel(wxString::Format("searching %s   (%d found)", dots, (int)scan_.size()));
    }

    void OnScanDone()
    {
        scanning_ = false;
        animTimer_.Stop();
        searchLbl_->SetLabel(wxString::Format("Scan complete: %d host(s) found", (int)scan_.size()));
        scanBtn_->SetLabel("Scan LAN");
        scanBtn_->Enable(true);
    }

    void OnScanHost(const std::string& ip, bool server)
    {
        auto& s = scan_[ip];
        s.server = server;
        s.seen = std::chrono::steady_clock::now();
        RebuildList();
    }

    void OnHostName(const std::string& ip, const wxString& name)
    {
        auto it = scan_.find(ip);
        if (it == scan_.end()) return;
        it->second.name = name;
        RebuildList();
    }

    // ------------------------------------------------------- network results
    void OnPeerReady(int id, const wxString& name, const wxString& addr, bool incoming)
    {
        connected_[id] = addr.ToStdString();
        Log("Connection established with " + name + " (" + addr + ")");

        // The side that accepted decides the initial layout and tells the other
        // side, which mirrors it. From then on either side can drag.
        static const double dirs[4][2] = {{1.4, 0}, {-1.4, 0}, {0, 1.4}, {0, -1.4}};
        if (incoming) {
            const auto& d = dirs[canvas_->PeerCount() % 4];
            canvas_->AddPeer(id, name, addr, d[0], d[1]);
            SendPos(id, d[0], d[1]);
        } else {
            canvas_->AddPeer(id, name, addr, -1.4, 0);
        }
        RebuildList();
    }

    void OnPeerLost(int id)
    {
        Log("Peer disconnected");
        connected_.erase(id);
        canvas_->RemovePeer(id);   // virtual window disappears with the connection
        RebuildList();
    }

    void OnMessage(int id, const std::string& line)
    {
        int x, y;
        if (std::sscanf(line.c_str(), "POS %d %d", &x, &y) == 2)
            canvas_->SetPeerPos(id, -x / 1000.0, -y / 1000.0);   // mirrored
    }

    void OnBeacon(const std::string& ip, const std::string& host, bool server)
    {
        auto it = disc_.find(ip);
        bool changed = it == disc_.end() || it->second.host != host || it->second.server != server;
        disc_[ip] = {host, server, std::chrono::steady_clock::now()};
        if (changed) RebuildList();
    }

    void OnTimer(wxTimerEvent&)
    {
        auto now = std::chrono::steady_clock::now();
        bool removed = false;
        for (auto it = disc_.begin(); it != disc_.end();) {
            if (now - it->second.seen > std::chrono::seconds(7)) { it = disc_.erase(it); removed = true; }
            else ++it;
        }
        for (auto it = scan_.begin(); it != scan_.end();) {
            if (now - it->second.seen > std::chrono::seconds(400)) { it = scan_.erase(it); removed = true; }
            else ++it;
        }
        if (removed) RebuildList();
        if (++tick_ % 120 == 0) StartScan();   // periodic rescan
    }

    // -------------------------------------------------------------------- GUI
    void RebuildList()
    {
        auto isConnected = [this](const std::string& ip) {
            for (auto& kv : connected_) if (kv.second == ip) return true;
            return false;
        };

        auto key = [](const std::string& ip) {
            unsigned a = 0, b = 0, c = 0, d = 0;
            std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d);
            return (std::uint32_t)((a << 24) | (b << 16) | (c << 8) | d);
        };

        // remote hosts: scan results first, UDP beacons override (they know the real host name)
        std::map<std::uint32_t, Entry> remote;
        for (const auto& kv : scan_) {
            const auto& s = kv.second;
            remote[key(kv.first)] = {s.name.empty() ? wxString("(unknown)") : s.name, kv.first,
                                     s.server ? "Server running" : "Online", false};
        }
        for (const auto& kv : disc_)
            remote[key(kv.first)] = {wxString::FromUTF8(kv.second.host), kv.first,
                                     kv.second.server ? "Server running" : "No server", false};

        entries_.clear();
        for (const auto& a : local_)
            entries_.push_back({host_ + " (" + wxString::FromUTF8(a.ifname) + ")", a.ip, "This machine", true});
        for (auto& kv : remote) entries_.push_back(kv.second);

        rebuilding_ = true;
        list_->Freeze();
        list_->DeleteAllItems();
        for (size_t i = 0; i < entries_.size(); ++i) {
            const auto& e = entries_[i];
            long row = list_->InsertItem((long)i, e.name);
            list_->SetItem(row, 1, wxString::FromUTF8(e.ip));
            list_->SetItem(row, 2, e.status + (isConnected(e.ip) ? "  [connected]" : ""));
            if (e.ip == selectedIp_)
                list_->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        }
        list_->Thaw();
        rebuilding_ = false;
    }

    void Log(const wxString& msg)
    {
        log_->AppendText(wxDateTime::Now().FormatTime() + "  " + msg + "\n");
    }

    void OnClose(wxCloseEvent&)
    {
        timer_.Stop();
        animTimer_.Stop();
        net_->Stop();   // joins the asio thread: no more callbacks after this
        Destroy();
    }

    wxString host_;
    std::vector<LocalAddress> local_;
    std::vector<Entry> entries_;
    std::map<std::string, Discovered> disc_;
    std::map<std::string, Scanned> scan_;
    std::map<int, std::string> connected_;
    std::string selectedIp_;
    bool rebuilding_ = false;
    bool scanning_ = false;
    int tick_ = 0;
    int animStep_ = 0;

    Canvas* canvas_ = nullptr;
    wxListCtrl* list_ = nullptr;
    wxTextCtrl* target_ = nullptr;
    wxTextCtrl* log_ = nullptr;
    wxButton* scanBtn_ = nullptr;
    wxStaticText* searchLbl_ = nullptr;
    wxTimer timer_;
    wxTimer animTimer_;
    std::unique_ptr<Network> net_;
};

class App : public wxApp {
public:
    bool OnInit() override
    {
        (new MainFrame)->Show();
        return true;
    }
};

wxIMPLEMENT_APP(App);
