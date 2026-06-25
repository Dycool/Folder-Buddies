#include "signaling.h"

#include "base91.h"
#include "common.h"
#include "crypto.h"

#include <QByteArray>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QDateTime>
#include <QUrl>
#include <QUrlQuery>

#include <sodium.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <cctype>

namespace fb {
namespace {

#ifndef FB_SIGNALING_URL
constexpr const char* kHardcodedWorkerUrl = "";
#else
constexpr const char* kHardcodedWorkerUrl = FB_SIGNALING_URL;
#endif

#ifndef FB_FIREBASE_DATABASE_URL
constexpr const char* kHardcodedFirebaseUrl = "";
#else
constexpr const char* kHardcodedFirebaseUrl = FB_FIREBASE_DATABASE_URL;
#endif

constexpr char kPayloadMagic[5] = {'F', 'B', 'Z', 'K', '1'};
constexpr char kOfflineMagic[5] = {'F', 'B', 'O', 'F', '1'};
constexpr uint32_t kPayloadVersion = 2;

// Argon2id parameters. Must match the webapp (@noble/hashes) exactly so both
// sides derive the same wrap key from the secret half of the code.
constexpr unsigned long long kArgonOps = 3;            // iterations (t)
constexpr size_t kArgonMem = 64ull * 1024 * 1024;      // 64 MiB (m)
constexpr size_t kArgonSaltLen = 16;

std::vector<uint8_t> vecOf(const QByteArray& b) {
    return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(b.constData()),
                                reinterpret_cast<const uint8_t*>(b.constData()) + b.size());
}

Key256 to_key(const std::vector<uint8_t>& v) {
    Key256 k{};
    std::memcpy(k.data(), v.data(), 32);
    return k;
}

std::vector<uint8_t> serialize_token_payload(const Token& tok) {
    Writer w;
    w.raw(kPayloadMagic, sizeof(kPayloadMagic));
    w.pod(kPayloadVersion);
    w.str(tok.ip);
    w.pod(tok.port);
    w.str(tok.folder);
    uint8_t writes = tok.allowWrites ? 1 : 0;
    w.pod(writes);
    w.bytes(tok.secret.data(), static_cast<uint32_t>(tok.secret.size()));
    return w.b;
}

bool deserialize_token_payload(const std::vector<uint8_t>& plain, Token& out) {
    Reader r(plain.data(), plain.size());
    char magic[sizeof(kPayloadMagic)]{};
    uint32_t version = 0;
    if (!r.raw(magic, sizeof(magic)) || std::memcmp(magic, kPayloadMagic, sizeof(magic)) != 0)
        return false;
    if (!r.pod(version) || (version != 1 && version != kPayloadVersion)) return false;
    if (!r.str(out.ip)) return false;
    if (!r.pod(out.port)) return false;
    if (!r.str(out.folder)) return false;
    if (version >= 2) {
        uint8_t writes = 0;
        if (!r.pod(writes)) return false;
        out.allowWrites = writes != 0;
    } else {
        // v1 tokens predate the explicit permission bit and were historically read/write.
        out.allowWrites = true;
    }
    if (!r.bytes(out.secret)) return false;
    return !out.ip.empty() && out.port != 0 && !out.secret.empty();
}

// Derive a 32-byte key from the secret half of a code via Argon2id.
bool argon2id_key(const std::string& keyPart, const std::vector<uint8_t>& salt, Key256& out,
                  std::string& err) {
    if (sodium_init() < 0) { err = "libsodium init failed"; return false; }
    if (salt.size() != kArgonSaltLen) { err = "bad salt length"; return false; }
    if (crypto_pwhash(out.data(), out.size(),
                      keyPart.data(), keyPart.size(), salt.data(),
                      kArgonOps, kArgonMem, crypto_pwhash_ALG_ARGON2ID13) != 0) {
        err = "Argon2id derivation failed (out of memory?)";
        return false;
    }
    return true;
}

// Seal a Token under a fresh random 256-bit key. Returns the key and the
// ciphertext bundle (MAGIC || nonce || ct || tag).
void seal_token(const Token& tok, std::vector<uint8_t>& blobKey, std::vector<uint8_t>& bundle) {
    blobKey = random_bytes(32);
    std::vector<uint8_t> plain = serialize_token_payload(tok);
    std::vector<uint8_t> nonceVec = random_bytes(12);
    std::array<uint8_t, 12> nonce{};
    std::memcpy(nonce.data(), nonceVec.data(), nonce.size());
    std::vector<uint8_t> ct(plain.size());
    uint8_t tag[16];
    aead_encrypt(to_key(blobKey), nonce, {}, plain, ct.data(), tag);

    bundle.clear();
    bundle.insert(bundle.end(), kPayloadMagic, kPayloadMagic + sizeof(kPayloadMagic));
    bundle.insert(bundle.end(), nonceVec.begin(), nonceVec.end());
    bundle.insert(bundle.end(), ct.begin(), ct.end());
    bundle.insert(bundle.end(), tag, tag + 16);
}

bool open_bundle(const std::vector<uint8_t>& blobKey, const std::vector<uint8_t>& bundle,
                 Token& out, std::string& err) {
    if (blobKey.size() != 32) { err = "bad key"; return false; }
    size_t header = sizeof(kPayloadMagic) + 12 + 16;
    if (bundle.size() < header ||
        std::memcmp(bundle.data(), kPayloadMagic, sizeof(kPayloadMagic)) != 0) {
        err = "malformed payload bundle";
        return false;
    }
    size_t off = sizeof(kPayloadMagic);
    std::array<uint8_t, 12> nonce{};
    std::memcpy(nonce.data(), bundle.data() + off, 12); off += 12;
    size_t ctLen = bundle.size() - off - 16;
    std::vector<uint8_t> ct(bundle.begin() + off, bundle.begin() + off + ctLen); off += ctLen;
    uint8_t tag[16];
    std::memcpy(tag, bundle.data() + off, 16);

    std::vector<uint8_t> plain(ct.size());
    if (!aead_decrypt(to_key(blobKey), nonce, {}, ct, tag, plain.data())) {
        err = "wrong code or tampered payload";
        return false;
    }
    if (!deserialize_token_payload(plain, out)) { err = "decrypted payload is malformed"; return false; }
    return true;
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
    req.setRawHeader("User-Agent", "FolderBuddies/1");
    return req;
}

std::string firebase_safe_key(const std::string& lookupId) {
    return QByteArray::fromStdString(lookupId)
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)
        .toStdString();
}

std::string firebase_room_path(const std::string& lookupId) {
    return "/nativeRooms/" + firebase_safe_key(lookupId) + ".json";
}

QNetworkRequest firebase_request(const std::string& lookupId) {
    QNetworkRequest req(QUrl(QString::fromStdString(FirebaseSignalingClient::base_url() + firebase_room_path(lookupId))));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("User-Agent", "FolderBuddies/1");
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

bool looks_like_room_code(const std::string& text) {
    std::string s = clean_one_line(text);
    return s.size() == kRoomCodeLength && base91_is_clean(s);
}

bool seal_for_offline(const Token& tok, std::string& offlineBlob, std::string& err) {
    std::vector<uint8_t> blobKey, bundle;
    seal_token(tok, blobKey, bundle);
    std::vector<uint8_t> blob;
    blob.insert(blob.end(), kOfflineMagic, kOfflineMagic + sizeof(kOfflineMagic));
    blob.insert(blob.end(), blobKey.begin(), blobKey.end());
    blob.insert(blob.end(), bundle.begin(), bundle.end());
    offlineBlob = base91_encode(blob);
    (void)err;
    return true;
}

bool open_offline_blob(const std::string& blob, Token& out, std::string& err) {
    bool ok = false;
    std::vector<uint8_t> raw = base91_decode(blob, &ok);
    size_t header = sizeof(kOfflineMagic) + 32;
    if (!ok || raw.size() < header || std::memcmp(raw.data(), kOfflineMagic, sizeof(kOfflineMagic)) != 0) {
        err = "not a valid offline blob";
        return false;
    }
    std::vector<uint8_t> blobKey(raw.begin() + sizeof(kOfflineMagic),
                                 raw.begin() + sizeof(kOfflineMagic) + 32);
    std::vector<uint8_t> bundle(raw.begin() + header, raw.end());
    return open_bundle(blobKey, bundle, out, err);
}

bool seal_for_cloud(const Token& tok, const std::string& roomCode, CloudRecord& rec,
                    std::string& ownerOut, std::string& err) {
    if (roomCode.size() != kRoomCodeLength) { err = "bad room code"; return false; }
    std::string keyPart = roomCode.substr(kLookupLen, kKeyPartLen);

    std::vector<uint8_t> blobKey, bundle;
    seal_token(tok, blobKey, bundle);

    std::vector<uint8_t> salt = random_bytes(kArgonSaltLen);
    Key256 wrapKey;
    if (!argon2id_key(keyPart, salt, wrapKey, err)) return false;

    std::vector<uint8_t> wnonceVec = random_bytes(12);
    std::array<uint8_t, 12> wnonce{};
    std::memcpy(wnonce.data(), wnonceVec.data(), wnonce.size());
    std::vector<uint8_t> wct(blobKey.size());
    uint8_t wtag[16];
    aead_encrypt(wrapKey, wnonce, {}, blobKey, wct.data(), wtag);

    std::vector<uint8_t> wrapped;
    wrapped.insert(wrapped.end(), wnonceVec.begin(), wnonceVec.end());
    wrapped.insert(wrapped.end(), wct.begin(), wct.end());
    wrapped.insert(wrapped.end(), wtag, wtag + 16);

    ownerOut = base91_encode(random_bytes(16));
    rec.lookupId = roomCode.substr(0, kLookupLen);
    rec.salt = base91_encode(salt);
    rec.wrapped = base91_encode(wrapped);
    rec.payload = base91_encode(bundle);
    rec.owner = ownerOut;
    return true;
}

bool open_cloud_record(const std::string& roomCode, const std::string& saltB91,
                       const std::string& wrappedB91, const std::string& payloadB91,
                       Token& out, std::string& err) {
    if (roomCode.size() != kRoomCodeLength) { err = "bad room code"; return false; }
    std::string keyPart = roomCode.substr(kLookupLen, kKeyPartLen);

    bool ok = false;
    std::vector<uint8_t> salt = base91_decode(saltB91, &ok);
    if (!ok || salt.size() != kArgonSaltLen) { err = "bad record salt"; return false; }
    std::vector<uint8_t> wrapped = base91_decode(wrappedB91, &ok);
    if (!ok || wrapped.size() != 12 + 32 + 16) { err = "bad wrapped key"; return false; }
    std::vector<uint8_t> bundle = base91_decode(payloadB91, &ok);
    if (!ok) { err = "bad record payload"; return false; }

    Key256 wrapKey;
    if (!argon2id_key(keyPart, salt, wrapKey, err)) return false;

    std::array<uint8_t, 12> wnonce{};
    std::memcpy(wnonce.data(), wrapped.data(), 12);
    std::vector<uint8_t> wct(wrapped.begin() + 12, wrapped.begin() + 12 + 32);
    uint8_t wtag[16];
    std::memcpy(wtag, wrapped.data() + 12 + 32, 16);

    std::vector<uint8_t> blobKey(32);
    if (!aead_decrypt(wrapKey, wnonce, {}, wct, wtag, blobKey.data())) {
        err = "wrong code";
        return false;
    }
    return open_bundle(blobKey, bundle, out, err);
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

bool SignalingClient::create(const CloudRecord& rec, std::string& err) {
    if (!configured()) { err = "Cloudflare signaling URL is not configured"; return false; }
    QJsonObject obj;
    obj["lookup"] = QString::fromStdString(rec.lookupId);
    obj["salt"] = QString::fromStdString(rec.salt);
    obj["wrapped"] = QString::fromStdString(rec.wrapped);
    obj["payload"] = QString::fromStdString(rec.payload);
    obj["owner"] = QString::fromStdString(rec.owner);
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

bool SignalingClient::get(const std::string& lookupId, std::string& salt, std::string& wrapped,
                          std::string& payload, std::string& err) {
    if (!configured()) { err = "Cloudflare signaling URL is not configured"; return false; }
    QUrl url(QString::fromStdString(base_url() + "/room"));
    QUrlQuery q;
    q.addQueryItem("code", QString::fromStdString(lookupId));
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setRawHeader("Accept", "application/json");

    int status = 0;
    QByteArray resp;
    if (!sync_http(req, "GET", {}, status, resp, err)) return false;
    if (status != 200) {
        err = status == 429 ? "rate limited by signaling server; wait a minute and try again"
              : status == 404 ? "no share found for that code"
                              : "Cloudflare room lookup failed (HTTP " + std::to_string(status) + ")";
        return false;
    }
    QJsonParseError pe{};
    QJsonDocument doc = QJsonDocument::fromJson(resp, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        err = "Cloudflare returned invalid JSON";
        return false;
    }
    QJsonObject o = doc.object();
    salt = o.value("salt").toString().toStdString();
    wrapped = o.value("wrapped").toString().toStdString();
    payload = o.value("payload").toString().toStdString();
    if (salt.empty() || wrapped.empty() || payload.empty()) { err = "Cloudflare returned an incomplete record"; return false; }
    return true;
}

bool SignalingClient::remove(const std::string& lookupId, const std::string& owner, std::string& err) {
    if (!configured()) { err = "Cloudflare signaling URL is not configured"; return false; }
    QUrl url(QString::fromStdString(base_url() + "/room"));
    QUrlQuery q;
    q.addQueryItem("code", QString::fromStdString(lookupId));
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("X-FB-Owner", QByteArray::fromStdString(owner));
    int status = 0;
    QByteArray resp;
    if (!sync_http(req, "DELETE", {}, status, resp, err)) return false;
    if (status == 200 || status == 204 || status == 404) return true;
    err = "Cloudflare room delete failed (HTTP " + std::to_string(status) + ")";
    return false;
}

std::string FirebaseSignalingClient::base_url() {
    std::string url;
    if (const char* env = std::getenv("FOLDERBUDDIES_FIREBASE_DATABASE_URL"); env && *env)
        url = std::string(env);
    else
        url = std::string(kHardcodedFirebaseUrl);
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

bool FirebaseSignalingClient::configured() {
    std::string u = base_url();
    return !u.empty() && u.rfind("https://", 0) == 0 &&
           (u.find("firebasedatabase.app") != std::string::npos ||
            u.find("firebaseio.com") != std::string::npos);
}

bool FirebaseSignalingClient::create(const CloudRecord& rec, std::string& err) {
    if (!configured()) { err = "Firebase fallback URL is not configured"; return false; }

    int status = 0;
    QByteArray resp;
    QNetworkRequest req = firebase_request(rec.lookupId);

    // Realtime Database REST has no anonymous compare-and-set. Check first, then PUT.
    // A race is still possible, but room collisions are retried and rare enough for fallback use.
    if (!sync_http(req, "GET", {}, status, resp, err)) return false;
    if (status != 200) {
        err = "Firebase fallback room check failed (HTTP " + std::to_string(status) + ")";
        return false;
    }
    if (QString::fromUtf8(resp).trimmed() != "null") {
        err = "Firebase fallback room code collision";
        return false;
    }

    QJsonObject obj;
    obj["v"] = 1;
    obj["lookup"] = QString::fromStdString(rec.lookupId);
    obj["salt"] = QString::fromStdString(rec.salt);
    obj["wrapped"] = QString::fromStdString(rec.wrapped);
    obj["payload"] = QString::fromStdString(rec.payload);
    obj["owner"] = QString::fromStdString(rec.owner);
    obj["createdAt"] = static_cast<qint64>(QDateTime::currentSecsSinceEpoch());
    QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);

    if (!sync_http(req, "PUT", body, status, resp, err)) return false;
    if (status == 200) return true;
    err = "Firebase fallback create failed (HTTP " + std::to_string(status) + "): " + resp.toStdString();
    return false;
}

bool FirebaseSignalingClient::get(const std::string& lookupId, std::string& salt, std::string& wrapped,
                                  std::string& payload, std::string& err) {
    if (!configured()) { err = "Firebase fallback URL is not configured"; return false; }
    int status = 0;
    QByteArray resp;
    if (!sync_http(firebase_request(lookupId), "GET", {}, status, resp, err)) return false;
    if (status != 200) {
        err = "Firebase fallback lookup failed (HTTP " + std::to_string(status) + ")";
        return false;
    }
    if (QString::fromUtf8(resp).trimmed() == "null") {
        err = "no Firebase fallback share found for that code";
        return false;
    }
    QJsonParseError pe{};
    QJsonDocument doc = QJsonDocument::fromJson(resp, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        err = "Firebase fallback returned invalid JSON";
        return false;
    }
    QJsonObject o = doc.object();
    salt = o.value("salt").toString().toStdString();
    wrapped = o.value("wrapped").toString().toStdString();
    payload = o.value("payload").toString().toStdString();
    if (salt.empty() || wrapped.empty() || payload.empty()) { err = "Firebase fallback returned an incomplete record"; return false; }
    return true;
}

bool FirebaseSignalingClient::remove(const std::string& lookupId, const std::string& owner, std::string& err) {
    (void)owner;
    if (!configured()) { err = "Firebase fallback URL is not configured"; return false; }
    int status = 0;
    QByteArray resp;
    if (!sync_http(firebase_request(lookupId), "DELETE", {}, status, resp, err)) return false;
    if (status == 200 || status == 204) return true;
    err = "Firebase fallback delete failed (HTTP " + std::to_string(status) + ")";
    return false;
}

} // namespace fb

#ifdef FB_SIGNALING_SELFTEST
#  include <cstdio>
int main() {
    using namespace fb;
    if (sodium_init() < 0) { std::printf("signaling self-test FAIL (sodium)\n"); return 1; }

    Token tok;
    tok.ip = "2001:db8::1";
    tok.port = 50321;
    tok.folder = "cool folder";
    tok.secret = random_bytes(kSecretBytes);
    tok.allowWrites = true;

    auto same = [&](const Token& a, const Token& b) {
        return a.ip == b.ip && a.port == b.port && a.folder == b.folder &&
               a.secret == b.secret && a.allowWrites == b.allowWrites;
    };

    std::string err;
    bool ok = true;

    // Offline blob round-trip.
    std::string blob;
    Token back;
    ok = ok && seal_for_offline(tok, blob, err) && open_offline_blob(blob, back, err) && same(tok, back);

    // Cloudflare record round-trip.
    std::string code = random_room_code();
    CloudRecord rec;
    std::string owner;
    Token back2;
    ok = ok && seal_for_cloud(tok, code, rec, owner, err) &&
         open_cloud_record(code, rec.salt, rec.wrapped, rec.payload, back2, err) && same(tok, back2);

    // Wrong secret half must fail.
    std::string wrong = code;
    wrong[kLookupLen] = (wrong[kLookupLen] == 'A') ? 'B' : 'A';
    Token bad;
    bool wrongFails = !open_cloud_record(wrong, rec.salt, rec.wrapped, rec.payload, bad, err);
    ok = ok && wrongFails;

    std::printf("%s\n", ok ? "signaling self-test PASS" : "signaling self-test FAIL");
    return ok ? 0 : 1;
}
#endif
