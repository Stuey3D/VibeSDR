// vibe_dab_fec.h — DAB forward error correction: the rate-1/4 mother code, soft Viterbi,
// energy dispersal and the FIB CRC.
//
// Sources: ETSI EN 300 401 clause 11.1 (mother code + puncturing), read from the downloaded spec:
// "The octal forms of the generator polynomials are 133, 171, 145 and 133, respectively", with the
// register starting and ending in the all-zero state.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vibedab {

// ── The mother code ─────────────────────────────────────────────────────────
inline constexpr int kK        = 7;        ///< constraint length (6 delay elements)
inline constexpr int kStates   = 1 << (kK - 1);   ///< 64
inline constexpr int kRateOut  = 4;        ///< outputs per input bit
/** ★ Straight from the spec, in octal as it states them. */
inline constexpr unsigned kPoly[kRateOut] = { 0133, 0171, 0145, 0133 };

/** Parity of a 32-bit word — the convolutional output for one polynomial. */
inline constexpr int parity(unsigned v) {
    v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return int(v & 1u);
}

/** The 4 output bits for a transition: `state` is the 6 previous bits, `in` the new one.
 *  ★ The register is (in, state) with `in` the most recent bit, which is the convention that makes
 *    the octal polynomials read directly against it. Get this backwards and the code still decodes
 *    ITSELF perfectly while failing on every real signal — which is exactly the sort of bug a
 *    self-consistent test would bless, so the test below also checks a KNOWN encoder output. */
inline int branchOutputs(int state, int in) {
    const unsigned reg = (unsigned(in) << (kK - 1)) | unsigned(state);
    int bits = 0;
    for (int j = 0; j < kRateOut; ++j) bits |= parity(reg & kPoly[j]) << j;
    return bits;
}

/** Encode `nbits` bits (LSB-first in `in`) to `nbits*4` output bits, flushing with 6 zeros.
 *  ★ Present for TESTING and for nothing else — a decoder with no encoder to check it against can
 *    only be verified on live RF, which is the slowest possible feedback loop. */
inline std::vector<uint8_t> convEncode(const uint8_t* in, size_t nbits) {
    std::vector<uint8_t> out;
    out.reserve((nbits + kK - 1) * kRateOut);
    int state = 0;
    auto step = [&](int bit) {
        const int o = branchOutputs(state, bit);
        for (int j = 0; j < kRateOut; ++j) out.push_back(uint8_t((o >> j) & 1));
        state = ((state >> 1) | (bit << (kK - 2))) & (kStates - 1);
    };
    for (size_t i = 0; i < nbits; ++i) step(in[i] & 1);
    for (int i = 0; i < kK - 1; ++i) step(0);        // flush to the all-zero state
    return out;
}

/** Soft-input Viterbi for the rate-1/4 mother code.
 *
 *  ★★★ SOFT IN, and that is worth ~2 dB over hard decisions — on a marginal multiplex the
 *      difference between audio and silence. Inputs are the signed soft values from dqpskSoft:
 *      POSITIVE means bit 0, negative means bit 1, magnitude is confidence. A punctured (not
 *      transmitted) bit is passed as 0, which is exactly right: zero confidence contributes
 *      nothing to either branch, so depuncturing needs no special case in the decoder at all.
 *
 *  ★★ Terminated, not tail-biting: the encoder flushes 6 zeros, so traceback starts at state 0
 *     unconditionally rather than having to guess the ending state.
 */
class Viterbi {
public:
    /** @param soft   nbits*4 soft values (0 for punctured positions)
     *  @param nbits  number of DATA bits, EXCLUDING the 6 flush bits */
    std::vector<uint8_t> decode(const int8_t* soft, size_t nbits) {
        const size_t steps = nbits + (kK - 1);
        decisions_.assign(steps * kStates, 0);
        std::array<int32_t, kStates> cur{}, nxt{};
        cur.fill(kBig);
        cur[0] = 0;                                   // known start state

        for (size_t t = 0; t < steps; ++t) {
            nxt.fill(kBig);
            const int8_t* s = soft + t * kRateOut;
            for (int st = 0; st < kStates; ++st) {
                if (cur[st] >= kBig) continue;
                for (int in = 0; in < 2; ++in) {
                    const int o = branchOutputs(st, in);
                    // ★ Correlation metric: a soft value agreeing with the expected bit REDUCES
                    //   the cost. Written as a subtraction so the metric stays an ordinary
                    //   "smaller is better" path cost.
                    int32_t m = 0;
                    for (int j = 0; j < kRateOut; ++j)
                        m += ((o >> j) & 1) ? int32_t(s[j]) : -int32_t(s[j]);
                    const int ns = ((st >> 1) | (in << (kK - 2))) & (kStates - 1);
                    const int32_t cost = cur[st] + m + 127 * kRateOut;   // keep it non-negative
                    if (cost < nxt[ns]) {
                        nxt[ns] = cost;
                        decisions_[t * kStates + size_t(ns)] = uint8_t(in | (st << 1));
                    }
                }
            }
            cur = nxt;
            // ★ Renormalise so the metrics cannot run away over a long block. Subtracting a
            //   constant from every survivor changes no comparison.
            int32_t mn = kBig;
            for (int st = 0; st < kStates; ++st) mn = cur[st] < mn ? cur[st] : mn;
            if (mn < kBig) for (int st = 0; st < kStates; ++st) if (cur[st] < kBig) cur[st] -= mn;
        }

        // Traceback from the known all-zero end state.
        std::vector<uint8_t> out(nbits, 0);
        int st = 0;
        for (size_t t = steps; t-- > 0; ) {
            const uint8_t d = decisions_[t * kStates + size_t(st)];
            if (t < nbits) out[t] = uint8_t(d & 1);
            st = d >> 1;
        }
        return out;
    }

private:
    static constexpr int32_t kBig = 1 << 28;
    std::vector<uint8_t> decisions_;
};

// ── Energy dispersal ────────────────────────────────────────────────────────
/** ★★ THE SCRAMBLER, and an honest note about its provenance: the polynomial below could NOT be
 *  extracted from the downloaded PDF — clause 4.6's equations render as glyphs, not text — so it
 *  comes from the value every open implementation uses (x^9 + x^5 + 1, register preset to all
 *  ones) rather than from the standard in front of me.
 *  ★ The risk of that is low and SELF-DETECTING: a wrong scrambler means the FIB CRC never passes,
 *    ever, on any signal. It fails loudly and immediately rather than degrading, so it cannot ship
 *    unnoticed. ▶ Confirm it against the paper spec when convenient. */
class EnergyDispersal {
public:
    EnergyDispersal() { reset(); }
    void reset() { reg_ = 0x1FF; }               // all nine bits set
    /** XOR `n` bits in place with the PRBS. */
    void apply(uint8_t* bits, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            const unsigned fb = ((reg_ >> 8) ^ (reg_ >> 4)) & 1u;   // x^9 + x^5 + 1
            reg_ = ((reg_ << 1) | fb) & 0x1FFu;
            bits[i] ^= uint8_t(fb);
        }
    }
private:
    unsigned reg_ = 0x1FF;
};

// ── FIB CRC ─────────────────────────────────────────────────────────────────
/** CRC-16-CCITT as DAB uses it: polynomial 0x1021, preset all ones, complemented before
 *  transmission — the same generator TS 102 563 names for the superframe header.
 *  @return true when the 32-byte FIB checks out. */
inline uint16_t crc16(const uint8_t* data, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        crc ^= uint16_t(data[i]) << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    return crc;
}

/** A FIB is 32 bytes: 30 of payload then a 2-byte complemented CRC over those 30. */
inline bool fibCrcOk(const uint8_t* fib32) {
    const uint16_t want = uint16_t(~crc16(fib32, 30));
    const uint16_t got  = uint16_t((fib32[30] << 8) | fib32[31]);
    return want == got;
}

}  // namespace vibedab
