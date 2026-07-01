#include "web_compat.h"

#include "base91.h"
#include "common.h"
#include "fileio.h"
#include "osflags.h"
#include "signaling.h"
#include "native_quic.h"
#include "client.h"
#include "server.h"

#include <QByteArray>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QEventLoop>
#include <QUuid>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#ifndef EROFS
#  define EROFS EACCES
#endif

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#ifdef FB_HAVE_LIBDATACHANNEL
#  include <rtc/rtc.hpp>
#endif

namespace fb {
namespace {

namespace fs = std::filesystem;
constexpr uint32_t kBinMagic = 0x4642494eu; // FBIN, big endian in browser buffers
constexpr size_t kWebChunk = 64 * 1024;

inline void compat_dbg(const std::string& s) {
    static const bool on = std::getenv("FB_COMPAT_DEBUG") != nullptr;
    if (on) { std::fprintf(stderr, "[compat] %s\n", s.c_str()); std::fflush(stderr); }
}

std::string strip_ws(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        if (!std::isspace(c)) out.push_back(static_cast<char>(c));
    return out;
}

std::string lookup_of(const std::string& code) {
    return room_lookup_id(code);
}

bool normalize_rel(const std::string& in, std::string& out) {
    std::string s = in.empty() ? "/" : in;
    if (s[0] != '/') s.insert(s.begin(), '/');
    fs::path p;
    for (const auto& part : fs::path(s).lexically_normal()) {
        std::string x = part.string();
        if (x.empty() || x == "/" || x == ".") continue;
        if (x == "..") return false;
        p /= x;
    }
    out = "/" + p.generic_string();
    if (out != "/" && out.ends_with('/')) out.pop_back();
    return true;
}

std::string parent_path(const std::string& path) {
    if (path == "/") return "/";
    auto pos = path.find_last_of('/');
    if (pos == 0 || pos == std::string::npos) return "/";
    return path.substr(0, pos);
}

std::string basename(const std::string& path) {
    if (path == "/") return "";
    auto pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string path_key(const fs::path& p) {
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

bool path_within(const fs::path& root, const fs::path& candidate) {
    std::string r = path_key(root);
    std::string c = path_key(candidate);
    if (c == r) return true;
    if (r.empty()) return false;
    if (r == "/") return !c.empty() && c.front() == '/';
    return c.size() > r.size() && c.rfind(r, 0) == 0 &&
           (c[r.size()] == '/' || c[r.size()] == '\\');
}

bool is_boundary_reparse_point(const fs::path& p) {
#ifdef _WIN32
    DWORD attrs = ::GetFileAttributesW(p.wstring().c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    std::error_code ec;
    return fs::is_symlink(fs::symlink_status(p, ec));
#endif
}

uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h ? h : 1;
}

WireAttr attr_from_fs(const fs::path& abs, const std::string& rel) {
    WireAttr a{};
    std::error_code ec;
    bool dir = fs::is_directory(abs, ec);
    bool reg = fs::is_regular_file(abs, ec);
    a.ino = fnv1a(rel);
    a.size = reg ? fs::file_size(abs, ec) : 0;
    a.blocks = (a.size + 511) / 512;
    auto now = QDateTime::currentSecsSinceEpoch();
    a.atime = a.mtime = a.ctime = now;
    a.mode = (dir ? 0040000u : 0100000u) | (dir ? 0755u : 0644u);
    a.nlink = dir ? 2 : 1;
    return a;
}

WireAttr attr_from_web_entry(const QJsonObject& e) {
    const std::string path = e.value("path").toString().toStdString();
    const bool dir = e.value("kind").toString() == "directory";
    WireAttr a{};
    a.ino = fnv1a(path);
    a.size = dir ? 0 : static_cast<uint64_t>(std::max<qint64>(0, static_cast<qint64>(e.value("size").toDouble(0))));
    a.blocks = (a.size + 511) / 512;
    qint64 ms = static_cast<qint64>(e.value("mtime").toDouble(0));
    qint64 sec = ms > 100000000000ll ? ms / 1000 : (ms ? ms : QDateTime::currentSecsSinceEpoch());
    a.atime = a.mtime = a.ctime = sec;
    a.mode = (dir ? 0040000u : 0100000u) | (dir ? 0755u : 0644u);
    a.nlink = dir ? 2 : 1;
    return a;
}

QByteArray json_compact(const QJsonObject& o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QJsonObject parse_json_obj(const std::string& s) {
    QJsonParseError pe{};
    QJsonDocument d = QJsonDocument::fromJson(QByteArray::fromStdString(s), &pe);
    return pe.error == QJsonParseError::NoError && d.isObject() ? d.object() : QJsonObject{};
}

std::string b64url_json(const QJsonObject& o) {
    QByteArray b = json_compact(o).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    return "plain:" + b.toStdString();
}

QJsonObject unb64url_json(const std::string& s) {
    if (s.rfind("plain:", 0) != 0) return {};
    QByteArray raw = QByteArray::fromBase64(QByteArray::fromStdString(s.substr(6)), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    QJsonParseError pe{};
    QJsonDocument d = QJsonDocument::fromJson(raw, &pe);
    return pe.error == QJsonParseError::NoError && d.isObject() ? d.object() : QJsonObject{};
}

bool extract_web_room(std::string text, std::string& code) {
    text = strip_ws(text);
    if (looks_like_room_code(text)) { code = text; return true; }
    if (text.rfind("FBS2:", 0) == 0) {
        bool ok = false;
        std::vector<uint8_t> raw = base91_decode(text.substr(5), &ok);
        if (!ok) return false;
        QJsonParseError pe{};
        QJsonDocument d = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(raw.data()), static_cast<int>(raw.size())), &pe);
        if (pe.error != QJsonParseError::NoError || !d.isObject()) return false;
        std::string c = d.object().value("code").toString().toStdString();
        if (!looks_like_room_code(c)) return false;
        code = c;
        return true;
    }
    return false;
}

std::string ws_url_for(const std::string& lookup, const char* role) {
    QUrl url(QString::fromStdString(SignalingClient::base_url()));
    if (url.scheme() == "https") url.setScheme("wss");
    else if (url.scheme() == "http") url.setScheme("ws");

    QString path = url.path();
    while (path.endsWith('/')) path.chop(1);
    url.setPath(path + "/room");

    QString query = QString::fromStdString("code=" + encode_url_query_value(lookup)) +
                    "&role=" + QString::fromLatin1(role) + "&web=1&compat=native";
    url.setQuery(query, QUrl::StrictMode);
    return url.toString(QUrl::FullyEncoded).toStdString();
}

#ifdef FB_HAVE_LIBDATACHANNEL

QByteArray compact_json(const QJsonObject& o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

bool sync_http_compat(QNetworkRequest req, const QByteArray& method, const QByteArray& body,
                      int& status, QByteArray& response, std::string& err) {
    QNetworkAccessManager nam;
    QNetworkReply* reply = nam.sendCustomRequest(req, method, body);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start(8000);
    loop.exec();

    if (!reply->isFinished()) {
        reply->abort();
        reply->deleteLater();
        err = "Firebase compatibility signaling timed out";
        return false;
    }
    status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    response = reply->readAll();
    if (reply->error() != QNetworkReply::NoError && status == 0) {
        err = reply->errorString().toStdString();
        reply->deleteLater();
        return false;
    }
    reply->deleteLater();
    return true;
}

std::string firebase_safe_key_web(const std::string& lookup) {
    return QByteArray::fromStdString(lookup)
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)
        .toStdString();
}

std::string firebase_web_path(const std::string& lookup) {
    return "/webRooms/" + firebase_safe_key_web(lookup);
}

constexpr qint64 kFirebaseWebRoomMaxAgeMs = 30LL * 24 * 60 * 60 * 1000;

qint64 firebase_web_created_at_ms(const QJsonObject& room) {
    QJsonValue v;

    const QJsonObject host = room.value("host").toObject();
    if (host.contains("createdAt"))
        v = host.value("createdAt");
    else
        v = room.value("createdAt");

    if (!v.isDouble()) return 0;

    const double raw = v.toDouble();
    if (raw <= 0.0) return 0;

    if (raw < 20000000000.0)
        return static_cast<qint64>(raw * 1000.0);

    return static_cast<qint64>(raw);
}

bool firebase_web_room_expired(const QJsonObject& room) {
    const qint64 createdAtMs = firebase_web_created_at_ms(room);
    return createdAtMs <= 0 ||
           createdAtMs < QDateTime::currentMSecsSinceEpoch() - kFirebaseWebRoomMaxAgeMs;
}

QNetworkRequest firebase_web_request(const std::string& pathNoJson) {
    QNetworkRequest req(QUrl(QString::fromStdString(FirebaseSignalingClient::base_url() + pathNoJson + ".json")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("User-Agent", "FolderBuddies/1");
    return req;
}

bool firebase_get_value(const std::string& pathNoJson, QJsonValue& out, std::string& err) {
    int status = 0;
    QByteArray resp;
    if (!sync_http_compat(firebase_web_request(pathNoJson), "GET", {}, status, resp, err)) return false;
    if (status != 200) {
        err = "Firebase compatibility GET failed (HTTP " + std::to_string(status) + "): " + resp.toStdString();
        return false;
    }
    if (QString::fromUtf8(resp).trimmed() == "null") { out = QJsonValue(); return true; }
    QJsonParseError pe{};
    QJsonDocument d = QJsonDocument::fromJson(resp, &pe);
    if (pe.error != QJsonParseError::NoError) { err = "Firebase compatibility returned invalid JSON"; return false; }
    if (d.isObject()) out = d.object();
    else if (d.isArray()) out = d.array();
    else if (d.isNull()) out = QJsonValue();
    else out = QJsonValue::fromVariant(d.toVariant());
    return true;
}

bool firebase_put_obj(const std::string& pathNoJson, const QJsonObject& obj, std::string& err) {
    int status = 0;
    QByteArray resp;
    if (!sync_http_compat(firebase_web_request(pathNoJson), "PUT", compact_json(obj), status, resp, err)) return false;
    if (status == 200) return true;
    err = "Firebase compatibility PUT failed (HTTP " + std::to_string(status) + "): " + resp.toStdString();
    return false;
}

bool firebase_post_obj(const std::string& pathNoJson, const QJsonObject& obj, std::string& err) {
    int status = 0;
    QByteArray resp;
    if (!sync_http_compat(firebase_web_request(pathNoJson), "POST", compact_json(obj), status, resp, err)) return false;
    if (status == 200) return true;
    err = "Firebase compatibility POST failed (HTTP " + std::to_string(status) + "): " + resp.toStdString();
    return false;
}

bool firebase_delete_path(const std::string& pathNoJson, std::string& err) {
    int status = 0;
    QByteArray resp;
    if (!sync_http_compat(firebase_web_request(pathNoJson), "DELETE", {}, status, resp, err)) return false;
    if (status == 200 || status == 204) return true;
    err = "Firebase compatibility DELETE failed (HTTP " + std::to_string(status) + "): " + resp.toStdString();
    return false;
}

std::string compat_peer_id() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}

class FirebaseCompatRelay {
public:
    enum class Role { Host, Client };
    using Callback = std::function<void(const QJsonObject&)>;

    ~FirebaseCompatRelay() { close(); }

    bool startHost(const std::string& lookup, Callback cb, std::string& err) {
        if (!FirebaseSignalingClient::configured()) { err = "Firebase fallback URL is not configured"; return false; }
        role_ = Role::Host;
        lookup_ = lookup;
        path_ = firebase_web_path(lookup);
        onMessage_ = std::move(cb);

        QJsonValue existing;
        if (!firebase_get_value(path_, existing, err)) return false;

        if (!existing.isUndefined() && !existing.isNull()) {
            if (existing.isObject() && firebase_web_room_expired(existing.toObject())) {
                std::string ignored;
                firebase_delete_path(path_, ignored);
            } else {
                err = "Firebase web compatibility room already exists";
                return false;
            }
        }

        QJsonObject host; host["createdAt"] = static_cast<double>(QDateTime::currentMSecsSinceEpoch());
        QJsonObject room; room["v"] = 1; room["host"] = host;
        if (!firebase_put_obj(path_, room, err)) return false;

        live_ = true;
        poller_ = std::thread([this] { pollHost(); });
        QJsonObject ready; ready["kind"] = "ready"; ready["role"] = "host"; ready["room"] = QString::fromStdString(lookup_);
        emitMessage(ready);
        return true;
    }

    bool startClient(const std::string& lookup, Callback cb, std::string& err) {
        if (!FirebaseSignalingClient::configured()) { err = "Firebase fallback URL is not configured"; return false; }
        role_ = Role::Client;
        lookup_ = lookup;
        path_ = firebase_web_path(lookup);
        peerId_ = compat_peer_id();
        onMessage_ = std::move(cb);

        QJsonValue room;
        if (!firebase_get_value(path_, room, err)) return false;
        if (room.isUndefined() || room.isNull()) { err = "no Firebase web compatibility room found for that code"; return false; }

        if (!room.isObject() || firebase_web_room_expired(room.toObject())) {
            std::string ignored;
            firebase_delete_path(path_, ignored);
            err = "Firebase web compatibility room expired";
            return false;
        }

        QJsonObject client; client["joinedAt"] = static_cast<double>(QDateTime::currentMSecsSinceEpoch());
        if (!firebase_put_obj(path_ + "/clients/" + peerId_, client, err)) return false;

        live_ = true;
        poller_ = std::thread([this] { pollClient(); });
        QJsonObject ready; ready["kind"] = "ready"; ready["role"] = "client"; ready["room"] = QString::fromStdString(lookup_); ready["peerId"] = QString::fromStdString(peerId_);
        emitMessage(ready);
        return true;
    }

    void sendSignal(const std::string& peerId, const QJsonObject& payload) {
        if (!live_) return;
        QJsonObject msg;
        msg["ciphertext"] = QString::fromStdString(b64url_json(payload));
        msg["at"] = static_cast<double>(QDateTime::currentMSecsSinceEpoch());
        std::string err;
        if (role_ == Role::Host) {
            firebase_post_obj(path_ + "/signalsToClient/" + peerId, msg, err);
        } else {
            msg["peerId"] = QString::fromStdString(peerId_);
            firebase_post_obj(path_ + "/signalsToHost", msg, err);
        }
    }

    void close() {
        bool wasLive = live_.exchange(false);
        if (poller_.joinable()) poller_.join();
        if (!wasLive || path_.empty()) return;
        std::string err;
        if (role_ == Role::Host) firebase_delete_path(path_, err);
        else if (!peerId_.empty()) firebase_delete_path(path_ + "/clients/" + peerId_, err);
    }

private:
    void emitMessage(const QJsonObject& obj) {
        if (onMessage_) {
            try { onMessage_(obj); } catch (...) { /* callback owns errors */ }
        }
    }

    void pollHost() {
        while (live_) {
            std::string err;
            QJsonValue clients;
            if (firebase_get_value(path_ + "/clients", clients, err) && clients.isObject()) {
                const QJsonObject clientObj = clients.toObject();
                std::set<std::string> now;
                for (auto it = clientObj.begin(); it != clientObj.end(); ++it) {
                    std::string peer = it.key().toStdString();
                    now.insert(peer);
                    if (!seenClients_.count(peer)) {
                        seenClients_.insert(peer);
                        QJsonObject joined; joined["kind"] = "client-joined"; joined["peerId"] = QString::fromStdString(peer);
                        emitMessage(joined);
                    }
                }
                for (auto it = seenClients_.begin(); it != seenClients_.end(); ) {
                    if (!now.count(*it)) {
                        QJsonObject left; left["kind"] = "client-left"; left["peerId"] = QString::fromStdString(*it);
                        emitMessage(left);
                        it = seenClients_.erase(it);
                    } else ++it;
                }
            }

            QJsonValue sigs;
            if (firebase_get_value(path_ + "/signalsToHost", sigs, err) && sigs.isObject()) {
                const QJsonObject obj = sigs.toObject();
                for (auto it = obj.begin(); it != obj.end(); ++it) {
                    if (seenSignals_.count(it.key().toStdString())) continue;
                    seenSignals_.insert(it.key().toStdString());
                    QJsonObject v = it.value().toObject();
                    if (v.value("peerId").isString() && v.value("ciphertext").isString()) {
                        QJsonObject msg; msg["kind"] = "signal"; msg["peerId"] = v.value("peerId").toString(); msg["ciphertext"] = v.value("ciphertext").toString();
                        emitMessage(msg);
                    }
                    std::string delErr;
                    firebase_delete_path(path_ + "/signalsToHost/" + it.key().toStdString(), delErr);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    void pollClient() {
        while (live_) {
            std::string err;
            QJsonValue room;
            if (firebase_get_value(path_, room, err) && (room.isUndefined() || room.isNull())) {
                QJsonObject msg; msg["kind"] = "host-left"; emitMessage(msg); live_ = false; break;
            }
            QJsonValue sigs;
            if (firebase_get_value(path_ + "/signalsToClient/" + peerId_, sigs, err) && sigs.isObject()) {
                const QJsonObject obj = sigs.toObject();
                for (auto it = obj.begin(); it != obj.end(); ++it) {
                    if (seenSignals_.count(it.key().toStdString())) continue;
                    seenSignals_.insert(it.key().toStdString());
                    QJsonObject v = it.value().toObject();
                    if (v.value("ciphertext").isString()) {
                        QJsonObject msg; msg["kind"] = "signal"; msg["peerId"] = QString::fromStdString(peerId_); msg["ciphertext"] = v.value("ciphertext").toString();
                        emitMessage(msg);
                    }
                    std::string delErr;
                    firebase_delete_path(path_ + "/signalsToClient/" + peerId_ + "/" + it.key().toStdString(), delErr);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    Role role_ = Role::Client;
    std::string lookup_;
    std::string path_;
    std::string peerId_;
    Callback onMessage_;
    std::atomic<bool> live_{false};
    std::thread poller_;
    std::set<std::string> seenClients_;
    std::set<std::string> seenSignals_;
};

void send_plain_signal(const std::shared_ptr<rtc::WebSocket>& ws, const std::string& peerId, const QJsonObject& payload) {
    if (!ws) return;
    QJsonObject msg;
    msg["kind"] = "signal";
    msg["peerId"] = QString::fromStdString(peerId);
    msg["ciphertext"] = QString::fromStdString(b64url_json(payload));
    ws->send(json_compact(msg).toStdString());
}

void send_bin(const std::shared_ptr<rtc::DataChannel>& dc, uint32_t id, const uint8_t* data, size_t n) {
    rtc::binary out;
    out.resize(8 + n);
    out[0] = static_cast<std::byte>((kBinMagic >> 24) & 0xff);
    out[1] = static_cast<std::byte>((kBinMagic >> 16) & 0xff);
    out[2] = static_cast<std::byte>((kBinMagic >> 8) & 0xff);
    out[3] = static_cast<std::byte>(kBinMagic & 0xff);
    out[4] = static_cast<std::byte>((id >> 24) & 0xff);
    out[5] = static_cast<std::byte>((id >> 16) & 0xff);
    out[6] = static_cast<std::byte>((id >> 8) & 0xff);
    out[7] = static_cast<std::byte>(id & 0xff);
    for (size_t i = 0; i < n; ++i) out[8 + i] = static_cast<std::byte>(data[i]);
    dc->send(out);
}

#endif

} // namespace

bool looks_like_web_compat_code(const std::string& text) {
    std::string c;
    return extract_web_room(text, c);
}

#ifdef FB_HAVE_LIBDATACHANNEL

struct WebRtcCompatHost::Impl {
    std::string root;
    bool allowWrites = false;
    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<FirebaseCompatRelay> fbRelay;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> wsClosed{false};
    std::string wsError;
    std::unordered_map<std::string, std::shared_ptr<rtc::PeerConnection>> pcs;
    std::unordered_map<std::string, std::shared_ptr<rtc::DataChannel>> channels;
    std::unordered_map<std::string, std::shared_ptr<NativeQuicEndpoint>> quicPeers;
    std::vector<std::thread> negotiationTimers;
    std::set<std::string> departedPeers;
    Server* nativeServer = nullptr;
    std::unordered_map<uint32_t, std::ofstream> uploads;
    std::atomic<bool> live{false};
    std::atomic<uint64_t>* bytesOut = nullptr;
    std::atomic<uint64_t>* bytesIn = nullptr;
    std::function<void()> notifyClientsChanged;

    void notifyClients() {
        if (notifyClientsChanged) {
            try { notifyClientsChanged(); } catch (...) { /* UI owns callback errors */ }
        }
    }

    void addBytesOut(uint64_t n) {
        if (bytesOut && n) bytesOut->fetch_add(n, std::memory_order_relaxed);
    }

    void addBytesIn(uint64_t n) {
        if (bytesIn && n) bytesIn->fetch_add(n, std::memory_order_relaxed);
    }

    bool resolve_common(const std::string& rel, fs::path& out) {
        std::string norm;
        if (!normalize_rel(rel, norm)) return false;
        std::error_code ec;
        fs::path rootPath = fs::weakly_canonical(root, ec);
        if (ec) return false;

        fs::path current = rootPath;
        fs::path tail;
        bool missing = false;
        for (const auto& partPath : fs::path(norm.substr(1))) {
            fs::path part = partPath.filename();
            if (part.empty()) continue;
            if (missing) {
                tail /= part;
                continue;
            }

            fs::path next = current / part;
            if (fs::exists(next, ec)) {
                if (is_boundary_reparse_point(next)) return false;
                current = fs::weakly_canonical(next, ec);
                if (ec || !path_within(rootPath, current)) return false;
            } else if (ec) {
                return false;
            } else {
                missing = true;
                tail /= part;
            }
        }

        fs::path candidate = (current / tail).lexically_normal();
        if (!path_within(rootPath, candidate)) return false;
        out = candidate;
        return true;
    }

    bool resolve(const std::string& rel, fs::path& out) { return resolve_common(rel, out); }

    bool resolve_write(const std::string& rel, fs::path& out) {
        return resolve_common(rel, out);
    }

    void send_error(const std::shared_ptr<rtc::DataChannel>& dc, int id, const std::string& e) {
        QJsonObject o; o["t"] = "error"; o["id"] = id; o["message"] = QString::fromStdString(e);
        dc->send(json_compact(o).toStdString());
    }
    void send_ok(const std::shared_ptr<rtc::DataChannel>& dc, int id) {
        QJsonObject o; o["t"] = "ok"; o["id"] = id; dc->send(json_compact(o).toStdString());
    }

    void on_json(const std::string& peerId, const std::shared_ptr<rtc::DataChannel>& dc, const QJsonObject& msg) {
        const QString t = msg.value("t").toString();
        const int id = msg.value("id").toInt();
        try {
            if (t == "list") {
                fs::path abs;
                std::string path = msg.value("path").toString("/").toStdString();
                if (!resolve(path, abs) || !fs::is_directory(abs)) throw std::runtime_error("Not a directory");
                QJsonArray entries;
                for (auto& de : fs::directory_iterator(abs)) {
                    if (is_boundary_reparse_point(de.path())) continue;
                    const std::string name = de.path().filename().string();
                    QJsonObject e;
                    e["name"] = QString::fromStdString(name);
                    std::string child = path == "/" ? "/" + name : path + "/" + name;
                    e["path"] = QString::fromStdString(child);
                    e["kind"] = de.is_directory() ? "directory" : "file";
                    if (de.is_regular_file()) e["size"] = static_cast<double>(de.file_size());
                    e["mtime"] = static_cast<double>(QDateTime::currentMSecsSinceEpoch());
                    entries.append(e);
                }
                QJsonObject out; out["t"] = "listResult"; out["id"] = id; out["path"] = QString::fromStdString(path); out["entries"] = entries; out["write"] = allowWrites; out["ranges"] = true;
                dc->send(json_compact(out).toStdString());
                return;
            }
            if (t == "download") {
                fs::path abs;
                if (!resolve(msg.value("path").toString().toStdString(), abs) || !fs::is_regular_file(abs)) throw std::runtime_error("Not a file");
                const uint64_t size = fs::file_size(abs);
                // Optional byte range so remote mounts can read huge files
                // block by block instead of transferring the whole file.
                uint64_t offset = static_cast<uint64_t>(std::max<double>(0.0, msg.value("offset").toDouble(0)));
                if (offset > size) offset = size;
                uint64_t remaining = size - offset;
                if (msg.contains("length")) {
                    const uint64_t want =
                        static_cast<uint64_t>(std::max<double>(0.0, msg.value("length").toDouble(0)));
                    remaining = std::min(remaining, want);
                }
                QJsonObject start; start["t"] = "fileStart"; start["id"] = id; start["name"] = QString::fromStdString(abs.filename().string()); start["size"] = static_cast<double>(remaining); start["offset"] = static_cast<double>(offset);
                dc->send(json_compact(start).toStdString());
                std::ifstream f(abs, std::ios::binary);
                f.seekg(static_cast<std::streamoff>(offset));
                std::vector<uint8_t> buf(kWebChunk);
                while (f && remaining > 0) {
                    const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, buf.size()));
                    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(want));
                    std::streamsize got = f.gcount();
                    if (got <= 0) break;
                    const auto n = static_cast<size_t>(got);
                    send_bin(dc, static_cast<uint32_t>(id), buf.data(), n);
                    addBytesOut(static_cast<uint64_t>(n));
                    remaining -= static_cast<uint64_t>(n);
                }
                QJsonObject end; end["t"] = "fileEnd"; end["id"] = id; dc->send(json_compact(end).toStdString());
                return;
            }
            if (t == "uploadStart") {
                if (!allowWrites) throw std::runtime_error("The host has writes disabled");
                fs::path abs;
                if (!resolve_write(msg.value("path").toString().toStdString(), abs)) throw std::runtime_error("Bad upload path");
                fs::create_directories(abs.parent_path());
                std::lock_guard<std::mutex> lk(mtx);
                uploads[static_cast<uint32_t>(id)] = std::ofstream(abs, std::ios::binary | std::ios::trunc);
                QJsonObject out; out["t"] = "uploadReady"; out["id"] = id; dc->send(json_compact(out).toStdString());
                return;
            }
            if (t == "mkdir") {
                if (!allowWrites) throw std::runtime_error("The host has writes disabled");
                fs::path abs;
                if (!resolve_write(msg.value("path").toString().toStdString(), abs)) throw std::runtime_error("Bad path");
                std::error_code ec;
                fs::create_directories(abs, ec);
                if (ec) throw std::runtime_error("Create folder failed");
                send_ok(dc, id);
                return;
            }
            if (t == "uploadEnd") {
                std::lock_guard<std::mutex> lk(mtx);
                uploads.erase(static_cast<uint32_t>(id));
                send_ok(dc, id);
                return;
            }
            if (t == "delete") {
                if (!allowWrites) throw std::runtime_error("The host has writes disabled");
                fs::path abs;
                if (!resolve(msg.value("path").toString().toStdString(), abs)) throw std::runtime_error("Bad path");
                std::error_code ec; fs::remove_all(abs, ec);
                if (ec) throw std::runtime_error("Delete failed");
                send_ok(dc, id);
                return;
            }
            throw std::runtime_error("Unknown request");
        } catch (const std::exception& e) { send_error(dc, id, e.what()); }
        (void)peerId;
    }

    void on_binary(const rtc::binary& b) {
        if (b.size() < 8) return;
        auto byte = [&](size_t i) { return static_cast<uint8_t>(b[i]); };
        uint32_t magic = (uint32_t(byte(0)) << 24) | (uint32_t(byte(1)) << 16) | (uint32_t(byte(2)) << 8) | byte(3);
        if (magic != kBinMagic) return;
        uint32_t id = (uint32_t(byte(4)) << 24) | (uint32_t(byte(5)) << 16) | (uint32_t(byte(6)) << 8) | byte(7);
        std::lock_guard<std::mutex> lk(mtx);
        auto it = uploads.find(id);
        if (it == uploads.end()) return;
        const uint64_t payloadBytes = static_cast<uint64_t>(b.size() - 8);
        addBytesIn(payloadBytes);
        for (size_t i = 8; i < b.size(); ++i) {
            char c = static_cast<char>(static_cast<uint8_t>(b[i]));
            it->second.write(&c, 1);
        }
    }

    void send_signal(const std::string& peerId, const QJsonObject& payload) {
        if (fbRelay) fbRelay->sendSignal(peerId, payload);
        else send_plain_signal(ws, peerId, payload);
    }

    void create_peer(const std::string& peerId) {
        if (!live.load()) return;
        compat_dbg("host create_peer peer=" + peerId);
        rtc::Configuration cfg;
        cfg.iceServers.emplace_back("stun:stun.l.google.com:19302");
        auto pc = std::make_shared<rtc::PeerConnection>(cfg);

        pc->onLocalDescription([this, peerId](rtc::Description desc) {
            if (!live.load()) return;
            compat_dbg("host sending offer to peer=" + peerId);
            QJsonObject sdp; sdp["type"] = "offer"; sdp["sdp"] = QString::fromStdString(std::string(desc));
            QJsonObject payload; payload["type"] = "offer"; payload["sdp"] = sdp;
            send_signal(peerId, payload);
        });
        pc->onLocalCandidate([this, peerId](rtc::Candidate cand) {
            if (!live.load()) return;
            QJsonObject c; c["candidate"] = QString::fromStdString(cand.candidate()); c["sdpMid"] = QString::fromStdString(cand.mid());
            QJsonObject payload; payload["type"] = "candidate"; payload["candidate"] = c;
            send_signal(peerId, payload);
        });
        pc->onStateChange([peerId](rtc::PeerConnection::State s) {
            compat_dbg("host pc state peer=" + peerId + " state=" + std::to_string(static_cast<int>(s)));
        });
        pc->onGatheringStateChange([peerId](rtc::PeerConnection::GatheringState g) {
            compat_dbg("host gathering peer=" + peerId + " state=" + std::to_string(static_cast<int>(g)));
        });

        auto dc = pc->createDataChannel("folderbuddies-files");
        dc->onMessage([this, peerId, dc](std::variant<rtc::binary, rtc::string> message) {
            if (std::holds_alternative<rtc::string>(message)) on_json(peerId, dc, parse_json_obj(std::get<rtc::string>(message)));
            else on_binary(std::get<rtc::binary>(message));
        });
        dc->onOpen([this, peerId] {
            compat_dbg("host datachannel OPEN peer=" + peerId);
            notifyClients();
        });
        dc->onClosed([this, peerId] {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lk(mtx);
                changed = channels.erase(peerId) > 0;
                pcs.erase(peerId);
            }
            if (changed) notifyClients();
        });

        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (!live.load()) return;
            changed = !channels.count(peerId);
            pcs[peerId] = pc;
            channels[peerId] = dc;
        }
        if (changed) notifyClients();
    }

    void create_quic_peer(const std::string& peerId) {
        if (!live.load() || !nativeServer || !native_quic_available()) return;
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (quicPeers.count(peerId)) return;
        }
        auto endpoint = std::make_shared<NativeQuicEndpoint>(NativeQuicEndpoint::Role::Server);
        endpoint->setLocalDescriptionCallback([this, peerId](const std::string& description) {
            QJsonObject payload;
            payload["type"] = "native-quic-description";
            payload["description"] = QString::fromStdString(description);
            send_signal(peerId, payload);
        });
        endpoint->setIncomingStreamCallback([this](std::shared_ptr<ByteStream> stream) {
            if (nativeServer) nativeServer->acceptStream(std::move(stream));
        });
        endpoint->setStateCallback([peerId](const std::string& state) {
            compat_dbg("native QUIC peer=" + peerId + " state=" + state);
        });
        std::string err;
        if (!endpoint->start(err)) {
            compat_dbg("native QUIC start failed peer=" + peerId + ": " + err);
            QJsonObject payload; payload["type"] = "native-quic-error";
            payload["message"] = QString::fromStdString(err);
            send_signal(peerId, payload);
            return;
        }
        bool keep = false;
        {
            std::lock_guard<std::mutex> lock(mtx);
            keep = live.load();
            if (keep) quicPeers[peerId] = endpoint;
        }
        if (!keep) endpoint->close();
    }

    void on_signal(const QJsonObject& msg) {
        const QString kind = msg.value("kind").toString();
        compat_dbg("host on_signal kind=" + kind.toStdString() +
                   " peer=" + msg.value("peerId").toString().toStdString());
        if (kind == "ready") { live = true; cv.notify_all(); return; }
        if (kind == "error") { std::lock_guard<std::mutex> lk(mtx); wsError = msg.value("error").toString("signaling error").toStdString(); wsClosed = true; cv.notify_all(); return; }
        if (kind == "client-joined") {
            const std::string peer = msg.value("peerId").toString().toStdString();
            {
                std::lock_guard<std::mutex> lock(mtx);
                if (!live.load()) return;
                departedPeers.erase(peer);
                negotiationTimers.emplace_back([this, peer] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    bool useWebRtc = false;
                    {
                        std::lock_guard<std::mutex> inner(mtx);
                        useWebRtc = live.load() && !departedPeers.count(peer) &&
                                    !quicPeers.count(peer) && !pcs.count(peer);
                    }
                    // Browser clients do not send compat-hello; native QUIC
                    // clients do, so only the silent peer gets a WebRTC offer.
                    if (useWebRtc) create_peer(peer);
                });
            }
            return;
        }
        if (kind == "client-left") {
            bool changed = false;
            std::shared_ptr<NativeQuicEndpoint> endpoint;
            {
                std::lock_guard<std::mutex> lk(mtx);
                const std::string peer = msg.value("peerId").toString().toStdString();
                departedPeers.insert(peer);
                pcs.erase(peer);
                if (auto q = quicPeers.find(peer); q != quicPeers.end()) {
                    endpoint = std::move(q->second);
                    quicPeers.erase(q);
                }
                changed = channels.erase(peer) > 0;
            }
            if (endpoint) endpoint->close();
            if (changed) notifyClients();
            return;
        }
        if (kind != "signal") return;
        std::string peerId = msg.value("peerId").toString().toStdString();
        QJsonObject sig = unb64url_json(msg.value("ciphertext").toString().toStdString());
        if (sig.isEmpty()) return;
        const QString signalType = sig.value("type").toString();
        if (signalType == "native-quic-hello") { create_quic_peer(peerId); return; }
        if (signalType == "compat-hello") {
            bool exists = false;
            { std::lock_guard<std::mutex> lock(mtx); exists = pcs.count(peerId) != 0; }
            if (!exists) create_peer(peerId);
            return;
        }
        if (signalType == "native-quic-description") {
            std::shared_ptr<NativeQuicEndpoint> endpoint;
            {
                std::lock_guard<std::mutex> lock(mtx);
                auto it = quicPeers.find(peerId);
                if (it != quicPeers.end()) endpoint = it->second;
            }
            if (!endpoint) {
                create_quic_peer(peerId);
                std::lock_guard<std::mutex> lock(mtx);
                auto it = quicPeers.find(peerId);
                if (it != quicPeers.end()) endpoint = it->second;
            }
            if (endpoint) {
                std::string err;
                if (!endpoint->setRemoteDescription(
                        sig.value("description").toString().toStdString(), err)) {
                    QJsonObject payload; payload["type"] = "native-quic-error";
                    payload["message"] = QString::fromStdString(err);
                    send_signal(peerId, payload);
                }
            }
            return;
        }
        std::lock_guard<std::mutex> lk(mtx);
        auto it = pcs.find(peerId);
        if (it == pcs.end()) return;
        if (sig.value("type").toString() == "answer") {
            QJsonObject sdp = sig.value("sdp").toObject();
            it->second->setRemoteDescription(rtc::Description(sdp.value("sdp").toString().toStdString()));
        } else if (sig.value("type").toString() == "candidate") {
            QJsonObject c = sig.value("candidate").toObject();
            it->second->addRemoteCandidate(rtc::Candidate(c.value("candidate").toString().toStdString(), c.value("sdpMid").toString().toStdString()));
        }
    }
};

struct WebRtcRemoteClient::Impl {
    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<FirebaseCompatRelay> fbRelay;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc;
    std::string peerId;
    std::mutex sendMtx;
    std::mutex mtx;
    std::mutex fsMtx;
    std::condition_variable cv;
    std::atomic<bool> open{false};
    std::atomic<bool> dead{false};
    std::unordered_map<uint32_t, QJsonObject> replies;
    std::unordered_map<uint32_t, std::vector<uint8_t>> downloads;
    struct Fh {
        std::string path;
        std::vector<uint8_t> data;
        bool loaded = false;
        bool dirty = false;
    };
    std::unordered_map<uint64_t, Fh> fhs;
    uint64_t nextFh = 1;
    uint32_t nextId = 100;
    std::atomic<bool> canWrite{false};
    std::atomic<bool> canRange{false}; // host supports ranged "download" requests

    uint32_t send_json_wait(QJsonObject obj, QJsonObject& out, int timeoutMs = 30000) {
        uint32_t id = 0;
        {
            std::lock_guard<std::mutex> slk(sendMtx);
            id = static_cast<uint32_t>(obj.value("id").toInt(0));
            if (!id) id = nextId++;
            obj["id"] = static_cast<int>(id);
            if (!dc) return 0;
            try { dc->send(json_compact(obj).toStdString()); }
            catch (...) { return 0; }
        }
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&]{ return replies.count(id) || dead.load(); });
        auto it = replies.find(id);
        if (it == replies.end()) return 0;
        out = it->second;
        replies.erase(it);
        return id;
    }
    bool wait_file(uint32_t id, std::vector<uint8_t>& out, int timeoutMs = 120000) {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&]{ return replies.count(id) || dead.load(); });
        auto replyIt = replies.find(id);
        if (replyIt == replies.end()) return false;
        bool ok = replyIt->second.value("t").toString() == "fileEnd";
        replies.erase(replyIt);
        if (!ok) {
            downloads.erase(id);
            return false;
        }
        out = std::move(downloads[id]);
        downloads.erase(id);
        return true;
    }
    void on_json(const QJsonObject& o) {
        std::lock_guard<std::mutex> lk(mtx);
        QString t = o.value("t").toString();
        if (t == "listResult") {
            canWrite = o.value("write").toBool(canWrite.load());
            if (o.contains("ranges")) canRange = o.value("ranges").toBool(false);
        }
        if (t == "fileStart") return;
        replies[static_cast<uint32_t>(o.value("id").toInt())] = o;
        cv.notify_all();
    }
    void on_binary(const rtc::binary& b) {
        if (b.size() < 8) return;
        auto byte = [&](size_t i) { return static_cast<uint8_t>(b[i]); };
        uint32_t magic = (uint32_t(byte(0)) << 24) | (uint32_t(byte(1)) << 16) | (uint32_t(byte(2)) << 8) | byte(3);
        if (magic != kBinMagic) return;
        uint32_t id = (uint32_t(byte(4)) << 24) | (uint32_t(byte(5)) << 16) | (uint32_t(byte(6)) << 8) | byte(7);
        std::lock_guard<std::mutex> lk(mtx);
        auto& v = downloads[id];
        for (size_t i = 8; i < b.size(); ++i) v.push_back(static_cast<uint8_t>(b[i]));
    }
    void send_signal(const QJsonObject& payload) {
        if (fbRelay) fbRelay->sendSignal(peerId, payload);
        else send_plain_signal(ws, peerId, payload);
    }

    int json_error(const QJsonObject& o) {
        if (o.value("t").toString() == "error") return EIO;
        return 0;
    }
    int fetch_file(const std::string& path, std::vector<uint8_t>& data) {
        QJsonObject req; req["t"] = "download"; req["path"] = QString::fromStdString(path);
        return send_download(req, data);
    }
    // Fetch only [offset, offset+length) from a range-capable host, so huge
    // files never have to be buffered whole on either side.
    int fetch_range(const std::string& path, uint64_t offset, uint32_t length,
                    std::vector<uint8_t>& data) {
        QJsonObject req; req["t"] = "download"; req["path"] = QString::fromStdString(path);
        req["offset"] = static_cast<double>(offset);
        req["length"] = static_cast<double>(length);
        return send_download(req, data);
    }
    int send_download(QJsonObject req, std::vector<uint8_t>& data) {
        uint32_t id = 0;
        {
            std::lock_guard<std::mutex> slk(sendMtx);
            id = nextId++;
            req["id"] = static_cast<int>(id);
            if (!dc) return EIO;
            try { dc->send(json_compact(req).toStdString()); }
            catch (...) { return EIO; }
        }
        return wait_file(id, data) ? 0 : EIO;
    }
    int upload_file(const std::string& path, const std::vector<uint8_t>& data) {
        uint32_t id = 0;
        {
            std::lock_guard<std::mutex> slk(sendMtx);
            id = nextId++;
        }
        QJsonObject start; start["t"] = "uploadStart"; start["id"] = static_cast<int>(id); start["path"] = QString::fromStdString(path); start["size"] = static_cast<double>(data.size());
        QJsonObject reply;
        if (!send_json_wait(start, reply) || json_error(reply)) return EIO;
        size_t off = 0;
        while (off < data.size()) {
            size_t n = std::min(kWebChunk, data.size() - off);
            {
                std::lock_guard<std::mutex> slk(sendMtx);
                if (!dc) return EIO;
                try { send_bin(dc, id, data.data() + off, n); }
                catch (...) { return EIO; }
            }
            off += n;
        }
        QJsonObject end; end["t"] = "uploadEnd"; end["id"] = static_cast<int>(id);
        if (!send_json_wait(end, reply, 60000) || json_error(reply)) return EIO;
        return 0;
    }
    bool refresh_capabilities(std::string& err) {
        QJsonObject req; req["t"] = "list"; req["path"] = "/";
        QJsonObject reply;
        if (!send_json_wait(req, reply, 30000) || json_error(reply) ||
            reply.value("t").toString() != "listResult") {
            err = "WebRTC compatibility connected, but the host did not return the folder listing";
            return false;
        }
        return true;
    }
};

WebRtcCompatHost::WebRtcCompatHost() : impl_(new Impl) {
    impl_->bytesOut = &bytesOut;
    impl_->bytesIn = &bytesIn;
    impl_->notifyClientsChanged = [this] { if (onClientsChanged) onClientsChanged(); };
}
WebRtcCompatHost::~WebRtcCompatHost() { stop(); }

bool WebRtcCompatHost::start(const std::string& folder, const std::string& roomCode,
                             bool allowWrites, Server* nativeServer, std::string& err) {
    if (!looks_like_room_code(roomCode)) { err = "WebRTC compatibility needs a 6- or 16-character room code"; return false; }
    if (is_boundary_reparse_point(fs::path(folder))) {
        err = "Cannot host a symlink, junction, or projected filesystem root";
        return false;
    }
    impl_->root = fs::weakly_canonical(folder).string();
    impl_->allowWrites = allowWrites;
    impl_->nativeServer = nativeServer;
    bytesOut = 0;
    bytesIn = 0;
    impl_->wsClosed = false;
    { std::lock_guard<std::mutex> lk(impl_->mtx); impl_->wsError.clear(); }

    std::string cloudErr;
    if (SignalingClient::configured()) {
        impl_->ws = std::make_shared<rtc::WebSocket>();
        impl_->ws->onMessage([this](std::variant<rtc::binary, rtc::string> msg) {
            if (!std::holds_alternative<rtc::string>(msg)) return;
            impl_->on_signal(parse_json_obj(std::get<rtc::string>(msg)));
        });
        impl_->ws->onOpen([this]() { impl_->cv.notify_all(); });
        impl_->ws->onClosed([this]() { impl_->live = false; impl_->wsClosed = true; impl_->cv.notify_all(); });
        const std::string wsUrl = ws_url_for(lookup_of(roomCode), "host");
        compat_dbg("host opening Cloudflare WS: " + wsUrl);
        try { impl_->ws->open(wsUrl); }
        catch (const std::exception& e) { cloudErr = e.what(); }
        if (cloudErr.empty()) {
            std::unique_lock<std::mutex> lk(impl_->mtx);
            impl_->cv.wait_for(lk, std::chrono::seconds(12), [this]{ return impl_->live.load() || impl_->wsClosed.load() || !impl_->wsError.empty(); });
            if (impl_->live.load()) { compat_dbg("host LIVE via Cloudflare"); return true; }
            cloudErr = impl_->wsError.empty() ? "Cloudflare WebRTC compatibility signaling did not become ready" : impl_->wsError;
        }
        compat_dbg("host Cloudflare WS failed: " + cloudErr);
        try { if (impl_->ws) impl_->ws->close(); } catch (...) {}
        impl_->ws.reset();
    } else {
        cloudErr = "Cloudflare signaling URL is not configured for WebRTC compatibility";
    }

    if (FirebaseSignalingClient::configured()) {
        compat_dbg("host falling back to Firebase relay");
        impl_->fbRelay = std::make_shared<FirebaseCompatRelay>();
        if (impl_->fbRelay->startHost(lookup_of(roomCode), [this](const QJsonObject& msg) { impl_->on_signal(msg); }, err)) {
            impl_->live = true;
            compat_dbg("host LIVE via Firebase");
            return true;
        }
        err = cloudErr + "; Firebase WebRTC compatibility fallback failed: " + err;
        impl_->fbRelay.reset();
        return false;
    }

    err = cloudErr + "; Firebase fallback URL is not configured for WebRTC compatibility";
    return false;
}
void WebRtcCompatHost::stop() {
    if (!impl_) return;
    impl_->live = false;
    if (impl_->ws) impl_->ws->close();
    if (impl_->fbRelay) impl_->fbRelay->close();
    bool hadClients = false;
    std::vector<std::shared_ptr<NativeQuicEndpoint>> quicPeers;
    std::vector<std::thread> negotiationTimers;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        hadClients = !impl_->channels.empty();
        for (auto& [id, endpoint] : impl_->quicPeers) quicPeers.push_back(endpoint);
        impl_->quicPeers.clear();
        impl_->pcs.clear();
        impl_->channels.clear();
        impl_->uploads.clear();
        negotiationTimers.swap(impl_->negotiationTimers);
    }
    for (auto& timer : negotiationTimers) if (timer.joinable()) timer.join();
    for (auto& endpoint : quicPeers) endpoint->close();
    if (hadClients) impl_->notifyClients();
}
bool WebRtcCompatHost::running() const { return impl_ && impl_->live.load(); }
int WebRtcCompatHost::clientCount() const { if (!impl_) return 0; std::lock_guard<std::mutex> lk(impl_->mtx); return static_cast<int>(impl_->channels.size()); }

WebRtcRemoteClient::WebRtcRemoteClient() : impl_(new Impl) {}
WebRtcRemoteClient::~WebRtcRemoteClient() { disconnect(); }

bool WebRtcRemoteClient::connect(const std::string& webCodeOrRoom, std::string& err) {
    std::string code;
    if (!extract_web_room(webCodeOrRoom, code)) { err = "not a web-compatible room code"; return false; }
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->open = false;
        impl_->dead = false;
        impl_->canWrite = false;
        impl_->canRange = false;
        impl_->replies.clear();
        impl_->downloads.clear();
    }
    {
        std::lock_guard<std::mutex> lk(impl_->fsMtx);
        impl_->fhs.clear();
        impl_->nextFh = 1;
    }
    {
        std::lock_guard<std::mutex> lk(impl_->sendMtx);
        impl_->dc.reset();
        impl_->nextId = 100;
    }
    rtc::Configuration cfg;
    cfg.iceServers.emplace_back("stun:stun.l.google.com:19302");
    impl_->pc = std::make_shared<rtc::PeerConnection>(cfg);
    impl_->pc->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
        {
            std::lock_guard<std::mutex> lk(impl_->sendMtx);
            impl_->dc = dc;
        }
        dc->onOpen([this]() { std::lock_guard<std::mutex> lk(impl_->mtx); impl_->open = true; impl_->cv.notify_all(); });
        dc->onClosed([this]() { std::lock_guard<std::mutex> lk(impl_->mtx); impl_->dead = true; impl_->open = false; impl_->cv.notify_all(); });
        dc->onMessage([this](std::variant<rtc::binary, rtc::string> msg) {
            if (std::holds_alternative<rtc::string>(msg)) impl_->on_json(parse_json_obj(std::get<rtc::string>(msg)));
            else impl_->on_binary(std::get<rtc::binary>(msg));
        });
    });
    impl_->pc->onLocalDescription([this](rtc::Description desc) {
        QJsonObject sdp; sdp["type"] = "answer"; sdp["sdp"] = QString::fromStdString(std::string(desc));
        QJsonObject payload; payload["type"] = "answer"; payload["sdp"] = sdp;
        impl_->send_signal(payload);
    });
    impl_->pc->onLocalCandidate([this](rtc::Candidate cand) {
        QJsonObject c; c["candidate"] = QString::fromStdString(cand.candidate()); c["sdpMid"] = QString::fromStdString(cand.mid());
        QJsonObject payload; payload["type"] = "candidate"; payload["candidate"] = c;
        impl_->send_signal(payload);
    });
    auto handleRelayObject = [this](const QJsonObject& o) {
        QString kind = o.value("kind").toString();
        if (kind == "ready") {
            impl_->peerId = o.value("peerId").toString().toStdString();
            QJsonObject hello; hello["type"] = "compat-hello"; impl_->send_signal(hello);
            return;
        }
        if (kind == "host-joined") {
            if (!impl_->peerId.empty()) { QJsonObject hello; hello["type"] = "compat-hello"; impl_->send_signal(hello); }
            return;
        }
        if (kind == "host-left") {
            std::lock_guard<std::mutex> lk(impl_->mtx);
            impl_->dead = true;
            impl_->cv.notify_all();
            return;
        }
        if (kind != "signal") return;
        QJsonObject sig = unb64url_json(o.value("ciphertext").toString().toStdString());
        if (sig.value("type").toString() == "offer") {
            QJsonObject sdp = sig.value("sdp").toObject();
            impl_->pc->setRemoteDescription(rtc::Description(sdp.value("sdp").toString().toStdString()));
            impl_->pc->setLocalDescription();
        } else if (sig.value("type").toString() == "candidate") {
            QJsonObject c = sig.value("candidate").toObject();
            impl_->pc->addRemoteCandidate(rtc::Candidate(c.value("candidate").toString().toStdString(), c.value("sdpMid").toString().toStdString()));
        }
    };

    std::string cloudErr;
    if (SignalingClient::configured()) {
        impl_->dead = false;
        impl_->ws = std::make_shared<rtc::WebSocket>();
        impl_->ws->onMessage([handleRelayObject](std::variant<rtc::binary, rtc::string> msg) {
            if (!std::holds_alternative<rtc::string>(msg)) return;
            handleRelayObject(parse_json_obj(std::get<rtc::string>(msg)));
        });
        impl_->ws->onClosed([this]() { std::lock_guard<std::mutex> lk(impl_->mtx); impl_->dead = true; impl_->cv.notify_all(); });
        try { impl_->ws->open(ws_url_for(lookup_of(code), "client")); }
        catch (const std::exception& e) { cloudErr = e.what(); }
        if (cloudErr.empty()) {
            std::unique_lock<std::mutex> lk(impl_->mtx);
            impl_->cv.wait_for(lk, std::chrono::seconds(20), [&]{ return impl_->open.load() || impl_->dead.load(); });
            if (impl_->open.load()) {
                lk.unlock();
                if (impl_->refresh_capabilities(err)) return true;
                disconnect();
                return false;
            }
            cloudErr = "Cloudflare WebRTC compatibility connection timed out";
        }
        try { if (impl_->ws) impl_->ws->close(); } catch (...) {}
        impl_->ws.reset();
    } else {
        cloudErr = "Cloudflare signaling URL is not configured for WebRTC compatibility";
    }

    if (FirebaseSignalingClient::configured()) {
        {
            std::lock_guard<std::mutex> lk(impl_->mtx);
            impl_->dead = false;
            impl_->open = false;
        }
        impl_->fbRelay = std::make_shared<FirebaseCompatRelay>();
        if (!impl_->fbRelay->startClient(lookup_of(code), handleRelayObject, err)) {
            err = cloudErr + "; Firebase WebRTC compatibility fallback failed: " + err;
            impl_->fbRelay.reset();
            return false;
        }
        std::unique_lock<std::mutex> lk(impl_->mtx);
        impl_->cv.wait_for(lk, std::chrono::seconds(20), [&]{ return impl_->open.load() || impl_->dead.load(); });
        if (impl_->open.load()) {
            lk.unlock();
            if (impl_->refresh_capabilities(err)) return true;
            disconnect();
            return false;
        }
        err = cloudErr + "; Firebase WebRTC compatibility connection timed out";
        return false;
    }

    err = cloudErr + "; Firebase fallback URL is not configured for WebRTC compatibility";
    return false;
}
void WebRtcRemoteClient::disconnect() { if (!impl_) return; { std::lock_guard<std::mutex> lk(impl_->mtx); impl_->dead = true; impl_->open = false; impl_->cv.notify_all(); } if (impl_->ws) impl_->ws->close(); if (impl_->fbRelay) impl_->fbRelay->close(); if (impl_->pc) impl_->pc->close(); }
bool WebRtcRemoteClient::connected() const { return impl_ && impl_->open.load(); }
bool WebRtcRemoteClient::canWrite() const { return impl_ && impl_->canWrite.load(); }

int WebRtcRemoteClient::request(uint16_t op, const std::vector<uint8_t>& payload, std::vector<uint8_t>& resp) {
    if (!impl_ || !impl_->open.load()) return EIO;
    if (!impl_->canWrite.load() &&
        (op == OP_WRITE || op == OP_CREATE || op == OP_MKDIR || op == OP_UNLINK ||
         op == OP_RMDIR || op == OP_RENAME || op == OP_TRUNCATE || op == OP_CHMOD ||
         op == OP_UTIMENS)) {
        return EROFS;
    }
    Reader r(payload.data(), payload.size());
    try {
        if (op == OP_GETATTR) {
            std::string path; r.str(path); std::string norm; if (!normalize_rel(path, norm)) return EACCES;
            WireAttr a{};
            if (norm == "/") { a.ino = 1; a.mode = 0040755; a.nlink = 2; a.atime = a.mtime = a.ctime = QDateTime::currentSecsSinceEpoch(); }
            else {
                QJsonObject req; req["t"] = "list"; req["path"] = QString::fromStdString(parent_path(norm));
                QJsonObject reply; if (!impl_->send_json_wait(req, reply) || impl_->json_error(reply)) return EIO;
                bool found = false; for (auto v : reply.value("entries").toArray()) { QJsonObject e = v.toObject(); if (e.value("name").toString().toStdString() == basename(norm)) { a = attr_from_web_entry(e); found = true; break; } }
                if (!found) return ENOENT;
            }
            Writer w; w.pod(a); resp = std::move(w.b); return 0;
        }
        if (op == OP_READDIR) {
            std::string path; r.str(path); QJsonObject req; req["t"] = "list"; req["path"] = QString::fromStdString(path);
            QJsonObject reply; if (!impl_->send_json_wait(req, reply) || impl_->json_error(reply)) return EIO;
            QJsonArray arr = reply.value("entries").toArray(); Writer w; uint32_t n = static_cast<uint32_t>(arr.size()); w.pod(n);
            for (auto v : arr) { QJsonObject e = v.toObject(); std::string name = e.value("name").toString().toStdString(); WireAttr a = attr_from_web_entry(e); w.str(name); w.pod(a); }
            resp = std::move(w.b); return 0;
        }
        if (op == OP_OPEN || op == OP_CREATE) {
            std::string path; int32_t flags = 0; uint32_t mode = 0;
            if (!r.str(path) || !r.pod(flags) || !r.pod(mode)) return EINVAL;
            const bool writeIntent = op == OP_CREATE ||
                                      ((flags & FB_O_ACCMODE) == FB_O_WRONLY) ||
                                      ((flags & FB_O_ACCMODE) == FB_O_RDWR) ||
                                      (flags & (FB_O_CREAT | FB_O_TRUNC | FB_O_APPEND));
            if (writeIntent && !impl_->canWrite.load()) return EROFS;
            // O_TRUNC discards existing content. O_CREAT without O_TRUNC must
            // preserve an existing file, so only start from an empty buffer
            // when the file does not exist yet; otherwise lazy-load on first
            // read/write.
            bool startEmpty = (flags & FB_O_TRUNC) != 0;
            if (!startEmpty && (op == OP_CREATE || (flags & FB_O_CREAT))) {
                Writer probe; probe.str(path);
                std::vector<uint8_t> attr;
                startEmpty = request(OP_GETATTR, probe.b, attr) == ENOENT;
            }
            uint64_t fh = 0;
            {
                std::lock_guard<std::mutex> lk(impl_->fsMtx);
                fh = impl_->nextFh++;
                WebRtcRemoteClient::Impl::Fh f; f.path = path;
                if (startEmpty) {
                    f.loaded = true;
                    f.dirty = true;
                }
                impl_->fhs[fh] = std::move(f);
            }
            Writer w; w.pod(fh); resp = std::move(w.b); return 0;
        }
        if (op == OP_READ) {
            uint64_t fh = 0, off = 0; uint32_t size = 0;
            if (!r.pod(fh) || !r.pod(off) || !r.pod(size)) return EINVAL;
            std::string path;
            bool loaded = false;
            {
                std::lock_guard<std::mutex> lk(impl_->fsMtx);
                auto it = impl_->fhs.find(fh);
                if (it == impl_->fhs.end()) return EBADF;
                path = it->second.path;
                loaded = it->second.loaded;
            }
            // Range-capable host and nothing buffered locally: stream just the
            // requested window instead of materialising the whole file. The
            // RamCache layered above turns these into cached 1 MiB blocks
            // with read-ahead, so huge files stay cheap on both sides.
            if (!loaded && impl_->canRange.load()) {
                std::vector<uint8_t> data;
                int st = impl_->fetch_range(path, off, size, data);
                if (st) return st;
                if (data.size() > size) data.resize(size);
                bytesRead += data.size();
                resp = std::move(data);
                return 0;
            }
            if (!loaded) {
                std::vector<uint8_t> data;
                int st = impl_->fetch_file(path, data);
                if (st) return st;
                std::lock_guard<std::mutex> lk(impl_->fsMtx);
                auto it = impl_->fhs.find(fh);
                if (it == impl_->fhs.end()) return EBADF;
                if (!it->second.loaded) {
                    it->second.data = std::move(data);
                    it->second.loaded = true;
                }
            }
            std::lock_guard<std::mutex> lk(impl_->fsMtx);
            auto it = impl_->fhs.find(fh);
            if (it == impl_->fhs.end()) return EBADF;
            if (off >= it->second.data.size()) { resp.clear(); return 0; }
            size_t pos = static_cast<size_t>(off);
            size_t n = std::min<size_t>(size, it->second.data.size() - pos);
            resp.assign(it->second.data.begin() + pos, it->second.data.begin() + pos + n); bytesRead += n; return 0;
        }
        if (op == OP_WRITE) {
            if (!impl_->canWrite.load()) return EROFS;
            uint64_t fh, off; r.pod(fh); r.pod(off);
            std::string path;
            bool loaded = false;
            {
                std::lock_guard<std::mutex> lk(impl_->fsMtx);
                auto it = impl_->fhs.find(fh);
                if (it == impl_->fhs.end()) return EBADF;
                path = it->second.path;
                loaded = it->second.loaded;
            }
            if (!loaded) {
                std::vector<uint8_t> data;
                int st = impl_->fetch_file(path, data);
                if (st) return st;
                std::lock_guard<std::mutex> lk(impl_->fsMtx);
                auto it = impl_->fhs.find(fh);
                if (it == impl_->fhs.end()) return EBADF;
                if (!it->second.loaded) {
                    it->second.data = std::move(data);
                    it->second.loaded = true;
                }
            }
            size_t n = static_cast<size_t>(r.e - r.p);
            if (off > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) - n) return EIO;
            std::lock_guard<std::mutex> lk(impl_->fsMtx);
            auto it = impl_->fhs.find(fh);
            if (it == impl_->fhs.end()) return EBADF;
            size_t pos = static_cast<size_t>(off);
            if (it->second.data.size() < pos + n) it->second.data.resize(pos + n);
            std::memcpy(it->second.data.data() + pos, r.p, n); it->second.dirty = true; it->second.loaded = true; bytesWritten += n; Writer w; uint32_t written = static_cast<uint32_t>(n); w.pod(written); resp = std::move(w.b); return 0;
        }
        if (op == OP_RELEASE) {
            uint64_t fh; r.pod(fh);
            WebRtcRemoteClient::Impl::Fh file;
            {
                std::lock_guard<std::mutex> lk(impl_->fsMtx);
                auto it = impl_->fhs.find(fh);
                if (it == impl_->fhs.end()) return 0;
                file = std::move(it->second);
                impl_->fhs.erase(it);
            }
            return file.dirty ? impl_->upload_file(file.path, file.data) : 0;
        }
        if (op == OP_UNLINK || op == OP_RMDIR) {
            if (!impl_->canWrite.load()) return EROFS;
            std::string path; r.str(path); QJsonObject req; req["t"] = "delete"; req["path"] = QString::fromStdString(path); QJsonObject reply; if (!impl_->send_json_wait(req, reply) || impl_->json_error(reply)) return EIO; return 0;
        }
        if (op == OP_MKDIR) {
            if (!impl_->canWrite.load()) return EROFS;
            std::string path; uint32_t mode; r.str(path); r.pod(mode); (void)mode;
            QJsonObject req; req["t"] = "mkdir"; req["path"] = QString::fromStdString(path);
            QJsonObject reply; if (!impl_->send_json_wait(req, reply) || impl_->json_error(reply)) return EIO; return 0;
        }
        if (op == OP_TRUNCATE) {
            if (!impl_->canWrite.load()) return EROFS;
            std::string path; uint64_t size; r.str(path); r.pod(size);
            if (size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) return EIO;
            std::vector<uint8_t> data;
            int st = impl_->fetch_file(path, data);
            if (st) return st;
            data.resize(static_cast<size_t>(size));
            return impl_->upload_file(path, data);
        }
        if (op == OP_ACCESS) {
            std::string path; uint32_t mode; r.str(path); r.pod(mode);
            if (!impl_->canWrite.load() && (mode & 2u)) return EROFS;
            Writer w; w.str(path);
            std::vector<uint8_t> attr;
            return request(OP_GETATTR, w.b, attr);
        }
        if (op == OP_FLUSH || op == OP_FSYNC) return 0;
        if (op == OP_STATFS) { WireStatvfs s{}; s.bsize = s.frsize = 4096; s.blocks = s.bfree = s.bavail = 1024ull * 1024 * 1024; s.namemax = 255; Writer w; w.pod(s); resp = std::move(w.b); return 0; }
        return ENOSYS;
    } catch (...) { return EIO; }
}

struct NativeQuicRemoteClient::Impl {
    Client client;
    std::shared_ptr<NativeQuicEndpoint> endpoint;
    std::shared_ptr<rtc::WebSocket> ws;
    std::mutex mutex;
    std::condition_variable cv;
    std::string peerId;
    std::string pendingDescription;
    std::string signalError;
    bool ready = false;
    bool dead = false;

    void send(const QJsonObject& payload) {
        std::shared_ptr<rtc::WebSocket> socket;
        std::string peer;
        {
            std::lock_guard<std::mutex> lock(mutex);
            socket = ws;
            peer = peerId;
        }
        if (socket && !peer.empty()) send_plain_signal(socket, peer, payload);
    }
};

NativeQuicRemoteClient::NativeQuicRemoteClient() : impl_(new Impl) {}
NativeQuicRemoteClient::~NativeQuicRemoteClient() { disconnect(); }

bool NativeQuicRemoteClient::connect(const std::string& roomCode, const Token& token,
                                     std::string& err) {
    disconnect();
    if (!native_quic_available()) { err = "native QUIC support was not built"; return false; }
    if (!looks_like_room_code(roomCode) || !SignalingClient::configured()) {
        err = "native QUIC needs a published room code and Cloudflare signaling";
        return false;
    }

    impl_->endpoint = std::make_shared<NativeQuicEndpoint>(NativeQuicEndpoint::Role::Client);
    impl_->endpoint->setLocalDescriptionCallback([this](const std::string& description) {
        bool canSend = false;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->pendingDescription = description;
            canSend = impl_->ready && !impl_->peerId.empty();
        }
        if (canSend) {
            QJsonObject payload; payload["type"] = "native-quic-description";
            payload["description"] = QString::fromStdString(description);
            impl_->send(payload);
        }
    });
    impl_->endpoint->setStateCallback([](const std::string& state) {
        compat_dbg("native QUIC client state=" + state);
    });
    if (!impl_->endpoint->start(err)) { disconnect(); return false; }

    impl_->ws = std::make_shared<rtc::WebSocket>();
    impl_->ws->onMessage([this](std::variant<rtc::binary, rtc::string> message) {
        if (!std::holds_alternative<rtc::string>(message)) return;
        QJsonObject outer = parse_json_obj(std::get<rtc::string>(message));
        const QString kind = outer.value("kind").toString();
        if (kind == "ready") {
            std::string description;
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                impl_->peerId = outer.value("peerId").toString().toStdString();
                impl_->ready = true;
                description = impl_->pendingDescription;
            }
            QJsonObject hello; hello["type"] = "native-quic-hello";
            impl_->send(hello);
            if (!description.empty()) {
                QJsonObject payload; payload["type"] = "native-quic-description";
                payload["description"] = QString::fromStdString(description);
                impl_->send(payload);
            }
            impl_->cv.notify_all();
            return;
        }
        if (kind == "host-joined") {
            std::string description;
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                description = impl_->pendingDescription;
            }
            QJsonObject hello; hello["type"] = "native-quic-hello";
            impl_->send(hello);
            if (!description.empty()) {
                QJsonObject payload; payload["type"] = "native-quic-description";
                payload["description"] = QString::fromStdString(description);
                impl_->send(payload);
            }
            return;
        }
        if (kind == "host-left") {
            std::shared_ptr<NativeQuicEndpoint> endpoint;
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                impl_->dead = true;
                impl_->signalError = "host left during QUIC negotiation";
                endpoint = impl_->endpoint;
                impl_->cv.notify_all();
            }
            if (endpoint) endpoint->close();
            return;
        }
        if (kind != "signal") return;
        QJsonObject signal = unb64url_json(outer.value("ciphertext").toString().toStdString());
        const QString type = signal.value("type").toString();
        if (type == "native-quic-description") {
            std::shared_ptr<NativeQuicEndpoint> endpoint;
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                endpoint = impl_->endpoint;
            }
            std::string setErr;
            if (!endpoint || !endpoint->setRemoteDescription(
                    signal.value("description").toString().toStdString(), setErr)) {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                impl_->signalError = setErr.empty() ? "native QUIC endpoint closed" : setErr;
                impl_->dead = true;
                impl_->cv.notify_all();
            }
        } else if (type == "native-quic-error") {
            std::shared_ptr<NativeQuicEndpoint> endpoint;
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                impl_->signalError = signal.value("message").toString().toStdString();
                impl_->dead = true;
                endpoint = impl_->endpoint;
                impl_->cv.notify_all();
            }
            if (endpoint) endpoint->close();
        }
    });
    impl_->ws->onClosed([this]() {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->endpoint || !impl_->endpoint->connected()) impl_->dead = true;
        impl_->cv.notify_all();
    });
    try { impl_->ws->open(ws_url_for(room_lookup_id(roomCode), "client")); }
    catch (const std::exception& e) { err = e.what(); disconnect(); return false; }

    {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        if (!impl_->cv.wait_for(lock, std::chrono::seconds(8), [this] {
                return impl_->ready || impl_->dead;
            }) || !impl_->ready) {
            err = impl_->signalError.empty() ? "native QUIC signaling timed out" : impl_->signalError;
            lock.unlock(); disconnect(); return false;
        }
    }
    if (!impl_->endpoint->waitConnected(std::chrono::seconds(12), err)) {
        disconnect();
        return false;
    }
    auto streams = impl_->endpoint->openStreams(kDefaultConns, err);
    if (streams.empty() || !impl_->client.connectStreams(token, std::move(streams), err)) {
        disconnect();
        return false;
    }
    // Keep the existing signaling socket open as presence. Closing it here
    // would emit client-left and make the host tear down the negotiated ICE
    // endpoint. No file data is sent over this socket.
    return true;
}

void NativeQuicRemoteClient::disconnect() {
    if (!impl_) return;
    impl_->client.disconnect();
    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<NativeQuicEndpoint> endpoint;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        ws = std::move(impl_->ws);
        endpoint = std::move(impl_->endpoint);
        impl_->peerId.clear(); impl_->pendingDescription.clear(); impl_->signalError.clear();
        impl_->ready = false; impl_->dead = false;
    }
    try { if (ws) ws->close(); } catch (...) {}
    if (endpoint) endpoint->close();
}
bool NativeQuicRemoteClient::connected() const { return impl_ && impl_->client.connected(); }
int NativeQuicRemoteClient::request(uint16_t op, const std::vector<uint8_t>& payload,
                                    std::vector<uint8_t>& resp) {
    return impl_ ? impl_->client.request(op, payload, resp) : EIO;
}

#else

struct WebRtcCompatHost::Impl {};
struct WebRtcRemoteClient::Impl {};
WebRtcCompatHost::WebRtcCompatHost() : impl_(new Impl) {}
WebRtcCompatHost::~WebRtcCompatHost() = default;
bool WebRtcCompatHost::start(const std::string&, const std::string&, bool, Server*, std::string& err) { err = "libdatachannel support was not built"; return false; }
void WebRtcCompatHost::stop() {}
bool WebRtcCompatHost::running() const { return false; }
int WebRtcCompatHost::clientCount() const { return 0; }
WebRtcRemoteClient::WebRtcRemoteClient() : impl_(new Impl) {}
WebRtcRemoteClient::~WebRtcRemoteClient() = default;
bool WebRtcRemoteClient::connect(const std::string&, std::string& err) { err = "libdatachannel support was not built"; return false; }
void WebRtcRemoteClient::disconnect() {}
bool WebRtcRemoteClient::connected() const { return false; }
bool WebRtcRemoteClient::canWrite() const { return false; }
int WebRtcRemoteClient::request(uint16_t, const std::vector<uint8_t>&, std::vector<uint8_t>&) { return EIO; }
struct NativeQuicRemoteClient::Impl {};
NativeQuicRemoteClient::NativeQuicRemoteClient() : impl_(new Impl) {}
NativeQuicRemoteClient::~NativeQuicRemoteClient() = default;
bool NativeQuicRemoteClient::connect(const std::string&, const Token&, std::string& err) { err = "native QUIC signaling support was not built"; return false; }
void NativeQuicRemoteClient::disconnect() {}
bool NativeQuicRemoteClient::connected() const { return false; }
int NativeQuicRemoteClient::request(uint16_t, const std::vector<uint8_t>&, std::vector<uint8_t>&) { return EIO; }
#endif

bool web_compat_available() {
#ifdef FB_HAVE_LIBDATACHANNEL
    return true;
#else
    return false;
#endif
}

} // namespace fb
