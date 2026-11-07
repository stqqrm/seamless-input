#include "service.hpp"

#include <chrono>
#include <thread>

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

bool WaitGone(const std::filesystem::path& dataDir, int waitMs)
{
    return WaitFor([&] { return !ipc::Ping(dataDir, 300); }, waitMs);
}

} // namespace svc
