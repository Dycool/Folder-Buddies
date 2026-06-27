// Folder Buddies — optional UPnP-IGD port mapping for WAN reachability.
#pragma once

#include <cstdint>
#include <string>

namespace fb {

class Upnp {
public:
    ~Upnp() { unmap(); }

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
