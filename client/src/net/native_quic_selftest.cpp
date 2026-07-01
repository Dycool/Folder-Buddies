#include "native_quic.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

int main() {
    using namespace std::chrono_literals;
    fb::NativeQuicEndpoint server(fb::NativeQuicEndpoint::Role::Server);
    fb::NativeQuicEndpoint client(fb::NativeQuicEndpoint::Role::Client);
    std::mutex mutex;
    std::condition_variable cv;
    std::string serverDescription, clientDescription;
    std::shared_ptr<fb::ByteStream> accepted;

    server.setLocalDescriptionCallback([&](const std::string& description) {
        { std::lock_guard<std::mutex> lock(mutex); serverDescription = description; }
        cv.notify_all();
    });
    client.setLocalDescriptionCallback([&](const std::string& description) {
        { std::lock_guard<std::mutex> lock(mutex); clientDescription = description; }
        cv.notify_all();
    });
    server.setIncomingStreamCallback([&](std::shared_ptr<fb::ByteStream> stream) {
        { std::lock_guard<std::mutex> lock(mutex); accepted = std::move(stream); }
        cv.notify_all();
    });

    std::string error;
    if (!server.start(error) || !client.start(error)) {
        std::fprintf(stderr, "start failed: %s\n", error.c_str());
        return 1;
    }
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, 15s, [&] {
                return !serverDescription.empty() && !clientDescription.empty();
            })) {
            std::fprintf(stderr, "ICE candidate gathering timed out\n");
            return 1;
        }
    }
    if (!server.setRemoteDescription(clientDescription, error) ||
        !client.setRemoteDescription(serverDescription, error) ||
        !client.waitConnected(15s, error) || !server.waitConnected(15s, error)) {
        std::fprintf(stderr, "direct connection failed: %s\n", error.c_str());
        return 1;
    }

    auto streams = client.openStreams(1, error);
    const char ping[] = "ping";
    if (streams.size() != 1 || !streams[0]->write(ping, sizeof(ping))) {
        std::fprintf(stderr, "client stream failed: %s\n", error.c_str());
        return 1;
    }
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, 5s, [&] { return accepted != nullptr; })) {
            std::fprintf(stderr, "server did not accept QUIC stream\n");
            return 1;
        }
    }
    char received[sizeof(ping)]{};
    if (!accepted->read(received, sizeof(received)) ||
        std::memcmp(received, ping, sizeof(ping)) != 0 ||
        !accepted->write(received, sizeof(received))) {
        std::fprintf(stderr, "server stream exchange failed\n");
        return 1;
    }
    std::memset(received, 0, sizeof(received));
    if (!streams[0]->read(received, sizeof(received)) ||
        std::memcmp(received, ping, sizeof(ping)) != 0) {
        std::fprintf(stderr, "client stream exchange failed\n");
        return 1;
    }
    client.close();
    server.close();
    std::puts("native QUIC self-test PASS");
    return 0;
}
