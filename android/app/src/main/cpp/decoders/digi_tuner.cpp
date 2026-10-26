// decoders/digi_tuner.cpp — see digi_tuner.h. Written from textbook multirate DSP (polyphase decimation, Kaiser windows).
#include "digi_tuner.h"
#include "../vibedsp/simd_internal.h"   // ★ VIBE_NEON / VIBE_SSE: dotReal, rotateBlockIlv (and the scalar twins)

#include <algorithm>
#include <cmath>

namespace vibe {
namespace digi {

namespace {
constexpr double kPi = 3.14159265358979323846;

/** I0, the modified Bessel function of the first kind — the Kaiser window's (series; converges fast for β ≤ 10). */
double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 64; ++k) {
        term *= q / ((double)k * k);
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}
/** Kaiser β for a stopband of `db` (Kaiser's formula). */
double kaiserBeta(double db) {
    return db > 50 ? 0.1102 * (db - 8.7) : db >= 21 ? 0.5842 * std::pow(db - 21, 0.4) + 0.07886 * (db - 21) : 0.0;
}
/** Taps for a Kaiser-windowed low-pass of `db` with a transition `tw` (Hz) at `rate` (Kaiser's estimate). */
int kaiserTaps(double db, double tw, double rate) {
    const double dw = 2.0 * kPi * tw / rate;
    return (int)std::ceil((db - 7.95) / (2.285 * dw)) + 1;
}
} // namespace

DigiTuner::DigiTuner(double dialHz, float loHz, float hiHz, int outRate)
    : dial_(dialHz), lo_(loHz), hi_(hiHz), fo_(outRate) {
    mid_ = 0.5f * (lo_ + hi_);
    half_ = 0.5f * (hi_ - lo_);
    /* ★ STAGE 2, once (it runs at the output rate, which never changes): pass ±half, stop from ±(half + 300 Hz), 60 dB.
     *  Odd length, symmetric, unity at DC. */
    const double tw = 300.0, db = 60.0;
    int n2 = kaiserTaps(db, tw, fo_) | 1;
    n2 = std::max(31, std::min(n2, 401));
    h2_.resize((size_t)n2);
    const double fc = (half_ + tw / 2.0) / fo_;   // cut-off (cycles/sample) mid-transition
    const double beta = kaiserBeta(db), i0b = besselI0(beta);
    double sum = 0;
    for (int k = 0; k < n2; ++k) {
        const double m = k - (n2 - 1) / 2.0;
        const double sinc = m == 0 ? 2.0 * fc : std::sin(2.0 * kPi * fc * m) / (kPi * m);
        const double r = 2.0 * k / (n2 - 1) - 1.0;
        const double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
        h2_[(size_t)k] = (float)(sinc * w);
        sum += sinc * w;
    }
    for (float& t : h2_) t = (float)(t / sum);
    zr_.assign((size_t)(n2 - 1), 0.0f);
    zi_.assign((size_t)(n2 - 1), 0.0f);
}

bool DigiTuner::fits(double dialHz, float loHz, float hiHz, double rate, double centreHz) {
    const double usable = 0.46 * rate;   // ★ the channeliser slice's outer few % are its own roll-off
    return dialHz + loHz >= centreHz - usable && dialHz + hiHz <= centreHz + usable;
}

void DigiTuner::design1(double rate) {
    rate_ = rate;
    /* ★ STAGE 1: R → fo. Pass ±(half + 50 Hz); stop from fo − (half + 50) — the first frequency whose alias lands inside
     *  the passband. 70 dB. The prototype is evaluated at kPhases sub-sample offsets, so each output needs n1_ taps. */
    const double pass = half_ + 50.0, stop = fo_ - pass, db = 70.0;
    const double tw = std::max(200.0, stop - pass);
    int n1 = kaiserTaps(db, tw, rate);
    n1 = std::max(8, std::min(n1, 512));
    n1 = (n1 + 3) & ~3;                  // ★ a multiple of four: the SIMD dot products have no scalar tail
    n1_ = n1;
    h1_.assign((size_t)kPhases * (size_t)n1, 0.0f);
    const double fc = 0.5 * (pass + stop) / rate;   // cycles per INPUT sample
    const double beta = kaiserBeta(db), i0b = besselI0(beta), halfLen = n1 / 2.0;
    for (int p = 0; p < kPhases; ++p) {
        const double frac = (double)p / kPhases;
        float* row = &h1_[(size_t)p * (size_t)n1];
        double sum = 0;
        for (int k = 0; k < n1; ++k) {
            // ★ The output sits at `base + n1/2 − 1 + frac`; tap k reads sample base + k. u = distance from the output.
            const double u = frac + halfLen - 1.0 - k;
            const double sinc = std::fabs(u) < 1e-12 ? 2.0 * fc : std::sin(2.0 * kPi * fc * u) / (kPi * u);
            const double r = u / halfLen;
            const double w = std::fabs(r) >= 1.0 ? 0.0 : besselI0(beta * std::sqrt(1.0 - r * r)) / i0b;
            row[k] = (float)(sinc * w);
            sum += sinc * w;
        }
        for (int k = 0; k < n1; ++k) row[k] = (float)(row[k] / sum);   // ★ unity gain at DC, every phase
    }
    br_.assign((size_t)n1, 0.0f);   // ★ history before the first sample: zeros, so the first output's taps are all real
    bi_.assign((size_t)n1, 0.0f);
    pos_ = n1 / 2 - 1;              // ★ its base is sample 0 of that history
}

void DigiTuner::feed(const cf32* x, int n, double rate, double centreHz, std::vector<float>& out) {
    if (n <= 0 || !(rate > 0)) return;
    if (std::fabs(rate - rate_) > 0.01) design1(rate);

    // 1. Mix: the passband's centre to 0 Hz.
    const double f = dial_ + mid_ - centreHz;
    const double w = 2.0 * kPi * f / rate;
    mr_.resize((size_t)n); mi_.resize((size_t)n);
    vibedsp::rotateBlockIlv(x, n, cr_, ci_, (float)std::cos(w), (float)std::sin(w), mr_.data(), mi_.data(), nullptr, nullptr);
    br_.insert(br_.end(), mr_.begin(), mr_.end());
    bi_.insert(bi_.end(), mi_.begin(), mi_.end());

    // 2. Decimate: every output whose taps are all in the buffer.
    const size_t z0 = zr_.size();
    const double step = rate / fo_;
    const int half1 = n1_ / 2;
    for (;;) {
        const double fl = std::floor(pos_);
        const long base = (long)fl - half1 + 1;
        if (base + n1_ > (long)br_.size()) break;
        int p = (int)std::lround((pos_ - fl) * kPhases);
        long b = base;
        if (p >= kPhases) { p = 0; ++b; if (b + n1_ > (long)br_.size()) break; }
        const float* h = &h1_[(size_t)p * (size_t)n1_];
        zr_.push_back(vibedsp::dotReal(h, &br_[(size_t)b], n1_));
        zi_.push_back(vibedsp::dotReal(h, &bi_[(size_t)b], n1_));
        pos_ += step;
    }
    // Drop what no future output can reach.
    {
        const long keepFrom = std::max(0L, (long)std::floor(pos_) - half1);
        if (keepFrom > 0) {
            br_.erase(br_.begin(), br_.begin() + keepFrom);
            bi_.erase(bi_.begin(), bi_.begin() + keepFrom);
            pos_ -= (double)keepFrom;
        }
    }

    // 3 + 4. Shape, up-mix, real part.
    const int n2 = (int)h2_.size();
    const size_t produced = zr_.size() - z0;
    if (produced == 0) return;
    const size_t at = out.size();
    out.resize(at + produced);
    // ★ The up-mix oscillator is a recursion (one complex multiply a sample), set exactly from the phase once a block —
    //   no per-sample sin/cos, and no drift.
    const double w2 = 2.0 * kPi * mid_ / fo_;
    const float sc = (float)std::cos(w2), ss = (float)std::sin(w2);
    float c = (float)std::cos(ph2_), s = (float)std::sin(ph2_);
    double blockPow = 0;
    for (size_t i = 0; i < produced; ++i) {
        // ★ zr_[i .. i+n2-1] is the window ending at the i-th new sample (the first n2−1 are the history).
        const float yr = vibedsp::dotReal(h2_.data(), &zr_[i], n2);
        const float yi = vibedsp::dotReal(h2_.data(), &zi_[i], n2);
        const float v = yr * c - yi * s;
        const float nc = c * sc - s * ss, ns = c * ss + s * sc;
        c = nc; s = ns;
        out[at + i] = v;
        blockPow += (double)v * v;
    }
    ph2_ = std::fmod(ph2_ + w2 * (double)produced, 2.0 * kPi);
    zr_.erase(zr_.begin(), zr_.begin() + (long)produced);
    zi_.erase(zi_.begin(), zi_.begin() + (long)produced);

    // 5. Level: a slow mean of the power, a gain to bring it to RMS 0.05 (FT8's waterfall sits mid-range there).
    blockPow /= (double)produced;
    if (blockPow > 0) {
        const double a = std::min(1.0, (double)produced / (8.0 * fo_));
        pow_ = pow_ <= 0 ? blockPow : pow_ + (blockPow - pow_) * a;
        const float g = (float)std::min(1e7, std::max(1e-3, 0.05 / std::sqrt(pow_)));
        gain_ = gain_ <= 0 ? g : gain_ + (g - gain_) * (float)a;
    }
    const float g = gain_ > 0 ? gain_ : 1.0f;
    for (size_t i = 0; i < produced; ++i) out[at + i] *= g;
}

} // namespace digi
} // namespace vibe
