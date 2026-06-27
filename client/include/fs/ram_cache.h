#pragma once

#include "remote_fs.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace fb {

class RamCache : public RemoteFs {
public:
    // `inner` must outlive the cache. The cache never takes ownership.
    explicit RamCache(RemoteFs* inner);
    ~RamCache() override;

    bool connected() const override;
    int request(uint16_t op, const std::vector<uint8_t>& payload,
                std::vector<uint8_t>& resp) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fb
