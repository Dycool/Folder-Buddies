#pragma once

#include "common.h"
#include "crypto.h"
#include "fileio.h"

#include <QByteArray>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fb {

class Server {
public:
    bool start(const std::string& folder, int port, std::string& err);
    void stop();
    bool running() const { return running_.load(); }

    // Accept an already-established reliable stream (for example, a QUIC
    // stream selected by ICE). Ownership is retained until its session ends.
    void acceptStream(std::shared_ptr<ByteStream> stream);

    std::vector<uint8_t> secret;       // auto-generated password (set by start)
    uint16_t boundPort = 0;
    std::string shareName;             // folder basename, baked into the token
    bool allowWrites = false;          // host-controlled permission gate

    std::atomic<uint64_t> bytesOut{0};
    std::atomic<uint64_t> bytesIn{0};
    std::function<void()> onClientsChanged;
    int clientCount();

private:
    // Each session drains its own bounded invalidation queue on a dedicated
    // sender thread, so one stalled client socket can never block request
    // handling or invalidation delivery for the other sessions. The queue is
    // best-effort: client caches self-heal via short TTLs, so overflow drops
    // the oldest entry instead of blocking.
    struct BroadcastSession {
        ByteStream* stream = nullptr;
        SecureChannel* chan = nullptr;
        std::mutex* sendMutex = nullptr;
        std::mutex qMtx;
        std::condition_variable qCv;
        std::deque<std::string> queue; // guarded by qMtx
        bool stopping = false;         // guarded by qMtx
        std::thread worker;
    };
    void broadcastInvalidate(const std::string& path);
    void registerSession(ByteStream* stream, SecureChannel* chan, std::mutex* sendMutex);
    void unregisterSession(ByteStream* stream);

    void acceptLoop();
    void launchStream(std::shared_ptr<ByteStream> stream,
                      socket_t tcpSocket = FB_BAD_SOCKET);
    void handleStream(std::shared_ptr<ByteStream> stream, socket_t tcpSocket = FB_BAD_SOCKET);
    bool handshake(ByteStream& stream, std::string& clientId, SecureChannel& chan);
    bool resolve(const std::string& rel, std::string& abs);

    std::string root_;
    QByteArray authKey_;

    socket_t listen_ = FB_BAD_SOCKET;
    std::thread acceptThread_;
    std::atomic<bool> running_{false};

    std::mutex connMtx_;
    std::condition_variable connCv_;             // notified when a conn thread exits
    int connCount_ = 0;                          // live connection threads (guarded by connMtx_)
    std::unordered_set<socket_t> activeSocks_;
    std::unordered_set<std::shared_ptr<ByteStream>> activeStreams_;

    std::mutex sessMtx_;
    std::unordered_map<std::string, int> sessions_; // clientId -> active conns

    std::mutex fhMtx_;
    std::unordered_map<uint64_t, std::shared_ptr<FileHandle>> handles_;
    std::atomic<uint64_t> nextFh_{1};
    std::unordered_map<uint64_t, std::string> fhPaths_; // guarded by fhMtx_

    std::mutex broadcastMtx_;
    std::vector<std::shared_ptr<BroadcastSession>> broadcastSessions_;
};

} // namespace fb
