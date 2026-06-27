#include "client.h"

#include "auth.h"

#include <cerrno>
#include <chrono>
#include <cstring>

namespace fb {

static socket_t connect_socket(const std::string& ip, uint16_t port, std::string& err) {
    bool v6 = ip.find(':') != std::string::npos;
    socket_t s = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
    if (s == FB_BAD_SOCKET) { err = "socket() failed"; return FB_BAD_SOCKET; }

    int rc;
    if (v6) {
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_port = htons(port);
        inet_pton(AF_INET6, ip.c_str(), &a.sin6_addr);
        rc = ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    } else {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
        rc = ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    }
    if (rc != 0) {
        err = "connect() to " + ip + ":" + std::to_string(port) + " failed";
        close_socket(s);
        return FB_BAD_SOCKET;
    }
    tune_socket(s);
    return s;
}

bool Client::handshake(Conn& c, const Token& tok, std::string& err) {
    std::vector<uint8_t> nonceC = random_bytes(16);
    QByteArray folderHash =
        QCryptographicHash::hash(QByteArray::fromStdString(tok.folder), QCryptographicHash::Sha256);
    Writer w;
    uint32_t version = kProtocolVersion;
    w.pod(version);
    w.raw(clientId_, 16);
    w.raw(folderHash.constData(), 32);
    w.raw(nonceC.data(), 16);
    if (!send_message(c.sock, OP_HELLO, 0, 1, w.b.data(), static_cast<uint32_t>(w.b.size()))) {
        err = "handshake send failed";
        return false;
    }

    MsgHeader h;
    std::vector<uint8_t> payload;
    if (!recv_message(c.sock, h, payload) || h.op != OP_CHALLENGE || payload.size() < 16) {
        err = "server rejected connection (folder/full?)";
        return false;
    }
    QByteArray nonceCQ(reinterpret_cast<char*>(nonceC.data()), 16);
    QByteArray nonceSQ(reinterpret_cast<const char*>(payload.data()), 16); // server nonce
    QByteArray key = derive_key(tok.secret);
    QByteArray proof = auth_proof(key, nonceCQ, nonceSQ);
    if (!send_message(c.sock, OP_AUTH, 0, 1,
                      reinterpret_cast<const uint8_t*>(proof.constData()),
                      static_cast<uint32_t>(proof.size()))) {
        err = "auth send failed";
        return false;
    }
    if (!recv_message(c.sock, h, payload) || h.op != OP_AUTH_OK) {
        err = "authentication failed (wrong password or token)";
        return false;
    }

    Key256 txKey, rxKey;
    derive_session_keys(key, nonceCQ, nonceSQ, /*isServer=*/false, txKey, rxKey);
    c.chan.activate(txKey, rxKey);
    return true;
}

bool Client::connect(const Token& tok, int nconns, std::string& err) {
    net_startup();
    auto rb = random_bytes(16);
    std::memcpy(clientId_, rb.data(), 16);

    for (int i = 0; i < nconns; ++i) {
        socket_t s = connect_socket(tok.ip, tok.port, err);
        if (s == FB_BAD_SOCKET) { disconnect(); return false; }
        auto c = std::make_unique<Conn>();
        c->sock = s;
        if (!handshake(*c, tok, err)) {
            close_socket(s);
            disconnect();
            return false;
        }
        c->alive = true;
        conns_.push_back(std::move(c));
    }

    connected_ = true;
    for (auto& c : conns_) {
        Conn* raw = c.get();
        raw->reader = std::thread([this, raw] { readerLoop(raw); });
    }
    return true;
}

void Client::disconnect() {
    connected_ = false;
    for (auto& c : conns_) {
        c->alive = false;
        close_socket(c->sock); // unblocks the reader's recv
        c->sock = FB_BAD_SOCKET;
    }
    for (auto& c : conns_)
        if (c->reader.joinable()) c->reader.join();
    conns_.clear();
}

void Client::readerLoop(Conn* c) {
    MsgHeader h;
    std::vector<uint8_t> payload;
    while (c->alive.load() && c->chan.recv(c->sock, h, payload)) {
        if (h.op == OP_INVALIDATE && onInvalidate) {
            Reader r(payload.data(), payload.size());
            std::string path;
            if (r.str(path)) onInvalidate(path);
            continue;
        }
        std::shared_ptr<Pending> p;
        {
            std::lock_guard<std::mutex> lk(c->pmtx);
            auto it = c->pend.find(h.req_id);
            if (it != c->pend.end()) p = it->second; // shared_ptr copy keeps it alive
        }
        if (!p) continue; // unknown or already-timed-out request id
        {
            std::lock_guard<std::mutex> lk(p->m);
            p->status = h.status;
            p->data = std::move(payload);
            p->done = true;
        }
        p->cv.notify_one();
    }
    // Connection died: fail every outstanding request so callers don't hang.
    c->alive = false;
    std::lock_guard<std::mutex> lk(c->pmtx);
    for (auto& kv : c->pend) {
        auto& p = kv.second;
        std::lock_guard<std::mutex> pl(p->m);
        p->status = EIO;
        p->done = true;
        p->cv.notify_one();
    }
}

int Client::request(uint16_t op, const std::vector<uint8_t>& payload, std::vector<uint8_t>& resp) {
    if (!connected_.load() || conns_.empty()) return EIO;
    Conn* c = conns_[rr_.fetch_add(1) % conns_.size()].get();
    if (!c->alive.load()) return EIO;

    uint64_t id = c->nextId.fetch_add(1);
    auto p = std::make_shared<Pending>();
    {
        std::lock_guard<std::mutex> lk(c->pmtx);
        if (!c->alive.load()) return EIO;
        c->pend[id] = p;
    }

    bool sent;
    {
        std::lock_guard<std::mutex> wlk(c->wmtx);
        sent = c->chan.send(c->sock, op, 0, id, payload.data(),
                            static_cast<uint32_t>(payload.size()));
    }
    if (!sent) {
        std::lock_guard<std::mutex> lk(c->pmtx);
        c->pend.erase(id);
        return EIO;
    }

    bool done;
    {
        std::unique_lock<std::mutex> lk(p->m);
        done = p->cv.wait_for(lk, std::chrono::seconds(10), [&] { return p->done; });
    }
    {
        std::lock_guard<std::mutex> lk(c->pmtx);
        c->pend.erase(id);
    }
    if (!done) return EIO; // timed out; reader keeps its own shared_ptr ref if mid-flight
    int status = p->status;
    resp = std::move(p->data);
    return status;
}

} // namespace fb
