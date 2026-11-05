#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "input/input_types.hpp"

// Callbacks run on the backend's own thread (hook thread / X thread / evdev thread).
struct BackendSink {
    // Local mode: the pointer is now at (x, y) in desktop coordinates.
    std::function<void(int x, int y, bool buttonsDown)> onLocalMotion;
    // Forwarding mode: a physical event that was swallowed locally.
    std::function<void(const InputEvent&)> onForwarded;
};

// One implementation per platform:
//   Windows : SetWindowsHookEx (WH_MOUSE_LL / WH_KEYBOARD_LL) + SendInput
//   X11     : XInput2 raw motion + XGrabPointer/Keyboard + XTest
//   Wayland : evdev (EVIOCGRAB) + uinput; Wayland has no hook/injection API for
//             ordinary clients, so this talks to the kernel instead.
class InputBackend {
public:
    virtual ~InputBackend() = default;

    virtual const char* Name() const = 0;

    // Returns false if capture cannot work. May fill *err with a warning even on
    // success (e.g. "injection unavailable": this PC can control others but
    // cannot be controlled).
    virtual bool Start(BackendSink sink, std::string* err) = 0;
    virtual void Stop() = 0;

    // Monitor rectangles of this PC in desktop coordinates. Call before Start().
    virtual void SetGeometry(const std::vector<InputRect>& /*monitors*/) {}

    // Swallow all local keyboard/mouse input and report it through onForwarded.
    // Always called from inside onLocalMotion (the backend thread). Returns false
    // if input could not be taken (grab refused, keys held, ...).
    virtual bool BeginForwarding(int parkX, int parkY, std::string* err) = 0;

    // Give local input back and put the cursor at (x, y). Any thread.
    virtual void EndForwarding(int x, int y) = 0;

    // Injection (this PC is being controlled). Any thread.
    virtual void InjectMoveTo(int x, int y) = 0;
    virtual void InjectButton(int button, bool down) = 0;
    virtual void InjectWheel(int dx, int dy) = 0;
    virtual void InjectKey(int evdevCode, bool down) = 0;
};
