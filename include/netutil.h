// Folder Buddies — local address discovery, IPv6-first.
//
// We prefer IPv6 and fall back to IPv4. For "LAN only" we deliberately advertise
// a *non-globally-routable* address (ULA IPv6 or private IPv4) so the code can't
// be used from the open internet; for internet sharing we prefer a globally
// reachable IPv6 (no NAT, so no UPnP needed) and only fall back to IPv4 + UPnP.
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
