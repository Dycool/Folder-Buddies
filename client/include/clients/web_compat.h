#pragma once

#include "remote_fs.h"
#include "token.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fb {

class Server;

class WebRtcCompatHost {
public:
    WebRtcCompatHost();
    ~WebRtcCompatHost();

    bool start(const std::string& folder, const std::string& roomCode, bool allowWrites,
               Server* nativeServer, std::string& err);
    void stop();
    bool running() const;
    int clientCount() const;

    std::atomic<uint64_t> bytesOut{0};
    std::atomic<uint64_t> bytesIn{0};
    std::function<void()> onClientsChanged;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Native QUIC client using the same room WebSocket for ICE descriptions. The
// filesystem protocol is the native binary protocol, not WebRTC/DataChannels.
class NativeQuicRemoteClient : public RemoteFs {
public:
    NativeQuicRemoteClient();
    ~NativeQuicRemoteClient() override;
    bool connect(const std::string& roomCode, const Token& token, std::string& err);
    void disconnect();
    bool connected() const override;
    int request(uint16_t op, const std::vector<uint8_t>& payload,
                std::vector<uint8_t>& resp) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class WebRtcRemoteClient : public RemoteFs {
public:
    WebRtcRemoteClient();
    ~WebRtcRemoteClient() override;

    bool connect(const std::string& webCodeOrRoom, std::string& err);
    void disconnect();
    bool connected() const override;
    bool canWrite() const;
    int request(uint16_t op, const std::vector<uint8_t>& payload,
                std::vector<uint8_t>& resp) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool web_compat_available();
bool looks_like_web_compat_code(const std::string& text);

} // namespace fb
