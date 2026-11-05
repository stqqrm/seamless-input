#pragma once
#include <algorithm>
#include <climits>
#include <vector>

struct InputRect {
    int x = 0, y = 0, w = 0, h = 0;
};

// What travels between PCs (and between a backend and the engine):
//   Move    a = dx, b = dy                       screen pixels
//   Button  a = 0 left, 1 right, 2 middle,
//               3 back, 4 forward;  b = 1 down / 0 up
//   Wheel   a = dx, b = dy                       1/120 notch units, +y = up, +x = right
//   Key     a = Linux evdev key code; b = 1 down / 0 up
// Linux evdev codes are the wire format for keys because they identify the
// *physical* key, so every PC applies its own keyboard layout.
enum class InputKind : unsigned char { Move, Button, Wheel, Key };

struct InputEvent {
    InputKind kind = InputKind::Move;
    int a = 0;
    int b = 0;
};

namespace inputgeom {

inline bool Contains(const InputRect& r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

inline bool InsideAny(const std::vector<InputRect>& v, int x, int y)
{
    for (const auto& r : v)
        if (Contains(r, x, y)) return true;
    return false;
}

// Moves (x, y) to the nearest pixel that belongs to one of the monitors.
inline void ClampToAny(const std::vector<InputRect>& v, int& x, int& y)
{
    long long best = LLONG_MAX;
    int bx = x, by = y;
    for (const auto& r : v) {
        if (r.w <= 0 || r.h <= 0) continue;
        const int cx = std::clamp(x, r.x, r.x + r.w - 1);
        const int cy = std::clamp(y, r.y, r.y + r.h - 1);
        const long long dx = x - cx, dy = y - cy, d = dx * dx + dy * dy;
        if (d < best) { best = d; bx = cx; by = cy; }
    }
    x = bx;
    y = by;
}

inline InputRect Bounds(const std::vector<InputRect>& v)
{
    if (v.empty()) return {0, 0, 1920, 1080};
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
    for (const auto& r : v) {
        x0 = std::min(x0, r.x);           y0 = std::min(y0, r.y);
        x1 = std::max(x1, r.x + r.w);     y1 = std::max(y1, r.y + r.h);
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

// Centre of the monitor that contains (or is nearest to) (x, y).
inline void MonitorCenter(const std::vector<InputRect>& v, int x, int y, int& cx, int& cy)
{
    ClampToAny(v, x, y);
    for (const auto& r : v)
        if (Contains(r, x, y)) { cx = r.x + r.w / 2; cy = r.y + r.h / 2; return; }
    cx = x;
    cy = y;
}

} // namespace inputgeom
