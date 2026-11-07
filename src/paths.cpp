#include "paths.hpp"

#include <cstdlib>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <shlobj.h>
#else
  #include <limits.h>
  #include <pwd.h>
  #include <unistd.h>
#endif

namespace paths {

std::filesystem::path DataDir()
{
#if defined(_WIN32)
    PWSTR p = nullptr;
    std::filesystem::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p) base = p;
    if (p) CoTaskMemFree(p);
    if (base.empty()) {
        const wchar_t* ad = _wgetenv(L"APPDATA");
        base = ad ? ad : L".";
    }
    return base / "seamless-input";
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) return std::filesystem::path(xdg) / "seamless-input";
    const char* home = std::getenv("HOME");
    std::string h = home && *home ? home : "";
    if (h.empty()) {
        if (const passwd* pw = getpwuid(getuid())) h = pw->pw_dir;
    }
    if (h.empty()) h = ".";
    return std::filesystem::path(h) / ".local/share/seamless-input";
#endif
}

std::filesystem::path ExePath()
{
#if defined(_WIN32)
    std::wstring buf(1024, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return {};
        if (n < buf.size()) { buf.resize(n); break; }
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::path(buf);
#else
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return std::filesystem::path(buf);
#endif
}

std::filesystem::path DaemonExe()
{
    const std::filesystem::path self = ExePath();
#if defined(_WIN32)
    return self.parent_path() / "seamless-input-daemon.exe";
#else
    return self.parent_path() / "seamless-input-daemon";
#endif
}

std::string HostName()
{
#if defined(_WIN32)
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(buf, &n) && n > 0) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, nullptr, 0, nullptr, nullptr);
        std::string s((size_t)std::max(len, 0), '\0');
        if (len > 0) WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, s.data(), len, nullptr, nullptr);
        return s;
    }
    return "windows-pc";
#else
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0]) return buf;
    return "linux-pc";
#endif
}

} // namespace paths
