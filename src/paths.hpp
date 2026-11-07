#pragma once
#include <filesystem>
#include <string>

namespace paths {

// Per-user data directory shared by the GUI and the daemon (identity, configs, trust, group, ipc token).
//   Windows: %APPDATA%\seamless-input        Linux: $XDG_DATA_HOME/seamless-input (~/.local/share/...)
std::filesystem::path DataDir();

std::filesystem::path ExePath();      // this executable
std::filesystem::path DaemonExe();    // seamless-input-daemon(.exe) next to this executable
std::string HostName();

} // namespace paths
