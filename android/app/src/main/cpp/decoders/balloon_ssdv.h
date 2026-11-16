// balloon_ssdv.h — V12 balloons, part 3: SSDV, pictures from a balloon a packet at a time (docs/v12/BALLOONS.md).
//
// The format as the UKHAS wiki describes it (guides:ssdv): each packet is 256 bytes —
//     0 sync 0x55 · 1 type (0x66 normal: 205 payload bytes + RS · 0x67 no-FEC: 237) · 2–5 callsign (base 40) ·
//     6 image ID · 7–8 packet ID (big-endian) · 9 width ÷ 16 · 10 height ÷ 16 · 11 flags 00qqqexx (quality XOR 4, end of
//     image, subsampling 0 = 2×2, 1 = 1×2, 2 = 2×1, 3 = 1×1) · 12 MCU offset (0xFF none) · 13–14 MCU index (0xFFFF none) ·
//     15… payload · then a CRC-32 of bytes 1 … end of payload · normal mode: 32 Reed–Solomon check bytes over bytes 1–223.
//   The payload is a baseline JPEG's scan data with no markers and no byte stuffing; the quantisation and Huffman tables
//   are fixed; the first MCU that STARTS in a packet starts on a byte (its offset is in the header) and its first DC value
//   of each component is relative to 0 — so every packet can be decoded without the one before it.
// ★ Our own decoder. Conventions the wiki does not spell out were read from the reference program's source (GPL-3, read
//   for the conventions, nothing copied), and are what a receiver must match:
//     • CRC-32 is the usual (zlib) one; the Reed–Solomon code is CCSDS's RS(255,223) in its conventional basis — field
//       x⁸ + x⁷ + x² + x + 1, generator roots β^(112 … 143), β = α¹¹ — over the 255 bytes after the sync byte, the first
//       byte the highest power;
//     • callsign: base 40, the first character least significant ('0'–'9' = 1–10, 'A'–'Z' = 14–39);
//     • tables: the JPEG standard's (ITU-T T.81 Annex K) Huffman tables; quantisation: Annex K's two tables with every
//       value rounded up to an even number — except the chrominance table's third and fifth (zig-zag) entries, 22 —
//       scaled for quality level 0–7 as the IJG library scales quality 1, 14, 29, 43, 50, 71, 86, 100;
//     • the MCU is 4, 2 or 1 luminance blocks then Cb then Cr; a packet's reset MCU has ITS FIRST luminance block's DC and
//       its Cb and Cr DC relative to 0 (the other luminance blocks of that MCU follow on from the first, as usual);
//     • a lost MCU is written as an "empty" block per component — DC difference 0, end of block — and an MCU cut short by
//       a lost packet is ended the same way.
// ★ Bounded (256 MB boxes): an image is at most kMaxPackets packets and kMaxSide16 × 16 pixels a side; at most
//   kMaxImages pictures are kept in progress (the oldest goes first).
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vibe {
namespace balloon {
namespace ssdv {

constexpr int kPkt = 256, kHeader = 15, kPayloadFec = 205, kPayloadNoFec = 237;
constexpr int kMaxPackets = 2048, kMaxSide16 = 128, kMaxImages = 3;

inline uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) { c ^= p[i]; for (int b = 0; b < 8; ++b) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1; }
    return c ^ 0xFFFFFFFFu;
}

// ── RS(255,223), CCSDS conventional basis ──────────────────────────────────────────────────────────────────────────
/** Codeword byte i is the coefficient of xⁱ (parity 0…31); everything in powers of β = α¹¹, the code's primitive element,
 *  so Berlekamp–Massey / Chien / Forney run as for a code with roots β^(FCR + j). */
class Rs {
public:
    static constexpr int N = 255, R = 32, FCR = 112;
    static const Rs& get() { static Rs r; return r; }
    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp_[log_[a] + log_[b]] : 0; }
    uint8_t div(uint8_t a, uint8_t b) const { return a ? exp_[(log_[a] + 255 - log_[b]) % 255] : 0; }
    uint8_t pw(int e) const { return exp_[((e % 255) + 255) % 255]; }
    const uint8_t* gen() const { return gen_; }
    void encode(uint8_t* cw) const {
        uint8_t rem[R] = {0};
        for (int i = N - 1; i >= R; --i) {
            const uint8_t fb = cw[i] ^ rem[R - 1];
            for (int k = R - 1; k >= 1; --k) rem[k] = rem[k - 1] ^ mul(fb, gen_[k]);
            rem[0] = mul(fb, gen_[0]);
        }
        std::memcpy(cw, rem, R);
    }
    void syndromes(const uint8_t* cw, uint8_t* S) const {
        for (int j = 0; j < R; ++j) {
            const uint8_t a = pw(FCR + j);
            uint8_t s = 0;
            for (int i = N - 1; i >= 0; --i) s = mul(s, a) ^ cw[i];
            S[j] = s;
        }
    }
    /** Errors only; returns bytes corrected or −1. */
    int decode(uint8_t* cw) const {
        uint8_t S[R];
        syndromes(cw, S);
        bool any = false;
        for (int j = 0; j < R; ++j) any |= S[j] != 0;
        if (!any) return 0;
        uint8_t L[R + 1] = {1}, B[R + 1] = {1}, T[R + 1];
        int deg = 0;
        for (int r = 0; r < R; ++r) {
            uint8_t d = 0;
            for (int i = 0; i <= deg && i <= r; ++i) d ^= mul(L[i], S[r - i]);
            for (int i = R; i >= 1; --i) B[i] = B[i - 1];
            B[0] = 0;
            if (!d) continue;
            for (int i = 0; i <= R; ++i) T[i] = L[i] ^ mul(d, B[i]);
            if (2 * deg <= r) { deg = r + 1 - deg; for (int i = 0; i <= R; ++i) B[i] = div(L[i], d); }
            std::memcpy(L, T, sizeof L);
        }
        int dl = R; while (dl > 0 && !L[dl]) --dl;
        if (dl == 0 || 2 * dl > R) return -1;
        uint8_t O[R] = {0};
        for (int i = 0; i < R; ++i) for (int j = 0; j <= i && j <= dl; ++j) O[i] ^= mul(L[j], S[i - j]);
        int found = 0, pos[R]; uint8_t val[R];
        for (int i = 0; i < N; ++i) {
            const uint8_t xi = pw(-i);
            uint8_t v = 0, p = 1;
            for (int k = 0; k <= dl; ++k) { v ^= mul(L[k], p); p = mul(p, xi); }
            if (v) continue;
            if (found == dl) return -1;
            uint8_t num = 0; p = 1;
            for (int k = 0; k < R; ++k) { num ^= mul(O[k], p); p = mul(p, xi); }
            uint8_t den = 0; p = 1;
            for (int k = 1; k <= dl; k += 2) { den ^= mul(L[k], p); p = mul(p, mul(xi, xi)); }
            if (!den) return -1;
            val[found] = mul(pw(i * (1 - FCR)), div(num, den));
            pos[found++] = i;
        }
        if (found != dl) return -1;
        for (int k = 0; k < found; ++k) cw[pos[k]] ^= val[k];
        syndromes(cw, S);
        for (int j = 0; j < R; ++j) if (S[j]) return -1;
        return found;
    }
private:
    Rs() {
        uint8_t ae[255]; int x = 1;
        for (int i = 0; i < 255; ++i) { ae[i] = (uint8_t)x; x <<= 1; if (x & 0x100) x ^= 0x187; }
        // β = α¹¹: exp_ / log_ are in powers of β
        for (int i = 0; i < 255; ++i) { exp_[i] = ae[(11 * i) % 255]; log_[exp_[i]] = (uint8_t)i; }
        for (int i = 255; i < 512; ++i) exp_[i] = exp_[i - 255];
        log_[0] = 0;
        uint8_t g[R + 1] = {1};
        for (int j = 0; j < R; ++j) {
            const uint8_t r = pw(FCR + j);
            for (int k = j + 1; k >= 1; --k) g[k] = g[k - 1] ^ mul(g[k], r);
            g[0] = mul(g[0], r);
        }
        std::memcpy(gen_, g, sizeof gen_);
    }
    uint8_t exp_[512]{}, log_[256]{}, gen_[R + 1]{};
};
/** Packet bytes 1…255 ↔ the codeword (byte 1 is x²⁵⁴). */
inline void toCw(const uint8_t* pkt, uint8_t* cw) { for (int k = 0; k < 255; ++k) cw[254 - k] = pkt[1 + k]; }
inline void fromCw(const uint8_t* cw, uint8_t* pkt) { for (int k = 0; k < 255; ++k) pkt[1 + k] = cw[254 - k]; }

// ── callsign ───────────────────────────────────────────────────────────────────────────────────────────────────────
inline uint32_t encodeCall(const std::string& c) {
    uint32_t x = 0;
    const size_t n = std::min<size_t>(c.size(), 6);
    for (size_t i = n; i-- > 0;) {
        const char ch = c[i];
        x *= 40;
        if (ch >= 'A' && ch <= 'Z') x += (uint32_t)(ch - 'A' + 14);
        else if (ch >= 'a' && ch <= 'z') x += (uint32_t)(ch - 'a' + 14);
        else if (ch >= '0' && ch <= '9') x += (uint32_t)(ch - '0' + 1);
    }
    return x;
}
inline std::string decodeCall(uint32_t x) {
    if (x > 0xF423FFFFu) return "";
    std::string s;
    while (x && s.size() < 6) {
        const uint32_t v = x % 40; x /= 40;
        s += v == 0 ? '-' : v < 11 ? (char)('0' + v - 1) : v < 14 ? '-' : (char)('A' + v - 14);
    }
    return s;
}

// ── a packet ───────────────────────────────────────────────────────────────────────────────────────────────────────
struct Packet {
    bool fec = true;
    std::string call;
    uint8_t image = 0;
    uint16_t id = 0;
    uint8_t w16 = 0, h16 = 0;
    int quality = 4, mode = 0;
    bool eoi = false;
    uint8_t mcuOffset = 0xFF;
    uint16_t mcuIndex = 0xFFFF;
    std::vector<uint8_t> payload;
    int rsFixed = 0;
    int mcuCount() const { const int m = w16 * h16; return mode == 0 ? m : (mode == 3 ? m * 4 : m * 2); }
};
/** ★ 256 bytes (sync first) → a packet, or false. Normal mode: the CRC, and if it fails Reed–Solomon then the CRC again;
 *  no-FEC: the CRC alone. Then the header must make sense (sizes, offset inside the payload, index inside the image). */
inline bool parsePacket(const uint8_t* in, Packet& p) {
    if (in[0] != 0x55 || (in[1] != 0x66 && in[1] != 0x67)) return false;
    uint8_t b[kPkt]; std::memcpy(b, in, kPkt);
    const bool fec = b[1] == 0x66;
    const int pl = fec ? kPayloadFec : kPayloadNoFec, crcAt = kHeader + pl;
    auto crcOk = [&] {
        const uint32_t c = crc32(b + 1, (size_t)crcAt - 1);
        return c == ((uint32_t)b[crcAt] << 24 | (uint32_t)b[crcAt + 1] << 16 | (uint32_t)b[crcAt + 2] << 8 | b[crcAt + 3]);
    };
    int fixed = 0;
    if (!crcOk()) {
        if (!fec) return false;
        uint8_t cw[255]; toCw(b, cw);
        fixed = Rs::get().decode(cw);
        if (fixed < 0) return false;
        fromCw(cw, b);
        if (b[1] != 0x66 || !crcOk()) return false;
    }
    p = Packet();
    p.fec = fec; p.rsFixed = fixed;
    p.call = decodeCall((uint32_t)b[2] << 24 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 8 | b[5]);
    p.image = b[6]; p.id = (uint16_t)(b[7] << 8 | b[8]);
    p.w16 = b[9]; p.h16 = b[10];
    p.quality = ((b[11] >> 3) & 7) ^ 4; p.eoi = (b[11] >> 2) & 1; p.mode = b[11] & 3;
    p.mcuOffset = b[12]; p.mcuIndex = (uint16_t)(b[13] << 8 | b[14]);
    if (p.w16 == 0 || p.h16 == 0 || p.w16 > kMaxSide16 || p.h16 > kMaxSide16 || (b[11] & 0xC0)) return false;
    if (p.mcuIndex != 0xFFFF && (p.mcuOffset >= pl || p.mcuIndex >= p.mcuCount())) return false;
    if (p.mcuIndex == 0xFFFF && p.mcuOffset != 0xFF) return false;
    p.payload.assign(b + kHeader, b + kHeader + pl);
    return true;
}

// ── the JPEG tables ────────────────────────────────────────────────────────────────────────────────────────────────
struct HuffSpec { uint8_t bits[16]; std::vector<uint8_t> vals; };
/** ITU-T T.81 Annex K.3: [0] DC luminance, [1] DC chrominance, [2] AC luminance, [3] AC chrominance. The AC lists are
 *  their published irregular head, then every remaining run/size symbol in ascending order (as the standard's lists are). */
inline const HuffSpec& huff(int t) {
    static const std::array<HuffSpec, 4> h = [] {
        std::array<HuffSpec, 4> s;
        const uint8_t b0[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
        const uint8_t b1[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
        const uint8_t b2[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
        const uint8_t b3[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
        std::memcpy(s[0].bits, b0, 16); std::memcpy(s[1].bits, b1, 16); std::memcpy(s[2].bits, b2, 16); std::memcpy(s[3].bits, b3, 16);
        for (int i = 0; i < 12; ++i) { s[0].vals.push_back((uint8_t)i); s[1].vals.push_back((uint8_t)i); }
        const std::vector<uint8_t> lumHead = { 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
                                               0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
                                               0x24, 0x33, 0x62, 0x72, 0x82 };
        const std::vector<uint8_t> chrHead = { 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
                                               0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
                                               0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1 };
        auto fill = [](HuffSpec& t, const std::vector<uint8_t>& head) {
            t.vals = head;
            for (int v = 0; v < 256; ++v) {
                const int r = v >> 4, sz = v & 15;
                const bool sym = (sz >= 1 && sz <= 10) || v == 0x00 || v == 0xf0;
                (void)r;
                if (sym && std::find(head.begin(), head.end(), (uint8_t)v) == head.end()) t.vals.push_back((uint8_t)v);
            }
        };
        fill(s[2], lumHead); fill(s[3], chrHead);
        return s;
    }();
    return h[(size_t)t];
}
/** Annex K's tables in zig-zag order, and SSDV's: each rounded up to even, chrominance zig-zag 3 and 5 = 22. */
inline const uint8_t* zigzag() {
    static const uint8_t z[64] = { 0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
                                   35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };
    return z;
}
inline void baseQuant(int table, uint8_t out[64]) {
    static const uint8_t lum[64] = { 16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56,
                                     14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
                                     49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99 };
    static const uint8_t chr[64] = { 17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99,
                                     47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                     99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99 };
    for (int i = 0; i < 64; ++i) {
        const int v = (table ? chr : lum)[zigzag()[i]];
        out[i] = (uint8_t)((v + 1) / 2 * 2);
    }
    if (table) out[3] = out[5] = 22;
}
/** Level 0–7 → the table SSDV's decoder writes (IJG scaling of quality 1, 14, 29, 43, 50, 71, 86, 100). */
inline void quantTable(int table, int level, uint8_t out[64]) {
    static const int q[8] = { 1, 14, 29, 43, 50, 71, 86, 100 };
    const int Q = q[std::max(0, std::min(7, level))];
    const int scale = Q < 50 ? 5000 / Q : 200 - 2 * Q;
    uint8_t b[64]; baseQuant(table, b);
    for (int i = 0; i < 64; ++i) { int v = (b[i] * scale + 50) / 100; out[i] = (uint8_t)std::max(1, std::min(255, v)); }
}

/** Huffman codes for encoding and the decoding tables (T.81 Annex C / F.2.2.3). */
struct Huff {
    uint16_t code[256] = {0}; uint8_t size[256] = {0};
    int maxcode[18]; int valptr[17]; int mincode[17]; std::vector<uint8_t> vals;
    explicit Huff(const HuffSpec& s) : vals(s.vals) {
        int k = 0, c = 0;
        std::fill(maxcode, maxcode + 18, -1);
        for (int l = 1; l <= 16; ++l) {
            valptr[l] = k; mincode[l] = c;
            for (int i = 0; i < s.bits[l - 1]; ++i) { if (k < (int)vals.size()) { code[vals[(size_t)k]] = (uint16_t)c; size[vals[(size_t)k]] = (uint8_t)l; } ++k; ++c; }
            maxcode[l] = s.bits[l - 1] ? c - 1 : -1;
            c <<= 1;
        }
    }
};
inline const Huff& huffTable(int t) {
    static const Huff h0(huff(0)), h1(huff(1)), h2(huff(2)), h3(huff(3));
    return t == 0 ? h0 : t == 1 ? h1 : t == 2 ? h2 : h3;
}

// ── bits ───────────────────────────────────────────────────────────────────────────────────────────────────────────
class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& o, bool stuff) : o_(o), stuff_(stuff) {}
    void put(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            acc_ = (uint8_t)((acc_ << 1) | ((v >> i) & 1));
            if (++nb_ == 8) { o_.push_back(acc_); if (stuff_ && acc_ == 0xFF) o_.push_back(0); acc_ = 0; nb_ = 0; }
        }
    }
    /** Pad to a byte with 1s. */
    void align() { if (nb_) put(0xFF, 8 - nb_); }
    int pending() const { return nb_; }
private:
    std::vector<uint8_t>& o_;
    bool stuff_;
    uint8_t acc_ = 0; int nb_ = 0;
};
class BitReader {
public:
    BitReader(const uint8_t* d, size_t nbytes) : d_(d), n_(nbytes * 8) {}
    bool get(int n, uint32_t& v) {
        if (pos_ + (size_t)n > n_) return false;
        v = 0;
        for (int i = 0; i < n; ++i, ++pos_) v = (v << 1) | ((d_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1u);
        return true;
    }
    /** One Huffman symbol (false: past the end, or not a code). */
    bool sym(const Huff& h, int& s) {
        int c = 0;
        for (int l = 1; l <= 16; ++l) {
            uint32_t b; if (!get(1, b)) return false;
            c = (c << 1) | (int)b;
            if (h.maxcode[l] >= 0 && c <= h.maxcode[l] && c >= h.mincode[l]) {
                const int k = h.valptr[l] + c - h.mincode[l];
                if (k >= (int)h.vals.size()) return false;
                s = h.vals[(size_t)k]; return true;
            }
        }
        return false;
    }
    size_t pos() const { return pos_; }
    void seek(size_t bit) { pos_ = bit; }
    size_t size() const { return n_; }
private:
    const uint8_t* d_; size_t n_; size_t pos_ = 0;
};
inline int extend(uint32_t v, int s) { return s == 0 ? 0 : (v < (1u << (s - 1)) ? (int)v - (1 << s) + 1 : (int)v); }
inline void sizeBits(int v, int& s, uint32_t& bits) {
    int a = v < 0 ? -v : v; s = 0; while (a) { ++s; a >>= 1; }
    bits = v < 0 ? (uint32_t)(v + (1 << s) - 1) : (uint32_t)v;
}

// ── one picture ────────────────────────────────────────────────────────────────────────────────────────────────────
/** The packets of one image (callsign + image ID) and the JPEG they make. */
class Image {
public:
    std::string call; int imageId = 0, w16 = 0, h16 = 0, quality = 4, mode = 0;
    bool sawEoi = false; int lastId = -1;   ///< the highest packet ID seen, and whether the end-of-image packet came
    std::map<uint16_t, Packet> packets;
    int64_t firstAt = 0, lastAt = 0;
    bool add(const Packet& p) {
        if (packets.empty()) { call = p.call; imageId = p.image; w16 = p.w16; h16 = p.h16; quality = p.quality; mode = p.mode; }
        else if (p.w16 != w16 || p.h16 != h16 || p.mode != mode || p.quality != quality || p.call != call) return false;
        if (packets.count(p.id) || (int)packets.size() >= kMaxPackets) return false;
        packets[p.id] = p;
        if (p.eoi) sawEoi = true;
        lastId = std::max(lastId, (int)p.id);
        return true;
    }
    int ycparts() const { return mode == 0 ? 4 : mode == 3 ? 1 : 2; }
    int mcuCount() const { const int m = w16 * h16; return mode == 0 ? m : (mode == 3 ? m * 4 : m * 2); }
    /** The packets expected: the end-of-image packet's ID + 1 once it is in, else unknown (0). */
    int expected() const { if (!sawEoi) return 0; int e = 0; for (auto& kv : packets) if (kv.second.eoi) e = std::max(e, (int)kv.first + 1); return e; }
    bool complete() const { const int e = expected(); return e > 0 && (int)packets.size() >= e; }
    /** MCUs decoded into the picture by the last build. */
    int mcusDecoded = 0;

    /** ★ The JPEG: headers, then every MCU in order — decoded from the packets we have, an empty block where one is lost. */
    std::vector<uint8_t> jpeg() {
        std::vector<uint8_t> o;
        o.reserve(packets.size() * 220 + 700);
        headers_(o);
        BitWriter w(o, true);
        const int total = mcuCount(), yc = ycparts();
        int outPred[3] = { 0, 0, 0 };
        int mcu = 0;   // the next MCU to write
        mcusDecoded = 0;
        // ★ a RUN is a stretch of consecutive packet IDs: their payloads are one bit stream
        auto it = packets.begin();
        while (it != packets.end() && mcu < total) {
            std::vector<uint8_t> buf;
            struct Mark { size_t startBit; int firstMcu; int offset; };
            std::vector<Mark> marks;
            int prev = -2;
            auto start = it;
            for (; it != packets.end(); ++it) {
                if (prev >= 0 && it->first != prev + 1) break;
                if (it != start && it->first != prev + 1) break;
                prev = it->first;
                marks.push_back({ buf.size() * 8, it->second.mcuIndex == 0xFFFF ? -1 : it->second.mcuIndex, it->second.mcuOffset });
                buf.insert(buf.end(), it->second.payload.begin(), it->second.payload.end());
            }
            // where the run starts: its first packet that starts an MCU
            size_t m0 = 0; while (m0 < marks.size() && marks[m0].firstMcu < 0) ++m0;
            if (m0 == marks.size()) continue;
            BitReader r(buf.data(), buf.size());
            int cur = marks[m0].firstMcu;
            if (cur < mcu) continue;            // out of order with what is written (an inconsistent image): skip the run
            while (mcu < cur && mcu < total) emptyMcu_(w, yc), ++mcu;
            size_t nm = m0;
            int abs_[3] = { 0, 0, 0 };
            while (mcu < total) {
                // ★ is this MCU a packet's first? then it starts at that packet's offset, its DC from 0
                while (nm < marks.size() && marks[nm].firstMcu < mcu) ++nm;   // (−1: no MCU starts there)
                bool reset = false;
                if (nm < marks.size() && marks[nm].firstMcu == mcu) {
                    r.seek(marks[nm].startBit + (size_t)marks[nm].offset * 8); reset = true; ++nm;
                }
                const bool ok = decodeMcu_(r, w, yc, reset, abs_, outPred);
                ++mcu;   // ★ written either way: a cut-short MCU was ended cleanly
                if (ok) { ++mcusDecoded; continue; }
                // ★ the stream broke (a lost packet next, or a bad code): on at the run's next packet that starts an MCU
                while (nm < marks.size() && marks[nm].firstMcu < mcu) ++nm;
                if (nm == marks.size()) break;
                while (mcu < marks[nm].firstMcu && mcu < total) emptyMcu_(w, yc), ++mcu;
            }
        }
        while (mcu < total) emptyMcu_(w, yc), ++mcu;
        w.align();
        o.push_back(0xFF); o.push_back(0xD9);
        return o;
    }

private:
    /** One MCU from the stream to the writer. On running out (a lost packet next) the MCU is ended cleanly and false comes
     *  back; nothing half-read is ever written. */
    bool decodeMcu_(BitReader& r, BitWriter& w, int yc, bool reset, int* abs_, int* outPred) {
        const int parts = yc + 2;
        for (int part = 0; part < parts; ++part) {
            const int comp = part < yc ? 0 : part - yc + 1;
            const Huff& hd = huffTable(comp ? 1 : 0);
            const Huff& ha = huffTable(comp ? 3 : 2);
            // DC
            int s;
            uint32_t v = 0;
            const size_t save = r.pos();
            if (!r.sym(hd, s) || s > 11 || !r.get(s, v)) { r.seek(save); endRest_(w, yc, part, false); return false; }
            const int diff = extend(v, s);
            if (reset && (part == 0 || comp != 0)) abs_[comp] = diff; else abs_[comp] += diff;
            const int od = abs_[comp] - outPred[comp];
            outPred[comp] = abs_[comp];
            int os; uint32_t ob; sizeBits(od, os, ob);
            if (os > 11) { endRest_(w, yc, part, false); return false; }
            w.put(hd.code[os], hd.size[os]); if (os) w.put(ob, os);
            // AC — copied symbol for symbol
            for (int k = 1; k < 64;) {
                int rs;
                if (!r.sym(ha, rs)) { endRest_(w, yc, part, true); return false; }
                const int run = rs >> 4, sz = rs & 15;
                uint32_t bits = 0;
                if (sz && !r.get(sz, bits)) { endRest_(w, yc, part, true); return false; }
                if (sz == 0 && run != 15) { w.put(ha.code[rs], ha.size[rs]); break; }   // end of block
                k += sz == 0 ? 16 : run + 1;
                if (k > 64) { endRest_(w, yc, part, true); return false; }
                w.put(ha.code[rs], ha.size[rs]); if (sz) w.put(bits, sz);
            }
        }
        return true;
    }
    /** End a cut-short MCU: an EOB for the block in its AC (`inAc`), then an empty block for each part left. */
    void endRest_(BitWriter& w, int yc, int part, bool inAc) {
        if (inAc) { const Huff& ha = huffTable(part < yc ? 2 : 3); w.put(ha.code[0], ha.size[0]); ++part; }
        for (; part < yc + 2; ++part) {
            const int comp = part < yc ? 0 : 1;
            const Huff& hd = huffTable(comp ? 1 : 0); const Huff& ha = huffTable(comp ? 3 : 2);
            w.put(hd.code[0], hd.size[0]); w.put(ha.code[0], ha.size[0]);
        }
    }
    void emptyMcu_(BitWriter& w, int yc) { endRest_(w, yc, 0, false); }
    void headers_(std::vector<uint8_t>& o) const {
        auto m = [&](uint8_t code, const std::vector<uint8_t>& body) {
            o.push_back(0xFF); o.push_back(code);
            const size_t n = body.size() + 2; o.push_back((uint8_t)(n >> 8)); o.push_back((uint8_t)n);
            o.insert(o.end(), body.begin(), body.end());
        };
        o.push_back(0xFF); o.push_back(0xD8);
        m(0xE0, { 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0 });
        for (int t = 0; t < 2; ++t) { uint8_t q[64]; quantTable(t, quality, q); std::vector<uint8_t> b{ (uint8_t)t }; b.insert(b.end(), q, q + 64); m(0xDB, b); }
        const int W = w16 * 16, H = h16 * 16;
        const uint8_t samp = mode == 0 ? 0x22 : mode == 1 ? 0x12 : mode == 2 ? 0x21 : 0x11;
        m(0xC0, { 8, (uint8_t)(H >> 8), (uint8_t)H, (uint8_t)(W >> 8), (uint8_t)W, 3, 1, samp, 0, 2, 0x11, 1, 3, 0x11, 1 });
        const uint8_t cls[4] = { 0x00, 0x01, 0x10, 0x11 };
        for (int t : { 0, 2, 1, 3 }) {
            const HuffSpec& h = huff(t);
            std::vector<uint8_t> b{ cls[t] }; b.insert(b.end(), h.bits, h.bits + 16); b.insert(b.end(), h.vals.begin(), h.vals.end());
            m(0xC4, b);
        }
        m(0xDA, { 3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0 });
    }
};

/** ★ Every picture being received: packets in, the picture they belong to out. Bounded: kMaxImages kept. */
class Collector {
public:
    /** The image a packet went to (null: refused — a repeat, or it does not match its image). */
    Image* add(const Packet& p, int64_t now) {
        const std::string key = p.call + "#" + std::to_string(p.image);
        auto it = images_.find(key);
        if (it == images_.end()) {
            if ((int)images_.size() >= kMaxImages) {
                auto old = images_.begin();
                for (auto j = images_.begin(); j != images_.end(); ++j) if (j->second->lastAt < old->second->lastAt) old = j;
                images_.erase(old);
            }
            it = images_.emplace(key, std::unique_ptr<Image>(new Image())).first;
            it->second->firstAt = now;
        }
        Image* im = it->second.get();
        if (!im->add(p)) return nullptr;
        im->lastAt = now;
        return im;
    }
    const std::map<std::string, std::unique_ptr<Image>>& images() const { return images_; }
    void clear() { images_.clear(); }
private:
    std::map<std::string, std::unique_ptr<Image>> images_;
};

/** ★ A byte stream (8-bit RTTY) → packets: a 256-byte window that is tried whenever it starts 0x55 0x66 / 0x67.
 *  A packet found is taken whole (its bytes are not looked in again). */
class Framer {
public:
    std::function<void(const Packet&)> onPacket;
    unsigned long found = 0, failed = 0;
    void put(uint8_t c) {
        win_[(size_t)n_ % kPkt] = c; ++n_;
        if (skip_ > 0) { --skip_; return; }
        if (n_ < kPkt) return;
        const size_t s = (size_t)(n_ - kPkt) % kPkt;
        if (win_[s] != 0x55) return;
        const uint8_t t = win_[(s + 1) % kPkt];
        if (t != 0x66 && t != 0x67) return;
        uint8_t b[kPkt];
        for (int i = 0; i < kPkt; ++i) b[i] = win_[(s + (size_t)i) % kPkt];
        Packet p;
        if (parsePacket(b, p)) { ++found; skip_ = kPkt - 1; if (onPacket) onPacket(p); }
        else ++failed;
    }
private:
    uint8_t win_[kPkt] = {0};
    long long n_ = 0;
    int skip_ = 0;
};

// ── our encoder (tests and the bench clip) ─────────────────────────────────────────────────────────────────────────
/** ★ Quantised coefficient blocks (zig-zag order, [0] the DC as a level, not a difference), MCU by MCU — yc + 2 blocks
 *  each — → SSDV packets, as the format lays them out. */
inline std::vector<std::vector<uint8_t>> encodeImage(const std::string& call, int imageId, int w16, int h16, int mode, int quality,
                                                     const std::vector<std::vector<int16_t>>& blocks, bool fec = true) {
    const int yc = mode == 0 ? 4 : mode == 3 ? 1 : 2;
    const int pl = fec ? kPayloadFec : kPayloadNoFec;
    const int mcus = (int)blocks.size() / (yc + 2);
    std::vector<std::vector<uint8_t>> out;
    std::vector<uint8_t> bytes;              // the current packet's payload
    BitWriter w(bytes, false);
    int firstMcu = -1, offset = 0xFF, pid = 0;
    int pred[3] = { 0, 0, 0 };
    auto emit = [&](bool last) {
        std::vector<uint8_t> pkt((size_t)kPkt, 0);
        pkt[0] = 0x55; pkt[1] = fec ? 0x66 : 0x67;
        const uint32_t c = encodeCall(call);
        pkt[2] = (uint8_t)(c >> 24); pkt[3] = (uint8_t)(c >> 16); pkt[4] = (uint8_t)(c >> 8); pkt[5] = (uint8_t)c;
        pkt[6] = (uint8_t)imageId; pkt[7] = (uint8_t)(pid >> 8); pkt[8] = (uint8_t)pid;
        pkt[9] = (uint8_t)w16; pkt[10] = (uint8_t)h16;
        pkt[11] = (uint8_t)((((quality ^ 4) & 7) << 3) | (last ? 4 : 0) | (mode & 3));
        pkt[12] = (uint8_t)(firstMcu < 0 ? 0xFF : offset);
        pkt[13] = (uint8_t)(firstMcu < 0 ? 0xFF : firstMcu >> 8); pkt[14] = (uint8_t)(firstMcu < 0 ? 0xFF : firstMcu);
        for (int i = 0; i < pl; ++i) pkt[(size_t)(kHeader + i)] = i < (int)bytes.size() ? bytes[(size_t)i] : 0;
        const uint32_t crc = crc32(pkt.data() + 1, (size_t)(kHeader + pl - 1));
        for (int i = 0; i < 4; ++i) pkt[(size_t)(kHeader + pl + i)] = (uint8_t)(crc >> (24 - 8 * i));
        if (fec) { uint8_t cw[255]; toCw(pkt.data(), cw); Rs::get().encode(cw); fromCw(cw, pkt.data()); }
        out.push_back(pkt);
        ++pid; firstMcu = -1; offset = 0xFF;
    };
    // bits beyond a full payload move to the next packet
    auto spill = [&] {
        while ((int)bytes.size() >= pl) {
            std::vector<uint8_t> rest(bytes.begin() + pl, bytes.end());
            bytes.resize((size_t)pl);
            emit(false);
            bytes = rest;
        }
    };
    for (int m = 0; m < mcus; ++m) {
        bool reset = false;
        if (firstMcu < 0) {
            w.align(); spill();
            if ((int)bytes.size() >= pl) spill();
            firstMcu = m; offset = (int)bytes.size(); reset = true;
        }
        for (int part = 0; part < yc + 2; ++part) {
            const int comp = part < yc ? 0 : part - yc + 1;
            const std::vector<int16_t>& b = blocks[(size_t)(m * (yc + 2) + part)];
            const Huff& hd = huffTable(comp ? 1 : 0); const Huff& ha = huffTable(comp ? 3 : 2);
            const int base = (reset && (part == 0 || comp != 0)) ? 0 : pred[comp];
            const int d = b[0] - base; pred[comp] = b[0];
            int s; uint32_t bits; sizeBits(d, s, bits);
            w.put(hd.code[s], hd.size[s]); if (s) w.put(bits, s);
            int run = 0;
            for (int k = 1; k < 64; ++k) {
                if (b[(size_t)k] == 0) { ++run; continue; }
                while (run > 15) { w.put(ha.code[0xF0], ha.size[0xF0]); run -= 16; }
                sizeBits(b[(size_t)k], s, bits);
                const int rs = (run << 4) | s;
                w.put(ha.code[rs], ha.size[rs]); w.put(bits, s);
                run = 0;
            }
            if (run > 0) w.put(ha.code[0], ha.size[0]);
            spill();
        }
    }
    w.align();
    spill();
    emit(true);
    return out;
}

} // namespace ssdv
} // namespace balloon
} // namespace vibe
