#include <wx/wx.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/graphics.h>
#include <wx/scrolwin.h>
#include <wx/simplebook.h>
#include <wx/settings.h>
#include <wx/statline.h>

#ifdef __WXMSW__
#include <dwmapi.h>
#include <uxtheme.h>
#endif
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
#include <fstream>
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
        const wxColour fill = local ? wxColour(48, 108, 226) : wxColour(66, 54, 38);
        const wxColour edge = local ? wxColour(132, 180, 255) : wxColour(255, 176, 72);
        const wxColour text = local ? wxColour(246, 249, 255) : wxColour(255, 218, 160);

        int minX = INT_MAX, minY = INT_MAX;
        for (const auto& m : g.mons) {
            wxRect r = MonRect(g, m, v);
            minX = std::min(minX, r.x);
            minY = std::min(minY, r.y);
            dc.SetPen(wxPen(dragging ? *wxWHITE : edge, 2, local ? wxPENSTYLE_SOLID : wxPENSTYLE_SHORT_DASH));
            dc.SetBrush(wxBrush(fill));
            dc.DrawRoundedRectangle(r, std::min(8, std::max(2, std::min(r.width, r.height) / 10)));
            dc.SetTextForeground(text);
            Centered(dc, wxString::Format("%dx%d", m.width, m.height), r);
        }

        wxString label = g.name;
        if (!g.addr.empty()) label += "   " + g.addr;
        dc.SetTextForeground(text);
        wxFont f = dc.GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        dc.SetFont(f);
        dc.DrawText(label, minX, minY - dc.GetCharHeight() - 4);
        f.SetWeight(wxFONTWEIGHT_NORMAL);
        dc.SetFont(f);
    }

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC pdc(this);
        pdc.SetBackground(wxBrush(wxColour(24, 26, 31)));
        pdc.Clear();
        wxGCDC dc(pdc);                    // anti-aliased drawing

        wxSize s = GetClientSize();
        dc.SetPen(wxPen(wxColour(52, 56, 65)));
        for (int x = 16; x < s.x; x += 24)
            for (int y = 16; y < s.y; y += 24) dc.DrawPoint(x, y);   // dotted grid

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
// Look & feel: a small theme (follows the system's light/dark setting) and a few
// custom-drawn controls used by the Devices tab.
// ============================================================================
struct Theme {
    bool dark = false;
    wxColour page, card, border, track, text, sub, accent, ok, warn, bad;
};

static Theme MakeTheme()
{
    Theme t;
    t.dark = wxSystemSettings::GetAppearance().IsDark();
    t.text = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    if (t.dark) {
        t.page   = wxColour(30, 32, 37);
        t.card   = wxColour(41, 44, 52);
        t.border = wxColour(62, 66, 77);
        t.track  = wxColour(66, 70, 82);
        t.sub    = wxColour(150, 156, 170);
        t.accent = wxColour(88, 150, 255);
        t.ok     = wxColour(64, 200, 130);
        t.warn   = wxColour(245, 185, 66);
        t.bad    = wxColour(240, 96, 96);
    } else {
        t.page   = wxColour(241, 243, 248);
        t.card   = wxColour(255, 255, 255);
        t.border = wxColour(220, 224, 233);
        t.track  = wxColour(224, 228, 237);
        t.sub    = wxColour(108, 115, 130);
        t.accent = wxColour(48, 108, 226);
        t.ok     = wxColour(34, 170, 104);
        t.warn   = wxColour(230, 150, 20);
        t.bad    = wxColour(214, 62, 62);
    }
    return t;
}

// Children of a card must paint with the card's colour.
static void ApplyBackground(wxWindow* w, const wxColour& bg)
{
    w->SetBackgroundColour(bg);
    for (wxWindow* c : w->GetChildren()) ApplyBackground(c, bg);
}

// Rounded panel with a thin border and an optional coloured accent stripe on the left.
class CardPanel : public wxPanel {
public:
    CardPanel(wxWindow* parent, const Theme& t, bool withAccent)
        : wxPanel(parent, wxID_ANY), t_(t), hasAccent_(withAccent), accent_(t.ok)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &CardPanel::OnPaint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Refresh(); e.Skip(); });
    }

    void SetAccent(const wxColour& c)
    {
        if (c == accent_) return;
        accent_ = c;
        Refresh();
    }

    // Different card colour (used as a background hint, e.g. for seamless-input clients)
    void SetFill(const wxColour& c) { fill_ = c; hasFill_ = true; Refresh(); }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC pdc(this);
        pdc.SetBackground(wxBrush(t_.page));
        pdc.Clear();
        wxGCDC dc(pdc);
        const wxSize s = GetClientSize();
        dc.SetPen(wxPen(t_.border, 1));
        dc.SetBrush(wxBrush(hasFill_ ? fill_ : t_.card));
        dc.DrawRoundedRectangle(0, 0, s.x, s.y, 10);
        if (hasAccent_) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(accent_));
            dc.DrawRoundedRectangle(12, 14, 5, std::max(8, s.y - 28), 2.5);
        }
    }

    Theme t_;
    bool hasAccent_;
    wxColour accent_;
    wxColour fill_;
    bool hasFill_ = false;
};

// Thin rounded level meter (0..1): green, amber above 70 %, red above 90 %.
class LevelBar : public wxControl {
public:
    LevelBar(wxWindow* parent, const Theme& t, const wxSize& size)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, size, wxBORDER_NONE), t_(t)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(size);
        Bind(wxEVT_PAINT, &LevelBar::OnPaint, this);
    }

    void SetLevel(float v)
    {
        v = std::clamp(v, 0.0f, 1.0f);
        if (std::fabs(v - level_) < 0.01f && !(v == 0.0f && level_ != 0.0f)) return;
        level_ = v;
        Refresh();
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC pdc(this);
        pdc.SetBackground(wxBrush(GetBackgroundColour()));
        pdc.Clear();
        wxGCDC dc(pdc);
        const wxSize s = GetClientSize();
        const double r = s.y / 2.0;
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(t_.track));
        dc.DrawRoundedRectangle(0, 0, s.x, s.y, r);
        if (level_ > 0.004f) {
            const wxColour c = level_ > 0.9f ? t_.bad : level_ > 0.7f ? t_.warn : t_.ok;
            dc.SetBrush(wxBrush(c));
            dc.DrawRoundedRectangle(0, 0, std::max<double>(s.y, s.x * level_), s.y, r);
        }
    }

    Theme t_;
    float level_ = 0;
};

// Speaker icon that toggles between "sound on" and "muted" (drawn, no bitmaps needed).
// Small on/off switch (green when on).
class ToggleSwitch : public wxControl {
public:
    std::function<void(bool on)> onToggle;

    ToggleSwitch(wxWindow* parent, const Theme& t)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxSize(42, 24), wxBORDER_NONE), t_(t)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(42, 24));
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &ToggleSwitch::OnPaint, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { hover_ = true; Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hover_ = false; Refresh(); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            on_ = !on_;
            Refresh();
            if (onToggle) onToggle(on_);
        });
    }

    void SetValue(bool v) { if (v != on_) { on_ = v; Refresh(); } }
    bool GetValue() const { return on_; }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC pdc(this);
        pdc.SetBackground(wxBrush(GetBackgroundColour()));
        pdc.Clear();
        wxGCDC dc(pdc);
        wxGraphicsContext* gc = dc.GetGraphicsContext();
        if (!gc) return;
        const wxSize s = GetClientSize();
        const double h = s.y - 4;
        wxColour track = on_ ? t_.ok : t_.track;
        if (hover_ && !on_) track = t_.border;
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(track));
        gc->DrawRoundedRectangle(0, 2, s.x, h, h / 2);
        const double d = h - 6;
        gc->SetBrush(wxBrush(wxColour(255, 255, 255)));
        gc->DrawEllipse(on_ ? s.x - 3 - d : 3, 2 + 3, d, d);
    }

    Theme t_;
    bool on_ = false, hover_ = false;
};

// Rounded label with a tinted background ("Connected", "Waiting", ...).
class StatusPill : public wxControl {
public:
    StatusPill(wxWindow* parent, const Theme& t)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), t_(t)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        wxFont f = GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        SetFont(f);
        Bind(wxEVT_PAINT, &StatusPill::OnPaint, this);
        Set("-", t.sub);
    }

    void Set(const wxString& text, const wxColour& c)
    {
        if (text == text_ && c == col_) return;
        const bool resize = text != text_;
        text_ = text;
        col_ = c;
        if (resize) {
            const wxSize te = GetTextExtent(text_);
            SetMinSize(wxSize(te.x + 22, te.y + 8));
            InvalidateBestSize();
            if (GetParent()) GetParent()->Layout();
        }
        Refresh();
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC pdc(this);
        pdc.SetBackground(wxBrush(GetBackgroundColour()));
        pdc.Clear();
        wxGCDC dc(pdc);
        const wxSize s = GetClientSize();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(wxColour(col_.Red(), col_.Green(), col_.Blue(), t_.dark ? 46 : 34)));
        dc.DrawRoundedRectangle(0, 0, s.x, s.y, s.y / 2.0);
        dc.SetFont(GetFont());
        dc.SetTextForeground(col_);
        const wxSize te = dc.GetTextExtent(text_);
        dc.DrawText(text_, (s.x - te.x) / 2, (s.y - te.y) / 2);
    }

    Theme t_;
    wxString text_;
    wxColour col_;
};

// Small outlined rounded button (used instead of a toggle: "Pair", "Unpair", ...).
class PillButton : public wxControl {
public:
    std::function<void()> onClick;

    PillButton(wxWindow* parent, const Theme& t)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), t_(t), col_(t.accent)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        wxFont f = GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        SetFont(f);
        Bind(wxEVT_PAINT, &PillButton::OnPaint, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { hover_ = true; Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hover_ = false; down_ = false; Refresh(); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { down_ = true; Refresh(); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (!down_) return;
            down_ = false;
            Refresh();
            if (onClick) onClick();
        });
        Set("-", t.accent);
    }

    void Set(const wxString& text, const wxColour& c)
    {
        if (text == text_ && c == col_) return;
        const bool resize = text != text_;
        text_ = text;
        col_ = c;
        if (resize) {
            const wxSize te = GetTextExtent(text_);
            SetMinSize(wxSize(te.x + 28, te.y + 10));
            InvalidateBestSize();
            if (GetParent()) GetParent()->Layout();
        }
        Refresh();
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC pdc(this);
        pdc.SetBackground(wxBrush(GetBackgroundColour()));
        pdc.Clear();
        wxGCDC dc(pdc);
        const wxSize s = GetClientSize();
        const int a = down_ ? 70 : hover_ ? 52 : 0;
        dc.SetPen(wxPen(wxColour(col_.Red(), col_.Green(), col_.Blue(), 150), 1));
        dc.SetBrush(wxBrush(wxColour(col_.Red(), col_.Green(), col_.Blue(), a)));
        dc.DrawRoundedRectangle(0, 0, s.x, s.y, s.y / 2.0);
        dc.SetFont(GetFont());
        dc.SetTextForeground(col_);
        const wxSize te = dc.GetTextExtent(text_);
        dc.DrawText(text_, (s.x - te.x) / 2, (s.y - te.y) / 2);
    }

    Theme t_;
    wxString text_;
    wxColour col_;
    bool hover_ = false, down_ = false;
};

// Horizontal divider with a caption in the middle: ---- Other hosts ----
class SectionDivider : public wxControl {
public:
    SectionDivider(wxWindow* parent, const Theme& t)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), t_(t)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        wxFont f = GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        SetFont(f);
        Bind(wxEVT_PAINT, &SectionDivider::OnPaint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Refresh(); e.Skip(); });
        Set("");
    }

    void Set(const wxString& text)
    {
        if (text == text_ && GetMinSize().y > 0) return;
        text_ = text;
        SetMinSize(wxSize(-1, GetTextExtent("Ag").y + 14));
        InvalidateBestSize();
        Refresh();
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC pdc(this);
        pdc.SetBackground(wxBrush(GetBackgroundColour()));
        pdc.Clear();
        wxGCDC dc(pdc);
        const wxSize s = GetClientSize();
        dc.SetFont(GetFont());
        const wxSize te = dc.GetTextExtent(text_);
        const int gap = 12, y = s.y / 2;
        const int tx = std::max(0, (s.x - te.x) / 2);
        dc.SetPen(wxPen(t_.border, 1));
        if (tx > gap) {
            dc.DrawLine(0, y, tx - gap, y);
            dc.DrawLine(tx + te.x + gap, y, s.x, y);
        }
        dc.SetTextForeground(t_.sub);
        dc.DrawText(text_, tx, (s.y - te.y) / 2);
    }

    Theme t_;
    wxString text_;
};

// Round badge with the first letter of a name.
class Avatar : public wxControl {
public:
    Avatar(wxWindow* parent, const Theme& t, int size = 40)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxSize(size, size), wxBORDER_NONE), t_(t), col_(t.accent)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(size, size));
        Bind(wxEVT_PAINT, &Avatar::OnPaint, this);
    }

    void Set(const wxString& name, const wxColour& c)
    {
        wxString l = name.empty() ? wxString("?") : name.Left(1).Upper();
        if (l == letter_ && c == col_) return;
        letter_ = l;
        col_ = c;
        Refresh();
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC pdc(this);
        pdc.SetBackground(wxBrush(GetBackgroundColour()));
        pdc.Clear();
        wxGCDC dc(pdc);
        const wxSize s = GetClientSize();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(wxColour(col_.Red(), col_.Green(), col_.Blue(), t_.dark ? 52 : 38)));
        dc.DrawEllipse(0, 0, s.x, s.y);
        wxFont f = GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        f.SetPointSize(f.GetPointSize() + 4);
        dc.SetFont(f);
        dc.SetTextForeground(col_);
        const wxSize te = dc.GetTextExtent(letter_);
        dc.DrawText(letter_, (s.x - te.x) / 2, (s.y - te.y) / 2);
    }

    Theme t_;
    wxString letter_;
    wxColour col_;
};

class SpeakerButton : public wxControl {
public:
    std::function<void(bool muted)> onToggle;

    SpeakerButton(wxWindow* parent, const Theme& t, int size = 34)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxSize(size, size), wxBORDER_NONE), t_(t)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(size, size));
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &SpeakerButton::OnPaint, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { hover_ = true; Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hover_ = false; down_ = false; Refresh(); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { down_ = true; Refresh(); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (!down_) return;
            down_ = false;
            muted_ = !muted_;
            Refresh();
            if (onToggle) onToggle(muted_);
        });
        UpdateTip();
    }

    void SetMuted(bool m)
    {
        if (m == muted_) return;
        muted_ = m;
        UpdateTip();
        Refresh();
    }
    bool IsMuted() const { return muted_; }

private:
    void UpdateTip() { SetToolTip(muted_ ? "Muted - click to unmute" : "Click to mute"); }

    void OnPaint(wxPaintEvent&)
    {
        UpdateTip();
        wxAutoBufferedPaintDC pdc(this);
        pdc.SetBackground(wxBrush(GetBackgroundColour()));
        pdc.Clear();
        wxGCDC dc(pdc);
        wxGraphicsContext* gc = dc.GetGraphicsContext();
        if (!gc) return;

        const wxSize s = GetClientSize();
        if (hover_ || down_ || muted_) {
            wxColour bg = muted_ ? wxColour(t_.bad.Red(), t_.bad.Green(), t_.bad.Blue(), t_.dark ? 50 : 34)
                                 : t_.track;
            if (down_) bg = t_.track;
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(bg));
            gc->DrawRoundedRectangle(0, 0, s.x, s.y, 8);
        }

        const double k = std::min(s.x, s.y) / 36.0;
        gc->PushState();
        gc->Scale(k, k);
        gc->Translate((s.x / k - 36.0) / 2, (s.y / k - 36.0) / 2);
        const wxColour ink = muted_ ? t_.bad : t_.text;

        wxGraphicsPath body = gc->CreatePath();
        body.MoveToPoint(9, 14);
        body.AddLineToPoint(14, 14);
        body.AddLineToPoint(20.5, 8.5);
        body.AddLineToPoint(20.5, 27.5);
        body.AddLineToPoint(14, 22);
        body.AddLineToPoint(9, 22);
        body.CloseSubpath();
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(ink));
        gc->FillPath(body);

        gc->SetPen(wxPen(ink, 2, wxPENSTYLE_SOLID));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        if (muted_) {
            wxGraphicsPath x = gc->CreatePath();
            x.MoveToPoint(24.5, 14.5);  x.AddLineToPoint(30.5, 21.5);
            x.MoveToPoint(30.5, 14.5);  x.AddLineToPoint(24.5, 21.5);
            gc->StrokePath(x);
        } else {
            wxGraphicsPath w1 = gc->CreatePath();
            w1.AddArc(20.5, 18, 5.5, -0.9, 0.9, true);
            gc->StrokePath(w1);
            wxGraphicsPath w2 = gc->CreatePath();
            w2.AddArc(20.5, 18, 10, -0.8, 0.8, true);
            gc->StrokePath(w2);
        }
        gc->PopState();
    }

    Theme t_;
    bool muted_ = false, hover_ = false, down_ = false;
};


// Dark-mode helpers for native Windows controls (wxWidgets 3.2 draws them light otherwise).
static void DarkenNative(wxWindow* w, const wchar_t* theme = L"DarkMode_Explorer")
{
#ifdef __WXMSW__
    if (w && w->GetHWND()) SetWindowTheme((HWND)w->GetHWND(), theme, nullptr);
#else
    (void)w; (void)theme;
#endif
}

static void DarkenTree(wxWindow* w)
{
#ifdef __WXMSW__
    if (!w) return;
    if (dynamic_cast<wxChoice*>(w))        DarkenNative(w, L"DarkMode_CFD");
    else if (auto* lc = dynamic_cast<wxListCtrl*>(w)) {
        DarkenNative(w, L"DarkMode_Explorer");
        if (HWND hdr = ListView_GetHeader((HWND)lc->GetHWND())) SetWindowTheme(hdr, L"DarkMode_ItemsView", nullptr);
    }
    else if (dynamic_cast<wxButton*>(w) || dynamic_cast<wxTextCtrl*>(w) || dynamic_cast<wxScrolledWindow*>(w))
        DarkenNative(w, L"DarkMode_Explorer");
    for (wxWindow* c : w->GetChildren()) DarkenTree(c);
#else
    (void)w;
#endif
}

static void DarkTitleBar(wxWindow* w)
{
#ifdef __WXMSW__
    if (!w || !w->GetHWND()) return;
    BOOL on = TRUE;
    DwmSetWindowAttribute((HWND)w->GetHWND(), 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &on, sizeof on);
    DwmSetWindowAttribute((HWND)w->GetHWND(), 19 /*same, Windows 10 before 20H1*/, &on, sizeof on);
#else
    (void)w;
#endif
}

// Flat tab strip (replaces wxNotebook, whose white frame cannot be themed).
class TabBar : public wxControl {
public:
    std::function<void(int)> onSelect;

    TabBar(wxWindow* parent, const Theme& t, std::vector<wxString> labels)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 38), wxBORDER_NONE),
          t_(t), labels_(std::move(labels))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(-1, 38));
        Bind(wxEVT_PAINT, &TabBar::OnPaint, this);
        Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
            const int h = HitTest(e.GetPosition());
            if (h != hover_) { hover_ = h; SetCursor(h >= 0 ? wxCursor(wxCURSOR_HAND) : wxNullCursor); Refresh(); }
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hover_ = -1; Refresh(); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
            const int h = HitTest(e.GetPosition());
            if (h < 0 || h == sel_) return;
            sel_ = h;
            Refresh();
            if (onSelect) onSelect(h);
        });
    }

    void SetSelection(int i) { sel_ = i; Refresh(); }

private:
    static constexpr int kPad = 20;

    int TabWidth(wxDC& dc, size_t i) const { return dc.GetTextExtent(labels_[i]).x + 2 * kPad; }

    wxFont BoldFont() const
    {
        wxFont f = GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        return f;
    }

    int HitTest(const wxPoint& p) const
    {
        wxClientDC dc(const_cast<TabBar*>(this));
        dc.SetFont(BoldFont());
        int x = 4;
        for (size_t i = 0; i < labels_.size(); ++i) {
            const int w = TabWidth(dc, i);
            if (p.x >= x && p.x < x + w) return (int)i;
            x += w;
        }
        return -1;
    }

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC pdc(this);
        pdc.SetBackground(wxBrush(t_.page));
        pdc.Clear();
        wxGCDC dc(pdc);
        dc.SetFont(BoldFont());
        const wxSize s = GetClientSize();

        dc.SetPen(wxPen(t_.border, 1));
        dc.DrawLine(0, s.y - 1, s.x, s.y - 1);

        int x = 4;
        for (size_t i = 0; i < labels_.size(); ++i) {
            const int w = TabWidth(dc, i);
            const bool sel = (int)i == sel_;
            dc.SetTextForeground(sel ? t_.text : ((int)i == hover_ ? t_.text : t_.sub));
            const wxSize ext = dc.GetTextExtent(labels_[i]);
            dc.DrawText(labels_[i], x + kPad, (s.y - ext.y) / 2 - 1);
            if (sel) {
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(t_.accent));
                dc.DrawRoundedRectangle(x + 8, s.y - 3, w - 16, 3, 1.5);
            }
            x += w;
        }
    }

    Theme t_;
    std::vector<wxString> labels_;
    int sel_ = 0, hover_ = -1;
};

// ============================================================================
// Main window
// ============================================================================
class MainFrame : public wxFrame {
public:
    MainFrame()
        : wxFrame(nullptr, wxID_ANY, "seamless-input", wxDefaultPosition, wxSize(1100, 860)),
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
            netTrust_ = std::make_unique<TrustStore>(dir / "network-trust.txt");
            netTrust_->Load();
            groupFile_ = dir / "group.txt";
            {
                std::ifstream gi(groupFile_);
                std::string line;
                if (gi && std::getline(gi, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    groupPwd_ = line;
                }
            }
            dataDir_ = wxString::FromUTF8(dir.u8string());
        }

        SetBackgroundColour(theme_.page);
        auto* root = new wxBoxSizer(wxVERTICAL);

        // ---- header: host name, addresses, key
        {
            wxString ips;
            for (const auto& a : local_) ips += (ips.empty() ? "" : ", ") + wxString::FromUTF8(a.ip);
            auto* title = new wxStaticText(this, wxID_ANY, host_);
            wxFont tf = title->GetFont();
            tf.SetWeight(wxFONTWEIGHT_BOLD);
            tf.SetPointSize(tf.GetPointSize() + 5);
            title->SetFont(tf);
            auto* sub = new wxStaticText(this, wxID_ANY,
                wxString::Format("IP %s   |   Key %s   |   TCP %d   UDP %d (discovery)   UDP %d (audio)",
                                 ips.empty() ? wxString("-") : ips,
                                 wxString::FromUTF8(ident_->Fingerprint()),
                                 (int)kTcpPort, (int)kDiscoveryPort, (int)kAudioPort));
            sub->SetForegroundColour(theme_.sub);
            auto* hs = new wxBoxSizer(wxVERTICAL);
            hs->Add(title, 0, wxBOTTOM, 2);
            hs->Add(sub, 0);
            root->Add(hs, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);
        }

        // ---- monitor layout
        canvas_ = new Canvas(this);
        canvas_->SetLocal(host_, localMons_);
        root->Add(canvas_, 2, wxEXPAND | wxALL, 10);

        // ---- lower part: tabs
        tabs_ = new TabBar(this, theme_, {"Devices", "Network"});
        root->Add(tabs_, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
        pages_ = new wxSimplebook(this, wxID_ANY);
        pages_->SetBackgroundColour(theme_.page);
        pages_->AddPage(BuildDevicesPage(pages_), "Devices", true);
        pages_->AddPage(BuildNetworkPage(pages_), "Network");
        tabs_->onSelect = [this](int i) {
            pages_->ChangeSelection((size_t)i);
            if (wxWindow* pg = pages_->GetCurrentPage()) pg->Layout();
        };
        root->Add(pages_, 3, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        SetSizer(root);

        // ---- events
        goBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Activate(target_->GetValue().ToStdString()); });
        target_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { Activate(target_->GetValue().ToStdString()); });
        scanBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StartScan(); });
        groupBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ApplyGroupPassword(); });
        groupCtl_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { ApplyGroupPassword(); });

        masterMute_->onToggle = [this](bool m) { audio_->SetMuted(m); MarkCfgDirty(); };
        volSlider_->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
            audio_->SetVolume(volSlider_->GetValue() / 100.0f);
            masterPct_->SetLabelText(wxString::Format("%d%%", volSlider_->GetValue()));
            MarkCfgDirty();
        });

        Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);

        timer_.SetOwner(this, kTimerId);
        Bind(wxEVT_TIMER, &MainFrame::OnTimer, this, kTimerId);
        timer_.Start(1000);

        animTimer_.SetOwner(this, kAnimTimerId);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { ++animStep_; UpdateSearchLabel(); }, kAnimTimerId);

        statTimer_.SetOwner(this, kStatTimerId);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { UpdateAudioStats(); }, kStatTimerId);
        statTimer_.Start(250);

        canvas_->onPeerMoved = [this](int id, double x, double y) { SendPos(id, x, y); BroadcastRelative(id); };

        // ---- network (callbacks arrive on the asio thread -> marshal to GUI)
        NetCallbacks cb;
        cb.onStatus = [this](const std::string& s) {
            CallAfter([this, s] { Log(wxString::FromUTF8(s)); });
        };
        cb.onPeerReady = [this](int id, std::uint32_t inst, const std::string& key,
                                const std::string& name, const std::string& addr, bool incoming,
                                bool viaGroup) {
            CallAfter([this, id, inst, key, name, addr, incoming, viaGroup] {
                OnPeerReady(id, inst, key, wxString::FromUTF8(name), addr, incoming, viaGroup);
            });
        };
        cb.onPeerPending = [this](int id, std::uint32_t inst, const std::string& key,
                                  const std::string& name, const std::string& addr, bool incoming,
                                  bool theyTrust) {
            CallAfter([this, id, inst, key, name, addr, incoming, theyTrust] {
                OnPeerPending(id, inst, key, wxString::FromUTF8(name), addr, incoming, theyTrust);
            });
        };
        cb.onPeerPendingLost = [this](int id) {
            CallAfter([this, id] { if (pend_.erase(id)) RebuildClients(); });
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
        cb.onBeacon = [this](const BeaconInfo& b) {
            CallAfter([this, b] { OnBeacon(b); });
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
        Log(groupPwd_.empty()
            ? "Ready. No password set - PCs without a password connect automatically."
            : "Ready. PCs with the same group password connect automatically.");
        net_->SetGroup(groupPwd_);
        UpdateGroupLabel();
        net_->Start();
        StartScan();
        listDirty_ = true;      // force the first real layout of the client / other-host list
        RebuildClients();
        Layout();
        Centre();
        // The first page is not laid out until it gets a size event; do it once the frame is up.
        CallAfter([this] {
            Layout();
            if (wxWindow* pg = pages_->GetCurrentPage()) pg->Layout();
            for (size_t i = 0; i < pages_->GetPageCount(); ++i) pages_->GetPage(i)->Layout();
            if (cliScroll_) { cliSizer_->Layout(); cliScroll_->FitInside(); }
        });
        if (theme_.dark) { DarkTitleBar(this); DarkenTree(this); }
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
        std::string key;            // announced public key (proven only by the handshake)
        bool sameGroup = false;
    };
    // connected to us but neither in our group nor mutually trusted (yet)
    struct PendInfo {
        std::uint32_t inst = 0;
        std::string key;
        wxString name;
        std::string addr;
        bool incoming = false;
        bool theyTrust = false;
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
        bool viaGroup = false;      // accepted because of the group password (else: mutual trust)
        bool placed = false;
        bool share = false;         // send my audio (system audio) to this device
        bool mic = false;           // send my microphone to this device
        bool mute = false;          // mute what it sends me
        int  vol = 100;             // 0..100, what I hear from it
    };

    // ---------------------------------------------------------------- pages
    // Network tab: clients + other hosts on the left; group, manual connect and activity log on the right.
    wxWindow* BuildNetworkPage(wxWindow* parent)
    {
        auto* p = new wxPanel(parent);
        p->SetBackgroundColour(theme_.page);
        auto* root = new wxBoxSizer(wxHORIZONTAL);

        // ================= left: clients, other hosts
        auto* left = new wxBoxSizer(wxVERTICAL);

        left->Add(MakeSection(p, "Clients", clientsCount_), 0, wxEXPAND | wxBOTTOM, 6);
        cliScroll_ = new wxScrolledWindow(p, wxID_ANY, wxDefaultPosition, wxSize(-1, 200),
                                          wxVSCROLL | wxBORDER_NONE);
        cliScroll_->SetBackgroundColour(theme_.page);
        cliScroll_->SetScrollRate(0, 12);
        cliSizer_ = new wxBoxSizer(wxVERTICAL);
        cliEmpty_ = new wxStaticText(cliScroll_, wxID_ANY,
            "No seamless-input clients found yet.\nStart it on another PC in this network.",
            wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
        cliEmpty_->SetForegroundColour(theme_.sub);
        cliSizer_->Add(cliEmpty_, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 22);
        cliScroll_->SetSizer(cliSizer_);
        othDivider_ = new SectionDivider(cliScroll_, theme_);
        othDivider_->SetBackgroundColour(theme_.page);
        othEmpty_ = new wxStaticText(cliScroll_, wxID_ANY, "No other hosts found yet.",
                                     wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
        othEmpty_->SetForegroundColour(theme_.sub);
        othEmpty_->SetBackgroundColour(theme_.page);
        cliSizer_->Add(othDivider_, 0, wxEXPAND | wxTOP | wxBOTTOM, 4);
        cliSizer_->Add(othEmpty_, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 10);
        left->Add(cliScroll_, 1, wxEXPAND);

        // ================= right: group, connect, activity
        auto* right = new wxBoxSizer(wxVERTICAL);

        // --- group
        auto* groupCard = new CardPanel(p, theme_, false);
        {
            auto* gv = new wxBoxSizer(wxVERTICAL);
            gv->Add(MakeHeader(groupCard, "Group"), 0, wxBOTTOM, 2);
            gv->Add(MakeLabel(groupCard, "PCs with the same password connect automatically. Leave empty for no password.", true), 0, wxBOTTOM, 8);
            auto* gr = new wxBoxSizer(wxHORIZONTAL);
            groupCtl_ = new wxTextCtrl(groupCard, wxID_ANY, wxString::FromUTF8(groupPwd_), wxDefaultPosition,
                                       wxDefaultSize, wxTE_PASSWORD | wxTE_PROCESS_ENTER);
            groupCtl_->SetHint("password");
            groupBtn_ = new wxButton(groupCard, wxID_ANY, "Apply");
            gr->Add(groupCtl_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            gr->Add(groupBtn_, 0, wxALIGN_CENTER_VERTICAL);
            gv->Add(gr, 0, wxEXPAND);
            groupLbl_ = MakeLabel(groupCard, "", true);
            gv->Add(groupLbl_, 0, wxTOP, 8);
            auto* go = new wxBoxSizer(wxVERTICAL);
            go->Add(gv, 1, wxEXPAND | wxALL, 14);
            groupCard->SetSizer(go);
            ApplyBackground(groupCard, theme_.card);
            groupCtl_->SetBackgroundColour(theme_.page);
            groupCtl_->SetForegroundColour(theme_.text);
        }
        right->Add(groupCard, 0, wxEXPAND | wxBOTTOM, 8);

        // --- connect by address
        auto* connCard = new CardPanel(p, theme_, false);
        {
            auto* cv = new wxBoxSizer(wxVERTICAL);
            cv->Add(MakeHeader(connCard, "Connect"), 0, wxBOTTOM, 8);
            auto* r1 = new wxBoxSizer(wxHORIZONTAL);
            target_ = new wxTextCtrl(connCard, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
            target_->SetHint("target IP address");
            goBtn_ = new wxButton(connCard, wxID_ANY, "Go");
            r1->Add(target_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            r1->Add(goBtn_, 0, wxALIGN_CENTER_VERTICAL);
            cv->Add(r1, 0, wxEXPAND);
            auto* r2 = new wxBoxSizer(wxHORIZONTAL);
            scanBtn_ = new wxButton(connCard, wxID_ANY, "Scan LAN");
            searchLbl_ = new wxStaticText(connCard, wxID_ANY, "");
            r2->Add(scanBtn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
            r2->Add(searchLbl_, 1, wxALIGN_CENTER_VERTICAL);
            cv->Add(r2, 0, wxEXPAND | wxTOP, 8);
            auto* co = new wxBoxSizer(wxVERTICAL);
            co->Add(cv, 1, wxEXPAND | wxALL, 14);
            connCard->SetSizer(co);
            ApplyBackground(connCard, theme_.card);
            target_->SetBackgroundColour(theme_.page);
            target_->SetForegroundColour(theme_.text);
            searchLbl_->SetForegroundColour(theme_.sub);
        }
        right->Add(connCard, 0, wxEXPAND | wxBOTTOM, 8);

        // --- activity log
        auto* logCard = new CardPanel(p, theme_, false);
        {
            auto* lv = new wxBoxSizer(wxVERTICAL);
            lv->Add(MakeHeader(logCard, "Activity"), 0, wxBOTTOM, 6);
            log_ = new wxTextCtrl(logCard, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 60),
                                  wxTE_MULTILINE | wxTE_READONLY | wxBORDER_NONE);
            log_->SetFont(wxFont(wxFontInfo(wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT).GetPointSize()).Family(wxFONTFAMILY_TELETYPE)));
            lv->Add(log_, 1, wxEXPAND);
            auto* lo = new wxBoxSizer(wxVERTICAL);
            lo->Add(lv, 1, wxEXPAND | wxALL, 14);
            logCard->SetSizer(lo);
            ApplyBackground(logCard, theme_.card);
            log_->SetForegroundColour(theme_.text);
        }
        right->Add(logCard, 1, wxEXPAND);

        root->Add(left, 3, wxEXPAND | wxALL, 10);
        root->Add(right, 2, wxEXPAND | wxTOP | wxRIGHT | wxBOTTOM, 10);
        p->SetSizer(root);
        return p;
    }

    // "Title ............ count"
    wxSizer* MakeSection(wxWindow* parent, const wxString& title, wxStaticText*& count)
    {
        auto* h = new wxBoxSizer(wxHORIZONTAL);
        h->Add(MakeHeader(parent, title), 0, wxALIGN_CENTER_VERTICAL);
        h->AddStretchSpacer();
        count = MakeLabel(parent, "", true);
        h->Add(count, 0, wxALIGN_CENTER_VERTICAL);
        return h;
    }

    wxStaticText* MakeHeader(wxWindow* parent, const wxString& text)
    {
        auto* l = new wxStaticText(parent, wxID_ANY, text);
        wxFont f = l->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        l->SetFont(f);
        return l;
    }

    wxStaticText* MakeLabel(wxWindow* parent, const wxString& text, bool subtle = false)
    {
        auto* l = new wxStaticText(parent, wxID_ANY, text);
        if (subtle) l->SetForegroundColour(theme_.sub);
        return l;
    }

    wxWindow* BuildDevicesPage(wxWindow* parent)
    {
        auto* p = new wxPanel(parent);
        p->SetBackgroundColour(theme_.page);
        auto* v = new wxBoxSizer(wxVERTICAL);

        // ---- toolbar: master output + a reminder of the hotkey
        auto* bar = new CardPanel(p, theme_, false);
        auto* bv = new wxBoxSizer(wxVERTICAL);

        masterMute_ = new SpeakerButton(bar, theme_, 32);
        volSlider_ = new wxSlider(bar, wxID_ANY, 100, 0, 100, wxDefaultPosition, wxSize(150, -1));
        masterPct_ = MakeLabel(bar, "100%", true);
        masterPct_->SetMinSize(wxSize(44, -1));

        auto* r1 = new wxBoxSizer(wxHORIZONTAL);
        r1->Add(MakeLabel(bar, "Scroll Lock returns control to this PC", true), 0, wxALIGN_CENTER_VERTICAL);
        r1->AddStretchSpacer();
        r1->Add(MakeLabel(bar, "Master output", true), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        r1->Add(masterMute_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        r1->Add(volSlider_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        r1->Add(masterPct_, 0, wxALIGN_CENTER_VERTICAL);
        bv->Add(r1, 0, wxEXPAND | wxALL, 14);
        bar->SetSizer(bv);
        ApplyBackground(bar, theme_.card);
        masterPct_->SetForegroundColour(theme_.sub);

        v->Add(bar, 0, wxEXPAND | wxALL, 10);

        // ---- one card per connected device
        devScroll_ = new wxScrolledWindow(p, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxBORDER_NONE);
        devScroll_->SetBackgroundColour(theme_.page);
        devScroll_->SetScrollRate(0, 12);
        devSizer_ = new wxBoxSizer(wxVERTICAL);
        emptyLbl_ = new wxStaticText(devScroll_, wxID_ANY,
            "No devices connected yet.\nUse the same password on your PCs (or none on all of them).",
            wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
        emptyLbl_->SetForegroundColour(theme_.sub);
        devSizer_->Add(emptyLbl_, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 28);
        devScroll_->SetSizer(devSizer_);
        v->Add(devScroll_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

        p->SetSizer(v);
        return p;
    }

    // ---- one device card: info | share audio / microphone + allow control | mute, volume, level
    struct DevRow {
        CardPanel* card = nullptr;
        wxStaticText *name = nullptr, *sub = nullptr, *detail = nullptr, *pct = nullptr, *audioState = nullptr;
        wxCheckBox *share = nullptr, *mic = nullptr, *allow = nullptr;
        SpeakerButton* mute = nullptr;
        wxSlider* vol = nullptr;
        LevelBar* level = nullptr;
    };

    void BuildDeviceRow(int id)
    {
        DevRow r;
        r.card = new CardPanel(devScroll_, theme_, true);
        wxWindow* c = r.card;

        // info
        r.name = new wxStaticText(c, wxID_ANY, "");
        wxFont nf = r.name->GetFont();
        nf.SetWeight(wxFONTWEIGHT_BOLD);
        nf.SetPointSize(nf.GetPointSize() + 2);
        r.name->SetFont(nf);
        r.sub = MakeLabel(c, "", true);
        r.detail = MakeLabel(c, "", true);
        auto* info = new wxBoxSizer(wxVERTICAL);
        info->Add(r.name, 0, wxBOTTOM, 3);
        info->Add(r.sub, 0, wxBOTTOM, 2);
        info->Add(r.detail, 0);

        // switches
        r.share = new wxCheckBox(c, wxID_ANY, "Share my audio");
        r.share->SetToolTip("Send what this PC plays (system audio) to this device only.\nIt may echo if this device sends its audio back.");
        if (!AudioEngine::SystemAudioSupported()) {
            r.share->Disable();
            r.share->SetToolTip("Sharing system audio is not supported on this platform");
        }
        r.mic = new wxCheckBox(c, wxID_ANY, "Share my microphone");
        r.mic->SetToolTip("Send this PC's microphone to this device only");
        r.allow = new wxCheckBox(c, wxID_ANY, "Allow remote control");
        r.allow->SetToolTip("Let this device use this PC's keyboard and mouse");
        auto* checks = new wxBoxSizer(wxVERTICAL);
        checks->Add(r.share, 0, wxBOTTOM, 8);
        checks->Add(r.mic, 0, wxBOTTOM, 8);
        checks->Add(r.allow, 0);

        // audio from this device
        r.mute = new SpeakerButton(c, theme_, 34);
        r.vol = new wxSlider(c, wxID_ANY, 100, 0, 100, wxDefaultPosition, wxSize(170, -1));
        r.pct = MakeLabel(c, "100%", true);
        r.pct->SetMinSize(wxSize(44, -1));
        r.level = new LevelBar(c, theme_, wxSize(-1, 8));
        r.audioState = MakeLabel(c, "", true);
        auto* top = new wxBoxSizer(wxHORIZONTAL);
        top->Add(r.mute, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        top->Add(r.vol, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        top->Add(r.pct, 0, wxALIGN_CENTER_VERTICAL);
        auto* audio = new wxBoxSizer(wxVERTICAL);
        audio->Add(MakeLabel(c, "Audio from this device", true), 0, wxBOTTOM, 2);
        audio->Add(top, 0, wxEXPAND);
        audio->Add(r.level, 0, wxEXPAND | wxTOP, 4);
        audio->Add(r.audioState, 0, wxTOP, 3);
        audio->SetMinSize(wxSize(300, -1));

        auto* h = new wxBoxSizer(wxHORIZONTAL);
        h->Add(info, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
        h->Add(checks, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 24);
        h->Add(audio, 0, wxALIGN_CENTER_VERTICAL);
        auto* outer = new wxBoxSizer(wxHORIZONTAL);
        outer->AddSpacer(28);
        outer->Add(h, 1, wxEXPAND | wxTOP | wxBOTTOM | wxRIGHT, 14);
        r.card->SetSizer(outer);
        ApplyBackground(r.card, theme_.card);
        r.sub->SetForegroundColour(theme_.sub);
        r.detail->SetForegroundColour(theme_.sub);

        // events (looked up by connection id: the card may outlive a rebuild of the map)
        r.share->Bind(wxEVT_CHECKBOX, [this, id](wxCommandEvent& e) {
            auto it = peers_.find(id);
            if (it == peers_.end()) return;
            it->second.share = e.IsChecked();
            audio_->SetPeerSend(it->second.inst, AudioSource::System, it->second.share);
            Log((it->second.share ? "Sharing audio with " : "Stopped sharing audio with ") + it->second.name);
            RefreshCapture();
            RecordPeerAudio(it->second);
            RefreshDeviceRows();
        });
        r.mic->Bind(wxEVT_CHECKBOX, [this, id](wxCommandEvent& e) {
            auto it = peers_.find(id);
            if (it == peers_.end()) return;
            it->second.mic = e.IsChecked();
            audio_->SetPeerSend(it->second.inst, AudioSource::Microphone, it->second.mic);
            Log((it->second.mic ? "Sharing microphone with " : "Stopped sharing microphone with ") + it->second.name);
            RefreshCapture();
            RecordPeerAudio(it->second);
            RefreshDeviceRows();
        });
        r.allow->Bind(wxEVT_CHECKBOX, [this, id](wxCommandEvent& e) {
            auto it = peers_.find(id);
            if (it == peers_.end()) return;
            const bool on = e.IsChecked();
            SetTrust(it->second, on ? TrustStore::State::Allowed : TrustStore::State::Denied);
            input_->SetPeerAllowed(id, on);
            Log((on ? "Allowed " : "Blocked ") + it->second.name + (on ? " to control this PC" : " from controlling this PC"));
            RefreshDeviceRows();
        });
        r.mute->onToggle = [this, id](bool m) {
            auto it = peers_.find(id);
            if (it == peers_.end()) return;
            it->second.mute = m;
            audio_->SetPeerMuted(it->second.inst, m);
            RecordPeerAudio(it->second);
        };
        r.vol->Bind(wxEVT_SLIDER, [this, id](wxCommandEvent& e) {
            auto it = peers_.find(id);
            if (it == peers_.end()) return;
            it->second.vol = e.GetInt();
            audio_->SetPeerVolume(it->second.inst, it->second.vol / 100.0f);
            auto rit = devRows_.find(id);
            if (rit != devRows_.end()) rit->second.pct->SetLabelText(wxString::Format("%d%%", it->second.vol));
            RecordPeerAudio(it->second);
        });

        devSizer_->Add(r.card, 0, wxEXPAND | wxBOTTOM, 10);
        devRows_[id] = r;
        RefreshDeviceRow(devRows_[id]);
    }

    static void SetText(wxStaticText* l, const wxString& t)
    {
        if (l->GetLabelText() != t) l->SetLabelText(t);
    }

    void RefreshDeviceRow(DevRow& r)
    {
        int id = -1;
        for (const auto& kv : devRows_) if (kv.second.card == r.card) { id = kv.first; break; }
        auto it = peers_.find(id);
        if (it == peers_.end()) return;
        const PeerInfo& p = it->second;
        const wxString dot = wxString::FromUTF8("  \xC2\xB7  ");

        SetText(r.name, p.name);
        SetText(r.sub, wxString::FromUTF8(p.addr) + dot + ScreensText(p.mons));

        const auto ts = trust_->Get(p.key);
        wxString in;
        wxColour accent = theme_.ok;
        if (inputMode_ == InputEngine::Mode::Forwarding && inputPeer_ == id)  { in = "you are controlling it now"; accent = theme_.accent; }
        else if (inputMode_ == InputEngine::Mode::Controlled && inputPeer_ == id) { in = "it is controlling this PC now"; accent = theme_.warn; }
        else if (ts == TrustStore::State::Allowed) in = "may control this PC";
        else if (ts == TrustStore::State::Denied)  in = "blocked from controlling this PC";
        else                                       in = "asks before controlling this PC";
        SetText(r.detail, "Key " + wxString::FromUTF8(Identity::Fingerprint(p.key)) + dot + in);
        r.card->SetAccent(accent);

        r.share->SetValue(p.share);
        r.mic->SetValue(p.mic);
        r.allow->SetValue(ts == TrustStore::State::Allowed);
        r.mute->SetMuted(p.mute);
        r.vol->SetValue(p.vol);
        SetText(r.pct, wxString::Format("%d%%", p.vol));
    }

    void RefreshDeviceRows()
    {
        for (auto& kv : devRows_) RefreshDeviceRow(kv.second);
    }

    // ----------------------------------------------------------- group password
    void UpdateGroupLabel()
    {
        groupLbl_->SetLabelText(groupPwd_.empty()
            ? wxString::FromUTF8("\xE2\x97\x8F  No password - open to PCs without a password")
            : wxString::FromUTF8("\xE2\x97\x8F  Password active"));
        groupLbl_->SetForegroundColour(groupPwd_.empty() ? theme_.warn : theme_.ok);
        groupLbl_->Refresh();
    }

    void ApplyGroupPassword()
    {
        groupPwd_ = std::string(groupCtl_->GetValue().utf8_str());
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
        net_->SetGroup(groupPwd_);
        UpdateGroupLabel();
        Log(groupPwd_.empty() ? "Password removed - reconnecting"
                              : "Password set - reconnecting");
    }

    // ------------------------------------------------------------ clients (Network tab)
    struct CliView {
        std::string key, ip;
        wxString name;
        bool connected = false, viaGroup = false;
        bool pending = false, theyTrust = false;
        bool sameGroup = false, seen = false;
    };

    // One entry per PC (by public key) that runs seamless-input: seen on the LAN, connected,
    // waiting for trust, or trusted earlier.
    std::vector<CliView> ClientViews() const
    {
        std::map<std::string, CliView> m;
        auto at = [&m](const std::string& key) -> CliView& { CliView& v = m[key]; v.key = key; return v; };
        for (const auto& kv : disc_) {
            if (kv.second.key.empty()) continue;
            CliView& v = at(kv.second.key);
            v.ip = kv.first;
            v.name = wxString::FromUTF8(kv.second.host);
            v.sameGroup = kv.second.sameGroup;
            v.seen = true;
        }
        for (const auto& kv : pend_) {
            CliView& v = at(kv.second.key);
            v.pending = true;
            v.theyTrust = kv.second.theyTrust;
            v.ip = kv.second.addr;
            if (v.name.empty()) v.name = kv.second.name;
        }
        for (const auto& kv : peers_) {
            if (kv.second.key.empty()) continue;
            CliView& v = at(kv.second.key);
            v.connected = true;
            v.viaGroup = kv.second.viaGroup;
            v.ip = kv.second.addr;
            v.name = kv.second.name;
        }
        std::vector<CliView> out;
        for (auto& kv : m) out.push_back(std::move(kv.second));
        std::sort(out.begin(), out.end(), [](const CliView& a, const CliView& b) {
            const int c = a.name.CmpNoCase(b.name);
            return c != 0 ? c < 0 : a.key < b.key;
        });
        return out;
    }

    wxColour ClientTint() const
    {
        auto mix = [](int a, int b) { return (unsigned char)(a + (b - a) * 12 / 100); };   // 12 % accent
        return wxColour(mix(theme_.card.Red(), theme_.accent.Red()),
                        mix(theme_.card.Green(), theme_.accent.Green()),
                        mix(theme_.card.Blue(), theme_.accent.Blue()));
    }

    struct CliRow {
        CardPanel* card = nullptr;
        Avatar* avatar = nullptr;
        wxStaticText *name = nullptr, *sub = nullptr, *detail = nullptr;
        StatusPill* pill = nullptr;
    };

    void BuildClientRow(const CliView& v)
    {
        CliRow r;
        const wxColour tint = ClientTint();
        r.card = new CardPanel(cliScroll_, theme_, true);
        r.card->SetFill(tint);                    // tinted background = "this PC runs seamless-input"
        wxWindow* c = r.card;

        r.avatar = new Avatar(c, theme_, 40);
        r.name = new wxStaticText(c, wxID_ANY, "");
        wxFont nf = r.name->GetFont();
        nf.SetWeight(wxFONTWEIGHT_BOLD);
        nf.SetPointSize(nf.GetPointSize() + 2);
        r.name->SetFont(nf);
        r.sub = MakeLabel(c, "", true);
        r.detail = MakeLabel(c, "", true);
        auto* info = new wxBoxSizer(wxVERTICAL);
        info->Add(r.name, 0, wxBOTTOM, 2);
        info->Add(r.sub, 0, wxBOTTOM, 2);
        info->Add(r.detail, 0);

        r.pill = new StatusPill(c, theme_);
        auto* right = new wxBoxSizer(wxVERTICAL);
        right->Add(r.pill, 0, wxALIGN_RIGHT);

        auto* h = new wxBoxSizer(wxHORIZONTAL);
        h->Add(r.avatar, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
        h->Add(info, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
        h->Add(right, 0, wxALIGN_CENTER_VERTICAL);
        auto* outer = new wxBoxSizer(wxHORIZONTAL);
        outer->AddSpacer(28);
        outer->Add(h, 1, wxEXPAND | wxTOP | wxBOTTOM | wxRIGHT, 12);
        r.card->SetSizer(outer);
        ApplyBackground(r.card, tint);
        r.sub->SetForegroundColour(theme_.sub);
        r.detail->SetForegroundColour(theme_.sub);

        cliRows_[v.key] = r;
    }

    void RefreshClientRow(CliRow& r, const CliView& v)
    {
        const wxString dot = wxString::FromUTF8("  \xC2\xB7  ");

        SetText(r.name, v.name.empty() ? wxString("(unknown)") : v.name);
        SetText(r.sub, (v.ip.empty() ? wxString("offline") : wxString::FromUTF8(v.ip)) + dot + "Key " +
                       wxString::FromUTF8(Identity::Fingerprint(v.key)));

        wxString pill, detail;
        wxColour col = theme_.sub;
        if (v.connected) {
            pill = "Connected";
            detail = groupPwd_.empty() ? "No password" : "Same password";
            col = theme_.ok;
        } else if (v.sameGroup) {
            pill = "Connecting";
            detail = groupPwd_.empty() ? "No password" : "Same password";
            col = theme_.accent;
        } else {
            pill = "Different password";
            detail = groupPwd_.empty() ? "That PC uses a password" : "Set the same password (or none) on both PCs";
            col = theme_.warn;
        }
        SetText(r.detail, detail);
        r.pill->Set(pill, col);
        r.avatar->Set(v.name, v.connected || v.sameGroup ? col : theme_.sub);
        r.card->SetAccent(col == theme_.sub ? theme_.border : col);
    }

    // ------------------------------------------------------------ other hosts (Network tab)
    struct OthRow {
        CardPanel* card = nullptr;
        Avatar* avatar = nullptr;
        wxStaticText *name = nullptr, *sub = nullptr, *detail = nullptr;
        StatusPill* pill = nullptr;
    };

    // Clicking anywhere on a card (or its labels) selects that host.
    void BindCardClick(wxWindow* w, const std::function<void()>& fn)
    {
        w->SetCursor(wxCursor(wxCURSOR_HAND));
        w->Bind(wxEVT_LEFT_UP, [fn](wxMouseEvent& e) { fn(); e.Skip(); });
        for (wxWindow* c : w->GetChildren()) BindCardClick(c, fn);
    }

    void BuildOtherRow(const Entry& e)
    {
        OthRow r;
        r.card = new CardPanel(cliScroll_, theme_, true);
        wxWindow* c = r.card;

        r.avatar = new Avatar(c, theme_, 40);
        r.name = new wxStaticText(c, wxID_ANY, "");
        wxFont nf = r.name->GetFont();
        nf.SetWeight(wxFONTWEIGHT_BOLD);
        nf.SetPointSize(nf.GetPointSize() + 2);
        r.name->SetFont(nf);
        r.sub = MakeLabel(c, "", true);
        r.detail = MakeLabel(c, "", true);
        auto* info = new wxBoxSizer(wxVERTICAL);
        info->Add(r.name, 0, wxBOTTOM, 2);
        info->Add(r.sub, 0, wxBOTTOM, 2);
        info->Add(r.detail, 0);

        r.pill = new StatusPill(c, theme_);

        auto* h = new wxBoxSizer(wxHORIZONTAL);
        h->Add(r.avatar, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
        h->Add(info, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
        h->Add(r.pill, 0, wxALIGN_CENTER_VERTICAL);
        auto* outer = new wxBoxSizer(wxHORIZONTAL);
        outer->AddSpacer(28);
        outer->Add(h, 1, wxEXPAND | wxTOP | wxBOTTOM | wxRIGHT, 12);
        r.card->SetSizer(outer);
        ApplyBackground(r.card, theme_.card);
        r.sub->SetForegroundColour(theme_.sub);
        r.detail->SetForegroundColour(theme_.sub);

        const std::string ip = e.ip;
        r.card->SetToolTip("Click to connect to this host");
        BindCardClick(r.card, [this, ip] {
            target_->SetValue(wxString::FromUTF8(ip));
            Activate(ip);
        });

        othRows_[ip] = r;
    }

    void RebuildOthers()
    {
        if (!cliScroll_) return;
        bool same = othRows_.size() == entries_.size();
        for (const auto& e : entries_) same = same && othRows_.count(e.ip);
        if (!same) {
            for (auto& kv : othRows_) kv.second.card->Destroy();
            othRows_.clear();
            for (const auto& e : entries_) BuildOtherRow(e);
            listDirty_ = true;
        }
        for (const auto& e : entries_) {
            OthRow& r = othRows_[e.ip];
            const bool server = e.status == "Server running";
            const wxColour col = server ? theme_.accent : theme_.sub;
            SetText(r.name, e.name);
            SetText(r.sub, wxString::FromUTF8(e.ip));
            SetText(r.detail, server ? "Server running - click to connect" : "Click to connect");
            r.pill->Set(e.status, col);
            r.avatar->Set(e.name, col);
            r.card->SetAccent(server ? col : theme_.border);
        }
    }

    // One list: client cards, then the "Other hosts" divider and the other-host cards right
    // below the last client, so the second group follows the first however long it gets.
    void ReflowList(const std::vector<CliView>& views)
    {
        listDirty_ = false;
        cliScroll_->Freeze();
        cliSizer_->Clear(false);   // detach only; the windows stay alive
        cliSizer_->Add(cliEmpty_, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 22);
        cliEmpty_->Show(views.empty());
        if (views.empty()) cliSizer_->AddSpacer(8);
        for (const auto& v : views)
            cliSizer_->Add(cliRows_[v.key].card, 0, wxEXPAND | wxBOTTOM, 8);
        cliSizer_->Add(othDivider_, 0, wxEXPAND | wxTOP | wxBOTTOM, 4);
        cliSizer_->Add(othEmpty_, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, 10);
        othEmpty_->Show(entries_.empty());
        for (const auto& e : entries_)
            cliSizer_->Add(othRows_[e.ip].card, 0, wxEXPAND | wxBOTTOM, 8);
        cliSizer_->Layout();
        cliScroll_->FitInside();
        cliScroll_->Thaw();
        cliScroll_->Refresh();
    }

    void RebuildClients()
    {
        if (!cliScroll_) return;
        const auto views = ClientViews();
        bool same = cliRows_.size() == views.size();
        for (const auto& v : views) same = same && cliRows_.count(v.key);
        if (!same) {
            for (auto& kv : cliRows_) kv.second.card->Destroy();
            cliRows_.clear();
            for (const auto& v : views) BuildClientRow(v);
            listDirty_ = true;
        }
        if (listDirty_) ReflowList(views);
        for (const auto& v : views) RefreshClientRow(cliRows_[v.key], v);
        int conn = 0;
        for (const auto& v : views) conn += v.connected ? 1 : 0;
        SetText(clientsCount_, wxString::Format("%d found, %d connected", (int)views.size(), conn));
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

    // With 3+ PCs every PC keeps the whole layout in its own frame. When the user drags PC X
    // here, the other PCs must learn where X now is relative to THEM (and X where they are),
    // otherwise PC2 and PC3 would disagree about their own neighbourhood.
    void BroadcastRelative(int movedId)
    {
        auto mit = peers_.find(movedId);
        double mx = 0, my = 0;
        if (mit == peers_.end() || mit->second.key.empty() || !canvas_->PeerPos(movedId, mx, my)) return;
        for (const auto& kv : peers_) {
            if (kv.first == movedId || !kv.second.placed || kv.second.key.empty()) continue;
            double qx = 0, qy = 0;
            if (!canvas_->PeerPos(kv.first, qx, qy)) continue;
            net_->Send(kv.first, "PEERPOS " + mit->second.key + " " +
                       std::to_string(std::llround(mx - qx)) + " " + std::to_string(std::llround(my - qy)));
            net_->Send(movedId, "PEERPOS " + kv.second.key + " " +
                       std::to_string(std::llround(qx - mx)) + " " + std::to_string(std::llround(qy - my)));
        }
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
        cfg_.audio.mute   = masterMute_->IsMuted();
        cfg_.audio.volume = volSlider_->GetValue();
        cfg_.inputEnabled = true;     // legacy field: permissions live in trust.txt, per device
        for (const auto& kv : peers_) RecordPeerAudio(kv.second, true);
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

    // 'force': also while loading / for the initial fill of a new config (caller handles saving)
    void RecordPeerAudio(const PeerInfo& p, bool force = false)
    {
        if (!haveCfg_ || p.key.empty() || (loading_ && !force)) return;
        SavedPeer& sp = cfg_.PeerRef(p.key);
        sp.hasAudio = true;
        sp.send = p.share;
        sp.mic = p.mic;
        sp.hasMic = true;
        sp.mute = p.mute;
        sp.volume = p.vol;
        if (!force) TouchCfg();
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
        sp.hasPos = true;
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
        const SavedPeer* sp = haveCfg_ ? cfg_.FindPeer(p.key) : nullptr;
        if (sp && sp->hasPos) {
            const auto pos = canvas_->PlacePeer(id, sp->ox, sp->oy);
            x = pos.first;
            y = pos.second;
        } else if (!canvas_->PeerPos(id, x, y)) {
            return;
        }
        if (p.incoming) SendPos(id, x, y);            // the accepting side is authoritative
        else { input_->SetPeerOffset(id, x, y); RecordPeerPos(id, x, y); }
    }

    void PushPeerAudio(const PeerInfo& p)
    {
        audio_->SetPeerSend(p.inst, AudioSource::System, p.share);
        audio_->SetPeerSend(p.inst, AudioSource::Microphone, p.mic);
        audio_->SetPeerMuted(p.inst, p.mute);
        audio_->SetPeerVolume(p.inst, p.vol / 100.0f);
    }

    // Audio / input settings of the config go into the widgets and the engines.
    void ApplyConfigSettings()
    {
        const SavedAudio& a = cfg_.audio;

        masterMute_->SetMuted(a.mute);
        audio_->SetMuted(a.mute);
        volSlider_->SetValue(a.volume);
        masterPct_->SetLabelText(wxString::Format("%d%%", a.volume));
        audio_->SetVolume(a.volume / 100.0f);

        for (auto& kv : peers_) {
            PeerInfo& p = kv.second;
            const SavedPeer* sp = cfg_.FindPeer(p.key);
            if (sp && sp->hasAudio) { p.share = sp->send; p.mic = sp->mic; p.mute = sp->mute; p.vol = sp->volume; }
            else {                                                      // old file: one switch and one source for all
                p.share = a.send && a.source == 1;
                p.mic   = a.send && a.source != 1;
                p.mute = false; p.vol = 100;
            }
            PushPeerAudio(p);
        }
        RefreshCapture();
        if (sysCapturing_ || micCapturing_) Log("Audio sharing switched on by the saved config");
        RefreshDeviceRows();

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

    // Each source is captured (on its default device) while at least one device has the
    // matching box ticked: "Share my audio" = system audio, "Share my microphone" = mic.
    void RefreshCapture()
    {
        bool anySys = false, anyMic = false;
        for (const auto& kv : peers_) { anySys = anySys || kv.second.share; anyMic = anyMic || kv.second.mic; }

        UpdateCapture(AudioSource::System,     anySys, sysCapturing_, "system audio");
        UpdateCapture(AudioSource::Microphone, anyMic, micCapturing_, "microphone");
    }

    void UpdateCapture(AudioSource src, bool want, bool& capturing, const char* what)
    {
        if (want == capturing) return;
        std::string err;
        if (!audio_->SetCapture(src, want, &err)) {
            Log(wxString("Capturing ") + what + " failed: " + wxString::FromUTF8(err));
            for (auto& kv : peers_) {
                bool& flag = src == AudioSource::System ? kv.second.share : kv.second.mic;
                flag = false;
                audio_->SetPeerSend(kv.second.inst, src, false);
                RecordPeerAudio(kv.second);
            }
            capturing = false;
            RefreshDeviceRows();
            return;
        }
        capturing = want;
        Log(wxString(want ? "Capturing " : "Stopped capturing ") + what);
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
    void OnPeerPending(int id, std::uint32_t inst, const std::string& key, const wxString& name,
                       const std::string& addr, bool incoming, bool theyTrust)
    {
        PendInfo& pi = pend_[id];
        const bool isNew = pi.key.empty();
        const bool trustChanged = !isNew && pi.theyTrust != theyTrust;
        pi.inst = inst; pi.key = key; pi.name = name; pi.addr = addr; pi.incoming = incoming; pi.theyTrust = theyTrust;
        if (isNew)
            Log(name + " (" + wxString::FromUTF8(addr) + "), key " + wxString::FromUTF8(Identity::Fingerprint(key)) +
                " verified - different password, not connected");
        if (trustChanged) Log(name + (theyTrust ? " trusts this PC" : " no longer trusts this PC"));
        RebuildClients();
    }

    void OnPeerReady(int id, std::uint32_t inst, const std::string& key, const wxString& name,
                     const std::string& addr, bool incoming, bool viaGroup)
    {
        pend_.erase(id);
        PeerInfo& p = peers_[id];
        p.viaGroup = viaGroup;
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
        pend_.erase(id);
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

        {   // PEERPOS <key> <x> <y>: where the PC with that key is, relative to THIS PC
            char key[80] = {};
            int rx = 0, ry = 0;
            if (p.placed && std::sscanf(line.c_str(), "PEERPOS %64s %d %d", key, &rx, &ry) == 3) {
                for (auto& kv : peers_) {
                    if (kv.first == id || !kv.second.placed || kv.second.key != key) continue;
                    canvas_->SetPeerPos(kv.first, rx, ry);
                    input_->SetPeerOffset(kv.first, rx, ry);
                    RecordPeerPos(kv.first, rx, ry);
                }
                return;
            }
        }

        int x, y;
        if (p.placed && std::sscanf(line.c_str(), "POS %d %d", &x, &y) == 2)
        {
            canvas_->SetPeerPos(id, -x, -y);   // mirrored: their view of us is the opposite offset
            input_->SetPeerOffset(id, -x, -y);
            RecordPeerPos(id, -x, -y);
        }
    }

    void OnBeacon(const BeaconInfo& b)
    {
        auto it = disc_.find(b.ip);
        bool changed = it == disc_.end() || it->second.host != b.host || it->second.server != b.server ||
                       it->second.key != b.key || it->second.sameGroup != b.sameGroup;
        disc_[b.ip] = {b.host, b.server, std::chrono::steady_clock::now(), b.key, b.sameGroup};
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
        // PCs that run seamless-input are shown as client cards, not in this list
        auto isClient = [this](const std::string& ip) {
            if (disc_.count(ip)) return true;
            for (auto& kv : peers_) if (kv.second.addr == ip) return true;
            for (auto& kv : pend_)  if (kv.second.addr == ip) return true;
            return false;
        };

        auto key = [](const std::string& ip) {
            unsigned a = 0, b = 0, c = 0, d = 0;
            std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d);
            return (std::uint32_t)((a << 24) | (b << 16) | (c << 8) | d);
        };

        // other hosts found by the scan (everything that is not a seamless-input client)
        std::map<std::uint32_t, Entry> remote;
        for (const auto& kv : scan_) {
            if (isClient(kv.first)) continue;
            const auto& s = kv.second;
            remote[key(kv.first)] = {s.name.empty() ? wxString("(unknown)") : s.name, kv.first,
                                     s.server ? "Server running" : "Online", false};
        }

        entries_.clear();
        for (auto& kv : remote) entries_.push_back(kv.second);

        RebuildOthers();
        {
            int others = 0;
            for (const auto& e : entries_) others += e.local ? 0 : 1;
            othDivider_->Set(wxString::Format("Other hosts  (IP addresses)  -  %d", others));
        }
        RebuildClients();
    }

    // Devices tab: one card per connected PC. Cards are only recreated when the set of
    // devices changes; otherwise they are refreshed in place (keeps sliders usable).
    void RebuildDevices()
    {
        bool same = devRows_.size() == peers_.size();
        for (const auto& kv : peers_) same = same && devRows_.count(kv.first);
        if (same) { RefreshDeviceRows(); return; }

        devScroll_->Freeze();
        for (auto& kv : devRows_) kv.second.card->Destroy();
        devRows_.clear();
        for (const auto& kv : peers_) BuildDeviceRow(kv.first);
        emptyLbl_->Show(peers_.empty());
        devSizer_->Layout();
        devScroll_->FitInside();
        devScroll_->Thaw();
        devScroll_->Refresh();
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

    void UpdateAudioStats()
    {
        const auto stats = audio_->Stats();
        for (auto& kv : devRows_) {
            auto pit = peers_.find(kv.first);
            if (pit == peers_.end()) continue;
            DevRow& r = kv.second;

            bool recv = false;
            double kbps = 0;
            float level = 0;
            for (const auto& st : stats)
                if (st.instance == pit->second.inst && st.receiving) { recv = true; kbps = st.kbps; level = st.level; }

            r.level->SetLevel(recv ? std::sqrt(level) : 0.0f);
            wxString txt = recv ? wxString::Format("Receiving %.0f kbps", kbps) : wxString("No incoming audio");
            const bool sendSys = pit->second.share && sysCapturing_;
            const bool sendMic = pit->second.mic && micCapturing_;
            if (sendSys || sendMic)
                txt += wxString::FromUTF8("  \xC2\xB7  Sending my ") + (sendSys && sendMic ? "audio + microphone" : sendSys ? "audio" : "microphone");
            SetText(r.audioState, txt);
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
    std::map<int, PendInfo> pend_;
    std::map<std::string, CliRow> cliRows_;
    std::map<std::string, Scanned> scan_;
    std::map<int, PeerInfo> peers_;
    std::string selectedIp_;
    bool rebuilding_ = false;
    bool scanning_ = false;
    int tick_ = 0;
    int animStep_ = 0;

    Canvas* canvas_ = nullptr;
    TabBar* tabs_ = nullptr;
    wxSimplebook* pages_ = nullptr;
    SectionDivider* othDivider_ = nullptr;
    wxStaticText* othEmpty_ = nullptr;
    bool listDirty_ = false;
    std::map<std::string, OthRow> othRows_;
    wxTextCtrl* groupCtl_ = nullptr;
    wxButton* groupBtn_ = nullptr;
    wxStaticText* groupLbl_ = nullptr;
    wxScrolledWindow* cliScroll_ = nullptr;
    wxBoxSizer* cliSizer_ = nullptr;
    wxStaticText* cliEmpty_ = nullptr;
    wxStaticText* clientsCount_ = nullptr;
    std::filesystem::path groupFile_;
    std::string groupPwd_;
    std::unique_ptr<TrustStore> netTrust_;       // keys we trust for network pairing (network-trust.txt)
    wxScrolledWindow* devScroll_ = nullptr;
    wxBoxSizer* devSizer_ = nullptr;
    wxStaticText* emptyLbl_ = nullptr;
    std::map<int, DevRow> devRows_;
    Theme theme_ = MakeTheme();
    wxTextCtrl* target_ = nullptr;
    wxTextCtrl* log_ = nullptr;
    wxButton* goBtn_ = nullptr;
    wxButton* scanBtn_ = nullptr;
    wxStaticText* searchLbl_ = nullptr;
    SpeakerButton* masterMute_ = nullptr;
    wxStaticText* masterPct_ = nullptr;
    wxSlider* volSlider_ = nullptr;
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
    bool sysCapturing_ = false;   // system audio capture running
    bool micCapturing_ = false;   // microphone capture running
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
