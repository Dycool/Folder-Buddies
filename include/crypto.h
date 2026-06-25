// Folder Buddies — wire encryption (ChaCha20-Poly1305 AEAD, RFC 8439) and the
// secure channel that wraps every post-handshake message. Per connection there
// are two directional 256-bit keys and a strictly increasing 96-bit counter
// nonce.
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
// handshake both peers call `activate` with their directional keys; tx and rx
// have independent counters.
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

    // Sealed mirror of common.h send_message/recv_message. Before activation
    // these fall through to the plaintext path used during the handshake.
    bool send(socket_t s, uint16_t op, int16_t status, uint64_t req_id,
              const uint8_t* payload, uint32_t len);
    bool recv(socket_t s, MsgHeader& h, std::vector<uint8_t>& payload);

private:
    Key256 txKey_{}, rxKey_{};
    uint64_t txCtr_ = 0, rxCtr_ = 0;
    bool active_ = false;
};

} // namespace fb
