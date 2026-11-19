// decoders/sarsat_rx.h — V12 COSPAS-SARSAT 406 MHz beacon receiver on a listener's channel IQ. Our own code from C/S T.001
// (§2.2–2.3: 160 ms of unmodulated carrier, then 400 bit/s ±1 % biphase-L, phase modulation ±1.1 ± 0.1 rad). docs/v12/SARSAT.md.
//
// ★★ THE CHAIN
//   1. A Hamming low-pass decimates the channel (≥ 24 kHz) by D to fs1 ≈ 8 kHz (SIMD dot products, nav_rx.h's kernels;
//      scalar twin bit for bit). A ring keeps the last 3 s.
//   2. FINDING A BURST: a 1024-point Hann FFT every 32 ms over ±3.5 kHz; a burst starts with a carrier LINE ≥ 14 dB over
//      the bins' median, at a frequency with no line in the half-second before. A line that stays more than 1.2 s is a
//      steady carrier (a spur, a CW signal) and is never decoded at all.
//   3. THE BURST, worked as a block 0.9 s after the line: the frequency from the FFT peak, refined by the slope of the
//      CARRIER's phase across the burst. ★ The carrier never goes: biphase PM keeps cos(1.1) of it in every bit (each bit's
//      two halves cancel the ±sin part), so a one-bit moving average of the signal IS the phase reference through the whole
//      message — no loop to lose lock. y = Im(z · conj(ref)) is then ± sin(1.1) per half bit.
//   4. TIMING: soft bits (first half − second half, from prefix sums) correlated with the 24 known sync bits — 15 ones and
//      000101111, or the self-test 011010000, either sense — every sample over the window; the best alignment's cosine
//      similarity ≥ 0.8. Then the bit rate (396–404 bit/s in 0.5 steps) and ±2 samples by the total soft energy of the message.
//   5. BCH-1 (82,61) and BCH-2 (38,26) correct (sarsat_msg.h). A soft "Chase" retry flips the 1–4 weakest bits first when
//      the hard word is not within t. ★ ACCEPTED ONLY WHEN: the sync bits had ≤ 1 error; BCH-1 corrected ≤ 3; a long
//      message's BCH-2 also corrects ≤ 2 (a long message whose BCH-2 fails is reported only if BCH-1 needed ≤ 1); a
//      3-error BCH-1 on a SHORT message is refused (the code's radius-3 sphere is 4 % of all words); the country is an
//      ITU MID; and the carrier has gone within 120 ms of the burst's end. 0 beacons on hours of noise and on carriers.
#pragma once
#include "decoders/nav_rx.h"
#include "decoders/sarsat_msg.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>
#include <functional>
#include <vector>

namespace vibe {
namespace sarsat {

struct Burst {
    Message msg;
    double offsetHz = 0;     ///< carrier off the dial
    double bitRate = 400;
    double snrDb = 0;        ///< Eb/N0 estimate from the soft bits
    double cn0 = 0;          ///< carrier to noise density estimate (dB-Hz)
    double index = 0;        ///< the phase deviation measured (rad)
    bool inverted = false;   ///< received in the opposite sense
    int syncErrors = 0;
    int64_t atSample = 0;    ///< the burst's carrier start, input samples
};

/** A small radix-2 complex FFT (in place, forward). n a power of two. Only for burst finding: 31 transforms a second. */
inline void fft(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / (double)len;
        const std::complex<float> wl((float)std::cos(ang), (float)std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

class Rx {
public:
    std::function<void(const Burst&)> onBurst;
    /** Counters, for the status line and the tests. */
    int lines = 0, steady = 0, attempts = 0, decoded = 0, refused = 0;
    const char* why = "";   ///< the last refusal's reason (tests, the log)
    double lastSync = 0;      ///< the last sync correlation

    explicit Rx(double fs, bool forceScalar = false) : scalar_(forceScalar) {
        D_ = std::max(1, (int)std::lround(fs / 8000.0));
        fs1_ = fs / D_;
        const double pass = 3600.0, stop = std::max(pass + 300.0, fs1_ - 3600.0);
        double dl = 0;
        h_ = nav::lowpass(0.5 * (pass + stop) / fs, (int)std::ceil(3.3 * fs / (stop - pass)) + 1, dl);
        T_ = (int)h_.size();
        hre_.assign((size_t)(2 * T_), 0.f); him_.assign((size_t)(2 * T_), 0.f);
        ringN_ = (size_t)std::lround(3.0 * fs1_);
        ring_.assign(ringN_, std::complex<float>(0, 0));
        win_.resize(kN);
        for (int i = 0; i < kN; ++i) win_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / kN));
        hop_ = 256;
        maxBin_ = std::min(kN / 2 - 2, (int)std::floor(3500.0 / (fs1_ / kN)));
    }
    double rate1() const { return fs1_; }

    void feed(const std::complex<float>* x, int n) {
        for (int i = 0; i < n; ++i) {
            hre_[(size_t)pos_] = x[i].real(); hre_[(size_t)(pos_ + T_)] = x[i].real();
            him_[(size_t)pos_] = x[i].imag(); him_[(size_t)(pos_ + T_)] = x[i].imag();
            if (++pos_ == T_) pos_ = 0;
            ++in_;
            if (++ph_ < D_) continue;
            ph_ = 0;
            const float* a = &hre_[(size_t)pos_]; const float* b = &him_[(size_t)pos_];
            const float re = scalar_ ? nav::dotScalar(a, h_.data(), T_) : nav::dot(a, h_.data(), T_);
            const float im = scalar_ ? nav::dotScalar(b, h_.data(), T_) : nav::dot(b, h_.data(), T_);
            check_ += (double)re + im;
            push_({ re, im });
        }
    }
    double checksum() const { return check_; }

private:
    static constexpr int kN = 1024;
    struct Pending { int64_t trig; double binF; double power; int bin; };

    void push_(std::complex<float> v) {
        ring_[(size_t)(n1_ % (int64_t)ringN_)] = v;
        ++n1_;
        if (n1_ >= kN && (n1_ % hop_) == 0) look_();
        while (!pend_.empty() && n1_ >= pend_.front().trig + (int64_t)(0.85 * fs1_)) { work_(pend_.front()); pend_.pop_front(); }
    }
    std::complex<float> at_(int64_t k) const { return ring_[(size_t)(((k % (int64_t)ringN_) + (int64_t)ringN_) % (int64_t)ringN_)]; }

    void look_() {
        std::vector<std::complex<float>> a((size_t)kN);
        const int64_t s0 = n1_ - kN;
        for (int i = 0; i < kN; ++i) a[(size_t)i] = at_(s0 + i) * win_[(size_t)i];
        fft(a);
        std::vector<float> p((size_t)(2 * maxBin_ + 1));
        for (int k = -maxBin_; k <= maxBin_; ++k) p[(size_t)(k + maxBin_)] = std::norm(a[(size_t)((k + kN) % kN)]);
        std::vector<float> s = p;
        std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
        const float med = std::max(s[s.size() / 2], 1e-30f);
        int best = 0; float bp = 0;
        for (int k = -maxBin_ + 1; k < maxBin_; ++k) { const float v = p[(size_t)(k + maxBin_)]; if (v > bp) { bp = v; best = k; } }
        const double ratio = bp / med;
        const bool line = ratio >= 25.0;   // 14 dB over the median bin (noise alone: a few a hour)
        // steady-carrier bookkeeping: how long has a line been at (about) this bin?
        if (line) {
            if (std::abs(best - lineBin_) <= 2 && n1_ - lineLast_ <= 2 * hop_) { lineLast_ = n1_; }
            else { lineBin_ = best; lineFrom_ = n1_; lineLast_ = n1_; ++lines; isNew_ = true; }
            const bool tooLong = (n1_ - lineFrom_) > (int64_t)(1.2 * fs1_);
            if (tooLong) { if (!steadyNow_) { ++steady; steadyNow_ = true; } pend_.clear(); return; }
            if (isNew_) {
                isNew_ = false;
                const float a0 = p[(size_t)(best - 1 + maxBin_)], a1 = bp, a2 = p[(size_t)(best + 1 + maxBin_)];
                const double v = nav_vertex(std::log(a0 + 1e-30), std::log(a1), std::log(a2 + 1e-30));
                pend_.push_back({ n1_, (best + v) * fs1_ / kN, bp, best });
                if (pend_.size() > 4) pend_.pop_front();
            }
        } else if (n1_ - lineLast_ > 2 * hop_) { steadyNow_ = false; lineBin_ = 100000; }
    }
    static double nav_vertex(double a, double b, double c) {
        const double d = a - 2 * b + c;
        if (!(d < 0)) return 0;
        const double v = 0.5 * (a - c) / d;
        return v < -1 ? -1 : v > 1 ? 1 : v;
    }

    void work_(const Pending& pd) {
        if (steadyNow_ && std::abs(pd.bin - lineBin_) <= 2) return;   // it became a steady carrier
        if (pd.trig - lastTrig_ < (int64_t)(0.7 * fs1_)) return;       // the same burst seen twice (its line dipped)
        ++attempts;
        // the block: from 0.25 s before the trigger to 0.85 s after (a burst is ≤ 0.52 s; its carrier began ≤ 0.128 s before)
        const int64_t b0 = pd.trig - (int64_t)(0.25 * fs1_), b1 = pd.trig + (int64_t)(0.85 * fs1_);
        if (n1_ - b0 > (int64_t)ringN_) return;
        const int L = (int)(b1 - b0);
        std::vector<std::complex<double>> z((size_t)L);
        for (int i = 0; i < L; ++i) {
            const double ph = -2 * M_PI * pd.binF * (double)(b0 + i) / fs1_;
            z[(size_t)i] = std::complex<double>(at_(b0 + i)) * std::polar(1.0, ph);
        }
        const double P = fs1_ / 400.0;   // samples per bit (nominal)
        const int M = std::max(4, (int)std::lround(P));
        // ★ the phase reference: a one-bit moving average (biphase PM leaves cos β of the carrier in every bit)
        std::vector<std::complex<double>> ref((size_t)L);
        std::vector<double> ys((size_t)L + 1);
        auto build = [&](int Mr) {
            std::vector<std::complex<double>> cs((size_t)L + 1); cs[0] = 0;
            for (int i = 0; i < L; ++i) cs[(size_t)i + 1] = cs[(size_t)i] + z[(size_t)i];
            for (int i = 0; i < L; ++i) {
                const int a = std::max(0, i - Mr / 2), b = std::min(L, a + Mr);
                ref[(size_t)i] = (cs[(size_t)b] - cs[(size_t)a]) / (double)(b - a);
            }
            // y = Im(z · conj(ref)) / |ref| (± sin β per half bit), its prefix sums
            ys[0] = 0;
            for (int i = 0; i < L; ++i) {
                const double m = std::abs(ref[(size_t)i]);
                const double y = m > 0 ? (z[(size_t)i] * std::conj(ref[(size_t)i])).imag() / m : 0;
                ys[(size_t)i + 1] = ys[(size_t)i] + y;
            }
        };
        // two bits long while the frequency is the FFT's (a few Hz off turns it little in 5 ms) …
        build(std::max(4, (int)std::lround(2 * P)));
        auto soft = [&](double s, double per) {   // a bit starting at sample s: first half − second half
            const double h = per / 2;
            const int a = (int)std::lround(s), b = (int)std::lround(s + h), c = (int)std::lround(s + per);
            if (a < 0 || c > L) return 0.0;
            return (ys[(size_t)b] - ys[(size_t)a]) - (ys[(size_t)c] - ys[(size_t)b]);
        };
        static const int sync[2][24] = {
            { 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 0,0,0,1,0,1,1,1,1 },
            { 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 0,1,1,0,1,0,0,0,0 } };
        double bestS = 0; int bestPat = 0; double bestT = -1;
        auto searchSync = [&](int s0, int s1) {
            bestS = 0; bestT = -1;
            for (int s = std::max(0, s0); s <= s1; ++s) {
                // ★ cosine similarity with the ±1 pattern: a window that only grazes the modulation (its other bits ≈ 0 on
                //   the unmodulated carrier) scores √(k/24), never 1
                double sb[24], sa = 0;
                for (int i = 0; i < 24; ++i) { sb[i] = soft(s + i * P, P); sa += sb[i] * sb[i]; }
                if (sa <= 0) continue;
                sa = std::sqrt(24.0 * sa);
                for (int p = 0; p < 2; ++p) {
                    double c = 0; for (int i = 0; i < 24; ++i) c += (sync[p][i] ? 1 : -1) * sb[i];
                    c /= sa;
                    if (std::fabs(c) > std::fabs(bestS)) { bestS = c; bestPat = p; bestT = s; }
                }
            }
        };
        // the whole block: the modulation can start anywhere in it that leaves room for a short message
        searchSync(0, L - (int)(112 * P) - 2);
        lastSync = bestS;
        if (bestT < 0 || std::fabs(bestS) < 0.8) { ++refused; why = "sync"; return; }
        // ★ refine the frequency on the burst itself (160 ms of carrier before the sync to the message's end): the slope of
        //   the reference's phase, every bit; then the reference again and the sync once more, ±3 samples
        {
            const int r0 = std::max(0, (int)(bestT - 0.15 * fs1_)), r1 = std::min(L - 1, (int)(bestT + 112 * P));
            std::vector<double> ts, phs; double prev = 0, unw = 0; bool first = true;
            for (int i = r0 + M; i <= r1 - M; i += M) {
                const double a = std::arg(ref[(size_t)i]);
                if (first) { unw = a; first = false; } else { double d = a - prev; while (d > M_PI) d -= 2 * M_PI; while (d < -M_PI) d += 2 * M_PI; unw += d; }
                prev = a; ts.push_back(i); phs.push_back(unw);
            }
            freqFix_ = 0;
            if (ts.size() >= 8) {
                double mt = 0, mp = 0; for (size_t k = 0; k < ts.size(); ++k) { mt += ts[k]; mp += phs[k]; }
                mt /= ts.size(); mp /= ts.size();
                double sxy = 0, sxx = 0; for (size_t k = 0; k < ts.size(); ++k) { sxy += (ts[k] - mt) * (phs[k] - mp); sxx += (ts[k] - mt) * (ts[k] - mt); }
                const double slope = sxx > 0 ? sxy / sxx : 0;   // rad per sample
                for (int i = 0; i < L; ++i) z[(size_t)i] *= std::polar(1.0, -slope * i);
                freqFix_ = slope * fs1_ / (2 * M_PI);
                // … eight bits long once it is the burst's own: 4× less noise on the reference (a whole number of bits, so
                //   the ±sin β halves still cancel)
                build(std::max(4, (int)std::lround(8 * P)));
                const int t0 = (int)bestT;
                searchSync(t0 - 3, std::min(L - (int)(112 * P) - 2, t0 + 3));
                if (bestT < 0 || std::fabs(bestS) < 0.8) { ++refused; why = "sync"; return; }
            }
        }
        const double sense = bestS > 0 ? 1.0 : -1.0;
        // the bit rate and a fine start: the most soft energy over the message (the format bit first, at 400)
        const bool longGuess = sense * soft(bestT + 24 * P, P) > 0;
        const int nb = longGuess ? 144 : 112;
        double bestE = -1, bestR = 400, bestT2 = bestT;
        for (int ri = -8; ri <= 8; ++ri) {
            const double r = 400 + ri * 0.5, per = fs1_ / r;
            for (int dt = -2; dt <= 2; ++dt) {
                double e = 0; for (int i = 0; i < nb; ++i) e += std::fabs(soft(bestT + dt + i * per, per));
                if (e > bestE) { bestE = e; bestR = r; bestT2 = bestT + dt; }
            }
        }
        const double per = fs1_ / bestR;
        std::vector<double> sv(145, 0.0);
        Bits b{};
        for (int i = 0; i < nb; ++i) { sv[(size_t)i + 1] = sense * soft(bestT2 + i * per, per); b[(size_t)i + 1] = sv[(size_t)i + 1] > 0; }
        // the reference's level through the message (the carrier that biphase PM leaves: cos β of it)
        double refMag = 0; int refN = 0;
        for (int i = (int)bestT2; i < std::min(L, (int)(bestT2 + nb * per)); ++i) { refMag += std::abs(ref[(size_t)i]); ++refN; }
        refMag = refN ? refMag / refN : 0;
        int syncErr = 0;
        for (int i = 0; i < 24; ++i) if (b[(size_t)i + 1] != sync[bestPat][i]) ++syncErr;
        if (syncErr > 1) { ++refused; why = "syncbits"; return; }
        const bool longMsg = b[25] == 1;
        if (longMsg != longGuess) { ++refused; why = "format"; return; }
        // BCH-1, then a Chase retry on the weakest bits of 25–106
        int e1 = bch1().correct(b);
        if (e1 < 0) e1 = chase_(b, sv, 25, 106, bch1(), 3);
        if (e1 < 0 || (!longMsg && e1 >= 3)) { ++refused; why = "bch1"; return; }
        int e2 = -1;
        if (longMsg) {
            e2 = bch2().correct(b);
            if (e2 < 0) e2 = chase_(b, sv, 107, 144, bch2(), 2);
            if (e2 < 0 && e1 > 1) { ++refused; why = "bch2"; return; }
        }
        // the carrier must have gone after the message (a beacon burst, not a carrier that happens to carry a pattern):
        // the reference's power 120–300 ms after the end, less the noise floor before the burst, under a quarter of the
        // message's (both less that floor)
        {
            auto meanPow = [&](int a, int b) { double s = 0; int n = 0; for (int i = std::max(0, a); i < std::min(L, b); ++i) { s += std::norm(ref[(size_t)i]); ++n; } return n ? s / n : -1.0; };
            const int endS = (int)(bestT2 + nb * per), modS = (int)bestT2;
            const double noise = meanPow(0, modS - (int)(0.22 * fs1_));
            const double msgP = meanPow(modS, endS), after = meanPow(endS + (int)(0.12 * fs1_), endS + (int)(0.3 * fs1_));
            if (after >= 0 && noise >= 0 && after - noise > 0.25 * (msgP - noise)) { ++refused; why = "after"; return; }
        }
        Burst out;
        out.msg = parse(b, bestPat == 1);
        out.msg.bch1Errors = e1; out.msg.bch2Errors = e2;
        if (!plausible(out.msg)) { ++refused; why = "country"; return; }
        out.offsetHz = pd.binF + freqFix_;
        out.bitRate = bestR;
        out.inverted = sense < 0;
        out.syncErrors = syncErr;
        out.atSample = (int64_t)((b0 + bestT2 - 0.16 * fs1_) * D_);
        // Eb/N0 from the soft bits: mean |s|² / var of |s|
        {
            double m = 0, m2 = 0;
            for (int i = 1; i <= nb; ++i) { const double v = std::fabs(sv[(size_t)i]); m += v; m2 += v * v; }
            m /= nb; m2 /= nb;
            const double var = std::max(1e-30, m2 - m * m);
            out.snrDb = 10 * std::log10(std::max(1e-12, m * m / var / 2));
            // the phase deviation: y ≈ |ref| · tan(index) per sample … mean |soft| / per = |ref|·tan(β)
            out.index = refMag > 0 ? std::atan(m / per / refMag) : 0;
            out.cn0 = out.snrDb + 10 * std::log10(bestR) - 10 * std::log10(std::sin(out.index) * std::sin(out.index) + 1e-12);
        }
        ++decoded;
        lastTrig_ = pd.trig;
        if (onBurst) onBurst(out);
    }
    /** Flip combinations of the 4 weakest bits of [from, to] and try the decoder again; the first word that decodes wins
     *  only if no other combination decodes to a DIFFERENT word. @return errors corrected (flips + BCH), or −1. */
    static int chase_(Bits& b, const std::vector<double>& sv, int from, int to, const Bch& code, int t) {
        std::vector<int> idx; for (int i = from; i <= to; ++i) idx.push_back(i);
        std::sort(idx.begin(), idx.end(), [&](int x, int y) { return std::fabs(sv[(size_t)x]) < std::fabs(sv[(size_t)y]); });
        idx.resize(std::min<size_t>(4, idx.size()));
        int found = -1; Bits got{};
        for (int mask = 1; mask < (1 << (int)idx.size()); ++mask) {
            Bits c = b; int flips = 0;
            for (size_t k = 0; k < idx.size(); ++k) if (mask & (1 << k)) { c[(size_t)idx[k]] ^= 1u; ++flips; }
            const int e = code.correct(c, t - 1);   // stricter inside the retry
            if (e < 0) continue;
            if (found >= 0) {
                bool same = true; for (int i = from; i <= to; ++i) if (c[(size_t)i] != got[(size_t)i]) same = false;
                if (!same) return -1;   // two different words: not believed
                continue;
            }
            found = e + flips; got = c;
        }
        if (found >= 0) for (int i = from; i <= to; ++i) b[(size_t)i] = got[(size_t)i];
        return found;
    }

    double fs1_ = 0;
    bool scalar_;
    int D_ = 1, T_ = 0, pos_ = 0, ph_ = 0, hop_ = 256, maxBin_ = 0;
    std::vector<float> h_, hre_, him_, win_;
    size_t ringN_ = 0;
    std::vector<std::complex<float>> ring_;
    int64_t n1_ = 0, in_ = 0;
    double check_ = 0, freqFix_ = 0;
    int lineBin_ = 100000;
    int64_t lineFrom_ = 0, lineLast_ = -1000000, lastTrig_ = -100000000;
    bool isNew_ = false, steadyNow_ = false;
    std::deque<Pending> pend_;
};

} // namespace sarsat
} // namespace vibe
