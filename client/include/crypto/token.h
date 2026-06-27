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
    bool allowWrites = false;        // whether clients may modify the shared folder
};

// Data-path session secret length (256-bit bearer secret).
constexpr size_t kSecretBytes = 32;

// n random bytes from the OS CSPRNG.
std::vector<uint8_t> random_bytes(size_t n);

} // namespace fb
