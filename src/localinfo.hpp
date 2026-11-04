#pragma once
#include <string>
#include <vector>

struct LocalAddress {
    std::string ifname;
    std::string ip;
    int prefix;          // subnet prefix length, e.g. 24
};

// Non-loopback, up, IPv4 addresses of this machine.
std::vector<LocalAddress> GetLocalAddresses();

// IPv4 neighbours currently in the OS ARP cache (hosts that answered on-link).
std::vector<std::string> ReadArpTable();
