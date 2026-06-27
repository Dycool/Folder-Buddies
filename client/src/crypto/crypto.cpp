#include "crypto.h"

#include <cstring>

namespace fb {
namespace {

// ---- ChaCha20 (RFC 8439 §2.3) --------------------------------------------

constexpr uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

inline uint32_t load_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline void store_le32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

inline void quarter(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    a += b; d ^= a; d = rotl32(d, 16);
    c += d; b ^= c; b = rotl32(b, 12);
    a += b; d ^= a; d = rotl32(d, 8);
    c += d; b ^= c; b = rotl32(b, 7);
}

void chacha20_block(const uint32_t key[8], uint32_t counter, const uint32_t nonce[3],
                    uint8_t out[64]) {
    uint32_t s[16] = {0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u,
                      key[0], key[1], key[2], key[3],
                      key[4], key[5], key[6], key[7],
                      counter, nonce[0], nonce[1], nonce[2]};
    uint32_t x[16];
    std::memcpy(x, s, sizeof x);
    for (int i = 0; i < 10; ++i) {
        quarter(x[0], x[4], x[8], x[12]);
        quarter(x[1], x[5], x[9], x[13]);
        quarter(x[2], x[6], x[10], x[14]);
        quarter(x[3], x[7], x[11], x[15]);
        quarter(x[0], x[5], x[10], x[15]);
        quarter(x[1], x[6], x[11], x[12]);
        quarter(x[2], x[7], x[8], x[13]);
        quarter(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; ++i) store_le32(out + 4 * i, x[i] + s[i]);
}

void chacha20_xor(const Key256& key, std::span<const uint8_t, 12> nonce, uint32_t counter,
                  const uint8_t* in, uint8_t* out, size_t len) {
    uint32_t k[8];
    for (int i = 0; i < 8; ++i) k[i] = load_le32(key.data() + 4 * i);
    uint32_t n[3] = {load_le32(nonce.data()), load_le32(nonce.data() + 4),
                     load_le32(nonce.data() + 8)};
    uint8_t block[64];
    size_t off = 0;
    while (len) {
        chacha20_block(k, counter++, n, block);
        size_t take = len < 64 ? len : 64;
        for (size_t i = 0; i < take; ++i) out[off + i] = in[off + i] ^ block[i];
        off += take;
        len -= take;
    }
}

// ---- Poly1305 (RFC 8439 §2.5), poly1305-donna 32-bit, fixed-width ---------

struct Poly1305 {
    uint32_t r[5], h[5], pad[4];
    size_t leftover = 0;
    uint8_t buffer[16];
    uint8_t final = 0;

    explicit Poly1305(const uint8_t key[32]) {
        r[0] = (load_le32(key + 0)) & 0x3ffffff;
        r[1] = (load_le32(key + 3) >> 2) & 0x3ffff03;
        r[2] = (load_le32(key + 6) >> 4) & 0x3ffc0ff;
        r[3] = (load_le32(key + 9) >> 6) & 0x3f03fff;
        r[4] = (load_le32(key + 12) >> 8) & 0x00fffff;
        h[0] = h[1] = h[2] = h[3] = h[4] = 0;
        pad[0] = load_le32(key + 16);
        pad[1] = load_le32(key + 20);
        pad[2] = load_le32(key + 24);
        pad[3] = load_le32(key + 28);
    }

    void blocks(const uint8_t* m, size_t bytes) {
        const uint32_t hibit = final ? 0 : (1u << 24);
        uint32_t r0 = r[0], r1 = r[1], r2 = r[2], r3 = r[3], r4 = r[4];
        uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
        uint32_t h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3], h4 = h[4];
        while (bytes >= 16) {
            h0 += (load_le32(m + 0)) & 0x3ffffff;
            h1 += (load_le32(m + 3) >> 2) & 0x3ffffff;
            h2 += (load_le32(m + 6) >> 4) & 0x3ffffff;
            h3 += (load_le32(m + 9) >> 6) & 0x3ffffff;
            h4 += (load_le32(m + 12) >> 8) | hibit;

            uint64_t d0 = uint64_t(h0) * r0 + uint64_t(h1) * s4 + uint64_t(h2) * s3 +
                          uint64_t(h3) * s2 + uint64_t(h4) * s1;
            uint64_t d1 = uint64_t(h0) * r1 + uint64_t(h1) * r0 + uint64_t(h2) * s4 +
                          uint64_t(h3) * s3 + uint64_t(h4) * s2;
            uint64_t d2 = uint64_t(h0) * r2 + uint64_t(h1) * r1 + uint64_t(h2) * r0 +
                          uint64_t(h3) * s4 + uint64_t(h4) * s3;
            uint64_t d3 = uint64_t(h0) * r3 + uint64_t(h1) * r2 + uint64_t(h2) * r1 +
                          uint64_t(h3) * r0 + uint64_t(h4) * s4;
            uint64_t d4 = uint64_t(h0) * r4 + uint64_t(h1) * r3 + uint64_t(h2) * r2 +
                          uint64_t(h3) * r1 + uint64_t(h4) * r0;

            uint32_t c = uint32_t(d0 >> 26); h0 = uint32_t(d0) & 0x3ffffff;
            d1 += c; c = uint32_t(d1 >> 26); h1 = uint32_t(d1) & 0x3ffffff;
            d2 += c; c = uint32_t(d2 >> 26); h2 = uint32_t(d2) & 0x3ffffff;
            d3 += c; c = uint32_t(d3 >> 26); h3 = uint32_t(d3) & 0x3ffffff;
            d4 += c; c = uint32_t(d4 >> 26); h4 = uint32_t(d4) & 0x3ffffff;
            h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
            h1 += c;

            m += 16;
            bytes -= 16;
        }
        h[0] = h0; h[1] = h1; h[2] = h2; h[3] = h3; h[4] = h4;
    }

    void update(const uint8_t* m, size_t bytes) {
        if (leftover) {
            size_t want = 16 - leftover;
            if (want > bytes) want = bytes;
            std::memcpy(buffer + leftover, m, want);
            leftover += want;
            m += want;
            bytes -= want;
            if (leftover < 16) return;
            blocks(buffer, 16);
            leftover = 0;
        }
        if (bytes >= 16) {
            size_t want = bytes & ~size_t(15);
            blocks(m, want);
            m += want;
            bytes -= want;
        }
        if (bytes) {
            std::memcpy(buffer + leftover, m, bytes);
            leftover += bytes;
        }
    }

    void finish(uint8_t mac[16]) {
        if (leftover) {
            size_t i = leftover;
            buffer[i++] = 1;
            for (; i < 16; ++i) buffer[i] = 0;
            final = 1;
            blocks(buffer, 16);
        }
        uint32_t h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3], h4 = h[4], c;
        c = h1 >> 26; h1 &= 0x3ffffff;
        h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
        h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
        h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;

        uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
        uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
        uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
        uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
        uint32_t g4 = h4 + c - (1u << 26);

        uint32_t mask = (g4 >> 31) - 1;
        g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
        mask = ~mask;
        h0 = (h0 & mask) | g0;
        h1 = (h1 & mask) | g1;
        h2 = (h2 & mask) | g2;
        h3 = (h3 & mask) | g3;
        h4 = (h4 & mask) | g4;

        h0 = (h0) | (h1 << 26);
        h1 = (h1 >> 6) | (h2 << 20);
        h2 = (h2 >> 12) | (h3 << 14);
        h3 = (h3 >> 18) | (h4 << 8);

        uint64_t f = uint64_t(h0) + pad[0];            h0 = uint32_t(f);
        f = uint64_t(h1) + pad[1] + (f >> 32);         h1 = uint32_t(f);
        f = uint64_t(h2) + pad[2] + (f >> 32);         h2 = uint32_t(f);
        f = uint64_t(h3) + pad[3] + (f >> 32);         h3 = uint32_t(f);

        store_le32(mac + 0, h0);
        store_le32(mac + 4, h1);
        store_le32(mac + 8, h2);
        store_le32(mac + 12, h3);
    }
};

// MAC over aad ‖ pad16(aad) ‖ ct ‖ pad16(ct) ‖ le64(aadLen) ‖ le64(ctLen).
void poly_tag(const uint8_t polyKey[32], std::span<const uint8_t> aad,
              const uint8_t* ct, size_t ctLen, uint8_t tag[16]) {
    static const uint8_t zero[16] = {0};
    Poly1305 p(polyKey);
    p.update(aad.data(), aad.size());
    if (aad.size() % 16) p.update(zero, 16 - (aad.size() % 16));
    p.update(ct, ctLen);
    if (ctLen % 16) p.update(zero, 16 - (ctLen % 16));
    uint8_t lengths[16];
    uint64_t a = aad.size(), cl = ctLen;
    for (int i = 0; i < 8; ++i) lengths[i] = static_cast<uint8_t>(a >> (8 * i));
    for (int i = 0; i < 8; ++i) lengths[8 + i] = static_cast<uint8_t>(cl >> (8 * i));
    p.update(lengths, 16);
    p.finish(tag);
}

void make_poly_key(const Key256& key, std::span<const uint8_t, 12> nonce, uint8_t out[32]) {
    uint32_t k[8];
    for (int i = 0; i < 8; ++i) k[i] = load_le32(key.data() + 4 * i);
    uint32_t n[3] = {load_le32(nonce.data()), load_le32(nonce.data() + 4),
                     load_le32(nonce.data() + 8)};
    uint8_t block[64];
    chacha20_block(k, 0, n, block); // counter 0 → Poly1305 one-time key
    std::memcpy(out, block, 32);
}

bool ct_equal(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= a[i] ^ b[i];
    return diff == 0;
}

} // namespace

void aead_encrypt(const Key256& key, std::span<const uint8_t, 12> nonce,
                  std::span<const uint8_t> aad, std::span<const uint8_t> pt,
                  uint8_t* ct, uint8_t tag[16]) {
    uint8_t polyKey[32];
    make_poly_key(key, nonce, polyKey);
    chacha20_xor(key, nonce, 1, pt.data(), ct, pt.size());
    poly_tag(polyKey, aad, ct, pt.size(), tag);
}

bool aead_decrypt(const Key256& key, std::span<const uint8_t, 12> nonce,
                  std::span<const uint8_t> aad, std::span<const uint8_t> ct,
                  const uint8_t tag[16], uint8_t* pt) {
    uint8_t polyKey[32];
    make_poly_key(key, nonce, polyKey);
    uint8_t expect[16];
    poly_tag(polyKey, aad, ct.data(), ct.size(), expect);
    if (!ct_equal(expect, tag, 16)) return false;
    chacha20_xor(key, nonce, 1, ct.data(), pt, ct.size());
    return true;
}

// ---- secure framing -------------------------------------------------------

namespace {
constexpr uint32_t kMaxRecord = 256u << 20;

void nonce_from_counter(uint64_t ctr, uint8_t out[12]) {
    for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(ctr >> (8 * i));
    out[8] = out[9] = out[10] = out[11] = 0;
}
} // namespace

bool SecureChannel::send(socket_t s, uint16_t op, int16_t status, uint64_t req_id,
                         const uint8_t* payload, uint32_t len) {
    if (!active_) return send_message(s, op, status, req_id, payload, len);

    MsgHeader h{kMagic, op, status, req_id, len};
    std::vector<uint8_t> plain(sizeof(h) + len);
    std::memcpy(plain.data(), &h, sizeof(h));
    if (len) std::memcpy(plain.data() + sizeof(h), payload, len);

    uint8_t nonce[12];
    nonce_from_counter(txCtr_++, nonce);

    std::vector<uint8_t> rec(plain.size() + 16);
    aead_encrypt(txKey_, std::span<const uint8_t, 12>(nonce, 12), {}, plain,
                 rec.data(), rec.data() + plain.size());

    uint32_t recLen = static_cast<uint32_t>(rec.size());
    uint8_t lenbuf[4];
    store_le32(lenbuf, recLen);
    if (!send_all(s, lenbuf, 4)) return false;
    return send_all(s, rec.data(), rec.size());
}

bool SecureChannel::recv(socket_t s, MsgHeader& h, std::vector<uint8_t>& payload) {
    if (!active_) return recv_message(s, h, payload);

    uint8_t lenbuf[4];
    if (!recv_all(s, lenbuf, 4)) return false;
    uint32_t recLen = load_le32(lenbuf);
    if (recLen < sizeof(MsgHeader) + 16 || recLen > kMaxRecord) return false;

    std::vector<uint8_t> rec(recLen);
    if (!recv_all(s, rec.data(), recLen)) return false;

    uint8_t nonce[12];
    nonce_from_counter(rxCtr_++, nonce);

    size_t plainLen = recLen - 16;
    std::vector<uint8_t> plain(plainLen);
    std::span<const uint8_t> ct(rec.data(), plainLen);
    if (!aead_decrypt(rxKey_, std::span<const uint8_t, 12>(nonce, 12), {}, ct,
                      rec.data() + plainLen, plain.data()))
        return false;

    std::memcpy(&h, plain.data(), sizeof(h));
    if (h.magic != kMagic) return false;
    if (h.length != plainLen - sizeof(h)) return false;
    payload.assign(plain.begin() + sizeof(h), plain.end());
    return true;
}

} // namespace fb

#ifdef FB_CRYPTO_SELFTEST
#  include <cstdio>
int main() {
    using namespace fb;
    Key256 key;
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(0x80 + i);
    uint8_t nonce[12] = {0x07, 0x00, 0x00, 0x00, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47};
    uint8_t aad[12] = {0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7};
    const char* msg =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one "
        "tip for the future, sunscreen would be it.";
    size_t mlen = std::strlen(msg);

    std::vector<uint8_t> ct(mlen);
    uint8_t tag[16];
    aead_encrypt(key, std::span<const uint8_t, 12>(nonce, 12),
                 std::span<const uint8_t>(aad, 12),
                 std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(msg), mlen),
                 ct.data(), tag);

    const uint8_t expectTag[16] = {0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a,
                                   0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91};
    const uint8_t expectCt0[8] = {0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb};
    bool ok = (std::memcmp(tag, expectTag, 16) == 0) &&
              (std::memcmp(ct.data(), expectCt0, 8) == 0);

    // Round-trip + tamper detection.
    std::vector<uint8_t> back(mlen);
    bool dec = aead_decrypt(key, std::span<const uint8_t, 12>(nonce, 12),
                            std::span<const uint8_t>(aad, 12),
                            std::span<const uint8_t>(ct.data(), ct.size()), tag, back.data());
    ok = ok && dec && std::memcmp(back.data(), msg, mlen) == 0;
    ct[0] ^= 1;
    ok = ok && !aead_decrypt(key, std::span<const uint8_t, 12>(nonce, 12),
                             std::span<const uint8_t>(aad, 12),
                             std::span<const uint8_t>(ct.data(), ct.size()), tag, back.data());

    std::printf("%s\n", ok ? "crypto self-test PASS" : "crypto self-test FAIL");
    return ok ? 0 : 1;
}
#endif
