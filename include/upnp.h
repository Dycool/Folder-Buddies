// Folder Buddies — optional UPnP-IGD port mapping for WAN reachability.
#pragma once

#include <cstdint>
#include <string>

namespace fb {

class Upnp {
public:
    ~Upnp() { unmap(); }

    // Forward `internalPort` and discover the external IP. The chosen *external*
    // port may differ from the internal one: if the requested external port (or
    // its mapping name) is already taken on the router — e.g. by another running
    // instance — we scan upward for a free one and return it in `externalPort`.
    // That external port is what callers must bake into the share code.
    bool map(uint16_t internalPort, std::string& externalIp, uint16_t& externalPort,
             std::string& err);
    void unmap();

private:
    bool active_ = false;
    uint16_t externalPort_ = 0; // the port actually mapped on the IGD
    std::string ctrlUrl_;
    std::string serviceType_;
};

} // namespace fb
