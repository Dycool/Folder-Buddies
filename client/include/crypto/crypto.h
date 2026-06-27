#pragma once

#include "common.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace fb {

using Key256 = std::array<uint8_t, 32>;

void aead_encrypt(const Key256& key, std::span<const uint8_t, 12> nonce,
                  std::span<const uint8_t> aad, std::span<const uint8_t> pt,
                  uint8_t* ct, uint8_t tag[16]);

// Returns false (without writing usable plaintext) if the tag does not verify.
[[nodiscard]] bool aead_decrypt(const Key256& key, std::span<const uint8_t, 12> nonce,
                                std::span<const uint8_t> aad, std::span<const uint8_t> ct,
                                const uint8_t tag[16], uint8_t* pt);

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

    bool send(socket_t s, uint16_t op, int16_t status, uint64_t req_id,
              const uint8_t* payload, uint32_t len);
    bool recv(socket_t s, MsgHeader& h, std::vector<uint8_t>& payload);

private:
    Key256 txKey_{}, rxKey_{};
    uint64_t txCtr_ = 0, rxCtr_ = 0;
    bool active_ = false;
};

} // namespace fb
