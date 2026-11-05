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
//                PC's own mouse is moved, control goes back too (TAKE).
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
//   TAKE           peer -> controller   the peer's user moved their own mouse: control is theirs again
//   DENY           peer -> controller   refused / revoked
//
// SECURITY: injecting input is remote control of the PC. A peer may only control
// this PC after SetPeerAllowed(id, true); until then ENTER is answered with DENY
// and onControlRequest fires once so the UI can ask the user.
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
    std::function<void(int peerId)> onControlRequest;

    void SetLocalMonitors(const std::vector<InputRect>& mons);   // before Start()
    bool Start(std::string* msg);                                // msg: error or warning
    void Stop();
    std::string BackendName() const;

    void SetEnabled(bool on);   // master switch; off = return to Local and refuse control

    // Layout: (ox, oy) is the peer's top-left corner relative to this PC's top-left
    // corner in canvas "world" pixels (exactly what Canvas keeps).
    void SetPeer(int id, const std::vector<InputRect>& mons, double ox, double oy);
    void SetPeerOffset(int id, double ox, double oy);
    void SetPeerAllowed(int id, bool allowed);
    void RemovePeer(int id);

    // Returns true if the line belonged to the input protocol. Call from the network thread.
    bool HandleMessage(int peerId, const std::string& line);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
