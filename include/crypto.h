// Folder Buddies — wire encryption (ChaCha20-Poly1305 AEAD) and the secure
// channel that wraps every post-handshake message.
//
// Why hand-rolled crypto instead of OpenSSL: the whole project is a single,
// dependency-light binary, and ChaCha20-Poly1305 is fast *in software* (no
// AES-NI required) so it adds the least possible overhead on the data path —
// exactly the goal of "secured with minimal overhead". The primitive is the
// IETF construction from RFC 8439 and is covered by a self-test built against
// the RFC's own vectors (see crypto.cpp / FB_CRYPTO_SELFTEST).
//
// Per connection we derive two independent 256-bit keys (one per direction)
// from the authenticated handshake, and use a strictly increasing 96-bit
// counter as the nonce. A unique key+counter per direction means a nonce is
// never reused, and any tampering (including a forged length prefix) fails the
// Poly1305 tag and drops the connection.
#pragma once

#include "common.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace fb {

using Key256 = std::array<uint8_t, 32>;

// AEAD primitive (RFC 8439, §2.8). `ct` must be `pt.size()` bytes; the 16-byte
// authentication tag is written to `tag`.
void aead_encrypt(const Key256& key, std::span<const uint8_t, 12> nonce,
                  std::span<const uint8_t> aad, std::span<const uint8_t> pt,
                  uint8_t* ct, uint8_t tag[16]);

// Returns false (without writing usable plaintext) if the tag does not verify.
[[nodiscard]] bool aead_decrypt(const Key256& key, std::span<const uint8_t, 12> nonce,
                                std::span<const uint8_t> aad, std::span<const uint8_t> ct,
                                const uint8_t tag[16], uint8_t* pt);

// A bidirectional encrypted message channel over one TCP connection. After the
// plaintext challenge/response handshake both peers call `activate` with the
// directional keys derived from the shared session secret; from then on every
// framed message is sealed. tx and rx have independent counters, so the writer
// thread and the reader thread can each touch "their" half without locking the
// other (the caller still serializes concurrent writers, as before).
class SecureChannel {
public:
    void activate(const Key256& txKey, const Key256& rxKey) {
        txKey_ = txKey;
        rxKey_ = rxKey;
        txCtr_ = 0;
        rxCtr_ = 0;
        active_ = true;
    }
    bool active() const { return active_; }

    // Mirror of common.h send_message/recv_message, but sealed. Before
    // activation they fall through to the plaintext path so the same call sites
    // work during the handshake.
    bool send(socket_t s, uint16_t op, int16_t status, uint64_t req_id,
              const uint8_t* payload, uint32_t len);
    bool recv(socket_t s, MsgHeader& h, std::vector<uint8_t>& payload);

private:
    Key256 txKey_{}, rxKey_{};
    uint64_t txCtr_ = 0, rxCtr_ = 0;
    bool active_ = false;
};

} // namespace fb
