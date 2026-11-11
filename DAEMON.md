# Background daemon / service

seamless-input is two programs:

| Program | Role |
|---|---|
| `seamless-input-daemon` | Headless engine: network, keyboard/mouse, audio, layout, saved configs, trust. No window, no wxWidgets, no display needed. |
| `seamless-input` | The window. A client of the daemon over local IPC. **Closing it does not stop sharing.** |

Starting the window starts the daemon if it is not running. Keep both executables in the same folder.

## Service / Daemon menu

The menu is called **Service** on Windows and **Daemon** on Linux.

| Item | Windows (*Service* menu) | Linux (*Daemon* menu) |
|---|---|---|
| Install / Uninstall | installs or removes the Windows service (one UAC prompt) | - |
| Enable / Disable | *Start service with Windows* (greyed out while the service is not installed) | *Autostart at login* |
| Stop / Start | stops or starts the service (greyed out while not installed) | stops or starts the daemon |

Stopping keeps the daemon stopped until you press Start. On Linux nothing is ever greyed out. After *Uninstall* on
Windows the window runs a plain daemon without administrator rights (no control of elevated windows or UAC) and does not
install the service again by itself; *Install* turns that back on.

| OS | What it does |
|---|---|
| Linux | systemd **user** unit `~/.config/systemd/user/seamless-input.service` (starts with your graphical session). Without systemd: `~/.config/autostart/seamless-input-daemon.desktop`. |
| Windows | The service `SeamlessInputService` is **always installed** (the window installs it on first start, one UAC prompt). *Start service with Windows* only switches it between automatic start (at boot) and manual start (the window starts it when it opens). |

It is a *user* service on Linux because keyboard/mouse (X11 or /dev/input) and audio (PipeWire/Pulse) belong to your login session.

On Windows a service lives in Session 0 and cannot see the desktop, so the service only **supervises**: it starts
`seamless-input-daemon.exe --run --worker` inside the logged-in user's session and restarts it when needed.

If your window manager never activates `graphical-session.target` (some minimal WMs), use the XDG entry or
start the unit from your session startup script.

## Elevated windows and UAC prompts (Windows)

A normal user process cannot see input going to elevated windows (Task Manager, admin consoles) and cannot
touch the UAC prompt, which is shown on the secure desktop. So the always-installed service starts the worker with its own **SYSTEM** token inside your session (still using your profile
folder) and the worker follows the input desktop, so mouse/keyboard also work over elevated windows, the UAC
prompt, Ctrl+Alt+Del and the lock screen. Started any other way (window button, `--run` in a terminal) the worker
has your normal rights and logs a warning at start.

When the build of the window and the service differ, the window asks to replace the service: it re-points it at the
current `seamless-input-daemon.exe` and restarts it (one UAC prompt).

Because the service runs as SYSTEM, install the program in a folder only administrators can write to
(e.g. `C:\Program Files\seamless-input`); `--install-service` warns when it is not. If the linker cannot overwrite
`seamless-input-daemon.exe`, the service is running it: `net stop SeamlessInputService` first.

## Command line (headless machines)

```
seamless-input-daemon --run                 # foreground
seamless-input-daemon --install-autostart   # / --uninstall-autostart
seamless-input-daemon --status              # exit code 0 = running
seamless-input-daemon --stop
seamless-input-daemon --install-service     # Windows, administrator
seamless-input-daemon --uninstall-service
```

## Files

Per user, in `%APPDATA%\seamless-input` or `~/.local/share/seamless-input`:

- `daemon.log` (rotated at 1 MB)
- `ipc.token` (owner-only; the window uses it to talk to the daemon)
- `monitors.txt` (monitor layout learned from the window, used on Wayland)
- `clipboard.txt` (clipboard size limit and the *Clipboard telemetry* switch)
- the existing identity, configs, trust and group files

Windows service supervisor log: `%ProgramData%\seamless-input-service.log`.

## Notes

- The daemon listens only on `127.0.0.1:45457` (local IPC, token-authenticated). One daemon per user.
- Dev builds without downloading dependencies: `cmake -DSI_SYSTEM_DEPS=ON ..` (needs `libwxgtk3.2-dev libboost-dev`).

## Build id

The build id shown in the window title, the log and the version handshake between PCs is the hash of
everything under `src/` (`cmake/BuildId.cmake`, run on every build; line endings are normalised, so a
Windows and a Linux checkout of the same code agree). There is no number to bump: any code change gives a
new id. When two PCs report different ids a "versions do not match" message appears; only a different
*clipboard protocol* (`ClipShare::kProtocol`, bump it by hand when the wire format changes) switches
clipboard sharing off. Built without CMake the id is `dev`.

## Clipboard sharing and telemetry

- The clipboard is sent **when you copy**, not when the cursor crosses: a watcher thread notices the change
  (Windows: clipboard sequence number; Linux: hash of the clipboard tool's output, polled every 0.7 s while
  PCs are connected) and uploads straight to every PC that has *Share my clipboard* on.
- **Newest copy wins.** A copy on one PC cancels every upload/download still running on that PC and on the
  PCs that receive its offer; an offer older than what a PC already holds is refused (`OLDER`). Two PCs that
  copy at the same moment end up with the same clipboard. Protocol version is now 5: update all PCs.
- *Settings -> Clipboard telemetry*: a semi-transparent popup (progress bar, "Uploading" / "Downloading") in the
  top right corner of the monitor under the mouse, only on PCs that are transferring. **The daemon draws it
  itself** (`overlay.cpp`), so it also appears when the window is closed. Windows: a layered Win32 window in the
  worker's user session (clicks go through it). Linux: an X11 override-redirect window (translucent when a
  compositing window manager runs). Pure Wayland sessions have no way to show an always-on-top window; the
  daemon logs that once. Finished transfers stay visible for ~1.5 s, and the log shows how long each transfer took.

## Monitor changes

The daemon re-reads the monitors once a second (Windows, X11; the window reports them on Wayland). A change
(plug, unplug, rotate, resolution) seen twice in a row updates the layout, restarts the input edges, and is
sent to the other PCs without reconnecting; PCs that now overlap a new monitor are pushed aside.
