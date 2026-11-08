// seamless-input-daemon: the headless engine. No window, no wxWidgets, no display needed.
//
//   --run                    run in the foreground (default)
//   --service                Windows: entry point of the service control manager
//   --install-autostart      start automatically (systemd user unit / XDG entry / Windows service)
//   --uninstall-autostart
//   --install-service / --uninstall-service     Windows only, needs administrator rights
//   --status                 is a daemon running? is autostart on?
//   --stop                   ask the running daemon to quit
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "core.hpp"
#include "ipc.hpp"
#include "ipc_server.hpp"
#include "paths.hpp"
#include "service.hpp"

#ifndef SI_VERSION
  #define SI_VERSION "dev"
#endif

namespace {

std::atomic<bool> g_quit{false};

void OnSignal(int) { g_quit = true; }

class FileLog {
public:
    void Open(const std::filesystem::path& file)
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::error_code ec;
        if (std::filesystem::exists(file, ec) && std::filesystem::file_size(file, ec) > 1024 * 1024) {
            std::filesystem::rename(file, file.string() + ".old", ec);
            if (ec) std::filesystem::remove(file, ec);
        }
        out_.open(file, std::ios::app);
    }
    void Line(const std::string& s)
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::fprintf(stderr, "%s\n", s.c_str());
        std::fflush(stderr);
        if (out_) { out_ << s << "\n"; out_.flush(); }
    }
private:
    std::mutex mu_;
    std::ofstream out_;
};

void Usage()
{
    std::puts(
        "seamless-input-daemon " SI_VERSION "\n"
        "Background engine of seamless-input (keyboard, mouse and audio sharing).\n\n"
        "  --run                  run in the foreground (default)\n"
        "  --install-autostart    start automatically at login / boot\n"
        "  --uninstall-autostart\n"
        "  --status               show whether the daemon runs and whether autostart is on\n"
        "  --stop                 ask the running daemon to quit\n"
#if defined(_WIN32)
        "  --install-service      install + start the Windows service (administrator)\n"
        "  --uninstall-service    remove the Windows service (administrator)\n"
        "  --service              (used by the service control manager)\n"
#endif
    );
}

int Run(bool worker)
{
    const auto dir = paths::DataDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    FileLog log;
    log.Open(dir / "daemon.log");
    log.Line(std::string("seamless-input-daemon ") + SI_VERSION + " starting");

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    if (worker) svc::WatchWorkerStop([] { g_quit = true; });

    Core core(dir);
    core.onLog = [&log](const std::string& l) { log.Line(l); };
    core.onQuit = [] { g_quit = true; };

    // The IPC port doubles as the single-instance lock: bind it before touching any engine.
    ipc::Server server(core, dir);
    std::string err;
    if (!server.Start(&err)) {
        if (server.AddressInUse()) {
            // The port is the single-instance lock, but "taken" does not mean "a working daemon": a hung
            // daemon or an unrelated program on the same port looks identical until we ask it.
            if (ipc::Ping(dir, 1500)) { log.Line("A daemon is already running - exiting"); return 0; }
            log.Line("Port " + std::to_string((int)ipc::kPort) + " is in use, but what listens there does not answer "
                     "(a hung old daemon or another program). Find it with: ss -ltnp 'sport = :" +
                     std::to_string((int)ipc::kPort) + "'  and stop it, then start again.");
            return 1;
        }
        log.Line("Cannot open the local control port: " + err);
        return 1;
    }
    core.Start(&err);

    while (!g_quit) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    log.Line("Stopping");
    server.Stop();
    core.Stop();
    return 0;
}

int Status()
{
    const auto dir = paths::DataDir();
    const bool up = ipc::Ping(dir);
    const auto a = svc::QueryAutostart();
    std::printf("daemon:    %s\n", up ? "running" : "not running");
    std::printf("autostart: %s (%s)\n", a.enabled ? "on" : "off", a.kind.c_str());
    return up ? 0 : 3;
}

} // namespace

int main(int argc, char** argv)
{
    bool worker = false;
    std::string mode = "--run";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--worker") worker = true;
        else if (a == "--help" || a == "-h") { Usage(); return 0; }
        else if (a == "--version") { std::puts(SI_VERSION); return 0; }
        else mode = a;
    }

    if (mode == "--run") return Run(worker);
    if (mode == "--status") return Status();
    if (mode == "--stop") {
        if (!ipc::RequestStop(paths::DataDir())) { std::puts("No daemon is running."); return 3; }
        std::puts("Stop requested.");
        return 0;
    }
    if (mode == "--install-autostart" || mode == "--uninstall-autostart") {
        std::string err;
        const bool on = mode == "--install-autostart";
        if (!(on ? svc::EnableAutostart(&err) : svc::DisableAutostart(&err))) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        if (on) {
            std::string e2;
            svc::StartManaged(&e2);
            std::puts("Autostart enabled.");
        } else {
            std::puts("Autostart disabled.");
        }
        return 0;
    }
#if defined(_WIN32)
    if (mode == "--service") return svc::RunServiceHost();
    if (mode == "--install-service") return svc::InstallService();
    if (mode == "--uninstall-service") return svc::UninstallService();
#endif
    Usage();
    return 1;
}
