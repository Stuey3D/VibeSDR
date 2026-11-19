// vibe_lrpt_image.h — METEOR-M MSU-MR imagery from LRPT packets (V12, 2026-11-19): the JPEG-like MCU decoder, the
// per-channel picture, the RGB composite and the gallery's PNGs.
//
// ★★★ SOURCES. ITU-T T.81 (JPEG) Annex K: the luminance DC and AC Huffman tables, the luminance quantisation table and
//     the zig-zag order; the IJG quality scaling (q < 50: 5000/q, else 200 − 2q). The LRPT packaging — 14 MCUs (8×8
//     blocks) a packet, 196 MCUs (1568 pixels) a line, the packet's 8-byte time, MCU number, 2-byte scan header and
//     3-byte segment header whose last byte is the quality factor, DC prediction restarting each packet, no byte
//     stuffing, APIDs 64–69 for the six MSU-MR channels, 43 packets a scan period (14 × 3 channels + 1 telemetry) —
//     are facts read from dbdexter's MIT-licensed meteor_decode; this code is ours.
//
// ★★ MEMORY (256 MB boards exist): a channel grows by 8-row strips (12.5 KB each) up to kMaxStrips (≈ 15 minutes of
//    pass, 10 MB a channel); at most kMaxChannels channels live at once. Six channels at the cap is the worst case
//    (60 MB) and never happens: METEOR sends three.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "vibe_png.h"
#include "decoders/auto_level.h"

namespace vibelrpt {

inline constexpr int kMcuPerPacket = 14;
inline constexpr int kMcuPerLine   = 196;
inline constexpr int kLineWidth    = kMcuPerLine * 8;    ///< 1568
inline constexpr int kPacketsPerPeriod = 43;
inline constexpr int kMaxStrips    = 800;                ///< 6400 rows ≈ 16 min of pass
inline constexpr int kMaxChannels  = 6;

// ── Huffman (T.81 Annex K.3, luminance) ─────────────────────────────────────────────────────────────────────────────
struct Huff {
    // Canonical decode: for code length L, codes in [minCode[L], maxCode[L]] map to vals[valPtr[L] + code − minCode[L]].
    int minCode[17] = {}, maxCode[17] = {}, valPtr[17] = {};
    std::vector<uint8_t> vals;
    Huff(const uint8_t* bits16, const uint8_t* v, int nv) : vals(v, v + nv) {
        int code = 0, k = 0;
        for (int L = 1; L <= 16; L++) {
            valPtr[L] = k; minCode[L] = code;
            code += bits16[L - 1]; k += bits16[L - 1];
            maxCode[L] = bits16[L - 1] ? code - 1 : -1;
            code <<= 1;
        }
    }
};
inline const Huff& dcTable() {
    static const uint8_t bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
    static const uint8_t v[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    static const Huff h(bits, v, 12);
    return h;
}
inline const Huff& acTable() {
    static const uint8_t bits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D};
    static const uint8_t v[162] = {
        0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
        0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
        0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
        0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
        0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
        0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
        0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
        0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
        0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
        0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
        0xF9, 0xFA};
    static const Huff h(bits, v, 162);
    return h;
}
inline const uint8_t* zigzag() {   // zig-zag index → natural (row-major) index
    static const uint8_t z[64] = {
        0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
    return z;
}
inline const uint8_t* stdQuant() {   // natural order
    static const uint8_t q[64] = {
        16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
        18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
    return q;
}

class BitReader {
public:
    BitReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    /** -1 past the end. */
    int bit() { if (pos_ >= n_ * 8) return -1; const int b = (p_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1; pos_++; return b; }
    int bits(int k, bool& ok) { int v = 0; for (int i = 0; i < k; i++) { const int b = bit(); if (b < 0) { ok = false; return 0; } v = (v << 1) | b; } return v; }
    int decode(const Huff& h) {
        int code = 0;
        for (int L = 1; L <= 16; L++) {
            const int b = bit(); if (b < 0) return -1;
            code = (code << 1) | b;
            if (h.maxCode[L] >= 0 && code <= h.maxCode[L] && code >= h.minCode[L]) return h.vals[size_t(h.valPtr[L] + code - h.minCode[L])];
        }
        return -2;                                           // not a code: corrupt
    }
private:
    const uint8_t* p_; size_t n_; size_t pos_ = 0;
};

inline int extend(int v, int s) { return (s && v < (1 << (s - 1))) ? v - (1 << s) + 1 : v; }

/** Float 8×8 inverse DCT (separable). ★ Not a hot loop: ≈ 500 blocks a second for a three-channel pass. */
inline void idct8x8(const float* in, uint8_t* out) {
    static const auto C = [] {
        std::array<float, 64> c{};
        for (int x = 0; x < 8; x++) for (int u = 0; u < 8; u++)
            c[size_t(x * 8 + u)] = float((u == 0 ? std::sqrt(0.125) : 0.5) * std::cos((2 * x + 1) * u * M_PI / 16));
        return c;
    }();
    float tmp[64];
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {       // rows: tmp[v][x] = Σ_u C[x][u] in[v][u]
        float s = 0; for (int u = 0; u < 8; u++) s += C[size_t(x * 8 + u)] * in[y * 8 + u];
        tmp[y * 8 + x] = s;
    }
    for (int x = 0; x < 8; x++) for (int y = 0; y < 8; y++) {
        float s = 0; for (int v = 0; v < 8; v++) s += C[size_t(y * 8 + v)] * tmp[v * 8 + x];
        const int p = int(std::lrint(s + 128.f));
        out[y * 8 + x] = uint8_t(p < 0 ? 0 : p > 255 ? 255 : p);
    }
}

/** Decode a packet's MCU data (`data`, `n` bytes after the 6-byte MCU header) into up to 14 blocks.
 *  Returns how many blocks decoded cleanly (the rest were cut short or corrupt and stay unset). */
inline int decodeMcus(const uint8_t* data, size_t n, int q, uint8_t blocks[kMcuPerPacket][64]) {
    if (q <= 0 || q > 100) return 0;
    int qt[64];
    const int scale = q < 50 ? 5000 / q : 200 - 2 * q;
    for (int i = 0; i < 64; i++) { const int v = (stdQuant()[i] * scale + 50) / 100; qt[i] = v < 1 ? 1 : v; }
    BitReader br(data, n);
    int dc = 0;
    for (int m = 0; m < kMcuPerPacket; m++) {
        float coef[64] = {};
        const int cat = br.decode(dcTable());
        if (cat < 0 || cat > 11) return m;
        bool ok = true;
        const int dv = br.bits(cat, ok);
        if (!ok) return m;
        dc += extend(dv, cat);
        coef[0] = float(dc * qt[0]);
        for (int k = 1; k < 64;) {
            const int rs = br.decode(acTable());
            if (rs < 0) return m;
            if (rs == 0) break;                               // EOB
            const int r = rs >> 4, s = rs & 15;
            if (s == 0) { if (r == 15) { k += 16; continue; } return m; }
            k += r;
            if (k > 63) return m;
            const int v = extend(br.bits(s, ok), s);
            if (!ok) return m;
            const int nat = zigzag()[k];
            coef[nat] = float(v * qt[nat]);
            k++;
        }
        idct8x8(coef, blocks[m]);
    }
    return kMcuPerPacket;
}

/** One MSU-MR channel as 8-row strips; strips may arrive out of order or be missing (black, never invented). */
class Channel {
public:
    explicit Channel(int apid = 0) : apid_(apid) {}
    int apid() const { return apid_; }
    int strips() const { return strips_; }
    int rows() const { return strips_ * 8; }
    /** Put 14 blocks at strip `s`, MCU column `mcu` (0, 14, … 182). `nGood` blocks are valid. Returns false if out of
     *  range (the cap, or a wild strip number from a bad sequence count). */
    bool put(int s, int mcu, const uint8_t blocks[kMcuPerPacket][64], int nGood) {
        if (s < 0 || s >= kMaxStrips || mcu < 0 || mcu + kMcuPerPacket > kMcuPerLine) return false;
        if (s >= strips_) { px_.resize(size_t(s + 1) * 8 * kLineWidth, 0); have_.resize(size_t(s + 1) * kMcuPerLine, 0); strips_ = s + 1; }
        for (int b = 0; b < nGood; b++)
            for (int r = 0; r < 8; r++)
                std::memcpy(&px_[(size_t(s) * 8 + size_t(r)) * kLineWidth + size_t(mcu + b) * 8], &blocks[b][r * 8], 8);
        for (int b = 0; b < nGood; b++) have_[size_t(s) * kMcuPerLine + size_t(mcu + b)] = 1;
        good_ += nGood;
        return true;
    }
    const uint8_t* pixels() const { return px_.data(); }
    uint64_t goodBlocks() const { return good_; }
    bool has(int s, int mcuCol) const { return s >= 0 && s < strips_ && have_[size_t(s) * kMcuPerLine + size_t(mcuCol)]; }
    /** The 2nd…98th percentile over the decoded blocks only (black gaps are not picture). */
    void levels(int& lo, int& hi) const {
        uint32_t hist[256] = {}; uint64_t n = 0;
        for (int s = 0; s < strips_; s++) for (int c = 0; c < kMcuPerLine; c++) {
            if (!has(s, c)) continue;
            for (int r = 0; r < 8; r++) for (int x = 0; x < 8; x++) { hist[px_[(size_t(s) * 8 + size_t(r)) * kLineWidth + size_t(c) * 8 + size_t(x)]]++; n++; }
        }
        if (!n) { lo = 0; hi = 255; return; }
        vibe::autoLevelRange(hist, n, lo, hi);
    }
private:
    int apid_;
    int strips_ = 0;
    std::vector<uint8_t> px_, have_;
    uint64_t good_ = 0;
};

/** ★ The picture's rows from `firstStrip` (trimmed of empty strips at both ends), auto-levelled, `invert` for the
 *  thermal channels (cloud white, as people expect). Greyscale bytes w×h. */
inline std::vector<uint8_t> channelImage(const Channel& ch, int s0, int s1, bool invert, int& w, int& h) {
    w = kLineWidth; h = (s1 - s0) * 8;
    std::vector<uint8_t> out;
    if (h <= 0) { h = 0; return out; }
    int lo, hi; ch.levels(lo, hi);
    uint8_t lut[256];
    for (int v = 0; v < 256; v++) {
        int o = (v - lo) * 255 / (hi - lo); o = o < 0 ? 0 : o > 255 ? 255 : o;
        lut[v] = uint8_t(invert ? 255 - o : o);
    }
    out.resize(size_t(w) * size_t(h));
    for (int s = s0; s < s1; s++) for (int r = 0; r < 8; r++) {
        uint8_t* d = &out[(size_t(s - s0) * 8 + size_t(r)) * size_t(w)];
        if (s >= ch.strips()) { std::memset(d, 0, size_t(w)); continue; }
        const uint8_t* p = ch.pixels() + (size_t(s) * 8 + size_t(r)) * kLineWidth;
        for (int c = 0; c < kMcuPerLine; c++) {
            if (!ch.has(s, c)) { std::memset(d + c * 8, 0, 8); continue; }
            for (int x = 0; x < 8; x++) d[c * 8 + x] = lut[p[c * 8 + x]];
        }
    }
    return out;
}

} // namespace vibelrpt
