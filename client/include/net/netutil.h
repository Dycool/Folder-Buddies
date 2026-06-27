#pragma once

#include <string>

namespace fb {

struct LocalAddrs {
    std::string globalV6;  // 2000::/3 — internet-reachable, no NAT
    std::string ulaV6;     // fc00::/7 — unique-local, LAN/VPN only
    std::string privateV4; // RFC1918 — typical home-LAN address
    std::string anyV4;     // any other non-loopback IPv4
};

LocalAddrs enumerate_local_addrs();

std::string best_local_ip(bool lanOnly);

} // namespace fb
