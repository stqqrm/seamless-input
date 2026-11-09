#include "clipboard.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#if defined(__linux__)
#include <csignal>
#endif

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <shlobj.h>
#endif

namespace clipboard {

// ------------------------------------------------------------------ base64
std::string Base64Encode(const std::string& in)
{
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
    }
    if (i + 1 == in.size()) {
        const unsigned v = (unsigned char)in[i] << 16;
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += '=';
    }
    return out;
}

bool Base64Decode(const std::string& in, std::string& out)
{
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    if (in.size() % 4 != 0) return false;
    out.reserve(in.size() / 4 * 3);
    for (std::size_t i = 0; i < in.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = in[i + k];
            if (c == '=') {
                if (i + 4 != in.size() || k < 2) return false;
                v[k] = 0;
                ++pad;
            } else {
                if (pad) return false;
                v[k] = val(c);
                if (v[k] < 0) return false;
            }
        }
        const unsigned n = ((unsigned)v[0] << 18) | ((unsigned)v[1] << 12) | ((unsigned)v[2] << 6) | (unsigned)v[3];
        out += (char)((n >> 16) & 0xFF);
        if (pad < 2) out += (char)((n >> 8) & 0xFF);
        if (pad < 1) out += (char)(n & 0xFF);
    }
    return true;
}

namespace {
std::mutex g_noteMx;
std::string g_note;
void SetNote(const std::string& n) { std::lock_guard<std::mutex> lk(g_noteMx); g_note = n; }
} // namespace

std::string LastNote() { std::lock_guard<std::mutex> lk(g_noteMx); return g_note; }

#if defined(_WIN32)
// ------------------------------------------------------------------ Windows
namespace {

HWND Owner()
{
    static HWND hw = nullptr;               // created on (and bound to) the first calling thread
    if (hw) return hw;
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SeamlessInputClipboardOwner";
    RegisterClassW(&wc);                    // fails harmlessly when already registered
    hw = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    return hw;
}

bool OpenRetry(HWND h)
{
    for (int i = 0; i < 25; ++i) {          // another program may hold the clipboard for a moment
        if (OpenClipboard(h)) return true;
        Sleep(10);
    }
    return false;
}

std::wstring ToWide(const std::string& u8)
{
    if (u8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), &w[0], n);
    return w;
}

std::string ToUtf8(const wchar_t* w, size_t len)
{
    if (!len) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, (int)len, &s[0], n, nullptr, nullptr);
    return s;
}

struct DropFilesHeader {                    // DROPFILES (shlobj.h) without the include
    DWORD pFiles;
    POINT pt;
    BOOL fNC;
    BOOL fWide;
};

} // namespace

std::string Backend() { return "Windows clipboard"; }
bool FilesSupported() { return true; }

bool GetText(std::string& out, std::size_t maxBytes)
{
    out.clear();
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return false;
    HWND owner = Owner();
    if (!owner || !OpenRetry(owner)) return false;
    bool ok = false;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h))) {
            const size_t maxLen = GlobalSize(h) / sizeof(wchar_t);
            size_t len = 0;
            while (len < maxLen && w[len]) ++len;
            std::string u8 = ToUtf8(w, len);
            GlobalUnlock(h);
            // CRLF -> LF on the wire
            std::string t;
            t.reserve(u8.size());
            for (size_t i = 0; i < u8.size(); ++i) {
                if (u8[i] == '\r' && i + 1 < u8.size() && u8[i + 1] == '\n') continue;
                t += u8[i];
            }
            if (!t.empty() && t.size() <= maxBytes) { out = std::move(t); ok = true; }
        }
    }
    CloseClipboard();
    return ok;
}

bool SetText(const std::string& text)
{
    // LF -> CRLF for Windows programs
    std::string crlf;
    crlf.reserve(text.size() + text.size() / 16);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) crlf += '\r';
        crlf += text[i];
    }
    const std::wstring w = ToWide(crlf);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    if (!mem) return false;
    if (void* p = GlobalLock(mem)) {
        std::memcpy(p, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
    } else {
        GlobalFree(mem);
        return false;
    }
    HWND owner = Owner();
    if (!owner || !OpenRetry(owner)) { GlobalFree(mem); return false; }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    if (!ok) GlobalFree(mem);               // on success the system owns the memory
    CloseClipboard();
    return ok;
}

bool GetFiles(std::vector<std::filesystem::path>& files)
{
    files.clear();
    SetNote("");
    if (!IsClipboardFormatAvailable(CF_HDROP)) { SetNote("no files on the clipboard"); return false; }
    HWND owner = Owner();
    if (!owner || !OpenRetry(owner)) { SetNote("the Windows clipboard is in use by another program"); return false; }
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        // DROPFILES header followed by a double-NUL terminated list (parsed by hand: no shell32 needed)
        const SIZE_T total = GlobalSize(h);
        if (const char* base = static_cast<const char*>(GlobalLock(h))) {
            if (total > sizeof(DropFilesHeader)) {
                const auto* d = reinterpret_cast<const DropFilesHeader*>(base);
                if (d->pFiles >= sizeof(DropFilesHeader) && d->pFiles < total) {
                    if (d->fWide) {
                        const wchar_t* p = reinterpret_cast<const wchar_t*>(base + d->pFiles);
                        const wchar_t* end = reinterpret_cast<const wchar_t*>(base + total);
                        while (p < end && *p) {
                            const wchar_t* e = p;
                            while (e < end && *e) ++e;
                            files.emplace_back(std::wstring(p, e));
                            p = e + 1;
                        }
                    } else {                       // ANSI list from an old program
                        const char* p = base + d->pFiles;
                        const char* end = base + total;
                        while (p < end && *p) {
                            const char* e = p;
                            while (e < end && *e) ++e;
                            const std::string a(p, e);
                            const int n = MultiByteToWideChar(CP_ACP, 0, a.data(), (int)a.size(), nullptr, 0);
                            std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
                            if (n > 0) MultiByteToWideChar(CP_ACP, 0, a.data(), (int)a.size(), &w[0], n);
                            files.emplace_back(w);
                            p = e + 1;
                        }
                    }
                }
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    if (files.empty()) SetNote("the file list on the clipboard could not be read");
    return !files.empty();
}

// Explorer's own copies carry "Shell IDList Array" next to CF_HDROP; its Paste menu entry needs it.
static HGLOBAL BuildShellIdList(const std::vector<std::filesystem::path>& files)
{
    if (files.empty()) return nullptr;
    const auto parentPath = files.front().parent_path();
    for (const auto& f : files) if (f.parent_path() != parentPath) return nullptr;   // one parent folder only
    PIDLIST_ABSOLUTE parent = ILCreateFromPathW(parentPath.c_str());
    if (!parent) return nullptr;
    std::vector<PIDLIST_ABSOLUTE> full;
    bool ok = true;
    for (const auto& f : files) {
        PIDLIST_ABSOLUTE p = ILCreateFromPathW(f.c_str());
        if (!p) { ok = false; break; }
        full.push_back(p);
    }
    HGLOBAL mem = nullptr;
    if (ok) {
        const UINT n = (UINT)files.size();
        const UINT head = (UINT)(sizeof(UINT) * (2 + n));
        UINT total = head + ILGetSize(parent);
        for (auto* p : full) total += ILGetSize(ILFindLastID(p));
        mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total);
        if (mem) {
            if (auto* base = static_cast<BYTE*>(GlobalLock(mem))) {
                auto* cida = reinterpret_cast<CIDA*>(base);
                cida->cidl = n;
                UINT off = head;
                cida->aoffset[0] = off;
                std::memcpy(base + off, parent, ILGetSize(parent));
                off += ILGetSize(parent);
                for (UINT i = 0; i < n; ++i) {
                    PCUIDLIST_RELATIVE c = ILFindLastID(full[i]);
                    cida->aoffset[i + 1] = off;
                    std::memcpy(base + off, c, ILGetSize(c));
                    off += ILGetSize(c);
                }
                GlobalUnlock(mem);
            } else { GlobalFree(mem); mem = nullptr; }
        }
    }
    for (auto* p : full) ILFree(p);
    ILFree(parent);
    return mem;
}

bool SetFiles(const std::vector<std::filesystem::path>& filesIn)
{
    SetNote("");
    if (filesIn.empty()) return false;
    std::vector<std::filesystem::path> files;        // backslashes only: Explorer rejects mixed separators
    for (auto f : filesIn) { f = f.lexically_normal(); f.make_preferred(); files.push_back(std::move(f)); }
    std::wstring list;                       // path\0path\0...\0\0
    for (const auto& f : files) {
        list += f.wstring();
        list += L'\0';
    }
    list += L'\0';
    const SIZE_T bytes = sizeof(DropFilesHeader) + list.size() * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!mem) return false;
    if (auto* d = static_cast<DropFilesHeader*>(GlobalLock(mem))) {
        d->pFiles = sizeof(DropFilesHeader);
        d->fWide = TRUE;
        std::memcpy(reinterpret_cast<char*>(d) + sizeof(DropFilesHeader), list.data(), list.size() * sizeof(wchar_t));
        GlobalUnlock(mem);
    } else {
        GlobalFree(mem);
        return false;
    }
    HGLOBAL effect = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));          // "copy", not "move"
    if (effect) {
        if (auto* e = static_cast<DWORD*>(GlobalLock(effect))) { *e = 1; GlobalUnlock(effect); }
    }
    HGLOBAL idl = BuildShellIdList(files);
    HWND owner = Owner();
    if (!owner || !OpenRetry(owner)) {
        GlobalFree(mem); if (effect) GlobalFree(effect); if (idl) GlobalFree(idl);
        SetNote("the Windows clipboard is in use by another program");
        return false;
    }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_HDROP, mem) != nullptr;
    if (!ok) { GlobalFree(mem); SetNote("Windows refused the file list (error " + std::to_string(GetLastError()) + ")"); }
    if (effect) {
        if (!(ok && SetClipboardData(RegisterClipboardFormatW(L"Preferred DropEffect"), effect))) GlobalFree(effect);
    }
    if (idl) {
        if (!(ok && SetClipboardData(RegisterClipboardFormatW(L"Shell IDList Array"), idl))) GlobalFree(idl);
    }
    CloseClipboard();
    // read back what Windows now holds, so the log shows whether the list survived
    if (ok) {
        std::vector<std::filesystem::path> back;
        const std::string note = LastNote();
        const bool readable = GetFiles(back);
        SetNote(readable ? std::string() : note);
        if (!readable || back.size() != files.size())
            SetNote("file list set, but read back " + std::to_string(back.size()) + " of " + std::to_string(files.size()) + " entries");
    }
    return ok;
}

#elif defined(__linux__)
// ------------------------------------------------------------------ Linux (external tools)
namespace {

struct Tools {
    std::string name;                       // empty: nothing available
    std::string get, set;                   // text
    bool files = false;
    std::string list;                       // prints the offered targets / mime types, one per line
    std::string readUri, readGnome;         // print file lists
    std::string writeUri, writeGnome;       // read a file list from stdin and put it on the clipboard
};

bool Have(const char* tool)
{
    const std::string c = std::string("command -v ") + tool + " >/dev/null 2>&1";
    return std::system(c.c_str()) == 0;
}

const Tools& Pick()
{
    static const Tools t = [] {
        Tools r;
        std::signal(SIGPIPE, SIG_IGN);      // a tool that exits early must not kill the daemon
        const bool wl = std::getenv("WAYLAND_DISPLAY") != nullptr;
        const bool x  = std::getenv("DISPLAY") != nullptr;
        if (wl && Have("wl-paste") && Have("wl-copy")) {
            r.name = "wl-clipboard";
            r.get = "timeout 2 wl-paste --no-newline --type text/plain 2>/dev/null";
            r.set = "timeout 3 wl-copy 2>/dev/null";
            r.files = true;
            r.list = "timeout 2 wl-paste --list-types 2>/dev/null";
            r.readUri = "timeout 5 wl-paste --no-newline --type text/uri-list 2>/dev/null";
            r.readGnome = "timeout 5 wl-paste --no-newline --type x-special/gnome-copied-files 2>/dev/null";
            r.writeUri = "timeout 5 wl-copy --type text/uri-list 2>/dev/null";
            r.writeGnome = "timeout 5 wl-copy --type x-special/gnome-copied-files 2>/dev/null";
        } else if ((x || wl) && Have("xclip")) {
            r.name = "xclip";
            r.get = "timeout 2 xclip -selection clipboard -o 2>/dev/null";
            r.set = "timeout 3 xclip -selection clipboard -i 2>/dev/null";
            r.files = true;
            r.list = "timeout 2 xclip -selection clipboard -t TARGETS -o 2>/dev/null";
            r.readUri = "timeout 5 xclip -selection clipboard -t text/uri-list -o 2>/dev/null";
            r.readGnome = "timeout 5 xclip -selection clipboard -t x-special/gnome-copied-files -o 2>/dev/null";
            r.writeUri = "timeout 5 xclip -selection clipboard -t text/uri-list -i 2>/dev/null";
            r.writeGnome = "timeout 5 xclip -selection clipboard -t x-special/gnome-copied-files -i 2>/dev/null";
        } else if ((x || wl) && Have("xsel")) {
            r.name = "xsel";                // text only: xsel cannot choose the clipboard target
            r.get = "timeout 2 xsel --clipboard --output 2>/dev/null";
            r.set = "timeout 3 xsel --clipboard --input 2>/dev/null";
        }
        return r;
    }();
    return t;
}

bool RunRead(const std::string& cmd, std::size_t maxBytes, std::string& out, bool* tooBig = nullptr)
{
    out.clear();
    if (tooBig) *tooBig = false;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[4096];
    std::size_t n;
    bool big = false;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) {
        if (out.size() + n > maxBytes) { big = true; break; }
        out.append(buf, n);
    }
    const int rc = pclose(p);
    if (tooBig) *tooBig = big;
    if (big || rc != 0 || out.empty()) { out.clear(); return false; }
    return true;
}

bool RunWrite(const std::string& cmd, const std::string& data)
{
    FILE* p = popen(cmd.c_str(), "w");
    if (!p) return false;
    const std::size_t w = std::fwrite(data.data(), 1, data.size(), p);
    const int rc = pclose(p);               // the tool forks into the background to own the selection
    return w == data.size() && rc == 0;
}

std::string PercentDecode(const std::string& s)
{
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            o += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else {
            o += s[i];
        }
    }
    return o;
}

std::string PercentEncodePath(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

// "file:///home/a/b%20c" -> /home/a/b c ; anything that is not a local file URI -> empty
std::string UriToPath(std::string u)
{
    while (!u.empty() && (u.back() == '\r' || u.back() == ' ' || u.back() == '\n')) u.pop_back();
    if (u.compare(0, 7, "file://") != 0) return {};
    u.erase(0, 7);
    if (!u.empty() && u[0] != '/') {          // file://host/path : only "localhost" is local
        const auto slash = u.find('/');
        if (slash == std::string::npos || u.substr(0, slash) != "localhost") return {};
        u.erase(0, slash);
    }
    return PercentDecode(u);
}

// Which format the file manager pastes. Only the desktops below read plain text/uri-list; every other
// file manager (Nautilus, Nemo, Caja, Thunar, ...) wants x-special/gnome-copied-files. A daemon started
// without desktop variables (systemd user unit, autostart) therefore gets the GNOME format too.
bool GnomeStyleDesktop()
{
    std::string s;
    for (const char* name : {"XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP", "DESKTOP_SESSION"}) {
        const char* d = std::getenv(name);
        if (d && *d) { s += d; s += ':'; }
    }
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    for (const char* k : {"KDE", "PLASMA", "LXQT", "RAZOR"})
        if (s.find(k) != std::string::npos) return false;
    return true;
}

} // namespace

std::string Backend() { return Pick().name; }
bool FilesSupported() { return Pick().files; }

bool GetText(std::string& out, std::size_t maxBytes)
{
    out.clear();
    const Tools& t = Pick();
    if (t.name.empty()) return false;
    return RunRead(t.get, maxBytes, out);
}

bool SetText(const std::string& text)
{
    const Tools& t = Pick();
    if (t.name.empty()) return false;
    return RunWrite(t.set, text);
}

bool GetFiles(std::vector<std::filesystem::path>& files)
{
    files.clear();
    SetNote("");
    const Tools& t = Pick();
    if (t.name.empty()) { SetNote("no clipboard tool found (install xclip or wl-clipboard)"); return false; }
    if (!t.files) { SetNote(t.name + " cannot read files (install xclip or wl-clipboard)"); return false; }
    std::string types;
    if (!RunRead(t.list, 64 * 1024, types)) { SetNote("could not list the clipboard formats with " + t.name + " (empty clipboard, or the desktop blocks background clipboard access)"); return false; }
    const bool gnome = types.find("x-special/gnome-copied-files") != std::string::npos;
    const bool uri = types.find("text/uri-list") != std::string::npos;
    if (!gnome && !uri) { SetNote("no files on the clipboard"); return false; }

    std::string data;
    if (!RunRead(gnome ? t.readGnome : t.readUri, 4 * 1024 * 1024, data)) { SetNote("could not read the file list with " + t.name); return false; }
    std::istringstream is(data);
    std::string line;
    while (std::getline(is, line)) {
        if (line.empty() || line[0] == '#' || line == "copy" || line == "cut" || line == "copy\r" || line == "cut\r") continue;
        const std::string p = UriToPath(line);
        if (!p.empty()) files.emplace_back(p);
    }
    if (files.empty()) SetNote("the clipboard file list has no local files");
    return !files.empty();
}

bool SetFiles(const std::vector<std::filesystem::path>& files)
{
    SetNote("");
    const Tools& t = Pick();
    if (t.name.empty()) { SetNote("no clipboard tool found (install xclip or wl-clipboard)"); return false; }
    if (!t.files || files.empty()) { SetNote(t.name + " cannot write files (install xclip or wl-clipboard)"); return false; }
    const bool gnome = GnomeStyleDesktop();
    std::string data;
    if (gnome) data = "copy";
    for (const auto& f : files) {
        const std::string uri = "file://" + PercentEncodePath(f.string());
        data += gnome ? "\n" + uri : uri + "\r\n";
    }
    const bool ok = RunWrite(gnome ? t.writeGnome : t.writeUri, data);
    if (!ok) SetNote(t.name + " failed to take the file list (" + (gnome ? "x-special/gnome-copied-files" : "text/uri-list") + ")");
    return ok;
}

#else
std::string Backend() { return {}; }
bool FilesSupported() { return false; }
bool GetText(std::string& out, std::size_t) { out.clear(); return false; }
bool SetText(const std::string&) { return false; }
bool GetFiles(std::vector<std::filesystem::path>& f) { f.clear(); SetNote("files are not supported on this platform"); return false; }
bool SetFiles(const std::vector<std::filesystem::path>&) { SetNote("files are not supported on this platform"); return false; }
#endif

} // namespace clipboard
