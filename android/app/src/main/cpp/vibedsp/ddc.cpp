// VibeSDR V5 — DDC: NCO (complex mixer) + windowed-sinc low-pass + decimator.
// Original VibeSDR code. The FIR inner loops and the NCO are the per-sample hot
// paths (they run at the full IQ rate), so they are SIMD-friendly + trig-free.
#include "vibedsp.h"
#include "simd_internal.h"   // dotReal/dotCplx (NEON), fastAtan2
#include <cmath>
#include <algorithm>
#include <cstring>

namespace vibedsp {

// ── NCO ──────────────────────────────────────────────────────────────────--
void NCO::setFreq(double normFreq) {
    const double step = 2.0 * M_PI * normFreq;     // radians/sample
    rot_ = cf32((float)std::cos(step), (float)-std::sin(step));  // exp(-j*step)
}

void NCO::mix(const cf32* in, cf32* out, int n) {
    cf32 cur = cur_;
    const cf32 rot = rot_;
    int i = 0;
    /* ★★ FOUR PHASORS AT ONCE. perf on the Pi's V4 child (2026-09-14): NCO::mix 5.9 % of the
     *  time — a scalar complex multiply per sample at the full 2.4 MS/s. This is a MAP, the
     *  shape SIMD is for (unlike the eye's serial IIR): lanes hold cur, cur·rot, cur·rot²,
     *  cur·rot³ and all four advance by rot⁴ per step. Output equals the scalar path to float
     *  rounding; phase continuity is exact because the scalar tail picks up from lane 0's
     *  successor. Renormalised every 1024 samples on all four lanes, as the scalar path did. */
#if VIBE_NEON || VIBE_SSE
    if (n >= 8) {
        const cf32 rot2 = rot * rot, rot3 = rot2 * rot, rot4 = rot2 * rot2;
        float pr[4] = { cur.real(), (cur * rot).real(), (cur * rot2).real(), (cur * rot3).real() };
        float pi_[4] = { cur.imag(), (cur * rot).imag(), (cur * rot2).imag(), (cur * rot3).imag() };
        const float* z = reinterpret_cast<const float*>(in);
        float* o = reinterpret_cast<float*>(out);
#if VIBE_NEON
        float32x4_t cr = vld1q_f32(pr), ci = vld1q_f32(pi_);
        const float32x4_t r4r = vdupq_n_f32(rot4.real()), r4i = vdupq_n_f32(rot4.imag());
        for (; i + 4 <= n; i += 4) {
            const float32x4x2_t a = vld2q_f32(z + 2 * i);
            float32x4x2_t y;
            y.val[0] = vmlsq_f32(vmulq_f32(a.val[0], cr), a.val[1], ci);   // re: ar*cr - ai*ci
            y.val[1] = vmlaq_f32(vmulq_f32(a.val[0], ci), a.val[1], cr);   // im: ar*ci + ai*cr
            vst2q_f32(o + 2 * i, y);
            const float32x4_t nr = vmlsq_f32(vmulq_f32(cr, r4r), ci, r4i);
            const float32x4_t ni = vmlaq_f32(vmulq_f32(cr, r4i), ci, r4r);
            cr = nr; ci = ni;
            sinceNorm_ += 4;
            if (sinceNorm_ >= 1024) {
                sinceNorm_ = 0;
                float32x4_t m2 = vmlaq_f32(vmulq_f32(cr, cr), ci, ci);
                float32x4_t inv = vrsqrteq_f32(m2);
                inv = vmulq_f32(inv, vrsqrtsq_f32(vmulq_f32(m2, inv), inv));
                inv = vmulq_f32(inv, vrsqrtsq_f32(vmulq_f32(m2, inv), inv));
                cr = vmulq_f32(cr, inv); ci = vmulq_f32(ci, inv);
            }
        }
        vst1q_f32(pr, cr); vst1q_f32(pi_, ci);
#else
        __m128 cr = _mm_loadu_ps(pr), ci = _mm_loadu_ps(pi_);
        const __m128 r4r = _mm_set1_ps(rot4.real()), r4i = _mm_set1_ps(rot4.imag());
        for (; i + 4 <= n; i += 4) {
            __m128 ar, ai;
            sseLoad2(z + 2 * i, ar, ai);
            const __m128 yr = _mm_sub_ps(_mm_mul_ps(ar, cr), _mm_mul_ps(ai, ci));
            const __m128 yi = _mm_add_ps(_mm_mul_ps(ar, ci), _mm_mul_ps(ai, cr));
            sseStore2(o + 2 * i, yr, yi);
            const __m128 nr = _mm_sub_ps(_mm_mul_ps(cr, r4r), _mm_mul_ps(ci, r4i));
            const __m128 ni = _mm_add_ps(_mm_mul_ps(cr, r4i), _mm_mul_ps(ci, r4r));
            cr = nr; ci = ni;
            sinceNorm_ += 4;
            if (sinceNorm_ >= 1024) {
                sinceNorm_ = 0;
                const __m128 m2 = _mm_add_ps(_mm_mul_ps(cr, cr), _mm_mul_ps(ci, ci));
                const __m128 inv = _mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(m2));
                cr = _mm_mul_ps(cr, inv); ci = _mm_mul_ps(ci, inv);
            }
        }
        _mm_storeu_ps(pr, cr); _mm_storeu_ps(pi_, ci);
#endif
        cur = cf32(pr[0], pi_[0]);   // lane 0 is the phasor for sample i — the scalar tail continues it
    }
#endif
    for (; i < n; ++i) {
        out[i] = in[i] * cur;     // multiply by running phasor (no per-sample trig)
        cur *= rot;
        // Renormalise occasionally so |cur| doesn't drift from 1 (recursion error).
        if (++sinceNorm_ >= 1024) {
            sinceNorm_ = 0;
            const float m = 1.0f / std::sqrt(cur.real() * cur.real() + cur.imag() * cur.imag());
            cur *= m;
        }
    }
    cur_ = cur;
}

// ── FIR low-pass design (windowed sinc, Hamming) ─────────────────────────--
std::vector<float> designLowpass(double cutoff, double transition, bool deepStop) {
    // Tap count from transition width (Hamming ~ 3.3/BW, Blackman ~ 5.5/BW), odd for
    // symmetry. Blackman buys ~74 dB of stopband against Hamming's ~53 dB — which
    // matters wherever what leaks through will FOLD into the audio and can't be
    // removed later.
    int n = (int)std::ceil((deepStop ? 5.5 : 3.3) / std::max(transition, 1e-4));
    if ((n & 1) == 0) ++n;
    if (n < 9) n = 9;
    std::vector<float> h(n);
    const double mid = (n - 1) / 2.0;
    const double wc = 2.0 * M_PI * cutoff;
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const double k = i - mid;
        double sinc = (k == 0.0) ? (wc / M_PI) : std::sin(wc * k) / (M_PI * k);
        const double t = 2.0 * M_PI * i / (n - 1);
        const double win = deepStop
            ? (0.42 - 0.5 * std::cos(t) + 0.08 * std::cos(2.0 * t))   // Blackman ~74 dB
            : (0.54 - 0.46 * std::cos(t));                            // Hamming  ~53 dB
        h[i] = (float)(sinc * win);
        sum += h[i];
    }
    for (auto& v : h) v /= (float)sum;   // unity DC gain
    return h;
}

// ── FIR decimator (complex stream, real taps) ────────────────────────────--
// ★★ THE DELAY LINE IS NEVER SHRUNK (2026-10-18). process() used to resize buf_ up to K-1+n and back
//    down to K-1 on every call, and growing a std::vector VALUE-INITIALISES the new tail — so every
//    block was zero-filled and then immediately overwritten by the copy: _M_default_append was 2.5 %
//    of the whole server on the Lenovo's WFM profile, pure waste. The buffer now keeps its size and
//    only the history (the first K-1 samples) carries meaning between calls.
// ★ kSlack: room past the block for the x86 banks to over-read (zeros, so they contribute nothing).
static constexpr int kFirSlack = 32;   // ≥ the worst bank over-read: RealFir Lp_-K ≤ 18 floats

#if VIBE_SSE
#ifndef VIBE_FIR_BANK_MAX_FLOATS
#define VIBE_FIR_BANK_MAX_FLOATS 2048
#endif
static inline bool aligned16(const void* p) { return ((uintptr_t)p & 15u) == 0; }
/** A 16-byte-aligned window into `v`, sized `floats`, returned as the offset of its first float. */
static int alignedBank(std::vector<float>& v, size_t floats) {
    v.assign(floats + 4, 0.0f);
    int off = 0;
    while (!aligned16(v.data() + off)) ++off;
    return off;
}
#endif

// ★ decim < 1 is clamped to 1: the output-stepping loop in process() advances by decim_ (it used to
//   count inputs, where 0 simply never fired; a step of 0 would never end).
FirDecimator::FirDecimator(std::vector<float> taps, int decim)
    : decim_(decim < 1 ? 1 : decim), phase_(decim < 1 ? 1 : decim), K_((int)taps.size()) {
    rtaps_.assign(taps.rbegin(), taps.rend());   // reversed for contiguous dot
    buf_.assign(K_ - 1 + kFirSlack, cf32(0.0f, 0.0f));   // K-1 sample history (+ slack)
    buildBank_();
}

/** ★ x86: two banks of pair-duplicated reversed taps, the second shifted one sample later, each
 *  Lp_ complex taps (a multiple of 8 = four SSE vectors per step). See sseDotAligned. */
void FirDecimator::buildBank_() {
#if VIBE_SSE
    Lp_ = ((K_ + 1) + 7) & ~7;
    if (4 * Lp_ > VIBE_FIR_BANK_MAX_FLOATS) { Lp_ = 0; bank_.clear(); return; }
    bankOff_ = alignedBank(bank_, (size_t)4 * Lp_);
    for (int p = 0; p < 2; ++p) {
        float* b = bank_.data() + bankOff_ + (size_t)p * 2 * Lp_;
        for (int k = 0; k < K_; ++k) b[2 * (k + p)] = b[2 * (k + p) + 1] = rtaps_[k];
    }
#endif
}

bool FirDecimator::retune(const std::vector<float>& taps) {
    // ★ The length is the contract, not a detail — see the header. Refuse rather than resize:
    //   growing buf_ here would leave the extra history zeroed, which is the very transient
    //   this function exists to avoid, and silently produce a click instead of preventing one.
    if ((int)taps.size() != K_) return false;
    // buf_ and phase_ are deliberately untouched: the delay line carries the signal across the
    // change and the decimation phase keeps the output sample grid aligned.
    rtaps_.assign(taps.rbegin(), taps.rend());
    buildBank_();
    return true;
}

void FirDecimator::reset() {
    std::fill(buf_.begin(), buf_.end(), cf32(0.0f, 0.0f));
    phase_ = decim_;
}

int FirDecimator::process(const cf32* in, int n, cf32* out) {
    // buf_ = [K-1 history][block]; y[i] = dot(rtaps_, &buf_[i]) over K samples.
    /* ★★ STEP FROM KEPT OUTPUT TO KEPT OUTPUT (2026-10-18). This walked every INPUT sample and counted
     *  phase_ down to the next kept one — for a decimate-by-101 stage, 100 empty iterations per output,
     *  each a read-modify-write of phase_ in memory (a store-forwarding chain): the hottest instruction in
     *  the narrow-SSB profile was `subl $1, phase_`. The kept indices are just phase_-1, +decim, +decim…,
     *  so loop over those and leave phase_ as the countdown to the next block's first one. Identical
     *  outputs, identical phase carried across calls. */
    const size_t used = (size_t)(K_ - 1 + n);
    if (buf_.size() < used + kFirSlack) buf_.resize(used + kFirSlack);
    std::copy(in, in + n, buf_.begin() + (K_ - 1));
    const float* z = reinterpret_cast<const float*>(buf_.data());  // interleaved re/im
    int outn = 0;
#if VIBE_SSE
    if (aligned16(z) && Lp_ > 0) {
        // ★ The banks over-read up to Lp_-K_ samples past the window: keep that slack ZERO, so a
        //   stale non-finite sample left there by an earlier block can never leak in (0 × NaN).
        std::fill(buf_.begin() + used, buf_.begin() + used + kFirSlack, cf32(0.0f, 0.0f));
        const float* bank = bank_.data() + bankOff_;
        const int nv = Lp_ / 2;                          // 4-float vectors per bank
        int i = phase_ - 1;
        for (; i < n; i += decim_) {
            const int p = i & 1;                         // samples are 8 bytes: two phases
            out[outn++] = sseDotCplxAligned(bank + (size_t)p * 2 * Lp_, z + 2 * (i - p), nv);
        }
        phase_ = i - n + 1;
    } else
#endif
    {
        int i = phase_ - 1;
        for (; i < n; i += decim_) out[outn++] = dotCplx(rtaps_.data(), z + 2 * i, K_);
        phase_ = i - n + 1;
    }
    // Carry the last K-1 samples as the next call's history.
    std::copy(buf_.begin() + n, buf_.begin() + used, buf_.begin());
    return outn;
}

// ── AM demod ─────────────────────────────────────────────────────────────--
void AmDemod::process(const cf32* in, float* out, int n) {
    // ★ The square roots four at a time into `out`, then the (serial) DC blocker in place —
    //   the sqrt was the cost, the one-pole is two multiplies (2026-09-16).
    magnitudes(in, out, n);
    for (int i = 0; i < n; ++i) {
        const float mag = out[i];
        // One-pole DC blocker removes the carrier term, leaving the modulation.
        dc_ = kPole * dc_ + (1.0f - kPole) * mag;
        out[i] = (mag - dc_) * kGain;
    }
}

// ★ Public wrapper so header-only classes (the noise blanker) can reach the vector kernel
//   without including simd_internal.h.
void complexMagnitudes(const cf32* z, float* out, int n) { magnitudes(z, out, n); }

// ── FM demod (quadrature discriminator) ──────────────────────────────────--
void FmDemod::process(const cf32* in, float* out, int n) {
    // Accurate fast atan2 (~1e-6 error): the discriminator feeds the 23-53 kHz MPX
    // and thus the L-R stereo difference, so it must stay clean — the earlier crude
    // approximation (~3.8e-3) audibly corrupted stereo. This minimax version is
    // inaudible and far cheaper than std::atan2 at the channel rate.
    //
    // NEON: four discriminator outputs at a time. The product z[i]*conj(z[i-1])
    // only reaches back one sample, so after the first output every operand is
    // already in the input block — no serial dependency at all, unlike the PLL
    // downstream. Sample 0 is done scalar against the previous block's tail.
    if (n <= 0) return;
    {
        const cf32 d = in[0] * std::conj(prev_);
        out[0] = gain_ * fastAtan2(d.imag(), d.real());
    }
    int i = 1;
#if VIBE_NEON
    const float* z = reinterpret_cast<const float*>(in);
    for (; i + 4 <= n; i += 4) {
        const float32x4x2_t a = vld2q_f32(z + 2 * i);          // z[i..i+3]
        const float32x4x2_t b = vld2q_f32(z + 2 * (i - 1));    // z[i-1..i+2]
        // d = a * conj(b)
        const float32x4_t dr = vmlaq_f32(vmulq_f32(a.val[0], b.val[0]), a.val[1], b.val[1]);
        const float32x4_t di = vmlsq_f32(vmulq_f32(a.val[1], b.val[0]), a.val[0], b.val[1]);
        vst1q_f32(out + i, vmulq_n_f32(fastAtan2q(di, dr), gain_));
    }
#elif VIBE_SSE
    // ★ Same algebra as the NEON block above, lane for lane: d = a * conj(b), then the 4-wide
    //   atan2. `b` reads one sample BEHIND `a` — the loop starts at i=1 with prev_ already placed,
    //   so the unaligned load at 2*(i-1) is always in range.
    const float* z = reinterpret_cast<const float*>(in);
    const __m128 gainv = _mm_set1_ps(gain_);
    for (; i + 4 <= n; i += 4) {
        __m128 ar, ai, br, bi;
        sseLoad2(z + 2 * i,       ar, ai);      // z[i..i+3]
        sseLoad2(z + 2 * (i - 1), br, bi);      // z[i-1..i+2]
        const __m128 dr = sseMla(_mm_mul_ps(ar, br), ai, bi);   // re: ar*br + ai*bi
        const __m128 di = _mm_sub_ps(_mm_mul_ps(ai, br), _mm_mul_ps(ar, bi));  // im: ai*br - ar*bi
        _mm_storeu_ps(out + i, _mm_mul_ps(fastAtan2q(di, dr), gainv));
    }
#endif
    for (; i < n; ++i) {
        const cf32 d = in[i] * std::conj(in[i - 1]);
        out[i] = gain_ * fastAtan2(d.imag(), d.real());
    }
    prev_ = in[n - 1];
}

// ── SSB / CW demod (Weaver / third method) ───────────────────────────────--
void SsbDemod::configure(Side side, double bwHz, double rate, double designBwHz) {
    side_ = side;
    rate_ = rate;
    const double fc = bwHz * 0.5;              // sub-carrier = half the SSB bandwidth
    // SIGNED: USB centres the upper sideband (mix down by +fc), LSB centres the
    // lower (mix down by -fc = up). Both mix stages must flip together or the
    // sidebands swap and the image isn't rejected (it appears frequency-mirrored).
    const double w = (side == Side::LSB ? -1.0 : 1.0) * 2.0 * M_PI * fc / rate;
    rot_ = cf32((float)std::cos(w), (float)std::sin(w));   // e^{j*w} per sample
    cur_ = cf32(1.0f, 0.0f); sinceNorm_ = 0;
    // Low-pass at bw/2 (normalised): keeps the centred wanted sideband, rejects
    // the image (which the down-mix pushed just past +/- bw/2). The wanted and
    // unwanted sidebands MEET at the carrier (= +/- bw/2 after centring), so the
    // filter's transition band straddles them — a wide transition lets the wrong
    // sideband's low-audio content bleed through. Use a SHARP transition (~80 Hz)
    // so only sub-~80 Hz of the wrong sideband leaks (inaudible); the FIRs are
    // NEON-accelerated so the longer filter is cheap.
    const double cutoff = fc / rate;
    // ★★★ THE TRANSITION FIXES THE TAP COUNT, and the tap count is what retune() must preserve.
    //     Designing it from the NARROWEST width in the band (designBwHz) instead of this one
    //     means every width in the band produces the same length, so the corner can be moved in
    //     place. Narrowest is also the safe direction: wider widths get a filter that is if
    //     anything sharper than they needed, never sloppier — and the ~80 Hz floor that protects
    //     the wrong sideband still applies.
    const double designFc = (designBwHz > 0.0 ? designBwHz : bwHz) * 0.5;
    trans_ = std::max((designFc / rate) * 0.06, 80.0 / rate);
    std::vector<float> taps = designLowpass(cutoff, trans_);
    lpfI_ = std::make_unique<RealFir>(taps);
    lpfQ_ = std::make_unique<RealFir>(taps);
}

bool SsbDemod::retune(Side side, double bwHz) {
    if (!lpfI_ || !lpfQ_ || rate_ <= 0.0 || trans_ <= 0.0) return false;
    const double fc = bwHz * 0.5;
    const double w  = (side == Side::LSB ? -1.0 : 1.0) * 2.0 * M_PI * fc / rate_;
    // cur_ is deliberately NOT reset: the rotator carries its phase across the change, so the
    // sub-carrier steps in frequency without a discontinuity in the mixed signal.
    std::vector<float> taps = designLowpass(fc / rate_, trans_);
    if (!lpfI_->retune(taps)) return false;
    // ★ Both arms are checked, though they cannot realistically disagree: they are handed the
    //   SAME taps and were built from the same design, so their K matches by construction. The
    //   check is here because a half-applied Weaver pair would mismatch I and Q and collapse the
    //   image rejection — a far worse failure than the rebuild being avoided — and that risk
    //   should be refused explicitly rather than left resting on an invariant elsewhere.
    if (!lpfQ_->retune(taps)) return false;
    side_ = side;
    rot_  = cf32((float)std::cos(w), (float)std::sin(w));
    return true;
}

void SsbDemod::reset() {
    cur_ = cf32(1.0f, 0.0f); sinceNorm_ = 0;
    if (lpfI_) lpfI_->reset();
    if (lpfQ_) lpfQ_->reset();
}

void SsbDemod::process(const cf32* in, float* out, int n) {
    aI_.resize(n); aQ_.resize(n); cbuf_.resize(n); sbuf_.resize(n);
    // Mix by signed fc via a recursive rotator (c,s = cur_ advanced by rot_ each
    // sample — no per-sample trig): a = z * e^{-j*phase} = z * (cos - j sin). The
    // sideband sign lives in rot_ (down by +fc for USB, up by fc for LSB).
    // ★ Four lanes of oscillator (rotateBlockIlv), renormalised once per block instead of every
    //   1024 samples — the same recurrence, no longer a serial chain (2026-09-16).
    {
        float cr = cur_.real(), ci = cur_.imag();
        rotateBlockIlv(in, n, cr, ci, rot_.real(), rot_.imag(),
                       aI_.data(), aQ_.data(), cbuf_.data(), sbuf_.data());
        cur_ = cf32(cr, ci);
    }
    fI_.resize(n); fQ_.resize(n);
    lpfI_->process(aI_.data(), n, fI_.data());
    lpfQ_->process(aQ_.data(), n, fQ_.data());
    // Mix back and take real: out = Re{aLPF * e^{+j*phase}} = fI*c - fQ*s. The
    // sideband sign already lives in phase, so the formula is the same for both.
    {
        int i = 0;
#if VIBE_NEON
        for (; i + 4 <= n; i += 4)
            vst1q_f32(out + i, vmlsq_f32(vmulq_f32(vld1q_f32(fI_.data() + i), vld1q_f32(cbuf_.data() + i)),
                                         vld1q_f32(fQ_.data() + i), vld1q_f32(sbuf_.data() + i)));
#elif VIBE_SSE
        for (; i + 4 <= n; i += 4)
            _mm_storeu_ps(out + i, _mm_sub_ps(_mm_mul_ps(_mm_loadu_ps(fI_.data() + i), _mm_loadu_ps(cbuf_.data() + i)),
                                              _mm_mul_ps(_mm_loadu_ps(fQ_.data() + i), _mm_loadu_ps(sbuf_.data() + i))));
#endif
        for (; i < n; ++i) out[i] = fI_[i] * cbuf_[i] - fQ_[i] * sbuf_[i];
    }
}

// ── Real FIR (low-pass / optional decimate) ──────────────────────────────--
// ★ Same never-shrunk delay line as FirDecimator, and on x86 four phase-shifted banks (real
//   samples are 4 bytes, so a window can start at any of four offsets within 16 bytes).
RealFir::RealFir(std::vector<float> taps, int decim)
    : decim_(decim < 1 ? 1 : decim), phase_(decim < 1 ? 1 : decim), K_((int)taps.size()) {   // ★ see FirDecimator
    rtaps_.assign(taps.rbegin(), taps.rend());   // reversed for contiguous dot
    buf_.assign(K_ - 1 + kFirSlack, 0.0f);         // K-1 sample history (+ slack)
    buildBank_();
}

void RealFir::buildBank_() {
#if VIBE_SSE
    Lp_ = ((K_ + 3) + 15) & ~15;                  // floats per bank: four SSE vectors per step
    if (4 * Lp_ > VIBE_FIR_BANK_MAX_FLOATS) { Lp_ = 0; bank_.clear(); return; }
    bankOff_ = alignedBank(bank_, (size_t)4 * Lp_);
    for (int p = 0; p < 4; ++p) {
        float* b = bank_.data() + bankOff_ + (size_t)p * Lp_;
        for (int k = 0; k < K_; ++k) b[k + p] = rtaps_[k];
    }
#endif
}

bool RealFir::retune(const std::vector<float>& taps) {
    if ((int)taps.size() != K_) return false;   // see FirDecimator::retune
    rtaps_.assign(taps.rbegin(), taps.rend());
    buildBank_();
    return true;
}

void RealFir::reset() {
    std::fill(buf_.begin(), buf_.end(), 0.0f);
    phase_ = decim_;
}

int RealFir::process(const float* in, int n, float* out) {
    // buf_ = [K-1 history][block]; y[i] = dot(rtaps_, &buf_[i]) over K samples.
    const size_t used = (size_t)(K_ - 1 + n);
    if (buf_.size() < used + kFirSlack) buf_.resize(used + kFirSlack);
    std::copy(in, in + n, buf_.begin() + (K_ - 1));
    int outn = 0;
#if VIBE_SSE
    if (aligned16(buf_.data()) && Lp_ > 0) {
        std::fill(buf_.begin() + used, buf_.begin() + used + kFirSlack, 0.0f);   // see FirDecimator
        const float* bank = bank_.data() + bankOff_;
        const float* z = buf_.data();
        const int nv = Lp_ / 4;
        int i = phase_ - 1;
        for (; i < n; i += decim_) {
            const int p = i & 3;
            out[outn++] = sseAddv(sseDotAligned(bank + (size_t)p * Lp_, z + (i - p), nv));
        }
        phase_ = i - n + 1;
    } else
#endif
    {
        int i = phase_ - 1;
        for (; i < n; i += decim_) out[outn++] = dotReal(rtaps_.data(), buf_.data() + i, K_);
        phase_ = i - n + 1;
    }
    std::copy(buf_.begin() + n, buf_.begin() + used, buf_.begin());
    return outn;
}


// ── HalfbandDecimator (see vibedsp.h) ─────────────────────────────────────────────────────────────────────────────

#if VIBE_NEON
/* ★ The half-band's NEON loop with the tap count a CONSTANT (dispatched below). ARMv7's vld1 has no immediate offset, so
 *  2P unaligned loads cost 2P address adds too (the generic loop: 18 instructions a tap pair; one load per tap: 12). So
 *  the 2P+3 odd samples four outputs need are loaded ONCE as consecutive quads ([r]! post-increment) and every shifted
 *  view is a VEXT of two of them: per tap pair, two VEXTs (or none), an add and a multiply-accumulate. */
template <int T> static inline float32x4_t hbAt(const float32x4_t* q) {
    if constexpr (T % 4 == 0) return q[T / 4]; else return vextq_f32(q[T / 4], q[T / 4 + 1], T % 4);
}
template <int P, int J> struct HbAcc {   // acc += h_J · (o[m−J] + o[m+J−1]) — window index of o[m+x] is P + x
    static inline float32x4_t run(float32x4_t acc, const float32x4_t* q, const float32x4_t* hv) {
        acc = vmlaq_f32(acc, vaddq_f32(hbAt<P - J>(q), hbAt<P + J - 1>(q)), hv[J]);
        return HbAcc<P, J - 1>::run(acc, q, hv);
    }
};
template <int P> struct HbAcc<P, 0> { static inline float32x4_t run(float32x4_t acc, const float32x4_t*, const float32x4_t*) { return acc; } };
template <int P>
static void hbNeon(const float* er, const float* ei, const float* orr, const float* oi, const float* h,
                   int& m, int last, float* o, int& k) {
    constexpr int Q = (2 * P + 2) / 4 + 1;   // quads covering window indices 0 … 2P+2
    float32x4_t hv[P + 1];
    for (int j = 0; j <= P; ++j) hv[j] = vdupq_n_f32(h[j]);
    // ★ one rail at a time (re, then im): Q window quads + P+1 taps + an accumulator stay in the 16 q registers
    auto rail = [&](const float* w, const float* e) {
        float32x4_t q[Q];
        for (int t = 0; t < Q; ++t) q[t] = vld1q_f32(w + 4 * t);
        return HbAcc<P, P>::run(vmulq_f32(vld1q_f32(e), hv[0]), q, hv);
    };
    for (; m + 4 <= last + 1; m += 4, k += 4) {
        float32x4x2_t y;
        y.val[0] = rail(orr + m - P, er + m);
        y.val[1] = rail(oi + m - P, ei + m);
        vst2q_f32(o + 2 * k, y);
    }
}
#endif
int HalfbandDecimator::halfTapsFor(double trans) {
    const int n = (int)std::ceil(5.5 / std::max(trans, 1e-3));   // designLowpass's Blackman rule
    return std::max(2, (n + 1 + 3) / 4);
}

HalfbandDecimator::HalfbandDecimator(int halfTaps) : P_(std::max(1, halfTaps)) {
    const int n = 4 * P_ - 1;
    const double mid = (n - 1) / 2.0;
    std::vector<double> full((size_t)n);
    double sum = 0;
    for (int i = 0; i < n; ++i) {
        const double k = i - mid;
        const double sinc = k == 0.0 ? 0.5 : std::sin(M_PI * k / 2.0) / (M_PI * k);
        const double t = 2.0 * M_PI * i / (n - 1);
        full[(size_t)i] = sinc * (0.42 - 0.5 * std::cos(t) + 0.08 * std::cos(2.0 * t));
        sum += full[(size_t)i];
    }
    h_.assign((size_t)P_ + 1, 0.0f);
    h_[0] = (float)(full[(size_t)mid] / sum);
    for (int j = 1; j <= P_; ++j) h_[(size_t)j] = (float)(full[(size_t)(mid + 2 * j - 1)] / sum);
    // ★ P pairs of history either side: start with P zero pairs (the filter's own start-up, not a signal)
    eR_.assign((size_t)P_ + 64, 0.0f); eI_.assign((size_t)P_ + 64, 0.0f); oR_.assign((size_t)P_ + 64, 0.0f); oI_.assign((size_t)P_ + 64, 0.0f);
    len_ = (size_t)P_;
}

int HalfbandDecimator::process(const cf32* in, int n, cf32* out) {
    if (n <= 0) return 0;
    // 1. split into planar even / odd streams, after the history (a sample carried from the last block first)
    const size_t base = len_;
    const int total = n + (carry_ ? 1 : 0);
    const int pairs = total / 2;
    if (pairs == 0) { carry_ = true; carried_ = in[0]; return 0; }   // one sample, held for its partner
    if (eR_.size() < base + (size_t)pairs + 4) {                     // ★ grow only (a resize per call zero-filled: memset 6 %)
        const size_t cap = 2 * (base + (size_t)pairs) + 8;
        eR_.resize(cap); eI_.resize(cap); oR_.resize(cap); oI_.resize(cap);
    }
    float* const er = eR_.data(); float* const ei = eI_.data(); float* const orr = oR_.data(); float* const oi = oI_.data();
    int i = 0, p = 0;
    if (carry_) {
        er[base] = carried_.real(); ei[base] = carried_.imag(); orr[base] = in[0].real(); oi[base] = in[0].imag();
        i = 1; p = 1;
    }
    const float* f = reinterpret_cast<const float*>(in);
#if VIBE_NEON
    for (; p + 4 <= pairs; p += 4, i += 8) {
        const float32x4x4_t v = vld4q_f32(f + 2 * i);   // re0 re2 re4 re6 | im0 im2 … | re1 re3 … | im1 im3 …
        vst1q_f32(er + base + p, v.val[0]); vst1q_f32(ei + base + p, v.val[1]);
        vst1q_f32(orr + base + p, v.val[2]); vst1q_f32(oi + base + p, v.val[3]);
    }
#endif
    for (; p < pairs; ++p, i += 2) {
        er[base + p] = f[2 * i]; ei[base + p] = f[2 * i + 1]; orr[base + p] = f[2 * i + 2]; oi[base + p] = f[2 * i + 3];
    }
    carry_ = (total & 1) != 0;
    if (carry_) carried_ = in[n - 1];
    len_ = base + (size_t)pairs;

    // 2. outputs m = P … A−P (A = pairs held): y[m] = c·e[m] + Σ_j h_j·(o[m−j] + o[m+j−1])
    const int P = P_;
    const int last = (int)len_ - P;         // the highest m with o[m+P−1] present
    int m = P, k = 0;
    const float c = h_[0];
    const float* const h = h_.data();
    float* o = reinterpret_cast<float*>(out);
#if VIBE_NEON
    switch (P) {   // ★ the common lengths unrolled; others take the generic loop just below
        case 3: hbNeon<3>(er, ei, orr, oi, h, m, last, o, k); break;
        case 4: hbNeon<4>(er, ei, orr, oi, h, m, last, o, k); break;
        case 5: hbNeon<5>(er, ei, orr, oi, h, m, last, o, k); break;
        case 6: hbNeon<6>(er, ei, orr, oi, h, m, last, o, k); break;
        case 7: hbNeon<7>(er, ei, orr, oi, h, m, last, o, k); break;
        case 8: hbNeon<8>(er, ei, orr, oi, h, m, last, o, k); break;
        default: break;
    }
    for (; m + 4 <= last + 1; m += 4, k += 4) {
        const float* lr = orr + m; const float* li = oi + m;       // o[m−j] = lr[−j], o[m+j−1] = lr[j−1]
        float32x4_t ar = vmulq_n_f32(vld1q_f32(er + m), c), ai = vmulq_n_f32(vld1q_f32(ei + m), c);
        for (int j = 1; j <= P; ++j) {
            const float hj = h[j];
            ar = vmlaq_n_f32(ar, vaddq_f32(vld1q_f32(lr - j), vld1q_f32(lr + j - 1)), hj);
            ai = vmlaq_n_f32(ai, vaddq_f32(vld1q_f32(li - j), vld1q_f32(li + j - 1)), hj);
        }
        float32x4x2_t y; y.val[0] = ar; y.val[1] = ai;
        vst2q_f32(o + 2 * k, y);
    }
#elif VIBE_SSE
    for (; m + 4 <= last + 1; m += 4, k += 4) {
        const float* lr = orr + m; const float* li = oi + m;
        const __m128 cc = _mm_set1_ps(c);
        __m128 ar = _mm_mul_ps(_mm_loadu_ps(er + m), cc), ai = _mm_mul_ps(_mm_loadu_ps(ei + m), cc);
        for (int j = 1; j <= P; ++j) {
            const __m128 hj = _mm_set1_ps(h[j]);
            ar = _mm_add_ps(ar, _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(lr - j), _mm_loadu_ps(lr + j - 1)), hj));
            ai = _mm_add_ps(ai, _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(li - j), _mm_loadu_ps(li + j - 1)), hj));
        }
        sseStore2(o + 2 * k, ar, ai);
    }
#endif
    for (; m <= last; ++m, ++k) {
        float ar = c * er[m], ai = c * ei[m];
        for (int j = 1; j <= P; ++j) {
            ar += h[j] * (orr[m - j] + orr[m + j - 1]);
            ai += h[j] * (oi[m - j] + oi[m + j - 1]);
        }
        out[k] = cf32(ar, ai);
    }
    // 3. keep the pairs from (next m − P) on — the next call's m starts at P again
    const size_t drop = (size_t)std::max(0, m - P);
    if (drop) {
        const size_t keep = len_ - drop;
        std::memmove(er, er + drop, keep * sizeof(float)); std::memmove(ei, ei + drop, keep * sizeof(float));
        std::memmove(orr, orr + drop, keep * sizeof(float)); std::memmove(oi, oi + drop, keep * sizeof(float));
        len_ = keep;
    }
    return k;
}

} // namespace vibedsp
