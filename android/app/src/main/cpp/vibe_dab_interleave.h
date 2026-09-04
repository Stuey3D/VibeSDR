// vibe_dab_interleave.h — DAB frequency interleaving (EN 300 401 clause 14.6.1, Mode I).
//
// Quoting the spec, because this is one of those rules that is easy to paraphrase wrongly:
//
//   "Let Π(i) be a permutation in the set of integers i = 0, 1, 2,..., 2047 obtained from the
//    following congruential relation: Π(i) = 13 Π(i-1) + 511 (mod 2048) and Π(0) = 0 …
//    Let D be the set D = {d0, d1, …, d1535} … the subset of A with the same element ordering,
//    comprising all the elements of A higher than or equal to 256 and lower than or equal to
//    1 792, excluding 1 024 … k = F(n) = dn - 1024."
//
// ★★★ WHY IT MATTERS AND WHY IT IS TESTABLE EXACTLY. Interleaving spreads a burst of errors — a
//     notch, a carrier obliterated by an adjacent transmitter — across the whole codeword, which
//     is what lets the Viterbi absorb it. Get the permutation subtly wrong and NOTHING decodes,
//     but it does not look like a permutation bug: it looks like a receiver that does not work.
//     Fortunately F is a stated BIJECTION between {0..1535} and the carriers, so the test can
//     prove correctness outright rather than sampling it.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace vibedab {

/** Mode I frequency-interleaving map: QPSK symbol index n -> carrier index k (-768..768 \ 0). */
class FreqInterleaveI {
public:
    FreqInterleaveI() {
        int pi = 0, n = 0;
        for (int i = 0; i < 2048; ++i) {
            // ★ Π(0) = 0 is the FIRST element of A, so it is considered before the recursion runs;
            //   starting the loop at i=1 would drop it. It happens to be out of range anyway, but
            //   relying on that would be luck rather than the spec.
            if (i > 0) pi = (13 * pi + 511) & 2047;
            if (pi >= 256 && pi <= 1792 && pi != 1024) {
                if (n < 1536) { toCarrier_[n] = pi - 1024; ++n; }
            }
        }
        count_ = n;
        for (int i = 0; i < 1536; ++i) fromCarrier_[carrierToIndex(toCarrier_[i])] = i;
    }

    /** How many symbols the permutation actually produced — must be 1536. */
    int count() const { return count_; }

    /** n (0..1535) -> carrier k. */
    int carrierFor(int n) const { return toCarrier_[n]; }

    /** carrier k -> n (0..1535). */
    int indexFor(int k) const { return fromCarrier_[carrierToIndex(k)]; }

    /** Deinterleave one symbol's carriers (in DAB carrier order, -768..-1,+1..+768) into
     *  QPSK-symbol order, which is what the FEC layer expects. */
    template <typename T>
    void deinterleave(const T* carriersInOrder, T* out) const {
        for (int n = 0; n < 1536; ++n) {
            const int k = toCarrier_[n];
            // carriersInOrder is indexed 0..1535 for k = -768..-1, +1..+768
            out[n] = carriersInOrder[carrierToIndex(k)];
        }
    }

private:
    /** k in [-768..-1] u [1..768] -> 0..1535, DC skipped. */
    static int carrierToIndex(int k) { return k < 0 ? k + 768 : k + 767; }

    std::array<int, 1536>  toCarrier_{};
    std::array<int, 1536>  fromCarrier_{};
    int count_ = 0;
};

}  // namespace vibedab
