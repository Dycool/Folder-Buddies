#include "netutil.h"

#include "common.h" // socket headers (incl. winsock on Windows)

#ifdef _WIN32
#  include <iphlpapi.h>
#  include <vector>
#else
#  include <ifaddrs.h>
#  include <net/if.h>
#endif

namespace fb {
namespace {

bool is_global_v6(const uint8_t* a) { return (a[0] & 0xe0) == 0x20; }   // 2000::/3
bool is_ula_v6(const uint8_t* a) { return (a[0] & 0xfe) == 0xfc; }      // fc00::/7
bool is_linklocal_v6(const uint8_t* a) { return a[0] == 0xfe && (a[1] & 0xc0) == 0x80; }
bool is_loopback_v6(const uint8_t* a) {
    for (int i = 0; i < 15; ++i)
        if (a[i]) return false;
    return a[15] == 1;
}

bool is_private_v4(uint32_t hostOrder) {
    uint8_t a = (hostOrder >> 24) & 0xff, b = (hostOrder >> 16) & 0xff;
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
}
bool is_usable_v4(uint32_t hostOrder) {
    uint8_t a = (hostOrder >> 24) & 0xff, b = (hostOrder >> 16) & 0xff;
    return a != 127 && !(a == 169 && b == 254) && a != 0; // skip loopback/link-local/unspec
}

void consider_v6(LocalAddrs& out, const in6_addr& a6) {
    const uint8_t* a = a6.s6_addr;
    if (is_loopback_v6(a) || is_linklocal_v6(a)) return; // link-local needs a scope id
    char buf[64];
    if (!inet_ntop(AF_INET6, &a6, buf, sizeof(buf))) return;
    if (is_global_v6(a) && out.globalV6.empty()) out.globalV6 = buf;
    else if (is_ula_v6(a) && out.ulaV6.empty()) out.ulaV6 = buf;
}

void consider_v4(LocalAddrs& out, const in_addr& a4) {
    uint32_t h = ntohl(a4.s_addr);
    if (!is_usable_v4(h)) return;
    char buf[32];
    if (!inet_ntop(AF_INET, &a4, buf, sizeof(buf))) return;
    if (is_private_v4(h) && out.privateV4.empty()) out.privateV4 = buf;
    if (out.anyV4.empty()) out.anyV4 = buf;
}

} // namespace

#ifdef _WIN32

LocalAddrs enumerate_local_addrs() {
    LocalAddrs out;
    ULONG size = 15000;
    std::vector<uint8_t> buf(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, adapters, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, adapters, &size) != NO_ERROR)
            return out;
    }
    for (auto* ad = adapters; ad; ad = ad->Next) {
        if (ad->OperStatus != IfOperStatusUp || ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* ua = ad->FirstUnicastAddress; ua; ua = ua->Next) {
            sockaddr* sa = ua->Address.lpSockaddr;
            if (sa->sa_family == AF_INET6)
                consider_v6(out, reinterpret_cast<sockaddr_in6*>(sa)->sin6_addr);
            else if (sa->sa_family == AF_INET)
                consider_v4(out, reinterpret_cast<sockaddr_in*>(sa)->sin_addr);
        }
    }
    return out;
}

#else

LocalAddrs enumerate_local_addrs() {
    LocalAddrs out;
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return out;
    for (ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || !(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        if (p->ifa_addr->sa_family == AF_INET6)
            consider_v6(out, reinterpret_cast<sockaddr_in6*>(p->ifa_addr)->sin6_addr);
        else if (p->ifa_addr->sa_family == AF_INET)
            consider_v4(out, reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr);
    }
    freeifaddrs(ifa);
    return out;
}

#endif

std::string best_local_ip(bool lanOnly) {
    LocalAddrs a = enumerate_local_addrs();
    if (lanOnly) {
        // Never a globally-routable address. IPv6 (ULA) preferred, then private v4.
        if (!a.ulaV6.empty()) return a.ulaV6;
        if (!a.privateV4.empty()) return a.privateV4;
        if (!a.anyV4.empty()) return a.anyV4;
        return "127.0.0.1";
    }
    // Internet: a global IPv6 is directly reachable (no NAT) — best case.
    if (!a.globalV6.empty()) return a.globalV6;
    return {}; // signal: no public IPv6; caller should use IPv4 + UPnP
}

} // namespace fb
