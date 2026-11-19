// vibe_il2p.h — V12: IL2P, the Improved Layer 2 Protocol (Reed-Solomon, no bit-stuffing, packet-synchronous
// scrambling), received on the same bit streams as AX.25. docs/v12/AX25-9600-FX25-IL2P.md.
//
// ★★★ FROM THE PUBLISHED SPECIFICATION, our own code: "IL2P Specification Draft v0.6", Nino Carrillo KK4HEJ, 16 March
//     2024 (tarpn.net/t/il2p). What it fixes, all of it used here:
//       - sync word 0xF15E48, "receiver will declare a match if 23 out of the last 24 bits received match"; all bytes
//         Most Significant Bit first; no NRZI ("Differential encoding is not used"), so "the decoder should check for
//         inverted data" — the inverted sync word starts an inverted packet;
//       - header: 13 bytes of control and addressing + 2 RS check bytes; payload: blocks of near-equal size (large blocks
//         first) each followed by its check bytes; the optional trailing CRC (CRC-16 of the AX.25 frame, four
//         Hamming(7,4) bytes, high nibble first, the spec's encode/decode tables);
//       - RS over 0x11D, first root 0; scrambling x^9+x^4+1, reset at the start of the header and of every payload block,
//         applied BEFORE the RS encoder (so the RS decoder corrects the bytes before they meet the descrambler); the
//         LFSRs are the spec's two schematics with their initial states (receive: 1 1 1 1 1 | 0 0 0 0);
//       - Type 1 header map (DEC SIXBIT callsigns, SSIDs in byte 12, UI, 4-bit PID, 7-bit control, header type, 10-bit
//         payload count); Type 0 = the whole AX.25 frame in the payload; the PID and I/S/U control tables.
//     The three example packets at the end of the spec are this file's test vectors (test-ax25hs §IL2P): each decodes
//     to the spec's AX.25 bytes, and our encoder reproduces the spec's IL2P bytes exactly.
//     ★ Two facts the v0.6 text does not settle were taken from public descriptions of what transmitters send:
//       (1) the C bit of a translated S or U control is bit 2 (S: P/F·N(R)·C·opcode; U: P/F·opcode·C — the spec's
//           example S-frame header decodes to exactly that), and the reconstructed AX.25 frame carries it as a command
//           (destination C = 1, source C = 0) or a response (the reverse), I-frames always commands;
//       (2) before v0.6 the header's byte-0 bit 7 chose "max FEC" (16 check bytes a block, 1) or the baseline (0):
//           blocks of up to 247 bytes with 2/4/6/8 check bytes for a small block of ≤ 61/123/185/247 bytes. v0.6 always
//           sends 16 and calls the bit reserved (its own I-frame example has the bit at 0 and 16 check bytes), so the
//           receiver tries the layout the bit names first and then the other one. A wrong layout cannot pass: every
//           block's RS decode would have to succeed on bytes that are not its own.
//
// ★★★ NO FALSE FRAMES ON NOISE. A 23-of-24 sync match on noise comes about every 69 s per bit stream at 9600 bit/s (the
//     spec's own figure), and a 2-check-byte header decodes ~6 % of random blocks. So, beyond the spec: a Type 1
//     header's callsigns must be capitals/digits, left-justified and non-empty; a Type 0 header must be all zero but its
//     count; and a frame WITHOUT a verified trailing CRC is accepted only when its header needed no correction, or its
//     payload exists and every block was decoded with at most nroots/2 − 1 corrections and at least 8 check bytes
//     (a decode at the very limit is the miscorrection the spec's CRC section describes). With the CRC verified,
//     everything the RS codes can fix is accepted. (Exactly: header clean and every block either clean or "strong" —
//     ≥ 8 check bytes and short of the limit — or header corrected once and every block strong.)
#pragma once
#include "vibe_hdlc.h"
#include "vibe_rs8.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace vibeil2p {

constexpr uint32_t kSync = 0xF15E48;

inline int popcount32(uint32_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcount(x);
#else
    int c = 0; while (x) { x &= x - 1; ++c; } return c;
#endif
}
inline const vibers::Rs& codec(int nroots) {
    static const vibers::Rs r2(2, 0), r4(4, 0), r6(6, 0), r8(8, 0), r16(16, 0);
    switch (nroots) { case 2: return r2; case 4: return r4; case 6: return r6; case 8: return r8; default: return r16; }
}

/** The transmit LFSR of the spec's schematic (Galois, x^9+x^4+1), bytes MSB first; its 5-bit delay is taken up and
 *  flushed, so n bytes in = n bytes out. */
inline void scramble(uint8_t* d, int n) {
    int c[10] = { 0, 0, 0, 0, 0, 0, 1, 1, 1, 1 };   // c[1..5] = 0, c[6..9] = 1
    std::vector<uint8_t> out(size_t(n), 0);
    const int total = n * 8 + 5;
    for (int i = 0; i < total; ++i) {
        const int b = i < n * 8 ? (d[i >> 3] >> (7 - (i & 7))) & 1 : 0;
        const int fb = c[9], x = c[5] ^ fb;
        c[9] = c[8]; c[8] = c[7]; c[7] = c[6]; c[6] = x; c[5] = c[4]; c[4] = c[3]; c[3] = c[2]; c[2] = c[1]; c[1] = b ^ fb;
        const int o = i - 5;
        if (o >= 0) out[size_t(o >> 3)] = uint8_t(out[size_t(o >> 3)] | (x << (7 - (o & 7))));
    }
    for (int i = 0; i < n; ++i) d[i] = out[size_t(i)];
}
/** The receive LFSR of the spec's schematic, bytes MSB first, in place. */
inline void descramble(uint8_t* d, int n) {
    int c[10] = { 0, 1, 1, 1, 1, 1, 0, 0, 0, 0 };   // c[1..5] = 1, c[6..9] = 0
    for (int i = 0; i < n; ++i) {
        uint8_t ob = 0;
        for (int k = 7; k >= 0; --k) {
            const int b = (d[i] >> k) & 1, o = c[9] ^ b, x = c[5] ^ b;
            c[9] = c[8]; c[8] = c[7]; c[7] = c[6]; c[6] = x; c[5] = c[4]; c[4] = c[3]; c[3] = c[2]; c[2] = c[1]; c[1] = b;
            ob = uint8_t((ob << 1) | o);
        }
        d[i] = ob;
    }
}

/** The spec's Hamming(7,4) tables for the trailing CRC. */
inline uint8_t hamEnc(int nib) {
    static const uint8_t t[16] = { 0x00, 0x71, 0x62, 0x13, 0x54, 0x25, 0x36, 0x47, 0x38, 0x49, 0x5a, 0x2b, 0x6c, 0x1d, 0x0e, 0x7f };
    return t[nib & 15];
}
inline int hamDec(uint8_t cw) {
    static const uint8_t t[128] = {
        0x0, 0x0, 0x0, 0x3, 0x0, 0x5, 0xe, 0x7, 0x0, 0x9, 0xe, 0xb, 0xe, 0xd, 0xe, 0xe,
        0x0, 0x3, 0x3, 0x3, 0x4, 0xd, 0x6, 0x3, 0x8, 0xd, 0xa, 0x3, 0xd, 0xd, 0xe, 0xd,
        0x0, 0x5, 0x2, 0xb, 0x5, 0x5, 0x6, 0x5, 0x8, 0xb, 0xb, 0xb, 0xc, 0x5, 0xe, 0xb,
        0x8, 0x1, 0x6, 0x3, 0x6, 0x5, 0x6, 0x6, 0x8, 0x8, 0x8, 0xb, 0x8, 0xd, 0x6, 0xf,
        0x0, 0x9, 0x2, 0x7, 0x4, 0x7, 0x7, 0x7, 0x9, 0x9, 0xa, 0x9, 0xc, 0x9, 0xe, 0x7,
        0x4, 0x1, 0xa, 0x3, 0x4, 0x4, 0x4, 0x7, 0xa, 0x9, 0xa, 0xa, 0x4, 0xd, 0xa, 0xf,
        0x2, 0x1, 0x2, 0x2, 0xc, 0x5, 0x2, 0x7, 0xc, 0x9, 0x2, 0xb, 0xc, 0xc, 0xc, 0xf,
        0x1, 0x1, 0x2, 0x1, 0x4, 0x1, 0x6, 0xf, 0x8, 0x1, 0xa, 0xf, 0xc, 0xf, 0xf, 0xf };
    return t[cw & 0x7F];
}

/** Payload block layout for `count` bytes: max FEC (16 check bytes, blocks ≤ 239) or the pre-v0.6 baseline. */
struct Layout { int blocks = 0, small = 0, large = 0, nLarge = 0, nSmall = 0, parity = 0; int bytes() const { return nLarge * (large + parity) + nSmall * (small + parity); } };
inline Layout layout(int count, bool maxFec) {
    Layout L;
    if (count <= 0) return L;
    L.blocks = maxFec ? (count + 238) / 239 : (count + 246) / 247;
    L.small = count / L.blocks; L.large = L.small + 1;
    L.nLarge = count - L.blocks * L.small; L.nSmall = L.blocks - L.nLarge;
    L.parity = maxFec ? 16 : L.small <= 61 ? 2 : L.small <= 123 ? 4 : L.small <= 185 ? 6 : 8;
    return L;
}

// ── the AX.25 ↔ IL2P header translation ─────────────────────────────────────────────────────────────────────────────
inline int pidToIl2p(int pid) {
    if ((pid & 0x30) == 0x20 || (pid & 0x30) == 0x10) return 0x2;
    switch (pid) { case 0x01: return 0x3; case 0x06: return 0x4; case 0x07: return 0x5; case 0x08: return 0x6;
                   case 0xCC: return 0xB; case 0xCD: return 0xC; case 0xCE: return 0xD; case 0xCF: return 0xE; case 0xF0: return 0xF; }
    return -1;
}
inline int pidFromIl2p(int p) {
    static const uint8_t t[16] = { 0xF0, 0xF0, 0x20, 0x01, 0x06, 0x07, 0x08, 0xF0, 0xF0, 0xF0, 0xF0, 0xCC, 0xCD, 0xCE, 0xCF, 0xF0 };
    return t[p & 15];
}
/** U-frame opcode (IL2P 3 bits) ↔ AX.25 control without its P/F bit. */
inline int uOpFromAx(int c) { switch (c & ~0x10) { case 0x2F: return 0; case 0x43: return 1; case 0x0F: return 2; case 0x63: return 3;
                                                    case 0x87: return 4; case 0x03: return 5; case 0xAF: return 6; case 0xE3: return 7; } return -1; }
inline int uAxFromOp(int op) { static const uint8_t t[8] = { 0x2F, 0x43, 0x0F, 0x63, 0x87, 0x03, 0xAF, 0xE3 }; return t[op & 7]; }

inline bool sixbitCallOk(const char* c) {          // 6 chars: capitals/digits, left-justified, non-empty
    if (c[0] == ' ') return false;
    bool sp = false;
    for (int i = 0; i < 6; ++i) {
        if (c[i] == ' ') { sp = true; continue; }
        if (sp) return false;
        if (!((c[i] >= 'A' && c[i] <= 'Z') || (c[i] >= '0' && c[i] <= '9'))) return false;
    }
    return true;
}

/** A descrambled 13-byte header → what it says. `axHead` = the reconstructed AX.25 address + control (+ PID) bytes
 *  for a Type 1 header. False = not a header we believe. */
struct Header { int type = 0, count = 0; bool maxFecBit = false; std::vector<uint8_t> axHead; };
inline bool parseHeader(const uint8_t* h, Header& o) {
    o.type = (h[1] >> 7) & 1;
    o.maxFecBit = (h[0] >> 7) & 1;
    o.count = 0;
    for (int i = 2; i <= 11; ++i) o.count = (o.count << 1) | ((h[i] >> 7) & 1);
    o.axHead.clear();
    if (o.type == 0) {
        // ★ beyond the spec: a Type 0 header carries nothing but the count — anything else is noise
        for (int i = 0; i <= 11; ++i) if (h[i] & 0x7F) return false;
        if (h[12]) return false;
        return o.count > 0;
    }
    char dst[6], src[6];
    for (int i = 0; i < 6; ++i) { dst[i] = char((h[i] & 0x3F) + 0x20); src[i] = char((h[6 + i] & 0x3F) + 0x20); }
    if (!sixbitCallOk(dst) || !sixbitCallOk(src)) return false;
    const int ui = (h[0] >> 6) & 1;
    int pid = 0, ctl = 0;
    for (int i = 1; i <= 4; ++i) pid = (pid << 1) | ((h[i] >> 6) & 1);
    for (int i = 5; i <= 11; ++i) ctl = (ctl << 1) | ((h[i] >> 6) & 1);
    const int dSsid = h[12] >> 4, sSsid = h[12] & 15;
    const int pf = (ctl >> 6) & 1;
    int axCtl = 0, axPid = -1; bool cmd = true;
    if (pid == 0) {                                  // S frame
        cmd = (ctl >> 2) & 1;
        axCtl = (((ctl >> 3) & 7) << 5) | (pf << 4) | ((ctl & 3) << 2) | 0x01;
    } else if (pid == 1) {                           // U frame other than UI
        cmd = (ctl >> 2) & 1;
        const int op = (ctl >> 3) & 7;
        if (op == 5) return false;                   // a UI frame is signalled by the UI bit, never by PID 1
        axCtl = uAxFromOp(op) | (pf << 4);
    } else if (ui) {                                 // UI frame
        cmd = (ctl >> 2) & 1;
        if (((ctl >> 3) & 7) != 5) return false;
        axCtl = 0x03 | (pf << 4); axPid = pidFromIl2p(pid);
    } else {                                         // I frame — always a command
        axCtl = (((ctl >> 3) & 7) << 5) | (pf << 4) | ((ctl & 7) << 1); axPid = pidFromIl2p(pid);
    }
    for (int i = 0; i < 6; ++i) o.axHead.push_back(uint8_t(dst[i] << 1));
    o.axHead.push_back(uint8_t((cmd ? 0x80 : 0) | 0x60 | (dSsid << 1)));
    for (int i = 0; i < 6; ++i) o.axHead.push_back(uint8_t(src[i] << 1));
    o.axHead.push_back(uint8_t((cmd ? 0 : 0x80) | 0x60 | (sSsid << 1) | 1));
    o.axHead.push_back(uint8_t(axCtl));
    if (axPid >= 0) o.axHead.push_back(uint8_t(axPid));
    return true;
}

/** ★ For the test generators and the spec vectors: an AX.25 frame (bytes WITHOUT the FCS) → IL2P bytes (no sync word).
 *  Type 1 when the frame translates, else Type 0. `maxFec`: 16 check bytes a block (false = the pre-v0.6 baseline).
 *  `fecBit`: byte-0 bit 7 — −1 = as `maxFec` (what pre-v0.6 transmitters send), 0 = v0.6's "reserved" (the spec's
 *  own examples). */
inline std::vector<uint8_t> encode(const std::vector<uint8_t>& ax, bool maxFec = true, bool crc = true, int fecBit = -1) {
    uint8_t h[15] = { 0 };
    std::vector<uint8_t> payload;
    bool type1 = false;
    // Type 1: exactly two addresses, a control we can map, a PID we can map, callsigns of capitals/digits
    if (ax.size() >= 15 && (ax[13] & 1)) {
        char dst[6], src[6]; bool callsOk = true;
        for (int i = 0; i < 6; ++i) { dst[i] = char(ax[size_t(i)] >> 1); src[i] = char(ax[size_t(7 + i)] >> 1); if ((ax[size_t(i)] & 1) || (ax[size_t(7 + i)] & 1)) callsOk = false; }
        callsOk = callsOk && sixbitCallOk(dst) && sixbitCallOk(src);
        const int c = ax[14];
        const bool cmd = (ax[6] & 0x80) != 0;
        const int pf = (c >> 4) & 1;
        int ui = 0, pid = -1, ctl = -1; size_t info = 15;
        if ((c & 1) == 0) {                                   // I
            if (ax.size() >= 16) { pid = pidToIl2p(ax[15]); ctl = (pf << 6) | (((c >> 5) & 7) << 3) | ((c >> 1) & 7); info = 16; }
        } else if ((c & 3) == 1) {                            // S
            pid = 0; ctl = (pf << 6) | (((c >> 5) & 7) << 3) | ((cmd ? 1 : 0) << 2) | ((c >> 2) & 3);
        } else {                                              // U
            const int op = uOpFromAx(c);
            if (op == 5) { if (ax.size() >= 16) { ui = 1; pid = pidToIl2p(ax[15]); ctl = (pf << 6) | (5 << 3) | ((cmd ? 1 : 0) << 2); info = 16; } }
            else if (op >= 0) { pid = 1; ctl = (pf << 6) | (op << 3) | ((cmd ? 1 : 0) << 2); }
        }
        if (callsOk && pid >= 0 && ctl >= 0 && ax.size() - info <= 1023) {
            type1 = true;
            for (int i = 0; i < 6; ++i) { h[i] = uint8_t(dst[i] - 0x20); h[6 + i] = uint8_t(src[i] - 0x20); }
            h[12] = uint8_t((((ax[6] >> 1) & 15) << 4) | ((ax[13] >> 1) & 15));
            h[0] = uint8_t(h[0] | (ui << 6));
            for (int i = 0; i < 4; ++i) h[1 + i] = uint8_t(h[1 + i] | (((pid >> (3 - i)) & 1) << 6));
            for (int i = 0; i < 7; ++i) h[5 + i] = uint8_t(h[5 + i] | (((ctl >> (6 - i)) & 1) << 6));
            payload.assign(ax.begin() + long(info), ax.end());
        }
    }
    if (!type1) payload = ax;
    const int count = int(payload.size());
    if (count > 1023) return {};
    if (type1) h[1] = uint8_t(h[1] | 0x80);
    if (fecBit < 0 ? maxFec : fecBit != 0) h[0] = uint8_t(h[0] | 0x80);
    for (int i = 0; i < 10; ++i) h[2 + i] = uint8_t(h[2 + i] | (((count >> (9 - i)) & 1) << 7));
    scramble(h, 13);
    codec(2).encode(h, 13, h + 13);
    std::vector<uint8_t> out(h, h + 15);
    const Layout L = layout(count, maxFec);
    size_t p = 0;
    for (int b = 0; b < L.blocks; ++b) {
        const int sz = b < L.nLarge ? L.large : L.small;
        std::vector<uint8_t> blk(payload.begin() + long(p), payload.begin() + long(p) + sz); p += size_t(sz);
        scramble(blk.data(), sz);
        blk.resize(size_t(sz + L.parity));
        codec(L.parity).encode(blk.data(), sz, blk.data() + sz);
        out.insert(out.end(), blk.begin(), blk.end());
    }
    if (crc) {
        const uint16_t f = vibehdlc::fcs16(ax.data(), ax.size());
        for (int i = 3; i >= 0; --i) out.push_back(hamEnc((f >> (4 * i)) & 15));
    }
    return out;
}
/** IL2P bytes → line bits for the modulator: `pre` bytes of 0x55 preamble, the sync word, the bytes, MSB first. */
inline std::vector<int> lineBits(const std::vector<uint8_t>& il2p, int pre = 8, bool invert = false) {
    std::vector<int> o;
    for (int i = 0; i < pre * 8; ++i) o.push_back((i & 1) ^ 1);
    for (int i = 23; i >= 0; --i) o.push_back(int((kSync >> i) & 1));
    for (uint8_t b : il2p) for (int i = 7; i >= 0; --i) o.push_back((b >> i) & 1);
    for (int i = 0; i < 48; ++i) o.push_back(i & 1);   // a tail: the receiver reads on for the optional CRC
    if (invert) for (int& b : o) b ^= 1;
    return o;
}

struct Result { std::vector<uint8_t> bytes; int fixed = 0; bool crc = false; bool inverted = false; };

/** Try one payload layout on the collected bytes. */
inline bool tryLayout(const std::vector<uint8_t>& buf, const Header& H, const Layout& L, int hdrFix, Result& r) {
    std::vector<uint8_t> payload; payload.reserve(size_t(H.count));
    size_t p = 0; int fixes = 0; bool allStrongOrClean = true;
    for (int b = 0; b < L.blocks; ++b) {
        const int sz = b < L.nLarge ? L.large : L.small;
        if (p + size_t(sz + L.parity) > buf.size()) return false;
        std::vector<uint8_t> blk(buf.begin() + long(p), buf.begin() + long(p) + sz + L.parity);
        const int f = codec(L.parity).decode(blk.data(), sz + L.parity);
        if (f < 0) return false;
        // strong = at least 8 check bytes and short of the code's limit; clean = no correction at all
        const bool strong = L.parity >= 8 && f <= L.parity / 2 - 1;
        allStrongOrClean = allStrongOrClean && (strong || f == 0);
        fixes += f;
        descramble(blk.data(), sz);
        payload.insert(payload.end(), blk.begin(), blk.begin() + sz);
        p += size_t(sz + L.parity);
    }
    std::vector<uint8_t> ax = H.type == 1 ? H.axHead : std::vector<uint8_t>();
    ax.insert(ax.end(), payload.begin(), payload.end());
    // a Type 0 payload must at least look like AX.25 addressing (two+ address fields ending in the extension bit)
    if (ax.size() < 15) return false;
    // the optional trailing CRC. ★ PRESENT = all four bytes are exact Hamming codewords with the top bit clear — four
    //   random bytes are that one time in 65 536 — and a present CRC must MATCH (a mismatch is an error we were told
    //   about: the frame is refused, whatever the RS codes said).
    bool crcOk = false, crcPresent = false;
    if (p + 4 <= buf.size()) {
        crcPresent = true;
        int c = 0;
        for (int i = 0; i < 4; ++i) {
            const uint8_t b = buf[p + size_t(i)];
            const int nib = hamDec(b);
            if ((b & 0x80) || hamEnc(nib) != b) crcPresent = false;
            c = (c << 4) | nib;
        }
        crcOk = uint16_t(c) == vibehdlc::fcs16(ax.data(), ax.size());
        if (crcPresent && !crcOk) return false;
    }
    if (!crcOk) {
        // ★ the no-false-frames rule (see the top of the file). A CORRECTED header is never trusted without the CRC: two
        //   wrong header bytes are "corrected" into another valid header about one time in seventeen, and the payload's
        //   own strength says nothing about the header (found by test-ax25hs --full: a GRBAlpha frame at 9 dB came out
        //   as from "OM9G0R").
        if (!(hdrFix == 0 && allStrongOrClean)) return false;
    }
    r.bytes.swap(ax); r.fixed = hdrFix + fixes; r.crc = crcOk;
    return true;
}

/** The receiver, one per bit stream: feed every LINE bit (no NRZI; for 9600 the bits BEFORE the G3RUH descrambler —
 *  IL2P brings its own scrambler and does not use G3RUH's). */
class Rx {
public:
    template <class Out> void bit(int b, Out&& out) {
        b &= 1;
        if (st_ == 0) {
            acc_ = ((acc_ << 1) | uint32_t(b)) & 0xFFFFFF;
            if (++have_ < 24) return;
            if (popcount32(acc_ ^ kSync) <= 1) inv_ = 0;
            else if (popcount32((~acc_ & 0xFFFFFF) ^ kSync) <= 1) inv_ = 1;
            else return;
            st_ = 1; buf_.clear(); cur_ = 0; nb_ = 0; need_ = 15; ++syncs_;
            return;
        }
        cur_ = uint8_t((cur_ << 1) | (b ^ inv_));
        if (++nb_ < 8) return;
        buf_.push_back(cur_); cur_ = 0; nb_ = 0;
        if (int(buf_.size()) < need_) return;
        if (st_ == 1) {
            uint8_t h[15]; for (int i = 0; i < 15; ++i) h[i] = buf_[size_t(i)];
            hdrFix_ = codec(2).decode(h, 15);
            if (hdrFix_ < 0) { finish_(0, nullptr, out); return; }
            descramble(h, 13);
            if (!parseHeader(h, H_)) { finish_(0, nullptr, out); return; }
            P_ = layout(H_.count, H_.maxFecBit); A_ = layout(H_.count, !H_.maxFecBit);
            triedP_ = false;
            need_ = 15 + P_.bytes() + 4;
            st_ = 2; ++headers_;
            return;
        }
        // st_ == 2: first the layout the header's bit names (with room for the CRC); if that fails, the other one
        std::vector<uint8_t> pay(buf_.begin() + 15, buf_.end());
        Result r; r.inverted = inv_ != 0;
        if (!triedP_) {
            if (tryLayout(pay, H_, P_, hdrFix_, r)) { finish_(15 + P_.bytes() + (r.crc ? 4 : 0), &r, out); return; }
            triedP_ = true;
            if (H_.count == 0) { finish_(0, nullptr, out); return; }
            if (int(pay.size()) < A_.bytes() + 4) { need_ = 15 + A_.bytes() + 4; return; }
        }
        if (tryLayout(pay, H_, A_, hdrFix_, r)) finish_(15 + A_.bytes() + (r.crc ? 4 : 0), &r, out);
        else finish_(0, nullptr, out);
    }
    bool busy() const { return st_ != 0; }
    uint64_t syncs() const { return syncs_; }
    uint64_t headers() const { return headers_; }
    uint64_t decoded() const { return ok_; }
private:
    void reset_() { st_ = 0; have_ = 0; acc_ = 0; buf_.clear(); }
    /** ★ A decision is made: hand over the frame (if any), then give the bytes collected beyond what the packet used
     *  back to the sync search — the next packet of a back-to-back burst has no preamble, and its sync word may already
     *  be in our buffer (we read on for the CRC, or for the longer layout). After a failure, everything after the sync
     *  word goes back (the sync was probably noise). */
    template <class Out> void finish_(size_t used, const Result* r, Out& out) {
        std::vector<uint8_t> rest;
        const size_t from = r ? used : 0;
        if (from < buf_.size()) rest.assign(buf_.begin() + long(from), buf_.end());
        const int inv = inv_, pbits = nb_; const uint8_t pcur = cur_;
        reset_();
        if (r) { ++ok_; out(*r); }
        // replay, in line-bit terms (undo our own inversion)
        for (uint8_t v : rest) for (int k = 7; k >= 0; --k) bit(((v >> k) & 1) ^ inv, out);
        for (int k = pbits - 1; k >= 0; --k) bit(((pcur >> k) & 1) ^ inv, out);
    }
    int st_ = 0, have_ = 0, inv_ = 0, nb_ = 0, need_ = 15, hdrFix_ = 0;
    uint32_t acc_ = 0; uint8_t cur_ = 0;
    std::vector<uint8_t> buf_;
    Header H_;
    Layout P_, A_;
    bool triedP_ = false;
    uint64_t syncs_ = 0, headers_ = 0, ok_ = 0;
};

} // namespace vibeil2p
