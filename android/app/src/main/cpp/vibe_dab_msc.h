// vibe_dab_msc.h — the Main Service Channel: CIF/CU geometry, time deinterleaving and the
// Equal Error Protection profiles.
//
// EN 300 401, read from the downloaded spec:
//   · 4.9  — "A CIF consists of 55 296 bits, grouped into 864 Capacity Units (CU) and is
//            transmitted every 24 ms." So one CU is 64 bits, and a Mode I frame carries 4 CIFs.
//   · 12   — time interleaving applies to the MSC and NOT to the FIC (which is why the FIC path
//            has none), and is 16 CIFs deep.
//   · 11.3.2 + tables 18/20 — the EEP profiles, as formulas in n.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "vibe_dab_punct.h"
#include "vibe_dab_uep.h"

namespace vibedab {

inline constexpr int kCifBits = 55296;
inline constexpr int kCifCus  = 864;
inline constexpr int kCuBits  = kCifBits / kCifCus;   // 64
inline constexpr int kCifsPerFrameI = 4;              // Mode I: 96 ms / 24 ms

/** ★★★ THE TIME-INTERLEAVER DELAY, TAKEN OFF THE SPEC'S OWN FIGURE 56.
 *
 *  Bit i of a logical frame is delayed by D[i mod 16] CIFs. Figure 56 tabulates it directly —
 *  a(r,0) leaves at r (delay 0), a(r-8,1) at r (delay 8), a(r-4,2), a(r-12,3), a(r-2,4) … — and
 *  the sequence that falls out is the 4-BIT BIT REVERSAL: 0,8,4,12,2,10,6,14,1,9,5,13,3,11,7,15.
 *
 *  ★★ I EXTRACTED IT FROM THE FIGURE RATHER THAN ASSUMING THE BIT-REVERSAL, then checked the two
 *     agreed. They do, exactly. That ordering is not decorative — spreading consecutive coded bits
 *     across 16 CIFs (384 ms) is what lets the Viterbi absorb a fade that would otherwise wipe out
 *     a whole codeword, and a wrong permutation decodes to noise while every stage reports healthy.
 */
inline constexpr int kTimeDelay[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };

/** Time deinterleaver for one sub-channel.
 *
 *  ★★ IT COSTS 15 CIFs OF LATENCY BEFORE THE FIRST COMPLETE FRAME — ~360 ms — and that is not a
 *     bug to optimise away, it is the interleaving depth. A receiver that emits audio before the
 *     buffer has filled is emitting frames with holes in them. `ready()` says when to start.
 */
class TimeDeinterleaver {
public:
    explicit TimeDeinterleaver(size_t frameBits) : n_(frameBits) {
        for (auto& b : buf_) b.assign(frameBits, 0);
    }

    /** Offer one logical frame of soft bits; returns the deinterleaved frame for this instant.
     *  ★ The output is only meaningful once ready() is true. */
    const std::vector<int8_t>& push(const int8_t* in) {
        buf_[cur_].assign(in, in + n_);
        out_.assign(n_, 0);
        for (size_t i = 0; i < n_; ++i) {
            /* ★★★ LOOK BACK (15 - d), NOT d. The transmitter puts bit i of logical frame f into
             *  AIR frame f + d. So the logical frame that completes when air frame g arrives is
             *  L = g - 15 (the deepest delay), and its bit i was carried by air frame
             *  L + d = g - 15 + d — i.e. (15 - d) frames ago, not d.
             *  ★ My first version used d and the test caught it immediately: bits with delay 0 and
             *    delay 15 were pulled from opposite ends of the buffer, so every frame was a
             *    shuffle of sixteen different ones. It would have decoded to pure noise on air
             *    with every stage below reporting itself perfectly healthy. */
            const int d = kTimeDelay[i & 15];
            const int idx = (cur_ - (15 - d) + 16 * 64) % 16;
            out_[i] = buf_[size_t(idx)][i];
        }
        cur_ = (cur_ + 1) % 16;
        if (filled_ < 16) ++filled_;
        return out_;
    }

    bool ready() const { return filled_ >= 16; }
    void reset() { for (auto& b : buf_) std::fill(b.begin(), b.end(), int8_t(0)); cur_ = 0; filled_ = 0; }

private:
    size_t n_;
    std::vector<int8_t> buf_[16];
    std::vector<int8_t> out_;
    int cur_ = 0, filled_ = 0;
};

// ── Equal Error Protection profiles ─────────────────────────────────────────
/** One EEP profile: the mother code is split into L1 blocks punctured at PI1 and L2 at PI2. */
struct EepProfile { int L1, L2, PI1, PI2; bool valid; };

/** Table 18 (set A) and table 20 (set B), as the spec states them — formulas in n.
 *  @param level  1..4
 *  @param setB   false = set A (8n kbit/s), true = set B (32n kbit/s)
 *  @param n      bit rate / 8 for set A, / 32 for set B
 *
 *  ★ 2-A at n = 1 is a SPECIAL CASE the table spells out separately (L1 = 5, L2 = 1, PI 13/12);
 *    the general 2-A formula gives L1 = 2n-3 = -1 there, which is why it needs one.
 */
inline EepProfile eepProfile(int level, bool setB, int n) {
    if (n <= 0 || level < 1 || level > 4) return { 0, 0, 0, 0, false };
    if (setB) {
        // Table 20: every level is 24n-3 / 3, differing only in the puncturing indices.
        static const int pi1[4] = { 10, 6, 4, 2 };     // levels 1..4
        static const int pi2[4] = {  9, 5, 3, 1 };
        return { 24 * n - 3, 3, pi1[level - 1], pi2[level - 1], true };
    }
    switch (level) {
        case 4: return { 4 * n - 3, 2 * n + 3, 3, 2, true };
        case 3: return { 6 * n - 3, 3, 8, 7, true };
        case 2: return n == 1 ? EepProfile{ 5, 1, 13, 12, true }
                              : EepProfile{ 2 * n - 3, 4 * n + 3, 14, 13, true };
        case 1: return { 6 * n - 3, 3, 24, 23, true };
    }
    return { 0, 0, 0, 0, false };
}

/** Coded (transmitted) length in bits for a profile, before the 12 tail bits are appended.
 *  ★ Each block is 128 mother bits = 4 sub-blocks of 32, and a sub-block at PI keeps 8+PI. */
inline int eepCodedBits(const EepProfile& p) {
    return p.valid ? 4 * (p.L1 * (8 + p.PI1) + p.L2 * (8 + p.PI2)) + 12 : 0;
}

/** Depuncture a whole EEP logical frame onto the mother code.
 *  @return mother-code positions written. */
inline size_t eepDepuncture(const int8_t* rx, size_t nRx, const EepProfile& p,
                            int8_t* out, size_t outCap) {
    if (!p.valid || !rx || !out) return 0;
    size_t r = 0, w = 0;
    auto blocks = [&](int count, int pi) {
        for (int b = 0; b < count && w + 128 <= outCap; ++b)
            for (int sub = 0; sub < 4; ++sub)
                for (int i = 0; i < 32; ++i)
                    out[w++] = punctured(pi, i) ? int8_t(0) : (r < nRx ? rx[r++] : int8_t(0));
    };
    blocks(p.L1, p.PI1);
    blocks(p.L2, p.PI2);
    for (int i = 0; i < 24 && w < outCap; ++i)
        out[w++] = tailPunctured(i) ? int8_t(0) : (r < nRx ? rx[r++] : int8_t(0));
    return w;
}

/** Depuncture a UEP logical frame onto the mother code — four block groups, then the tail.
 *  ★ Padding bits are transmitted but carry nothing; they are simply not consumed. */
inline size_t uepDepuncture(const int8_t* rx, size_t nRx, const UepProfile& p,
                            int8_t* out, size_t outCap) {
    if (!p.valid || !rx || !out) return 0;
    size_t r = 0, w = 0;
    for (int g = 0; g < 4; ++g)
        for (int blk = 0; blk < p.L[g] && w + 128 <= outCap; ++blk)
            for (int sub = 0; sub < 4; ++sub)
                for (int i = 0; i < 32; ++i)
                    out[w++] = punctured(p.PI[g], i) ? int8_t(0) : (r < nRx ? rx[r++] : int8_t(0));
    for (int i = 0; i < 24 && w < outCap; ++i)
        out[w++] = tailPunctured(i) ? int8_t(0) : (r < nRx ? rx[r++] : int8_t(0));
    return w;
}

}  // namespace vibedab
