#include "signaling.h"

#include "auth.h"
#include "base91.h"
#include "common.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <array>
#include <cstdlib>
#include <cstring>
#include <cctype>

namespace fb {
namespace {

// Replace this after you deploy the Worker, for example:
//   https://folderbuddies-signaling.<your-subdomain>.workers.dev
#ifndef FB_SIGNALING_URL
constexpr const char* kHardcodedWorkerUrl = "";
#else
constexpr const char* kHardcodedWorkerUrl = FB_SIGNALING_URL;
#endif

constexpr char kPayloadMagic[5] = {'F', 'B', 'Z', 'K', '1'};
constexpr char kPayloadAadPrefix[] = "FB-room-payload-v1";
constexpr uint32_t kPayloadVersion = 1;

QByteArray qbytes(const std::vector<uint8_t>& v) {
    return QByteArray(reinterpret_cast<const char*>(v.data()), static_cast<int>(v.size()));
}

std::vector<uint8_t> vec(const QByteArray& b) {
    return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(b.constData()),
                                reinterpret_cast<const uint8_t*>(b.constData()) + b.size());
}

QByteArray password_root(const std::string& password) {
    QByteArray material("FolderBuddies-password-root-v1", 31);
    material.append('\0');
    material.append(password.data(), static_cast<int>(password.size()));
    return QCryptographicHash::hash(material, QCryptographicHash::Sha256);
}

Key256 payload_key(const std::string& password, const std::vector<uint8_t>& salt) {
    QByteArray root = password_root(password);
    QByteArray material = QByteArray(kPayloadAadPrefix) + "\0" + qbytes(salt);
    return to_key256(hmac_sha256(root, material));
}

QByteArray payload_aad(const std::string& aadRoomCode) {
    return QByteArray(kPayloadAadPrefix) + "\0" + QByteArray(aadRoomCode.data(),
                                                             static_cast<int>(aadRoomCode.size()));
}

std::vector<uint8_t> serialize_token_payload(const Token& tok) {
    Writer w;
    w.raw(kPayloadMagic, sizeof(kPayloadMagic));
    w.pod(kPayloadVersion);
    w.str(tok.ip);
    w.pod(tok.port);
    w.str(tok.folder);
    w.bytes(tok.secret.data(), static_cast<uint32_t>(tok.secret.size()));
    return w.b;
}

bool deserialize_token_payload(const std::vector<uint8_t>& plain, Token& out) {
    Reader r(plain.data(), plain.size());
    char magic[sizeof(kPayloadMagic)]{};
    uint32_t version = 0;
    if (!r.raw(magic, sizeof(magic)) || std::memcmp(magic, kPayloadMagic, sizeof(magic)) != 0)
        return false;
    if (!r.pod(version) || version != kPayloadVersion) return false;
    if (!r.str(out.ip)) return false;
    if (!r.pod(out.port)) return false;
    if (!r.str(out.folder)) return false;
    if (!r.bytes(out.secret)) return false;
    return !out.ip.empty() && out.port != 0 && !out.secret.empty();
}

bool sync_http(QNetworkRequest req, const QByteArray& method, const QByteArray& body,
               int& status, QByteArray& response, std::string& err) {
    QNetworkAccessManager nam;
    QNetworkReply* reply = nam.sendCustomRequest(req, method, body);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start(10000);
    loop.exec();

    if (!reply->isFinished()) {
        reply->abort();
        reply->deleteLater();
        err = "Cloudflare signaling timed out";
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

QNetworkRequest json_request(const std::string& path) {
    QNetworkRequest req(QUrl(QString::fromStdString(SignalingClient::base_url() + path)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("User-Agent", "FolderBuddies/1 zero-knowledge-cpp23");
    return req;
}

std::string clean_one_line(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        if (!std::isspace(c)) out.push_back(static_cast<char>(c));
    return out;
}

} // namespace

std::string random_room_code() {
    std::vector<uint8_t> rb = random_bytes(kRoomCodeLength);
    std::string code;
    code.reserve(kRoomCodeLength);
    for (uint8_t b : rb) code.push_back(kBase91Alphabet[b % kBase91Base]);
    return code;
}

std::string random_room_password() {
    // 32 random bytes ≈ 256 bits before Base91 encoding. The password is not
    // shortened for display; the whole generated string is required.
    return base91_encode(random_bytes(32));
}

bool looks_like_room_code(const std::string& text) {
    std::string s = clean_one_line(text);
    return s.size() == kRoomCodeLength && base91_is_clean(s);
}

std::string encrypt_room_payload(const Token& tok, const std::string& password,
                                 const std::string& aadRoomCode, std::string& err) {
    if (password.empty()) { err = "password is empty"; return {}; }
    std::vector<uint8_t> plain = serialize_token_payload(tok);
    std::vector<uint8_t> salt = random_bytes(16);
    std::vector<uint8_t> nonceVec = random_bytes(12);
    std::array<uint8_t, 12> nonce{};
    std::memcpy(nonce.data(), nonceVec.data(), nonce.size());
    Key256 key = payload_key(password, salt);
    QByteArray aad = payload_aad(aadRoomCode);

    std::vector<uint8_t> ct(plain.size());
    uint8_t tag[16];
    aead_encrypt(key, nonce, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(aad.constData()),
                                                       static_cast<size_t>(aad.size())),
                 plain, ct.data(), tag);

    std::vector<uint8_t> blob;
    blob.reserve(sizeof(kPayloadMagic) + 16 + 12 + ct.size() + 16);
    blob.insert(blob.end(), kPayloadMagic, kPayloadMagic + sizeof(kPayloadMagic));
    blob.insert(blob.end(), salt.begin(), salt.end());
    blob.insert(blob.end(), nonceVec.begin(), nonceVec.end());
    blob.insert(blob.end(), ct.begin(), ct.end());
    blob.insert(blob.end(), tag, tag + 16);
    return base91_encode(blob);
}

bool decrypt_room_payload(const std::string& encryptedBase91, const std::string& password,
                          const std::string& aadRoomCode, Token& out, std::string& err) {
    if (password.empty()) { err = "password is required"; return false; }
    bool ok = false;
    std::vector<uint8_t> blob = base91_decode(encryptedBase91, &ok);
    if (!ok || blob.size() < sizeof(kPayloadMagic) + 16 + 12 + 16) {
        err = "encrypted room payload is not valid Base91";
        return false;
    }
    size_t off = 0;
    if (std::memcmp(blob.data(), kPayloadMagic, sizeof(kPayloadMagic)) != 0) {
        err = "encrypted room payload has the wrong format";
        return false;
    }
    off += sizeof(kPayloadMagic);
    std::vector<uint8_t> salt(blob.begin() + off, blob.begin() + off + 16); off += 16;
    std::array<uint8_t, 12> nonce{};
    std::memcpy(nonce.data(), blob.data() + off, nonce.size()); off += nonce.size();
    size_t ctLen = blob.size() - off - 16;
    std::vector<uint8_t> ct(blob.begin() + off, blob.begin() + off + ctLen); off += ctLen;
    uint8_t tag[16];
    std::memcpy(tag, blob.data() + off, 16);

    std::vector<uint8_t> plain(ct.size());
    Key256 key = payload_key(password, salt);
    QByteArray aad = payload_aad(aadRoomCode);
    if (!aead_decrypt(key, nonce,
                      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(aad.constData()),
                                               static_cast<size_t>(aad.size())),
                      ct, tag, plain.data())) {
        err = "wrong password or tampered room payload";
        return false;
    }
    if (!deserialize_token_payload(plain, out)) {
        err = "decrypted room payload is malformed";
        return false;
    }
    return true;
}

std::string worker_auth_verifier(const std::string& password) {
    QByteArray root = password_root(password);
    QByteArray verifier = hmac_sha256(root, "FB-worker-auth-verifier-v1");
    return base91_encode(vec(verifier));
}

std::string worker_auth_proof(const std::string& password, const std::string& method,
                              const std::string& roomCode) {
    QByteArray verifier = qbytes(base91_decode(worker_auth_verifier(password)));
    QByteArray msg = QByteArray(method.data(), static_cast<int>(method.size())) + "\n" +
                     QByteArray(roomCode.data(), static_cast<int>(roomCode.size())) +
                     "\nFB-worker-auth-proof-v1";
    QByteArray proof = hmac_sha256(verifier, msg);
    return base91_encode(vec(proof));
}

std::string SignalingClient::base_url() {
    std::string url;
    if (const char* env = std::getenv("FOLDERBUDDIES_SIGNALING_URL"); env && *env)
        url = std::string(env);
    else
        url = std::string(kHardcodedWorkerUrl);
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

bool SignalingClient::configured() {
    std::string u = base_url();
    return !u.empty() && u.rfind("https://", 0) == 0;
}

bool SignalingClient::create_room(const std::string& roomCode, const std::string& password,
                                  const std::string& encryptedPayload, std::string& err) {
    if (!configured()) { err = "Cloudflare signaling URL is not configured"; return false; }
    QJsonObject obj;
    obj["room"] = QString::fromStdString(roomCode);
    obj["auth"] = QString::fromStdString(worker_auth_verifier(password));
    obj["payload"] = QString::fromStdString(encryptedPayload);
    obj["ttl"] = kRoomTtlSeconds;
    QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);

    int status = 0;
    QByteArray resp;
    if (!sync_http(json_request("/create"), "POST", body, status, resp, err)) return false;
    if (status == 201 || status == 200) return true;
    if (status == 409) { err = "room code collision"; return false; }
    err = "Cloudflare create failed (HTTP " + std::to_string(status) + "): " + resp.toStdString();
    return false;
}

bool SignalingClient::get_room(const std::string& roomCode, const std::string& password,
                               std::string& encryptedPayload, std::string& err) {
    if (!configured()) { err = "Cloudflare signaling URL is not configured"; return false; }
    QUrl url(QString::fromStdString(base_url() + "/room"));
    QUrlQuery q;
    q.addQueryItem("code", QString::fromStdString(roomCode));
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("X-FB-Auth", QByteArray::fromStdString(worker_auth_proof(password, "GET", roomCode)));

    int status = 0;
    QByteArray resp;
    if (!sync_http(req, "GET", {}, status, resp, err)) return false;
    if (status != 200) {
        err = status == 429 ? "rate limited by signaling server; wait a minute and try again"
                            : "Cloudflare room lookup failed (HTTP " + std::to_string(status) + ")";
        return false;
    }
    QJsonParseError pe{};
    QJsonDocument doc = QJsonDocument::fromJson(resp, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        err = "Cloudflare returned invalid JSON";
        return false;
    }
    encryptedPayload = doc.object().value("payload").toString().toStdString();
    if (encryptedPayload.empty()) { err = "Cloudflare returned an empty payload"; return false; }
    return true;
}

bool SignalingClient::delete_room(const std::string& roomCode, const std::string& password,
                                  std::string& err) {
    if (!configured()) { err = "Cloudflare signaling URL is not configured"; return false; }
    QUrl url(QString::fromStdString(base_url() + "/room"));
    QUrlQuery q;
    q.addQueryItem("code", QString::fromStdString(roomCode));
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("X-FB-Auth", QByteArray::fromStdString(worker_auth_proof(password, "DELETE", roomCode)));
    int status = 0;
    QByteArray resp;
    if (!sync_http(req, "DELETE", {}, status, resp, err)) return false;
    if (status == 200 || status == 204 || status == 404) return true;
    err = status == 429 ? "rate limited by signaling server; wait a minute and try again"
                        : "Cloudflare room delete failed (HTTP " + std::to_string(status) + "): " + resp.toStdString();
    return false;
}

} // namespace fb
