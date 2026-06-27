#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace fb {

class RemoteFs {
public:
    virtual ~RemoteFs() = default;
    virtual bool connected() const = 0;
    virtual int request(uint16_t op, const std::vector<uint8_t>& payload,
                        std::vector<uint8_t>& resp) = 0;

    std::atomic<uint64_t> bytesRead{0};
    std::atomic<uint64_t> bytesWritten{0};
};

} // namespace fb
