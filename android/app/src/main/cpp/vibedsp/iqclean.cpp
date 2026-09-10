// VibeSDR V5 — IQ cleaner (DC + I/Q imbalance + rotation) and a wide-stream impulse blanker.
// Original VibeSDR code. NEON / SSE2 / scalar, one pass over the block each.
//
// ★★★ WHY THESE EXIST (Stuart, 2026-09-10, on the full-rate raw IQ stream): "with IQ correction
//     off the weak station is barely audible, with it on it's about the same as VibeServer".
//     VibeServer AVOIDS the dongle's DC spike by tuning 15 kHz above the dial and rotating the
//     offset out in the down-converter; a raw-IQ consumer that tunes to its own centre lands
//     exactly on the spike. So: correct the raw capture for everybody (DC and the I/Q image, on
//     every radio), and for the full-rate stream ALSO keep the offset and rotate it back so the
//     consumer's centre is clean AND exact. Correction first, at the true DC; rotation after.
//
// ★★ THE IMBALANCE ESTIMATE IS BLIND AND SECOND-ORDER. A proper complex signal is circular:
//    E[i·q] = 0 and E[i²] = E[q²] whatever the spectrum looks like, so any correlation between
//    the two rails, or any power difference, IS the front end's mismatch. From the slow averages
//    S_ii, S_qq, S_iq:  sin(phase error) φ = S_iq / sqrt(S_ii·S_qq),  gain ratio α = sqrt(S_qq/S_ii),
//    and the corrected  q' = (q/α − φ·i) / sqrt(1 − φ²).  One multiply-add per sample once the two
//    coefficients are known. ★ Fail safe: an estimate outside a sane range (|φ| > 0.5, α outside
//    [0.5, 2]) is a broken estimator, not a broken radio — apply nothing rather than something wild.
//
// ★★ THE DC BLOCK IS A SLOW MEAN, NOT A FILTER PER SAMPLE. One running mean of I and of Q, updated
//    once per block with a time constant of about a second, subtracted from every sample. It only
//    removes what does not move, which is the offset and nothing else.
//
// ★★ THE ROTATION IS FOUR LANES OF PHASOR. Each lane holds e^{jθ}, e^{j(θ+w)}, e^{j(θ+2w)},
//    e^{j(θ+3w)} and the whole vector steps by e^{j4w} per iteration; the phase is kept as a
//    double and the lanes are re-seeded from it every 1024 samples so float drift never adds up.
//
// ★★ THE BLANKER IS THE CLASSIC HOLD BLANKER, VECTORISED IN TWO PASSES. Pass one computes |z|²
//    for the block and the sum of the samples UNDER the threshold (the reference must not learn
//    the impulses it is removing). Pass two finds hits four at a time and only drops to scalar
//    inside a group that has one — on a clean stream that is never. A hit is replaced by the last
//    kept sample, and a run longer than maxRun is let through: that is not an impulse, and a
//    blanker that swallows a signal is worse than one that misses a click.
#include "vibedsp.h"
#include "simd_internal.h"
#include <cmath>
#include <algorithm>

namespace vibedsp {

// ── IqCleaner ──────────────────────────────────────────────────────────────────────────────

void IqCleaner::configure(double rate, double dcTauSec, double imbTauSec) {
    rate_ = rate;
    dcTau_ = dcTauSec; imbTau_ = imbTauSec;
    reset();
}

void IqCleaner::reset() {
    dcI_ = dcQ_ = 0.0f; dcSeeded_ = false;
    sII_ = sQQ_ = sIQ_ = 0.0; imbSeeded_ = false; imbWarmSec_ = 0.0;
    A_ = 1.0f; B_ = 0.0f;
    phase_ = 0.0;
}

void IqCleaner::setRotation(double hz) { rotHz_ = hz; }

void IqCleaner::process(cf32* z, int n) {
    if (n <= 0 || rate_ <= 0.0) return;
    float* f = reinterpret_cast<float*>(z);

    // ── 1. DC: block means, slow update, subtract ─────────────────────────────────────────
    if (dcOn_) {
        double si = 0.0, sq = 0.0;
        int k = 0;
#if VIBE_NEON
        float32x4_t ai = vdupq_n_f32(0.0f), aq = vdupq_n_f32(0.0f);
        for (; k + 4 <= n; k += 4) {
            float32x4x2_t v = vld2q_f32(f + 2 * k);
            ai = vaddq_f32(ai, v.val[0]); aq = vaddq_f32(aq, v.val[1]);
        }
        si = vaddvq_f32(ai); sq = vaddvq_f32(aq);
#elif VIBE_SSE
        __m128 ai = _mm_setzero_ps(), aq = _mm_setzero_ps();
        for (; k + 4 <= n; k += 4) {
            __m128 re, im; sseLoad2(f + 2 * k, re, im);
            ai = _mm_add_ps(ai, re); aq = _mm_add_ps(aq, im);
        }
        si = sseAddv(ai); sq = sseAddv(aq);
#endif
        for (; k < n; ++k) { si += f[2 * k]; sq += f[2 * k + 1]; }
        const float mi = (float)(si / n), mq = (float)(sq / n);
        if (!dcSeeded_) { dcI_ = mi; dcQ_ = mq; dcSeeded_ = true; }
        else {
            const float a = (float)std::min(1.0, (double)n / (dcTau_ * rate_));
            dcI_ += a * (mi - dcI_); dcQ_ += a * (mq - dcQ_);
        }
        if (!std::isfinite(dcI_) || !std::isfinite(dcQ_)) { dcI_ = mi; dcQ_ = mq; }
    }
    const float di = dcOn_ ? dcI_ : 0.0f, dq = dcOn_ ? dcQ_ : 0.0f;

    // ── 2. Imbalance: subtract DC, accumulate S_ii/S_qq/S_iq, apply q' = A·q + B·i ─────────
    //    (coefficients from the PREVIOUS block's averages — one block of latency on a two-second
    //    time constant is nothing, and it keeps this a single pass.)
    const float A = imbOn_ ? A_ : 1.0f, B = imbOn_ ? B_ : 0.0f;
    double bII = 0.0, bQQ = 0.0, bIQ = 0.0;
    {
        int k = 0;
#if VIBE_NEON
        const float32x4_t vdi = vdupq_n_f32(di), vdq = vdupq_n_f32(dq);
        const float32x4_t vA = vdupq_n_f32(A), vB = vdupq_n_f32(B);
        float32x4_t aII = vdupq_n_f32(0.0f), aQQ = vdupq_n_f32(0.0f), aIQ = vdupq_n_f32(0.0f);
        for (; k + 4 <= n; k += 4) {
            float32x4x2_t v = vld2q_f32(f + 2 * k);
            float32x4_t i = vsubq_f32(v.val[0], vdi);
            float32x4_t q = vsubq_f32(v.val[1], vdq);
            aII = vmlaq_f32(aII, i, i); aQQ = vmlaq_f32(aQQ, q, q); aIQ = vmlaq_f32(aIQ, i, q);
            q = vmlaq_f32(vmulq_f32(q, vA), i, vB);
            v.val[0] = i; v.val[1] = q;
            vst2q_f32(f + 2 * k, v);
        }
        bII = vaddvq_f32(aII); bQQ = vaddvq_f32(aQQ); bIQ = vaddvq_f32(aIQ);
#elif VIBE_SSE
        const __m128 vdi = _mm_set1_ps(di), vdq = _mm_set1_ps(dq);
        const __m128 vA = _mm_set1_ps(A), vB = _mm_set1_ps(B);
        __m128 aII = _mm_setzero_ps(), aQQ = _mm_setzero_ps(), aIQ = _mm_setzero_ps();
        for (; k + 4 <= n; k += 4) {
            __m128 i, q; sseLoad2(f + 2 * k, i, q);
            i = _mm_sub_ps(i, vdi); q = _mm_sub_ps(q, vdq);
            aII = _mm_add_ps(aII, _mm_mul_ps(i, i));
            aQQ = _mm_add_ps(aQQ, _mm_mul_ps(q, q));
            aIQ = _mm_add_ps(aIQ, _mm_mul_ps(i, q));
            q = _mm_add_ps(_mm_mul_ps(q, vA), _mm_mul_ps(i, vB));
            sseStore2(f + 2 * k, i, q);
        }
        bII = sseAddv(aII); bQQ = sseAddv(aQQ); bIQ = sseAddv(aIQ);
#endif
        for (; k < n; ++k) {
            const float i = f[2 * k] - di, q0 = f[2 * k + 1] - dq;
            bII += (double)i * i; bQQ += (double)q0 * q0; bIQ += (double)i * q0;
            f[2 * k] = i; f[2 * k + 1] = A * q0 + B * i;
        }
    }
    if (imbOn_) {
        const double mII = bII / n, mQQ = bQQ / n, mIQ = bIQ / n;
        if (!imbSeeded_) { sII_ = mII; sQQ_ = mQQ; sIQ_ = mIQ; imbSeeded_ = true; }
        else {
            const double a = std::min(1.0, (double)n / (imbTau_ * rate_));
            sII_ += a * (mII - sII_); sQQ_ += a * (mQQ - sQQ_); sIQ_ += a * (mIQ - sIQ_);
        }
        imbWarmSec_ += (double)n / rate_;
        // Coefficients for the NEXT block.
        float nA = 1.0f, nB = 0.0f;
        /* ★★★ NOT UNTIL A SECOND OF STATISTICS EXISTS. The first block after start seeded the
         *  averages and was APPLIED at once — on the V4 it read 24° of "phase error" (the ADC
         *  settling, a retune mid-block, whatever the first few ms hold) and the "correction"
         *  smeared an image across the whole band until the two-second average caught up. The
         *  sferic detector keys on exactly that lift across the band, and the STORM badge lit on
         *  a clear morning (Stuart, 2026-09-10). A front end's mismatch is a constant; a second's
         *  wait costs nothing and a wild first block costs a false alarm. */
        if (imbWarmSec_ >= 1.0 && sII_ > 1e-12 && sQQ_ > 1e-12) {
            const double phi   = sIQ_ / std::sqrt(sII_ * sQQ_);   // sin(phase error)
            const double alpha = std::sqrt(sQQ_ / sII_);          // gain ratio Q/I
            if (std::fabs(phi) < 0.5 && alpha > 0.5 && alpha < 2.0) {
                const double c = 1.0 / std::sqrt(1.0 - phi * phi);
                nA = (float)(c / alpha);
                nB = (float)(-phi * c);
            }
        }
        if (!std::isfinite(nA) || !std::isfinite(nB)) { nA = 1.0f; nB = 0.0f; }
        A_ = nA; B_ = nB;
    }

    // ── 3. Rotation by rotHz_ (positive = the stream moves UP in frequency) ─────────────────
    if (rotHz_ != 0.0) {
        const double w = 2.0 * M_PI * rotHz_ / rate_;   // radians per sample
        int k = 0;
        while (k < n) {
            const int m = std::min(n - k, 1024);        // re-seed the lanes every 1024 samples
            int j = 0;
#if VIBE_NEON || VIBE_SSE
            {
                // Lane phasors θ, θ+w, θ+2w, θ+3w and the per-iteration step e^{j4w}.
                float lr[4], li[4];
                for (int t = 0; t < 4; ++t) { lr[t] = (float)std::cos(phase_ + t * w); li[t] = (float)std::sin(phase_ + t * w); }
                const float sr = (float)std::cos(4.0 * w), si = (float)std::sin(4.0 * w);
#if VIBE_NEON
                float32x4_t pr = vld1q_f32(lr), pi = vld1q_f32(li);
                const float32x4_t vsr = vdupq_n_f32(sr), vsi = vdupq_n_f32(si);
                for (; j + 4 <= m; j += 4) {
                    float32x4x2_t v = vld2q_f32(f + 2 * (k + j));
                    // (i + jq)(pr + j pi) = (i·pr − q·pi) + j(i·pi + q·pr)
                    float32x4_t oi = vmlsq_f32(vmulq_f32(v.val[0], pr), v.val[1], pi);
                    float32x4_t oq = vmlaq_f32(vmulq_f32(v.val[0], pi), v.val[1], pr);
                    v.val[0] = oi; v.val[1] = oq;
                    vst2q_f32(f + 2 * (k + j), v);
                    const float32x4_t npr = vmlsq_f32(vmulq_f32(pr, vsr), pi, vsi);
                    const float32x4_t npi = vmlaq_f32(vmulq_f32(pr, vsi), pi, vsr);
                    pr = npr; pi = npi;
                }
#else
                __m128 pr = _mm_loadu_ps(lr), pi = _mm_loadu_ps(li);
                const __m128 vsr = _mm_set1_ps(sr), vsi = _mm_set1_ps(si);
                for (; j + 4 <= m; j += 4) {
                    __m128 i, q; sseLoad2(f + 2 * (k + j), i, q);
                    __m128 oi = _mm_sub_ps(_mm_mul_ps(i, pr), _mm_mul_ps(q, pi));
                    __m128 oq = _mm_add_ps(_mm_mul_ps(i, pi), _mm_mul_ps(q, pr));
                    sseStore2(f + 2 * (k + j), oi, oq);
                    const __m128 npr = _mm_sub_ps(_mm_mul_ps(pr, vsr), _mm_mul_ps(pi, vsi));
                    const __m128 npi = _mm_add_ps(_mm_mul_ps(pr, vsi), _mm_mul_ps(pi, vsr));
                    pr = npr; pi = npi;
                }
#endif
            }
#endif
            for (; j < m; ++j) {
                const double th = phase_ + j * w;
                const float cr = (float)std::cos(th), ci = (float)std::sin(th);
                const float i = f[2 * (k + j)], q = f[2 * (k + j) + 1];
                f[2 * (k + j)]     = i * cr - q * ci;
                f[2 * (k + j) + 1] = i * ci + q * cr;
            }
            phase_ += m * w;
            phase_ = std::fmod(phase_, 2.0 * M_PI);
            k += m;
        }
    }
}

// ── ImpulseBlanker ─────────────────────────────────────────────────────────────────────────

void ImpulseBlanker::configure(double rate, double tauSec, float k, double maxRunSec) {
    rate_ = rate; tau_ = tauSec; k2_ = k * k;
    maxRun_ = std::max(8, (int)std::lround(rate * maxRunSec));
    reset();
}

void ImpulseBlanker::reset() {
    avgP_ = 0.0f; seeded_ = false; run_ = 0; last_ = cf32{0.0f, 0.0f};
    blanked_ = 0; seen_ = 0;
}

void ImpulseBlanker::process(cf32* z, int n) {
    if (n <= 0 || rate_ <= 0.0) return;
    const float* f = reinterpret_cast<const float*>(z);
    if ((int)p_.size() < n) p_.resize((size_t)n);
    float* p = p_.data();

    // ── Pass 1: |z|² per sample; sum and count of the samples under the threshold ───────────
    const float thr = seeded_ ? k2_ * avgP_ : INFINITY;
    double keptSum = 0.0; long long keptN = 0;
    int k = 0;
#if VIBE_NEON
    {
        const float32x4_t vthr = vdupq_n_f32(thr);
        float32x4_t acc = vdupq_n_f32(0.0f); uint32x4_t cnt = vdupq_n_u32(0);
        for (; k + 4 <= n; k += 4) {
            float32x4x2_t v = vld2q_f32(f + 2 * k);
            float32x4_t pw = vmlaq_f32(vmulq_f32(v.val[0], v.val[0]), v.val[1], v.val[1]);
            vst1q_f32(p + k, pw);
            uint32x4_t keep = vcleq_f32(pw, vthr);
            acc = vaddq_f32(acc, vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(pw), keep)));
            cnt = vsubq_u32(cnt, keep);   // keep is all-ones (== -1) per kept lane
        }
        keptSum = vaddvq_f32(acc); keptN = (long long)vaddvq_u32(cnt);
    }
#elif VIBE_SSE
    {
        const __m128 vthr = _mm_set1_ps(thr);
        __m128 acc = _mm_setzero_ps(); __m128i cnt = _mm_setzero_si128();
        for (; k + 4 <= n; k += 4) {
            __m128 i, q; sseLoad2(f + 2 * k, i, q);
            __m128 pw = _mm_add_ps(_mm_mul_ps(i, i), _mm_mul_ps(q, q));
            _mm_storeu_ps(p + k, pw);
            __m128 keep = _mm_cmple_ps(pw, vthr);
            acc = _mm_add_ps(acc, _mm_and_ps(pw, keep));
            cnt = _mm_sub_epi32(cnt, _mm_castps_si128(keep));
        }
        keptSum = sseAddv(acc);
        alignas(16) int c4[4]; _mm_store_si128((__m128i*)c4, cnt);
        keptN = (long long)c4[0] + c4[1] + c4[2] + c4[3];
    }
#endif
    for (; k < n; ++k) {
        const float i = f[2 * k], q = f[2 * k + 1];
        const float pw = i * i + q * q;
        p[k] = pw;
        if (pw <= thr) { keptSum += pw; ++keptN; }
    }
    if (!seeded_) {
        // First block: seed from everything (there is no threshold yet), blank nothing.
        double s = 0.0; for (int t = 0; t < n; ++t) s += p[t];
        avgP_ = (float)(s / n); seeded_ = std::isfinite(avgP_) && avgP_ > 0.0f;
        seen_ += n;
        return;
    }

    // ── Pass 2: find hits four at a time; scalar only inside a group that has one ───────────
    k = 0;
#if VIBE_NEON
    {
        const float32x4_t vthr = vdupq_n_f32(thr);
        for (; k + 4 <= n; k += 4) {
            uint32x4_t hit = vcgtq_f32(vld1q_f32(p + k), vthr);
            if (vmaxvq_u32(hit) == 0) { last_ = z[k + 3]; run_ = 0; continue; }
            for (int t = 0; t < 4; ++t) blankOne(z, k + t, p[k + t] > thr);
        }
    }
#elif VIBE_SSE
    {
        const __m128 vthr = _mm_set1_ps(thr);
        for (; k + 4 <= n; k += 4) {
            const int mask = _mm_movemask_ps(_mm_cmpgt_ps(_mm_loadu_ps(p + k), vthr));
            if (!mask) { last_ = z[k + 3]; run_ = 0; continue; }
            for (int t = 0; t < 4; ++t) blankOne(z, k + t, (mask >> t) & 1);
        }
    }
#endif
    for (; k < n; ++k) blankOne(z, k, p[k] > thr);
    seen_ += n;

    // ── Reference: the kept samples only, slow time constant ────────────────────────────────
    if (keptN > 0) {
        const float m = (float)(keptSum / (double)keptN);
        const float a = (float)std::min(1.0, (double)keptN / (tau_ * rate_));
        avgP_ += a * (m - avgP_);
        if (!std::isfinite(avgP_) || avgP_ <= 0.0f) { avgP_ = m; }
    }
}

inline void ImpulseBlanker::blankOne(cf32* z, int i, bool hit) {
    if (hit && run_ < maxRun_) { z[i] = last_; ++run_; ++blanked_; }
    else { run_ = 0; last_ = z[i]; }
}

float ImpulseBlanker::rate() {
    const float r = (seen_ > 0) ? (float)blanked_ / (float)seen_ : 0.0f;
    blanked_ = 0; seen_ = 0;
    return r;
}

} // namespace vibedsp
