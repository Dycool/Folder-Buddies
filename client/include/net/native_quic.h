#pragma once

#include "common.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fb {

// Native-only QUIC endpoint. libjuice performs ICE/STUN and carries quiche's
// UDP datagrams over the selected direct candidate pair. No TURN server is
// configured, so application data can never be relayed.
class NativeQuicEndpoint {
public:
    enum class Role { Client, Server };

    explicit NativeQuicEndpoint(Role role);
    ~NativeQuicEndpoint();

    NativeQuicEndpoint(const NativeQuicEndpoint&) = delete;
    NativeQuicEndpoint& operator=(const NativeQuicEndpoint&) = delete;

    void setLocalDescriptionCallback(std::function<void(const std::string&)> cb);
    void setIncomingStreamCallback(std::function<void(std::shared_ptr<ByteStream>)> cb);
    void setStateCallback(std::function<void(const std::string&)> cb);

    bool start(std::string& err);
    bool setRemoteDescription(const std::string& description, std::string& err);
    bool waitConnected(std::chrono::milliseconds timeout, std::string& err);
    std::vector<std::shared_ptr<ByteStream>> openStreams(size_t count, std::string& err);
    bool connected() const;
    void close();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

bool native_quic_available();

} // namespace fb
