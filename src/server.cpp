#include "server.h"

#include "auth.h"
#include "osflags.h"
#include "token.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>

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
    }
    abs = p.string();
    return true;
}

// ---- lifecycle ------------------------------------------------------------

int Server::clientCount() {
    std::lock_guard<std::mutex> lk(sessMtx_);
    return static_cast<int>(sessions_.size());
}

bool Server::start(const std::string& folder, int port, int maxClients, std::string& err) {
    net_startup();
    root_ = fs::weakly_canonical(fs::path(folder)).string();
    if (!fs::is_directory(root_)) { err = "Not a directory: " + folder; return false; }
    shareName = fs::path(root_).filename().string();
    if (shareName.empty()) shareName = "share";

    secret = random_bytes(kSecretBytes); // the auto-generated password baked into the code
    authKey_ = derive_key(secret);
    maxClients_ = maxClients;

    // Listen on IPv6 in dual-stack mode (IPV6_V6ONLY off) so one socket accepts
    // both IPv6 and IPv4-mapped clients — IPv6 is preferred, IPv4 still works.
    // Fall back to a plain IPv4 socket on the rare host without IPv6.
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

    // Find an available port. Port 0 lets the OS pick a free ephemeral one,
    // which is how two instances on the same machine end up on different ports
    // without coordinating. If a *specific* port is requested but busy, scan
    // upward so a second instance can still start. The bound port is read back
    // and baked into the share code, so each instance is independently reachable.
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
    close_socket(listen_);
    listen_ = FB_BAD_SOCKET;
    {
        std::lock_guard<std::mutex> lk(connMtx_);
        for (socket_t s : activeSocks_) close_socket(s);
    }
    if (acceptThread_.joinable()) acceptThread_.join();
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lk(connMtx_);
        threads.swap(connThreads_);
    }
    for (auto& t : threads) if (t.joinable()) t.join();
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
        std::lock_guard<std::mutex> lk(connMtx_);
        activeSocks_.insert(s);
        connThreads_.emplace_back([this, s] { handleConn(s); });
    }
}

// ---- handshake ------------------------------------------------------------

bool Server::handshake(socket_t s, std::string& clientId, SecureChannel& chan) {
    MsgHeader h;
    std::vector<uint8_t> payload;
    if (!recv_message(s, h, payload) || h.op != OP_HELLO) return false;

    Reader r(payload.data(), payload.size());
    uint32_t version;
    uint8_t cid[16];
    std::string folder;
    uint8_t nonceC[16];
    if (!r.pod(version) || !r.raw(cid, 16) || !r.str(folder) || !r.raw(nonceC, 16)) return false;
    if (version != kProtocolVersion || folder != shareName) {
        send_message(s, OP_AUTH_FAIL, 0, h.req_id, nullptr, 0);
        return false;
    }
    clientId.assign(reinterpret_cast<char*>(cid), 16);

    // Enforce max clients (count distinct client ids).
    {
        std::lock_guard<std::mutex> lk(sessMtx_);
        if (maxClients_ > 0 && sessions_.find(clientId) == sessions_.end() &&
            static_cast<int>(sessions_.size()) >= maxClients_) {
            send_message(s, OP_AUTH_FAIL, 0, h.req_id, nullptr, 0);
            return false;
        }
    }

    std::vector<uint8_t> nonceS = random_bytes(16);
    if (!send_message(s, OP_CHALLENGE, 0, h.req_id, nonceS.data(), 16)) return false;

    if (!recv_message(s, h, payload) || h.op != OP_AUTH) return false;
    QByteArray nonceCQ(reinterpret_cast<char*>(nonceC), 16);
    QByteArray nonceSQ(reinterpret_cast<char*>(nonceS.data()), 16);
    QByteArray proof(reinterpret_cast<const char*>(payload.data()), static_cast<int>(payload.size()));
    QByteArray expect = auth_proof(authKey_, nonceCQ, nonceSQ);
    if (proof.size() != expect.size() ||
        ::memcmp(proof.constData(), expect.constData(), expect.size()) != 0) {
        send_message(s, OP_AUTH_FAIL, 0, h.req_id, nullptr, 0);
        return false;
    }
    send_message(s, OP_AUTH_OK, 0, h.req_id, nullptr, 0);

    // Both sides now hold the same authenticated material; seal the channel.
    Key256 txKey, rxKey;
    derive_session_keys(authKey_, nonceCQ, nonceSQ, /*isServer=*/true, txKey, rxKey);
    chan.activate(txKey, rxKey);
    return true;
}

// ---- request dispatch -----------------------------------------------------

void Server::handleConn(socket_t s) {
    std::string clientId;
    SecureChannel chan;
    bool authed = handshake(s, clientId, chan);
    if (authed) {
        std::lock_guard<std::mutex> lk(sessMtx_);
        sessions_[clientId]++;
        if (onClientsChanged) onClientsChanged();
    }

    std::vector<uint8_t> payload;
    std::vector<uint8_t> resp;
    MsgHeader h;

    while (authed && chan.recv(s, h, payload)) {
        Reader r(payload.data(), payload.size());
        Writer w;
        int16_t status = 0;

        switch (h.op) {
        case OP_GETATTR: {
            std::string path, abs;
            r.str(path);
            if (!resolve(path, abs)) { status = EACCES; break; }
            WireAttr a;
            if (!fill_attr(abs, a)) { status = ENOENT; break; }
            w.pod(a);
            break;
        }
        case OP_READDIR: {
            std::string path, abs;
            r.str(path);
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
            int32_t pflags;
            uint32_t mode;
            r.str(path);
            r.pod(pflags);
            r.pod(mode);
            if (!resolve(path, abs)) { status = EACCES; break; }
            auto fh = std::make_shared<FileHandle>();
            if (!fh->open(abs, from_portable_flags(pflags), mode)) {
#ifdef _WIN32
                status = fs::exists(abs) ? EACCES : ENOENT;
#else
                status = errno ? errno : EIO;
#endif
                break;
            }
            uint64_t id = nextFh_++;
            {
                std::lock_guard<std::mutex> lk(fhMtx_);
                handles_[id] = fh;
            }
            w.pod(id);
            break;
        }
        case OP_READ: {
            uint64_t fhid, off;
            uint32_t size;
            r.pod(fhid);
            r.pod(off);
            r.pod(size);
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
            uint64_t fhid, off;
            r.pod(fhid);
            r.pod(off);
            uint32_t n = static_cast<uint32_t>(r.e - r.p);
            std::shared_ptr<FileHandle> fh;
            {
                std::lock_guard<std::mutex> lk(fhMtx_);
                auto it = handles_.find(fhid);
                if (it != handles_.end()) fh = it->second;
            }
            if (!fh) { status = EBADF; break; }
            int64_t put = fh->pwrite(r.p, n, off);
            if (put < 0) { status = EIO; break; }
            bytesIn += static_cast<uint64_t>(put);
            uint32_t written = static_cast<uint32_t>(put);
            w.pod(written);
            break;
        }
        case OP_RELEASE: {
            uint64_t fhid;
            r.pod(fhid);
            std::lock_guard<std::mutex> lk(fhMtx_);
            handles_.erase(fhid);
            break;
        }
        case OP_FSYNC: {
            uint64_t fhid;
            r.pod(fhid);
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
            std::string path, abs;
            uint32_t mode;
            r.str(path);
            r.pod(mode);
            if (!resolve(path, abs)) { status = EACCES; break; }
            std::error_code ec;
            if (!fs::create_directory(abs, ec)) status = ec ? EIO : EEXIST;
            break;
        }
        case OP_UNLINK:
        case OP_RMDIR: {
            std::string path, abs;
            r.str(path);
            if (!resolve(path, abs)) { status = EACCES; break; }
            std::error_code ec;
            if (!fs::remove(abs, ec)) status = ec ? EIO : ENOENT;
            break;
        }
        case OP_RENAME: {
            std::string from, to, afrom, ato;
            r.str(from);
            r.str(to);
            if (!resolve(from, afrom) || !resolve(to, ato)) { status = EACCES; break; }
            std::error_code ec;
            fs::rename(afrom, ato, ec);
            if (ec) status = EIO;
            break;
        }
        case OP_TRUNCATE: {
            std::string path, abs;
            uint64_t size;
            r.str(path);
            r.pod(size);
            if (!resolve(path, abs)) { status = EACCES; break; }
            std::error_code ec;
            fs::resize_file(abs, size, ec);
            if (ec) status = EIO;
            break;
        }
        case OP_STATFS: {
            std::string path, abs;
            r.str(path);
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
            uint32_t mode;
            r.str(path);
            r.pod(mode);
            if (!resolve(path, abs)) { status = EACCES; break; }
            if (!fs::exists(abs)) status = ENOENT;
            break;
        }
        case OP_CHMOD: {
            std::string path, abs;
            uint32_t mode;
            r.str(path);
            r.pod(mode);
            if (!resolve(path, abs)) { status = EACCES; break; }
#ifndef _WIN32
            if (::chmod(abs.c_str(), mode) != 0) status = errno;
#endif
            break;
        }
        case OP_UTIMENS: {
            std::string path, abs;
            int64_t atime, mtime;
            r.str(path);
            r.pod(atime);
            r.pod(mtime);
            if (!resolve(path, abs)) { status = EACCES; break; }
            std::error_code ec;
            fs::last_write_time(
                abs, fs::file_time_type(std::chrono::seconds(mtime)), ec);
            (void)atime;
            if (ec) status = EIO;
            break;
        }
        default:
            status = ENOSYS;
            break;
        }

        if (!chan.send(s, h.op, status, h.req_id, w.b.data(),
                       static_cast<uint32_t>(w.b.size())))
            break;
    }

    if (authed) {
        std::lock_guard<std::mutex> lk(sessMtx_);
        if (--sessions_[clientId] <= 0) sessions_.erase(clientId);
        if (onClientsChanged) onClientsChanged();
    }
    {
        std::lock_guard<std::mutex> lk(connMtx_);
        activeSocks_.erase(s);
    }
    close_socket(s);
}

} // namespace fb
