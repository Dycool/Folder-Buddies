// Folder Buddies — RAM-only performance cache for native remote mounts.
//
// A transport decorator that wraps any RemoteFs (native TCP or WebRTC
// compatibility) and sits between the OS mount layer and the real transport.
// It accelerates reads and metadata lookups using ONLY volatile process memory:
//
//   * short-TTL metadata, directory-listing, and negative-lookup caches;
//   * an LRU RAM block cache with sequential read-ahead and pipelined prefetch;
//   * strict write-through — every write/truncate/delete only completes after
//     the host acknowledges, and immediately invalidates the affected caches.
//
// There is NO persistence of any kind: no disk cache, no temp files, no journal,
// no write-back queue. Everything disappears when the process exits. Cache sizes
// are chosen automatically from detected system RAM; there are no user knobs.
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
