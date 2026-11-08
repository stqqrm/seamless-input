#include "service.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#include <signal.h>
#include <unistd.h>
#include <cctype>
#endif

#include "ipc.hpp"

namespace svc {

static bool WaitFor(const std::function<bool()>& pred, int waitMs)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    for (;;) {
        if (pred()) return true;
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
}

bool EnsureDaemon(const std::filesystem::path& dataDir, std::string* err, int waitMs)
{
    auto up = [&] { return ipc::Ping(dataDir, 500); };
    if (up()) return true;

    std::string why;
    if (QueryAutostart().enabled && StartManaged(&why) && WaitFor(up, std::min(waitMs / 2, 4000))) return true;

    if (!SpawnDetached(&why)) {
        if (err) *err = why;
        return false;
    }
    if (WaitFor(up, waitMs)) return true;
    if (err) *err = "the daemon did not start (see daemon.log in " + dataDir.string() + ")";
    return false;
}

int KillDaemons()
{
#if defined(__linux__)
    std::vector<pid_t> pids;
    const pid_t self = getpid();
    if (DIR* d = opendir("/proc")) {
        while (dirent* e = readdir(d)) {
            if (!std::isdigit(static_cast<unsigned char>(e->d_name[0]))) continue;
            const pid_t pid = (pid_t)std::atoi(e->d_name);
            if (pid <= 1 || pid == self) continue;
            std::ifstream in(std::string("/proc/") + e->d_name + "/cmdline", std::ios::binary);
            std::string arg0;
            std::getline(in, arg0, '\0');                         // argv[0]: still right when the binary was replaced
            const auto slash = arg0.find_last_of('/');
            const std::string base = slash == std::string::npos ? arg0 : arg0.substr(slash + 1);
            if (base == "seamless-input-daemon") pids.push_back(pid);
        }
        closedir(d);
    }
    for (pid_t p : pids) kill(p, SIGTERM);                         // processes of other users: refused, ignored
    for (int i = 0; i < 15; ++i) {
        bool alive = false;
        for (pid_t p : pids) if (kill(p, 0) == 0) alive = true;
        if (!alive) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    for (pid_t p : pids) if (kill(p, 0) == 0) kill(p, SIGKILL);
    return (int)pids.size();
#elif defined(_WIN32)
    // The service supervisor (if installed) starts a fresh worker afterwards.
    std::system("taskkill /F /IM seamless-input-daemon.exe >nul 2>&1");
    return -1;
#else
    return 0;
#endif
}

bool WaitGone(const std::filesystem::path& dataDir, int waitMs)
{
    return WaitFor([&] { return !ipc::Ping(dataDir, 300); }, waitMs);
}

} // namespace svc
