#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "input/input_types.hpp"

// Shares this PC's keyboard and mouse with the PCs of the TCP mesh.
//
//   Local        you are using this PC. When the cursor touches a screen edge that
//                borders a peer's monitor in the canvas layout, the engine
//                switches to Forwarding.
//   Forwarding   local input is swallowed and sent to one peer (ENTER/M/B/W/K).
//   Controlled   a peer drives this PC. When its cursor leaves through the edge
//                that borders the controller, control goes back (RETURN); when this
//                PC's own mouse is moved or keyboard used, control goes back too (TAKE) and stays with the
//                owner until they have been idle for a few seconds.
//
// With several PCs the controller follows the controlled cursor in its own layout and
// hops straight to a third PC when the cursor runs off towards it (RELEASE + ENTER).
//
// Wire protocol (added to the existing line protocol, handled by HandleMessage):
//   ENTER x y      controller -> peer   take over, cursor at x,y (peer desktop coords)
//   M dx dy        controller -> peer   relative motion
//   B btn down     controller -> peer   mouse button
//   W dx dy        controller -> peer   wheel, 120 = one notch
//   K code down    controller -> peer   key (Linux evdev code)
//   RETURN x y     peer -> controller   cursor left the peer, put yours at x,y
//   RELEASE        controller -> peer   stop controlling (also sent when control hops to a third PC)
//   TAKE           peer -> controller   the peer's owner used their own mouse/keyboard: control is theirs again.
//                                       Also the answer to an ENTER that arrives while the owner is active.
//                                       The controller waits a few seconds before it tries again (owner priority).
//   DENY           peer -> controller   refused / revoked
//   CTRL 0|1       peer -> controller   "you may (not) control me" (sent whenever that changes; handled by
//                                       Core, which calls SetPeerAccepts). The controller never takes over
//                                       local input for a peer that has not said 1, so a PC that does not
//                                       allow remote control never makes the cursor jump or the input freeze.
//
// SECURITY: injecting input is remote control of the PC. A peer may only control
// this PC after SetPeerAllowed(id, true); until then ENTER is answered with DENY.
class InputEngine {
public:
    enum class Mode { Local, Forwarding, Controlled };

    using SendFn = std::function<void(int peerId, const std::string& line)>;
    using LogFn  = std::function<void(const std::string& msg)>;

    InputEngine(SendFn send, LogFn log);
    ~InputEngine();
    InputEngine(const InputEngine&) = delete;
    InputEngine& operator=(const InputEngine&) = delete;

    // Callbacks: invoked on engine/backend/network threads, never re-enter the engine from them.
    std::function<void(Mode mode, int peerId)> onModeChange;

    // The cursor of this PC has just moved over to peer 'peerId': it started controlling that peer
    // (also when control hops from one peer to another), or this PC handed control back to the peer
    // that was driving it (RETURN). Used to send the clipboard along. Same threading rules as above.
    std::function<void(int peerId)> onCursorToPeer;

    void SetLocalMonitors(const std::vector<InputRect>& mons);   // before Start()
    bool Start(std::string* msg);                                // msg: error or warning
    void Stop();
    std::string BackendName() const;
    bool CanInject() const;                                      // can this PC be controlled? (valid after Start())

    // Layout: (ox, oy) is the peer's top-left corner relative to this PC's top-left
    // corner in canvas "world" pixels (exactly what Canvas keeps).
    void SetPeer(int id, const std::vector<InputRect>& mons, double ox, double oy);
    void SetPeerOffset(int id, double ox, double oy);
    void SetPeerAllowed(int id, bool allowed);     // may this peer control THIS PC?
    void SetPeerAccepts(int id, bool accepts);     // does this peer let THIS PC control it? (its CTRL line)
    void SetPeerRate(int id, int hz);              // keyboard/mouse updates per second sent to this peer (10..1000)
    void RemovePeer(int id);

    // Returns true if the line belonged to the input protocol. Call from the network thread.
    bool HandleMessage(int peerId, const std::string& line);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
