#pragma once
// Layout model of the connected PCs (headless twin of the geometry in the GUI's Canvas).
// Positions are "world" pixels: a group's top-left corner relative to this PC's top-left.
#include <algorithm>
#include <climits>
#include <cmath>
#include <tuple>
#include <utility>
#include <vector>

#include "input/input_types.hpp"

namespace layout {

struct Group {
    int id = 0;
    std::vector<InputRect> mons;    // relative to the group's top-left corner
    int w = 0, h = 0;
    double ox = 0, oy = 0;
};

inline Group MakeGroup(int id, std::vector<InputRect> mons)
{
    Group g;
    g.id = id;
    if (mons.empty()) mons.push_back({0, 0, 1920, 1080});
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
    for (const auto& m : mons) {
        x0 = std::min(x0, m.x);        y0 = std::min(y0, m.y);
        x1 = std::max(x1, m.x + m.w);  y1 = std::max(y1, m.y + m.h);
    }
    for (auto& m : mons) { m.x -= x0; m.y -= y0; }
    g.mons = std::move(mons);
    g.w = x1 - x0;
    g.h = y1 - y0;
    return g;
}

class Model {
public:
    Model() { local_ = MakeGroup(-1, {}); }

    void SetLocal(const std::vector<InputRect>& mons) { local_ = MakeGroup(-1, mons); }

    // New peers start flush against the existing layout (top aligned with this PC).
    std::pair<double, double> AddPeer(int id, const std::vector<InputRect>& mons, bool toTheRight)
    {
        RemovePeer(id);
        Group g = MakeGroup(id, mons);
        g.ox = toTheRight ? RightEdge() : LeftEdge() - g.w;
        g.oy = local_.oy;
        peers_.push_back(g);
        return {g.ox, g.oy};
    }

    void RemovePeer(int id)
    {
        peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                                    [id](const Group& g) { return g.id == id; }),
                     peers_.end());
    }

    void SetPeerPos(int id, double ox, double oy)
    {
        if (Group* g = Find(id)) { g->ox = ox; g->oy = oy; }
    }

    // The peer's monitors changed (plugged, unplugged, rotated, other resolution): same place, new shape.
    // Returns false when nothing changed.
    bool UpdatePeerMons(int id, const std::vector<InputRect>& mons)
    {
        Group* g = Find(id);
        if (!g) return false;
        Group n = MakeGroup(id, mons);
        if (n.w == g->w && n.h == g->h && SameRects(n.mons, g->mons)) return false;
        n.ox = g->ox;
        n.oy = g->oy;
        *g = std::move(n);
        return true;
    }

    // Moves the peer there, but if one of its monitors would land on another monitor it is
    // pushed out to the nearest free edge. Returns the position it really got.
    std::pair<double, double> PlacePeer(int id, double ox, double oy)
    {
        Group* g = Find(id);
        if (!g) return {ox, oy};
        ox = std::round(ox);
        oy = std::round(oy);
        if (Collides(*g, ox, oy)) std::tie(ox, oy) = PushOut(*g, ox, oy);
        g->ox = ox;
        g->oy = oy;
        return {ox, oy};
    }

    bool PeerPos(int id, double& ox, double& oy) const
    {
        if (const Group* g = Find(id)) { ox = g->ox; oy = g->oy; return true; }
        return false;
    }

private:
    static bool SameRects(const std::vector<InputRect>& a, const std::vector<InputRect>& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].w != b[i].w || a[i].h != b[i].h) return false;
        return true;
    }

    Group* Find(int id)
    {
        for (auto& g : peers_) if (g.id == id) return &g;
        return nullptr;
    }
    const Group* Find(int id) const
    {
        for (const auto& g : peers_) if (g.id == id) return &g;
        return nullptr;
    }

    double RightEdge() const
    {
        double r = local_.ox + local_.w;
        for (const auto& g : peers_) r = std::max<double>(r, g.ox + g.w);
        return r;
    }

    double LeftEdge() const
    {
        double l = local_.ox;
        for (const auto& g : peers_) l = std::min(l, g.ox);
        return l;
    }

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
                    if (Overlap(ox + m.x, oy + m.y, ox + m.x + m.w, oy + m.y + m.h,
                                o.ox + n.x, o.oy + n.y, o.ox + n.x + n.w, o.oy + n.y + n.h))
                        return true;
            return false;
        };
        if (test(local_)) return true;
        for (const auto& o : peers_) if (test(o)) return true;
        return false;
    }

    std::pair<double, double> PushOut(const Group& g, double ox, double oy) const
    {
        for (int guard = 0; guard < 64 && Collides(g, ox, oy); ++guard) {
            double bestAbs = 1e18, bdx = 0, bdy = 0;
            auto consider = [&](const Group& o) {
                if (o.id == g.id) return;
                for (const auto& m : g.mons) {
                    const double l = ox + m.x, t = oy + m.y, r = l + m.w, b = t + m.h;
                    for (const auto& n : o.mons) {
                        const double nl = o.ox + n.x, nt = o.oy + n.y, nr = nl + n.w, nb = nt + n.h;
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
            double r = local_.ox + local_.w;
            for (const auto& o : peers_) if (o.id != g.id) r = std::max<double>(r, o.ox + o.w);
            ox = r;
            oy = local_.oy;
        }
        return {ox, oy};
    }

    Group local_;
    std::vector<Group> peers_;
};

} // namespace layout
