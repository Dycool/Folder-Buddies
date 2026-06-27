#pragma once

#include "ram_cache.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#ifndef _WIN32
struct fuse;
#endif

namespace fb {

class RemoteFs;

class Mount {
public:
    using EjectedCallback = std::function<void()>;

    bool start(RemoteFs* client, const std::string& mountBase, const std::string& volname,
               bool allowWrites, std::string& err);
    void stop();
    bool active() const { return active_.load(); }
    const std::string& mountpoint() const { return mp_; }
    void setEjectedCallback(EjectedCallback cb) {
        std::lock_guard<std::mutex> lk(callbackMtx_);
        onEjected_ = std::move(cb);
    }

    RemoteFs* remote() const { return cache_.get(); }

private:
    void notifyEjected() {
        EjectedCallback cb;
        {
            std::lock_guard<std::mutex> lk(callbackMtx_);
            cb = onEjected_;
        }
        if (cb) cb();
    }

#ifdef _WIN32
    void* backend_ = nullptr; // ProjFS PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT
    std::wstring driveName_;
    std::wstring driveTarget_;
#else
    struct fuse* fuse_ = nullptr;
#endif
    std::unique_ptr<RamCache> cache_; // RAM-only read/metadata cache (no persistence)
    std::thread thread_;
    std::string mp_;
    std::atomic_bool active_{false};
    std::atomic_bool stopping_{false};
    std::mutex callbackMtx_;
    EjectedCallback onEjected_;
};

} // namespace fb
