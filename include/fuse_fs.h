// Folder Buddies — OS mount abstraction.
//
// Linux/macOS use FUSE/FUSE-T. Windows uses Microsoft's native Projected File
// System (ProjFS) so Explorer hydrates placeholders on demand from the P2P
// stream without a third-party filesystem driver.
#pragma once

#include <string>
#include <thread>

#ifndef _WIN32
struct fuse;
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

private:
#ifdef _WIN32
    void* backend_ = nullptr; // ProjFS PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT
#else
    struct fuse* fuse_ = nullptr;
#endif
    std::thread thread_;
    std::string mp_;
};

} // namespace fb
