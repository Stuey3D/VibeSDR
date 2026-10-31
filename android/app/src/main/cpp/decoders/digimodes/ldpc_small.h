// decoders/digimodes/ldpc_small.h — BELIEF PROPAGATION AND ORDERED-STATISTICS DECODING for the short binary LDPC codes
// of the wave-2 modes: MSK144's (128,90), and FST4/FST4W's (240,101) and (240,74). Our own code (V12, 2026-10-31).
//
// ★★ WHAT IT DOES.
//   • bp(): sum-product in the log domain, flooding schedule. The check update is the tanh rule with every product
//     taken from prefix and suffix products (no division by a message), four CHECKS at a time (NEON / SSE2 / scalar
//     lanes, vmath4.h: our own exp and log polynomials, so every path gives the same bits). The variable update is a
//     plain sum per variable. Stops on the first iteration whose hard decision satisfies every check; gives up early
//     when the count of unsatisfied checks has not improved for a while (noise does not converge).
//   • osd(): ordered-statistics decoding after BP fails — the textbook method (Fossorier & Lin, 1995): sort the bits by
//     reliability, find by Gaussian elimination the K most reliable INDEPENDENT positions, take their hard decisions,
//     and re-encode; then flip each of them in turn (order 1), and every pair among the `pairSpan` least reliable of
//     them (a partial order 2). Each candidate's cost is the sum of |LLR| over the positions where it disagrees with the
//     hard decisions; the cheapest that the caller ACCEPTS (its CRC, and whatever else it checks) wins.
//   • encode(): systematic — K information bits first, the M = N − K parity bits after (the codes here are all laid out
//     so: info then parity, the last M columns of H invertible).
// ★ LLR convention: log P(1)/P(0), codeword order (as ft8_lib's and the rest of the decoders here).
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace vibe {
namespace ldpc {

class Code {
public:
    /** `checks` = for each parity check, the codeword positions it covers (0 … N−1). */
    Code(int n, int k, const std::vector<std::vector<int>>& checks);
    int n() const { return n_; }
    int k() const { return k_; }
    int m() const { return m_; }

    /** K information bits (0/1) → the N-bit codeword (info first, then parity). */
    void encode(const uint8_t* info, uint8_t* cw) const;
    /** Every parity check holds. */
    bool checksOk(const uint8_t* cw) const;
    /** Unsatisfied checks of a hard decision. */
    int unsatisfied(const uint8_t* cw) const;

    /** Sum-product BP. Returns the iteration that satisfied every check (≥ 1), or 0 (then `cw` is the last hard decision
     *  and `*unsat`, if given, its unsatisfied check count). `giveUp` = stop after this many iterations without the
     *  unsatisfied count improving (0 = never). */
    int bp(const float* llr, int maxIter, uint8_t* cw, int* unsat = nullptr, int giveUp = 12) const;

    /** OSD (order 1 + pairs among the `pairSpan` least reliable information positions). Calls `accept(cw)` on
     *  candidates cheapest first and returns true with the first accepted one in `cw`, `*cost` its discrepancy (Σ|LLR|
     *  where it disagrees with the hard decisions) and `*flips` how many hard decisions it overturned. `maxCost` > 0
     *  bounds what is tried. */
    bool osd(const float* llr, int pairSpan, const std::function<bool(const uint8_t*)>& accept, uint8_t* cw,
             float* cost = nullptr, int* flips = nullptr, float maxCost = 0) const;

private:
    int n_, k_, m_, dmax_ = 0, groups_ = 0;
    std::vector<std::vector<int>> chk_;          // check → variables
    std::vector<int> evar_;                      // [group][slot][lane] → variable (n_ = the padding variable)
    std::vector<std::vector<int>> vedge_;        // variable → its edge indices (into evar_)
    std::vector<std::vector<uint8_t>> enc_;      // parity bit r = XOR over info bits where enc_[r][i]
};

} // namespace ldpc
} // namespace vibe
