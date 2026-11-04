#include "localinfo.hpp"

#include <cstdint>
#include <fstream>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <iphlpapi.h>
#else
  #include <arpa/inet.h>
  #include <ifaddrs.h>
  #include <net/if.h>
  #include <netinet/in.h>
#endif

#ifdef _WIN32
static std::string Narrow(const wchar_t* w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::vector<LocalAddress> GetLocalAddresses()
{
    std::vector<LocalAddress> out;
    ULONG size = 16 * 1024;
    std::vector<char> buf(size);
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;

    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size);
    }
    if (rc != NO_ERROR) return out;

    for (auto* a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            char s[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &sin->sin_addr, s, sizeof s))
                out.push_back({Narrow(a->FriendlyName), s, (int)u->OnLinkPrefixLength});
        }
    }
    return out;
}

std::vector<std::string> ReadArpTable()
{
    std::vector<std::string> out;
    ULONG size = 0;
    GetIpNetTable(nullptr, &size, FALSE);
    if (!size) return out;
    std::vector<char> buf(size);
    auto* t = reinterpret_cast<MIB_IPNETTABLE*>(buf.data());
    if (GetIpNetTable(t, &size, FALSE) != NO_ERROR) return out;

    for (DWORD i = 0; i < t->dwNumEntries; ++i) {
        const auto& r = t->table[i];
        if (r.dwType == MIB_IPNET_TYPE_INVALID || r.dwPhysAddrLen != 6) continue;
        bool allFF = true;
        for (int k = 0; k < 6; ++k) allFF = allFF && r.bPhysAddr[k] == 0xFF;
        if (allFF) continue;                                   // broadcast
        if ((ntohl(r.dwAddr) >> 28) == 0xE) continue;          // multicast
        in_addr ia; ia.s_addr = r.dwAddr;
        char s[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &ia, s, sizeof s)) out.push_back(s);
    }
    return out;
}
#else
static bool IsVirtualInterface(const std::string& n)
{
    static const char* prefixes[] = {"docker", "veth", "br-", "virbr", "vboxnet", "vmnet", "lxcbr", "cni", "flannel"};
    for (const char* p : prefixes)
        if (n.rfind(p, 0) == 0) return true;
    return false;
}

std::vector<LocalAddress> GetLocalAddresses()
{
    std::vector<LocalAddress> out;
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return out;

    for (auto* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        if (IsVirtualInterface(p->ifa_name)) continue;
        auto* sin = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
        char s[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &sin->sin_addr, s, sizeof s)) continue;

        int prefix = 24;
        if (p->ifa_netmask) {
            auto* m = reinterpret_cast<sockaddr_in*>(p->ifa_netmask);
            std::uint32_t mk = ntohl(m->sin_addr.s_addr);
            prefix = 0;
            while (mk & 0x80000000u) { ++prefix; mk <<= 1; }
        }
        out.push_back({p->ifa_name, s, prefix});
    }
    freeifaddrs(ifa);
    return out;
}

std::vector<std::string> ReadArpTable()
{
    std::vector<std::string> out;
    std::ifstream f("/proc/net/arp");
    if (!f) return out;
    std::string header;
    std::getline(f, header);
    std::string ip, hw, flags, mac, mask, dev;
    while (f >> ip >> hw >> flags >> mac >> mask >> dev) {
        if (flags == "0x0" || mac == "00:00:00:00:00:00") continue;   // incomplete entry
        out.push_back(ip);
    }
    return out;
}
#endif
