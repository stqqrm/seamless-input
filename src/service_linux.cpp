#if defined(__linux__)

#include "service.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <vector>

#include "ipc.hpp"
#include "paths.hpp"

namespace svc {

namespace {

const char* kUnit = "seamless-input.service";

std::filesystem::path ConfigHome()
{
    const char* x = std::getenv("XDG_CONFIG_HOME");
    if (x && *x) return x;
    const char* h = std::getenv("HOME");
    return std::filesystem::path(h && *h ? h : ".") / ".config";
}

std::filesystem::path UnitPath()    { return ConfigHome() / "systemd/user" / kUnit; }
std::filesystem::path DesktopPath() { return ConfigHome() / "autostart/seamless-input-daemon.desktop"; }

// Runs a program (no shell), returns its exit status (127: could not run). Output is captured when asked.
int RunCmd(const std::vector<std::string>& args, std::string* out = nullptr)
{
    int fd[2] = {-1, -1};
    if (out && pipe(fd) != 0) return 127;
    const pid_t pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        const int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 2); if (!out) dup2(nul, 1); }
        if (out) { dup2(fd[1], 1); close(fd[0]); close(fd[1]); }
        std::vector<char*> argv;
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    if (out) {
        close(fd[1]);
        char buf[512];
        ssize_t n;
        while ((n = read(fd[0], buf, sizeof(buf))) > 0) out->append(buf, (size_t)n);
        close(fd[0]);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 127;
}

bool SystemdUser() { return RunCmd({"systemctl", "--user", "show-environment"}) == 0; }

std::string UnitEscape(const std::string& s)
{
    std::string o;
    for (char c : s) {
        if (c == '%') o += "%%";
        else if (c == '$') o += "$$";
        else if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else o += c;
    }
    return o;
}

bool WriteFile(const std::filesystem::path& p, const std::string& text, std::string* err)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::trunc);
    out << text;
    out.close();
    if (!out) { if (err) *err = "cannot write " + p.string(); return false; }
    return true;
}

} // namespace

AutostartInfo QueryAutostart()
{
    AutostartInfo i;
    i.supported = true;
    if (SystemdUser()) {
        i.kind = "systemd user service";
        i.enabled = std::filesystem::exists(UnitPath()) && RunCmd({"systemctl", "--user", "is-enabled", kUnit}) == 0;
        i.managerRunning = RunCmd({"systemctl", "--user", "is-active", kUnit}) == 0;
    } else {
        i.kind = "XDG autostart entry";
        i.enabled = std::filesystem::exists(DesktopPath());
    }
    return i;
}

bool EnableAutostart(std::string* err)
{
    const auto exe = paths::DaemonExe();
    std::error_code ec;
    if (!std::filesystem::exists(exe, ec)) {
        if (err) *err = exe.string() + " not found (it must sit next to the GUI executable)";
        return false;
    }

    if (SystemdUser()) {
        const bool haveGraphical = RunCmd({"systemctl", "--user", "cat", "graphical-session.target"}) == 0;
        std::string unit =
            "[Unit]\n"
            "Description=seamless-input daemon (keyboard, mouse and audio sharing)\n"
            "After=network-online.target" + std::string(haveGraphical ? " graphical-session.target" : "") + "\n";
        if (haveGraphical) unit += "PartOf=graphical-session.target\n";
        unit +=
            "\n[Service]\n"
            "Type=simple\n"
            "ExecStart=\"" + UnitEscape(exe.string()) + "\" --run\n"
            "Restart=on-failure\n"
            "RestartSec=3\n"
            "\n[Install]\n"
            "WantedBy=" + std::string(haveGraphical ? "graphical-session.target" : "default.target") + "\n";
        if (!WriteFile(UnitPath(), unit, err)) return false;
        std::filesystem::remove(DesktopPath(), ec);
        if (RunCmd({"systemctl", "--user", "daemon-reload"}) != 0 ||
            RunCmd({"systemctl", "--user", "enable", kUnit}) != 0) {
            if (err) *err = "systemctl --user enable failed";
            return false;
        }
        return true;
    }

    const std::string desktop =
        "[Desktop Entry]\nType=Application\nName=seamless-input daemon\n"
        "Comment=Keyboard, mouse and audio sharing (background)\n"
        "Exec=\"" + exe.string() + "\" --run\nTerminal=false\nX-GNOME-Autostart-enabled=true\n";
    return WriteFile(DesktopPath(), desktop, err);
}

bool DisableAutostart(std::string* err)
{
    std::error_code ec;
    if (SystemdUser()) {
        RunCmd({"systemctl", "--user", "disable", kUnit});      // a running daemon keeps running
        std::filesystem::remove(UnitPath(), ec);
        RunCmd({"systemctl", "--user", "daemon-reload"});
    }
    std::filesystem::remove(DesktopPath(), ec);
    (void)err;
    return true;
}

bool StartManaged(std::string* err)
{
    if (!SystemdUser() || !std::filesystem::exists(UnitPath())) {
        if (err) *err = "no service manager entry";
        return false;
    }
    if (RunCmd({"systemctl", "--user", "start", kUnit}) != 0) {
        if (err) *err = "systemctl --user start failed";
        return false;
    }
    return true;
}

bool StopManaged(const std::filesystem::path& dataDir, std::string* err)
{
    if (SystemdUser() && std::filesystem::exists(UnitPath()) &&
        RunCmd({"systemctl", "--user", "is-active", kUnit}) == 0) {
        if (RunCmd({"systemctl", "--user", "stop", kUnit}) != 0) {
            if (err) *err = "systemctl --user stop failed";
            return false;
        }
        WaitGone(dataDir, 5000);
        return true;
    }
    // started directly (no unit, or the unit is off): ask politely, then force
    if (ipc::Ping(dataDir)) { ipc::RequestStop(dataDir); WaitGone(dataDir, 3000); }
    KillDaemons();
    if (!WaitGone(dataDir, 3000)) { if (err) *err = "the daemon did not stop"; return false; }
    return true;
}

bool SpawnDetached(std::string* err)
{
    const auto exe = paths::DaemonExe();
    std::error_code ec;
    if (!std::filesystem::exists(exe, ec)) {
        if (err) *err = exe.string() + " not found (it must sit next to the GUI executable)";
        return false;
    }
    const std::string path = exe.string();
    const pid_t first = fork();
    if (first < 0) { if (err) *err = "fork failed"; return false; }
    if (first == 0) {
        setsid();
        const pid_t second = fork();            // double fork: the daemon is nobody's child, no zombie
        if (second != 0) _exit(0);
        const int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        execl(path.c_str(), path.c_str(), "--run", (char*)nullptr);
        _exit(127);
    }
    int st = 0;
    waitpid(first, &st, 0);
    return true;
}

void WatchWorkerStop(std::function<void()>) {}

} // namespace svc

#endif // __linux__
