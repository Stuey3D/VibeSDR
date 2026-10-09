// vibe_gzip.h — a small, dependency-free gzip ENCODER for the few dynamic responses worth compressing.
//
// ★★★ WHY THIS EXISTS (Stuart, 2026-10-10: "why are they 307KB when the entire page was less than 200KB?"). GET
//     /bookmarks on the XCover — 1,623 bookmarks after the UK ATC import — went out as 307 KB of RAW JSON, every field
//     name repeated 1,623 times, while the page itself goes out brotli'd at build time. JSON like that compresses ~10x.
// ★★ WHY NOT zlib. It is on every platform we ship, but the Linux packages are built in THREE images (bookworm
//    amd64/arm64, the armhf cross, ARMv6), each of which would need its -dev package for its own arch — a release-
//    pipeline change for one endpoint. This is LZ77 + the FIXED Huffman code of RFC 1951 §3.2.6 (no dynamic tables),
//    which on repetitive JSON lands within a few percent of zlib, in one header any build already compiles.
// ★ Encoder only. Output is a single final fixed-Huffman block in a standard gzip member (RFC 1952) — any decoder
//   reads it. scripts/test-bookmark-import.mjs decodes it with Node's zlib.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace vibegz {

inline uint32_t crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

class BitWriter {
public:
    explicit BitWriter(std::string& out) : out_(out) {}
    /** `n` bits of `v`, least significant first — DEFLATE's order for everything but Huffman codes. */
    void bits(uint32_t v, int n) {
        acc_ |= uint64_t(v) << nbits_; nbits_ += n;
        while (nbits_ >= 8) { out_ += char(acc_ & 0xFF); acc_ >>= 8; nbits_ -= 8; }
    }
    /** A Huffman code: defined most significant bit first, so it is reversed before packing. */
    void code(uint32_t c, int len) {
        uint32_t r = 0;
        for (int i = 0; i < len; ++i) { r = (r << 1) | (c & 1); c >>= 1; }
        bits(r, len);
    }
    void flush() { if (nbits_ > 0) { out_ += char(acc_ & 0xFF); acc_ = 0; nbits_ = 0; } }
private:
    std::string& out_;
    uint64_t acc_ = 0;
    int nbits_ = 0;
};

/** The fixed literal/length code (RFC 1951 §3.2.6). */
inline void putLitLen(BitWriter& w, int v) {
    if (v < 144)      w.code(0x30 + v, 8);
    else if (v < 256) w.code(0x190 + (v - 144), 9);
    else if (v < 280) w.code(v - 256, 7);
    else              w.code(0xC0 + (v - 280), 8);
}

inline void putMatch(BitWriter& w, int len, int dist) {
    static const int lBase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
    static const int lExtra[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
    static const int dBase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,
                                   4097,6145,8193,12289,16385,24577 };
    static const int dExtra[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
    int li = 28;
    while (li > 0 && lBase[li] > len) --li;
    putLitLen(w, 257 + li);
    if (lExtra[li]) w.bits(uint32_t(len - lBase[li]), lExtra[li]);
    int di = 29;
    while (di > 0 && dBase[di] > dist) --di;
    w.code(uint32_t(di), 5);                       // fixed distance codes: 5 bits, the index itself
    if (dExtra[di]) w.bits(uint32_t(dist - dBase[di]), dExtra[di]);
}

/** gzip(in) — a complete .gz member. Greedy LZ77 over a 32 KB window, hash chains capped for speed (this runs on a
 *  connection thread on a Pi 2: ~300 KB of JSON is a few ms). */
inline std::string gzip(const std::string& input) {
    const uint8_t* in = reinterpret_cast<const uint8_t*>(input.data());
    const size_t n = input.size();
    std::string out;
    out.reserve(n / 4 + 64);
    static const unsigned char hdr[10] = { 0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0xFF };   // deflate, no name, OS unknown
    out.append(reinterpret_cast<const char*>(hdr), sizeof hdr);
    BitWriter w(out);
    w.bits(1, 1);   // BFINAL
    w.bits(1, 2);   // BTYPE = 01, fixed Huffman

    constexpr int kWin = 32768, kHashBits = 15, kMaxChain = 48, kMaxLen = 258, kMinLen = 3;
    std::vector<int32_t> head(size_t(1) << kHashBits, -1), prev(kWin, -1);
    auto hashAt = [&](size_t i) {
        return ((uint32_t(in[i]) << 10) ^ (uint32_t(in[i + 1]) << 5) ^ uint32_t(in[i + 2])) & ((1u << kHashBits) - 1);
    };
    auto insert = [&](size_t i) {
        if (i + 2 >= n) return;
        const uint32_t h = hashAt(i);
        prev[i & (kWin - 1)] = head[h];
        head[h] = int32_t(i);
    };
    size_t i = 0;
    while (i < n) {
        int bestLen = 0, bestDist = 0;
        if (i + kMinLen <= n) {
            int32_t cand = head[hashAt(i)];
            const size_t maxLen = std::min<size_t>(kMaxLen, n - i);
            for (int chain = 0; cand >= 0 && chain < kMaxChain; ++chain) {
                const size_t dist = i - size_t(cand);
                if (dist == 0 || dist > size_t(kWin)) break;
                if (in[cand + bestLen] == in[i + bestLen]) {
                    size_t l = 0;
                    while (l < maxLen && in[cand + l] == in[i + l]) ++l;
                    if (int(l) > bestLen) { bestLen = int(l); bestDist = int(dist); if (l == maxLen) break; }
                }
                const int32_t nx = prev[size_t(cand) & (kWin - 1)];
                if (nx >= cand) break;   // the slot was reused by a newer position — the chain has ended
                cand = nx;
            }
        }
        if (bestLen >= kMinLen) {
            putMatch(w, bestLen, bestDist);
            for (int k = 0; k < bestLen; ++k) insert(i + size_t(k));
            i += size_t(bestLen);
        } else {
            putLitLen(w, in[i]);
            insert(i);
            ++i;
        }
    }
    putLitLen(w, 256);   // end of block
    w.flush();
    const uint32_t crc = crc32(in, n), size = uint32_t(n);
    for (int k = 0; k < 4; ++k) out += char((crc >> (8 * k)) & 0xFF);
    for (int k = 0; k < 4; ++k) out += char((size >> (8 * k)) & 0xFF);
    return out;
}

}  // namespace vibegz
