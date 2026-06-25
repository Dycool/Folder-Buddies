// Folder Buddies — connection metadata carried inside the encrypted room payload.
//
// A Token is the plaintext the host shares with a client: the address/port to
// connect to, the auto-generated data-path secret, and the folder display name.
// It is never serialized as human-readable text; signaling.cpp serializes it to
// a binary blob, seals it (ChaCha20-Poly1305), and Base91-encodes the result as
// either the Cloudflare room payload or the offline fallback blob.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fb {

struct Token {
    std::string ip;                  // literal IPv4 or IPv6 to connect to
    uint16_t port = 0;
    std::vector<uint8_t> secret;     // auto-generated password / bearer credential
    std::string folder;              // human-readable folder name (shown verbatim)
};

// Data-path session secret length (256-bit bearer secret).
constexpr size_t kSecretBytes = 32;

// n random bytes from the OS CSPRNG.
std::vector<uint8_t> random_bytes(size_t n);

} // namespace fb
