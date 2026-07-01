// Folder Buddies — shared wire protocol, framing, and cross-platform socket helpers.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socket_t = SOCKET;
#  define FB_BAD_SOCKET INVALID_SOCKET
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <unistd.h>
using socket_t = int;
#  define FB_BAD_SOCKET (-1)
#endif

namespace fb {

// Reliable ordered byte stream used by the wire protocol. TCP and native
// QUIC streams both implement this interface, keeping framing, authentication,
// and filesystem operations transport-independent.
class ByteStream {
public:
    virtual ~ByteStream() = default;
    virtual bool read(void* data, size_t size) = 0;
    virtual bool write(const void* data, size_t size) = 0;
    virtual void close() = 0;
};

class SocketByteStream final : public ByteStream {
public:
    explicit SocketByteStream(socket_t socket) : socket_(socket) {}
    ~SocketByteStream() override { close(); }
    bool read(void* data, size_t size) override;
    bool write(const void* data, size_t size) override;
    void close() override;
    socket_t socket() const { return socket_.load(); }

private:
    std::atomic<socket_t> socket_{FB_BAD_SOCKET};
};

constexpr uint32_t kMagic = 0x46424459; // "FBDY"
constexpr uint32_t kProtocolVersion = 2; // v2: post-handshake ChaCha20-Poly1305
constexpr uint32_t kMaxIO = 1u << 20;   // 1 MiB read/write chunk
constexpr int kDefaultConns = 8;        // automatic parallel TCP streams per client

constexpr uint32_t kMaxHandshakeMsg = 64u * 1024;

// Operation codes. Auth ops < 10, filesystem ops >= 10.
enum Op : uint16_t {
    OP_HELLO = 1,
    OP_CHALLENGE = 2,
    OP_AUTH = 3,
    OP_AUTH_OK = 4,
    OP_AUTH_FAIL = 5,

    OP_GETATTR = 10,
    OP_READDIR = 11,
    OP_OPEN = 12,
    OP_READ = 13,
    OP_WRITE = 14,
    OP_CREATE = 15,
    OP_MKDIR = 16,
    OP_UNLINK = 17,
    OP_RMDIR = 18,
    OP_RENAME = 19,
    OP_TRUNCATE = 20,
    OP_RELEASE = 21,
    OP_FSYNC = 22,
    OP_STATFS = 23,
    OP_UTIMENS = 24,
    OP_CHMOD = 25,
    OP_FLUSH = 26,
    OP_ACCESS = 27,
    OP_INVALIDATE = 28,
};

#pragma pack(push, 1)
struct MsgHeader {
    uint32_t magic;   // kMagic
    uint16_t op;      // Op
    int16_t status;   // response: 0 ok, else positive errno; request: 0
    uint64_t req_id;  // correlates request/response on a connection
    uint32_t length;  // payload bytes following this header
};

// Portable file attributes. Times are seconds since epoch.
struct WireAttr {
    uint64_t ino;
    uint64_t size;
    uint64_t blocks;
    int64_t atime;
    int64_t mtime;
    int64_t ctime;
    uint32_t mode; // POSIX st_mode (type + perms)
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
};

struct WireStatvfs {
    uint64_t bsize;
    uint64_t frsize;
    uint64_t blocks;
    uint64_t bfree;
    uint64_t bavail;
    uint64_t files;
    uint64_t ffree;
    uint64_t namemax;
};
#pragma pack(pop)

// ---- payload (de)serialization -------------------------------------------

struct Writer {
    std::vector<uint8_t> b;
    void raw(const void* p, size_t n) {
        size_t s = b.size();
        b.resize(s + n);
        std::memcpy(b.data() + s, p, n);
    }
    template <class T>
    void pod(const T& v) { raw(&v, sizeof(T)); }
    void str(const std::string& s) {
        uint32_t n = static_cast<uint32_t>(s.size());
        pod(n);
        raw(s.data(), n);
    }
    void bytes(const void* p, uint32_t n) {
        pod(n);
        raw(p, n);
    }
};

struct Reader {
    const uint8_t* p;
    const uint8_t* e;
    Reader(const uint8_t* d, size_t n) : p(d), e(d + n) {}
    bool raw(void* o, size_t n) {
        if (static_cast<size_t>(e - p) < n) return false;
        std::memcpy(o, p, n);
        p += n;
        return true;
    }
    template <class T>
    bool pod(T& v) { return raw(&v, sizeof(T)); }
    bool str(std::string& s) {
        uint32_t n;
        if (!pod(n)) return false;
        if (static_cast<size_t>(e - p) < n) return false;
        s.assign(reinterpret_cast<const char*>(p), n);
        p += n;
        return true;
    }
    bool bytes(std::vector<uint8_t>& out) {
        uint32_t n;
        if (!pod(n)) return false;
        if (static_cast<size_t>(e - p) < n) return false;
        out.assign(p, p + n);
        p += n;
        return true;
    }
};

// ---- blocking socket I/O --------------------------------------------------

inline bool recv_all(socket_t s, void* buf, size_t n) {
    char* p = static_cast<char*>(buf);
    size_t got = 0;
    while (got < n) {
        int r = ::recv(s, p + got, static_cast<int>(n - got), 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

inline bool send_all(socket_t s, const void* buf, size_t n) {
    const char* p = static_cast<const char*>(buf);
    size_t sent = 0;
    while (sent < n) {
        int r = ::send(s, p + sent, static_cast<int>(n - sent), 0);
        if (r <= 0) return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}

inline bool recv_all(ByteStream& stream, void* buf, size_t n) {
    return stream.read(buf, n);
}

inline bool send_all(ByteStream& stream, const void* buf, size_t n) {
    return stream.write(buf, n);
}

inline void close_socket(socket_t s) {
    if (s == FB_BAD_SOCKET) return;
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
}

// Latency + throughput tuning applied to every data socket.
inline void tune_socket(socket_t s) {
    int one = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    int buf = 4 * 1024 * 1024;
    ::setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));
    ::setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));
#ifdef SO_REUSEADDR
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
#endif
}

inline bool net_startup() {
#ifdef _WIN32
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) == 0;
#else
    return true;
#endif
}

// Read a full framed message (header + payload). Returns false on disconnect.
inline bool recv_message(socket_t s, MsgHeader& h, std::vector<uint8_t>& payload) {
    if (!recv_all(s, &h, sizeof(h))) return false;
    if (h.magic != kMagic) return false;
    if (h.length > kMaxHandshakeMsg) return false; // reject oversized pre-auth frames
    payload.resize(h.length);
    if (h.length && !recv_all(s, payload.data(), h.length)) return false;
    return true;
}

// Write a framed message under the caller's responsibility for locking.
inline bool send_message(socket_t s, uint16_t op, int16_t status, uint64_t req_id,
                         const uint8_t* payload, uint32_t len) {
    MsgHeader h{kMagic, op, status, req_id, len};
    if (!send_all(s, &h, sizeof(h))) return false;
    if (len && !send_all(s, payload, len)) return false;
    return true;
}

inline bool recv_message(ByteStream& stream, MsgHeader& h, std::vector<uint8_t>& payload) {
    if (!stream.read(&h, sizeof(h))) return false;
    if (h.magic != kMagic || h.length > kMaxHandshakeMsg) return false;
    payload.resize(h.length);
    return !h.length || stream.read(payload.data(), h.length);
}

inline bool send_message(ByteStream& stream, uint16_t op, int16_t status, uint64_t req_id,
                         const uint8_t* payload, uint32_t len) {
    MsgHeader h{kMagic, op, status, req_id, len};
    if (!stream.write(&h, sizeof(h))) return false;
    return !len || stream.write(payload, len);
}

} // namespace fb
