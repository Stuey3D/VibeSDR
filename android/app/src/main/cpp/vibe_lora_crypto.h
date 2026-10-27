// vibe_lora_crypto.h — the three primitives the mesh networks' PUBLIC channels need: AES-128 (FIPS-197), SHA-256 (FIPS
// 180-4) and HMAC (RFC 2104). Written from the standards; checked in test-lora against their own published test vectors
// (FIPS-197 Appendix C.1, FIPS 180-4's "abc", RFC 4231 case 2).
//
// ★★★ WHAT THEY ARE FOR (Stuart, 2026-10-27): reading what a mesh network publishes ON PURPOSE — Meshtastic's default
//     channel (its key is printed in their documentation) and MeshCore's built-in Public channel (likewise). Nothing
//     here guesses or searches for a key; a packet that does not open with the published key is shown by its header
//     only (vibe_meshtastic.h, vibe_meshcore.h). docs/v12/LORA-MESH.md §Privacy.
// ★ Table-free AES (S-box computed once) — speed is irrelevant at a few packets a minute, size and clarity are not.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace vibelora {
namespace crypto {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  AES-128
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class Aes128 {
public:
    explicit Aes128(const uint8_t key[16]) {
        const Tables& t = tables();
        std::memcpy(rk_, key, 16);
        uint8_t rcon = 1;
        for (int i = 4; i < 44; ++i) {
            uint8_t w[4];
            std::memcpy(w, rk_ + 4 * (i - 1), 4);
            if (i % 4 == 0) {
                const uint8_t a = w[0];
                w[0] = uint8_t(t.sbox[w[1]] ^ rcon); w[1] = t.sbox[w[2]]; w[2] = t.sbox[w[3]]; w[3] = t.sbox[a];
                rcon = xt(rcon);
            }
            for (int k = 0; k < 4; ++k) rk_[4 * i + k] = uint8_t(rk_[4 * (i - 4) + k] ^ w[k]);
        }
    }
    void encrypt(const uint8_t in[16], uint8_t out[16]) const {
        const Tables& t = tables();
        uint8_t s[16];
        for (int i = 0; i < 16; ++i) s[i] = uint8_t(in[i] ^ rk_[i]);
        for (int r = 1; r <= 10; ++r) {
            for (int i = 0; i < 16; ++i) s[i] = t.sbox[s[i]];
            shiftRows(s);
            if (r < 10) mixColumns(s);
            for (int i = 0; i < 16; ++i) s[i] ^= rk_[16 * r + i];
        }
        std::memcpy(out, s, 16);
    }
    void decrypt(const uint8_t in[16], uint8_t out[16]) const {
        const Tables& t = tables();
        uint8_t s[16];
        for (int i = 0; i < 16; ++i) s[i] = uint8_t(in[i] ^ rk_[160 + i]);
        for (int r = 9; r >= 0; --r) {
            invShiftRows(s);
            for (int i = 0; i < 16; ++i) s[i] = t.inv[s[i]];
            for (int i = 0; i < 16; ++i) s[i] ^= rk_[16 * r + i];
            if (r > 0) invMixColumns(s);
        }
        std::memcpy(out, s, 16);
    }

private:
    struct Tables { uint8_t sbox[256], inv[256]; };
    static uint8_t xt(uint8_t a) { return uint8_t((a << 1) ^ ((a & 0x80) ? 0x1B : 0)); }
    static uint8_t mul(uint8_t a, uint8_t b) { uint8_t r = 0; while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; } return r; }
    static const Tables& tables() {
        static const Tables t = [] {
            Tables x{};
            // S-box: the multiplicative inverse in GF(2^8), then the affine map (FIPS-197 §5.1.1)
            for (int a = 0; a < 256; ++a) {
                uint8_t inv = 0;
                if (a) for (int b = 1; b < 256; ++b) if (mul(uint8_t(a), uint8_t(b)) == 1) { inv = uint8_t(b); break; }
                uint8_t s = inv, r = inv;
                for (int k = 0; k < 4; ++k) { r = uint8_t((r << 1) | (r >> 7)); s ^= r; }
                x.sbox[a] = uint8_t(s ^ 0x63);
            }
            for (int a = 0; a < 256; ++a) x.inv[x.sbox[a]] = uint8_t(a);
            return x;
        }();
        return t;
    }
    // state is column-major: s[4c + r]
    static void shiftRows(uint8_t* s) { uint8_t t[16]; for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) t[4 * c + r] = s[4 * ((c + r) & 3) + r]; std::memcpy(s, t, 16); }
    static void invShiftRows(uint8_t* s) { uint8_t t[16]; for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) t[4 * ((c + r) & 3) + r] = s[4 * c + r]; std::memcpy(s, t, 16); }
    static void mixColumns(uint8_t* s) {
        for (int c = 0; c < 4; ++c) {
            uint8_t* a = s + 4 * c; const uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
            a[0] = uint8_t(mul(a0, 2) ^ mul(a1, 3) ^ a2 ^ a3); a[1] = uint8_t(a0 ^ mul(a1, 2) ^ mul(a2, 3) ^ a3);
            a[2] = uint8_t(a0 ^ a1 ^ mul(a2, 2) ^ mul(a3, 3)); a[3] = uint8_t(mul(a0, 3) ^ a1 ^ a2 ^ mul(a3, 2));
        }
    }
    static void invMixColumns(uint8_t* s) {
        for (int c = 0; c < 4; ++c) {
            uint8_t* a = s + 4 * c; const uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
            a[0] = uint8_t(mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9));
            a[1] = uint8_t(mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13));
            a[2] = uint8_t(mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11));
            a[3] = uint8_t(mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14));
        }
    }
    uint8_t rk_[176];
};

/** AES-128 in counter mode: the 16-byte counter block's LAST bytes count up (big-endian), as NIST SP 800-38A's
 *  standard incrementing function. Encrypt and decrypt are the same operation. */
inline void aesCtr(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t n) {
    Aes128 a(key);
    uint8_t ctr[16], ks[16];
    std::memcpy(ctr, iv, 16);
    for (size_t i = 0; i < n; i += 16) {
        a.encrypt(ctr, ks);
        for (size_t k = 0; k < 16 && i + k < n; ++k) out[i + k] = uint8_t(in[i + k] ^ ks[k]);
        for (int b = 15; b >= 0; --b) if (++ctr[b]) break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  SHA-256 and HMAC-SHA-256
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline void sha256(const uint8_t* msg, size_t len, uint8_t out[32]) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<uint8_t> m(msg, msg + len);
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    const uint64_t bits = uint64_t(len) * 8;
    for (int i = 7; i >= 0; --i) m.push_back(uint8_t(bits >> (8 * i)));
    auto ror = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = uint32_t(m[off + 4 * i]) << 24 | uint32_t(m[off + 4 * i + 1]) << 16 | uint32_t(m[off + 4 * i + 2]) << 8 | m[off + 4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25), ch = (e & f) ^ (~e & g);
            const uint32_t t1 = k + S1 + ch + K[i] + w[i];
            const uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22), mj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + mj;
            k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
    }
    for (int i = 0; i < 8; ++i) for (int b = 0; b < 4; ++b) out[4 * i + b] = uint8_t(h[i] >> (24 - 8 * b));
}

inline void hmacSha256(const uint8_t* key, size_t klen, const uint8_t* msg, size_t len, uint8_t out[32]) {
    uint8_t k[64] = {0};
    if (klen > 64) sha256(key, klen, k); else std::memcpy(k, key, klen);
    std::vector<uint8_t> in(64 + len);
    for (int i = 0; i < 64; ++i) in[size_t(i)] = uint8_t(k[i] ^ 0x36);
    std::memcpy(in.data() + 64, msg, len);
    uint8_t ih[32];
    sha256(in.data(), in.size(), ih);
    uint8_t o[96];
    for (int i = 0; i < 64; ++i) o[i] = uint8_t(k[i] ^ 0x5c);
    std::memcpy(o + 64, ih, 32);
    sha256(o, 96, out);
}

inline std::vector<uint8_t> fromHex(const std::string& h) {
    std::vector<uint8_t> v;
    auto nib = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i + 1 < h.size(); i += 2) { const int a = nib(h[i]), b = nib(h[i + 1]); if (a < 0 || b < 0) break; v.push_back(uint8_t(a << 4 | b)); }
    return v;
}
inline std::string toHex(const uint8_t* p, size_t n, bool upper = false) {
    static const char* lo = "0123456789abcdef"; static const char* up = "0123456789ABCDEF";
    const char* d = upper ? up : lo;
    std::string s; for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

} // namespace crypto
} // namespace vibelora
