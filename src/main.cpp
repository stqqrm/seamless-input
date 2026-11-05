#include <wx/wx.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/graphics.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/statline.h>
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

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC pdc(this);
        pdc.SetBackground(wxBrush(t_.page));
        pdc.Clear();
        wxGCDC dc(pdc);
        const wxSize s = GetClientSize();
        dc.SetPen(wxPen(t_.border, 1));
        dc.SetBrush(wxBrush(t_.card));
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
        notebook_ = new wxNotebook(this, wxID_ANY);
        notebook_->AddPage(BuildDevicesPage(notebook_), "Devices", true);
        notebook_->AddPage(BuildNetworkPage(notebook_), "Network");
        root->Add(notebook_, 3, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        SetSizer(root);

        // ---- events
        list_->Bind(wxEVT_LIST_ITEM_SELECTED, &MainFrame::OnListSelected, this);
        goBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Activate(target_->GetValue().ToStdString()); });
        target_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { Activate(target_->GetValue().ToStdString()); });
        scanBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StartScan(); });

        srcChoice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { if (sendingNow_) RefreshCapture(); MarkCfgDirty(); });
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
        bool share = false;         // send my audio to this device
        bool mute = false;          // mute what it sends me
        int  vol = 100;             // 0..100, what I hear from it
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
        log_->SetFont(wxFont(wxFontInfo(wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT).GetPointSize()).Family(wxFONTFAMILY_TELETYPE)));

        bottom->Add(left, 1, wxEXPAND | wxALL, 6);
        bottom->Add(log_, 1, wxEXPAND | wxTOP | wxRIGHT | wxBOTTOM, 6);
        p->SetSizer(bottom);
        return p;
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

        // ---- toolbar: audio source, master output, keyboard/mouse master switch
        auto* bar = new CardPanel(p, theme_, false);
        auto* bv = new wxBoxSizer(wxVERTICAL);

        auto* r1 = new wxBoxSizer(wxHORIZONTAL);
        srcChoice_ = new wxChoice(bar, wxID_ANY);
        srcChoice_->Append("Microphone");
        if (AudioEngine::SystemAudioSupported()) srcChoice_->Append("System audio (may echo)");
        srcChoice_->SetSelection(0);
        srcChoice_->SetToolTip("What is sent to the devices that have \"Share my audio\" ticked");
        inGauge_ = new LevelBar(bar, theme_, wxSize(120, 8));

        masterMute_ = new SpeakerButton(bar, theme_, 32);
        volSlider_ = new wxSlider(bar, wxID_ANY, 100, 0, 100, wxDefaultPosition, wxSize(150, -1));
        masterPct_ = MakeLabel(bar, "100%", true);
        masterPct_->SetMinSize(wxSize(44, -1));

        r1->Add(MakeLabel(bar, "Audio source", true), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        r1->Add(srcChoice_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
        r1->Add(MakeLabel(bar, "Sending", true), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        r1->Add(inGauge_, 0, wxALIGN_CENTER_VERTICAL);
        r1->AddStretchSpacer();
        r1->Add(MakeLabel(bar, "Master output", true), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        r1->Add(masterMute_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        r1->Add(volSlider_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        r1->Add(masterPct_, 0, wxALIGN_CENTER_VERTICAL);
        bv->Add(r1, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);

        auto* r2 = new wxBoxSizer(wxHORIZONTAL);
        inputChk_ = new wxCheckBox(bar, wxID_ANY, "Share keyboard && mouse");
        inputChk_->SetValue(true);
        inputChk_->SetToolTip("Master switch for moving the cursor onto other PCs");
        r2->Add(inputChk_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 14);
        r2->Add(MakeLabel(bar, "Scroll Lock returns control to this PC", true), 0, wxALIGN_CENTER_VERTICAL);
        bv->Add(r2, 0, wxEXPAND | wxALL, 14);
        bar->SetSizer(bv);
        ApplyBackground(bar, theme_.card);
        masterPct_->SetForegroundColour(theme_.sub);
        inputChk_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { input_->SetEnabled(inputChk_->GetValue()); MarkCfgDirty(); });

        v->Add(bar, 0, wxEXPAND | wxALL, 10);

        // ---- one card per connected device
        devScroll_ = new wxScrolledWindow(p, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxBORDER_NONE);
        devScroll_->SetBackgroundColour(theme_.page);
        devScroll_->SetScrollRate(0, 12);
        devSizer_ = new wxBoxSizer(wxVERTICAL);
        emptyLbl_ = new wxStaticText(devScroll_, wxID_ANY,
            "No devices connected yet.\nOther seamless-input PCs on your network connect automatically.",
            wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
        emptyLbl_->SetForegroundColour(theme_.sub);
        devSizer_->Add(emptyLbl_, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 28);
        devScroll_->SetSizer(devSizer_);
        v->Add(devScroll_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

        p->SetSizer(v);
        return p;
    }

    // ---- one device card: info | share audio + allow control | mute, volume, level
    struct DevRow {
        CardPanel* card = nullptr;
        wxStaticText *name = nullptr, *sub = nullptr, *detail = nullptr, *pct = nullptr, *audioState = nullptr;
        wxCheckBox *share = nullptr, *allow = nullptr;
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
        r.share->SetToolTip("Send this PC's audio (source chosen in the toolbar) to this device only");
        r.allow = new wxCheckBox(c, wxID_ANY, "Allow remote control");
        r.allow->SetToolTip("Let this device use this PC's keyboard and mouse");
        auto* checks = new wxBoxSizer(wxVERTICAL);
        checks->Add(r.share, 0, wxBOTTOM, 8);
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
            audio_->SetPeerSend(it->second.inst, it->second.share);
            Log((it->second.share ? "Sharing audio with " : "Stopped sharing audio with ") + it->second.name);
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
        r.allow->SetValue(ts == TrustStore::State::Allowed);
        r.mute->SetMuted(p.mute);
        r.vol->SetValue(p.vol);
        SetText(r.pct, wxString::Format("%d%%", p.vol));
    }

    void RefreshDeviceRows()
    {
        for (auto& kv : devRows_) RefreshDeviceRow(kv.second);
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
        cfg_.audio.source = srcChoice_->GetSelection() == 1 ? 1 : 0;
        cfg_.audio.mute   = masterMute_->IsMuted();
        cfg_.audio.volume = volSlider_->GetValue();
        cfg_.inputEnabled = inputChk_->GetValue();
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
        audio_->SetPeerSend(p.inst, p.share);
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

        const int src = (a.source == 1 && srcChoice_->GetCount() > 1) ? 1 : 0;
        srcChoice_->SetSelection(src);

        for (auto& kv : peers_) {
            PeerInfo& p = kv.second;
            const SavedPeer* sp = cfg_.FindPeer(p.key);
            if (sp && sp->hasAudio) { p.share = sp->send; p.mute = sp->mute; p.vol = sp->volume; }
            else                    { p.share = a.send;   p.mute = false;    p.vol = 100; }   // old file: one switch for all
            PushPeerAudio(p);
        }
        RefreshCapture();
        if (sendingNow_) Log("Audio sharing switched on by the saved config");
        RefreshDeviceRows();

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

    // Capture runs while at least one device has "Share my audio" ticked.
    void RefreshCapture()
    {
        bool any = false;
        for (const auto& kv : peers_) any = any || kv.second.share;
        const AudioSource src = srcChoice_->GetSelection() == 1 ? AudioSource::System : AudioSource::Microphone;
        const int srcIdx = src == AudioSource::System ? 1 : 0;
        if (any == sendingNow_ && (!any || srcIdx == sendingSrc_)) return;   // nothing to (re)start

        std::string err;
        if (!audio_->SetSending(any, src, &err)) {
            Log("Audio capture failed: " + wxString::FromUTF8(err));
            for (auto& kv : peers_) { kv.second.share = false; audio_->SetPeerSend(kv.second.inst, false); }
            sendingNow_ = false;
            inGauge_->SetLevel(0);
            RefreshDeviceRows();
            return;
        }
        sendingNow_ = any;
        sendingSrc_ = srcIdx;
        if (!any)                              Log("Stopped capturing audio");
        else if (src == AudioSource::System)   Log("Capturing system audio");
        else                                   Log("Capturing microphone");
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
        inGauge_->SetLevel(sendingNow_ ? std::min(1.0f, std::sqrt(audio_->InputLevel())) : 0.0f);

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
            if (pit->second.share && sendingNow_) txt += wxString::FromUTF8("  \xC2\xB7  Sending my audio");
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
    wxChoice* srcChoice_ = nullptr;
    LevelBar* inGauge_ = nullptr;
    wxSlider* volSlider_ = nullptr;
    wxCheckBox* inputChk_ = nullptr;
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
