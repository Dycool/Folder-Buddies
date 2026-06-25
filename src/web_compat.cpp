#include "web_compat.h"

#include "base91.h"
#include "common.h"
#include "fileio.h"
#include "osflags.h"
#include "signaling.h"

#include <QByteArray>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#ifdef FB_HAVE_LIBDATACHANNEL
#  include <rtc/rtc.hpp>
#endif

namespace fb {
namespace {

namespace fs = std::filesystem;
constexpr uint32_t kBinMagic = 0x4642494eu; // FBIN, big endian in browser buffers
constexpr size_t kWebChunk = 64 * 1024;

std::string strip_ws(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        if (!std::isspace(c)) out.push_back(static_cast<char>(c));
    return out;
}

std::string lookup_of(const std::string& code) {
    return code.size() >= static_cast<size_t>(kLookupLen) ? code.substr(0, kLookupLen) : code;
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
    std::string base = SignalingClient::base_url();
    if (base.rfind("https://", 0) == 0) base.replace(0, 5, "wss");
    else if (base.rfind("http://", 0) == 0) base.replace(0, 4, "ws");
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/room?code=" + lookup + "&role=" + role + "&web=1&compat=native";
}

#ifdef FB_HAVE_LIBDATACHANNEL

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
    int maxClients = 0;
    std::shared_ptr<rtc::WebSocket> ws;
    std::mutex mtx;
    std::unordered_map<std::string, std::shared_ptr<rtc::PeerConnection>> pcs;
    std::unordered_map<std::string, std::shared_ptr<rtc::DataChannel>> channels;
    std::unordered_map<uint32_t, std::ofstream> uploads;
    std::atomic<bool> live{false};

    bool resolve(const std::string& rel, fs::path& out) {
        std::string norm;
        if (!normalize_rel(rel, norm)) return false;
        std::error_code ec;
        fs::path rootPath = fs::weakly_canonical(root, ec);
        if (ec) return false;
        fs::path raw = rootPath / fs::path(norm.substr(1));
        fs::path candidate;
        if (fs::exists(raw, ec)) candidate = fs::weakly_canonical(raw, ec);
        else {
            fs::path parent = fs::weakly_canonical(raw.parent_path(), ec);
            if (ec) parent = rootPath;
            candidate = parent / raw.filename();
        }
        std::string rp = rootPath.string();
        std::string cp = candidate.lexically_normal().string();
        if (cp.rfind(rp, 0) != 0) return false;
        out = candidate;
        return true;
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
                QJsonObject out; out["t"] = "listResult"; out["id"] = id; out["path"] = QString::fromStdString(path); out["entries"] = entries; out["write"] = allowWrites;
                dc->send(json_compact(out).toStdString());
                return;
            }
            if (t == "download") {
                fs::path abs;
                if (!resolve(msg.value("path").toString().toStdString(), abs) || !fs::is_regular_file(abs)) throw std::runtime_error("Not a file");
                uint64_t size = fs::file_size(abs);
                QJsonObject start; start["t"] = "fileStart"; start["id"] = id; start["name"] = QString::fromStdString(abs.filename().string()); start["size"] = static_cast<double>(size);
                dc->send(json_compact(start).toStdString());
                std::ifstream f(abs, std::ios::binary);
                std::vector<uint8_t> buf(kWebChunk);
                while (f) {
                    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
                    std::streamsize got = f.gcount();
                    if (got > 0) send_bin(dc, static_cast<uint32_t>(id), buf.data(), static_cast<size_t>(got));
                }
                QJsonObject end; end["t"] = "fileEnd"; end["id"] = id; dc->send(json_compact(end).toStdString());
                return;
            }
            if (t == "uploadStart") {
                if (!allowWrites) throw std::runtime_error("The host has writes disabled");
                fs::path abs;
                if (!resolve(msg.value("path").toString().toStdString(), abs)) throw std::runtime_error("Bad upload path");
                fs::create_directories(abs.parent_path());
                std::lock_guard<std::mutex> lk(mtx);
                uploads[static_cast<uint32_t>(id)] = std::ofstream(abs, std::ios::binary | std::ios::trunc);
                QJsonObject out; out["t"] = "uploadReady"; out["id"] = id; dc->send(json_compact(out).toStdString());
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
        for (size_t i = 8; i < b.size(); ++i) {
            char c = static_cast<char>(static_cast<uint8_t>(b[i]));
            it->second.write(&c, 1);
        }
    }

    void create_peer(const std::string& peerId) {
        std::lock_guard<std::mutex> lk(mtx);
        if (maxClients > 0 && static_cast<int>(pcs.size()) >= maxClients) return;
        rtc::Configuration cfg;
        cfg.iceServers.emplace_back("stun:stun.l.google.com:19302");
        auto pc = std::make_shared<rtc::PeerConnection>(cfg);
        auto dc = pc->createDataChannel("folderbuddies-files");
        pcs[peerId] = pc;
        channels[peerId] = dc;
        dc->onMessage([this, peerId, dc](std::variant<rtc::binary, rtc::string> message) {
            if (std::holds_alternative<rtc::string>(message)) on_json(peerId, dc, parse_json_obj(std::get<rtc::string>(message)));
            else on_binary(std::get<rtc::binary>(message));
        });
        pc->onLocalDescription([this, peerId](rtc::Description desc) {
            QJsonObject sdp; sdp["type"] = "offer"; sdp["sdp"] = QString::fromStdString(std::string(desc));
            QJsonObject payload; payload["type"] = "offer"; payload["sdp"] = sdp;
            send_plain_signal(ws, peerId, payload);
        });
        pc->onLocalCandidate([this, peerId](rtc::Candidate cand) {
            QJsonObject c; c["candidate"] = QString::fromStdString(cand.candidate()); c["sdpMid"] = QString::fromStdString(cand.mid());
            QJsonObject payload; payload["type"] = "candidate"; payload["candidate"] = c;
            send_plain_signal(ws, peerId, payload);
        });
        pc->setLocalDescription();
    }

    void on_signal(const QJsonObject& msg) {
        const QString kind = msg.value("kind").toString();
        if (kind == "client-joined") { create_peer(msg.value("peerId").toString().toStdString()); return; }
        if (kind == "client-left") { std::lock_guard<std::mutex> lk(mtx); pcs.erase(msg.value("peerId").toString().toStdString()); channels.erase(msg.value("peerId").toString().toStdString()); return; }
        if (kind != "signal") return;
        std::string peerId = msg.value("peerId").toString().toStdString();
        QJsonObject sig = unb64url_json(msg.value("ciphertext").toString().toStdString());
        if (sig.isEmpty()) return;
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
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc;
    std::string peerId;
    std::mutex mtx;
    std::condition_variable cv;
    bool open = false;
    bool dead = false;
    std::unordered_map<uint32_t, QJsonObject> replies;
    std::unordered_map<uint32_t, std::vector<uint8_t>> downloads;
    struct Fh { std::string path; std::vector<uint8_t> data; bool dirty = false; };
    std::unordered_map<uint64_t, Fh> fhs;
    uint64_t nextFh = 1;
    uint32_t nextId = 100;
    bool canWrite = false;

    uint32_t send_json_wait(QJsonObject obj, QJsonObject& out, int timeoutMs = 30000) {
        uint32_t id = obj.value("id").toInt(0);
        if (!id) id = nextId++;
        obj["id"] = static_cast<int>(id);
        dc->send(json_compact(obj).toStdString());
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&]{ return replies.count(id) || dead; });
        auto it = replies.find(id);
        if (it == replies.end()) return 0;
        out = it->second;
        replies.erase(it);
        return id;
    }
    bool wait_file(uint32_t id, std::vector<uint8_t>& out, int timeoutMs = 120000) {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&]{ return replies.count(id) || dead; });
        if (!replies.count(id)) return false;
        replies.erase(id);
        out = std::move(downloads[id]);
        downloads.erase(id);
        return true;
    }
    void on_json(const QJsonObject& o) {
        std::lock_guard<std::mutex> lk(mtx);
        QString t = o.value("t").toString();
        if (t == "listResult") canWrite = o.value("write").toBool(canWrite);
        // A download emits fileStart, binary chunks, then fileEnd. Only fileEnd
        // completes the synchronous request; otherwise native reads can return
        // before the browser has sent the file bytes.
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
    int json_error(const QJsonObject& o) {
        if (o.value("t").toString() == "error") return EIO;
        return 0;
    }
    int fetch_file(const std::string& path, std::vector<uint8_t>& data) {
        uint32_t id = nextId++;
        QJsonObject req; req["t"] = "download"; req["id"] = static_cast<int>(id); req["path"] = QString::fromStdString(path);
        dc->send(json_compact(req).toStdString());
        return wait_file(id, data) ? 0 : EIO;
    }
    int upload_file(const std::string& path, const std::vector<uint8_t>& data) {
        uint32_t id = nextId++;
        QJsonObject start; start["t"] = "uploadStart"; start["id"] = static_cast<int>(id); start["path"] = QString::fromStdString(path); start["size"] = static_cast<double>(data.size());
        QJsonObject reply;
        if (!send_json_wait(start, reply) || json_error(reply)) return EIO;
        size_t off = 0;
        while (off < data.size()) {
            size_t n = std::min(kWebChunk, data.size() - off);
            send_bin(dc, id, data.data() + off, n);
            off += n;
        }
        QJsonObject end; end["t"] = "uploadEnd"; end["id"] = static_cast<int>(id);
        if (!send_json_wait(end, reply, 60000) || json_error(reply)) return EIO;
        return 0;
    }
};

WebRtcCompatHost::WebRtcCompatHost() : impl_(new Impl) {}
WebRtcCompatHost::~WebRtcCompatHost() { stop(); }

bool WebRtcCompatHost::start(const std::string& folder, const std::string& roomCode, bool allowWrites, int maxClients, std::string& err) {
    if (!SignalingClient::configured()) { err = "Cloudflare signaling URL is not configured for WebRTC compatibility"; return false; }
    if (!looks_like_room_code(roomCode)) { err = "WebRTC compatibility needs a 6-character room code"; return false; }
    impl_->root = fs::weakly_canonical(folder).string();
    impl_->allowWrites = allowWrites;
    impl_->maxClients = maxClients;
    impl_->ws = std::make_shared<rtc::WebSocket>();
    impl_->ws->onMessage([this](std::variant<rtc::binary, rtc::string> msg) {
        if (!std::holds_alternative<rtc::string>(msg)) return;
        impl_->on_signal(parse_json_obj(std::get<rtc::string>(msg)));
    });
    impl_->ws->onOpen([this]() { impl_->live = true; });
    impl_->ws->onClosed([this]() { impl_->live = false; });
    impl_->ws->open(ws_url_for(lookup_of(roomCode), "host"));
    return true;
}
void WebRtcCompatHost::stop() { if (!impl_) return; impl_->live = false; if (impl_->ws) impl_->ws->close(); std::lock_guard<std::mutex> lk(impl_->mtx); impl_->pcs.clear(); impl_->channels.clear(); impl_->uploads.clear(); }
bool WebRtcCompatHost::running() const { return impl_ && impl_->live.load(); }
int WebRtcCompatHost::clientCount() const { if (!impl_) return 0; std::lock_guard<std::mutex> lk(impl_->mtx); return static_cast<int>(impl_->channels.size()); }

WebRtcRemoteClient::WebRtcRemoteClient() : impl_(new Impl) {}
WebRtcRemoteClient::~WebRtcRemoteClient() { disconnect(); }

bool WebRtcRemoteClient::connect(const std::string& webCodeOrRoom, std::string& err) {
    std::string code;
    if (!extract_web_room(webCodeOrRoom, code)) { err = "not a web-compatible room code"; return false; }
    if (!SignalingClient::configured()) { err = "Cloudflare signaling URL is not configured for WebRTC compatibility"; return false; }
    rtc::Configuration cfg;
    cfg.iceServers.emplace_back("stun:stun.l.google.com:19302");
    impl_->pc = std::make_shared<rtc::PeerConnection>(cfg);
    impl_->pc->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
        impl_->dc = dc;
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
        send_plain_signal(impl_->ws, impl_->peerId, payload);
    });
    impl_->pc->onLocalCandidate([this](rtc::Candidate cand) {
        QJsonObject c; c["candidate"] = QString::fromStdString(cand.candidate()); c["sdpMid"] = QString::fromStdString(cand.mid());
        QJsonObject payload; payload["type"] = "candidate"; payload["candidate"] = c;
        send_plain_signal(impl_->ws, impl_->peerId, payload);
    });
    impl_->ws = std::make_shared<rtc::WebSocket>();
    impl_->ws->onMessage([this](std::variant<rtc::binary, rtc::string> msg) {
        if (!std::holds_alternative<rtc::string>(msg)) return;
        QJsonObject o = parse_json_obj(std::get<rtc::string>(msg));
        QString kind = o.value("kind").toString();
        if (kind == "ready") {
            impl_->peerId = o.value("peerId").toString().toStdString();
            QJsonObject hello; hello["type"] = "compat-hello"; send_plain_signal(impl_->ws, impl_->peerId, hello);
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
    });
    impl_->ws->onClosed([this]() { std::lock_guard<std::mutex> lk(impl_->mtx); impl_->dead = true; impl_->cv.notify_all(); });
    impl_->ws->open(ws_url_for(lookup_of(code), "client"));
    std::unique_lock<std::mutex> lk(impl_->mtx);
    if (!impl_->cv.wait_for(lk, std::chrono::seconds(20), [&]{ return impl_->open || impl_->dead; }) || !impl_->open) {
        err = "WebRTC compatibility connection timed out";
        return false;
    }
    return true;
}
void WebRtcRemoteClient::disconnect() { if (!impl_) return; { std::lock_guard<std::mutex> lk(impl_->mtx); impl_->dead = true; impl_->open = false; impl_->cv.notify_all(); } if (impl_->ws) impl_->ws->close(); if (impl_->pc) impl_->pc->close(); }
bool WebRtcRemoteClient::connected() const { return impl_ && impl_->open; }

int WebRtcRemoteClient::request(uint16_t op, const std::vector<uint8_t>& payload, std::vector<uint8_t>& resp) {
    if (!impl_ || !impl_->open || !impl_->dc) return EIO;
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
            std::string path; int32_t flags; uint32_t mode; r.str(path); r.pod(flags); r.pod(mode);
            uint64_t fh = impl_->nextFh++;
            WebRtcRemoteClient::Impl::Fh f; f.path = path;
            if (op == OP_CREATE || (flags & (FB_O_CREAT | FB_O_TRUNC))) { f.dirty = true; }
            impl_->fhs[fh] = std::move(f); Writer w; w.pod(fh); resp = std::move(w.b); return 0;
        }
        if (op == OP_READ) {
            uint64_t fh, off; uint32_t size; r.pod(fh); r.pod(off); r.pod(size); auto it = impl_->fhs.find(fh); if (it == impl_->fhs.end()) return EBADF;
            if (it->second.data.empty()) { int st = impl_->fetch_file(it->second.path, it->second.data); if (st) return st; }
            if (off >= it->second.data.size()) { resp.clear(); return 0; }
            size_t n = std::min<size_t>(size, it->second.data.size() - static_cast<size_t>(off));
            resp.assign(it->second.data.begin() + static_cast<size_t>(off), it->second.data.begin() + static_cast<size_t>(off) + n); bytesRead += n; return 0;
        }
        if (op == OP_WRITE) {
            uint64_t fh, off; r.pod(fh); r.pod(off); auto it = impl_->fhs.find(fh); if (it == impl_->fhs.end()) return EBADF;
            size_t n = static_cast<size_t>(r.e - r.p); if (it->second.data.size() < off + n) it->second.data.resize(static_cast<size_t>(off + n)); std::memcpy(it->second.data.data() + off, r.p, n); it->second.dirty = true; bytesWritten += n; Writer w; uint32_t written = static_cast<uint32_t>(n); w.pod(written); resp = std::move(w.b); return 0;
        }
        if (op == OP_RELEASE) {
            uint64_t fh; r.pod(fh); auto it = impl_->fhs.find(fh); if (it == impl_->fhs.end()) return 0; int st = 0; if (it->second.dirty) st = impl_->upload_file(it->second.path, it->second.data); impl_->fhs.erase(it); return st;
        }
        if (op == OP_UNLINK || op == OP_RMDIR) {
            std::string path; r.str(path); QJsonObject req; req["t"] = "delete"; req["path"] = QString::fromStdString(path); QJsonObject reply; if (!impl_->send_json_wait(req, reply) || impl_->json_error(reply)) return EIO; return 0;
        }
        if (op == OP_FLUSH || op == OP_FSYNC) return 0;
        if (op == OP_STATFS) { WireStatvfs s{}; s.bsize = s.frsize = 4096; s.blocks = s.bfree = s.bavail = 1024ull * 1024 * 1024; s.namemax = 255; Writer w; w.pod(s); resp = std::move(w.b); return 0; }
        return ENOSYS;
    } catch (...) { return EIO; }
}

#else

struct WebRtcCompatHost::Impl {};
struct WebRtcRemoteClient::Impl {};
WebRtcCompatHost::WebRtcCompatHost() : impl_(new Impl) {}
WebRtcCompatHost::~WebRtcCompatHost() = default;
bool WebRtcCompatHost::start(const std::string&, const std::string&, bool, int, std::string& err) { err = "libdatachannel support was not built"; return false; }
void WebRtcCompatHost::stop() {}
bool WebRtcCompatHost::running() const { return false; }
int WebRtcCompatHost::clientCount() const { return 0; }
WebRtcRemoteClient::WebRtcRemoteClient() : impl_(new Impl) {}
WebRtcRemoteClient::~WebRtcRemoteClient() = default;
bool WebRtcRemoteClient::connect(const std::string&, std::string& err) { err = "libdatachannel support was not built"; return false; }
void WebRtcRemoteClient::disconnect() {}
bool WebRtcRemoteClient::connected() const { return false; }
int WebRtcRemoteClient::request(uint16_t, const std::vector<uint8_t>&, std::vector<uint8_t>&) { return EIO; }
#endif

bool web_compat_available() {
#ifdef FB_HAVE_LIBDATACHANNEL
    return true;
#else
    return false;
#endif
}

} // namespace fb
