// Folder Buddies — local address discovery, IPv6-first. LAN-only mode advertises
// a non-globally-routable address; internet mode prefers global IPv6, else
// IPv4 + UPnP.
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

// Best address to put in a share code. lanOnly → a private/ULA address only
// (never a globally-routable one). Otherwise → prefer global IPv6, else "" so
// the caller knows to use IPv4 + UPnP.
std::string best_local_ip(bool lanOnly);

} // namespace fb
