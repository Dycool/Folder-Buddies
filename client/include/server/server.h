// Folder Buddies — file server. Serves one folder over a pool of TCP
// connections. Thread-per-connection; positioned I/O; path-confined to root.
#pragma once

#include "common.h"
#include "crypto.h"
#include "fileio.h"

#include <QByteArray>
#include <QString>
#include <atomic>
#include <condition_variable>
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

    std::vector<uint8_t> secret;       // auto-generated password (set by start)
    uint16_t boundPort = 0;
    std::string shareName;             // folder basename, baked into the token
    bool allowWrites = false;          // host-controlled permission gate

    std::atomic<uint64_t> bytesOut{0};
    std::atomic<uint64_t> bytesIn{0};
    std::function<void()> onClientsChanged;
    int clientCount();

private:
    struct BroadcastSession {
        socket_t sock;
        SecureChannel* chan;
    };
    void broadcastInvalidate(const std::string& path);
    void registerSession(socket_t s, SecureChannel* chan);
    void unregisterSession(socket_t s);

    void acceptLoop();
    void handleConn(socket_t s);
    bool handshake(socket_t s, std::string& clientId, SecureChannel& chan);
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

    std::mutex sessMtx_;
    std::unordered_map<std::string, int> sessions_; // clientId -> active conns

    std::mutex fhMtx_;
    std::unordered_map<uint64_t, std::shared_ptr<FileHandle>> handles_;
    std::atomic<uint64_t> nextFh_{1};
    std::unordered_map<uint64_t, std::string> fhPaths_; // guarded by fhMtx_

    std::mutex broadcastMtx_;
    std::vector<BroadcastSession> broadcastSessions_;
};

} // namespace fb
