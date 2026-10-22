// icom_scope.h — the radio's spectrum scope (CI-V 27 00) as OUR waterfall: divisions → whole
// sweeps → SPEC rows with the radio's real edges.
//
// ★★★ AN ICOM IS NOT AN IQ RADIO (plan §0, §2.2). There is no FFT here and nothing to zoom
//     digitally: the radio measures 475 points (689 on a 7610) across the span IT is set to, and
//     that is the resolution there is. So the "zoom" a client sees is the radio's own span ladder
//     (27 15), and a wire row is these points resampled to the bin count the listener asked for —
//     sample-and-hold upward (never interpolated: a smooth curve between two measurements would
//     be invented detail), peak-hold downward (a narrow carrier must not vanish between bins).
//
// ★★ FRAMING (Icom CI-V reference, IC-7610 and IC-7300 guides; [Saber] for exact 7300 bytes):
//      27 00 <sel> <seq BCD> <total BCD> …
//      seq 1:      <mode> <A: 5 BCD> <B: 5 BCD> <out-of-range>   [+ amplitudes when total == 1, i.e. LAN]
//      seq 2..N:   <amplitude bytes>                              (50 per division on USB, the last short)
//    Centre mode (0) and scroll-C (2): A = centre, B = HALF-span. Fixed (1) and scroll-F (3): A = lower
//    edge, B = upper edge. A division out of order drops the sweep in progress — a sweep with a hole
//    in it would draw a band of zero across the waterfall, which reads as the radio failing.
//
// ★ Pure, header-only, no allocation per frame: the sweep buffer is fixed. Tested by
//   test-icom-scope.cpp from synthetic frames and from the fixture format under test/fixtures/icom/.
#pragma once
#include "civ_codec.h"
#include <cmath>
#include <cstdint>
#include <cstring>

namespace icom {

constexpr int kMaxPoints = 1024;

struct Sweep {
    uint8_t  mode = 0;               ///< 0 centre, 1 fixed, 2 scroll-C, 3 scroll-F
    int64_t  loHz = 0, hiHz = 0;     ///< the real edges of THIS sweep
    bool     outOfRange = false;     ///< the radio says the span runs off its coverage
    int      n = 0;                  ///< amplitude points
    uint8_t  amp[kMaxPoints];
    double centreHz() const { return 0.5 * (double)(loHz + hiHz); }
    double spanHz()   const { return (double)(hiHz - loHz); }
};

/** Divisions in, whole sweeps out. ★ feed() takes the frame's data (everything after 27) — the
 *  caller has already established cmd == 27 and data[0] == 00. */
class ScopeAssembler {
public:
    uint64_t sweeps = 0, dropped = 0, divisions = 0;

    /** @return true when this division completed a sweep; it is then in `out()` until the next. */
    bool feed(const uint8_t* d, int len) {
        // d[0] = 00 (the 27 00 sub), d[1] = selector, d[2] = seq, d[3] = total
        if (len < 4 || d[0] != 0x00) return false;
        const int seq = civ::bcdByte(d[2]), total = civ::bcdByte(d[3]);
        if (seq < 1 || total < 1 || seq > total) { dropped += inProgress_ ? 1 : 0; inProgress_ = false; return false; }
        divisions++;
        int at = 4;
        if (seq == 1) {
            if (inProgress_) dropped++;          // the previous sweep never finished
            if (len < 4 + 12) { inProgress_ = false; return false; }
            cur_.mode = d[at];
            const int64_t a = civ::bcdToFreq(d + at + 1, 5), b = civ::bcdToFreq(d + at + 6, 5);
            if (a < 0 || b < 0) { inProgress_ = false; dropped++; return false; }
            if (cur_.mode == 0 || cur_.mode == 2) { cur_.loHz = a - b; cur_.hiHz = a + b; }
            else { cur_.loHz = a; cur_.hiHz = b; }
            cur_.outOfRange = d[at + 11] != 0;
            cur_.n = 0;
            at += 12;
            inProgress_ = cur_.hiHz > cur_.loHz;
            expect_ = 2;
            if (!inProgress_) { dropped++; return false; }
        } else {
            if (!inProgress_ || seq != expect_) { if (inProgress_) dropped++; inProgress_ = false; return false; }
            expect_++;
        }
        for (; at < len && cur_.n < kMaxPoints; at++) cur_.amp[cur_.n++] = d[at];
        if (seq == total) {
            inProgress_ = false;
            if (cur_.n == 0) { dropped++; return false; }
            sweeps++;
            return true;
        }
        return false;
    }
    const Sweep& out() const { return cur_; }
    void reset() { inProgress_ = false; }

private:
    Sweep cur_;
    bool inProgress_ = false;
    int expect_ = 2;
};

/** Amplitude → the wire's dB. ★ The wire byte is dB + 256 (spectrum.ts: U8_DBFS_OFFSET = -256).
 *  The scope is RELATIVE to the radio's REF level, not dBFS, so this places 0..ampMax over
 *  `dbRange` dB with its floor at kScopeFloorDb — where a dongle's own noise floor draws — so the
 *  client's colour map and auto-range behave as they do for any radio. The S-meter (15 02) is
 *  the real level and travels separately (icom status message). */
constexpr double kScopeFloorDb = -130.0;
inline uint8_t ampToWire(int amp, int ampMax, double dbRange) {
    const double db = kScopeFloorDb + (double)amp * dbRange / (double)(ampMax > 0 ? ampMax : 160);
    int v = (int)std::lround(db + 256.0);
    return (uint8_t)(v < 1 ? 1 : (v > 255 ? 255 : v));
}

/** ★ A sweep as one SPEC row of `outBins` bins, in the wire's FFT ORDER (bin 0 = the centre, then
 *  +f, then −f — spectrum.ts rotates by n/2), over the sweep's OWN edges. */
inline void sweepToWire(const Sweep& s, int outBins, uint8_t* out, int ampMax, double dbRange) {
    if (outBins <= 0) return;
    const int n = s.n;
    if (n <= 0) { std::memset(out, 1, (size_t)outBins); return; }
    for (int i = 0; i < outBins; i++) {
        const int signedOut = (i <= outBins / 2) ? i : i - outBins;
        // Fraction of the span this output bin covers: [x0, x1) of 0..1
        const double x0 = 0.5 + ((double)signedOut - 0.5) / (double)outBins;
        const double x1 = x0 + 1.0 / (double)outBins;
        int lo = (int)std::floor(x0 * n), hi = (int)std::ceil(x1 * n);
        if (lo < 0) lo = 0;
        if (hi > n) hi = n;
        int best = -1;
        if ((x1 - x0) * n < 1.0 || hi <= lo) {           // upsampling: the ONE point under this bin's centre
            int k = (int)((x0 + x1) * 0.5 * n);
            if (k < 0) k = 0; if (k >= n) k = n - 1;
            best = s.amp[k];
        } else {
            for (int k = lo; k < hi; k++) if (s.amp[k] > best) best = s.amp[k];
        }
        out[i] = ampToWire(best < 0 ? 0 : best, ampMax, dbRange);
    }
}

}  // namespace icom
