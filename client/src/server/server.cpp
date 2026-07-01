#include "server.h"

#include "auth.h"
#include "osflags.h"
#include "token.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>

#ifndef EROFS
#  define EROFS EACCES
#endif

namespace fs = std::filesystem;

namespace fb {

// ---- attribute / path helpers --------------------------------------------

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
static int64_t ft_to_unix(FILETIME f) {
    uint64_t t = (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    return static_cast<int64_t>(t / 10000000ULL) - 11644473600LL;
}
#endif

static bool fill_attr(const std::string& path, WireAttr& a) {
    std::memset(&a, 0, sizeof(a));
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!::GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &d)) return false;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
    bool dir = d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    uint64_t size = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    a.size = size;
    a.blocks = (size + 511) / 512;
    a.atime = ft_to_unix(d.ftLastAccessTime);
    a.mtime = ft_to_unix(d.ftLastWriteTime);
    a.ctime = ft_to_unix(d.ftCreationTime);
    bool ro = d.dwFileAttributes & FILE_ATTRIBUTE_READONLY;
    a.mode = (dir ? 0040000u : 0100000u) | (dir ? 0755u : (ro ? 0444u : 0644u));
    a.nlink = 1;
    return true;
#else
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) return false;
    if (S_ISLNK(st.st_mode)) return false;
    a.ino = st.st_ino;
    a.size = static_cast<uint64_t>(st.st_size);
    a.blocks = static_cast<uint64_t>(st.st_blocks);
    a.atime = st.st_atime;
    a.mtime = st.st_mtime;
    a.ctime = st.st_ctime;
    a.mode = st.st_mode;
    a.nlink = st.st_nlink;
    a.uid = st.st_uid;
    a.gid = st.st_gid;
    return true;
#endif
}

static std::string path_key(const fs::path& p) {
    std::string s = p.lexically_normal().string();
#ifdef _WIN32
    std::replace(s.begin(), s.end(), '\\', '/');
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
#endif
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

static bool path_within(const fs::path& root, const fs::path& candidate) {
    std::string r = path_key(root);
    std::string c = path_key(candidate);
    if (c == r) return true;
    if (r.empty()) return false;
    if (r == "/") return !c.empty() && c.front() == '/';
    char sep = '/';
#ifndef _WIN32
    sep = fs::path::preferred_separator;
#endif
    return c.size() > r.size() && c.rfind(r, 0) == 0 &&
           (c[r.size()] == '/' || c[r.size()] == '\\' || c[r.size()] == sep);
}

// Constant-time comparison for authentication proofs: never let the number
// of matching prefix bytes influence how long the check takes.
static bool ct_equal_bytes(const char* a, const char* b, size_t n) {
    unsigned char diff = 0;
    for (size_t i = 0; i < n; ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}

static bool is_boundary_reparse_point(const fs::path& p) {
#ifdef _WIN32
    DWORD attrs = ::GetFileAttributesW(widen(p.string()).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    std::error_code ec;
    return fs::is_symlink(fs::symlink_status(p, ec));
#endif
}

bool Server::resolve(const std::string& rel, std::string& abs) {
    fs::path base(root_);
    std::string r = rel;
    while (!r.empty() && (r.front() == '/' || r.front() == '\\')) r.erase(r.begin());
    fs::path p = base;
    for (const auto& part : fs::path(r)) {
        std::string s = part.string();
        if (s.empty() || s == ".") continue;
        if (s == "..") return false; // refuse to escape the share
        p /= s;
        std::error_code ec;
        if (fs::exists(p, ec)) {
            if (is_boundary_reparse_point(p)) return false;
            fs::path canon = fs::weakly_canonical(p, ec);
            if (ec || !path_within(base, canon)) return false;
            p = std::move(canon);
        } else if (ec) {
            return false;
        }
    }
    if (!path_within(base, p)) return false;
    abs = p.string();
    return true;
}

// ---- lifecycle ------------------------------------------------------------

int Server::clientCount() {
    std::lock_guard<std::mutex> lk(sessMtx_);
    return static_cast<int>(sessions_.size());
}

bool Server::start(const std::string& folder, int port, std::string& err) {
    net_startup();
    if (is_boundary_reparse_point(fs::path(folder))) {
        err = "Cannot host a symlink, junction, or projected filesystem root";
        return false;
    }
    root_ = fs::weakly_canonical(fs::path(folder)).string();
    if (!fs::is_directory(root_)) { err = "Not a directory: " + folder; return false; }
    shareName = fs::path(root_).filename().string();
    if (shareName.empty()) shareName = "share";

    secret = random_bytes(kSecretBytes);
    authKey_ = derive_key(secret);

    bool v6 = true;
    listen_ = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (listen_ == FB_BAD_SOCKET) {
        v6 = false;
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
    }
    if (listen_ == FB_BAD_SOCKET) { err = "socket() failed"; return false; }
    int one = 1;
    ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    if (v6) {
        int off = 0;
        ::setsockopt(listen_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off),
                     sizeof(off));
    }

    sockaddr_storage ss{};
    socklen_t addrlen;
    auto set_port = [&](uint16_t p) {
        if (v6) {
            auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
            a->sin6_family = AF_INET6;
            a->sin6_addr = in6addr_any;
            a->sin6_port = htons(p);
            addrlen = sizeof(sockaddr_in6);
        } else {
            auto* a = reinterpret_cast<sockaddr_in*>(&ss);
            a->sin_family = AF_INET;
            a->sin_addr.s_addr = INADDR_ANY;
            a->sin_port = htons(p);
            addrlen = sizeof(sockaddr_in);
        }
    };
    bool didBind = false;
    uint16_t want = static_cast<uint16_t>(port);
    int attempts = (port == 0) ? 1 : 64;
    for (int i = 0; i < attempts && !didBind; ++i, ++want) {
        if (want == 0 && port != 0) ++want; // never wrap onto the "auto" sentinel
        set_port(want);
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&ss), addrlen) == 0) didBind = true;
    }
    if (!didBind) {
        err = "bind() failed: no available port near " + std::to_string(port);
        close_socket(listen_);
        listen_ = FB_BAD_SOCKET;
        return false;
    }
    if (::listen(listen_, 64) != 0) { err = "listen() failed"; close_socket(listen_); return false; }

    sockaddr_storage bound{};
    socklen_t blen = sizeof(bound);
    ::getsockname(listen_, reinterpret_cast<sockaddr*>(&bound), &blen);
    boundPort = ntohs(bound.ss_family == AF_INET6
                          ? reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port
                          : reinterpret_cast<sockaddr_in*>(&bound)->sin_port);

    running_ = true;
    acceptThread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void Server::stop() {
    if (!running_.exchange(false)) return;
    if (listen_ != FB_BAD_SOCKET) {
        // close() alone does not reliably wake a thread blocked in accept()
        // on POSIX; shut the socket down first.
#ifdef _WIN32
        ::shutdown(listen_, SD_BOTH);
#else
        ::shutdown(listen_, SHUT_RDWR);
#endif
    }
    close_socket(listen_);
    listen_ = FB_BAD_SOCKET;
    std::vector<std::shared_ptr<ByteStream>> streams;
    {
        std::lock_guard<std::mutex> lk(connMtx_);
        streams.assign(activeStreams_.begin(), activeStreams_.end());
    }
    for (auto& stream : streams) stream->close();
    if (acceptThread_.joinable()) acceptThread_.join();
    {
        std::unique_lock<std::mutex> lk(connMtx_);
        connCv_.wait(lk, [this] { return connCount_ == 0; });
    }
    {
        std::lock_guard<std::mutex> lk(fhMtx_);
        handles_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(sessMtx_);
        sessions_.clear();
    }
}

void Server::acceptLoop() {
    while (running_.load()) {
        socket_t s = ::accept(listen_, nullptr, nullptr);
        if (s == FB_BAD_SOCKET) {
            if (!running_.load()) break;
            continue;
        }
        tune_socket(s);
        launchStream(std::make_shared<SocketByteStream>(s), s);
    }
}

// ---- handshake ------------------------------------------------------------

bool Server::handshake(ByteStream& stream, std::string& clientId, SecureChannel& chan) {
    MsgHeader h;
    std::vector<uint8_t> payload;
    if (!recv_message(stream, h, payload) || h.op != OP_HELLO) return false;

    Reader r(payload.data(), payload.size());
    uint32_t version;
    uint8_t cid[16];
    uint8_t folderHash[32];
    uint8_t nonceC[16];
    if (!r.pod(version) || !r.raw(cid, 16) || !r.raw(folderHash, 32) || !r.raw(nonceC, 16)) return false;
    QByteArray expectFolder =
        QCryptographicHash::hash(QByteArray::fromStdString(shareName), QCryptographicHash::Sha256);
    if (version != kProtocolVersion || expectFolder.size() != 32 ||
        std::memcmp(folderHash, expectFolder.constData(), 32) != 0) {
        send_message(stream, OP_AUTH_FAIL, 0, h.req_id, nullptr, 0);
        return false;
    }
    clientId.assign(reinterpret_cast<char*>(cid), 16);

    std::vector<uint8_t> nonceS = random_bytes(16);
    if (!send_message(stream, OP_CHALLENGE, 0, h.req_id, nonceS.data(), 16)) return false;

    if (!recv_message(stream, h, payload) || h.op != OP_AUTH) return false;
    QByteArray nonceCQ(reinterpret_cast<char*>(nonceC), 16);
    QByteArray nonceSQ(reinterpret_cast<char*>(nonceS.data()), 16);
    QByteArray proof(reinterpret_cast<const char*>(payload.data()), static_cast<int>(payload.size()));
    QByteArray expect = auth_proof(authKey_, nonceCQ, nonceSQ);
    if (proof.size() != expect.size() ||
        !ct_equal_bytes(proof.constData(), expect.constData(),
                        static_cast<size_t>(expect.size()))) {
        send_message(stream, OP_AUTH_FAIL, 0, h.req_id, nullptr, 0);
        return false;
    }
    send_message(stream, OP_AUTH_OK, 0, h.req_id, nullptr, 0);

    Key256 txKey, rxKey;
    derive_session_keys(authKey_, nonceCQ, nonceSQ, /*isServer=*/true, txKey, rxKey);
    chan.activate(txKey, rxKey);
    return true;
}

// ---- request dispatch -----------------------------------------------------

void Server::acceptStream(std::shared_ptr<ByteStream> stream) {
    launchStream(std::move(stream));
}

void Server::launchStream(std::shared_ptr<ByteStream> stream, socket_t tcpSocket) {
    if (!stream) return;
    {
        std::lock_guard<std::mutex> lk(connMtx_);
        if (!running_.load()) { stream->close(); return; }
        if (tcpSocket != FB_BAD_SOCKET) activeSocks_.insert(tcpSocket);
        activeStreams_.insert(stream);
        ++connCount_;
    }
    try {
        std::thread([this, stream, tcpSocket] {
            handleStream(stream, tcpSocket);
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lk(connMtx_);
        if (tcpSocket != FB_BAD_SOCKET) activeSocks_.erase(tcpSocket);
        activeStreams_.erase(stream);
        if (--connCount_ == 0) connCv_.notify_all();
        stream->close();
    }
}

void Server::handleStream(std::shared_ptr<ByteStream> stream, socket_t tcpSocket) {
    std::string clientId;
    SecureChannel chan;
    std::mutex sendMutex;
    bool authed = handshake(*stream, clientId, chan);
    if (authed) {
        bool clientAdded = false;
        {
            std::lock_guard<std::mutex> lk(sessMtx_);
            clientAdded = sessions_[clientId]++ == 0;
        }
        registerSession(stream.get(), &chan, &sendMutex);
        // Callbacks commonly query clientCount(), so never invoke one while
        // holding sessMtx_. Eight parallel transport streams are one client.
        if (clientAdded && onClientsChanged) onClientsChanged();
    }

    std::vector<uint8_t> payload;
    std::vector<uint8_t> resp;
    MsgHeader h;

    while (authed && chan.recv(*stream, h, payload)) {
        Reader r(payload.data(), payload.size());
        Writer w;
        int16_t status = 0;
        auto denyIfReadOnly = [&]() -> bool {
            if (allowWrites) return false;
            status = EROFS;
            return true;
        };

        switch (h.op) {
        case OP_GETATTR: {
            std::string path, abs;
            if (!r.str(path)) { status = EINVAL; break; }
            if (!resolve(path, abs)) { status = EACCES; break; }
            WireAttr a;
            if (!fill_attr(abs, a)) { status = ENOENT; break; }
            w.pod(a);
            break;
        }
        case OP_READDIR: {
            std::string path, abs;
            if (!r.str(path)) { status = EINVAL; break; }
            if (!resolve(path, abs)) { status = EACCES; break; }
            std::error_code ec;
            std::vector<std::pair<std::string, WireAttr>> entries;
            for (auto it = fs::directory_iterator(abs, ec);
                 !ec && it != fs::directory_iterator(); it.increment(ec)) {
                WireAttr a;
                if (fill_attr(it->path().string(), a))
                    entries.emplace_back(it->path().filename().string(), a);
            }
            if (ec) { status = ENOENT; break; }
            uint32_t n = static_cast<uint32_t>(entries.size());
            w.pod(n);
            for (auto& e : entries) { w.str(e.first); w.pod(e.second); }
            break;
        }
        case OP_OPEN:
        case OP_CREATE: {
            std::string path, abs;
            int32_t pflags = 0;
            uint32_t mode = 0;
            if (!r.str(path) || !r.pod(pflags) || !r.pod(mode)) { status = EINVAL; break; }
            bool opensForWrite = h.op == OP_CREATE ||
                                  ((pflags & FB_O_ACCMODE) == FB_O_WRONLY) ||
                                  ((pflags & FB_O_ACCMODE) == FB_O_RDWR) ||
                                  (pflags & (FB_O_CREAT | FB_O_TRUNC | FB_O_APPEND));
            if (opensForWrite && denyIfReadOnly()) break;
            if (!resolve(path, abs)) { status = EACCES; break; }
            auto fh = std::make_shared<FileHandle>();
            if (!fh->open(abs, from_portable_flags(pflags), mode)) {
#ifdef _WIN32
                std::error_code exErr;
                status = fs::exists(abs, exErr) ? EACCES : ENOENT;
#else
                status = errno ? errno : EIO;
#endif
                break;
            }
            uint64_t id = nextFh_++;
            {
                std::lock_guard<std::mutex> lk(fhMtx_);
                handles_[id] = fh;
                fhPaths_[id] = path;
            }
            w.pod(id);
            if (h.op == OP_CREATE) broadcastInvalidate(path);
            break;
        }
        case OP_READ: {
            uint64_t fhid = 0, off = 0;
            uint32_t size = 0;
            if (!r.pod(fhid) || !r.pod(off) || !r.pod(size)) { status = EINVAL; break; }
            if (size > kMaxIO) size = kMaxIO;
            std::shared_ptr<FileHandle> fh;
            {
                std::lock_guard<std::mutex> lk(fhMtx_);
                auto it = handles_.find(fhid);
                if (it != handles_.end()) fh = it->second;
            }
            if (!fh) { status = EBADF; break; }
            w.b.resize(size);
            int64_t got = fh->pread(w.b.data(), size, off);
            if (got < 0) { status = EIO; w.b.clear(); break; }
            w.b.resize(static_cast<size_t>(got));
            bytesOut += static_cast<uint64_t>(got);
            break;
        }
        case OP_WRITE: {
            if (denyIfReadOnly()) break;
            uint64_t fhid = 0, off = 0;
            if (!r.pod(fhid) || !r.pod(off)) { status = EINVAL; break; }
            uint32_t n = static_cast<uint32_t>(r.e - r.p);
            std::string writePath;
            std::shared_ptr<FileHandle> fh;
            {
                std::lock_guard<std::mutex> lk(fhMtx_);
                auto it = handles_.find(fhid);
                if (it != handles_.end()) fh = it->second;
                auto pit = fhPaths_.find(fhid);
                if (pit != fhPaths_.end()) writePath = pit->second;
            }
            if (!fh) { status = EBADF; break; }
            int64_t put = fh->pwrite(r.p, n, off);
            if (put < 0) { status = EIO; break; }
            bytesIn += static_cast<uint64_t>(put);
            uint32_t written = static_cast<uint32_t>(put);
            w.pod(written);
            if (!writePath.empty()) broadcastInvalidate(writePath);
            break;
        }
        case OP_RELEASE: {
            uint64_t fhid = 0;
            if (!r.pod(fhid)) { status = EINVAL; break; }
            std::lock_guard<std::mutex> lk(fhMtx_);
            handles_.erase(fhid);
            fhPaths_.erase(fhid);
            break;
        }
        case OP_FSYNC: {
            uint64_t fhid = 0;
            if (!r.pod(fhid)) { status = EINVAL; break; }
            std::shared_ptr<FileHandle> fh;
            {
                std::lock_guard<std::mutex> lk(fhMtx_);
                auto it = handles_.find(fhid);
                if (it != handles_.end()) fh = it->second;
            }
            if (fh && !fh->fsync_data()) status = EIO;
            break;
        }
        case OP_FLUSH:
            break; // nothing buffered server-side
        case OP_MKDIR: {
            if (denyIfReadOnly()) break;
            std::string path, abs;
            uint32_t mode = 0;
            if (!r.str(path) || !r.pod(mode)) { status = EINVAL; break; }
            if (!resolve(path, abs)) { status = EACCES; break; }
            std::error_code ec;
            if (!fs::create_directory(abs, ec)) status = ec ? EIO : EEXIST;
            if (status == 0) broadcastInvalidate(path);
            break;
        }
        case OP_UNLINK:
        case OP_RMDIR: {
            if (denyIfReadOnly()) break;
            std::string path, abs;
            if (!r.str(path)) { status = EINVAL; break; }
            if (!resolve(path, abs)) { status = EACCES; break; }
            std::error_code ec;
            if (!fs::remove(abs, ec)) status = ec ? EIO : ENOENT;
            if (status == 0) broadcastInvalidate(path);
            break;
        }
        case OP_RENAME: {
            if (denyIfReadOnly()) break;
            std::string from, to, afrom, ato;
            if (!r.str(from) || !r.str(to)) { status = EINVAL; break; }
            if (!resolve(from, afrom) || !resolve(to, ato)) { status = EACCES; break; }
            std::error_code ec;
            fs::rename(afrom, ato, ec);
            if (ec) status = EIO;
            if (status == 0) { broadcastInvalidate(from); broadcastInvalidate(to); }
            break;
        }
        case OP_TRUNCATE: {
            if (denyIfReadOnly()) break;
            std::string path, abs;
            uint64_t size = 0;
            if (!r.str(path) || !r.pod(size)) { status = EINVAL; break; }
            if (!resolve(path, abs)) { status = EACCES; break; }
            std::error_code ec;
            fs::resize_file(abs, size, ec);
            if (ec) status = EIO;
            if (status == 0) broadcastInvalidate(path);
            break;
        }
        case OP_STATFS: {
            std::string path, abs;
            if (!r.str(path)) { status = EINVAL; break; }
            if (!resolve(path, abs)) { status = EACCES; break; }
            std::error_code ec;
            fs::space_info si = fs::space(abs, ec);
            WireStatvfs sv{};
            sv.bsize = 4096;
            sv.frsize = 4096;
            sv.blocks = si.capacity / 4096;
            sv.bfree = si.free / 4096;
            sv.bavail = si.available / 4096;
            sv.namemax = 255;
            w.pod(sv);
            break;
        }
        case OP_ACCESS: {
            std::string path, abs;
            uint32_t mode = 0;
            if (!r.str(path) || !r.pod(mode)) { status = EINVAL; break; }
            if (!allowWrites && (mode & 2u)) { status = EROFS; break; }
            if (!resolve(path, abs)) { status = EACCES; break; }
            if (!fs::exists(abs)) status = ENOENT;
            break;
        }
        case OP_CHMOD: {
            if (denyIfReadOnly()) break;
            std::string path, abs;
            uint32_t mode = 0;
            if (!r.str(path) || !r.pod(mode)) { status = EINVAL; break; }
            if (!resolve(path, abs)) { status = EACCES; break; }
#ifndef _WIN32
            if (::chmod(abs.c_str(), mode) != 0) status = errno;
#endif
            if (status == 0) broadcastInvalidate(path);
            break;
        }
        case OP_UTIMENS: {
            if (denyIfReadOnly()) break;
            std::string path, abs;
            int64_t atime = 0, mtime = 0;
            if (!r.str(path) || !r.pod(atime) || !r.pod(mtime)) { status = EINVAL; break; }
            if (!resolve(path, abs)) { status = EACCES; break; }
            std::error_code ec;
            // mtime is Unix seconds, but file_clock's epoch is not the Unix
            // epoch (1601 on MSVC, 2174 on libstdc++); anchor on "now" in
            // both clocks instead of constructing file_time_type directly.
            const auto want = std::chrono::sys_seconds(std::chrono::seconds(mtime));
            const auto fileTime = fs::file_time_type::clock::now() +
                std::chrono::duration_cast<fs::file_time_type::duration>(
                    want - std::chrono::system_clock::now());
            fs::last_write_time(abs, fileTime, ec);
            (void)atime;
            if (ec) status = EIO;
            if (status == 0) broadcastInvalidate(path);
            break;
        }
        default:
            status = ENOSYS;
            break;
        }

        {
            std::lock_guard<std::mutex> sendLock(sendMutex);
            if (!chan.send(*stream, h.op, status, h.req_id, w.b.data(),
                           static_cast<uint32_t>(w.b.size())))
                break;
        }
    }

    // Close before unregistering: unblocks this session's invalidation
    // sender if it is stalled mid-write, so joining it cannot hang.
    stream->close();
    unregisterSession(stream.get());
    if (authed) {
        bool clientRemoved = false;
        {
            std::lock_guard<std::mutex> lk(sessMtx_);
            auto it = sessions_.find(clientId);
            if (it != sessions_.end() && --it->second <= 0) {
                sessions_.erase(it);
                clientRemoved = true;
            }
        }
        if (clientRemoved && onClientsChanged) onClientsChanged();
    }
    {
        std::lock_guard<std::mutex> lk(connMtx_);
        if (tcpSocket != FB_BAD_SOCKET) activeSocks_.erase(tcpSocket);
        activeStreams_.erase(stream);
        if (--connCount_ == 0) connCv_.notify_all();
    }
    stream->close();
}

// ---- cross-client invalidation broadcast -----------------------------------

namespace {
// Invalidations are advisory (client caches expire via TTL within seconds),
// so a slow consumer sheds the oldest entries instead of blocking anyone.
constexpr size_t kMaxQueuedInvalidations = 256;
} // namespace

void Server::registerSession(ByteStream* stream, SecureChannel* chan, std::mutex* sendMutex) {
    auto bs = std::make_shared<BroadcastSession>();
    bs->stream = stream;
    bs->chan = chan;
    bs->sendMutex = sendMutex;
    bs->worker = std::thread([bs] {
        std::unique_lock<std::mutex> lk(bs->qMtx);
        for (;;) {
            bs->qCv.wait(lk, [&] { return bs->stopping || !bs->queue.empty(); });
            if (bs->stopping) break;
            std::string path = std::move(bs->queue.front());
            bs->queue.pop_front();
            lk.unlock();
            Writer w;
            w.str(path);
            {
                // Only this session's send lock: a stalled socket here blocks
                // nothing but this session's own invalidation sender.
                std::lock_guard<std::mutex> sendLock(*bs->sendMutex);
                bs->chan->send(*bs->stream, OP_INVALIDATE, 0, 0, w.b.data(),
                               static_cast<uint32_t>(w.b.size()));
            }
            lk.lock();
        }
    });
    std::lock_guard<std::mutex> lk(broadcastMtx_);
    broadcastSessions_.push_back(std::move(bs));
}

void Server::unregisterSession(ByteStream* stream) {
    std::shared_ptr<BroadcastSession> victim;
    {
        std::lock_guard<std::mutex> lk(broadcastMtx_);
        auto it = std::find_if(broadcastSessions_.begin(), broadcastSessions_.end(),
            [stream](const std::shared_ptr<BroadcastSession>& bs) { return bs->stream == stream; });
        if (it != broadcastSessions_.end()) {
            victim = *it;
            broadcastSessions_.erase(it);
        }
    }
    if (!victim) return;
    {
        std::lock_guard<std::mutex> lk(victim->qMtx);
        victim->stopping = true;
    }
    victim->qCv.notify_all();
    // The caller closes the stream before unregistering, so a sender blocked
    // mid-write is released and this join cannot hang.
    if (victim->worker.joinable()) victim->worker.join();
}

void Server::broadcastInvalidate(const std::string& path) {
    std::lock_guard<std::mutex> lk(broadcastMtx_);
    for (auto& bs : broadcastSessions_) {
        {
            std::lock_guard<std::mutex> qlk(bs->qMtx);
            if (bs->stopping) continue;
            if (bs->queue.size() >= kMaxQueuedInvalidations) bs->queue.pop_front();
            bs->queue.push_back(path);
        }
        bs->qCv.notify_one();
    }
}

} // namespace fb
