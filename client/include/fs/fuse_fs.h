// Folder Buddies — OS mount abstraction.
//
// Linux/macOS use FUSE/FUSE-T. Windows uses Microsoft's native Projected File
// System (ProjFS) so Explorer hydrates placeholders on demand from the P2P
// stream without a third-party filesystem driver.
#pragma once

#include "ram_cache.h"

#include <memory>
#include <string>
#include <thread>

#ifndef _WIN32
struct fuse;
struct fuse_chan;
#endif

namespace fb {

class RemoteFs;

class Mount {
public:
    bool start(RemoteFs* client, const std::string& mountBase, const std::string& volname,
               bool allowWrites, std::string& err);
    void stop();
#ifdef _WIN32
    bool active() const { return backend_ != nullptr; }
#else
    bool active() const { return fuse_ != nullptr; }
#endif
    const std::string& mountpoint() const { return mp_; }

    // The RAM-only cache that actually talks to the OS mount layer (wraps the
    // underlying transport). Byte counters here reflect mount-perceived I/O.
    RemoteFs* remote() const { return cache_.get(); }

private:
#ifdef _WIN32
    void* backend_ = nullptr; // ProjFS PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT
#else
    struct fuse* fuse_ = nullptr;
    struct fuse_chan* fuse_chan_ = nullptr;
#endif
    std::unique_ptr<RamCache> cache_; // RAM-only read/metadata cache (no persistence)
    std::thread thread_;
    std::string mp_;
};

} // namespace fb
