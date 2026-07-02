#include "common.h"

namespace fb {

bool SocketByteStream::read(void* data, size_t size) {
    const socket_t socket = socket_.load();
    return socket != FB_BAD_SOCKET && recv_all(socket, data, size);
}

bool SocketByteStream::write(const void* data, size_t size) {
    const socket_t socket = socket_.load();
    return socket != FB_BAD_SOCKET && send_all(socket, data, size);
}

void SocketByteStream::close() {
    const socket_t socket = socket_.exchange(FB_BAD_SOCKET);
    if (socket == FB_BAD_SOCKET) return;
    // On Linux, close() alone does not wake a thread blocked in recv()/send()
    // on this socket (it does on Windows and macOS). shutdown() first so
    // reader threads unblock and disconnect()/stop() can join them.
#ifdef _WIN32
    ::shutdown(socket, SD_BOTH);
#else
    ::shutdown(socket, SHUT_RDWR);
#endif
    close_socket(socket);
}

} // namespace fb
