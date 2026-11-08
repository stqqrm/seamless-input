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
#include <wx/popupwin.h>
#include <wx/stdpaths.h>
#include <wx/timer.h>

#include <algorithm>
#include <cctype>
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
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "audio.hpp"
#include "config.hpp"
#include "identity.hpp"
#include "input/input.hpp"
#include "ipc.hpp"
#include "localinfo.hpp"
#include "network.hpp"
#include "paths.hpp"
#include "service.hpp"
#include "state.hpp"

// ============================================================================
// Canvas: every PC is a group of monitor rectangles drawn at real pixel
// proportions (one shared scale). The central group is this PC; the other
// groups are virtual windows of connected PCs, draggable and edge-snapping.
// Positions are "world" pixels: the group's top-left corner relative to the
// top-left corner of this PC's desktop.
// ============================================================================
// Theme data. The palettes come from theme/*.theme files (see ThemeStore below). Widgets keep a
// REFERENCE to the frame's theme_, so assigning a new palette to it re-colours everything they draw.
// ============================================================================
struct Theme {
    wxString name;
    bool dark = false;
    wxColour page, card, border, track, text, sub, accent, ok, warn, bad;
};

// Maps a colour of the OLD theme to the same role in the NEW one; unknown colours stay as they are.
static wxColour RemapColour(const wxColour& c, const Theme& o, const Theme& n)
{
    if (!c.IsOk()) return c;
    if (c == o.page)   return n.page;
    if (c == o.card)   return n.card;
    if (c == o.border) return n.border;
    if (c == o.track)  return n.track;
    if (c == o.text)   return n.text;
    if (c == o.sub)    return n.sub;
    if (c == o.accent) return n.accent;
    if (c == o.ok)     return n.ok;
    if (c == o.warn)   return n.warn;
    if (c == o.bad)    return n.bad;
    return c;
}

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

    void SetTheme(const Theme* t) { th_ = t; Refresh(); }      // the frame's theme_ (lives as long as the frame)

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

    // The daemon owns positions: the canvas just shows them (no collision handling here).
    void AddPeerAt(int id, const wxString& name, const wxString& addr, const std::vector<wxRect>& mons,
                   double ox, double oy)
    {
        RemovePeer(id);
        Group g = MakeGroup(id, name, addr, mons);
        g.ox = ox;
        g.oy = oy;
        peers_.push_back(g);
        Refresh();
    }
    bool HasPeer(int id) const { for (const auto& g : peers_) if (g.id == id) return true; return false; }
    bool IsDragging(int id) const { return dragId_ == id; }
    std::vector<int> PeerIds() const { std::vector<int> v; for (const auto& g : peers_) v.push_back(g.id); return v; }

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
        wxColour fill = local ? wxColour(48, 108, 226) : wxColour(66, 54, 38);
        wxColour edge = local ? wxColour(132, 180, 255) : wxColour(255, 176, 72);
        wxColour text = local ? wxColour(246, 249, 255) : wxColour(255, 218, 160);
        if (th_) {
            // this PC: solid accent; other PCs: a pale tint of the warning colour, dashed outline
            auto mix = [](const wxColour& a, const wxColour& b, int pct) {
                return wxColour((unsigned char)(a.Red() + (b.Red() - a.Red()) * pct / 100),
                                (unsigned char)(a.Green() + (b.Green() - a.Green()) * pct / 100),
                                (unsigned char)(a.Blue() + (b.Blue() - a.Blue()) * pct / 100));
            };
            const wxColour white(255, 255, 255), black(0, 0, 0);
            if (local) {
                fill = th_->accent;
                edge = mix(th_->accent, th_->dark ? white : black, 35);
                const int lum = (fill.Red() * 299 + fill.Green() * 587 + fill.Blue() * 114) / 1000;
                text = lum > 150 ? wxColour(20, 22, 28) : wxColour(246, 249, 255);
            } else {
                fill = mix(th_->card, th_->warn, th_->dark ? 22 : 30);
                edge = th_->warn;
                text = th_->dark ? mix(th_->warn, white, 35) : mix(th_->warn, black, 45);
            }
        }

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
        const wxColour gridCol = th_ ? th_->border : wxColour(52, 56, 65);
        const wxColour hintCol = th_ ? th_->sub : wxColour(140, 146, 158);
        pdc.SetBackground(wxBrush(th_ ? (th_->dark ? wxColour((unsigned char)(th_->page.Red() * 80 / 100),
                                                               (unsigned char)(th_->page.Green() * 80 / 100),
                                                               (unsigned char)(th_->page.Blue() * 80 / 100))
                                                  : wxColour((unsigned char)(th_->page.Red() * 94 / 100),
                                                             (unsigned char)(th_->page.Green() * 94 / 100),
                                                             (unsigned char)(th_->page.Blue() * 94 / 100)))
                                  : wxColour(24, 26, 31)));
        pdc.Clear();
        wxGCDC dc(pdc);                    // anti-aliased drawing

        wxSize s = GetClientSize();
        dc.SetPen(wxPen(gridCol));
        for (int x = 16; x < s.x; x += 24)
            for (int y = 16; y < s.y; y += 24) dc.DrawPoint(x, y);   // dotted grid

        const View v = frozen_ ? frozenView_ : ComputeView();
        DrawGroup(dc, local_, v, true, false);
        for (const auto& g : peers_) DrawGroup(dc, g, v, false, g.id == dragId_);

        if (peers_.empty()) {
            int bottom = 0;
            for (const auto& m : local_.mons) bottom = std::max(bottom, MonRect(local_, m, v).GetBottom());
            dc.SetTextForeground(hintCol);
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
    const Theme* th_ = nullptr;
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
static Theme BuiltinTheme(bool dark)
{
    Theme t;
    t.dark = dark;
    t.name = dark ? "Dark" : "Light";
    // Explicit colours: on Windows the system text colour stays black even in dark mode.
    if (t.dark) {
        t.text   = wxColour(232, 234, 240);
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
        t.text   = wxColour(28, 31, 38);
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

static const char* const kSystemThemeName = "System (auto)";

static Theme SystemTheme()
{
    Theme t = BuiltinTheme(wxSystemSettings::GetAppearance().IsDark());
    t.name = kSystemThemeName;
    return t;
}

// ---------------------------------------------------------------- theme files (theme/*.theme)
static std::filesystem::path PathFromWx(const wxString& s)
{
#ifdef __WXMSW__
    return std::filesystem::path(std::wstring(s.wc_str()));
#else
    return std::filesystem::path(std::string(s.utf8_str()));
#endif
}

static wxString WxFromPath(const std::filesystem::path& p)
{
#ifdef __WXMSW__
    return wxString(p.wstring());
#else
    return wxString::FromUTF8(p.string().c_str());
#endif
}

static std::string LowerStr(std::string v)
{
    for (auto& c : v) c = (char)std::tolower((unsigned char)c);
    return v;
}

static std::string TrimStr(const std::string& v)
{
    const auto b = v.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return v.substr(b, v.find_last_not_of(" \t\r\n") - b + 1);
}

// The folder in the user's data directory (yours: survives updates, wins over the shipped files).
static std::filesystem::path UserThemeDir() { return paths::DataDir() / "theme"; }

// Folders searched for *.theme files; a later folder replaces a theme of the same name.
static std::vector<std::filesystem::path> ThemeDirs()
{
    std::vector<std::filesystem::path> d;
    const std::filesystem::path exe = PathFromWx(wxStandardPaths::Get().GetExecutablePath()).parent_path();
    d.push_back(exe / "theme");                                          // next to the program (build folder, portable copy)
    d.push_back(exe.parent_path() / "share" / "seamless-input" / "theme"); // installed: <prefix>/bin + <prefix>/share
    d.push_back(UserThemeDir());
    return d;
}

static bool ParseHexColour(const std::string& text, wxColour& out)
{
    std::string v = TrimStr(text);
    if (!v.empty() && v[0] == '#') v.erase(0, 1);
    if (v.size() != 6) return false;
    for (char c : v) if (!std::isxdigit((unsigned char)c)) return false;
    const unsigned long n = std::stoul(v, nullptr, 16);
    out = wxColour((unsigned char)((n >> 16) & 255), (unsigned char)((n >> 8) & 255), (unsigned char)(n & 255));
    return true;
}

// Live switching recognises a colour by its value, so two roles must never share one.
static void MakeDistinct(Theme& t)
{
    wxColour* c[] = {&t.page, &t.card, &t.border, &t.track, &t.text, &t.sub, &t.accent, &t.ok, &t.warn, &t.bad};
    for (size_t i = 1; i < 10; ++i) {
        for (int guard = 0; guard < 8; ++guard) {
            bool dup = false;
            for (size_t j = 0; j < i; ++j) {
                if (*c[i] == *c[j]) {
                    const int b = c[i]->Blue();
                    *c[i] = wxColour(c[i]->Red(), c[i]->Green(), (unsigned char)(b < 255 ? b + 1 : b - 1));
                    dup = true;
                }
            }
            if (!dup) break;
        }
    }
}

static bool LoadThemeFile(const std::filesystem::path& file, Theme& out, std::string& err)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) { err = "cannot read the file"; return false; }
    std::map<std::string, std::string> kv;
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);    // UTF-8 byte order mark
        first = false;
        const std::string t = TrimStr(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        kv[LowerStr(TrimStr(t.substr(0, eq)))] = TrimStr(t.substr(eq + 1));
    }
    std::string name = kv["name"];
    if (name.empty()) name = file.stem().string();

    wxColour pageProbe;
    bool dark;
    if (kv.count("dark")) {
        const std::string d = LowerStr(kv["dark"]);
        dark = d == "true" || d == "1" || d == "yes";
    } else if (kv.count("page") && ParseHexColour(kv["page"], pageProbe)) {
        dark = (pageProbe.Red() * 299 + pageProbe.Green() * 587 + pageProbe.Blue() * 114) / 1000 < 128;
    } else {
        dark = true;
    }

    Theme t = BuiltinTheme(dark);                       // keys that are missing keep the built-in colour
    struct Field { const char* key; wxColour* col; };
    const Field fields[] = {{"page", &t.page}, {"card", &t.card}, {"border", &t.border}, {"track", &t.track},
                            {"text", &t.text}, {"sub", &t.sub}, {"accent", &t.accent}, {"ok", &t.ok},
                            {"warn", &t.warn}, {"bad", &t.bad}};
    for (const auto& f : fields) {
        auto it = kv.find(f.key);
        if (it == kv.end()) continue;
        if (!ParseHexColour(it->second, *f.col)) { err = std::string("bad colour for '") + f.key + "' (use #RRGGBB)"; return false; }
    }
    MakeDistinct(t);
    t.name = wxString::FromUTF8(name.c_str());
    out = t;
    return true;
}

struct ThemeEntry {
    Theme theme;
    std::filesystem::path file;                         // empty: built in
};

// All themes: the built-in Light and Dark, then every theme file found (same name = later wins).
static std::vector<ThemeEntry> LoadThemes(std::vector<std::string>* problems = nullptr)
{
    std::map<std::string, ThemeEntry> byName;           // lower-case name: sorted menu, no duplicates
    for (bool dark : {false, true}) {
        ThemeEntry e;
        e.theme = BuiltinTheme(dark);
        byName[LowerStr(std::string(e.theme.name.utf8_str()))] = e;
    }
    for (const auto& dir : ThemeDirs()) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) continue;
        std::vector<std::filesystem::path> files;
        for (const auto& de : std::filesystem::directory_iterator(dir, ec)) {
            std::error_code ec2;
            if (!de.is_regular_file(ec2)) continue;
            if (LowerStr(de.path().extension().string()) != ".theme") continue;
            files.push_back(de.path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            ThemeEntry e;
            std::string err;
            if (!LoadThemeFile(f, e.theme, err)) {
                if (problems) problems->push_back(f.filename().string() + ": " + err);
                continue;
            }
            e.file = f;
            byName[LowerStr(std::string(e.theme.name.utf8_str()))] = e;
        }
    }
    std::vector<ThemeEntry> v;
    for (auto& kv : byName) v.push_back(std::move(kv.second));
    return v;
}

static std::filesystem::path ThemeChoiceFile() { return paths::DataDir() / "gui-theme.txt"; }

static std::string LoadThemeChoice()
{
    std::ifstream f(ThemeChoiceFile());
    std::string l;
    std::getline(f, l);
    return TrimStr(l);
}

static void SaveThemeChoice(const wxString& name)
{
    std::error_code ec;
    std::filesystem::create_directories(paths::DataDir(), ec);
    std::ofstream f(ThemeChoiceFile(), std::ios::trunc);
    if (f) f << std::string(name.utf8_str()) << "\n";
}

// The theme chosen last time (by name), else the system's light / dark setting.
static Theme InitialTheme()
{
    const std::string want = LoadThemeChoice();
    if (!want.empty() && LowerStr(want) != LowerStr(kSystemThemeName)) {
        for (const auto& e : LoadThemes())
            if (LowerStr(std::string(e.theme.name.utf8_str())) == LowerStr(want)) return e.theme;
    }
    return SystemTheme();
}


// Blend two colours (pct = share of b, 0..100).
static wxColour MixColour(const wxColour& a, const wxColour& b, int pct)
{
    auto m = [pct](int x, int y) { return (unsigned char)(x + (y - x) * pct / 100); };
    return wxColour(m(a.Red(), b.Red()), m(a.Green(), b.Green()), m(a.Blue(), b.Blue()));
}

// Text field drawn by us (rounded, themed border, accent ring on focus). The real text control
// sits inside it without any native border, so it looks the same on every OS.
class InputBox : public wxPanel {
public:
    wxTextCtrl* ctl = nullptr;

    InputBox(wxWindow* parent, const Theme& t, long style, const wxString& value, const wxString& hint)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxTAB_TRAVERSAL), t_(t)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        ctl = new wxTextCtrl(this, wxID_ANY, value, wxDefaultPosition, wxDefaultSize, style | wxBORDER_NONE);
        ctl->SetBackgroundColour(t.page);
        ctl->SetForegroundColour(t.text);
        if (!hint.empty()) ctl->SetHint(hint);
        auto* sz = new wxBoxSizer(wxVERTICAL);
        const bool multi = (style & wxTE_MULTILINE) != 0;
        sz->Add(ctl, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, multi ? 8 : 7);
        SetSizer(sz);
        Bind(wxEVT_PAINT, &InputBox::OnPaint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Refresh(); e.Skip(); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { ctl->SetFocus(); });
        ctl->Bind(wxEVT_SET_FOCUS,  [this](wxFocusEvent& e) { focus_ = true;  Refresh(); e.Skip(); });
        ctl->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) { focus_ = false; Refresh(); e.Skip(); });
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC pdc(this);
        pdc.SetBackground(wxBrush(GetBackgroundColour()));
        pdc.Clear();
        wxGCDC dc(pdc);
        const wxSize s = GetClientSize();
        dc.SetPen(wxPen(focus_ ? t_.accent : t_.border, 1));
        dc.SetBrush(wxBrush(t_.page));
        dc.DrawRoundedRectangle(0, 0, s.x, s.y, 8);
    }

    const Theme& t_;                      // the frame's theme_: assigning to it re-colours the widget
    bool focus_ = false;
};

// Rounded push button. Sends a normal wxEVT_BUTTON, so it is bound like a wxButton.
class FlatButton : public wxControl {
public:
    FlatButton(wxWindow* parent, const Theme& t, const wxString& label, bool primary = false)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
          t_(t), label_(label), primary_(primary)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        wxFont f = GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        SetFont(f);
        Bind(wxEVT_PAINT, &FlatButton::OnPaint, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { hover_ = true; Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hover_ = false; down_ = false; Refresh(); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { if (IsEnabled()) { down_ = true; Refresh(); } });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            const bool fire = down_ && hover_;
            down_ = false;
            Refresh();
            if (fire) Fire();
        });
        Bind(wxEVT_SET_FOCUS,  [this](wxFocusEvent& e) { focus_ = true;  Refresh(); e.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) { focus_ = false; Refresh(); e.Skip(); });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
            if (e.GetKeyCode() == WXK_SPACE || e.GetKeyCode() == WXK_RETURN) Fire();
            else e.Skip();
        });
    }

    bool AcceptsFocus() const override { return IsEnabled(); }
    bool Enable(bool on = true) override { const bool r = wxControl::Enable(on); Refresh(); return r; }

    void SetLabel(const wxString& l) override { label_ = l; InvalidateBestSize(); Refresh(); }

protected:
    wxSize DoGetBestSize() const override
    {
        const wxSize te = GetTextExtent(label_);
        return wxSize(te.x + 32, te.y + 16);
    }

private:
    void Fire()
    {
        if (!IsEnabled()) return;
        wxCommandEvent ev(wxEVT_BUTTON, GetId());
        ev.SetEventObject(this);
        ProcessWindowEvent(ev);
    }

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC pdc(this);
        pdc.SetBackground(wxBrush(GetBackgroundColour()));
        pdc.Clear();
        wxGCDC dc(pdc);
        const wxSize s = GetClientSize();
        wxColour fill = primary_ ? t_.accent : t_.track;
        wxColour ink  = primary_ ? wxColour(255, 255, 255) : t_.text;
        if (hover_) fill = MixColour(fill, primary_ ? wxColour(255, 255, 255) : t_.text, primary_ ? 12 : 8);
        if (down_)  fill = MixColour(fill, wxColour(0, 0, 0), 18);
        if (!IsEnabled()) { fill = MixColour(t_.card, t_.track, 60); ink = t_.sub; }
        dc.SetPen(wxPen(focus_ ? t_.accent : (primary_ ? fill : t_.border), 1));
        dc.SetBrush(wxBrush(fill));
        dc.DrawRoundedRectangle(0, 0, s.x, s.y, 8);
        dc.SetFont(GetFont());
        dc.SetTextForeground(ink);
        const wxSize te = dc.GetTextExtent(label_);
        dc.DrawText(label_, (s.x - te.x) / 2, (s.y - te.y) / 2);
    }

    const Theme& t_;                      // the frame's theme_: assigning to it re-colours the widget
    wxString label_;
    bool primary_;
    bool hover_ = false, down_ = false, focus_ = false;
};

// Check box with a label. Sends wxEVT_CHECKBOX (IsChecked() works as usual).
class FlatCheck : public wxControl {
public:
    FlatCheck(wxWindow* parent, const Theme& t, const wxString& label)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), t_(t), label_(label)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &FlatCheck::OnPaint, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { hover_ = true; Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hover_ = false; Refresh(); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { Toggle(); });
        Bind(wxEVT_SET_FOCUS,  [this](wxFocusEvent& e) { focus_ = true;  Refresh(); e.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) { focus_ = false; Refresh(); e.Skip(); });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
            if (e.GetKeyCode() == WXK_SPACE) Toggle(); else e.Skip();
        });
    }

    bool AcceptsFocus() const override { return IsEnabled(); }
    bool Enable(bool on = true) override { const bool r = wxControl::Enable(on); Refresh(); return r; }

    void SetValue(bool v) { if (v != on_) { on_ = v; Refresh(); } }
    bool GetValue() const { return on_; }

protected:
    wxSize DoGetBestSize() const override
    {
        const wxSize te = GetTextExtent(label_);
        return wxSize(kBox + 9 + te.x + 2, std::max(kBox, te.y) + 4);
    }

private:
    static constexpr int kBox = 18;

    void Toggle()
    {
        if (!IsEnabled()) return;
        on_ = !on_;
        Refresh();
        wxCommandEvent ev(wxEVT_CHECKBOX, GetId());
        ev.SetEventObject(this);
        ev.SetInt(on_ ? 1 : 0);
        ProcessWindowEvent(ev);
    }

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC pdc(this);
        pdc.SetBackground(wxBrush(GetBackgroundColour()));
        pdc.Clear();
        wxGCDC dc(pdc);
        const wxSize s = GetClientSize();
        const bool en = IsEnabled();
        const int by = (s.y - kBox) / 2;
        wxColour fill = on_ ? t_.accent : t_.page;
        wxColour edge = on_ ? t_.accent : (hover_ ? t_.sub : t_.border);
        if (!en) { fill = on_ ? MixColour(t_.card, t_.sub, 40) : t_.card; edge = t_.border; }
        dc.SetPen(wxPen(focus_ ? t_.accent : edge, focus_ ? 2 : 1));
        dc.SetBrush(wxBrush(fill));
        dc.DrawRoundedRectangle(1, by + 1, kBox - 2, kBox - 2, 5);
        if (on_) {
            dc.SetPen(wxPen(wxColour(255, 255, 255), 2, wxPENSTYLE_SOLID));
            const wxPoint pts[3] = { wxPoint(5, by + 9), wxPoint(8, by + 12), wxPoint(13, by + 6) };
            dc.DrawLines(3, pts);
        }
        dc.SetFont(GetFont());
        dc.SetTextForeground(en ? t_.text : t_.sub);
        const wxSize te = dc.GetTextExtent(label_);
        dc.DrawText(label_, kBox + 9, (s.y - te.y) / 2);
    }

    const Theme& t_;                      // the frame's theme_: assigning to it re-colours the widget
    wxString label_;
    bool on_ = false, hover_ = false, focus_ = false;
};

// Horizontal slider: thin track, accent fill, round thumb. Sends wxEVT_SLIDER (GetInt()).
class FlatSlider : public wxControl {
public:
    FlatSlider(wxWindow* parent, const Theme& t, int minV, int maxV, int value, int width)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxSize(width, 26), wxBORDER_NONE),
          t_(t), min_(minV), max_(maxV), val_(value), width_(width)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &FlatSlider::OnPaint, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { hover_ = true; Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hover_ = false; Refresh(); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
            if (!IsEnabled()) return;
            drag_ = true;
            if (!HasCapture()) CaptureMouse();
            SetFromX(e.GetX());
        });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& e) { if (drag_) SetFromX(e.GetX()); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            drag_ = false;
            if (HasCapture()) ReleaseMouse();
            Refresh();
        });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { drag_ = false; Refresh(); });
        Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& e) {
            if (!IsEnabled()) return;
            Change(val_ + (e.GetWheelRotation() > 0 ? 5 : -5));
        });
        Bind(wxEVT_SET_FOCUS,  [this](wxFocusEvent& e) { focus_ = true;  Refresh(); e.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) { focus_ = false; Refresh(); e.Skip(); });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
            switch (e.GetKeyCode()) {
                case WXK_LEFT:  case WXK_DOWN: Change(val_ - 1);  break;
                case WXK_RIGHT: case WXK_UP:   Change(val_ + 1);  break;
                case WXK_PAGEDOWN:             Change(val_ - 10); break;
                case WXK_PAGEUP:               Change(val_ + 10); break;
                default: e.Skip();
            }
        });
    }

    bool AcceptsFocus() const override { return IsEnabled(); }
    bool Enable(bool on = true) override { const bool r = wxControl::Enable(on); Refresh(); return r; }

    void SetValue(int v) { v = std::clamp(v, min_, max_); if (v != val_) { val_ = v; Refresh(); } }
    int GetValue() const { return val_; }

protected:
    wxSize DoGetBestSize() const override { return wxSize(width_, 26); }

private:
    static constexpr int kPad = 10;

    void SetFromX(int x)
    {
        const double w = std::max(1, GetClientSize().x - 2 * kPad);
        const double f = std::clamp((x - kPad) / w, 0.0, 1.0);
        Change(min_ + (int)std::lround(f * (max_ - min_)));
    }

    void Change(int v)
    {
        v = std::clamp(v, min_, max_);
        if (v == val_) return;
        val_ = v;
        Refresh();
        wxCommandEvent ev(wxEVT_SLIDER, GetId());
        ev.SetEventObject(this);
        ev.SetInt(val_);
        ProcessWindowEvent(ev);
    }

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC pdc(this);
        pdc.SetBackground(wxBrush(GetBackgroundColour()));
        pdc.Clear();
        wxGCDC dc(pdc);
        const wxSize s = GetClientSize();
        const bool en = IsEnabled();
        const double cy = s.y / 2.0, th = 6.0;
        const double span = std::max(1, s.x - 2 * kPad);
        const double frac = max_ > min_ ? double(val_ - min_) / (max_ - min_) : 0.0;
        const double tx = kPad + span * frac;
        const wxColour fillCol = en ? t_.accent : t_.sub;

        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(t_.track));
        dc.DrawRoundedRectangle(kPad, cy - th / 2, span, th, th / 2);
        if (tx > kPad) {
            dc.SetBrush(wxBrush(fillCol));
            dc.DrawRoundedRectangle(kPad, cy - th / 2, tx - kPad, th, th / 2);
        }
        const double r = (drag_ || hover_) ? 8.5 : 7.5;
        if (focus_) {
            dc.SetPen(wxPen(wxColour(fillCol.Red(), fillCol.Green(), fillCol.Blue(), 90), 3));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawCircle(wxPoint((int)tx, (int)cy), (int)(r + 3));
        }
        dc.SetPen(wxPen(fillCol, 2));
        dc.SetBrush(wxBrush(wxColour(255, 255, 255)));
        dc.DrawCircle(wxPoint((int)tx, (int)cy), (int)r);
    }

    const Theme& t_;                      // the frame's theme_: assigning to it re-colours the widget
    int min_, max_, val_, width_;
    bool hover_ = false, drag_ = false, focus_ = false;
};

// Children of a card must paint with the card's colour.
static void ApplyBackground(wxWindow* w, const wxColour& bg)
{
    w->SetBackgroundColour(bg);
    if (dynamic_cast<InputBox*>(w)) return;     // its text control keeps the field colour
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
    void RemapColours(const Theme& o, const Theme& n) { accent_ = RemapColour(accent_, o, n); Refresh(); }

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

    const Theme& t_;                      // the frame's theme_: assigning to it re-colours the widget
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

    const Theme& t_;                      // the frame's theme_: assigning to it re-colours the widget
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

    const Theme& t_;                      // the frame's theme_: assigning to it re-colours the widget
    bool on_ = false, hover_ = false;
};

// Rounded label with a tinted background ("Connected", "Waiting", ...).
class StatusPill : public wxControl {
public:
    void RemapColours(const Theme& o, const Theme& n) { col_ = RemapColour(col_, o, n); Refresh(); }
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

    const Theme& t_;                      // the frame's theme_: assigning to it re-colours the widget
    wxString text_;
    wxColour col_;
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

    const Theme& t_;                      // the frame's theme_: assigning to it re-colours the widget
    wxString text_;
};

// Round badge with the first letter of a name.
class Avatar : public wxControl {
public:
    void RemapColours(const Theme& o, const Theme& n) { col_ = RemapColour(col_, o, n); Refresh(); }
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

    const Theme& t_;                      // the frame's theme_: assigning to it re-colours the widget
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

    const Theme& t_;                      // the frame's theme_: assigning to it re-colours the widget
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

// Switching between a dark and a light theme while the window is open (Windows only; no-op elsewhere).
static void NativeLookTree(wxWindow* w, bool dark)
{
#ifdef __WXMSW__
    if (!w) return;
    if (dynamic_cast<wxChoice*>(w))        DarkenNative(w, dark ? L"DarkMode_CFD" : L"CFD");
    else if (auto* lc = dynamic_cast<wxListCtrl*>(w)) {
        DarkenNative(w, dark ? L"DarkMode_Explorer" : L"Explorer");
        if (HWND hdr = ListView_GetHeader((HWND)lc->GetHWND())) SetWindowTheme(hdr, dark ? L"DarkMode_ItemsView" : L"ItemsView", nullptr);
    }
    else if (dynamic_cast<wxButton*>(w) || dynamic_cast<wxTextCtrl*>(w) || dynamic_cast<wxScrolledWindow*>(w))
        DarkenNative(w, dark ? L"DarkMode_Explorer" : L"Explorer");
    for (wxWindow* c : w->GetChildren()) NativeLookTree(c, dark);
#else
    (void)w; (void)dark;
#endif
}

static void SetTitleBarDark(wxWindow* w, bool dark)
{
#ifdef __WXMSW__
    if (!w || !w->GetHWND()) return;
    BOOL on = dark ? TRUE : FALSE;
    DwmSetWindowAttribute((HWND)w->GetHWND(), 20, &on, sizeof on);
    DwmSetWindowAttribute((HWND)w->GetHWND(), 19, &on, sizeof on);
#else
    (void)w; (void)dark;
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

    const Theme& t_;                      // the frame's theme_: assigning to it re-colours the widget
    std::vector<wxString> labels_;
    int sel_ = 0, hover_ = -1;
};

// ============================================================================
// Custom menu bar. The native wxMenuBar is drawn by the OS (different look on Windows, GTK and
// macOS, and it stays light in dark mode), so the menu strip and its drop-down menus are drawn
// here with the same theme as the rest of the window.
// ============================================================================
struct MenuItemDef {
    int id = 0;
    wxString label;
    wxString accel;                 // shortcut text shown on the right (display only)
    bool separator = false;
    bool check = false, checked = false;
    bool enabled = true;
};

struct MenuDef {
    wxString title;
    std::vector<MenuItemDef> items;
};

// The drop-down list. A transient popup: it closes itself when you click anywhere else.
class MenuPopup : public wxPopupTransientWindow {
public:
    std::function<void(int)> onClosed;                      // chosen item index, or -1
    std::function<void(int)> onSwitch;                      // Left / Right arrow: -1 or +1

    MenuPopup(wxWindow* parent, const Theme& t, std::vector<MenuItemDef> items)
        : wxPopupTransientWindow(parent, wxBORDER_NONE), t_(t), items_(std::move(items))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetFont(parent->GetFont());

        pad_   = FromDIP(4);
        side_  = FromDIP(12);
        checkW_ = FromDIP(26);
        const int gap  = FromDIP(28);
        const int rowH = std::max(FromDIP(28), GetCharHeight() + FromDIP(12));
        const int sepH = FromDIP(9);

        int textW = 0, y = 1 + pad_;
        for (const auto& it : items_) {
            Row r;
            r.y = y;
            r.h = it.separator ? sepH : rowH;
            rows_.push_back(r);
            y += r.h;
            if (!it.separator) {
                int w = GetTextExtent(it.label).x;
                if (!it.accel.empty()) w += gap + GetTextExtent(it.accel).x;
                textW = std::max(textW, w);
            }
        }
        const int width = std::max(FromDIP(210), 2 + 2 * pad_ + checkW_ + textW + side_);
        const int height = y + pad_ + 1;
        SetClientSize(width, height);

        // A child panel covers the popup, draws the menu and takes the keyboard focus: a bare popup
        // window gets no key events on GTK.
        body_ = new wxPanel(this, wxID_ANY, wxPoint(0, 0), wxSize(width, height), wxBORDER_NONE | wxWANTS_CHARS);
        body_->SetBackgroundStyle(wxBG_STYLE_PAINT);
        body_->SetFont(GetFont());
        body_->Bind(wxEVT_PAINT, &MenuPopup::OnPaint, this);
        body_->Bind(wxEVT_MOTION, &MenuPopup::OnMotion, this);
        body_->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { SetHover(-1); });
        body_->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
            const int i = HitTest(e.GetPosition());
            if (i >= 0) Choose(i);
        });
        // keys reach the popup itself (wx forwards them from the focused child)
        Bind(wxEVT_KEY_DOWN, &MenuPopup::OnKey, this);
        body_->Bind(wxEVT_KEY_DOWN, &MenuPopup::OnKey, this);
    }

    wxWindow* FocusTarget() { return body_; }

    void CloseMenu() { DismissAndNotify(); }

    void OnDismiss() override
    {
        if (closed_) return;
        closed_ = true;
        if (onClosed) onClosed(chosen_);
    }

private:
    struct Row { int y = 0, h = 0; };

    bool Selectable(int i) const
    {
        return i >= 0 && i < (int)items_.size() && !items_[(size_t)i].separator && items_[(size_t)i].enabled;
    }

    int HitTest(const wxPoint& p) const
    {
        if (p.x < 0 || p.x >= body_->GetClientSize().x) return -1;
        for (size_t i = 0; i < rows_.size(); ++i)
            if (p.y >= rows_[i].y && p.y < rows_[i].y + rows_[i].h) return Selectable((int)i) ? (int)i : -1;
        return -1;
    }

    void SetHover(int i)
    {
        if (i == hover_) return;
        hover_ = i;
        body_->Refresh();
    }

    void Choose(int i)
    {
        if (!Selectable(i)) return;
        chosen_ = i;
        DismissAndNotify();
    }

    void Step(int dir)
    {
        const int n = (int)items_.size();
        if (n == 0) return;
        int i = hover_;
        for (int k = 0; k < n; ++k) {
            i = i < 0 ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
            if (Selectable(i)) { SetHover(i); return; }
        }
    }

    void OnMotion(wxMouseEvent& e)
    {
        SetHover(HitTest(e.GetPosition()));
    }

    void OnKey(wxKeyEvent& e)
    {
        switch (e.GetKeyCode()) {
            case WXK_ESCAPE: DismissAndNotify(); break;
            case WXK_DOWN:   Step(+1); break;
            case WXK_UP:     Step(-1); break;
            case WXK_LEFT:   if (onSwitch) onSwitch(-1); break;
            case WXK_RIGHT:  if (onSwitch) onSwitch(+1); break;
            case WXK_RETURN:
            case WXK_NUMPAD_ENTER:
            case WXK_SPACE:  Choose(hover_); break;
            default:         e.Skip(); break;
        }
    }

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC pdc(body_);
        pdc.SetBackground(wxBrush(t_.card));
        pdc.Clear();
        wxGCDC dc(pdc);
        const wxSize s = body_->GetClientSize();
        dc.SetPen(wxPen(t_.border, 1));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(0, 0, s.x, s.y);
        dc.SetFont(GetFont());

        for (size_t i = 0; i < items_.size(); ++i) {
            const MenuItemDef& it = items_[i];
            const Row& r = rows_[i];
            if (it.separator) {
                dc.SetPen(wxPen(t_.border, 1));
                dc.DrawLine(pad_ + side_, r.y + r.h / 2, s.x - pad_ - side_, r.y + r.h / 2);
                continue;
            }
            if ((int)i == hover_) {
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(t_.track));
                dc.DrawRoundedRectangle(1 + pad_, r.y, s.x - 2 * (1 + pad_), r.h, 5);
            }
            const wxColour ink = it.enabled ? t_.text : t_.sub;
            const wxSize te = dc.GetTextExtent(it.label);
            const int ty = r.y + (r.h - te.y) / 2;

            if (it.check && it.checked) {
                const int cx = 1 + pad_ + FromDIP(10), cy = r.y + r.h / 2;
                const wxPoint pts[3] = { wxPoint(cx, cy), wxPoint(cx + FromDIP(4), cy + FromDIP(4)),
                                         wxPoint(cx + FromDIP(11), cy - FromDIP(4)) };
                dc.SetPen(wxPen(it.enabled ? t_.accent : t_.sub, 2));
                dc.DrawLines(3, pts);
            }

            dc.SetTextForeground(ink);
            dc.DrawText(it.label, 1 + pad_ + checkW_, ty);
            if (!it.accel.empty()) {
                dc.SetTextForeground(t_.sub);
                const wxSize ae = dc.GetTextExtent(it.accel);
                dc.DrawText(it.accel, s.x - 1 - pad_ - side_ - ae.x, r.y + (r.h - ae.y) / 2);
            }
        }
    }

    const Theme& t_;                      // the frame's theme_: assigning to it re-colours the widget
    std::vector<MenuItemDef> items_;
    std::vector<Row> rows_;
    wxPanel* body_ = nullptr;
    int pad_ = 4, side_ = 12, checkW_ = 26;
    int hover_ = -1, chosen_ = -1;
    bool closed_ = false;
};

// The strip with the menu titles (File, Service, ...). Fires onCommand(id, checked) for a chosen item.
class MenuStrip : public wxControl {
public:
    std::function<void(int)> onOpen;                    // a menu is about to open: refresh its items
    std::function<void(int, bool)> onCommand;           // item id, new checked state (check items)

    MenuStrip(wxWindow* parent, const Theme& t)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 34), wxBORDER_NONE), t_(t)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(-1, 34));
        Bind(wxEVT_PAINT, &MenuStrip::OnPaint, this);
        Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
            const int h = HitTest(e.GetPosition());
            if (h != hover_) { hover_ = h; SetCursor(h >= 0 ? wxCursor(wxCURSOR_HAND) : wxNullCursor); Refresh(); }
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hover_ = -1; Refresh(); });
        // While a menu is open the popup holds the mouse grab, so the strip gets no mouse events.
        // Poll the pointer instead: hovering another title slides over to that menu.
        pollTimer_.SetOwner(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            if (open_ < 0) { pollTimer_.Stop(); return; }
            const wxPoint pos = wxGetMousePosition();
            if (pos == lastMouse_) return;      // only a real pointer move counts (keyboard switching must stick)
            lastMouse_ = pos;
            const int h = HitTest(ScreenToClient(pos));
            if (h >= 0 && h != open_) OpenMenu(h);
        });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
            const int h = HitTest(e.GetPosition());
            if (h < 0) return;
            // the click that just closed this menu (click outside the popup) must not reopen it
            const bool justClosed = h == closedIdx_ &&
                std::chrono::steady_clock::now() - closedAt_ < std::chrono::milliseconds(250);
            closedIdx_ = -1;
            if (open_ == h) { DetachPopup(); return; }
            if (justClosed) return;
            OpenMenu(h);
        });
    }

    ~MenuStrip() override { pollTimer_.Stop(); if (popup_) popup_->onClosed = nullptr; }

    int AddMenu(const wxString& title, std::vector<MenuItemDef> items)
    {
        menus_.push_back({ title, std::move(items) });
        Refresh();
        return (int)menus_.size() - 1;
    }

    void SetMenuItems(int idx, std::vector<MenuItemDef> items)
    {
        if (idx < 0 || idx >= (int)menus_.size()) return;
        menus_[(size_t)idx].items = std::move(items);
        Refresh();
    }

    MenuItemDef* Item(int id)
    {
        for (auto& m : menus_)
            for (auto& it : m.items)
                if (!it.separator && it.id == id) return &it;
        return nullptr;
    }

    void OpenMenu(int idx)
    {
        if (idx < 0 || idx >= (int)menus_.size()) return;
        DetachPopup();
        if (onOpen) onOpen(idx);

        auto* p = new MenuPopup(this, t_, menus_[(size_t)idx].items);
        popup_ = p;
        open_ = idx;
        p->onClosed = [this, p, idx](int chosen) { PopupClosed(p, idx, chosen); };
        p->onSwitch = [this](int dir) {
            const int n = (int)menus_.size();
            if (n > 0 && open_ >= 0) { const int next = (open_ + dir + n) % n; CallAfter([this, next] { OpenMenu(next); }); }
        };
        p->Position(ClientToScreen(wxPoint(TitleX(idx), GetClientSize().y - 1)), wxSize(0, 0));
        p->Popup(p->FocusTarget());
        lastMouse_ = wxGetMousePosition();
        pollTimer_.Start(60);
        Refresh();
    }

private:
    static constexpr int kPad = 14;

    int TitleW(size_t i) const { return GetTextExtent(menus_[i].title).x + 2 * FromDIP(kPad); }

    int TitleX(int idx) const
    {
        int x = FromDIP(6);
        for (int i = 0; i < idx; ++i) x += TitleW((size_t)i);
        return x;
    }

    int HitTest(const wxPoint& p) const
    {
        if (p.y < 0 || p.y >= GetClientSize().y) return -1;
        int x = FromDIP(6);
        for (size_t i = 0; i < menus_.size(); ++i) {
            const int w = TitleW(i);
            if (p.x >= x && p.x < x + w) return (int)i;
            x += w;
        }
        return -1;
    }

    // Close the open menu without choosing anything (used when switching or toggling).
    void DetachPopup()
    {
        if (!popup_) return;
        MenuPopup* p = popup_;
        popup_ = nullptr;
        open_ = -1;
        p->onClosed = nullptr;
        p->CloseMenu();
        CallAfter([p] { p->Destroy(); });
        Refresh();
    }

    void PopupClosed(MenuPopup* p, int idx, int chosen)
    {
        if (p == popup_) popup_ = nullptr;
        open_ = -1;
        closedIdx_ = idx;
        closedAt_ = std::chrono::steady_clock::now();
        Refresh();
        CallAfter([p] { p->Destroy(); });

        if (chosen < 0 || idx < 0 || idx >= (int)menus_.size()) return;
        auto& items = menus_[(size_t)idx].items;
        if (chosen >= (int)items.size()) return;
        MenuItemDef& it = items[(size_t)chosen];
        if (it.check) it.checked = !it.checked;
        const int id = it.id;
        const bool checked = it.checked;
        // run the command after the popup is fully gone (it may close this window)
        CallAfter([this, id, checked] { if (onCommand) onCommand(id, checked); });
    }

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC pdc(this);
        pdc.SetBackground(wxBrush(t_.page));
        pdc.Clear();
        wxGCDC dc(pdc);
        dc.SetFont(GetFont());
        const wxSize s = GetClientSize();

        dc.SetPen(wxPen(t_.border, 1));
        dc.DrawLine(0, s.y - 1, s.x, s.y - 1);

        int x = FromDIP(6);
        for (size_t i = 0; i < menus_.size(); ++i) {
            const int w = TitleW(i);
            const bool active = (int)i == open_;
            const bool hot = (int)i == hover_;
            if (active || hot) {
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(active ? t_.track : MixColour(t_.page, t_.track, 70)));
                dc.DrawRoundedRectangle(x + 1, 4, w - 2, s.y - 9, 6);
            }
            dc.SetTextForeground(active || hot ? t_.text : t_.sub);
            const wxSize te = dc.GetTextExtent(menus_[i].title);
            dc.DrawText(menus_[i].title, x + (w - te.x) / 2, (s.y - 1 - te.y) / 2);
            x += w;
        }
    }

    const Theme& t_;                      // the frame's theme_: assigning to it re-colours the widget
    std::vector<MenuDef> menus_;
    MenuPopup* popup_ = nullptr;
    int open_ = -1, hover_ = -1, closedIdx_ = -1;
    std::chrono::steady_clock::time_point closedAt_{};
    wxTimer pollTimer_;
    wxPoint lastMouse_;
};

// ============================================================================
// Main window
// ============================================================================
class MainFrame : public wxFrame {
public:
    MainFrame()
        : wxFrame(nullptr, wxID_ANY, "seamless-input", wxDefaultPosition, wxSize(1100, 860)),
          host_(wxString::FromUTF8(paths::HostName())),
          alive_(std::make_shared<bool>(true))
    {
        SetTitle("seamless-input - " + host_);
        local_ = GetLocalAddresses();
        localMons_ = GetLocalMonitors();      // until the daemon reports the monitors it uses itself
        dataDir_ = paths::DataDir();

        // The window holds no engine: the daemon (seamless-input-daemon) does the network, keyboard,
        // mouse and audio work and keeps running when this window is closed. We mirror its state.
        ipc::Client::Handlers h;
        auto alive = alive_;
        auto post = [alive](std::function<void(MainFrame*)> fn, MainFrame* self) {
            wxTheApp->CallAfter([alive, fn, self] { if (*alive) fn(self); });
        };
        h.onSnapshot = [post, this](const state::Snapshot& s) {
            post([s](MainFrame* f) { f->ApplySnapshot(s); }, this);
        };
        h.onLog = [post, this](const std::string& l) {
            post([l](MainFrame* f) { f->LogRaw(wxString::FromUTF8(l)); }, this);
        };
        h.onStats = [post, this](const state::Stats& st) {
            post([st](MainFrame* f) { f->stats_ = st; f->UpdateAudioStats(); }, this);
        };
        h.onDisconnected = [post, this]() { post([](MainFrame* f) { f->OnDisconnected(); }, this); };
        client_ = std::make_shared<ipc::Client>(std::move(h));

        SetBackgroundColour(theme_.page);
        SetForegroundColour(theme_.text);
        auto* root = new wxBoxSizer(wxVERTICAL);

        // ---- menu bar (custom drawn, filled in by BuildMenu)
        menuBar_ = new MenuStrip(this, theme_);
        root->Add(menuBar_, 0, wxEXPAND);

        // ---- header: host name, addresses, key
        {
            auto* title = new wxStaticText(this, wxID_ANY, host_);
            wxFont tf = title->GetFont();
            tf.SetWeight(wxFONTWEIGHT_BOLD);
            tf.SetPointSize(tf.GetPointSize() + 5);
            title->SetFont(tf);
            title->SetForegroundColour(theme_.text);
            hdrSub_ = new wxStaticText(this, wxID_ANY, "");
            hdrSub_->SetForegroundColour(theme_.sub);
            auto* hs = new wxBoxSizer(wxVERTICAL);
            hs->Add(title, 0, wxBOTTOM, 2);
            hs->Add(hdrSub_, 0);
            root->Add(hs, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);
        }

        // ---- monitor layout
        canvas_ = new Canvas(this);
        canvas_->SetTheme(&theme_);
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

        BuildMenu();
        UpdateHeader();

        // ---- events
        goBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Activate(target_->GetValue().ToStdString()); });
        target_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { Activate(target_->GetValue().ToStdString()); });
        scanBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StartScan(); });
        groupBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ApplyGroupPassword(); });
        groupCtl_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { ApplyGroupPassword(); });
        clipBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ApplyClipLimit(); });
        clipLimitCtl_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { ApplyClipLimit(); });
        clipUnit_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { ApplyClipLimit(); });
        ShowClipLimit(clipLimit_);

        masterMute_->onToggle = [this](bool) { SendMaster(); };
        volSlider_->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
            masterPct_->SetLabelText(wxString::Format("%d%%", volSlider_->GetValue()));
            SendMaster();
        });

        Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);

        timer_.SetOwner(this, kTimerId);
        Bind(wxEVT_TIMER, &MainFrame::OnTimer, this, kTimerId);
        timer_.Start(1000);

        animTimer_.SetOwner(this, kAnimTimerId);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { ++animStep_; UpdateSearchLabel(); }, kAnimTimerId);

        canvas_->onPeerMoved = [this](int id, double x, double y) {
            lastMove_[id] = std::chrono::steady_clock::now();     // ignore stale echoes of our own drag
            client_->Send("MOVE " + std::to_string(id) + " " + std::to_string(std::llround(x)) + " " + std::to_string(std::llround(y)));
        };

        RebuildList();
        UpdateGroupLabel();
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

        // Starting the window starts the background service if it is not running yet.
        LogRaw(wxDateTime::Now().FormatTime() + "  Connecting to the background service...");
        StartConnect();
    }

    ~MainFrame() override { *alive_ = false; }

private:
    static constexpr int kTimerId     = 9000;
    static constexpr int kAnimTimerId = 9001;
    enum { kIdAutostart = 9100, kIdSvcInfo, kIdToggleSvc, kIdQuitAll, kIdOpenFileMenu, kIdOpenSettingsMenu,
           kIdClearCache, kIdRegenKeys, kIdViewKeys,
           kIdThemeSystem = 9400, kIdThemeReload, kIdThemeFolder, kIdThemeBase = 9500 };   // kIdThemeBase + n: the n-th theme

    struct Entry { wxString name; std::string ip; wxString status; };
    struct Discovered {
        std::string host;
        bool server;
        std::chrono::steady_clock::time_point seen;
        std::string key;            // announced public key (proven only by the handshake)
        bool sameGroup = false;
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
        double ox = 0, oy = 0;
        bool share = false;         // send my audio (system audio) to this device
        bool mic = false;           // send my microphone to this device
        bool mute = false;          // mute what it sends me
        int  vol = 100;             // 0..100, what I hear from it
        bool allowed = false;       // may it control this PC?
        bool clip = false;          // send my clipboard to this device when the cursor moves over to it
    };

    // ------------------------------------------------------------ daemon connection
    void StartConnect()
    {
        if (connecting_) return;
        connecting_ = true;
        auto client = client_;
        auto alive = alive_;
        const auto dir = dataDir_;
        std::thread([this, client, alive, dir] {
            std::string err;
            const bool ok = svc::EnsureDaemon(dir, &err) && client->Connect(dir, false, &err);
            wxTheApp->CallAfter([this, alive, ok, err] { if (*alive) FinishConnect(ok, err); });
        }).detach();
    }

    void FinishConnect(bool ok, const std::string& err)
    {
        connecting_ = false;
        if (!ok) {
            if (err != lastConnErr_) Log("Cannot reach the background service: " + wxString::FromUTF8(err));
            lastConnErr_ = err;
            UpdateHeader();
            // A few failures in a row: whatever sits on the daemon's port is old or stuck - offer to restart it.
            if (daemonWanted_ && ++connFails_ >= 3 && !restartAsked_) AskRestartDaemon();
            return;
        }
        connFails_ = 0;
        restartAsked_ = false;
        lastConnErr_.clear();
        log_->Clear();                                  // the daemon replays its log history on attach
        connected_ = true;
        client_->Send("ATTACH");
        SendMonitorsHint();
        client_->Send("SCAN");
        UpdateHeader();
    }

    void AskRestartDaemon()
    {
        restartAsked_ = true;                       // ask once; asks again only after a restart attempt failed
        busy_ = true;                               // pauses the automatic retries while the question is open
        const int r = wxMessageBox(
            "The background service does not respond.\n\n"
            "It may be an older version that is still running, or a stuck process. Restart it?",
            "Background service", wxYES_NO | wxYES_DEFAULT | wxICON_QUESTION, this);
        if (r != wxYES) { busy_ = false; return; }

        Log("Restarting the background service ...");
        auto alive = alive_;
        const auto dir = dataDir_;
        std::thread([this, alive, dir] {
            std::string err;
            ipc::RequestStop(dir);                  // politely first: works when it still answers
            svc::WaitGone(dir, 1500);
            svc::KillDaemons();                     // then force: an old or hung daemon never answers
            svc::WaitGone(dir, 2000);
            const bool ok = svc::EnsureDaemon(dir, &err);
            wxTheApp->CallAfter([this, alive, ok, err] { if (*alive) RestartDaemonDone(ok, err); });
        }).detach();
    }

    void RestartDaemonDone(bool ok, const std::string& err)
    {
        busy_ = false;
        connFails_ = 0;
        retry_ = 3;                                 // reconnect right away
        if (ok) { Log("Background service restarted"); return; }
        restartAsked_ = false;                      // may ask again after the next failures
        Log("Could not restart the background service: " + wxString::FromUTF8(err));
    }

    void OnDisconnected()
    {
        connected_ = false;
        Log(daemonWanted_ ? "Lost the connection to the background service - reconnecting"
                          : "Background service stopped");
        retry_ = 0;
        UpdateHeader();
    }

    void SendMonitorsHint()
    {
        std::string l = "MONITORS " + std::to_string(GetLocalMonitors().size());
        for (const auto& m : GetLocalMonitors())
            l += " " + std::to_string(m.x) + " " + std::to_string(m.y) + " " + std::to_string(m.width) + " " + std::to_string(m.height);
        client_->Send(l);
    }

    void UpdateHeader()
    {
        wxString ips;
        for (const auto& a : local_) ips += (ips.empty() ? "" : ", ") + wxString::FromUTF8(a.ip);
        wxString svcState = connected_ ? "connected"
                          : !daemonWanted_ ? "stopped"
                          : connecting_ ? "starting..." : "not connected";
        hdrSub_->SetLabelText(wxString::Format(
            "IP %s   |   Key %s   |   TCP %d   UDP %d (discovery)   UDP %d (audio)   |   Background service: %s",
            ips.empty() ? wxString("-") : ips,
            snap_.pubKey.empty() ? wxString("-") : wxString::FromUTF8(Identity::Fingerprint(snap_.pubKey)),
            (int)kTcpPort, (int)kDiscoveryPort, (int)kAudioPort, svcState));
    }

    // ------------------------------------------------------------ service menu
    // The menus are drawn by MenuStrip (not a native wxMenuBar) so they look the same on every OS.
    void BuildMenu()
    {
#if defined(__WXOSX__)
        const wxString closeKey = "Cmd+W";
#else
        const wxString closeKey = "Ctrl+W";
#endif
        auto item = [](int id, const wxString& label) { MenuItemDef d; d.id = id; d.label = label; return d; };
        auto sep = [] { MenuItemDef d; d.separator = true; return d; };

        MenuItemDef close = item(wxID_CLOSE, "Close window");
        close.accel = closeKey;
        menuBar_->AddMenu("File", { close, item(kIdQuitAll, "Quit and stop the background service") });

#if defined(__WXMSW__)
        MenuItemDef autostart = item(kIdAutostart, "Start the background service with Windows");
#else
        MenuItemDef autostart = item(kIdAutostart, "Start the background daemon at login");
#endif
        autostart.check = true;
        MenuItemDef info = item(kIdSvcInfo, "Background service: ...");
        info.enabled = false;
        menuBar_->AddMenu("Service", { autostart, sep(), info, item(kIdToggleSvc, "Stop background service now") });

        menuBar_->AddMenu("Security", { item(kIdClearCache, "Clear cache"),
                                        item(kIdRegenKeys, "Regenerate keys"),
                                        item(kIdViewKeys, "View keys") });

        {
            std::vector<std::string> problems;
            themes_ = LoadThemes(&problems);
            for (const auto& p : problems) Log("Theme skipped: " + wxString::FromUTF8(p.c_str()));
            themeName_ = theme_.name;
            themeMenuIdx_ = menuBar_->AddMenu("Theme", ThemeMenuItems());
        }

        menuBar_->onOpen = [this](int) { RefreshServiceMenu(); RefreshThemeMenu(); };
        menuBar_->onCommand = [this](int id, bool checked) {
            wxCommandEvent ev(wxEVT_MENU, id);
            ev.SetEventObject(menuBar_);
            ev.SetInt(checked ? 1 : 0);
            ProcessWindowEvent(ev);
        };

        Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_CLOSE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { QuitAll(); }, kIdQuitAll);
        Bind(wxEVT_MENU, &MainFrame::OnAutostart, this, kIdAutostart);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { ToggleService(); }, kIdToggleSvc);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { OnClearCache(); }, kIdClearCache);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { OnRegenKeys(); }, kIdRegenKeys);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { OnViewKeys(); }, kIdViewKeys);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { ApplyTheme(SystemTheme(), true); }, kIdThemeSystem);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { ReloadThemes(); }, kIdThemeReload);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { OpenThemeFolder(); }, kIdThemeFolder);
        Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
            const size_t i = (size_t)(e.GetId() - kIdThemeBase);
            if (i < themes_.size()) ApplyTheme(themes_[i].theme, true);
        }, kIdThemeBase, kIdThemeBase + 299);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { menuBar_->OpenMenu(0); }, kIdOpenFileMenu);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { menuBar_->OpenMenu(1); }, kIdOpenSettingsMenu);

        // keyboard shortcuts (a native menu bar used to provide these)
        std::vector<wxAcceleratorEntry> keys;
        keys.emplace_back(wxACCEL_CTRL, (int)'W', wxID_CLOSE);
#if !defined(__WXOSX__)
        keys.emplace_back(wxACCEL_ALT, (int)'F', kIdOpenFileMenu);
        keys.emplace_back(wxACCEL_ALT, (int)'S', kIdOpenSettingsMenu);
#endif
        SetAcceleratorTable(wxAcceleratorTable((int)keys.size(), keys.data()));
    }

    // ------------------------------------------------------------ themes (theme/*.theme)
    std::vector<MenuItemDef> ThemeMenuItems() const
    {
        std::vector<MenuItemDef> v;
        MenuItemDef sp;
        sp.separator = true;

        MenuItemDef sys;
        sys.id = kIdThemeSystem;
        sys.label = kSystemThemeName;
        sys.accel = "light / dark";
        sys.check = true;
        sys.checked = themeName_ == kSystemThemeName;
        v.push_back(sys);
        v.push_back(sp);
        for (size_t i = 0; i < themes_.size() && i < 299; ++i) {
            MenuItemDef d;
            d.id = kIdThemeBase + (int)i;
            d.label = themes_[i].theme.name;
            d.accel = themes_[i].theme.dark ? "dark" : "light";
            d.check = true;
            d.checked = themes_[i].theme.name == themeName_;
            v.push_back(d);
        }
        v.push_back(sp);
        MenuItemDef rl;
        rl.id = kIdThemeReload;
        rl.label = "Reload themes";
        v.push_back(rl);
        MenuItemDef of;
        of.id = kIdThemeFolder;
        of.label = "Open theme folder";
        v.push_back(of);
        return v;
    }

    void RefreshThemeMenu()
    {
        if (!menuBar_) return;
        if (MenuItemDef* m = menuBar_->Item(kIdThemeSystem)) m->checked = themeName_ == kSystemThemeName;
        for (size_t i = 0; i < themes_.size() && i < 299; ++i)
            if (MenuItemDef* m = menuBar_->Item(kIdThemeBase + (int)i)) m->checked = themes_[i].theme.name == themeName_;
    }

    void ReloadThemes()
    {
        std::vector<std::string> problems;
        themes_ = LoadThemes(&problems);
        for (const auto& p : problems) Log("Theme skipped: " + wxString::FromUTF8(p.c_str()));
        if (menuBar_) menuBar_->SetMenuItems(themeMenuIdx_, ThemeMenuItems());
        Log(wxString::Format("%d themes loaded", (int)themes_.size()));
        if (themeName_ != kSystemThemeName)              // the chosen theme may just have been edited
            for (const auto& e : themes_)
                if (e.theme.name == themeName_) { ApplyTheme(e.theme, false); break; }
    }

    void OpenThemeFolder()
    {
        std::error_code ec;
        std::filesystem::create_directories(UserThemeDir(), ec);
        const wxString dir = WxFromPath(UserThemeDir());
        if (!wxLaunchDefaultApplication(dir))
            wxMessageBox("Could not open the folder:\n" + dir, "Themes", wxOK | wxICON_WARNING, this);
    }

    // Colours an existing widget tree: every colour that belongs to the old palette becomes the same role of the new one.
    static void ReThemeTree(wxWindow* w, const Theme& o, const Theme& n)
    {
        const wxColour bg = w->GetBackgroundColour(), nbg = RemapColour(bg, o, n);
        if (nbg != bg) w->SetBackgroundColour(nbg);
        const wxColour fg = w->GetForegroundColour(), nfg = RemapColour(fg, o, n);
        if (nfg != fg) w->SetForegroundColour(nfg);
        if (auto* c = dynamic_cast<CardPanel*>(w))  c->RemapColours(o, n);
        if (auto* p = dynamic_cast<StatusPill*>(w)) p->RemapColours(o, n);
        if (auto* a = dynamic_cast<Avatar*>(w))     a->RemapColours(o, n);
        for (wxWindow* c : w->GetChildren()) ReThemeTree(c, o, n);
        w->Refresh();
    }

    void ApplyTheme(const Theme& nt, bool save)
    {
        const Theme old = theme_;
        Freeze();
        theme_ = nt;                                     // widgets hold a reference to this: custom-drawn ones follow at once
        themeName_ = nt.name;
        ReThemeTree(this, old, nt);
        for (auto& kv : cliRows_) kv.second.card->SetFill(ClientTint());     // derived from the palette: recompute
        if (canvas_) canvas_->Refresh();
        SetTitleBarDark(this, nt.dark);
        NativeLookTree(this, nt.dark);
        Thaw();
        Layout();
        Refresh();
        if (save) SaveThemeChoice(nt.name);
    }

    void RefreshServiceMenu()
    {
        if (!menuBar_) return;
        const svc::AutostartInfo a = svc::QueryAutostart();
        if (MenuItemDef* m = menuBar_->Item(kIdAutostart)) {
            m->checked = a.enabled;
            m->enabled = a.supported && !busy_;
        }
        wxString st = connected_ ? "running" : daemonWanted_ ? "not reachable" : "stopped";
        if (MenuItemDef* m = menuBar_->Item(kIdSvcInfo))
            m->label = "Background service: " + st + "  (" + wxString::FromUTF8(a.kind) +
                       (a.enabled ? ", autostart on)" : ", autostart off)");
        if (MenuItemDef* m = menuBar_->Item(kIdToggleSvc))
            m->label = daemonWanted_ ? "Stop background service now" : "Start background service now";
    }

    void OnAutostart(wxCommandEvent& e)
    {
        if (busy_) return;
        busy_ = true;
        const bool on = e.IsChecked();
        daemonWanted_ = true;
        Log(on ? "Enabling autostart of the background service..." : "Disabling autostart of the background service...");
        auto alive = alive_;
        const auto dir = dataDir_;
        std::thread([this, alive, dir, on] {
            std::string err, err2;
            bool ok = on ? svc::EnableAutostart(&err) : svc::DisableAutostart(&err);
            if (ok) {
                // hand the daemon over to its new owner, or keep one running for this window
                if (ipc::Ping(dir)) { ipc::RequestStop(dir); svc::WaitGone(dir, 5000); }
                svc::EnsureDaemon(dir, &err2);
            }
            wxTheApp->CallAfter([this, alive, ok, on, err] { if (*alive) AutostartDone(ok, on, err); });
        }).detach();
    }

    void AutostartDone(bool ok, bool on, const std::string& err)
    {
        busy_ = false;
        if (ok) Log(on ? "Autostart is on: the background service starts by itself"
                       : "Autostart is off");
        else    wxMessageBox(wxString::FromUTF8(err), "Autostart", wxOK | wxICON_WARNING, this);
        retry_ = 3;     // reconnect right away
    }

    // ------------------------------------------------------------ security menu
    void OnClearCache()
    {
        if (!connected_) {
            wxMessageBox("The background service is not running.", "Clear cache", wxOK | wxICON_INFORMATION, this);
            return;
        }
        const int r = wxMessageBox(
            "Clear the cache?\n\n"
            "This forgets which devices may control this PC (each one has to be allowed again) and the "
            "remembered monitor layout.\n\n"
            "Your keys, group password, layout and per-device settings are kept.",
            "Clear cache", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this);
        if (r != wxYES) return;
        client_->Send("CLEARCACHE");
        Log("Clearing the cache ...");
    }

    void OnRegenKeys()
    {
        if (!connected_) {
            wxMessageBox("The background service is not running.", "Regenerate keys", wxOK | wxICON_INFORMATION, this);
            return;
        }
        const int r = wxMessageBox(
            "Regenerate this PC's keys?\n\n"
            "This PC gets a new identity. Other PCs will see it as a NEW device: they have to allow it "
            "again, and settings saved for the current set of PCs no longer apply.\n"
            "The old key is deleted and cannot be restored. The background service restarts.",
            "Regenerate keys", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
        if (r != wxYES) return;
        client_->Send("REGENKEYS");
        Log("Generating new keys; the background service restarts ...");
    }

    // Opens the folder that holds identity.key (the private key: never share it) and trust.txt.
    void OnViewKeys()
    {
#if defined(__WXMSW__)
        const wxString dir(dataDir_.wstring());
#else
        const wxString dir = wxString::FromUTF8(dataDir_.string().c_str());
#endif
        if (!wxLaunchDefaultApplication(dir))
            wxMessageBox("Could not open the folder:\n" + dir, "View keys", wxOK | wxICON_WARNING, this);
    }

    void ToggleService()
    {
        if (daemonWanted_) {
            daemonWanted_ = false;
            ipc::RequestStop(dataDir_);
            Log("Stopping the background service (it starts again with the next window, or at login if autostart is on)");
        } else {
            daemonWanted_ = true;
            retry_ = 3;
            Log("Starting the background service...");
        }
        UpdateHeader();
    }

    void QuitAll()
    {
        daemonWanted_ = false;
        ipc::RequestStop(dataDir_);
        Close();
    }

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
            auto* groupBox = new InputBox(groupCard, theme_, wxTE_PASSWORD | wxTE_PROCESS_ENTER,
                                          wxString::FromUTF8(groupPwd_), "password");
            groupCtl_ = groupBox->ctl;
            groupBtn_ = new FlatButton(groupCard, theme_, "Apply", true);
            gr->Add(groupBox, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            gr->Add(groupBtn_, 0, wxALIGN_CENTER_VERTICAL);
            gv->Add(gr, 0, wxEXPAND);
            groupLbl_ = MakeLabel(groupCard, "", true);
            gv->Add(groupLbl_, 0, wxTOP, 8);
            auto* go = new wxBoxSizer(wxVERTICAL);
            go->Add(gv, 1, wxEXPAND | wxALL, 14);
            groupCard->SetSizer(go);
            ApplyBackground(groupCard, theme_.card);
        }
        right->Add(groupCard, 0, wxEXPAND | wxBOTTOM, 8);

        // --- clipboard transfer limit
        auto* clipCard = new CardPanel(p, theme_, false);
        {
            auto* cv = new wxBoxSizer(wxVERTICAL);
            cv->Add(MakeHeader(clipCard, "Clipboard"), 0, wxBOTTOM, 2);
            cv->Add(MakeLabel(clipCard, "Largest text or set of files that is copied to another PC. Bigger clipboards are neither sent nor accepted.", true), 0, wxBOTTOM, 8);
            auto* cr = new wxBoxSizer(wxHORIZONTAL);
            auto* clipBox = new InputBox(clipCard, theme_, wxTE_PROCESS_ENTER, "1", "size");
            clipLimitCtl_ = clipBox->ctl;
            clipBox->SetMinSize(wxSize(110, -1));
            clipUnit_ = new wxChoice(clipCard, wxID_ANY);
            clipUnit_->Append("KiB");
            clipUnit_->Append("MiB");
            clipUnit_->Append("GiB");
            clipUnit_->SetSelection(2);
            clipBtn_ = new FlatButton(clipCard, theme_, "Apply", true);
            cr->Add(clipBox, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            cr->Add(clipUnit_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            cr->Add(clipBtn_, 0, wxALIGN_CENTER_VERTICAL);
            cv->Add(cr, 0, wxEXPAND);
            clipLbl_ = MakeLabel(clipCard, "", true);
            cv->Add(clipLbl_, 0, wxTOP, 8);
            auto* co = new wxBoxSizer(wxVERTICAL);
            co->Add(cv, 1, wxEXPAND | wxALL, 14);
            clipCard->SetSizer(co);
            ApplyBackground(clipCard, theme_.card);
        }
        right->Add(clipCard, 0, wxEXPAND | wxBOTTOM, 8);

        // --- connect by address
        auto* connCard = new CardPanel(p, theme_, false);
        {
            auto* cv = new wxBoxSizer(wxVERTICAL);
            cv->Add(MakeHeader(connCard, "Connect"), 0, wxBOTTOM, 8);
            auto* r1 = new wxBoxSizer(wxHORIZONTAL);
            auto* targetBox = new InputBox(connCard, theme_, wxTE_PROCESS_ENTER, "", "target IP address");
            target_ = targetBox->ctl;
            goBtn_ = new FlatButton(connCard, theme_, "Go", true);
            r1->Add(targetBox, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            r1->Add(goBtn_, 0, wxALIGN_CENTER_VERTICAL);
            cv->Add(r1, 0, wxEXPAND);
            auto* r2 = new wxBoxSizer(wxHORIZONTAL);
            scanBtn_ = new FlatButton(connCard, theme_, "Scan LAN");
            searchLbl_ = new wxStaticText(connCard, wxID_ANY, "");
            r2->Add(scanBtn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
            r2->Add(searchLbl_, 1, wxALIGN_CENTER_VERTICAL);
            cv->Add(r2, 0, wxEXPAND | wxTOP, 8);
            auto* co = new wxBoxSizer(wxVERTICAL);
            co->Add(cv, 1, wxEXPAND | wxALL, 14);
            connCard->SetSizer(co);
            ApplyBackground(connCard, theme_.card);
            searchLbl_->SetForegroundColour(theme_.sub);
        }
        right->Add(connCard, 0, wxEXPAND | wxBOTTOM, 8);

        // --- activity log
        auto* logCard = new CardPanel(p, theme_, false);
        {
            auto* lv = new wxBoxSizer(wxVERTICAL);
            lv->Add(MakeHeader(logCard, "Activity"), 0, wxBOTTOM, 6);
            auto* logBox = new InputBox(logCard, theme_, wxTE_MULTILINE | wxTE_READONLY, "", "");
            log_ = logBox->ctl;
            log_->SetFont(wxFont(wxFontInfo(wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT).GetPointSize()).Family(wxFONTFAMILY_TELETYPE)));
            logBox->SetMinSize(wxSize(-1, 70));
            lv->Add(logBox, 1, wxEXPAND);
            auto* lo = new wxBoxSizer(wxVERTICAL);
            lo->Add(lv, 1, wxEXPAND | wxALL, 14);
            logCard->SetSizer(lo);
            ApplyBackground(logCard, theme_.card);
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
        l->SetForegroundColour(theme_.text);
        return l;
    }

    wxStaticText* MakeLabel(wxWindow* parent, const wxString& text, bool subtle = false)
    {
        auto* l = new wxStaticText(parent, wxID_ANY, text);
        l->SetForegroundColour(subtle ? theme_.sub : theme_.text);
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
        volSlider_ = new FlatSlider(bar, theme_, 0, 100, 100, 150);
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
        FlatCheck *share = nullptr, *mic = nullptr, *allow = nullptr, *clip = nullptr;
        SpeakerButton* mute = nullptr;
        FlatSlider* vol = nullptr;
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
        r.name->SetForegroundColour(theme_.text);
        r.sub = MakeLabel(c, "", true);
        r.detail = MakeLabel(c, "", true);
        auto* info = new wxBoxSizer(wxVERTICAL);
        info->Add(r.name, 0, wxBOTTOM, 3);
        info->Add(r.sub, 0, wxBOTTOM, 2);
        info->Add(r.detail, 0);

        // switches
        r.share = new FlatCheck(c, theme_, "Share my audio");
        r.share->SetToolTip("Send what this PC plays (system audio) to this device only.\nIt may echo if this device sends its audio back.");
        if (!AudioEngine::SystemAudioSupported()) {
            r.share->Disable();
            r.share->SetToolTip("Sharing system audio is not supported on this platform");
        }
        r.mic = new FlatCheck(c, theme_, "Share my microphone");
        r.mic->SetToolTip("Send this PC's microphone to this device only");
        r.clip = new FlatCheck(c, theme_, "Share my clipboard");
        r.clip->SetToolTip("Send this PC's clipboard (text, or one or more files) to this device when the cursor moves over to it.\n"
                           "It is accepted there if this device may control that PC or shares its clipboard back.\n"
                           "On Linux, text needs xclip, xsel or wl-clipboard; copying files needs xclip or wl-clipboard.");
        r.allow = new FlatCheck(c, theme_, "Allow remote control");
        r.allow->SetToolTip("Let this device use this PC's keyboard and mouse");
        auto* checks = new wxBoxSizer(wxVERTICAL);
        checks->Add(r.share, 0, wxBOTTOM, 8);
        checks->Add(r.mic, 0, wxBOTTOM, 8);
        checks->Add(r.clip, 0, wxBOTTOM, 8);
        checks->Add(r.allow, 0);

        // audio from this device
        r.mute = new SpeakerButton(c, theme_, 34);
        r.vol = new FlatSlider(c, theme_, 0, 100, 100, 170);
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

        // events (looked up by connection id: the card may outlive a rebuild of the map).
        // The change is applied here at once and sent to the daemon, which does the real work.
        r.share->Bind(wxEVT_CHECKBOX, [this, id](wxCommandEvent& e) {
            auto it = peers_.find(id);
            if (it == peers_.end()) return;
            it->second.share = e.IsChecked();
            SendAudio(id);
        });
        r.mic->Bind(wxEVT_CHECKBOX, [this, id](wxCommandEvent& e) {
            auto it = peers_.find(id);
            if (it == peers_.end()) return;
            it->second.mic = e.IsChecked();
            SendAudio(id);
        });
        r.clip->Bind(wxEVT_CHECKBOX, [this, id](wxCommandEvent& e) {
            auto it = peers_.find(id);
            if (it == peers_.end()) return;
            it->second.clip = e.IsChecked();
            lastAudio_[id] = std::chrono::steady_clock::now();      // keep the click until the daemon confirms it
            client_->Send("CLIPBOARD " + std::to_string(id) + (e.IsChecked() ? " 1" : " 0"));
        });
        r.allow->Bind(wxEVT_CHECKBOX, [this, id](wxCommandEvent& e) {
            if (peers_.find(id) == peers_.end()) return;
            if (e.IsChecked() && !snap_.inputCanInject) {      // cannot work: do not even try
                if (auto it = devRows_.find(id); it != devRows_.end()) it->second.allow->SetValue(false);
                ShowInputProblem(false);
                return;
            }
            client_->Send("CONTROL " + std::to_string(id) + (e.IsChecked() ? " 1" : " 0"));
        });
        r.mute->onToggle = [this, id](bool m) {
            auto it = peers_.find(id);
            if (it == peers_.end()) return;
            it->second.mute = m;
            SendAudio(id);
        };
        r.vol->Bind(wxEVT_SLIDER, [this, id](wxCommandEvent& e) {
            auto it = peers_.find(id);
            if (it == peers_.end()) return;
            it->second.vol = e.GetInt();
            auto rit = devRows_.find(id);
            if (rit != devRows_.end()) rit->second.pct->SetLabelText(wxString::Format("%d%%", it->second.vol));
            SendAudio(id);
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

        wxString in;
        wxColour accent = theme_.ok;
        if (inputMode_ == 1 && inputPeer_ == id)  { in = "you are controlling it now"; accent = theme_.accent; }
        else if (inputMode_ == 2 && inputPeer_ == id) { in = "it is controlling this PC now"; accent = theme_.warn; }
        else if (p.allowed) in = "may control this PC";
        else                in = "cannot control this PC";
        SetText(r.detail, "Key " + wxString::FromUTF8(Identity::Fingerprint(p.key)) + dot + in);
        r.card->SetAccent(accent);

        r.share->SetValue(p.share);
        r.mic->SetValue(p.mic);
        r.clip->SetValue(p.clip);
        r.allow->SetValue(p.allowed);
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

    // ------------------------------------------------------------ input problems
    // Shown at start when this PC cannot be controlled (or cannot share input at all).
    void ShowInputProblem(bool atStart)
    {
        const bool none = snap_.backend == "none";
        wxString msg;
        if (none) msg = "Keyboard and mouse sharing could not start on this PC.\n\n";
        else      msg = "This PC can control other PCs, but it cannot be controlled.\n\n";
        if (!snap_.inputNote.empty()) msg += wxString::FromUTF8(snap_.inputNote.c_str()) + "\n\n";
        msg += "Remote control of this PC is switched off until this works, so the other PC does not "
               "lose its keyboard and mouse while trying.\n\n";
#if defined(__WXGTK__)
        msg += "How to fix it (Linux):\n"
               "1. Run linux-input-setup.sh (it asks for sudo).\n"
               "2. Log out and back in.\n\n"
               "Being in the 'input' group is not enough by itself: the group must be active in this login "
               "session, the 'uinput' kernel module must be loaded, and /dev/uinput must be writable for the "
               "group (the script sets up the udev rule).";
#endif
        wxMessageBox(msg, atStart ? "Keyboard and mouse sharing" : "Remote control not possible",
                     wxOK | wxICON_WARNING, this);
    }

    // ------------------------------------------------------------ clipboard limit (Network tab)
    static wxString WithSeparators(unsigned long long v)
    {
        std::string s = std::to_string(v), o;
        for (size_t i = 0; i < s.size(); ++i) {
            if (i && (s.size() - i) % 3 == 0) o += ',';
            o += s[i];
        }
        return wxString::FromUTF8(o.c_str());
    }

    void UpdateClipLabel(const wxString& error = wxString())
    {
        if (!error.empty()) {
            clipLbl_->SetLabelText(error);
            clipLbl_->SetForegroundColour(theme_.warn);
        } else {
            wxString unit;
            unsigned long long v = clipLimit_;
            if (clipLimit_ % (1ull << 30) == 0)      { v = clipLimit_ >> 30; unit = "GiB"; }
            else if (clipLimit_ % (1ull << 20) == 0) { v = clipLimit_ >> 20; unit = "MiB"; }
            else                                     { v = clipLimit_ >> 10; unit = "KiB"; }
            clipLbl_->SetLabelText("Limit: " + WithSeparators(v) + " " + unit + "  (" + WithSeparators(clipLimit_) + " bytes)");
            clipLbl_->SetForegroundColour(theme_.ok);
        }
        clipLbl_->Refresh();
    }

    // Shows a byte count as "<value> <KiB|MiB|GiB>", using the largest unit that divides it exactly.
    void ShowClipLimit(unsigned long long bytes)
    {
        clipLimit_ = bytes;
        unsigned long long v = bytes >> 10;
        int unit = 0;
        if (bytes % (1ull << 30) == 0)      { v = bytes >> 30; unit = 2; }
        else if (bytes % (1ull << 20) == 0) { v = bytes >> 20; unit = 1; }
        clipLimitCtl_->ChangeValue(wxString::Format("%llu", v));
        clipUnit_->SetSelection(unit);
        UpdateClipLabel();
    }

    void ApplyClipLimit()
    {
        wxString s = clipLimitCtl_->GetValue();
        s.Trim(true).Trim(false);
        wxULongLong_t v = 0;
        if (s.empty() || !s.ToULongLong(&v) || v == 0) {
            UpdateClipLabel("Enter a whole number, 1 or more.");
            return;
        }
        const int u = clipUnit_->GetSelection();           // 0 KiB, 1 MiB, 2 GiB
        if (u < 0) return;
        const unsigned long long mult = 1ull << (10 + 10 * u);
        if (v > (1ull << 40) / mult) {
            UpdateClipLabel("The largest possible limit is 1024 GiB.");
            return;
        }
        clipLimit_ = (unsigned long long)v * mult;
        clipEdited_ = std::chrono::steady_clock::now();
        client_->Send("CLIPLIMIT " + std::to_string(clipLimit_));
        UpdateClipLabel();
    }

    void ApplyGroupPassword()
    {
        groupPwd_ = std::string(groupCtl_->GetValue().utf8_str());
        client_->Send("GROUP " + ipc::Enc(groupPwd_));
        UpdateGroupLabel();
    }

    // ------------------------------------------------------------ clients (Network tab)
    struct CliView {
        std::string key, ip;
        wxString name;
        bool connected = false;
        bool sameGroup = false, seen = false;
    };

    // One entry per PC (by public key) that runs seamless-input: seen on the LAN, connected,
    // or connected.
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
        for (const auto& kv : peers_) {
            if (kv.second.key.empty()) continue;
            CliView& v = at(kv.second.key);
            v.connected = true;
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
        r.name->SetForegroundColour(theme_.text);
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
        r.name->SetForegroundColour(theme_.text);
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

    // ---------------------------------------------------------------- actions (all go to the daemon)
    void Activate(const std::string& ip)
    {
        if (ip.empty()) return;
        client_->Send("SELECT " + ipc::Enc(ip));
    }

    void StartScan() { client_->Send("SCAN"); }

    void SendMaster()
    {
        masterSent_ = std::chrono::steady_clock::now();
        client_->Send(wxString::Format("MASTER %d %d", masterMute_->IsMuted() ? 1 : 0, volSlider_->GetValue()).ToStdString());
    }

    void SendAudio(int id)
    {
        auto it = peers_.find(id);
        if (it == peers_.end()) return;
        const PeerInfo& p = it->second;
        lastAudio_[id] = std::chrono::steady_clock::now();
        client_->Send("AUDIO " + std::to_string(id) + " " + (p.share ? "1" : "0") + " " + (p.mic ? "1" : "0") + " " +
                      (p.mute ? "1" : "0") + " " + std::to_string(p.vol));
    }

    void UpdateSearchLabel()
    {
        const int n = animStep_ % 3 + 1;
        wxString dots = wxString("...").Left(n);
        searchLbl_->SetLabel(wxString::Format("searching %s   (%d found)", dots, (int)scan_.size()));
    }

    // ------------------------------------------------------- state from the daemon
    void ApplySnapshot(const state::Snapshot& s)
    {
        const auto lines = ipc::SerializeSnapshot(s);
        if (lines == snapLines_) return;                // nothing visible changed
        snapLines_ = lines;
        const bool keyChanged = snap_.pubKey != s.pubKey;
        snap_ = s;
        if (!inputChecked_) {                           // once per start, with the first real answer from the daemon
            inputChecked_ = true;
            if (!s.inputCanInject || s.backend == "none")
                wxTheApp->CallAfter([this] { ShowInputProblem(true); });
        }
        if (keyChanged) UpdateHeader();

        using clock = std::chrono::steady_clock;
        const auto now = clock::now();
        auto recent = [&](const std::map<int, clock::time_point>& m, int id) {
            auto it = m.find(id);
            return it != m.end() && now - it->second < std::chrono::milliseconds(400);
        };

        // this PC's monitors, as the daemon sees them (it is who tells the peers)
        std::vector<wxRect> lm;
        for (const auto& r : s.mons) lm.emplace_back(r.x, r.y, r.w, r.h);
        if (lm != localMons_) { localMons_ = lm; canvas_->SetLocal(host_, localMons_); }

        if (groupPwd_ != s.group) {
            groupPwd_ = s.group;
            UpdateGroupLabel();
            if (!groupCtl_->HasFocus()) groupCtl_->ChangeValue(wxString::FromUTF8(groupPwd_));
        } else if (!groupInit_) {
            groupCtl_->ChangeValue(wxString::FromUTF8(groupPwd_));
        }
        groupInit_ = true;

        if (!clipInit_ || (s.clipLimit != clipLimit_ && now - clipEdited_ > std::chrono::milliseconds(1500))) {
            ShowClipLimit(s.clipLimit);
            clipInit_ = true;
        }

        inputMode_ = s.inputMode;
        inputPeer_ = s.inputPeer;

        if (now - masterSent_ > std::chrono::milliseconds(400)) {
            masterMute_->SetMuted(s.masterMute);
            volSlider_->SetValue(s.masterVol);
            masterPct_->SetLabelText(wxString::Format("%d%%", s.masterVol));
        }

        std::map<int, PeerInfo> np;
        for (const auto& p : s.peers) {
            PeerInfo pi;
            pi.inst = p.inst; pi.key = p.key; pi.name = wxString::FromUTF8(p.name); pi.addr = p.addr;
            for (const auto& r : p.mons) pi.mons.emplace_back(r.x, r.y, r.w, r.h);
            pi.incoming = p.incoming; pi.placed = p.placed;
            pi.ox = p.ox; pi.oy = p.oy;
            pi.share = p.share; pi.mic = p.mic; pi.mute = p.mute; pi.vol = p.vol;
            pi.allowed = p.allowed;
            pi.clip = p.clip;
            auto old = peers_.find(p.id);
            if (old != peers_.end() && recent(lastAudio_, p.id)) {      // the user is touching these right now
                pi.share = old->second.share; pi.mic = old->second.mic;
                pi.mute = old->second.mute;   pi.vol = old->second.vol;
                pi.clip = old->second.clip;
            }
            np[p.id] = std::move(pi);
        }
        peers_.swap(np);

        disc_.clear();
        for (const auto& d : s.disc) disc_[d.ip] = {d.host, d.server, now, d.key, d.sameGroup};
        scan_.clear();
        for (const auto& c : s.scan) scan_[c.ip] = {wxString::FromUTF8(c.name), c.server, now};

        // the layout canvas follows the daemon (which owns positions and collision handling)
        for (const auto& kv : peers_) {
            const PeerInfo& p = kv.second;
            if (!p.placed) continue;
            if (!canvas_->HasPeer(kv.first)) canvas_->AddPeerAt(kv.first, p.name, wxString::FromUTF8(p.addr), p.mons, p.ox, p.oy);
            else if (!canvas_->IsDragging(kv.first) && !recent(lastMove_, kv.first)) canvas_->SetPeerPos(kv.first, p.ox, p.oy);
        }
        for (int id : canvas_->PeerIds()) {
            auto it = peers_.find(id);
            if (it == peers_.end() || !it->second.placed) canvas_->RemovePeer(id);
        }

        if (s.scanning != scanning_) {
            scanning_ = s.scanning;
            if (scanning_) {
                scanBtn_->Enable(false);
                scanBtn_->SetLabel("Scanning...");
                animStep_ = 0;
                UpdateSearchLabel();
                animTimer_.Start(300);
            } else {
                animTimer_.Stop();
                searchLbl_->SetLabel(wxString::Format("Scan complete: %d host(s) found", (int)scan_.size()));
                scanBtn_->SetLabel("Scan LAN");
                scanBtn_->Enable(true);
            }
        }

        RebuildList();
        RebuildDevices();
    }

    // -------------------------------------------------------------------- GUI
    void RebuildList()
    {
        // PCs that run seamless-input are shown as client cards, not in this list
        auto isClient = [this](const std::string& ip) {
            if (disc_.count(ip)) return true;
            for (auto& kv : peers_) if (kv.second.addr == ip) return true;
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
                                     s.server ? "Server running" : "Online"};
        }

        entries_.clear();
        for (auto& kv : remote) entries_.push_back(kv.second);

        RebuildOthers();
        othDivider_->Set(wxString::Format("Other hosts  (IP addresses)  -  %d", (int)entries_.size()));
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

    void UpdateAudioStats()
    {
        for (auto& kv : devRows_) {
            auto pit = peers_.find(kv.first);
            if (pit == peers_.end()) continue;
            DevRow& r = kv.second;

            bool recv = false;
            double kbps = 0;
            float level = 0;
            for (const auto& st : stats_.peers)
                if (st.inst == pit->second.inst && st.receiving) { recv = true; kbps = st.kbps; level = st.level; }

            r.level->SetLevel(recv ? std::sqrt(level) : 0.0f);
            wxString txt = recv ? wxString::Format("Receiving %.0f kbps", kbps) : wxString("No incoming audio");
            const bool sendSys = pit->second.share && snap_.sysCapturing;
            const bool sendMic = pit->second.mic && snap_.micCapturing;
            if (sendSys || sendMic)
                txt += wxString::FromUTF8("  \xC2\xB7  Sending my ") + (sendSys && sendMic ? "audio + microphone" : sendSys ? "audio" : "microphone");
            SetText(r.audioState, txt);
        }
    }

    void Log(const wxString& msg) { LogRaw(wxDateTime::Now().FormatTime() + "  " + msg); }
    void LogRaw(const wxString& line) { log_->AppendText(line + "\n"); }

    void OnTimer(wxTimerEvent&)
    {
        // The window needs its daemon: bring it back if it went away (unless the user stopped it).
        if (!connected_ && !connecting_ && daemonWanted_ && !busy_ && ++retry_ >= 3) {
            retry_ = 0;
            UpdateHeader();
            StartConnect();
        }
    }

    // Closing the window does NOT stop sharing: the daemon keeps running in the background.
    void OnClose(wxCloseEvent&)
    {
        timer_.Stop();
        animTimer_.Stop();
        *alive_ = false;
        client_->Close();
        Destroy();
    }

    wxString host_;
    std::shared_ptr<bool> alive_;
    std::shared_ptr<ipc::Client> client_;
    std::filesystem::path dataDir_;
    state::Snapshot snap_;
    std::vector<std::string> snapLines_;
    state::Stats stats_;
    bool connected_ = false, connecting_ = false, daemonWanted_ = true, busy_ = false, groupInit_ = false;
    int retry_ = 0;
    int connFails_ = 0;             // consecutive failed attempts to reach the daemon
    bool restartAsked_ = false;     // the restart question was shown (until success or a failed restart)
    std::string lastConnErr_;
    std::map<int, std::chrono::steady_clock::time_point> lastMove_, lastAudio_;
    std::chrono::steady_clock::time_point masterSent_;
    wxStaticText* hdrSub_ = nullptr;
    MenuStrip* menuBar_ = nullptr;

    std::vector<LocalAddress> local_;
    std::vector<wxRect> localMons_;
    std::vector<Entry> entries_;
    std::map<std::string, Discovered> disc_;
    std::map<std::string, CliRow> cliRows_;
    std::map<std::string, Scanned> scan_;
    std::map<int, PeerInfo> peers_;
    bool scanning_ = false;
    int animStep_ = 0;

    Canvas* canvas_ = nullptr;
    TabBar* tabs_ = nullptr;
    wxSimplebook* pages_ = nullptr;
    SectionDivider* othDivider_ = nullptr;
    wxStaticText* othEmpty_ = nullptr;
    bool listDirty_ = false;
    std::map<std::string, OthRow> othRows_;
    wxTextCtrl* groupCtl_ = nullptr;
    wxTextCtrl* clipLimitCtl_ = nullptr;
    wxChoice* clipUnit_ = nullptr;
    FlatButton* clipBtn_ = nullptr;
    wxStaticText* clipLbl_ = nullptr;
    bool inputChecked_ = false;                     // the start-up check for input problems was done
    unsigned long long clipLimit_ = 1ull << 30;     // 1 GiB until the daemon tells us otherwise
    bool clipInit_ = false;
    std::chrono::steady_clock::time_point clipEdited_;
    FlatButton* groupBtn_ = nullptr;
    wxStaticText* groupLbl_ = nullptr;
    wxScrolledWindow* cliScroll_ = nullptr;
    wxBoxSizer* cliSizer_ = nullptr;
    wxStaticText* cliEmpty_ = nullptr;
    wxStaticText* clientsCount_ = nullptr;
    std::string groupPwd_;
    wxScrolledWindow* devScroll_ = nullptr;
    wxBoxSizer* devSizer_ = nullptr;
    wxStaticText* emptyLbl_ = nullptr;
    std::map<int, DevRow> devRows_;
    Theme theme_ = InitialTheme();
    wxString themeName_;                            // the theme in use (menu check mark, saved choice)
    std::vector<ThemeEntry> themes_;                // everything found in the theme folders
    int themeMenuIdx_ = -1;
    wxTextCtrl* target_ = nullptr;
    wxTextCtrl* log_ = nullptr;
    FlatButton* goBtn_ = nullptr;
    FlatButton* scanBtn_ = nullptr;
    wxStaticText* searchLbl_ = nullptr;
    SpeakerButton* masterMute_ = nullptr;
    wxStaticText* masterPct_ = nullptr;
    FlatSlider* volSlider_ = nullptr;
    wxTimer timer_;
    wxTimer animTimer_;
    int inputMode_ = 0;           // 0 local, 1 forwarding (I control a peer), 2 controlled
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
