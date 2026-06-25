// Folder Buddies — network client. Maintains a pool of authenticated TCP
// connections and multiplexes synchronous request/response over them so a
// large transfer on one connection never blocks a metadata call on another.
#pragma once

#include "common.h"
#include "crypto.h"
#include "token.h"
#include "remote_fs.h"

#include <QString>
#include <atomic>
#include <condition_variable>
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
    void disconnect();
    bool connected() const override { return connected_.load(); }

    // Returns 0 on success (resp filled) or a positive errno on failure.
    int request(uint16_t op, const std::vector<uint8_t>& payload, std::vector<uint8_t>& resp) override;


private:
    struct Pending {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        int16_t status = 0;
        std::vector<uint8_t> data;
    };
    struct Conn {
        socket_t sock = FB_BAD_SOCKET;
        std::thread reader;
        std::mutex wmtx;
        std::mutex pmtx;
        std::unordered_map<uint64_t, Pending*> pend;
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
