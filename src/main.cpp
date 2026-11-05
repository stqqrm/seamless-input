#include <wx/wx.h>
#include <wx/dcbuffer.h>
#include <wx/display.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/stdpaths.h>
#include <wx/timer.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

#include "audio.hpp"
#include "config.hpp"
#include "identity.hpp"
#include "input/input.hpp"
#include "localinfo.hpp"
#include "network.hpp"

// ============================================================================
// Canvas: every PC is a group of monitor rectangles drawn at real pixel
// proportions (one shared scale). The central group is this PC; the other
// groups are virtual windows of connected PCs, draggable and edge-snapping.
// Positions are "world" pixels: the group's top-left corner relative to the
// top-left corner of this PC's desktop.
// ============================================================================
class Canvas : public wxPanel {
public:
    struct Group {
        int id = 0;
        wxString name, addr;
        std::vector<wxRect> mons;   // relative to the group's top-left corner
        wxSize size;
        double ox = 0, oy = 0;
    };

    std::function<void(int, double, double)> onPeerMoved;

    explicit Canvas(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(420, 260))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(420, 240));
        Bind(wxEVT_PAINT, &Canvas::OnPaint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Refresh(); e.Skip(); });
        Bind(wxEVT_LEFT_DOWN, &Canvas::OnDown, this);
        Bind(wxEVT_LEFT_UP, &Canvas::OnUp, this);
        Bind(wxEVT_MOTION, &Canvas::OnMove, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { EndDrag(); });
        local_ = MakeGroup(-1, "", "", {});
    }

    void SetLocal(const wxString& host, const std::vector<wxRect>& mons)
    {
        local_ = MakeGroup(-1, host, "This PC", mons);
        Refresh();
    }

    // New peers start flush against the existing layout (top aligned with this PC).
    std::pair<double, double> AddPeer(int id, const wxString& name, const wxString& addr,
                                      const std::vector<wxRect>& mons, bool toTheRight)
    {
        Group g = MakeGroup(id, name, addr, mons);
        g.ox = toTheRight ? RightEdge() : LeftEdge() - g.size.x;
        g.oy = local_.oy;
        peers_.push_back(g);
        Refresh();
        return {g.ox, g.oy};
    }

    void RemovePeer(int id)
    {
        peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                                    [id](const Group& g) { return g.id == id; }),
                     peers_.end());
        if (dragId_ == id) EndDrag();
        Refresh();
    }

    // Remote update: does not fire onPeerMoved.
    void SetPeerPos(int id, double ox, double oy)
    {
        if (Group* g = Find(id)) { g->ox = ox; g->oy = oy; Refresh(); }
    }

    // Local update (e.g. a saved config): moves the peer there, but if that would put one
    // of its monitors on top of another monitor it is pushed out to the nearest free edge.
    // Returns the position it really got. Does not fire onPeerMoved.
    std::pair<double, double> PlacePeer(int id, double ox, double oy)
    {
        Group* g = Find(id);
        if (!g) return {ox, oy};
        ox = std::round(ox);
        oy = std::round(oy);
        if (Collides(*g, ox, oy)) std::tie(ox, oy) = PushOut(*g, ox, oy);
        g->ox = ox;
        g->oy = oy;
        Refresh();
        return {ox, oy};
    }

    bool PeerPos(int id, double& ox, double& oy) const
    {
        for (const auto& g : peers_)
            if (g.id == id) { ox = g.ox; oy = g.oy; return true; }
        return false;
    }

private:
    struct View { double s = 0.1, cx = 0, cy = 0; int w = 1, h = 1; };

    static Group MakeGroup(int id, const wxString& name, const wxString& addr, std::vector<wxRect> mons)
    {
        Group g;
        g.id = id; g.name = name; g.addr = addr;
        if (mons.empty()) mons.push_back(wxRect(0, 0, 1920, 1080));
        int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
        for (const auto& m : mons) {
            x0 = std::min(x0, m.x);            y0 = std::min(y0, m.y);
            x1 = std::max(x1, m.x + m.width);  y1 = std::max(y1, m.y + m.height);
        }
        for (auto& m : mons) m.Offset(-x0, -y0);
        g.mons = std::move(mons);
        g.size = wxSize(x1 - x0, y1 - y0);
        return g;
    }

    Group* Find(int id)
    {
        for (auto& g : peers_) if (g.id == id) return &g;
        return nullptr;
    }

    double RightEdge() const
    {
        double r = local_.ox + local_.size.x;
        for (const auto& g : peers_) r = std::max(r, g.ox + g.size.x);
        return r;
    }

    double LeftEdge() const
    {
        double l = local_.ox;
        for (const auto& g : peers_) l = std::min(l, g.ox);
        return l;
    }

    // ---- view: one scale for everything, with room around the layout to drag into
    View ComputeView() const
    {
        View v;
        wxSize cs = GetClientSize();
        v.w = std::max(1, cs.x);
        v.h = std::max(1, cs.y);

        double x0 = local_.ox, y0 = local_.oy;
        double x1 = x0 + local_.size.x, y1 = y0 + local_.size.y;
        double maxW = local_.size.x, maxH = local_.size.y;
        for (const auto& g : peers_) {
            x0 = std::min(x0, g.ox);               y0 = std::min(y0, g.oy);
            x1 = std::max(x1, g.ox + g.size.x);    y1 = std::max(y1, g.oy + g.size.y);
            maxW = std::max<double>(maxW, g.size.x);
            maxH = std::max<double>(maxH, g.size.y);
        }
        const double bw = (x1 - x0) + 2 * 0.6 * maxW;
        const double bh = (y1 - y0) + 2 * 0.45 * maxH;
        v.s = std::min(0.5, std::min(v.w / bw, v.h / bh));
        v.cx = (x0 + x1) / 2;
        v.cy = (y0 + y1) / 2;
        return v;
    }

    wxRect MonRect(const Group& g, const wxRect& m, const View& v) const
    {
        auto sx = [&](double x) { return (int)std::lround((x - v.cx) * v.s + v.w / 2.0); };
        auto sy = [&](double y) { return (int)std::lround((y - v.cy) * v.s + v.h / 2.0); };
        const int x0 = sx(g.ox + m.x), x1 = sx(g.ox + m.x + m.width);
        const int y0 = sy(g.oy + m.y), y1 = sy(g.oy + m.y + m.height);
        return wxRect(x0, y0, x1 - x0, y1 - y0);   // edges rounded separately: neighbours stay flush
    }

    // ---- snapping: pull the dragged group's edges onto other monitors' edges
    void Snap(const Group& g, double& ox, double& oy, double thr) const
    {
        double bestX = thr + 1, bestY = thr + 1, adjX = 0, adjY = 0;

        auto consider = [&](const Group& o) {
            for (const auto& m : g.mons) {
                const double l = ox + m.x, r = l + m.width, t = oy + m.y, b = t + m.height;
                for (const auto& n : o.mons) {
                    const double nl = o.ox + n.x, nr = nl + n.width, nt = o.oy + n.y, nb = nt + n.height;
                    if (t < nb + thr && b > nt - thr) {          // vertically touching: snap in x
                        for (double d : {nr - l, nl - r, nl - l, nr - r})
                            if (std::fabs(d) < bestX) { bestX = std::fabs(d); adjX = d; }
                    }
                    if (l < nr + thr && r > nl - thr) {          // horizontally touching: snap in y
                        for (double d : {nb - t, nt - b, nt - t, nb - b})
                            if (std::fabs(d) < bestY) { bestY = std::fabs(d); adjY = d; }
                    }
                }
            }
        };

        consider(local_);
        for (const auto& o : peers_) if (o.id != g.id) consider(o);

        if (bestX <= thr) ox += adjX;
        if (bestY <= thr) oy += adjY;
    }

    // ---- no overlap: monitors of different PCs may touch (share an edge) but never intersect
    static bool Overlap(double l1, double t1, double r1, double b1,
                        double l2, double t2, double r2, double b2)
    {
        return l1 < r2 && r1 > l2 && t1 < b2 && b1 > t2;   // strict: flush edges are fine
    }

    bool Collides(const Group& g, double ox, double oy) const
    {
        auto test = [&](const Group& o) {
            if (o.id == g.id) return false;
            for (const auto& m : g.mons)
                for (const auto& n : o.mons)
                    if (Overlap(ox + m.x, oy + m.y, ox + m.x + m.width, oy + m.y + m.height,
                                o.ox + n.x, o.oy + n.y, o.ox + n.x + n.width, o.oy + n.y + n.height))
                        return true;
            return false;
        };
        if (test(local_)) return true;
        for (const auto& o : peers_) if (test(o)) return true;
        return false;
    }

    // How far (<= |d|, same sign) can g move along one axis from (ox, oy) before one of its
    // monitors would run into another monitor? It stops exactly at the touching edge.
    double Sweep(const Group& g, double ox, double oy, double d, bool horizontal) const
    {
        if (d == 0) return 0;
        auto one = [&](const Group& o) {
            if (o.id == g.id) return;
            for (const auto& m : g.mons) {
                const double l = ox + m.x, t = oy + m.y, r = l + m.width, b = t + m.height;
                for (const auto& n : o.mons) {
                    const double nl = o.ox + n.x, nt = o.oy + n.y, nr = nl + n.width, nb = nt + n.height;
                    // only monitors that share rows (moving in x) / columns (moving in y) can block
                    const double lo = horizontal ? t : l,  hi = horizontal ? b : r;
                    const double nlo = horizontal ? nt : nl, nhi = horizontal ? nb : nr;
                    if (!(lo < nhi && hi > nlo)) continue;
                    const double front = horizontal ? r : b,  back = horizontal ? l : t;
                    const double nfront = horizontal ? nl : nt, nback = horizontal ? nr : nb;
                    if (d > 0 && front <= nfront) d = std::min(d, nfront - front);
                    if (d < 0 && back >= nback)   d = std::max(d, nback - back);
                }
            }
        };
        one(local_);
        for (const auto& o : peers_) one(o);
        return d;
    }

    // Moves g from its current (valid) position towards (tx, ty) and stops/slides along
    // whatever is in the way, so a monitor can never jump over another one.
    std::pair<double, double> SlideTo(const Group& g, double tx, double ty) const
    {
        double x = g.ox, y = g.oy;
        x += Sweep(g, x, y, tx - x, true);
        y += Sweep(g, x, y, ty - y, false);
        return {x, y};
    }

    // Shifts (ox, oy) by the smallest amounts until nothing overlaps (ends flush at an edge).
    std::pair<double, double> PushOut(const Group& g, double ox, double oy) const
    {
        for (int guard = 0; guard < 64 && Collides(g, ox, oy); ++guard) {
            double bestAbs = 1e18, bdx = 0, bdy = 0;
            auto consider = [&](const Group& o) {
                if (o.id == g.id) return;
                for (const auto& m : g.mons) {
                    const double l = ox + m.x, t = oy + m.y, r = l + m.width, b = t + m.height;
                    for (const auto& n : o.mons) {
                        const double nl = o.ox + n.x, nt = o.oy + n.y, nr = nl + n.width, nb = nt + n.height;
                        if (!Overlap(l, t, r, b, nl, nt, nr, nb)) continue;
                        const double cand[4][2] = {{nr - l, 0}, {nl - r, 0}, {0, nb - t}, {0, nt - b}};
                        for (const auto& c : cand) {
                            const double a = std::fabs(c[0]) + std::fabs(c[1]);
                            if (a < bestAbs) { bestAbs = a; bdx = c[0]; bdy = c[1]; }
                        }
                    }
                }
            };
            consider(local_);
            for (const auto& o : peers_) consider(o);
            if (bestAbs >= 1e18) break;
            ox += bdx;
            oy += bdy;
        }
        if (Collides(g, ox, oy)) {          // could not resolve: park it right of everything else
            double r = local_.ox + local_.size.x;
            for (const auto& o : peers_) if (o.id != g.id) r = std::max(r, o.ox + o.size.x);
            ox = r;
            oy = local_.oy;
        }
        return {ox, oy};
    }

    // ---- painting
    void Centered(wxDC& dc, const wxString& text, const wxRect& r)
    {
        wxString t = wxControl::Ellipsize(text, dc, wxELLIPSIZE_END, std::max(10, r.width - 8));
        wxSize ext = dc.GetTextExtent(t);
        if (ext.y > r.height) return;
        dc.DrawText(t, r.x + (r.width - ext.x) / 2, r.y + (r.height - ext.y) / 2);
    }

    void DrawGroup(wxDC& dc, const Group& g, const View& v, bool local, bool dragging)
    {
        const wxColour fill = local ? wxColour(52, 120, 246) : wxColour(70, 58, 40);
        const wxColour edge = local ? wxColour(140, 185, 255) : wxColour(255, 170, 60);
        const wxColour text = local ? *wxWHITE : wxColour(255, 214, 150);

        int minX = INT_MAX, minY = INT_MAX;
        for (const auto& m : g.mons) {
            wxRect r = MonRect(g, m, v);
            minX = std::min(minX, r.x);
            minY = std::min(minY, r.y);
            dc.SetPen(wxPen(dragging ? *wxWHITE : edge, 2, local ? wxPENSTYLE_SOLID : wxPENSTYLE_SHORT_DASH));
            dc.SetBrush(wxBrush(fill));
            dc.DrawRectangle(r);
            dc.SetTextForeground(text);
            Centered(dc, wxString::Format("%dx%d", m.width, m.height), r);
        }

        wxString label = g.name;
        if (!g.addr.empty()) label += "   " + g.addr;
        dc.SetTextForeground(text);
        dc.DrawText(label, minX, minY - dc.GetCharHeight() - 3);
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

        const View v = frozen_ ? frozenView_ : ComputeView();
        DrawGroup(dc, local_, v, true, false);
        for (const auto& g : peers_) DrawGroup(dc, g, v, false, g.id == dragId_);

        if (peers_.empty()) {
            int bottom = 0;
            for (const auto& m : local_.mons) bottom = std::max(bottom, MonRect(local_, m, v).GetBottom());
            dc.SetTextForeground(wxColour(140, 146, 158));
            wxString hint = "Waiting for other seamless-input PCs on the LAN ...";
            wxSize ext = dc.GetTextExtent(hint);
            dc.DrawText(hint, (s.x - ext.x) / 2, bottom + 14);
        }
    }

    // ---- mouse
    void OnDown(wxMouseEvent& e)
    {
        const View v = ComputeView();
        for (size_t i = peers_.size(); i-- > 0;) {
            for (const auto& m : peers_[i].mons) {
                if (!MonRect(peers_[i], m, v).Contains(e.GetPosition())) continue;
                dragId_ = peers_[i].id;
                frozenView_ = v;               // keep the scale fixed while dragging
                frozen_ = true;
                grabX_ = (e.GetPosition().x - v.w / 2.0) / v.s + v.cx - peers_[i].ox;
                grabY_ = (e.GetPosition().y - v.h / 2.0) / v.s + v.cy - peers_[i].oy;
                std::rotate(peers_.begin() + i, peers_.begin() + i + 1, peers_.end());  // to front
                if (!HasCapture()) CaptureMouse();
                Refresh();
                return;
            }
        }
    }

    void OnUp(wxMouseEvent&)
    {
        if (HasCapture()) ReleaseMouse();
        EndDrag();
    }

    void EndDrag()
    {
        dragId_ = -1;
        frozen_ = false;
        Refresh();
    }

    void OnMove(wxMouseEvent& e)
    {
        if (dragId_ >= 0 && e.LeftIsDown()) {
            if (Group* g = Find(dragId_)) {
                const View& v = frozenView_;
                wxSize cs = GetClientSize();
                const int px = std::clamp(e.GetPosition().x, 0, cs.x);
                const int py = std::clamp(e.GetPosition().y, 0, cs.y);
                double ox = (px - v.w / 2.0) / v.s + v.cx - grabX_;
                double oy = (py - v.h / 2.0) / v.s + v.cy - grabY_;
                Snap(*g, ox, oy, 14.0 / v.s);
                ox = std::round(ox);
                oy = std::round(oy);
                // never through another monitor: slide along it and stop flush at its edge
                std::tie(ox, oy) = SlideTo(*g, ox, oy);
                if (Collides(*g, ox, oy)) std::tie(ox, oy) = PushOut(*g, ox, oy);
                if (ox != g->ox || oy != g->oy) {
                    g->ox = ox;
                    g->oy = oy;
                    if (onPeerMoved) onPeerMoved(g->id, ox, oy);
                    Refresh();
                }
            }
            return;
        }
        const View v = ComputeView();
        bool over = false;
        for (const auto& g : peers_)
            for (const auto& m : g.mons) over = over || MonRect(g, m, v).Contains(e.GetPosition());
        SetCursor(over ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
    }

    Group local_;
    std::vector<Group> peers_;
    int dragId_ = -1;
    bool frozen_ = false;
    View frozenView_;
    double grabX_ = 0, grabY_ = 0;
};

// ============================================================================
// Helpers
// ============================================================================
static std::vector<wxRect> GetLocalMonitors()
{
    std::vector<wxRect> v;
    for (unsigned i = 0; i < wxDisplay::GetCount(); ++i)
        v.push_back(wxDisplay(i).GetGeometry());
    if (v.empty()) v.push_back(wxRect(0, 0, 1920, 1080));
    return v;
}

static wxString ScreensText(const std::vector<wxRect>& mons)
{
    if (mons.empty()) return "...";
    wxString s = wxString::Format("%d monitor%s: ", (int)mons.size(), mons.size() == 1 ? "" : "s");
    for (size_t i = 0; i < mons.size(); ++i) {
        if (i) s += " + ";
        s += wxString::Format("%dx%d", mons[i].width, mons[i].height);
    }
    return s;
}

// ============================================================================
// Main window
// ============================================================================
class MainFrame : public wxFrame {
public:
    MainFrame()
        : wxFrame(nullptr, wxID_ANY, "seamless-input", wxDefaultPosition, wxSize(1000, 760)),
          host_(wxGetHostName())
    {
        SetTitle("seamless-input - " + host_);
        local_ = GetLocalAddresses();
        localMons_ = GetLocalMonitors();

        // ---- this PC's identity (key pair) and the saved configs live in the user data dir
        {
#if defined(__WXMSW__)
            // %APPDATA%\seamless-input
            const wxString base = wxStandardPaths::Get().GetUserDataDir();
#else
            // $XDG_DATA_HOME/seamless-input, normally ~/.local/share/seamless-input
            wxString xdg;
            if (!wxGetEnv("XDG_DATA_HOME", &xdg) || xdg.empty()) xdg = wxGetHomeDir() + "/.local/share";
            const wxString base = xdg + "/seamless-input";
#endif
            const std::filesystem::path dir = std::filesystem::u8path(std::string(base.utf8_str()));
            ident_ = std::make_unique<Identity>(Identity::LoadOrCreate(dir, &identNote_));
            store_ = std::make_unique<ConfigStore>(dir / "configs");
            trust_ = std::make_unique<TrustStore>(dir / "trust.txt");
            trust_->Load();
            dataDir_ = wxString::FromUTF8(dir.u8string());
        }

        auto* root = new wxBoxSizer(wxVERTICAL);

        // ---- upper part: monitor layout
        canvas_ = new Canvas(this);
        canvas_->SetLocal(host_, localMons_);
        root->Add(canvas_, 3, wxEXPAND);

        // ---- header
        wxString ips;
        for (const auto& a : local_) ips += (ips.empty() ? "" : ", ") + wxString::FromUTF8(a.ip);
        auto* header = new wxStaticText(this, wxID_ANY,
            wxString::Format("Host: %s     IP: %s     TCP %d / UDP %d (discovery) / UDP %d (audio)     Key: %s",
                             host_, ips.empty() ? wxString("-") : ips,
                             (int)kTcpPort, (int)kDiscoveryPort, (int)kAudioPort,
                             wxString::FromUTF8(ident_->Fingerprint())));
        root->Add(header, 0, wxALL, 8);

        // ---- lower part: tabs
        notebook_ = new wxNotebook(this, wxID_ANY);
        notebook_->AddPage(BuildNetworkPage(notebook_), "Network", true);
        notebook_->AddPage(BuildDevicesPage(notebook_), "Devices");
        root->Add(notebook_, 2, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
        SetSizer(root);

        // ---- events
        list_->Bind(wxEVT_LIST_ITEM_SELECTED, &MainFrame::OnListSelected, this);
        goBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Activate(target_->GetValue().ToStdString()); });
        target_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { Activate(target_->GetValue().ToStdString()); });
        scanBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StartScan(); });

        sendChk_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { ApplySending(); MarkCfgDirty(); });
        srcChoice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { if (sendChk_->GetValue()) ApplySending(); MarkCfgDirty(); });
        muteChk_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { audio_->SetMuted(muteChk_->GetValue()); MarkCfgDirty(); });
        volSlider_->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { audio_->SetVolume(volSlider_->GetValue() / 100.0f); MarkCfgDirty(); });

        Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);

        timer_.SetOwner(this, kTimerId);
        Bind(wxEVT_TIMER, &MainFrame::OnTimer, this, kTimerId);
        timer_.Start(1000);

        animTimer_.SetOwner(this, kAnimTimerId);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { ++animStep_; UpdateSearchLabel(); }, kAnimTimerId);

        statTimer_.SetOwner(this, kStatTimerId);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { UpdateAudioStats(); }, kStatTimerId);
        statTimer_.Start(250);

        canvas_->onPeerMoved = [this](int id, double x, double y) { SendPos(id, x, y); };

        // ---- network (callbacks arrive on the asio thread -> marshal to GUI)
        NetCallbacks cb;
        cb.onStatus = [this](const std::string& s) {
            CallAfter([this, s] { Log(wxString::FromUTF8(s)); });
        };
        cb.onPeerReady = [this](int id, std::uint32_t inst, const std::string& key,
                                const std::string& name, const std::string& addr, bool incoming) {
            CallAfter([this, id, inst, key, name, addr, incoming] {
                OnPeerReady(id, inst, key, wxString::FromUTF8(name), addr, incoming);
            });
        };
        cb.onPeerLost = [this](int id) {
            if (input_) input_->RemovePeer(id);     // hand input back immediately
            CallAfter([this, id] { OnPeerLost(id); });
        };
        cb.onMessage = [this](int id, const std::string& line) {
            // Keyboard/mouse traffic is injected right here on the network thread:
            // going through the GUI event loop would add visible lag.
            if (input_ && input_->HandleMessage(id, line)) return;
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
        net_ = std::make_unique<Network>(host_.ToStdString(), *ident_, std::move(cb));

        audio_ = std::make_unique<AudioEngine>(net_->Instance());
        {
            std::string err;
            if (audio_->Start(&err)) Log("Audio ready (UDP " + wxString::Format("%d", (int)kAudioPort) + ")");
            else                     Log("Audio problem: " + wxString::FromUTF8(err));
        }

        input_ = std::make_unique<InputEngine>(
            [this](int id, const std::string& line) { net_->Send(id, line); },
            [this](const std::string& msg) { CallAfter([this, msg] { Log("Input: " + wxString::FromUTF8(msg)); }); });
        {
            std::vector<InputRect> rects;
            for (const auto& m : localMons_) rects.push_back({m.x, m.y, m.width, m.height});
            input_->SetLocalMonitors(rects);
        }
        input_->onModeChange = [this](InputEngine::Mode m, int id) {
            CallAfter([this, m, id] { inputMode_ = m; inputPeer_ = id; RebuildDevices(); });
        };
        input_->onControlRequest = [this](int id) { CallAfter([this, id] { OnControlRequest(id); }); };
        {
            std::string msg;
            if (input_->Start(&msg)) Log("Keyboard/mouse sharing ready: " + wxString::FromUTF8(input_->BackendName()));
            else                     Log("Keyboard/mouse sharing unavailable: " + wxString::FromUTF8(msg));
            if (!msg.empty() && input_->BackendName() != "none") Log("Input: " + wxString::FromUTF8(msg));
        }

        RebuildList();
        Log("This PC's key: " + wxString::FromUTF8(ident_->Fingerprint()) + "   (data: " + dataDir_ + ")");
        if (!identNote_.empty()) Log("Identity: " + wxString::FromUTF8(identNote_));
        Log("Ready. Other seamless-input PCs connect automatically.");
        net_->Start();
        StartScan();
        Centre();
    }

private:
    static constexpr int kTimerId     = 9000;
    static constexpr int kAnimTimerId = 9001;
    static constexpr int kStatTimerId = 9002;

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
    struct PeerInfo {
        std::uint32_t inst = 0;
        std::string key;            // public key (hex), proven by the handshake
        wxString name;
        std::string addr;
        std::vector<wxRect> mons;
        bool incoming = false;
        bool placed = false;
    };

    // ---------------------------------------------------------------- pages
    wxWindow* BuildNetworkPage(wxWindow* parent)
    {
        auto* p = new wxPanel(parent);
        auto* bottom = new wxBoxSizer(wxHORIZONTAL);

        auto* left = new wxBoxSizer(wxVERTICAL);
        searchLbl_ = new wxStaticText(p, wxID_ANY, "");
        left->Add(searchLbl_, 0, wxBOTTOM, 4);

        list_ = new wxListCtrl(p, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxLC_REPORT | wxLC_SINGLE_SEL);
        list_->InsertColumn(0, "Name", wxLIST_FORMAT_LEFT, 200);
        list_->InsertColumn(1, "Address", wxLIST_FORMAT_LEFT, 130);
        list_->InsertColumn(2, "Status", wxLIST_FORMAT_LEFT, 140);
        left->Add(list_, 1, wxEXPAND);

        auto* row = new wxBoxSizer(wxHORIZONTAL);
        target_ = new wxTextCtrl(p, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
        target_->SetHint("target IP");
        goBtn_ = new wxButton(p, wxID_ANY, "Go");
        scanBtn_ = new wxButton(p, wxID_ANY, "Scan LAN");
        row->Add(target_, 1, wxRIGHT, 6);
        row->Add(goBtn_, 0, wxRIGHT, 6);
        row->Add(scanBtn_, 0);
        left->Add(row, 0, wxEXPAND | wxTOP, 6);

        log_ = new wxTextCtrl(p, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                              wxTE_MULTILINE | wxTE_READONLY);

        bottom->Add(left, 1, wxEXPAND | wxALL, 6);
        bottom->Add(log_, 1, wxEXPAND | wxTOP | wxRIGHT | wxBOTTOM, 6);
        p->SetSizer(bottom);
        return p;
    }

    wxWindow* BuildDevicesPage(wxWindow* parent)
    {
        auto* p = new wxPanel(parent);
        auto* v = new wxBoxSizer(wxVERTICAL);

        devList_ = new wxListCtrl(p, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxLC_REPORT | wxLC_SINGLE_SEL);
        devList_->InsertColumn(0, "Device", wxLIST_FORMAT_LEFT, 170);
        devList_->InsertColumn(1, "IP address", wxLIST_FORMAT_LEFT, 130);
        devList_->InsertColumn(2, "Screens", wxLIST_FORMAT_LEFT, 260);
        devList_->InsertColumn(3, "Audio from device", wxLIST_FORMAT_LEFT, 240);
        devList_->InsertColumn(4, "Keyboard / mouse", wxLIST_FORMAT_LEFT, 230);
        devList_->InsertColumn(5, "Key", wxLIST_FORMAT_LEFT, 150);
        v->Add(devList_, 1, wxEXPAND | wxALL, 6);

        auto* row = new wxBoxSizer(wxHORIZONTAL);
        sendChk_ = new wxCheckBox(p, wxID_ANY, "Share my audio");
        srcChoice_ = new wxChoice(p, wxID_ANY);
        srcChoice_->Append("Microphone");
        if (AudioEngine::SystemAudioSupported()) srcChoice_->Append("System audio (may echo)");
        srcChoice_->SetSelection(0);
        inGauge_ = new wxGauge(p, wxID_ANY, 100, wxDefaultPosition, wxSize(90, -1));
        muteChk_ = new wxCheckBox(p, wxID_ANY, "Mute incoming");
        volSlider_ = new wxSlider(p, wxID_ANY, 100, 0, 100, wxDefaultPosition, wxSize(130, -1));

        row->Add(sendChk_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        row->Add(srcChoice_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        row->Add(inGauge_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 20);
        row->Add(muteChk_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 20);
        row->Add(new wxStaticText(p, wxID_ANY, "Volume"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        row->Add(volSlider_, 0, wxALIGN_CENTER_VERTICAL);
        v->Add(row, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

        auto* irow = new wxBoxSizer(wxHORIZONTAL);
        inputChk_ = new wxCheckBox(p, wxID_ANY, "Share keyboard && mouse");
        inputChk_->SetValue(true);
        inputBtn_ = new wxButton(p, wxID_ANY, "Allow / block control of this PC by selected device");
        irow->Add(inputChk_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
        irow->Add(inputBtn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
        irow->Add(new wxStaticText(p, wxID_ANY, "Scroll Lock returns control to this PC"), 0, wxALIGN_CENTER_VERTICAL);
        v->Add(irow, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);
        inputChk_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { input_->SetEnabled(inputChk_->GetValue()); MarkCfgDirty(); });
        inputBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ToggleSelectedAllowed(); });

        p->SetSizer(v);
        return p;
    }

    // ---------------------------------------------------------------- actions
    void Activate(const std::string& ip)
    {
        if (ip.empty()) return;
        bool isLocal = ip.rfind("127.", 0) == 0;
        for (const auto& a : local_) isLocal = isLocal || a.ip == ip;

        selectedIp_ = ip;
        if (isLocal) Log("Selected this machine (" + wxString::FromUTF8(ip) + ")");
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
        input_->SetPeerOffset(id, x, y);
        net_->Send(id, "POS " + std::to_string(std::lround(x)) + " " + std::to_string(std::lround(y)));
        RecordPeerPos(id, x, y);
    }

    // ------------------------------------------------------------ saved configs
    // The config that belongs to the exact set of connected PCs (by public key) is
    // kept in cfg_. Every change goes into cfg_ right away and is written to disk
    // shortly afterwards; when the set changes the matching config is loaded.
    void UpdateCfgFromUi()
    {
        cfg_.audio.send   = sendChk_->GetValue();
        cfg_.audio.source = srcChoice_->GetSelection() == 1 ? 1 : 0;
        cfg_.audio.mute   = muteChk_->GetValue();
        cfg_.audio.volume = volSlider_->GetValue();
        cfg_.inputEnabled = inputChk_->GetValue();
    }

    void TouchCfg()
    {
        cfgDirty_ = true;
        dirtyAt_ = std::chrono::steady_clock::now();
    }

    void MarkCfgDirty()
    {
        if (!haveCfg_ || loading_) return;
        UpdateCfgFromUi();
        TouchCfg();
    }

    void RecordPeerPos(int id, double x, double y)
    {
        if (!haveCfg_) return;
        auto it = peers_.find(id);
        if (it == peers_.end() || it->second.key.empty()) return;
        SavedPeer& sp = cfg_.PeerRef(it->second.key);
        const std::string name = std::string(it->second.name.utf8_str());
        const bool changed = std::llround(sp.ox) != std::llround(x) || std::llround(sp.oy) != std::llround(y) ||
                             sp.name != name;
        sp.ox = x;
        sp.oy = y;
        sp.name = name;
        if (changed && !loading_) TouchCfg();
    }

    void FlushConfig()
    {
        if (!haveCfg_ || !cfgDirty_) return;
        cfgDirty_ = false;
        cfg_.rev = NowMs();
        std::string err;
        if (!store_->Save(cfg_, &err)) Log("Could not save config: " + wxString::FromUTF8(err));
    }

    // Puts the saved position of peer 'id' on the canvas (pushed off any overlap) and, when this
    // side accepted the connection, tells the peer. Without a saved position the current one is kept.
    void ApplyStoredLayout(int id)
    {
        auto it = peers_.find(id);
        if (it == peers_.end() || !it->second.placed) return;
        PeerInfo& p = it->second;
        double x = 0, y = 0;
        if (const SavedPeer* sp = haveCfg_ ? cfg_.FindPeer(p.key) : nullptr) {
            const auto pos = canvas_->PlacePeer(id, sp->ox, sp->oy);
            x = pos.first;
            y = pos.second;
        } else if (!canvas_->PeerPos(id, x, y)) {
            return;
        }
        if (p.incoming) SendPos(id, x, y);            // the accepting side is authoritative
        else { input_->SetPeerOffset(id, x, y); RecordPeerPos(id, x, y); }
    }

    // Audio / input settings of the config go into the widgets and the engines.
    void ApplyConfigSettings()
    {
        const SavedAudio& a = cfg_.audio;

        muteChk_->SetValue(a.mute);
        audio_->SetMuted(a.mute);
        volSlider_->SetValue(a.volume);
        audio_->SetVolume(a.volume / 100.0f);

        const int src = (a.source == 1 && srcChoice_->GetCount() > 1) ? 1 : 0;
        srcChoice_->SetSelection(src);
        sendChk_->SetValue(a.send);
        if (a.send != sendingNow_ || (a.send && src != sendingSrc_)) {   // capture is only restarted when needed
            ApplySending();
            if (a.send && sendingNow_) Log("Audio sharing switched on by the saved config");
        }

        const bool inputWas = inputChk_->GetValue();
        inputChk_->SetValue(cfg_.inputEnabled);
        if (inputWas != cfg_.inputEnabled) input_->SetEnabled(cfg_.inputEnabled);
    }

    // Called whenever a PC joins or leaves: save the old config, load (or start) the one
    // for the new set of PCs.
    void OnSetChanged()
    {
        FlushConfig();

        std::vector<std::string> members{ident_->PublicHex()};
        wxString names = host_;
        for (const auto& kv : peers_) {
            members.push_back(kv.second.key);
            names += " + " + kv.second.name;
        }
        members = ConfigStore::Normalize(members);

        if (members.size() < 2) { haveCfg_ = false; return; }          // alone: nothing to save
        if (haveCfg_ && cfg_.members == members) return;

        loading_ = true;
        SavedConfig loaded;
        const wxString id = wxString::FromUTF8(ConfigStore::SetId(members));
        if (store_->Load(members, loaded)) {
            cfg_ = std::move(loaded);
            haveCfg_ = true;
            Log("Loaded saved config " + id + " for " + names);
            ApplyConfigSettings();
        } else {
            cfg_ = SavedConfig{};                    // new combination: start from the current settings
            cfg_.members = members;
            haveCfg_ = true;
            UpdateCfgFromUi();
            TouchCfg();
            Log("New combination of PCs (" + names + ") - created config " + id);
        }
        cfg_.label = std::string(names.utf8_str());

        for (const auto& kv : peers_) ApplyStoredLayout(kv.first);   // peers that already sent SCREENS
        loading_ = false;
    }

    std::string ScreensLine() const
    {
        std::string s = "SCREENS " + std::to_string(localMons_.size());
        for (const auto& m : localMons_)
            s += " " + std::to_string(m.x) + " " + std::to_string(m.y) + " " +
                 std::to_string(m.width) + " " + std::to_string(m.height);
        return s;
    }

    void ApplySending()
    {
        const AudioSource src = srcChoice_->GetSelection() == 1 ? AudioSource::System : AudioSource::Microphone;
        const bool on = sendChk_->GetValue();
        std::string err;
        if (!audio_->SetSending(on, src, &err)) {
            Log("Audio capture failed: " + wxString::FromUTF8(err));
            sendChk_->SetValue(false);
            sendingNow_ = false;
            return;
        }
        sendingNow_ = on;
        sendingSrc_ = src == AudioSource::System ? 1 : 0;
        if (!on)                           Log("Stopped sharing audio");
        else if (src == AudioSource::System) Log("Sharing system audio with all connected PCs");
        else                               Log("Sharing microphone with all connected PCs");
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
    void OnPeerReady(int id, std::uint32_t inst, const std::string& key, const wxString& name,
                     const std::string& addr, bool incoming)
    {
        PeerInfo& p = peers_[id];
        p.inst = inst;
        p.key = key;
        p.name = name;
        p.addr = addr;
        p.incoming = incoming;
        p.placed = false;

        Log("Connection established with " + name + " (" + wxString::FromUTF8(addr) + "), key " +
            wxString::FromUTF8(Identity::Fingerprint(key)) + " verified");
        audio_->AddPeer(inst, addr);
        net_->Send(id, ScreensLine());      // tell the peer about our monitors
        RebuildList();
        RebuildDevices();
        OnSetChanged();                     // a new set of PCs: switch to its config
        // The virtual window appears once the peer's SCREENS line arrives.
    }

    void OnPeerLost(int id)
    {
        auto it = peers_.find(id);
        if (it != peers_.end()) {
            Log("Disconnected from " + it->second.name);
            audio_->RemovePeer(it->second.inst);
            peers_.erase(it);
        }
        canvas_->RemovePeer(id);            // virtual window disappears with the connection
        RebuildList();
        RebuildDevices();
        OnSetChanged();
    }

    void OnMessage(int id, const std::string& line)
    {
        auto it = peers_.find(id);
        if (it == peers_.end()) return;
        PeerInfo& p = it->second;

        if (line.rfind("SCREENS ", 0) == 0) {
            if (p.placed) return;
            std::istringstream is(line.substr(8));
            int n = 0;
            is >> n;
            std::vector<wxRect> mons;
            for (int i = 0; i < n && i < 16; ++i) {
                int x, y, w, h;
                if (!(is >> x >> y >> w >> h)) break;
                if (w > 0 && h > 0) mons.emplace_back(x, y, w, h);
            }
            if (mons.empty()) mons.emplace_back(0, 0, 1920, 1080);
            p.mons = mons;
            p.placed = true;

            // The side that accepted the connection decides the initial layout and sends it;
            // the connecting side only places a provisional window until that POS arrives.
            auto pos = canvas_->AddPeer(id, p.name, wxString::FromUTF8(p.addr), mons, p.incoming);
            {
                std::vector<InputRect> rects;
                for (const auto& m : mons) rects.push_back({m.x, m.y, m.width, m.height});
                input_->SetPeer(id, rects, pos.first, pos.second);
                input_->SetPeerAllowed(id, trust_->Get(p.key) == TrustStore::State::Allowed);
            }
            ApplyStoredLayout(id);              // saved position for this set of PCs (or the default one)
            RebuildDevices();
            return;
        }

        int x, y;
        if (p.placed && std::sscanf(line.c_str(), "POS %d %d", &x, &y) == 2)
        {
            canvas_->SetPeerPos(id, -x, -y);   // mirrored: their view of us is the opposite offset
            input_->SetPeerOffset(id, -x, -y);
            RecordPeerPos(id, -x, -y);
        }
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
        if (cfgDirty_ && now - dirtyAt_ > std::chrono::milliseconds(600)) FlushConfig();
        if (++tick_ % 120 == 0) StartScan();   // periodic rescan
    }

    // -------------------------------------------------------------------- GUI
    void RebuildList()
    {
        auto isConnected = [this](const std::string& ip) {
            for (auto& kv : peers_) if (kv.second.addr == ip) return true;
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

    // Devices tab: one row per connected PC
    void RebuildDevices()
    {
        devList_->Freeze();
        devList_->DeleteAllItems();
        for (const auto& kv : peers_) {
            const PeerInfo& p = kv.second;
            long row = devList_->InsertItem(devList_->GetItemCount(), p.name);
            devList_->SetItemData(row, kv.first);
            devList_->SetItem(row, 1, wxString::FromUTF8(p.addr));
            devList_->SetItem(row, 2, ScreensText(p.mons));
            devList_->SetItem(row, 3, "no audio");
            const auto ts = trust_->Get(p.key);
            wxString in = ts == TrustStore::State::Allowed ? "may control this PC"
                        : ts == TrustStore::State::Denied  ? "blocked" : "asks first";
            if (inputMode_ == InputEngine::Mode::Forwarding && inputPeer_ == kv.first) in = "YOU are controlling it";
            if (inputMode_ == InputEngine::Mode::Controlled && inputPeer_ == kv.first) in = "controlling this PC now";
            devList_->SetItem(row, 4, in);
            devList_->SetItem(row, 5, wxString::FromUTF8(Identity::Fingerprint(p.key)));
        }
        devList_->Thaw();
    }

    void SetTrust(const PeerInfo& p, TrustStore::State st)
    {
        std::string err;
        if (!trust_->Set(p.key, st, std::string(p.name.utf8_str()), &err))
            Log("Could not save permissions: " + wxString::FromUTF8(err));
    }

    // A peer reached its screen edge towards us: nothing happens until the user agrees.
    void OnControlRequest(int id)
    {
        auto it = peers_.find(id);
        if (it == peers_.end() || trust_->Get(it->second.key) == TrustStore::State::Denied) return;
        const PeerInfo& p = it->second;
        wxMessageDialog dlg(this,
            p.name + " (" + wxString::FromUTF8(p.addr) + ") wants to control the keyboard and mouse of THIS PC.\n\n"
            "Anything it types or clicks will happen here. Only allow PCs you own.\n\nAllow it?",
            "Input control request", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
        if (dlg.ShowModal() == wxID_YES) {
            SetTrust(p, TrustStore::State::Allowed);
            input_->SetPeerAllowed(id, true);
            Log("Allowed " + p.name + " to control this PC - move the cursor to its edge again");
        } else {
            SetTrust(p, TrustStore::State::Denied);
            Log("Blocked input control from " + p.name);
        }
        RebuildDevices();
    }

    void ToggleSelectedAllowed()
    {
        long row = devList_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (row < 0) return;
        const int id = (int)devList_->GetItemData(row);
        auto it = peers_.find(id);
        if (it == peers_.end()) return;
        const bool wasAllowed = trust_->Get(it->second.key) == TrustStore::State::Allowed;
        SetTrust(it->second, wasAllowed ? TrustStore::State::Denied : TrustStore::State::Allowed);
        input_->SetPeerAllowed(id, !wasAllowed);
        RebuildDevices();
    }

    void UpdateAudioStats()
    {
        inGauge_->SetValue((int)std::lround(std::min(1.0f, std::sqrt(audio_->InputLevel())) * 100));

        const auto stats = audio_->Stats();
        for (long row = 0; row < devList_->GetItemCount(); ++row) {
            auto pit = peers_.find((int)devList_->GetItemData(row));
            if (pit == peers_.end()) continue;

            wxString txt = "no audio";
            for (const auto& s : stats) {
                if (s.instance != pit->second.inst || !s.receiving) continue;
                const int bars = std::min(10, (int)(std::sqrt(s.level) * 10));
                txt = wxString::Format("receiving  %.0f kbps  ", s.kbps) +
                      wxString::FromUTF8(std::string((size_t)bars, '|'));
            }
            if (devList_->GetItemText(row, 3) != txt) devList_->SetItem(row, 3, txt);
        }
    }

    void Log(const wxString& msg)
    {
        log_->AppendText(wxDateTime::Now().FormatTime() + "  " + msg + "\n");
    }

    void OnClose(wxCloseEvent&)
    {
        FlushConfig();
        timer_.Stop();
        animTimer_.Stop();
        statTimer_.Stop();
        input_->Stop();   // return the keyboard/mouse before anything else shuts down
        audio_->Stop();
        net_->Stop();   // joins the asio thread: no more callbacks after this
        Destroy();
    }

    wxString host_;
    std::vector<LocalAddress> local_;
    std::vector<wxRect> localMons_;
    std::vector<Entry> entries_;
    std::map<std::string, Discovered> disc_;
    std::map<std::string, Scanned> scan_;
    std::map<int, PeerInfo> peers_;
    std::string selectedIp_;
    bool rebuilding_ = false;
    bool scanning_ = false;
    int tick_ = 0;
    int animStep_ = 0;

    Canvas* canvas_ = nullptr;
    wxNotebook* notebook_ = nullptr;
    wxListCtrl* list_ = nullptr;
    wxListCtrl* devList_ = nullptr;
    wxTextCtrl* target_ = nullptr;
    wxTextCtrl* log_ = nullptr;
    wxButton* goBtn_ = nullptr;
    wxButton* scanBtn_ = nullptr;
    wxStaticText* searchLbl_ = nullptr;
    wxCheckBox* sendChk_ = nullptr;
    wxCheckBox* muteChk_ = nullptr;
    wxChoice* srcChoice_ = nullptr;
    wxGauge* inGauge_ = nullptr;
    wxSlider* volSlider_ = nullptr;
    wxCheckBox* inputChk_ = nullptr;
    wxButton* inputBtn_ = nullptr;
    wxTimer timer_;
    wxTimer animTimer_;
    wxTimer statTimer_;
    std::unique_ptr<Network> net_;
    std::unique_ptr<AudioEngine> audio_;
    std::unique_ptr<InputEngine> input_;
    std::unique_ptr<TrustStore> trust_;           // peers (by public key) that may / may not control this PC; persisted
    std::unique_ptr<Identity> ident_;             // this PC's key pair
    std::unique_ptr<ConfigStore> store_;
    std::string identNote_;
    wxString dataDir_;
    SavedConfig cfg_;                             // config of the currently connected set of PCs
    bool haveCfg_ = false;                        // false while no other PC is connected
    bool cfgDirty_ = false;
    bool loading_ = false;
    std::chrono::steady_clock::time_point dirtyAt_;
    bool sendingNow_ = false;
    int sendingSrc_ = 0;
    InputEngine::Mode inputMode_ = InputEngine::Mode::Local;
    int inputPeer_ = -1;
};

class App : public wxApp {
public:
    bool OnInit() override
    {
        SetAppName("seamless-input");
        (new MainFrame)->Show();
        return true;
    }
};

wxIMPLEMENT_APP(App);
