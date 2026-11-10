#pragma once
// Running the daemon in the background and starting it automatically.
//
//   Linux    a systemd USER service (~/.config/systemd/user/seamless-input.service). It must run in
//            the user's session: keyboard/mouse (X11 or /dev/input) and audio (PipeWire/Pulse) belong
//            to that session. Without systemd: an XDG autostart entry.
//   Windows  a real Windows service ("SeamlessInputService"). Services live in session 0 and cannot
//            see the desktop, so the service only SUPERVISES: it starts the worker
//            (seamless-input-daemon.exe --run --worker) inside the logged-in user's session
//            and restarts it when it dies or the user changes.
#include <filesystem>
#include <functional>
#include <string>

namespace svc {

struct AutostartInfo {
    bool supported = false;       // can autostart be configured on this system?
    bool enabled = false;         // will the daemon start on its own at login / boot?
    bool managerRunning = false;  // the service / unit itself reports "running"
    bool installed = false;       // Windows: the service exists (always the case once a window has run; autostart only
                                  // switches it between automatic and manual start)
    bool current = true;          // Windows: the installed service points at THIS build's daemon exe
    std::string kind;             // "systemd user service" | "XDG autostart entry" | "Windows service"
};

AutostartInfo QueryAutostart();
bool EnableAutostart(std::string* err);     // Windows: asks for administrator permission (UAC prompt)
bool DisableAutostart(std::string* err);    // Windows: same

// Asks systemd / the Windows service manager to start the daemon (only when autostart is enabled).
bool StartManaged(std::string* err);
// Starts the daemon directly, detached from the caller (it survives the caller).
bool SpawnDetached(std::string* err);

// Makes sure a daemon answers on the IPC port: managed start when autostart is on, else a direct
// spawn. Blocks up to waitMs. Safe to call when one already runs.
bool EnsureDaemon(const std::filesystem::path& dataDir, std::string* err, int waitMs = 8000);
// Force-stops every seamless-input-daemon process of this user (SIGTERM, then SIGKILL on Linux;
// taskkill on Windows). For a daemon that does not answer: an older version, or a hung one.
// Returns how many processes were signalled (-1: unknown).
int KillDaemons();
// Waits until no daemon answers any more (after RequestStop).
bool WaitGone(const std::filesystem::path& dataDir, int waitMs);

// Windows worker only (--worker): calls onStop when the service asks the worker to stop. No-op elsewhere.
void WatchWorkerStop(std::function<void()> onStop);

#if defined(_WIN32)
// Windows: the service is ALWAYS installed (it is what gives the worker the rights to control elevated windows
// and the UAC prompt). Installs it when missing or when it points at another exe (one UAC prompt), and clears a
// stray user-level daemon that would keep the service's worker from binding the port. Asks at most once per run.
bool PrepareManaged(const std::filesystem::path& dataDir, std::string* err);
// Windows: re-points the service at this build's daemon exe and restarts it (one UAC prompt): the answer to
// "versions do not match".
bool ReplaceService(std::string* err);
// Windows: true when the daemon exe sits where ordinary users can write (the service runs it as SYSTEM).
bool InstallLocationUnsafe();

// mode: 0 = keep the current start type (manual for a new service), 1 = automatic, 2 = manual.
// restart: stop the service first and start it again with the new settings.
int InstallService(int mode, bool restart);   // --install-service [--auto|--manual|--keep] [--restart]  (administrator)
int RunServiceHost();        // --service  (called by the service control manager)
int UninstallService();      // --uninstall-service  (needs administrator rights)
#endif

} // namespace svc
