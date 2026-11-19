// vibe_sonde_manch.h — V12 radiosondes: ONE MANCHESTER FSK DEMODULATOR for the Graw DFM (2500 chips/s) and the Meteomodem
// M10 / M20 (≈ 9600 chips/s). docs/v12/SONDES-TYPES.md. Our own code, in the RS41 receiver's shape (vibe_rs41_rx.h).
//
// ★★ THE CHAIN (per channel, on the sondes service thread): FM discriminator (vibers41::fmDisc — the SIMD atan2) → linear
//    resampling to exactly 8 samples a CHIP → a one-chip box integrator → the raw header's chips searched at all eight
//    phases by shift register + popcount (either polarity — a hard compare, no soft work on noise) → the best phase by
//    soft correlation → amplitude, offset and SNR by least squares on the header's known chips → chips at that phase with
//    an early–late timing loop (proportional + integral: the M10 and M20 differ by 0.16 % in chip rate and the loop must
//    follow either across a whole frame) → Manchester pairs → soft bits handed to the type's sink.
// ★ Manchester as the listener documentation gives it: a bit is a chip pair, "01" = 1 and "10" = 0 (the DFM header's
//   raw chips and its published 16 bits agree under this rule). The sink decides how many bits it wants (it sees them
//   arrive) and judges the frame — this file never decides that anything is a sonde.
#pragma once
#include "vibe_sonde_disc.h"   // fmDisc, cf32

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace vibesonde {

using cf32 = vibedsp::cf32;
constexpr int kSps = 8;              ///< samples a chip after the resampler
constexpr int kMaxBits = 1100;       ///< the longest frame any sink asks for (an M10 with its aux block: 119 bytes)

/** A sink: `int need(const float* soft, int n, float prevSoft)` after each bit — the total bits wanted (n itself = done, < 0 = give up);
 *  `void done(const float* soft, int n, float prevSoft, double snrDb, double offsetHz, double tS)` with the frame.
 *  soft[k] > 0 means bit 1, scaled so a clean bit is ≈ ±1. prevSoft is the header's LAST Manchester pair as a bit (the
 *  M10 / M20 differential code starts from it). tS = the channel's clock (seconds of samples seen). */
class ManchRx {
public:
    /** `rawHdr`: the header's raw chips as '0'/'1', first sent first (≤ 64). */
    ManchRx(double chRate, double chipRate, const char* rawHdr, int maxHdrErr)
        : rate_(chRate), chip_(chipRate), step_(chRate / (kSps * chipRate)), maxErr_(maxHdrErr) {
        hl_ = int(std::strlen(rawHdr));
        hdr_ = 0;
        for (int i = 0; i < hl_; ++i) hdr_ = (hdr_ << 1) | (rawHdr[i] == '1' ? 1u : 0u);
        hmask_ = hl_ >= 64 ? ~0ull : ((1ull << hl_) - 1);
    }
    double rate() const { return rate_; }
    uint64_t syncs() const { return syncs_; }

    template <class Sink> void process(const cf32* z, int n, Sink& sink) {
        if (n <= 0) return;
        d_.resize(size_t(n));
        vibers41::fmDisc(z, n, prev_, d_.data(), tmp_);
        while (pos_ < double(n - 1)) {
            const int i = int(std::floor(pos_));
            const double fr = pos_ - i;
            const float a = i < 0 ? dPrev_ : d_[size_t(i)], b = d_[size_t(i + 1)];
            push_(float(a + fr * (b - a)), sink);
            pos_ += step_;
        }
        pos_ -= n;
        dPrev_ = d_[size_t(n - 1)];
    }

private:
    static constexpr int kRing = 4096;
    float at_(uint64_t m) const { return S_[m & (kRing - 1)]; }

    template <class Sink> void push_(float y, Sink& sink) {
        box_[bi_ & 7] = y; ++bi_;
        float s = 0;
        for (int j = 0; j < 8; ++j) s += box_[j];
        const uint64_t m = m_++;
        S_[m & (kRing - 1)] = s;
        const int p = int(m & 7);
        reg_[p] = (reg_[p] << 1) | (s > 0 ? 1u : 0u);
        if (state_ == kSearch) {
            if (m >= uint64_t(hl_ * kSps + 16)) {
                const int e = __builtin_popcountll((reg_[p] ^ hdr_) & hmask_);
                if (e <= maxErr_ || e >= hl_ - maxErr_) {
                    double c = 0, mag = 0;
                    for (int j = 0; j < hl_; ++j) { const float v = at_(m - uint64_t(kSps * j)); c += ((hdr_ >> j) & 1) ? v : -v; mag += std::fabs(v); }
                    const double q = mag > 0 ? std::fabs(c) / mag : 0;
                    if (!pend_ || q > pendQ_) { pend_ = true; pendQ_ = q; pendM_ = m; pendUntil_ = m + kSps; }
                }
            }
            if (pend_ && m >= pendUntil_) { pend_ = false; commit_(pendM_); }
        } else if (m == t_ + 1) {
            chip_at_(sink);
        }
    }

    void commit_(uint64_t mh) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int j = 0; j < hl_; ++j) {
            const double x = ((hdr_ >> j) & 1) ? 1.0 : -1.0, y = at_(mh - uint64_t(kSps * j));
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        const double vx = sxx - sx * sx / hl_, A = vx > 0 ? (sxy - sx * sy / hl_) / vx : 0, D = (sy - A * sx) / hl_;
        if (std::fabs(A) < 1e-6) return;
        double r2 = 0;
        for (int j = 0; j < hl_; ++j) {
            const double x = ((hdr_ >> j) & 1) ? 1.0 : -1.0, y = at_(mh - uint64_t(kSps * j));
            r2 += (y - A * x - D) * (y - A * x - D);
        }
        A_ = A; D_ = D;
        snrDb_ = 10 * std::log10(A * A / std::max(1e-12, r2 / std::max(1, hl_ - 2)));
        ++syncs_;
        // ★ the header's last pair as a bit (chips hl−2, hl−1): the differential code's starting point
        prevSoft_ = float(((at_(mh) - D) - (at_(mh - kSps) - D)) / (2 * A));
        state_ = kCollect;
        t_ = mh + kSps;
        tau_ = 0; fr_ = 0;
        half_ = 0; nbits_ = 0; want_ = 1;
    }

    template <class Sink> void chip_at_(Sink& sink) {
        const double a = std::fabs(A_);
        const double v = (at_(t_) - D_) / A_;
        const double e = (std::fabs(at_(t_ + 1) - D_) - std::fabs(at_(t_ - 1) - D_)) / a;
        // ★ PI timing loop: the integral term carries a chip-rate error (M10 vs M20, a sonde's crystal)
        fr_ += 0.004 * e;
        fr_ = std::max(-0.05, std::min(0.05, fr_));
        tau_ += 0.05 * e + fr_;
        t_ += kSps;
        if (tau_ > 0.5) { ++t_; tau_ -= 1; } else if (tau_ < -0.5) { --t_; tau_ += 1; }
        if (half_ == 0) { c1_ = float(v); half_ = 1; return; }
        half_ = 0;
        soft_[nbits_++] = 0.5f * (float(v) - c1_);       // "01" = 1, "10" = 0
        want_ = sink.need(soft_, nbits_, prevSoft_);
        if (want_ < 0 || nbits_ >= kMaxBits) { state_ = kSearch; return; }
        if (nbits_ >= want_) {
            state_ = kSearch;
            sink.done(soft_, nbits_, prevSoft_, snrDb_, (D_ / kSps) * rate_ / (2 * M_PI), double(m_) / (kSps * chip_));
        }
    }

    enum { kSearch, kCollect };
    double rate_, chip_, step_;
    int maxErr_, hl_ = 0;
    uint64_t hdr_ = 0, hmask_ = 0;
    std::vector<float> d_;
    std::vector<cf32> tmp_;
    cf32 prev_{1, 0};
    float dPrev_ = 0;
    double pos_ = 0;
    float box_[8] = {0};
    uint64_t bi_ = 0, m_ = 0;
    float S_[kRing] = {0};
    uint64_t reg_[8] = {0};
    int state_ = kSearch;
    bool pend_ = false; double pendQ_ = 0; uint64_t pendM_ = 0, pendUntil_ = 0;
    double A_ = 1, D_ = 0, snrDb_ = 0, tau_ = 0, fr_ = 0;
    uint64_t t_ = 0;
    int half_ = 0, nbits_ = 0, want_ = 1;
    float c1_ = 0, prevSoft_ = 0;
    float soft_[kMaxBits] = {0};
    uint64_t syncs_ = 0;
};

/** Days from 1970-01-01 to a civil date (proleptic Gregorian). */
inline long daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

}  // namespace vibesonde
