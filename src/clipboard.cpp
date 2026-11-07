#include "clipboard.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

} // namespace

std::string Backend() { return "Windows clipboard"; }

bool GetText(std::string& out)
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
            if (!t.empty() && t.size() <= kMaxBytes) { out = std::move(t); ok = true; }
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

#elif defined(__linux__)
// ------------------------------------------------------------------ Linux (external tools)
namespace {

struct Tools {
    std::string name, get, set;             // empty name: nothing available
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
        } else if ((x || wl) && Have("xclip")) {
            r.name = "xclip";
            r.get = "timeout 2 xclip -selection clipboard -o 2>/dev/null";
            r.set = "timeout 3 xclip -selection clipboard -i 2>/dev/null";
        } else if ((x || wl) && Have("xsel")) {
            r.name = "xsel";
            r.get = "timeout 2 xsel --clipboard --output 2>/dev/null";
            r.set = "timeout 3 xsel --clipboard --input 2>/dev/null";
        }
        return r;
    }();
    return t;
}

} // namespace

std::string Backend() { return Pick().name; }

bool GetText(std::string& out)
{
    out.clear();
    const Tools& t = Pick();
    if (t.name.empty()) return false;
    FILE* p = popen(t.get.c_str(), "r");
    if (!p) return false;
    char buf[4096];
    std::size_t n;
    bool tooBig = false;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) {
        if (out.size() + n > kMaxBytes) { tooBig = true; break; }
        out.append(buf, n);
    }
    const int rc = pclose(p);
    if (tooBig || rc != 0 || out.empty()) { out.clear(); return false; }
    return true;
}

bool SetText(const std::string& text)
{
    const Tools& t = Pick();
    if (t.name.empty()) return false;
    FILE* p = popen(t.set.c_str(), "w");
    if (!p) return false;
    const std::size_t w = std::fwrite(text.data(), 1, text.size(), p);
    const int rc = pclose(p);               // the tool forks into the background to own the selection
    return w == text.size() && rc == 0;
}

#else
std::string Backend() { return {}; }
bool GetText(std::string& out) { out.clear(); return false; }
bool SetText(const std::string&) { return false; }
#endif

} // namespace clipboard
