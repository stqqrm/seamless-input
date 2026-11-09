#pragma once
// One place for the version shown in the window title, the startup log and the version handshake
// between PCs. The daemon reports its own version to the window, so an OLD daemon that is still
// running (it keeps running after an update until it is stopped) is visible at a glance.
//
// The build id is the hash of everything under src/ (cmake/BuildId.cmake writes it into a generated
// header on every build): no number to bump, any code change gives a new id. Built without CMake
// (a quick g++ run) there is no generated header and the id is "dev".
#include <string>

#if defined(__has_include)
  #if __has_include("seamless_build_id.hpp")
    #include "seamless_build_id.hpp"
  #endif
#endif
#ifndef SI_BUILD_ID
  #define SI_BUILD_ID "dev"
#endif

namespace appver {

inline constexpr const char* kVersion = "1.0.0";
inline constexpr const char* kBuildId = SI_BUILD_ID;       // 8 hex digits

inline std::string Full() { return std::string(kVersion) + " (build " + kBuildId + ")"; }

} // namespace appver
