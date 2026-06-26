// Folder Buddies — native/browser WebRTC compatibility transport.
//
// Native↔native remains the fast TCP transport. This layer exists only so the
// native app can interoperate with the browser app over RTCDataChannel when one
// side is a browser, or when TCP publishing failed and a WebRTC route exists.
#pragma once

#include "remote_fs.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fb {

class WebRtcCompatHost {
public:
    WebRtcCompatHost();
    ~WebRtcCompatHost();

    bool start(const std::string& folder, const std::string& roomCode, bool allowWrites, std::string& err);
    void stop();
    bool running() const;
    int clientCount() const;

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
    int request(uint16_t op, const std::vector<uint8_t>& payload,
                std::vector<uint8_t>& resp) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool web_compat_available();
bool looks_like_web_compat_code(const std::string& text);

} // namespace fb
