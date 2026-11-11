#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>

#include "ipc.hpp"
#include "paths.hpp"
#include "service.hpp"

namespace svc {

namespace {

constexpr wchar_t kName[]      = L"SeamlessInputService";
constexpr wchar_t kDisplay[]   = L"Seamless Input";
constexpr wchar_t kStopEvent[] = L"Global\\SeamlessInputWorkerStop";
constexpr DWORD   kCtlWake     = 128;      // user-defined control: "start the worker now"

// Local users may query/start/stop the service and send kCtlWake (so the GUI needs no UAC prompt
// for that); only administrators may change or delete it. kCtlWake only makes the service (re)start
// the one worker executable it was installed with. The worker runs as SYSTEM, so that executable MUST
// sit in a folder ordinary users cannot write to (InstallService warns when it does not).
constexpr wchar_t kServiceSddl[] =
    L"D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)"
    L"(A;;CCLCSWRPWPLOCRRC;;;IU)(A;;CCLCSWLOCRRC;;;SU)";
constexpr wchar_t kEventSddl[] = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100000;;;IU)";   // IU: SYNCHRONIZE only

std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)std::max(n, 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string ErrText(DWORD e) { return "Windows error " + std::to_string(e); }

void SvcLog(const std::string& msg)
{
    PWSTR pd = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &pd)) || !pd) return;
    std::filesystem::path p = std::filesystem::path(pd) / "seamless-input-service.log";
    CoTaskMemFree(pd);
    std::error_code ec;
    if (std::filesystem::exists(p, ec) && std::filesystem::file_size(p, ec) > 512 * 1024) std::filesystem::remove(p, ec);
    std::ofstream out(p, std::ios::app);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char ts[32];
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    out << ts << msg << "\n";
}

struct ScHandle {
    SC_HANDLE h = nullptr;
    ScHandle() = default;
    explicit ScHandle(SC_HANDLE x) : h(x) {}
    ~ScHandle() { if (h) CloseServiceHandle(h); }
    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

// Runs the daemon exe elevated (UAC prompt) with one argument and waits for its exit code.
bool RunElevated(const wchar_t* arg, std::string* err)
{
    const std::wstring exe = paths::DaemonExe().wstring();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = arg;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        if (err) *err = GetLastError() == ERROR_CANCELLED ? "Administrator permission was not granted"
                                                           : "Could not start the installer: " + ErrText(GetLastError());
        return false;
    }
    WaitForSingleObject(sei.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (code != 0) {
        if (err) *err = "The service change failed (exit code " + std::to_string(code) +
                        "). Run \"seamless-input-daemon.exe " + Narrow(arg) + "\" from an administrator prompt to see why.";
        return false;
    }
    return true;
}

// The service and its worker run as SYSTEM. If a normal user can replace the executable (for example it sits
// in Downloads or on the Desktop), that user could run code as SYSTEM through it.
bool UnderProgramFiles(const std::filesystem::path& exe)
{
    std::error_code ec;
    const std::wstring e = std::filesystem::weakly_canonical(exe, ec).wstring();
    for (const KNOWNFOLDERID* id : {&FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86}) {
        PWSTR pf = nullptr;
        std::wstring rs;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, 0, nullptr, &pf)) && pf) rs = pf;
        if (pf) CoTaskMemFree(pf);
        if (!rs.empty() && e.size() > rs.size() && _wcsnicmp(e.c_str(), rs.c_str(), rs.size()) == 0) return true;
    }
    return false;
}

void WarnIfUserWritable(const std::filesystem::path& exe)
{
    if (UnderProgramFiles(exe)) return;
    std::fprintf(stderr,
        "WARNING: the service runs as SYSTEM, but %ls is not under Program Files. Any user who can write to its\n"
        "folder could run code as SYSTEM through it. Put the program in a folder only administrators can write to.\n",
        exe.c_str());
}

std::wstring ServiceBinPath() { return L"\"" + paths::DaemonExe().wstring() + L"\" --service"; }

// ------------------------------------------------------------------ supervisor
SERVICE_STATUS_HANDLE g_status = nullptr;
HANDLE g_stop = nullptr;       // service is stopping
HANDLE g_wake = nullptr;       // re-evaluate: session changed / kCtlWake

void Report(DWORD state, DWORD waitHintMs = 0)
{
    static DWORD checkpoint = 1;
    SERVICE_STATUS st{};
    st.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    st.dwCurrentState = state;
    st.dwControlsAccepted = state == SERVICE_START_PENDING ? 0
        : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE;
    st.dwWaitHint = waitHintMs;
    st.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    SetServiceStatus(g_status, &st);
}

DWORD WINAPI CtrlHandler(DWORD ctl, DWORD type, LPVOID, LPVOID)
{
    switch (ctl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        Report(SERVICE_STOP_PENDING, 15000);
        SetEvent(g_stop);
        return NO_ERROR;
    case SERVICE_CONTROL_SESSIONCHANGE:
        if (type != WTS_SESSION_LOCK && type != WTS_SESSION_UNLOCK && type != WTS_SESSION_REMOTE_CONTROL) SetEvent(g_wake);
        return NO_ERROR;
    case kCtlWake:
        SetEvent(g_wake);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

struct Worker {
    HANDLE proc = nullptr;
    DWORD session = 0xFFFFFFFF;
    ULONGLONG started = 0;
};

// Why SYSTEM and not the user's own token: a hook and SendInput of a normal-integrity process are blocked by
// UIPI whenever an elevated window (Task Manager, an admin console, ...) is under the pointer or has the focus,
// and the UAC prompt lives on the secure desktop, which only SYSTEM may open. So the worker gets this service's
// own (SYSTEM) token, moved into the user's session, together with the user's environment block (profile paths).
HANDLE MakeSystemTokenForSession(DWORD sid)
{
    HANDLE self = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID, &self))
        return nullptr;
    HANDLE prim = nullptr;
    const BOOL dup = DuplicateTokenEx(self, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &prim);
    CloseHandle(self);
    if (!dup) return nullptr;
    if (!SetTokenInformation(prim, TokenSessionId, &sid, sizeof(sid))) {
        SvcLog("SetTokenInformation(TokenSessionId) failed: " + ErrText(GetLastError()));
        CloseHandle(prim);
        return nullptr;
    }
    return prim;
}

bool LaunchWorker(DWORD sid, Worker& w)
{
    HANDLE userTok = nullptr;
    if (!WTSQueryUserToken(sid, &userTok)) return false;   // nobody logged on yet

    LPVOID env = nullptr;
    if (!CreateEnvironmentBlock(&env, userTok, FALSE)) env = nullptr;

    // 1st choice: SYSTEM in the user's session. Fallback: the user's own token (then elevated windows and UAC
    // cannot be controlled, but everything else works).
    HANDLE prim = MakeSystemTokenForSession(sid);
    bool asSystem = prim != nullptr;
    if (!prim) {
        SvcLog("could not build a SYSTEM token for the worker - falling back to the user token");
        if (!DuplicateTokenEx(userTok, MAXIMUM_ALLOWED, nullptr, SecurityIdentification, TokenPrimary, &prim)) prim = nullptr;
    }
    CloseHandle(userTok);
    if (!prim) { if (env) DestroyEnvironmentBlock(env); return false; }

    const std::wstring exe = paths::DaemonExe().wstring();
    std::wstring cmd = L"\"" + exe + L"\" --run --worker";
    std::wstring dir = paths::DaemonExe().parent_path().wstring();
    wchar_t desktop[] = L"winsta0\\default";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = desktop;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessAsUserW(prim, exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                   CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env, dir.c_str(), &si, &pi);
    DWORD err = ok ? 0 : GetLastError();
    CloseHandle(prim);
    if (!ok && asSystem) {
        SvcLog("CreateProcessAsUser (SYSTEM) failed: " + ErrText(err) + " - falling back to the user token");
        asSystem = false;
        HANDLE ut = nullptr;
        HANDLE up = nullptr;
        if (WTSQueryUserToken(sid, &ut)) {
            if (DuplicateTokenEx(ut, MAXIMUM_ALLOWED, nullptr, SecurityIdentification, TokenPrimary, &up)) {
                ok = CreateProcessAsUserW(up, exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                          CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env, dir.c_str(), &si, &pi);
                err = ok ? 0 : GetLastError();
                CloseHandle(up);
            }
            CloseHandle(ut);
        }
    }
    if (env) DestroyEnvironmentBlock(env);
    if (!ok) { SvcLog("CreateProcessAsUser failed: " + ErrText(err)); return false; }
    CloseHandle(pi.hThread);
    w.proc = pi.hProcess;
    w.session = sid;
    w.started = GetTickCount64();
    SvcLog("worker started in session " + std::to_string(sid) + " (pid " + std::to_string(pi.dwProcessId) + ", " +
           (asSystem ? "SYSTEM: elevated windows and UAC can be controlled" : "user rights: elevated windows and UAC cannot be controlled") + ")");
    return true;
}

void StopWorker(Worker& w, HANDLE stopEvent)
{
    if (!w.proc) return;
    SetEvent(stopEvent);                                    // graceful: the worker watches this event
    if (WaitForSingleObject(w.proc, 8000) != WAIT_OBJECT_0) {
        SvcLog("worker did not stop in time - terminating");
        TerminateProcess(w.proc, 1);
    }
    CloseHandle(w.proc);
    w = {};
}

void Supervise()
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    PSECURITY_DESCRIPTOR psd = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(kEventSddl, SDDL_REVISION_1, &psd, nullptr);
    sa.lpSecurityDescriptor = psd;
    HANDLE stopEvent = CreateEventW(&sa, TRUE, FALSE, kStopEvent);   // manual reset
    if (psd) LocalFree(psd);
    if (!stopEvent) { SvcLog("cannot create the worker stop event: " + ErrText(GetLastError())); return; }

    Worker w;
    bool idle = false;                 // the worker ended on purpose: stay quiet until the next session event
    int fast = 0;
    ULONGLONG nextTry = 0;

    for (;;) {
        if (WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0) break;
        const DWORD active = WTSGetActiveConsoleSessionId();

        if (w.proc && w.session != active) StopWorker(w, stopEvent);     // user switched / logged off

        if (!w.proc && !idle && active != 0xFFFFFFFF && GetTickCount64() >= nextTry) {
            ResetEvent(stopEvent);
            if (!LaunchWorker(active, w)) nextTry = GetTickCount64() + 5000;
        }

        HANDLE h[3];
        DWORD n = 0;
        h[n++] = g_stop;
        h[n++] = g_wake;
        if (w.proc) h[n++] = w.proc;
        const DWORD r = WaitForMultipleObjects(n, h, FALSE, w.proc ? INFINITE : 5000);

        if (r == WAIT_OBJECT_0) break;
        if (r == WAIT_OBJECT_0 + 1) { idle = false; continue; }
        if (w.proc && r == WAIT_OBJECT_0 + 2) {
            DWORD code = 0;
            GetExitCodeProcess(w.proc, &code);
            const ULONGLONG up = GetTickCount64() - w.started;
            CloseHandle(w.proc);
            w = {};
            SvcLog("worker exited with code " + std::to_string(code));
            if (code == 0) {
                idle = true;           // asked to stop (e.g. from the window)
            } else {
                fast = up < 15000 ? fast + 1 : 0;
                nextTry = GetTickCount64() + std::min<ULONGLONG>(30000, 2000ULL * (fast + 1));
            }
        }
    }
    StopWorker(w, stopEvent);
    CloseHandle(stopEvent);
}

void WINAPI ServiceMain(DWORD, LPWSTR*)
{
    g_status = RegisterServiceCtrlHandlerExW(kName, CtrlHandler, nullptr);
    if (!g_status) return;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Report(SERVICE_START_PENDING, 5000);
    Report(SERVICE_RUNNING);
    SvcLog("service started");
    Supervise();
    SvcLog("service stopped");
    Report(SERVICE_STOPPED);
    CloseHandle(g_stop);
    CloseHandle(g_wake);
}

bool StopServiceAndWait(SC_HANDLE sv, int waitMs)
{
    SERVICE_STATUS st{};
    ControlService(sv, SERVICE_CONTROL_STOP, &st);
    const ULONGLONG end = GetTickCount64() + (ULONGLONG)waitMs;
    while (QueryServiceStatus(sv, &st) && st.dwCurrentState != SERVICE_STOPPED && GetTickCount64() < end) Sleep(200);
    return st.dwCurrentState == SERVICE_STOPPED;
}

} // namespace

// ------------------------------------------------------------------ public API
AutostartInfo QueryAutostart()
{
    AutostartInfo i;
    i.supported = true;
    i.kind = "Windows service";
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) return i;
    ScHandle sv(OpenServiceW(scm.h, kName, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
    if (!sv) return i;
    i.installed = true;

    SERVICE_STATUS_PROCESS ssp{};
    DWORD need = 0;
    if (QueryServiceStatusEx(sv.h, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &need))
        i.managerRunning = ssp.dwCurrentState == SERVICE_RUNNING;

    QueryServiceConfigW(sv.h, nullptr, 0, &need);
    std::vector<BYTE> buf(need ? need : 1);
    auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
    if (QueryServiceConfigW(sv.h, cfg, (DWORD)buf.size(), &need)) {
        i.enabled = cfg->dwStartType == SERVICE_AUTO_START;
        i.current = cfg->lpBinaryPathName && _wcsicmp(cfg->lpBinaryPathName, ServiceBinPath().c_str()) == 0;
    }
    return i;
}

// The service stays installed either way: "autostart" only switches it between automatic and manual start.
bool EnableAutostart(std::string* err) { return RunElevated(L"--install-service --auto", err); }
bool DisableAutostart(std::string* err) { return RunElevated(L"--install-service --manual", err); }

bool InstallLocationUnsafe() { return !UnderProgramFiles(paths::DaemonExe()); }

namespace {
// A daemon started by a window without the service (an older version, or the fallback) holds the port, so the
// service's worker could never start: ask it to quit, then force.
void ClearStrayDaemons(const std::filesystem::path& dataDir)
{
    if (!ipc::Ping(dataDir, 500)) return;
    ipc::RequestStop(dataDir);
    WaitGone(dataDir, 1500);
    KillDaemons();
    WaitGone(dataDir, 2000);
}
std::atomic<bool> g_installTried{false};
} // namespace

namespace {
std::filesystem::path DeclinedMarker(const std::filesystem::path& dataDir) { return dataDir / "service-uninstalled"; }
bool Declined(const std::filesystem::path& dataDir)
{
    std::error_code ec;
    return std::filesystem::exists(DeclinedMarker(dataDir), ec);
}
} // namespace

bool PrepareManaged(const std::filesystem::path& dataDir, std::string* err)
{
    AutostartInfo a = QueryAutostart();
    if (!a.installed && Declined(dataDir)) return false;            // the user removed it on purpose: do not nag
    if (!a.installed || !a.current) {
        if (g_installTried.exchange(true)) return a.installed;      // declined or failed once: do not nag again
        ClearStrayDaemons(dataDir);
        if (!RunElevated(L"--install-service --keep", err)) return false;
        return true;
    }
    if (!a.managerRunning && ipc::Ping(dataDir, 500)) ClearStrayDaemons(dataDir);
    return true;
}

bool InstallServiceNow(const std::filesystem::path& dataDir, std::string* err)
{
    std::error_code ec;
    std::filesystem::remove(DeclinedMarker(dataDir), ec);
    ClearStrayDaemons(dataDir);                                     // a user-level daemon would hold the port
    return RunElevated(L"--install-service --keep", err);
}

bool UninstallServiceNow(const std::filesystem::path& dataDir, std::string* err)
{
    // Marker first: from here on nothing may start the daemon again (see EnsureDaemon).
    std::error_code ec;
    std::filesystem::create_directories(dataDir, ec);
    std::ofstream(DeclinedMarker(dataDir)) << "the service was removed from the Service menu\n";
    if (!RunElevated(L"--uninstall-service", err)) {
        std::filesystem::remove(DeclinedMarker(dataDir), ec);       // nothing was removed
        return false;
    }
    // Close every daemon that is still running (the elevated step already ended the service's own).
    if (ipc::Ping(dataDir, 500)) { ipc::RequestStop(dataDir); WaitGone(dataDir, 1500); }
    KillDaemons();
    WaitGone(dataDir, 2000);
    return true;
}

bool StopManaged(const std::filesystem::path&, std::string* err)
{
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    ScHandle sv(scm ? OpenServiceW(scm.h, kName, SERVICE_STOP | SERVICE_QUERY_STATUS) : nullptr);
    if (!sv) { if (err) *err = "the service is not installed or not accessible"; return false; }
    if (!StopServiceAndWait(sv.h, 15000)) { if (err) *err = "the service did not stop in time"; return false; }
    return true;
}

bool ReplaceService(std::string* err)
{
    ClearStrayDaemons(paths::DataDir());
    return RunElevated(L"--install-service --keep --restart", err);
}

bool StartManaged(std::string* err)
{
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    ScHandle sv(scm ? OpenServiceW(scm.h, kName, SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_USER_DEFINED_CONTROL) : nullptr);
    if (!sv) { if (err) *err = "the service is not installed or not accessible"; return false; }

    SERVICE_STATUS_PROCESS ssp{};
    DWORD need = 0;
    if (!QueryServiceStatusEx(sv.h, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &need)) return false;
    if (ssp.dwCurrentState == SERVICE_STOPPED) {
        if (StartServiceW(sv.h, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) return true;
        if (err) *err = "could not start the service: " + ErrText(GetLastError());
        return false;
    }
    SERVICE_STATUS st{};
    return ControlService(sv.h, kCtlWake, &st) != 0;     // running but idle: start the worker now
}

bool SpawnDetached(std::string* err)
{
    const std::wstring exe = paths::DaemonExe().wstring();
    std::error_code ec;
    if (!std::filesystem::exists(exe, ec)) {
        if (err) *err = Narrow(exe) + " not found (it must sit next to the GUI executable)";
        return false;
    }
    std::wstring cmd = L"\"" + exe + L"\" --run";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                        nullptr, nullptr, &si, &pi)) {
        if (err) *err = "could not start the daemon: " + ErrText(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void WatchWorkerStop(std::function<void()> onStop)
{
    std::thread([onStop = std::move(onStop)] {
        HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, kStopEvent);
        if (!ev) return;                                   // not started by the service
        WaitForSingleObject(ev, INFINITE);
        CloseHandle(ev);
        onStop();
    }).detach();
}

int RunServiceHost()
{
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kName), ServiceMain}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        std::fprintf(stderr, "This mode is started by the Windows service manager (error %lu).\n", GetLastError());
        return 2;
    }
    return 0;
}

int InstallService(int mode, bool restart)
{
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    if (!scm) { std::fprintf(stderr, "Cannot open the service manager (run as administrator): %lu\n", GetLastError()); return 1; }

    const std::wstring bin = ServiceBinPath();
    WarnIfUserWritable(paths::DaemonExe());
    DWORD startType = mode == 1 ? SERVICE_AUTO_START : SERVICE_DEMAND_START;     // a new service starts manually
    ScHandle sv(OpenServiceW(scm.h, kName, SERVICE_ALL_ACCESS));
    if (sv) {
        DWORD need = 0;
        QueryServiceConfigW(sv.h, nullptr, 0, &need);
        std::vector<BYTE> buf(need ? need : 1);
        auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
        bool samePath = false;
        if (QueryServiceConfigW(sv.h, cfg, (DWORD)buf.size(), &need)) {
            samePath = cfg->lpBinaryPathName && _wcsicmp(cfg->lpBinaryPathName, bin.c_str()) == 0;
            if (mode == 0) startType = cfg->dwStartType;                        // keep what is configured
        }
        // another exe (or an explicit restart): the running service still executes the old file - stop it first
        if (!samePath || restart) StopServiceAndWait(sv.h, 15000);
        if (!ChangeServiceConfigW(sv.h, SERVICE_WIN32_OWN_PROCESS, startType, SERVICE_ERROR_NORMAL, bin.c_str(),
                                  nullptr, nullptr, nullptr, nullptr, nullptr, kDisplay)) {
            std::fprintf(stderr, "ChangeServiceConfig failed: %lu\n", GetLastError());
            return 1;
        }
    } else {
        sv.h = CreateServiceW(scm.h, kName, kDisplay, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, startType,
                              SERVICE_ERROR_NORMAL, bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!sv) { std::fprintf(stderr, "CreateService failed: %lu\n", GetLastError()); return 1; }
    }

    wchar_t desc[] = L"Shares keyboard, mouse and audio between your PCs (runs seamless-input in the background).";
    SERVICE_DESCRIPTIONW sd{desc};
    ChangeServiceConfig2W(sv.h, SERVICE_CONFIG_DESCRIPTION, &sd);

    SC_ACTION acts[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 30000}};
    SERVICE_FAILURE_ACTIONSW fa{};
    fa.dwResetPeriod = 86400;
    fa.cActions = 3;
    fa.lpsaActions = acts;
    ChangeServiceConfig2W(sv.h, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    PSECURITY_DESCRIPTOR psd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(kServiceSddl, SDDL_REVISION_1, &psd, nullptr)) {
        SetServiceObjectSecurity(sv.h, DACL_SECURITY_INFORMATION, psd);
        LocalFree(psd);
    }

    if (!StartServiceW(sv.h, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        std::fprintf(stderr, "Installed, but could not start: %lu\n", GetLastError());
        return 1;
    }
    std::printf("Service \"%ls\" installed (%s start) and running.\n", kName,
                startType == SERVICE_AUTO_START ? "automatic" : "manual");
    return 0;
}

int UninstallService()
{
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) { std::fprintf(stderr, "Cannot open the service manager (run as administrator): %lu\n", GetLastError()); return 1; }
    ScHandle sv(OpenServiceW(scm.h, kName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE));
    if (!sv) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) { std::printf("Service is not installed.\n"); return 0; }
        std::fprintf(stderr, "Cannot open the service: %lu\n", GetLastError());
        return 1;
    }
    StopServiceAndWait(sv.h, 15000);
    KillDaemons();                                  // elevated: also ends a worker the stop did not reach
    if (!DeleteService(sv.h) && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE) {
        std::fprintf(stderr, "DeleteService failed: %lu\n", GetLastError());
        return 1;
    }
    std::printf("Service removed.\n");
    return 0;
}

} // namespace svc

#endif // _WIN32
