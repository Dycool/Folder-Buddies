#include "native_quic.h"

#ifdef FB_HAVE_NATIVE_QUIC

#include <juice/juice.h>
#include <picoquic.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <QByteArray>
#include <QFile>
#include <QTemporaryFile>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace fb {
namespace {

constexpr size_t kMaxDatagram = 1350;
constexpr uint64_t kIdleTimeoutMs = 30000;
constexpr uint64_t kStreamWindow = 16u * 1024u * 1024u;
constexpr uint64_t kConnectionWindow = 128u * 1024u * 1024u;
constexpr char kAlpn[] = "fbq";

sockaddr_storage dummy_address(bool server, bool local, socklen_t& len) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool first = server == local;
    a.sin_port = htons(first ? 41000 : 41001);
    sockaddr_storage out{};
    std::memcpy(&out, &a, sizeof(a));
    len = sizeof(a);
    return out;
}

bool make_transport_identity(QByteArray& certificate, QByteArray& privateKey,
                             std::string& err) {
    EVP_PKEY_CTX* keyContext = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    BIO* certBio = nullptr;
    BIO* keyBio = nullptr;
    bool ok = keyContext && EVP_PKEY_keygen_init(keyContext) > 0 &&
              EVP_PKEY_CTX_set_ec_paramgen_curve_nid(
                  keyContext, NID_X9_62_prime256v1) > 0 &&
              EVP_PKEY_keygen(keyContext, &key) > 0;
    if (ok) {
        cert = X509_new();
        ok = cert && X509_set_version(cert, 2) == 1 &&
             ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) == 1 &&
             X509_gmtime_adj(X509_get_notBefore(cert), -60) != nullptr &&
             X509_gmtime_adj(X509_get_notAfter(cert), 24 * 60 * 60) != nullptr &&
             X509_set_pubkey(cert, key) == 1;
    }
    if (ok) {
        X509_NAME* name = X509_get_subject_name(cert);
        ok = name && X509_NAME_add_entry_by_txt(
                         name, "CN", MBSTRING_ASC,
                         reinterpret_cast<const unsigned char*>("folderbuddies"),
                         -1, -1, 0) == 1 &&
             X509_set_issuer_name(cert, name) == 1 &&
             X509_sign(cert, key, EVP_sha256()) > 0;
    }
    if (ok) {
        certBio = BIO_new(BIO_s_mem());
        keyBio = BIO_new(BIO_s_mem());
        ok = certBio && keyBio && PEM_write_bio_X509(certBio, cert) == 1 &&
             PEM_write_bio_PrivateKey(keyBio, key, nullptr, nullptr, 0, nullptr, nullptr) == 1;
    }
    if (ok) {
        char* certData = nullptr;
        char* keyData = nullptr;
        const long certLength = BIO_get_mem_data(certBio, &certData);
        const long keyLength = BIO_get_mem_data(keyBio, &keyData);
        ok = certLength > 0 && keyLength > 0;
        if (ok) {
            certificate = QByteArray(certData, static_cast<int>(certLength));
            privateKey = QByteArray(keyData, static_cast<int>(keyLength));
        }
    }
    BIO_free(certBio);
    BIO_free(keyBio);
    X509_free(cert);
    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(keyContext);
    if (!ok) err = "failed to generate an ephemeral QUIC transport certificate";
    return ok;
}

} // namespace

struct NativeQuicEndpoint::Impl : std::enable_shared_from_this<Impl> {
    struct StreamState {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<uint8_t> rx;
        std::deque<uint8_t> tx;
        bool localClosed = false;
        bool remoteClosed = false;
        bool finSent = false;
    };

    class Stream final : public ByteStream {
    public:
        Stream(std::shared_ptr<Impl> owner, uint64_t id, std::shared_ptr<StreamState> state)
            : owner_(std::move(owner)), id_(id), state_(std::move(state)) {}
        bool read(void* data, size_t size) override {
            auto* out = static_cast<uint8_t*>(data);
            size_t done = 0;
            std::unique_lock<std::mutex> lock(state_->mutex);
            while (done < size) {
                state_->cv.wait(lock, [&] {
                    return !state_->rx.empty() || state_->remoteClosed || state_->localClosed;
                });
                while (done < size && !state_->rx.empty()) {
                    out[done++] = state_->rx.front();
                    state_->rx.pop_front();
                }
                if (done < size && (state_->remoteClosed || state_->localClosed)) return false;
            }
            return true;
        }
        bool write(const void* data, size_t size) override {
            const auto* in = static_cast<const uint8_t*>(data);
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                if (state_->localClosed || state_->remoteClosed) return false;
                state_->tx.insert(state_->tx.end(), in, in + size);
            }
            if (auto owner = owner_.lock()) owner->wake();
            return true;
        }
        void close() override {
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                state_->localClosed = true;
            }
            state_->cv.notify_all();
            if (auto owner = owner_.lock()) owner->wake();
        }
    private:
        std::weak_ptr<Impl> owner_;
        uint64_t id_;
        std::shared_ptr<StreamState> state_;
    };

    explicit Impl(Role r) : role(r) {}
    ~Impl() { shutdown(); }

    Role role;
    juice_agent_t* ice = nullptr;
    picoquic_quic_t* quic = nullptr;
    picoquic_cnx_t* conn = nullptr;
    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> datagrams;
    std::deque<std::vector<uint8_t>> pendingSends; // QUIC worker thread only
    std::map<uint64_t, std::shared_ptr<StreamState>> streams;
    std::mutex streamsMutex;
    bool stopping = false;
    bool workPending = false;
    std::atomic<bool> iceConnected{false};
    bool established = false;
    std::atomic<bool> failed{false};
    std::string failure;
    std::function<void(const std::string&)> localDescription;
    std::function<void(std::shared_ptr<ByteStream>)> incomingStream;
    std::function<void(const std::string&)> stateChanged;

    void wake() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            workPending = true;
        }
        cv.notify_all();
    }

    static void ice_state(juice_agent_t*, juice_state_t state, void* ptr) {
        auto* self = static_cast<Impl*>(ptr);
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            if (state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED) {
                self->iceConnected.store(true);
            } else {
                // ICE may return to negotiation while resolving a role
                // conflict. Do not let a transient CONNECTED notification
                // authorize QUIC packets after the selected path was revoked.
                self->iceConnected.store(false);
            }
            if (state == JUICE_STATE_FAILED) {
                self->failure = "ICE/STUN could not establish a direct UDP path";
                self->failed = true;
            }
            self->workPending = true;
        }
        if (self->stateChanged) self->stateChanged(juice_state_to_string(state));
        self->cv.notify_all();
    }

    static void ice_gathered(juice_agent_t* agent, void* ptr) {
        auto* self = static_cast<Impl*>(ptr);
        std::array<char, JUICE_MAX_SDP_STRING_LEN> sdp{};
        if (juice_get_local_description(agent, sdp.data(), sdp.size()) == JUICE_ERR_SUCCESS &&
            self->localDescription) self->localDescription(sdp.data());
    }

    static void ice_recv(juice_agent_t*, const char* data, size_t size, void* ptr) {
        auto* self = static_cast<Impl*>(ptr);
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            self->datagrams.emplace_back(reinterpret_cast<const uint8_t*>(data),
                                         reinterpret_cast<const uint8_t*>(data) + size);
            self->workPending = true;
        }
        self->cv.notify_all();
    }

    static int quic_event(picoquic_cnx_t* cnx, uint64_t streamId, uint8_t* bytes,
                          size_t length, picoquic_call_back_event_t event,
                          void* callbackContext, void*) {
        auto* self = static_cast<Impl*>(callbackContext);
        if (!self) return 0;
        if (!self->conn) self->conn = cnx;

        switch (event) {
        case picoquic_callback_stream_data:
        case picoquic_callback_stream_fin: {
            auto state = self->ensure_stream(streamId, self->role == Role::Server);
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->rx.insert(state->rx.end(), bytes, bytes + length);
                if (event == picoquic_callback_stream_fin) state->remoteClosed = true;
            }
            state->cv.notify_all();
            break;
        }
        case picoquic_callback_stream_reset:
        case picoquic_callback_stop_sending: {
            auto state = self->ensure_stream(streamId, false);
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->remoteClosed = true;
            }
            state->cv.notify_all();
            break;
        }
        case picoquic_callback_ready:
            self->mark_established();
            break;
        case picoquic_callback_close:
        case picoquic_callback_application_close:
        case picoquic_callback_stateless_reset:
            self->set_failure("QUIC connection closed");
            break;
        default:
            break;
        }
        return 0;
    }

    bool initialize(std::string& err) {
        QByteArray certPath, keyPath;
        QString certFileName, keyFileName;
        if (role == Role::Server) {
            // Picotls opens certificate paths with the MSVC CRT. Destroy the
            // QTemporaryFile wrappers before that open so their Windows file
            // handles cannot cause a sharing violation.
            {
                QTemporaryFile certFile, keyFile;
                QByteArray certificate, privateKey;
                if (!make_transport_identity(certificate, privateKey, err)) return false;
                if (!certFile.open() || !keyFile.open() ||
                    certFile.write(certificate) < 0 || keyFile.write(privateKey) < 0) {
                    err = "failed to materialize the ephemeral QUIC certificate";
                    return false;
                }
                certFile.flush(); keyFile.flush();
                certFileName = certFile.fileName();
                keyFileName = keyFile.fileName();
                certPath = certFileName.toLocal8Bit();
                keyPath = keyFileName.toLocal8Bit();
#ifdef _WIN32
                certPath.replace('/', '\\');
                keyPath.replace('/', '\\');
#endif
                certFile.setAutoRemove(false);
                keyFile.setAutoRemove(false);
            }
        }
        const uint64_t now = picoquic_current_time();
        quic = picoquic_create(
            1,
            role == Role::Server ? certPath.constData() : nullptr,
            role == Role::Server ? keyPath.constData() : nullptr,
            nullptr, kAlpn, &Impl::quic_event, this,
            nullptr, nullptr, nullptr, now, nullptr, nullptr, nullptr, 0);
        if (role == Role::Server) {
            QFile::remove(certFileName);
            QFile::remove(keyFileName);
        }
        if (!quic) {
            err = "failed to create picoquic context";
            return false;
        }
        picoquic_disable_port_blocking(quic, 1);
        picoquic_set_default_pmtud_policy(quic, picoquic_pmtud_blocked);
        picoquic_set_default_tp_value(quic, picoquic_tp_idle_timeout, kIdleTimeoutMs);
        picoquic_set_default_tp_value(quic, picoquic_tp_initial_max_data, kConnectionWindow);
        picoquic_set_default_tp_value(quic, picoquic_tp_initial_max_stream_data_bidi_local,
                                      kStreamWindow);
        picoquic_set_default_tp_value(quic, picoquic_tp_initial_max_stream_data_bidi_remote,
                                      kStreamWindow);
        picoquic_set_default_tp_value(quic, picoquic_tp_initial_max_streams_bidi, 64);
        picoquic_set_default_tp_value(quic, picoquic_tp_disable_migration, 1);

        juice_config_t jc{};
        jc.concurrency_mode = JUICE_CONCURRENCY_MODE_THREAD;
        jc.stun_server_host = "stun.l.google.com";
        jc.stun_server_port = 19302;
        jc.cb_state_changed = &Impl::ice_state;
        jc.cb_gathering_done = &Impl::ice_gathered;
        jc.cb_recv = &Impl::ice_recv;
        jc.user_ptr = this;
        ice = juice_create(&jc);
        if (!ice) { err = "failed to create ICE agent"; picoquic_free(quic); quic = nullptr; return false; }
        worker = std::thread([self = shared_from_this()] { self->run(); });
        if (juice_gather_candidates(ice) != JUICE_ERR_SUCCESS) {
            err = "failed to start ICE candidate gathering";
            shutdown();
            return false;
        }
        return true;
    }

    void set_failure(std::string message) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) return;
            failure = std::move(message);
            failed = true;
        }
        cv.notify_all();
    }

    void mark_established() {
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!established) { established = true; notify = true; }
        }
        if (notify && stateChanged) stateChanged("connected");
        cv.notify_all();
    }

    void create_client_connection() {
        if (conn) return;
        socklen_t peerLen = 0;
        auto peer = dummy_address(false, false, peerLen);
        conn = picoquic_create_client_cnx(
            quic, reinterpret_cast<sockaddr*>(&peer), picoquic_current_time(),
            0, "folderbuddies", kAlpn, &Impl::quic_event, this);
        // The public helper creates and starts the client handshake.
        if (!conn) set_failure("failed to create QUIC client connection");
    }

    void process_datagram(std::vector<uint8_t>& packet) {
        socklen_t localLen = 0, peerLen = 0;
        auto local = dummy_address(role == Role::Server, true, localLen);
        auto peer = dummy_address(role == Role::Server, false, peerLen);
        picoquic_incoming_packet(
            quic, packet.data(), packet.size(), reinterpret_cast<sockaddr*>(&peer),
            reinterpret_cast<sockaddr*>(&local), 0, 0, picoquic_current_time());
        if (!conn) conn = picoquic_get_first_cnx(quic);
    }

    std::shared_ptr<StreamState> ensure_stream(uint64_t id, bool announce) {
        std::shared_ptr<StreamState> state;
        bool inserted = false;
        {
            std::lock_guard<std::mutex> lock(streamsMutex);
            auto result = streams.try_emplace(id, std::make_shared<StreamState>());
            state = result.first->second;
            inserted = result.second;
        }
        if (inserted && announce && incomingStream) {
            auto stream = std::make_shared<Stream>(shared_from_this(), id, state);
            incomingStream(std::move(stream));
        }
        return state;
    }

    void pump_streams() {
        if (!conn || !established) return;

        std::vector<std::pair<uint64_t, std::shared_ptr<StreamState>>> streamSnapshot;
        {
            std::lock_guard<std::mutex> lock(streamsMutex);
            streamSnapshot.assign(streams.begin(), streams.end());
        }
        for (auto& [streamId, state] : streamSnapshot) {
            std::array<uint8_t, 64 * 1024> chunk{};
            size_t count = 0;
            bool closeAfter = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                count = std::min(chunk.size(), state->tx.size());
                for (size_t i = 0; i < count; ++i) chunk[i] = state->tx[i];
                closeAfter = state->localClosed && !state->finSent && count == state->tx.size();
            }
            if (!count && !closeAfter) continue;
            const int result = picoquic_add_to_stream(
                conn, streamId, count ? chunk.data() : nullptr, count, closeAfter ? 1 : 0);
            if (result == 0) {
                std::lock_guard<std::mutex> lock(state->mutex);
                for (size_t i = 0; i < count; ++i) state->tx.pop_front();
                if (closeAfter) state->finSent = true;
            } else set_failure("picoquic rejected filesystem stream data");
        }
    }

    void flush_quic() {
        if (!quic || !ice || !iceConnected.load()) return;
        while (!pendingSends.empty()) {
            const auto& packet = pendingSends.front();
            const int result = juice_send(
                ice, reinterpret_cast<const char*>(packet.data()), packet.size());
            if (result == JUICE_ERR_AGAIN) return;
            if (result < 0) { set_failure("ICE path rejected a QUIC datagram"); return; }
            pendingSends.pop_front();
        }
        std::array<uint8_t, kMaxDatagram> out{};
        for (;;) {
            size_t length = 0;
            sockaddr_storage to{}, from{};
            int ifIndex = 0;
            picoquic_connection_id_t logCid{};
            picoquic_cnx_t* lastConn = nullptr;
            const int prepared = picoquic_prepare_next_packet(
                quic, picoquic_current_time(), out.data(), out.size(), &length,
                &to, &from, &ifIndex, &logCid, &lastConn);
            if (prepared != 0) { set_failure("QUIC packet generation failed"); break; }
            if (!length) break;
            const int sendResult = juice_send(
                ice, reinterpret_cast<const char*>(out.data()), length);
            if (sendResult == JUICE_ERR_AGAIN) {
                // picoquic has consumed this packet; retain it until libjuice's
                // bounded send queue accepts it rather than silently dropping.
                pendingSends.emplace_back(out.begin(), out.begin() + length);
                break;
            }
            if (sendResult < 0) {
                set_failure("ICE path rejected a QUIC datagram");
                break;
            }
        }
    }

    void run() {
        int64_t waitMicros = 1000000;
        for (;;) {
            std::deque<std::vector<uint8_t>> incoming;
            bool startClient = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                auto ready = [&] { return stopping || workPending || !datagrams.empty(); };
                cv.wait_for(lock, std::chrono::microseconds(waitMicros), ready);
                if (stopping) break;
                workPending = false;
                incoming.swap(datagrams);
                startClient = iceConnected.load() && role == Role::Client && !conn;
            }
            if (startClient) create_client_connection();
            for (auto& packet : incoming) process_datagram(packet);
            pump_streams();
            flush_quic();
            if (failed.load()) break;
            waitMicros = pendingSends.empty()
                ? picoquic_get_next_wake_delay(quic, picoquic_current_time(), 1000000)
                : 1000;
            if (waitMicros < 0) waitMicros = 0;
        }
        std::vector<std::shared_ptr<StreamState>> streamSnapshot;
        {
            std::lock_guard<std::mutex> lock(streamsMutex);
            for (auto& [id, state] : streams) streamSnapshot.push_back(state);
        }
        for (auto& state : streamSnapshot) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->remoteClosed = true;
            state->cv.notify_all();
        }
    }

    std::vector<std::shared_ptr<ByteStream>> make_streams(size_t count) {
        std::vector<std::shared_ptr<ByteStream>> result;
        result.reserve(count);
        std::lock_guard<std::mutex> lock(mutex);
        for (size_t i = 0; i < count; ++i) {
            const uint64_t id = static_cast<uint64_t>(i) * 4; // client-initiated bidirectional
            auto state = ensure_stream(id, false);
            result.push_back(std::make_shared<Stream>(shared_from_this(), id, state));
        }
        return result;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) return;
            stopping = true;
            if (!established && !failed.load()) {
                failure = "native QUIC endpoint stopped";
                failed = true;
            }
        }
        cv.notify_all();
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) worker.join();
        if (ice) juice_destroy(ice);
        ice = nullptr;
        conn = nullptr;
        if (quic) picoquic_free(quic);
        quic = nullptr;
    }
};

NativeQuicEndpoint::NativeQuicEndpoint(Role role) : impl_(std::make_shared<Impl>(role)) {}
NativeQuicEndpoint::~NativeQuicEndpoint() { close(); }
void NativeQuicEndpoint::setLocalDescriptionCallback(std::function<void(const std::string&)> cb) { impl_->localDescription = std::move(cb); }
void NativeQuicEndpoint::setIncomingStreamCallback(std::function<void(std::shared_ptr<ByteStream>)> cb) { impl_->incomingStream = std::move(cb); }
void NativeQuicEndpoint::setStateCallback(std::function<void(const std::string&)> cb) { impl_->stateChanged = std::move(cb); }
bool NativeQuicEndpoint::start(std::string& err) { return impl_->initialize(err); }
bool NativeQuicEndpoint::setRemoteDescription(const std::string& description, std::string& err) {
    if (!impl_->ice || juice_set_remote_description(impl_->ice, description.c_str()) != JUICE_ERR_SUCCESS) {
        err = "ICE rejected the remote candidate description";
        return false;
    }
    return true;
}
bool NativeQuicEndpoint::waitConnected(std::chrono::milliseconds timeout, std::string& err) {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    if (!impl_->cv.wait_for(lock, timeout, [&] { return impl_->established || impl_->failed; })) {
        err = "direct QUIC/ICE connection timed out";
        return false;
    }
    if (!impl_->established) { err = impl_->failure; return false; }
    return true;
}
std::vector<std::shared_ptr<ByteStream>> NativeQuicEndpoint::openStreams(size_t count, std::string& err) {
    if (!connected()) { err = "QUIC is not connected"; return {}; }
    if (impl_->role != Role::Client) { err = "only the QUIC client opens filesystem streams"; return {}; }
    return impl_->make_streams(count);
}
bool NativeQuicEndpoint::connected() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->established && !impl_->failed; }
void NativeQuicEndpoint::close() { if (impl_) impl_->shutdown(); }
bool native_quic_available() { return true; }

} // namespace fb

#else

namespace fb {
struct NativeQuicEndpoint::Impl {};
NativeQuicEndpoint::NativeQuicEndpoint(Role) : impl_(std::make_shared<Impl>()) {}
NativeQuicEndpoint::~NativeQuicEndpoint() = default;
void NativeQuicEndpoint::setLocalDescriptionCallback(std::function<void(const std::string&)>) {}
void NativeQuicEndpoint::setIncomingStreamCallback(std::function<void(std::shared_ptr<ByteStream>)>) {}
void NativeQuicEndpoint::setStateCallback(std::function<void(const std::string&)>) {}
bool NativeQuicEndpoint::start(std::string& err) { err = "native QUIC support was not built"; return false; }
bool NativeQuicEndpoint::setRemoteDescription(const std::string&, std::string& err) { err = "native QUIC support was not built"; return false; }
bool NativeQuicEndpoint::waitConnected(std::chrono::milliseconds, std::string& err) { err = "native QUIC support was not built"; return false; }
std::vector<std::shared_ptr<ByteStream>> NativeQuicEndpoint::openStreams(size_t, std::string& err) { err = "native QUIC support was not built"; return {}; }
bool NativeQuicEndpoint::connected() const { return false; }
void NativeQuicEndpoint::close() {}
bool native_quic_available() { return false; }
} // namespace fb

#endif
