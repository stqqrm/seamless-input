#pragma once
// One place for the version shown in the window title and the startup log. The daemon reports its own
// version to the window, so an OLD daemon that is still running (it keeps running after an update until
// it is stopped) is visible at a glance. Bump kBuild whenever a change must reach both PCs.
#include <string>

namespace appver {

inline constexpr const char* kVersion = "1.0.0";
inline constexpr int kBuild = 6;

inline std::string Full() { return std::string(kVersion) + " (build " + std::to_string(kBuild) + ")"; }

} // namespace appver
