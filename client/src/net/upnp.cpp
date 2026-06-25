#include "upnp.h"

#include "common.h" // socket helpers + headers

#include <cstdio>

#ifdef HAVE_MINIUPNPC
#  include <miniupnpc/miniupnpc.h>
#  include <miniupnpc/upnpcommands.h>
#endif

namespace fb {

#ifdef HAVE_MINIUPNPC

bool Upnp::map(uint16_t internalPort, std::string& externalIp, uint16_t& externalPort,
               std::string& err) {
    int error = 0;
    UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error);
    if (!devlist) { err = "no UPnP IGD discovered"; return false; }

    UPNPUrls urls;
    IGDdatas data;
    char lan[64] = {0};
#if defined(MINIUPNPC_API_VERSION) && MINIUPNPC_API_VERSION >= 18
    char wan[64] = {0};
    int r = UPNP_GetValidIGD(devlist, &urls, &data, lan, sizeof(lan), wan, sizeof(wan));
#else
    int r = UPNP_GetValidIGD(devlist, &urls, &data, lan, sizeof(lan));
#endif
    if (r != 1 && r != 2) {
        freeUPNPDevlist(devlist);
        err = "no connected IGD";
        return false;
    }

    char ext[40] = {0};
    UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, ext);
    char ips[16];
    std::snprintf(ips, sizeof(ips), "%u", internalPort); // LAN-side port to forward to

    // Scan for a free external port; a busy mapping or a failed AddPortMapping
    // both move to the next. The rule name carries the external port.
    bool mapped = false;
    uint16_t candidate = internalPort;
    for (int tries = 0; tries < 64 && !mapped; ++tries, ++candidate) {
        if (candidate == 0) ++candidate;
        char eps[16];
        std::snprintf(eps, sizeof(eps), "%u", candidate);

        char inClient[64] = {0}, inPort[16] = {0}, desc[128] = {0};
        char enabled[8] = {0}, lease[16] = {0};
        int exists = UPNP_GetSpecificPortMappingEntry(
            urls.controlURL, data.first.servicetype, eps, "TCP", nullptr, inClient, inPort,
            desc, enabled, lease);
        if (exists == UPNPCOMMAND_SUCCESS) continue; // port busy → another rule there

        char name[64];
        std::snprintf(name, sizeof(name), "FolderBuddies-%u", candidate);
        int a = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, eps, ips,
                                    lan, name, "TCP", nullptr, "0");
        if (a == UPNPCOMMAND_SUCCESS) {
            externalPort = candidate;
            externalPort_ = candidate;
            mapped = true;
        }
    }

    if (!mapped) {
        FreeUPNPUrls(&urls);
        freeUPNPDevlist(devlist);
        err = "AddPortMapping failed (no free external port on the router)";
        return false;
    }

    externalIp = ext[0] ? ext : lan;
    ctrlUrl_ = urls.controlURL;
    serviceType_ = data.first.servicetype;
    active_ = true;

    FreeUPNPUrls(&urls);
    freeUPNPDevlist(devlist);
    return true;
}

void Upnp::unmap() {
    if (!active_) return;
    char ps[16];
    std::snprintf(ps, sizeof(ps), "%u", externalPort_);
    UPNP_DeletePortMapping(ctrlUrl_.c_str(), serviceType_.c_str(), ps, "TCP", nullptr);
    active_ = false;
}

#else // !HAVE_MINIUPNPC

bool Upnp::map(uint16_t, std::string&, uint16_t&, std::string& err) {
    err = "built without UPnP support";
    return false;
}
void Upnp::unmap() {}

#endif

} // namespace fb
