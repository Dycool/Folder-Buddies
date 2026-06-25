#include "token.h"

#include "common.h" // socket headers for inet_pton / inet_ntop / in_addr
#include "wordlist.h"

#include <QRandomGenerator>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace fb {
namespace {

// byte <-> word, using the 256-entry list (index == byte value).
const std::unordered_map<std::string, int>& word_index() {
    static const std::unordered_map<std::string, int> m = [] {
        std::unordered_map<std::string, int> r;
        for (int i = 0; i < 256; ++i) r.emplace(kWordList[i], i);
        return r;
    }();
    return m;
}

std::string join(const std::vector<std::string>& parts, char sep) {
    std::string s;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) s += sep;
        s += parts[i];
    }
    return s;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

// Split on the literal "--" field separator, at most `maxParts` fields; the
// final field keeps any further "--" so folder names survive verbatim.
std::vector<std::string> split_fields(const std::string& s, size_t maxParts) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (out.size() + 1 < maxParts) {
        size_t d = s.find("--", pos);
        if (d == std::string::npos) break;
        out.push_back(s.substr(pos, d - pos));
        pos = d + 2;
    }
    out.push_back(s.substr(pos));
    return out;
}

std::string ip_to_groups(const std::string& ip) {
    if (ip.find(':') != std::string::npos) {
        in6_addr a{};
        if (inet_pton(AF_INET6, ip.c_str(), &a) != 1) return {};
        std::vector<std::string> g;
        for (int i = 0; i < 16; i += 2) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%02x%02x", a.s6_addr[i], a.s6_addr[i + 1]);
            g.emplace_back(buf);
        }
        return join(g, '-'); // 8 hextets
    }
    in_addr a{};
    if (inet_pton(AF_INET, ip.c_str(), &a) != 1) return {};
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&a.s_addr);
    std::vector<std::string> g;
    for (int i = 0; i < 4; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%03u", b[i]);
        g.emplace_back(buf);
    }
    return join(g, '-'); // 4 decimal octets
}

bool groups_to_ip(const std::string& groups, std::string& ip) {
    std::vector<std::string> g = split(groups, '-');
    if (g.size() == 4) { // IPv4
        char buf[16];
        unsigned o[4];
        for (int i = 0; i < 4; ++i) {
            if (g[i].empty() || std::sscanf(g[i].c_str(), "%u", &o[i]) != 1 || o[i] > 255)
                return false;
        }
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", o[0], o[1], o[2], o[3]);
        ip = buf;
        return true;
    }
    if (g.size() == 8) { // IPv6
        in6_addr a{};
        for (int i = 0; i < 8; ++i) {
            unsigned h;
            if (g[i].empty() || std::sscanf(g[i].c_str(), "%x", &h) != 1 || h > 0xffff)
                return false;
            a.s6_addr[2 * i] = static_cast<uint8_t>(h >> 8);
            a.s6_addr[2 * i + 1] = static_cast<uint8_t>(h & 0xff);
        }
        char buf[64];
        if (!inet_ntop(AF_INET6, &a, buf, sizeof(buf))) return false;
        ip = buf;
        return true;
    }
    return false;
}

} // namespace

std::string Token::encode() const {
    std::vector<std::string> words;
    words.reserve(secret.size());
    for (uint8_t b : secret) words.emplace_back(kWordList[b]);

    return join(words, '-') + "--" + ip_to_groups(ip) + "--" + std::to_string(port) + "--" +
           folder;
}

bool Token::decode(const std::string& s, Token& out) {
    std::vector<std::string> f = split_fields(s, 4);
    if (f.size() != 4) return false;

    // words -> secret
    const auto& idx = word_index();
    out.secret.clear();
    for (std::string w : split(f[0], '-')) {
        for (char& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto it = idx.find(w);
        if (it == idx.end()) return false;
        out.secret.push_back(static_cast<uint8_t>(it->second));
    }
    if (out.secret.empty()) return false;

    if (!groups_to_ip(f[1], out.ip)) return false;

    unsigned port;
    if (std::sscanf(f[2].c_str(), "%u", &port) != 1 || port > 65535) return false;
    out.port = static_cast<uint16_t>(port);

    out.folder = f[3];
    return true;
}

std::vector<uint8_t> random_bytes(size_t n) {
    std::vector<uint8_t> v((n + 3) & ~size_t(3));
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(v.data()),
                                          static_cast<qsizetype>(v.size() / 4));
    v.resize(n);
    return v;
}

} // namespace fb
