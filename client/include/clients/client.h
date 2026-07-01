#pragma once

#include "common.h"
#include "crypto.h"
#include "token.h"
#include "remote_fs.h"

#include <QString>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fb {

class Client : public RemoteFs {
public:
    bool connect(const Token& tok, int nconns, std::string& err);
    // Connect over already-established streams, used by native QUIC after
    // ICE has selected a direct path.
    bool connectStreams(const Token& tok, std::vector<std::shared_ptr<ByteStream>> streams,
                        std::string& err);
    void disconnect();
    bool connected() const override { return connected_.load(); }

    // Returns 0 on success (resp filled) or a positive errno on failure.
    int request(uint16_t op, const std::vector<uint8_t>& payload, std::vector<uint8_t>& resp) override;

    // Thread-safe: the callback fires from reader threads, so it is guarded
    // by a mutex and can be cleared before its captures are destroyed.
    // Clearing blocks until any in-flight invocation has finished.
    void setInvalidateCallback(std::function<void(const std::string& path)> cb) {
        std::lock_guard<std::mutex> lk(invalidateMtx_);
        onInvalidate_ = std::move(cb);
    }

private:
    std::mutex invalidateMtx_;
    std::function<void(const std::string& path)> onInvalidate_;

    struct Pending {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        int16_t status = 0;
        std::vector<uint8_t> data;
    };
    struct Conn {
        std::shared_ptr<ByteStream> stream;
        std::thread reader;
        std::mutex wmtx;
        std::mutex pmtx;
        std::unordered_map<uint64_t, std::shared_ptr<Pending>> pend;
        std::atomic<uint64_t> nextId{1};
        std::atomic<bool> alive{false};
        SecureChannel chan; // sealed after the handshake; tx under wmtx, rx in readerLoop
    };

    bool handshake(Conn& c, const Token& tok, std::string& err);
    void readerLoop(Conn* c);

    std::vector<std::unique_ptr<Conn>> conns_;
    std::atomic<uint32_t> rr_{0};
    uint8_t clientId_[16] = {};
    std::atomic<bool> connected_{false};
};

} // namespace fb
