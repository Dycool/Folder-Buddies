// Folder Buddies — authentication helpers shared by client and server.
//
// The code carries a computer-generated `secret` (the password — there is no
// user-chosen one). Proof of knowledge is an HMAC over both handshake nonces,
// and the *same* secret derives the per-connection encryption keys, so a passive
// eavesdropper without the code can neither connect nor read the data.
#pragma once

#include "crypto.h" // Key256

#include <QByteArray>
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fb {

inline QByteArray derive_key(const std::vector<uint8_t>& secret) {
    QByteArray material(reinterpret_cast<const char*>(secret.data()),
                        static_cast<int>(secret.size()));
    return QCryptographicHash::hash(material, QCryptographicHash::Sha256);
}

inline QByteArray hmac_sha256(const QByteArray& key, const QByteArray& data) {
    QMessageAuthenticationCode mac(QCryptographicHash::Sha256);
    mac.setKey(key);
    mac.addData(data);
    return mac.result();
}

inline QByteArray auth_proof(const QByteArray& key, const QByteArray& nonceClient,
                             const QByteArray& nonceServer) {
    return hmac_sha256(key, nonceClient + nonceServer);
}

inline Key256 to_key256(const QByteArray& b) {
    Key256 k{};
    std::memcpy(k.data(), b.constData(), 32); // SHA-256 output is exactly 32 bytes
    return k;
}

// Derive the two directional ChaCha20 keys for one connection from the
// authenticated handshake. Both peers feed identical material; only the
// tx/rx assignment differs, so server.tx == client.rx and vice versa.
inline void derive_session_keys(const QByteArray& authKey, const QByteArray& nonceClient,
                                const QByteArray& nonceServer, bool isServer, Key256& txKey,
                                Key256& rxKey) {
    QByteArray master = hmac_sha256(authKey, nonceClient + nonceServer + "FB-session-v2");
    Key256 kc2s = to_key256(hmac_sha256(master, "client->server"));
    Key256 ks2c = to_key256(hmac_sha256(master, "server->client"));
    if (isServer) { txKey = ks2c; rxKey = kc2s; }
    else { txKey = kc2s; rxKey = ks2c; }
}

} // namespace fb
