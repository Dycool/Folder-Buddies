#include "base91.h"

#include <array>
#include <cctype>

namespace fb {
namespace {

const std::array<int8_t, 256>& decode_table() {
    static const std::array<int8_t, 256> t = [] {
        std::array<int8_t, 256> r{};
        r.fill(-1);
        for (int i = 0; i < static_cast<int>(kBase91Base); ++i)
            r[static_cast<unsigned char>(kBase91Alphabet[i])] = static_cast<int8_t>(i);
        return r;
    }();
    return t;
}

} // namespace

std::string base91_encode(const std::vector<uint8_t>& data) {
    std::string out;
    out.reserve((data.size() * 123) / 100 + 8);

    unsigned int b = 0;
    unsigned int n = 0;
    for (uint8_t byte : data) {
        b |= static_cast<unsigned int>(byte) << n;
        n += 8;
        if (n > 13) {
            unsigned int v = b & 8191u;
            if (v > 88) {
                b >>= 13;
                n -= 13;
            } else {
                v = b & 16383u;
                b >>= 14;
                n -= 14;
            }
            out.push_back(kBase91Alphabet[v % 91]);
            out.push_back(kBase91Alphabet[v / 91]);
        }
    }
    if (n) {
        out.push_back(kBase91Alphabet[b % 91]);
        if (n > 7 || b > 90) out.push_back(kBase91Alphabet[b / 91]);
    }
    return out;
}

std::vector<uint8_t> base91_decode(const std::string& text, bool* ok) {
    if (ok) *ok = false;
    const auto& table = decode_table();
    std::vector<uint8_t> out;
    out.reserve((text.size() * 100) / 123 + 8);

    int v = -1;
    unsigned int b = 0;
    unsigned int n = 0;
    for (unsigned char ch : text) {
        if (std::isspace(ch)) continue; // tolerate accidental line wrapping
        int8_t c = table[ch];
        if (c < 0) return {};
        if (v < 0) {
            v = c;
        } else {
            v += c * 91;
            b |= static_cast<unsigned int>(v) << n;
            n += (v & 8191) > 88 ? 13 : 14;
            do {
                out.push_back(static_cast<uint8_t>(b & 0xff));
                b >>= 8;
                n -= 8;
            } while (n > 7);
            v = -1;
        }
    }
    if (v >= 0) out.push_back(static_cast<uint8_t>((b | (static_cast<unsigned int>(v) << n)) & 0xff));
    if (ok) *ok = true;
    return out;
}

bool base91_is_clean(const std::string& text) {
    const auto& table = decode_table();
    for (unsigned char ch : text) {
        if (std::isspace(ch)) continue;
        if (table[ch] < 0) return false;
    }
    return true;
}

} // namespace fb
