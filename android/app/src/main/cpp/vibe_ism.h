// vibe_ism.h — V12 ISM-band sensor decoder: the generic FRONT END (docs/v12/ISM-DECODER.md).
//
// Whole sub-band capture (433.92 / 868.6 / 315 / 915 MHz) → a sparse FFT energy detector that finds each burst's
// frequency → one narrow channel per burst (mix + integrate-and-dump to ~250 kS/s, from a pre-roll ring so the start
// is not lost) → at the end of the burst, an OOK envelope slice and an FSK discriminator slice, both as signed run
// lengths in µs (+mark / −space; for FSK +high tone / −low tone). The protocol decoders (vibe_ism_protocols.h) take the
// runs; nothing here knows a protocol.
//
// ★★★ CLEAN ROOM. Written from protocol FACTS only — no rtl_433 or other GPL decoder source was read. The DSP here is
//     textbook (Hann-windowed FFT power, a minimum-tracking noise floor, a complex NCO, integrate-and-dump, a polar
//     discriminator, a hysteresis slicer).
// ★★ Header-only and links nothing, like vibe_adsb.h, so test-ism builds with one g++ line (scripts/run-tests.sh).
// ★★ CPU: the detector FFTs one block every kHopUs (not every block), so its cost is a fraction of a full-rate FFT; a channel
//    costs a mix and an add per capture sample, and only while a burst is on the air. Measured in test-ism (CPU row) and
//    quoted in ISM-DECODER.md §CPU.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#if !defined(VIBE_FORCE_SCALAR) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
#include <arm_neon.h>
#define VIBE_ISM_NEON 1
#elif !defined(VIBE_FORCE_SCALAR) && (defined(__SSE2__) || defined(_M_X64))
#include <emmintrin.h>
#define VIBE_ISM_SSE2 1
#endif
#if defined(VIBE_DAB_PFFFT)
#include "vibe_dab_fft.h"
#endif
#if defined(VIBE_ISM_PROF)
#include <cstdio>
#include <cstdlib>
#include <time.h>
#endif

namespace vibeism {

/* ★ VIBE_ISM_PROF (2026-10-31, the Pi 2 thread split): where the front end's time goes, stage by stage, for ism-offline on
 *  the Pi itself. Compiled out of every real build — the server and the app never define it. */
#if defined(VIBE_ISM_PROF)
struct IsmProf {
    double copy = 0, detect = 0, mix = 0, slice = 0, decode = 0;   ///< seconds (monotonic; the bench is single-threaded). decode: the caller's onBurst
    uint64_t detects = 0, opens = 0, cancels = 0, promotes = 0, closeQuiet = 0, closeLong = 0, fewHits = 0, sliceNo = 0;
    uint64_t holds = 0, held = 0;                       ///< hogs held (kHoldQuietS); detections a hold swallowed
    uint64_t mixSamp = 0, sliceSamp = 0, longSamp = 0;  ///< capture samples mixed; channel samples sliced; of those, in "ran too long" channels
    bool log = std::getenv("VIBE_ISM_PROF_LOG") != nullptr;   ///< every channel close to stderr
};
inline double ismProfNow() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return double(t.tv_sec) + 1e-9 * double(t.tv_nsec); }
#define VIBE_ISM_P(...) __VA_ARGS__
#else
#define VIBE_ISM_P(...)
#endif

using cf = std::complex<float>;
/** Signed run lengths, µs: + = mark (carrier on / high tone), − = space (carrier off / low tone). */
using Runs = std::vector<int>;

/** One burst, demodulated two ways. The decoders try both. */
struct Burst {
    double tSec = 0;          ///< capture time of the burst's first sample (seconds since FrontEnd start)
    double offHz = 0;         ///< channel centre relative to the capture centre
    float  levelDb = -99;     ///< envelope peak (95th percentile), dBFS
    float  noiseDb = -99;     ///< the channel's level before the burst, dBFS
    bool   clipped = false;   ///< the capture neared full scale during the burst
    Runs   ook;               ///< envelope slice (always present) — the finest of ookAll
    Runs   fsk;               ///< discriminator slice — empty when the envelope is not constant enough to be FSK
    /** ★ The same burst sliced at several smoothings (finest first): a 17 kbps FSK bit and a 1 kHz Oregon pulse want
     *  very different filters, and a weak slow burst only slices cleanly when smoothed to its own speed. The decoders
     *  try each (decodeAll). */
    std::vector<Runs> ookAll, fskAll;
    float  carrierHz = 0;     ///< the carrier (OOK) or the tones' midpoint (FSK), relative to the channel centre
    float  fskDevHz = 0;      ///< half the distance between the two tones (0 = not FSK)
    float  sampleUs = 0;      ///< the slicer's time resolution
    /** ★ V12 wM-Bus (2026-10-26): the channel's complex baseband, kept only where the service asked for it
     *  (FrontEnd::setKeepBaseband — the 868 band) for a decoder that works on SAMPLES, not run lengths: a T1/C1 meter's
     *  chip is 10 µs, finer than any run-length slice (vibe_wmbus.h). Shared, so copying a Burst never copies it; freed
     *  when the last copy goes (as soon as the decoders return). Empty everywhere else. */
    std::shared_ptr<const std::vector<cf>> bb;
    double bbRate = 0;        ///< its sample rate
};

/** ★★ A complex multiply WITHOUT std::complex's NaN/Inf recovery (2026-10-31, the decoder SIMD audit). C99 Annex G,
 *  which std::complex<float>::operator* follows under GCC unless -fcx-limited-range, re-tests every product for NaN and
 *  branches to __mulsc3: on the Pi 2 (qemu-arm instruction count, ISM mode) the products in the per-sample loops cost
 *  more than the FFTs did, and the branch keeps the compiler from vectorising them. Our samples are always finite. */
inline cf cmulf(cf a, cf b) {
    return cf(a.real() * b.real() - a.imag() * b.imag(), a.real() * b.imag() + a.imag() * b.real());
}

/* ★★ THE PER-SAMPLE COMPLEX RUNS, FOUR AT A TIME (2026-10-31, the decoder SIMD audit). GCC never auto-vectorises a float
 *  loop for 32-bit ARM NEON (NEON flushes denormals, so it needs -funsafe-math-optimizations, which the server does not
 *  use) — on the Pi 2 every one of these is NEON by hand or scalar VFP. Scalar tails; same products as the scalar form. */
/** y[i] = a[i]·b[i], with b conjugated first (conjB) and/or the product conjugated (conjOut). */
inline void cmulRun(const cf* a, const cf* b, cf* y, int n, bool conjB, bool conjOut) {
    const float sb = conjB ? -1.0f : 1.0f, so = conjOut ? -1.0f : 1.0f;
    int i = 0;
#if VIBE_ISM_NEON
    const float32x4_t vsb = vdupq_n_f32(sb), vso = vdupq_n_f32(so);
    for (; i + 4 <= n; i += 4) {
        const float32x4x2_t x = vld2q_f32(reinterpret_cast<const float*>(a + i));
        float32x4x2_t h = vld2q_f32(reinterpret_cast<const float*>(b + i));
        h.val[1] = vmulq_f32(h.val[1], vsb);
        float32x4x2_t r;
        r.val[0] = vmlsq_f32(vmulq_f32(x.val[0], h.val[0]), x.val[1], h.val[1]);
        r.val[1] = vmulq_f32(vmlaq_f32(vmulq_f32(x.val[0], h.val[1]), x.val[1], h.val[0]), vso);
        vst2q_f32(reinterpret_cast<float*>(y + i), r);
    }
#elif VIBE_ISM_SSE2
    const __m128 vsb = _mm_setr_ps(1, sb, 1, sb), vso = _mm_setr_ps(1, so, 1, so);
    for (; i + 2 <= n; i += 2) {
        const __m128 x = _mm_loadu_ps(reinterpret_cast<const float*>(a + i));                  // r0 i0 r1 i1
        const __m128 h = _mm_mul_ps(_mm_loadu_ps(reinterpret_cast<const float*>(b + i)), vsb);
        const __m128 hr = _mm_shuffle_ps(h, h, _MM_SHUFFLE(2, 2, 0, 0)), hi = _mm_shuffle_ps(h, h, _MM_SHUFFLE(3, 3, 1, 1));
        const __m128 xs = _mm_shuffle_ps(x, x, _MM_SHUFFLE(2, 3, 0, 1));                       // i0 r0 i1 r1
        // r·hr − i·hi in lanes 0/2 (re), i·hr + r·hi in lanes 1/3 (im)
        const __m128 r = _mm_add_ps(_mm_mul_ps(x, hr), _mm_mul_ps(_mm_mul_ps(xs, hi), _mm_setr_ps(-1, 1, -1, 1)));
        _mm_storeu_ps(reinterpret_cast<float*>(y + i), _mm_mul_ps(r, vso));
    }
#endif
    for (; i < n; ++i) {
        const cf h(b[i].real(), sb * b[i].imag());
        const cf r = cmulf(a[i], h);
        y[i] = cf(r.real(), so * r.imag());
    }
}

/** y[i] = (xConj ? conj(x[i]) : x[i]) · rot·incⁱ; rot is left at rot·incⁿ. ★ Four rotators a step apart, each stepping
 *  by inc⁴: the one serial chain (rot ·= inc) was what kept this from being vector work. Renormalised once a run. */
inline void rotateRun(const cf* x, cf* y, int n, bool xConj, cf& rot, cf inc) {
    int i = 0;
    const float sx = xConj ? -1.0f : 1.0f;
#if VIBE_ISM_NEON
    if (n >= 8) {
        const cf r1 = cmulf(rot, inc), r2 = cmulf(r1, inc), r3 = cmulf(r2, inc);
        const cf i2 = cmulf(inc, inc), i4 = cmulf(i2, i2);
        const float rr[4] = { rot.real(), r1.real(), r2.real(), r3.real() }, ri[4] = { rot.imag(), r1.imag(), r2.imag(), r3.imag() };
        float32x4_t Rr = vld1q_f32(rr), Ri = vld1q_f32(ri);
        const float32x4_t Ir = vdupq_n_f32(i4.real()), Ii = vdupq_n_f32(i4.imag()), vsx = vdupq_n_f32(sx);
        for (; i + 4 <= n; i += 4) {
            float32x4x2_t v = vld2q_f32(reinterpret_cast<const float*>(x + i));
            v.val[1] = vmulq_f32(v.val[1], vsx);
            float32x4x2_t r;
            r.val[0] = vmlsq_f32(vmulq_f32(v.val[0], Rr), v.val[1], Ri);
            r.val[1] = vmlaq_f32(vmulq_f32(v.val[0], Ri), v.val[1], Rr);
            vst2q_f32(reinterpret_cast<float*>(y + i), r);
            const float32x4_t nr = vmlsq_f32(vmulq_f32(Rr, Ir), Ri, Ii);
            Ri = vmlaq_f32(vmulq_f32(Rr, Ii), Ri, Ir);
            Rr = nr;
        }
        rot = cf(vgetq_lane_f32(Rr, 0), vgetq_lane_f32(Ri, 0));
    }
#elif VIBE_ISM_SSE2
    if (n >= 8) {
        const cf r1 = cmulf(rot, inc), r2 = cmulf(r1, inc), r3 = cmulf(r2, inc);
        const cf i2 = cmulf(inc, inc), i4 = cmulf(i2, i2);
        __m128 Rr = _mm_setr_ps(rot.real(), r1.real(), r2.real(), r3.real()), Ri = _mm_setr_ps(rot.imag(), r1.imag(), r2.imag(), r3.imag());
        const __m128 Ir = _mm_set1_ps(i4.real()), Ii = _mm_set1_ps(i4.imag()), vsx = _mm_set1_ps(sx);
        for (; i + 4 <= n; i += 4) {
            const __m128 a = _mm_loadu_ps(reinterpret_cast<const float*>(x + i)), b = _mm_loadu_ps(reinterpret_cast<const float*>(x + i + 2));
            const __m128 vr = _mm_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0));
            const __m128 vi = _mm_mul_ps(_mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1)), vsx);
            const __m128 orr = _mm_sub_ps(_mm_mul_ps(vr, Rr), _mm_mul_ps(vi, Ri));
            const __m128 oi = _mm_add_ps(_mm_mul_ps(vr, Ri), _mm_mul_ps(vi, Rr));
            _mm_storeu_ps(reinterpret_cast<float*>(y + i), _mm_unpacklo_ps(orr, oi));
            _mm_storeu_ps(reinterpret_cast<float*>(y + i + 2), _mm_unpackhi_ps(orr, oi));
            const __m128 nr = _mm_sub_ps(_mm_mul_ps(Rr, Ir), _mm_mul_ps(Ri, Ii));
            Ri = _mm_add_ps(_mm_mul_ps(Rr, Ii), _mm_mul_ps(Ri, Ir));
            Rr = nr;
        }
        float lr[4], li[4]; _mm_storeu_ps(lr, Rr); _mm_storeu_ps(li, Ri);
        rot = cf(lr[0], li[0]);
    }
#endif
    for (; i < n; ++i) {
        y[i] = cmulf(cf(x[i].real(), sx * x[i].imag()), rot);
        rot = cmulf(rot, inc);
    }
    rot /= std::abs(rot);
}

/** A radix-2 FFT, forward, natural order (the DAB one's shape, kept local so this header links nothing). */
class SmallFft {
public:
    explicit SmallFft(int n) : n_(n), rev_(size_t(n)), tw_(size_t(n / 2)) {
        int bits = 0; while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) { int r = 0; for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b); rev_[size_t(i)] = r; }
        for (int i = 0; i < n / 2; ++i) { const double a = -2.0 * M_PI * i / n; tw_[size_t(i)] = cf(float(std::cos(a)), float(std::sin(a))); }
    }
    void forward(cf* x) const {
        for (int i = 0; i < n_; ++i) { const int r = rev_[size_t(i)]; if (r > i) std::swap(x[i], x[r]); }
        for (int len = 2; len <= n_; len <<= 1) {
            const int half = len >> 1, step = n_ / len;
            for (int i = 0; i < n_; i += len)
                for (int k = 0; k < half; ++k) {
                    const cf w = tw_[size_t(k * step)];
                    const cf u = x[i + k], v = x[i + k + half] * w;
                    x[i + k] = u + v; x[i + k + half] = u - v;
                }
        }
    }
private:
    int n_;
    std::vector<int> rev_;
    std::vector<cf> tw_;
};

/* ★★ THE DETECTOR'S FFT IS PFFFT WHERE THE BUILD LINKS IT (the server and the app define VIBE_DAB_PFFFT for DAB's OFDM —
 *  vibe_dab_fft.h), the radix-2 above where it does not (test-ism's one-line build). On the Pi 500's A76 the radix-2 was
 *  over half the front end's cost; PFFFT is NEON/SSE throughout. Same convention: forward, natural order, unscaled. */
#if defined(VIBE_DAB_PFFFT)
using DetFft = vibedab::Fft;
#else
using DetFft = SmallFft;
#endif

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE FRONT END — three stages (2026-10-31, the Pi 2 thread split)
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/* ★★★ WHY THREE STAGES. On the Pi 2 (the minimum spec) ISM 433 was ONE thread at 106 % of a core: it could not keep up
 *  and lost bursts, with three cores idle beside it. The work is a pipeline, and only the first stage must keep real
 *  time:
 *    1. Detector   — the FFT energy detector over the whole capture. Decides where a burst is, when it starts and ends,
 *                    and says so as ChanEvents (open / promote / cancel / close, each at a sample position). Mixes nothing.
 *    2. ChanMixer  — follows those events over the same samples: each burst's narrow channel (the triangle filter with the
 *                    mixer folded in), handed on as a RawBurst when it closes.
 *    3. the slicer — FrontEnd::finish: the OOK/FSK slices (slice()), then the decoders.
 *  FrontEnd runs all three inline (the tests, ism-offline); vibe_ism_service.h runs each on its own thread with a bounded
 *  queue between them, so a slow decoder lags, then drops, and never costs the detector its real time.
 * ★★ THE SAME BURSTS, BIT FOR BIT. A channel's output depends only on the samples it covers (start to close) and its
 *    frequency, never on when it was mixed: macRun carries its sums in floats across calls in sample order. So the mixer
 *    may run whenever it likes, as long as it applies each event at the event's own sample position. */
struct FeConst {
    static constexpr int    kN = 256;            ///< detector FFT size (≈ 9.4 kHz bins at 2.4 MS/s)
    /** How often the detector looks: one kN-point FFT every kHopUs of capture (~3 100 a second). ★ 32-BIT ARM (the Pi 2 —
     *  the minimum spec — and Pi 0/1/3 on armhf) looks half as often: the detector FFT is most of the front end's cost on
     *  an in-order core (Pi 500 A76: 2.0 of 3.4 % of a core), and a 10 ms burst still crosses ~15 frames. A TIME, not a
     *  block count: at 3 MS/s "6 blocks" was a 1 ms hop, and test-ism lost bursts at it. VIBE_ISM_HOP_US overrides. */
#if defined(VIBE_ISM_HOP_US)
    static constexpr double kHopUs = VIBE_ISM_HOP_US;
#elif defined(__arm__)
    static constexpr double kHopUs = 640;
#else
    static constexpr double kHopUs = 320;
#endif
    static constexpr double kChanRate = 250e3;   ///< a burst's channel rate (integrate-and-dump target)
    static constexpr double kPrerollS = 0.006;   ///< channel starts this far before the first detection
    static constexpr double kCloseS = 0.025;     ///< no energy for this long = the burst is over (PT2262's sync is ~15 ms)
    static constexpr double kMaxBurstS = 1.6;    ///< a carrier that never stops is cut here (and its floor rises)
    static constexpr double kMatchHz = 120e3;    ///< a detection this close to an open channel belongs to it (FSK's two tones — see below)
    /* ★★ 120 kHz, not 80 (2026-10-25). Stuart's car key, recorded on the Pi 500 (434.137 MHz), is FSK with its tones
     *  83 kHz apart: at 80 kHz each tone opened its own channel and neither was the whole signal. Wide-deviation FSK
     *  (car keys) needs the pair kept together; the 250 kHz channel holds both tones. test-ism unchanged (105/0). */
    static constexpr int    kMaxChans = 4;
    static constexpr float  kMinBurstSnrDb = 6.5f; ///< see slice(): noise alone reads ~4.5 dB
    static constexpr size_t kMaxRuns = 6000;      ///< more transitions than any sensor sends in 1.6 s = noise; not decoded
    /* ★★ A HOG IS HELD, NOT RE-OPENED (2026-10-31, the Pi 2 thread split). Something that keeps a channel busy for the
     *  whole kMaxBurstS — a carrier, a spur, a wandering emitter the floor cannot absorb because it moves between bins —
     *  used to be cut at 1.6 s and re-opened on the very next frame, for ever: a channel mixed and a 1.6 s slice, every
     *  1.6 s, decoding nothing. On Stuart's Pi 500 capture two such emitters held 43 % of every sample mixed; on a host
     *  with a poor antenna (the Pi 2 at 433, "OUTSIDE ANTENNA RANGE") several do. Now, when a channel runs the full
     *  1.6 s, its frequency is HELD: detections within kMatchHz of it refresh the hold and open nothing, until none has
     *  been seen there for kHoldQuietS. ★ NOTHING A SENSOR SENDS IS LOST BY IT: while the hog's channel was open, any
     *  burst within kMatchHz of it was folded into that channel anyway — and a burst standing kHoldOverDb clear of the
     *  hog's own peak still opens a channel of its own (better than before: it used to drown in the hog's). */
    static constexpr double kHoldQuietS = 0.2;    ///< a held frequency is released after this long with nothing there
    static constexpr float  kHoldOver = 4.0f;     ///< (6 dB) a newcomer this far over the hog's peak is a burst of its own
    static constexpr int    kMaxHolds = 8;
    /** The pre-decimation for a capture rate: anything above 2.6 MS/s (an Airspy at 10 MS/s) by an integer boxcar, so the
     *  detector and the channels cost the same whatever the radio runs at. The sensors sit near the middle, the boxcar's
     *  first null is at the new rate — plenty for a detector and a 250 kHz channel. */
    static int preFor(double rateHz) { return std::max(1, int(std::ceil(rateHz / 2.6e6))); }
};

/** What the detector tells the mixer, at work-sample position `at` (the detector's sample count when it decided). */
struct ChanEvent {
    enum Kind : uint8_t { kOpen, kPromote, kCancel, kClose };
    Kind kind = kOpen;
    uint8_t slot = 0;          ///< which of the kMaxChans channels
    uint32_t serial = 0;       ///< which opening of that slot (a mixer that lost the open ignores the rest)
    uint64_t at = 0;
    uint64_t start = 0;        ///< kOpen: the channel's first sample (the pre-roll before the detection)
    double offHz = 0;          ///< kOpen: its centre, relative to the capture centre
};

/** A closed channel, mixed and not yet sliced: the mixer's output, the slicer's input. */
struct RawBurst {
    double tSec = 0;           ///< capture time of its first sample
    double offHz = 0;
    double chanRate = 0;
    float peakAbs = 0;         ///< its capture samples' peak |re|, |im| (the clip flag)
    std::vector<cf> bb;        ///< the channel's complex baseband
};

/** max(pk, every |re| and |im| of x[0..n)) — the capture's overload meter. ★ NEON / SSE2: the scalar form's compares each
 *  moved a flag out of the VFP (vmrs), which stalls a Cortex-A7. A maximum is exact in any order, so all three agree. */
inline float absPeakRun(const cf* x, size_t n, float pk) {
    const float* f = reinterpret_cast<const float*>(x);
    size_t i = 0;
    const size_t nf = 2 * n;
#if VIBE_ISM_NEON
    if (nf >= 8) {
        float32x4_t m0 = vdupq_n_f32(pk), m1 = m0;
        for (; i + 8 <= nf; i += 8) {
            m0 = vmaxq_f32(m0, vabsq_f32(vld1q_f32(f + i)));
            m1 = vmaxq_f32(m1, vabsq_f32(vld1q_f32(f + i + 4)));
        }
        const float32x4_t m = vmaxq_f32(m0, m1);
        float32x2_t h = vpmax_f32(vget_low_f32(m), vget_high_f32(m));
        h = vpmax_f32(h, h);
        pk = vget_lane_f32(h, 0);
    }
#elif VIBE_ISM_SSE2
    if (nf >= 8) {
        const __m128 absMask = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
        __m128 m0 = _mm_set1_ps(pk), m1 = m0;
        for (; i + 8 <= nf; i += 8) {
            m0 = _mm_max_ps(m0, _mm_and_ps(_mm_loadu_ps(f + i), absMask));
            m1 = _mm_max_ps(m1, _mm_and_ps(_mm_loadu_ps(f + i + 4), absMask));
        }
        float l[4]; _mm_storeu_ps(l, _mm_max_ps(m0, m1));
        pk = std::max(std::max(l[0], l[1]), std::max(l[2], l[3]));
    }
#endif
    for (; i < nf; ++i) pk = std::max(pk, std::fabs(f[i]));
    return pk;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  STAGE 1 — THE DETECTOR (keeps real time; mixes nothing)
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class Detector : public FeConst {
public:
    float threshDb = 10.0f;                        ///< detection: a bin this far over its own noise floor
    bool holdHogs = true;                          ///< kHoldQuietS's rule (off only in test-ism, to compare against)
    /** Channels that ran the full kMaxBurstS, and detections a hold swallowed (never a channel), since start. */
    uint64_t longCloses() const { return longs_; }
    uint64_t heldHits() const { return heldN_; }

    explicit Detector(double rateHz) : fft_(kN) {
        pre_ = preFor(rateHz);
        w_ = rateHz / pre_;
        hist_.assign(kHist, cf(0, 0));
        win_.resize(kN);
        for (int i = 0; i < kN; ++i) win_[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / kN));
        floor_.assign(kN, 0.0f);
        pw_.assign(kN, 0.0f);
        fbuf_.resize(kN);
        hop_ = std::max(kN, int(std::lround(w_ * kHopUs * 1e-6)));
    }
    double workRate() const { return w_; }
    int pre() const { return pre_; }
    /** Work samples seen so far = the position of the next one. */
    uint64_t total() const { return total_; }
    /** The detector's median bin floor, dBFS per bin (for the status line and the gain rule). */
    double noiseDb() const { return noiseDb_; }
    /** Share of detector frames in the last second where any sample neared full scale — the gain rule's overload. */
    double clipFrac() const { return clipFrac_; }
    /** Ignore detections within this many Hz of the capture centre (the DC remnant). */
    void setDcGuardHz(double hz) { dcGuardHz_ = hz; tablesOk_ = false; }
    /** Detect only between these offsets from the capture centre (the band's own span): the tuner's filter skirts
     *  outside it are where the floor moves fastest and false bursts cluster (Pi 500 RTL, 433.92, 2026-10-25). */
    void setUsableHz(double lo, double hi) { useLo_ = lo; useHi_ = hi; tablesOk_ = false; }
#if defined(VIBE_ISM_PROF)
    IsmProf* prof_ = nullptr;
#endif

    /** Feed capture samples; what the channels must do is appended to `ev` (in sample order). Where the capture is
     *  pre-decimated (pre() > 1) the work samples are appended to `work` — the mixer's input; otherwise the mixer takes
     *  `x` itself, and `work` is not touched. */
    void feed(const cf* x, size_t n, std::vector<ChanEvent>& ev, std::vector<cf>* work) {
        if (pre_ > 1) {
            for (size_t i = 0; i < n; ++i) {
                preAcc_ += x[i]; if (++preCnt_ < pre_) continue;
                const cf v = preAcc_ * (1.0f / pre_); preAcc_ = 0; preCnt_ = 0;
                if (work) work->push_back(v);
                hist_[size_t(total_ & kHistMask)] = v;
                blockPeak_ = std::max(blockPeak_, std::max(std::fabs(v.real()), std::fabs(v.imag())));
                ++total_;
                if (++sinceFft_ >= hop_) { sinceFft_ = 0; detect(ev); }
            }
            return;
        }
        /* ★★ NO COPY. The window for each FFT is read straight from the block (and, where it starts before the block,
         *  from hist_ — the last kHist samples of the blocks before). The overload meter is a vector max over each run. */
        xCur_ = x; xBase_ = total_;
        size_t i = 0;
        while (i < n) {
            const size_t room = std::min(n - i, size_t(hop_ - sinceFft_));
            VIBE_ISM_P(const double tp0 = ismProfNow();)
            blockPeak_ = absPeakRun(x + i, room, blockPeak_);
            VIBE_ISM_P(if (prof_) prof_->copy += ismProfNow() - tp0;)
            total_ += room; sinceFft_ += int(room); i += room;
            if (sinceFft_ >= hop_) { sinceFft_ = 0; detect(ev); }
        }
        const size_t keep = std::min(n, kHist);
        for (size_t k = n - keep; k < n; ++k) hist_[size_t((xBase_ + k) & kHistMask)] = x[k];
        xCur_ = nullptr;
    }
    /** End of stream: every channel closes now (a pending one is dropped). */
    void flush(std::vector<ChanEvent>& ev) {
        for (int s = 0; s < kMaxChans; ++s) {
            DChan& c = ch_[s];
            if (!c.on) continue;
            emit(ev, c.pending || c.hits < 3 ? ChanEvent::kCancel : ChanEvent::kClose, s);
            c.on = false; c.pending = false;
        }
    }

private:
    static constexpr size_t kHist = 1024, kHistMask = kHist - 1;   ///< ≥ kN: the window before the current block
    /** The detector's view of a channel: where, since when, how often seen. The mixing is the mixer's. */
    struct DChan {
        bool on = false;
        /** ★★ A CANDIDATE until seen twice. One noise bin in a few thousand crosses 10 dB, and mixing a channel for 25 ms
         *  after each was ~75 % of the front end's whole cost on pure noise (M4: 2.8 % of a core → 0.8 %). Nothing is lost
         *  by waiting: the channel mixes from its pre-roll when promoted. */
        bool pending = false;
        double offHz = 0;
        uint64_t start = 0, lastHit = 0;
        int hits = 0;           ///< detector frames that saw it — a noise spike is one; a burst is many
        float peakPow = 0, framePow = 0;   ///< its strongest bin power, ever and in the current detector frame
        uint32_t serial = 0;
    };
    void emit(std::vector<ChanEvent>& ev, ChanEvent::Kind k, int slot) {
        ChanEvent e; e.kind = k; e.slot = uint8_t(slot); e.serial = ch_[slot].serial; e.at = total_;
        if (k == ChanEvent::kOpen) { e.start = ch_[slot].start; e.offHz = ch_[slot].offHz; }
        ev.push_back(e);
    }
    /** Capture sample `s` (absolute work position), from the current block or the history before it. */
    cf sampleAt(uint64_t s) const { return xCur_ && s >= xBase_ ? xCur_[s - xBase_] : hist_[size_t(s & kHistMask)]; }

    void detect(std::vector<ChanEvent>& ev) {
        VIBE_ISM_P(const double tp0 = ismProfNow(); if (prof_) ++prof_->detects;)
        detectBody(ev);
        VIBE_ISM_P(if (prof_) prof_->detect += ismProfNow() - tp0;)
    }
    void detectBody(std::vector<ChanEvent>& ev) {
        const uint64_t s0 = total_ - kN;
        for (int i = 0; i < kN; ++i) fbuf_[size_t(i)] = sampleAt(s0 + uint64_t(i)) * win_[size_t(i)];
        fft_.forward(fbuf_.data());
        const float norm = 1.0f / (float(kN) * float(kN) * 0.375f);   // Hann's power gain, so a bin reads as dBFS-ish
        for (int i = 0; i < kN; ++i) pw_[size_t(i)] = std::norm(fbuf_[size_t(i)]) * norm;
        // ★ the overload meter: a block of capture that neared full scale (an 8-bit RTL clips at 1.0)
        clipWin_ += blockPeak_ > 0.85f ? 1 : 0; ++clipFrames_; blockPeak_ = 0;
        if (frames_ < 64) {
            // ★ the first frames SEED the floor fast (signals present at start would otherwise stay "noise" for a while)
            for (int i = 0; i < kN; ++i) floor_[size_t(i)] = frames_ == 0 ? pw_[size_t(i)] : floor_[size_t(i)] + 0.15f * (pw_[size_t(i)] - floor_[size_t(i)]);
        }
        ++frames_;
        if (thrDbCached_ != threshDb) { thrDbCached_ = threshDb; thr_ = std::pow(10.0f, threshDb / 10.0f); }
        const float thr = thr_;
        if (!tablesOk_) buildTables();
        // groups of over-threshold bins, scanned in frequency order (negative first), one-bin gaps bridged
        struct G { double f, wsum; float peak, pmax; int width; };
        G groups[16]; int ng = 0;
        bool inG = false; int gapRun = 0; G cur{0, 0, 0, 0, 0};
        for (int j = 0; j < kN; ++j) {
            const int k = order_[size_t(j)];                // -N/2 .. N/2-1, as bins
            const float p = pw_[size_t(k)];
            // ★ no division: a bin is "over" when p > floor × thr; the ratio is worked out only for the few that are
            if (usable_[size_t(k)] && p > floor_[size_t(k)] * thr) {
                const float ex = p / floor_[size_t(k)];
                if (!inG) { cur = G{0, 0, 0, 0, 0}; inG = true; }
                gapRun = 0;
                cur.f += double(freq_[size_t(k)]) * p; cur.wsum += p;
                cur.peak = std::max(cur.peak, ex); cur.pmax = std::max(cur.pmax, p); ++cur.width;
            } else if (inG && ++gapRun > 1) {
                inG = false;
                if (ng < 16) groups[ng++] = cur;
            }
        }
        if (inG && ng < 16) groups[ng++] = cur;
        /* the floor: a CENSORED MEAN per bin — one FFT bin of noise is exponentially distributed, so a minimum-tracker
         *  settles at a fraction of the mean and calls 1 bin in 10 a signal. The mean of everything under 3× the floor
         *  (≈ 0.85 of the true mean), with whatever is over it — a signal — barely moving it. */
        for (int k = 0; k < kN; ++k) {
            float& fl = floor_[size_t(k)];
            const float p = pw_[size_t(k)];
            const float a = p < fl * 3.0f ? 0.01f : 0.0003f;
            fl = std::max(1e-14f, fl + a * (p - fl));
        }
        if ((frames_ & 255) == 0) {
            std::vector<float> t(floor_.begin(), floor_.end());
            std::nth_element(t.begin(), t.begin() + kN / 2, t.end());
            noiseDb_ = 10.0 * std::log10(std::max(1e-14f, t[size_t(kN / 2)]));
        }
        if (clipFrames_ >= int(w_ / hop_)) { clipFrac_ = double(clipWin_) / clipFrames_; clipWin_ = 0; clipFrames_ = 0; }

        // ★ strongest first, so a strong burst takes its channel before its own splatter can
        std::sort(groups, groups + ng, [](const G& a, const G& b) { return a.pmax > b.pmax; });
        for (auto& c : ch_) if (c.on) c.framePow = 0;
        for (int g = 0; g < ng; ++g) {
            const G& gr = groups[g];
            if (gr.wsum <= 0) continue;
            // ★ a single bin must be clearly strong; a wider group may be weaker — a real burst spreads over bins
            if (gr.width < 2 && gr.peak < thr * 3.0f) continue;
            const double f = gr.f / gr.wsum;
            int hit = -1;
            for (int s = 0; s < kMaxChans; ++s) if (ch_[s].on && std::fabs(ch_[s].offHz - f) < kMatchHz) { hit = s; break; }
            if (hit >= 0) {
                DChan& h = ch_[hit];
                h.lastHit = total_; ++h.hits; h.peakPow = std::max(h.peakPow, gr.pmax); h.framePow = std::max(h.framePow, gr.pmax);
                if (h.pending) { VIBE_ISM_P(if (prof_) ++prof_->promotes;) h.pending = false; emit(ev, ChanEvent::kPromote, hit); }   // ★ a second sighting: now it is worth mixing
                continue;
            }
            // ★★ a held hog (see kHoldQuietS): seen again — refresh it, open nothing, unless this stands well clear of it
            {
                int held = -1;
                for (int h = 0; h < kMaxHolds; ++h) if (hold_[h].on && std::fabs(hold_[h].f - f) < kMatchHz) { held = h; break; }
                if (held >= 0) {
                    Hold& hd = hold_[held];
                    hd.lastHit = total_;
                    if (gr.pmax <= hd.peak * kHoldOver) { ++heldN_; VIBE_ISM_P(if (prof_) ++prof_->held;) continue; }
                }
            }
            /* ★★ SPLATTER IS NOT A BURST. A strong OOK transmitter's keying edges spread −30…−45 dB sidebands hundreds of
             *  kHz either side; each would open a channel of its own. A newcomer 25 dB under a burst that is ON THE AIR
             *  while it is open is taken as its splatter (its keying edges splatter in the frames where it is
             *  switching, not only where it is on). (Two real sensors 25 dB apart at the same instant: the weak
             *  one is lost — the price, and rare.) */
            bool shadow = false;
            for (auto& c : ch_) if (c.on && c.peakPow > gr.pmax * 300.0f) { shadow = true; break; }
            if (shadow) continue;
            int freeS = -1;
            for (int s = 0; s < kMaxChans; ++s) if (!ch_[s].on) { freeS = s; break; }
            if (freeS < 0) continue;                           // ★ four bursts at once already: the fifth is lost
            VIBE_ISM_P(if (prof_) ++prof_->opens;)
            openChan(freeS, f, ev);
            ch_[freeS].peakPow = ch_[freeS].framePow = gr.pmax;
        }
        // close channels that went quiet, or ran too long
        for (int s = 0; s < kMaxChans; ++s) {
            DChan& c = ch_[s];
            if (!c.on) continue;
            if (c.pending) {
                if (double(total_ - c.lastHit) > std::max(3.0 * hop_, 0.004 * w_)) {
                    VIBE_ISM_P(if (prof_) ++prof_->cancels;)
                    c.on = false; c.pending = false; emit(ev, ChanEvent::kCancel, s);
                }
                continue;
            }
            const double quiet = double(total_ - c.lastHit) / w_;
            const double len = double(total_ - c.start) / w_;
            if (quiet > kCloseS || len > kMaxBurstS) {
                VIBE_ISM_P(if (prof_) { if (quiet > kCloseS) ++prof_->closeQuiet; else { ++prof_->closeLong; prof_->longSamp += total_ - c.start; }
                                        if (c.hits < 3) ++prof_->fewHits;
                                        if (prof_->log) std::fprintf(stderr, "close %9.3f s  %+9.1f kHz  len %6.1f ms  hits %5d  peak %5.1f dB  %s\n",
                                            double(c.start) / w_, c.offHz / 1e3, 1e3 * len, c.hits, 10.0 * std::log10(std::max(1e-20f, c.peakPow)),
                                            quiet > kCloseS ? "quiet" : "LONG"); })
                // ★ fewer than 3 detector frames = a noise spike (1 frame in ~0.3 ms; the shortest real burst spans dozens)
                c.on = false;
                emit(ev, c.hits >= 3 ? ChanEvent::kClose : ChanEvent::kCancel, s);
                if (quiet <= kCloseS) { ++longs_; if (holdHogs) holdAt(c.offHz, c.peakPow); }   // ★ it ran the full kMaxBurstS: hold it (kHoldQuietS)
            }
        }
        // release the holds that went quiet
        for (auto& hd : hold_) if (hd.on && double(total_ - hd.lastHit) > kHoldQuietS * w_) hd.on = false;
    }
    void holdAt(double f, float peak) {
        Hold* slot = nullptr;
        for (auto& hd : hold_) if (hd.on && std::fabs(hd.f - f) < kMatchHz) { slot = &hd; break; }   // the same hog again
        if (!slot) for (auto& hd : hold_) if (!hd.on) { slot = &hd; break; }
        if (!slot) { slot = &hold_[0]; for (auto& hd : hold_) if (hd.lastHit < slot->lastHit) slot = &hd; }   // full: the stalest
        if (!slot->on || std::fabs(slot->f - f) >= kMatchHz) { slot->peak = 0; VIBE_ISM_P(if (prof_) ++prof_->holds;) }
        slot->on = true; slot->f = f; slot->lastHit = total_; slot->peak = std::max(slot->peak, peak);
    }
public:
    /** Frequencies held right now (kHoldQuietS): carriers and spurs that are being ignored — for the status. */
    int holds() const { int n = 0; for (const auto& hd : hold_) n += hd.on ? 1 : 0; return n; }
private:
    /** The bin tables: scan order (most negative frequency first), each bin's frequency, and whether it may detect. */
    void buildTables() {
        const double binHz = w_ / kN;
        const int edge = int(kN * 0.05);
        order_.resize(kN); freq_.resize(kN); usable_.resize(kN);
        for (int j = 0; j < kN; ++j) order_[size_t(j)] = (j + kN / 2) % kN;
        for (int k = 0; k < kN; ++k) {
            const int sk = k < kN / 2 ? k : k - kN;
            const double f = sk * binHz;
            freq_[size_t(k)] = float(f);
            usable_[size_t(k)] = std::abs(sk) < kN / 2 - edge && std::fabs(f) > dcGuardHz_ && f >= useLo_ && f <= useHi_;
        }
        tablesOk_ = true;
    }
    void openChan(int s, double f, std::vector<ChanEvent>& ev) {
        DChan& c = ch_[s];
        c.on = true;
        c.offHz = f;
        const uint64_t pre = uint64_t(kPrerollS * w_) + kN;
        c.start = total_ > pre ? total_ - pre : 0;
        c.lastHit = total_;
        c.hits = 1;
        c.pending = true;
        c.peakPow = 0; c.framePow = 0;
        ++c.serial;
        emit(ev, ChanEvent::kOpen, s);
    }

    int pre_ = 1, preCnt_ = 0, hop_ = kN, sinceFft_ = 0;
    cf preAcc_{0, 0};
    double w_ = 0, dcGuardHz_ = 0, noiseDb_ = -99, clipFrac_ = 0, useLo_ = -1e12, useHi_ = 1e12;
    int clipWin_ = 0, clipFrames_ = 0;
    float blockPeak_ = 0;
    std::vector<cf> hist_;
    const cf* xCur_ = nullptr;
    uint64_t xBase_ = 0, total_ = 0, frames_ = 0;
    DetFft fft_;
    std::vector<float> win_, floor_, pw_, freq_;
    std::vector<int> order_;
    std::vector<uint8_t> usable_;
    bool tablesOk_ = false;
    float thr_ = 10.0f, thrDbCached_ = -1.0f;
    std::vector<cf> fbuf_;
    DChan ch_[kMaxChans];
    struct Hold { bool on = false; double f = 0; uint64_t lastHit = 0; float peak = 0; };
    Hold hold_[kMaxHolds];
    uint64_t longs_ = 0, heldN_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  STAGE 2 — THE CHANNEL MIXER (may lag the detector; follows its events at their sample positions)
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class ChanMixer : public FeConst {
public:
    explicit ChanMixer(double rateHz) {
        w_ = rateHz / preFor(rateHz);
        chanM_ = std::max(1, int(std::lround(w_ / kChanRate)));
        ring_.assign(size_t(1) << 18, cf(0, 0));      // ~109 ms at 2.4 MS/s
    }
    double chanRate() const { return w_ / chanM_; }
    /** ★ V12 wM-Bus: a wider burst channel — the work rate over the integer nearest `hz` (2.4 MS/s → 480 kS/s, 2.048 →
     *  512 k). Call before feeding. */
    void setChanRate(double hz) { chanM_ = std::max(1, int(std::lround(w_ / std::max(1.0, hz)))); }
    uint64_t total() const { return total_; }
    /** Times the input jumped (a block dropped before it): every open channel was abandoned. */
    uint64_t gaps() const { return gaps_; }
#if defined(VIBE_ISM_PROF)
    IsmProf* prof_ = nullptr;
#endif

    /** Work samples x[0..n), the first of them sample number `start`, and the detector's events for them (sorted, each
     *  at ≤ start + n). `onRaw(RawBurst&&)` is called for each channel that closes. ★ A `start` past where the mixer
     *  is means work was dropped in between: the channels open across the hole are abandoned, never stitched. */
    template <class F> void feed(const cf* x, size_t n, uint64_t start, const ChanEvent* ev, size_t nev, F&& onRaw) {
        if (start != total_) {
            for (auto& c : ch_) abandon(c);
            total_ = start; validFrom_ = start; ++gaps_;
        }
        const size_t size = ring_.size(), mask = size - 1;
        size_t i = 0, e = 0;
        for (;;) {
            while (e < nev && ev[e].at <= total_) apply(ev[e++], onRaw);
            if (i >= n) break;
            size_t room = std::min(n - i, 1024 - size_t(total_ & 1023));
            if (e < nev) room = std::min(room, size_t(ev[e].at - total_));
            VIBE_ISM_P(const double tp0 = ismProfNow();)
            const size_t pos = size_t(total_ & mask), a = std::min(room, size - pos);
            std::memcpy(ring_.data() + pos, x + i, a * sizeof(cf));
            if (a < room) std::memcpy(ring_.data(), x + i + a, (room - a) * sizeof(cf));
            VIBE_ISM_P(if (prof_) prof_->copy += ismProfNow() - tp0;)
            total_ += room; i += room;
            // ★ channels consume up to "now" every 1024 samples (and at an event), never more than the ring holds
            if ((total_ & 1023) == 0) for (auto& c : ch_) if (c.on && !c.pending) runChan(c, total_);
        }
        while (e < nev) apply(ev[e++], onRaw);   // ★ defensive: none should lie past the block
    }

private:
    struct Chan {
        bool on = false, pending = false;
        uint32_t serial = 0;
        double offHz = 0;
        cf rot{1, 0}, incM{1, 0};   ///< the mixer at the current output's first sample, and its step per output
        int M = 1, mcnt = 0, renorm = 0;
        std::vector<float> wA, wB;  ///< the triangle's falling / rising weights × incʲ, and × j (runChan, macRun)
        cf sF{0, 0}, sR{0, 0}, carry{0, 0};
        uint64_t next = 0;      ///< the next working sample (absolute) this channel has not yet consumed
        uint64_t start = 0;
        float peakAbs = 0;
        std::vector<cf> bb;
    };
    void abandon(Chan& c) { c.on = false; c.pending = false; c.bb.clear(); c.bb.shrink_to_fit(); }

    template <class F> void apply(const ChanEvent& e, F& onRaw) {
        Chan& c = ch_[e.slot];
        const uint64_t at = std::min(e.at, total_);
        if (e.kind == ChanEvent::kOpen) { openChan(c, e); return; }
        if (!c.on || c.serial != e.serial) return;      // ★ its open was lost with a dropped block
        if (e.kind == ChanEvent::kPromote) { c.pending = false; runChan(c, at); return; }
        if (e.kind == ChanEvent::kCancel) { abandon(c); return; }
        runChan(c, at);
        c.on = false;
        RawBurst r;
        r.tSec = double(c.start) / w_;
        r.offHz = c.offHz;
        r.chanRate = chanRate();
        r.peakAbs = c.peakAbs;
        r.bb = std::move(c.bb);     // ★ moved, not copied: a new buffer is reserved at the next open
        c.bb = std::vector<cf>();
        onRaw(std::move(r));
    }

    /* ★★ THE CHANNEL FILTER IS A TRIANGLE (a second-order CIC, done as two weighted accumulators — no integrator to drift).
     *  A plain integrate-and-dump of M samples leaks a strong burst one output rate away straight onto DC (its sinc's
     *  null is only exact AT k·R), and test-ism's TPMS, 240 kHz from an empty channel, decoded twice. The triangle's
     *  sinc² halves every sidelobe in dB for one more multiply-add per sample. */
    /** One run of a channel's triangle: `n` ring samples `x` into the four sums acc = {falling re, im, rising re, im},
     *  weights wA/wB (4 floats a sample: the falling and rising weights × incʲ, and the same × j — see openChan), and
     *  the run's peak |re|,|im|. ★ NEON / SSE2: two multiply-adds of four lanes a sample and a vector max — the scalar
     *  form's compares each moved a flag out of the VFP (vmrs), which stalls a Cortex-A7. Same products, same order. */
    static void macRun(const cf* x, int n, const float* wA, const float* wB, float* acc, float& pk) {
#if VIBE_ISM_NEON
        float32x4_t s = vld1q_f32(acc);
        float32x2_t m = vdup_n_f32(pk);
        for (int k = 0; k < n; ++k) {
            const float32x2_t v = vld1_f32(reinterpret_cast<const float*>(x + k));
            m = vmax_f32(m, vabs_f32(v));
            s = vmlaq_lane_f32(s, vld1q_f32(wA + 4 * k), v, 0);
            s = vmlaq_lane_f32(s, vld1q_f32(wB + 4 * k), v, 1);
        }
        vst1q_f32(acc, s);
        pk = std::max(vget_lane_f32(m, 0), vget_lane_f32(m, 1));
#elif VIBE_ISM_SSE2
        __m128 s = _mm_loadu_ps(acc);
        __m128 m = _mm_set1_ps(pk);
        const __m128 absMask = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
        for (int k = 0; k < n; ++k) {
            const float* v = reinterpret_cast<const float*>(x + k);
            const __m128 re = _mm_set1_ps(v[0]), im = _mm_set1_ps(v[1]);
            m = _mm_max_ps(m, _mm_and_ps(_mm_unpacklo_ps(re, im), absMask));
            s = _mm_add_ps(s, _mm_add_ps(_mm_mul_ps(re, _mm_loadu_ps(wA + 4 * k)), _mm_mul_ps(im, _mm_loadu_ps(wB + 4 * k))));
        }
        _mm_storeu_ps(acc, s);
        float l[4]; _mm_storeu_ps(l, m);
        pk = std::max(std::max(l[0], l[1]), pk);
#else
        for (int k = 0; k < n; ++k) {
            const float re = x[k].real(), im = x[k].imag();
            pk = std::max(pk, std::max(std::fabs(re), std::fabs(im)));
            for (int l = 0; l < 4; ++l) acc[l] += re * wA[4 * k + l] + im * wB[4 * k + l];
        }
#endif
    }

    void runChan(Chan& c, uint64_t upto) {
        /* ★★ THE TRIANGLE WITH THE MIXER FOLDED INTO ITS WEIGHTS (2026-10-31, the decoder SIMD audit). Per sample this was
         *  v·rot, rot·inc and two weighted accumulates — three std::complex products, each NaN-checked, and a recurrence
         *  the compiler could not vectorise: ~15 % of ISM mode on the Pi 2 (qemu-arm count, Stuart's 433.92 capture). Over
         *  one output's M samples the rotator is rot₀·incʲ, so the falling and rising weights × incʲ are tables built when
         *  the channel opens; a sample is then two four-lane multiply-adds (macRun), and the rotator steps once an OUTPUT
         *  (rot₀ ·= inc^M), renormalised as before. Same sums, in another order. */
        VIBE_ISM_P(const double tp0 = ismProfNow(); if (prof_) prof_->mixSamp += upto - c.next;)
        const size_t size = ring_.size(), mask = size - 1;
        const int M = c.M;
        const float norm = 1.0f / (float(M) * float(M + 1));
        const cf* ring = ring_.data();
        float acc[4] = { c.sF.real(), c.sF.imag(), c.sR.real(), c.sR.imag() };
        float pk = c.peakAbs;
        int j = c.mcnt;
        for (uint64_t s = c.next; s < upto;) {
            const size_t pos = size_t(s & mask);
            // a run inside one output and one unbroken stretch of the ring
            const int n = int(std::min<uint64_t>(std::min<uint64_t>(uint64_t(M - j), upto - s), uint64_t(size - pos)));
            macRun(ring + pos, n, c.wA.data() + 4 * j, c.wB.data() + 4 * j, acc, pk);
            s += uint64_t(n); j += n;
            if (j >= M) {
                c.bb.push_back((c.carry + cmulf(c.rot, cf(acc[0], acc[1]))) * norm);
                c.carry = cmulf(c.rot, cf(acc[2], acc[3]));
                c.rot = cmulf(c.rot, c.incM);
                acc[0] = acc[1] = acc[2] = acc[3] = 0; j = 0;
                if (++c.renorm >= 256) { c.renorm = 0; c.rot /= std::abs(c.rot); }
            }
        }
        c.sF = cf(acc[0], acc[1]); c.sR = cf(acc[2], acc[3]); c.mcnt = j; c.peakAbs = pk;
        if (upto > c.next) c.next = upto;
        VIBE_ISM_P(if (prof_) prof_->mix += ismProfNow() - tp0;)
    }

    void openChan(Chan& c, const ChanEvent& e) {
        c.on = true;
        c.pending = true;
        c.serial = e.serial;
        c.offHz = e.offHz;
        c.start = std::max(e.start, validFrom_);   // ★ never into the hole a dropped block left
        c.next = c.start;
        const double ph = -2.0 * M_PI * e.offHz / w_;
        c.M = chanM_;
        c.wA.resize(size_t(4 * c.M)); c.wB.resize(size_t(4 * c.M));
        for (int j = 0; j < c.M; ++j) {   // incʲ in double: the table is exact where the per-sample recurrence drifted
            const double cr = std::cos(ph * j), ci = std::sin(ph * j);
            const float f = float(c.M - j), r = float(j + 1);   // the falling half of THIS output, the rising of the NEXT
            const float a[4] = { float(cr) * f, float(ci) * f, float(cr) * r, float(ci) * r };
            // x·w = re·(w) + im·(j·w): wA is w, wB is j·w = (−w.im, w.re)
            const float b[4] = { -a[1], a[0], -a[3], a[2] };
            for (int l = 0; l < 4; ++l) { c.wA[size_t(4 * j + l)] = a[l]; c.wB[size_t(4 * j + l)] = b[l]; }
        }
        c.incM = cf(float(std::cos(ph * c.M)), float(std::sin(ph * c.M)));
        c.rot = cf(1, 0);
        c.mcnt = 0; c.renorm = 0; c.sF = 0; c.sR = 0; c.carry = 0;
        c.peakAbs = 0;
        c.bb.clear();
        c.bb.reserve(size_t(0.1 * chanRate()));
    }

    int chanM_ = 1;
    double w_ = 0;
    std::vector<cf> ring_;
    uint64_t total_ = 0, validFrom_ = 0, gaps_ = 0;
    Chan ch_[kMaxChans];
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE FRONT END — the three stages inline (the tests, ism-offline); the service threads them
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class FrontEnd : public FeConst {
public:
    float threshDb = 10.0f;                        ///< detection: a bin this far over its own noise floor

    explicit FrontEnd(double rateHz) : det_(rateHz), mix_(rateHz) {
#if defined(VIBE_ISM_PROF)
        det_.prof_ = &prof; mix_.prof_ = &prof;
#endif
    }

    double workRate() const { return det_.workRate(); }
    double chanRate() const { return mix_.chanRate(); }
    /** ★ V12 wM-Bus: a wider burst channel — the work rate over the integer nearest `hz` (2.4 MS/s → 480 kS/s, 2.048 →
     *  512 k). The 868 band asks for ~500 kS/s: a T1/C1 chip is 10 µs, 2.5 samples at 250 kS/s. The slicer's smoothing is
     *  in µs (slice()), so every other family slices as it did at 250 k. Call before feeding. */
    void setChanRate(double hz) { mix_.setChanRate(hz); }
    /** ★ V12 wM-Bus: hand each burst's complex baseband on in Burst::bb (the 868 band only — see Burst::bb). */
    void setKeepBaseband(bool on) { keepBb_ = on; }
    uint64_t bursts() const { return bursts_; }
#if defined(VIBE_ISM_PROF)
    IsmProf prof;
#endif
    /** The detector's median bin floor, dBFS per bin (for the status line and the gain rule). */
    double noiseDb() const { return det_.noiseDb(); }
    /** Carriers / spurs held right now (FeConst::kHoldQuietS); channels that ran the full 1.6 s; detections held. */
    int holds() const { return det_.holds(); }
    uint64_t longCloses() const { return det_.longCloses(); }
    uint64_t heldHits() const { return det_.heldHits(); }
    /** ★ Tests only: the hog rule off, to compare against (FeConst::kHoldQuietS). */
    void setHoldHogs(bool on) { det_.holdHogs = on; }
    /** Share of detector frames in the last second where any sample neared full scale — the gain rule's overload. */
    double clipFrac() const { return det_.clipFrac(); }
    /** Ignore detections within this many Hz of the capture centre (the DC remnant). */
    void setDcGuardHz(double hz) { det_.setDcGuardHz(hz); }
    /** Detect only between these offsets from the capture centre (the band's own span). */
    void setUsableHz(double lo, double hi) { det_.setUsableHz(lo, hi); }

    /** Feed capture samples. `onBurst(const Burst&)` is called on THIS thread when a burst ends. */
    template <class F> void feed(const cf* x, size_t n, F&& onBurst) {
        ev_.clear(); work_.clear();
        det_.threshDb = threshDb;
        const uint64_t s0 = det_.total();
        const bool pre = det_.pre() > 1;
        det_.feed(x, n, ev_, pre ? &work_ : nullptr);
        mix_.feed(pre ? work_.data() : x, pre ? work_.size() : n, s0, ev_.data(), ev_.size(),
                  [&](RawBurst&& r) { deliver(std::move(r), onBurst); });
    }
    /** Close every open channel now (end of stream). */
    template <class F> void flush(F&& onBurst) {
        ev_.clear();
        det_.flush(ev_);
        mix_.feed(nullptr, 0, mix_.total(), ev_.data(), ev_.size(), [&](RawBurst&& r) { deliver(std::move(r), onBurst); });
    }

    /** ★ STAGE 3's first half: a mixed channel → a sliced Burst (false = nothing worth decoding). `keepBb`: hand the
     *  baseband on (the 868 band). The service calls this on its decoder thread. */
    static bool finish(RawBurst&& r, bool keepBb, Burst& b) {
        b.tSec = r.tSec;
        b.offHz = r.offHz;
        b.clipped = r.peakAbs > 0.85f;
        if (!slice(r.bb, r.chanRate, b)) return false;
        if (keepBb) { b.bb = std::make_shared<const std::vector<cf>>(std::move(r.bb)); b.bbRate = r.chanRate; }
        return true;
    }
    /** A centred moving average of `w` taps (w odd, ≥ 1). */
    static void boxcar(const std::vector<float>& x, size_t w, std::vector<float>& y) {
        const size_t n = x.size(), h = w / 2;
        y.assign(n, 0.0f);
        double acc = 0; size_t lo = 0, hi = 0;   // window [lo, hi)
        for (size_t i = 0; i < n; ++i) {
            const size_t a = i >= h ? i - h : 0, e = std::min(n, i + h + 1);
            while (hi < e) acc += x[hi++];
            while (lo < a) acc -= x[lo++];
            y[i] = float(acc / double(hi - lo));
        }
    }
    /** The shortest-run cluster of a run list, µs (0 if too few runs). */
    static double shortestRun(const Runs& r) {
        if (r.size() < 12) return 0;
        std::vector<int> a; a.reserve(r.size());
        for (int d : r) a.push_back(std::abs(d));
        std::sort(a.begin(), a.end());
        return a[a.size() / 8];
    }

    /** ★ The slicer, public for the tests: baseband at `rate` → b.ook / b.fsk. False = nothing worth decoding.
     *  ★★ TWO PASSES. The channel is ~250 kHz wide because a burst may be anywhere in it and as fast as 17 kbps, but an
     *     Oregon sensor's shortest pulse is ~350 µs: slicing it at 4 µs resolution throws ~15 dB of SNR away. Pass 1
     *     slices lightly smoothed; the burst's own shortest run then sets a smoothing of a fifth of it, and pass 2 slices
     *     again — each burst is filtered to its own speed (a matched filter in all but name). */
    static bool slice(const std::vector<cf>& bb, double rate, Burst& b) {
        const size_t n = bb.size();
        if (n < 64) return false;
        const double us = 1e6 / rate;
        b.sampleUs = float(us);
        std::vector<float> env(n);
        for (size_t i = 0; i < n; ++i) env[i] = std::abs(bb[i]);
        std::vector<float> sm;
        std::vector<uint8_t> on(n), on0;
        b.ookAll.clear(); b.fskAll.clear();
        /* ★★ THE SMOOTHINGS ARE TIMES (V12 wM-Bus, 2026-10-26): 12, 36, 100, 244 µs — what {3, 9, 25, 61} taps were at
         *  250 kS/s. The 868 band's channel is ~500 kS/s now (setChanRate), and a La Crosse or a Fine Offset there must
         *  slice exactly as before: the window is the odd tap count nearest the time at THIS rate. */
        static const double kWinUs[4] = {12, 36, 100, 244};
        auto taps = [&](double wus) { const size_t t = size_t(std::max(1L, std::lround(wus / us))); return t | 1; };
        const size_t kWins[4] = {taps(kWinUs[0]), taps(kWinUs[1]), taps(kWinUs[2]), taps(kWinUs[3])};
        const size_t env9 = taps(36);
        // the level before the burst (the pre-roll's first ~2 ms) and the burst's own level, from the 9-tap envelope
        {
            boxcar(env, env9, sm);
            const size_t preN = std::min(n / 4, size_t(rate * 0.002));
            std::vector<float> t(sm.begin(), sm.begin() + std::max<size_t>(preN, 8));
            std::sort(t.begin(), t.end());
            float noise = t[t.size() / 2];
            std::vector<float> all(sm);
            std::nth_element(all.begin(), all.begin() + size_t(n * 0.95), all.end());
            const float sig = all[size_t(n * 0.95)];
            std::nth_element(all.begin(), all.begin() + size_t(n * 0.10), all.end());
            noise = std::min(noise, std::max(all[size_t(n * 0.10)], 1e-9f));
            b.levelDb = 20.0f * std::log10(std::max(sig, 1e-9f));
            b.noiseDb = 20.0f * std::log10(std::max(noise, 1e-9f));
            /* ★★ NOISE LOOKS LIKE THIS: the 95th percentile of a smoothed Rayleigh envelope sits ~4.5 dB over its median,
             *  and that is what every false detection measured on Stuart's Pi 500 RTL at 433.92 (4.4–5.4 dB, ~5 a second
             *  under VibeAGC's gain). A sensor worth slicing stands clear of it; the weakest test-ism decodes read 7+ dB. */
            if (b.levelDb - b.noiseDb < kMinBurstSnrDb) return false;
        }
        for (size_t wi = 0; wi < 4; ++wi) {
            const size_t win = kWins[wi];
            if (win * us > 0.5 * 1e6 * double(n) / rate / 8) break;   // a window longer than the burst's eighth says nothing
            boxcar(env, win, sm);
            const size_t preN = std::min(n / 4, size_t(rate * 0.002));
            std::vector<float> t(sm.begin(), sm.begin() + std::max<size_t>(preN, 8));
            std::sort(t.begin(), t.end());
            float noise = t[t.size() / 2];
            std::vector<float> all(sm);
            std::nth_element(all.begin(), all.begin() + size_t(n * 0.95), all.end());
            const float sig = all[size_t(n * 0.95)];
            std::nth_element(all.begin(), all.begin() + size_t(n * 0.10), all.end());
            noise = std::min(noise, std::max(all[size_t(n * 0.10)], 1e-9f));
            if (noise <= 0) noise = 1e-9f;
            if (sig < noise * 1.6f) continue;              // under 4 dB at this smoothing: nothing a slicer can use
            const float thr = std::sqrt(sig * noise);     // the midpoint in dB
            const float hi = thr * 1.1f, lo = thr / 1.1f;
            bool st = false;
            for (size_t i = 0; i < n; ++i) { if (!st && sm[i] > hi) st = true; else if (st && sm[i] < lo) st = false; on[i] = st; }
            Runs r = runsOf(on, us, false, int(win / 2) + int(std::lround(8.0 / us)));
            if (r.empty()) continue;
            if (r.size() > kMaxRuns) { if (wi == 0) return false; continue; }
            if (on0.empty()) on0 = on;                      // the finest usable slice marks where the carrier is
            b.ookAll.push_back(std::move(r));
        }
        if (b.ookAll.empty()) return false;
        b.ook = b.ookAll.front();
        on = on0;
        // ── FSK: the discriminator, over the stretch where the carrier is up
        size_t first = n, last = 0, cnt = 0;
        for (size_t i = 0; i < n; ++i) if (on[i]) { if (first == n) first = i; last = i; ++cnt; }
        b.fsk.clear(); b.fskDevHz = 0;
        if (first < last) {
            // the carrier: the mean instantaneous frequency where it is up (refines the detector's bin centroid)
            double acc = 0; size_t na = 0;
            for (size_t i = first + 1; i <= last; ++i) if (on[i] && on[i - 1]) { acc += std::arg(bb[i] * std::conj(bb[i - 1])); ++na; }
            if (na) b.carrierHz = float(acc / double(na) * rate / (2 * M_PI));
        }
        if (first < last && last - first > 32 && double(cnt) / double(last - first + 1) > 0.85) {
            const size_t m = last - first + 1;
            std::vector<float> fq(m);
            const float k = float(rate / (2 * M_PI));
            for (size_t i = 0; i < m; ++i) {
                const size_t j = first + i;
                fq[i] = j ? std::arg(bb[j] * std::conj(bb[j - 1])) * k : 0.0f;
            }
            std::vector<float> fs;
            const size_t kFw[3] = {kWins[0], kWins[1], kWins[2]};   // ★ 12, 36, 100 µs, as kWins
            for (size_t wi = 0; wi < 3; ++wi) {
                boxcar(fq, kFw[wi], fs);
                std::vector<float> s2(fs);
                std::nth_element(s2.begin(), s2.begin() + size_t(m * 0.1), s2.end());
                const float p10 = s2[size_t(m * 0.1)];
                std::nth_element(s2.begin(), s2.begin() + size_t(m * 0.9), s2.end());
                const float p90 = s2[size_t(m * 0.9)];
                const float dev = 0.5f * (p90 - p10), mid = 0.5f * (p90 + p10);
                if (dev <= 4000.0f) continue;
                if (b.fskAll.empty()) { b.fskDevHz = dev; b.carrierHz = mid; }
                std::vector<uint8_t> hiTone(m);
                bool sv = fs[0] > mid;
                const float hy = dev * 0.15f;
                for (size_t i = 0; i < m; ++i) {
                    if (!sv && fs[i] > mid + hy) sv = true; else if (sv && fs[i] < mid - hy) sv = false;
                    hiTone[i] = sv;
                }
                b.fskAll.push_back(runsOf(hiTone, us, /*leadingSpaceOk=*/true, int(kFw[wi] / 2) + int(std::lround(8.0 / us))));
            }
            if (!b.fskAll.empty()) b.fsk = b.fskAll.front();
        }
        return !b.ook.empty();
    }

    /** Level runs → signed µs runs, with one- and two-sample glitches folded into their neighbours. */
    static Runs runsOf(const std::vector<uint8_t>& lv, double us, bool leadingSpaceOk = false, int glitch = 2) {
        std::vector<std::pair<uint8_t, int>> r;
        for (size_t i = 0; i < lv.size(); ++i) {
            if (r.empty() || r.back().first != lv[i]) r.push_back({lv[i], 1});
            else ++r.back().second;
        }
        // fold glitches (≤ `glitch` samples) that sit between two runs of the other level
        std::vector<std::pair<uint8_t, int>> g;
        for (size_t i = 0; i < r.size(); ++i) {
            if (r[i].second <= glitch && !g.empty() && i + 1 < r.size()) {
                g.back().second += r[i].second + r[i + 1].second;
                ++i;
                continue;
            }
            if (!g.empty() && g.back().first == r[i].first) g.back().second += r[i].second;
            else g.push_back(r[i]);
        }
        Runs out;
        bool started = leadingSpaceOk;
        for (auto& p : g) {
            if (!started && !p.first) continue;            // OOK: the runs start at the first mark
            started = true;
            const int d = int(std::lround(p.second * us));
            out.push_back(p.first ? d : -d);
        }
        return out;
    }

private:
    template <class F> void deliver(RawBurst&& r, F& onBurst) {
        VIBE_ISM_P(const double tp0 = ismProfNow(); prof.sliceSamp += r.bb.size();)
        Burst b;
        const bool ok = finish(std::move(r), keepBb_, b);
        VIBE_ISM_P(prof.slice += ismProfNow() - tp0; if (!ok) ++prof.sliceNo;)
        if (!ok) return;
        ++bursts_;
        onBurst(static_cast<const Burst&>(b));
    }

    Detector det_;
    ChanMixer mix_;
    std::vector<ChanEvent> ev_;
    std::vector<cf> work_;
    bool keepBb_ = false;
    uint64_t bursts_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  SLICERS — runs → bits. Every protocol decoder is built from these.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
using Bits = std::vector<uint8_t>;   ///< one bit per element, in order of transmission

inline bool near(int d, double want, double tol) { return std::fabs(std::abs(d) - want) <= want * tol; }

/** PWM: the MARK width carries the bit; a space longer than `rowGapUs` ends a row. `shortIsOne` as the chip says. */
inline std::vector<Bits> slicePwm(const Runs& r, double shortUs, double longUs, double rowGapUs, bool shortIsOne,
                                  double tol = 0.4) {
    std::vector<Bits> rows(1);
    for (size_t i = 0; i < r.size(); ++i) {
        const int d = r[i];
        if (d > 0) {
            const double ds = std::fabs(d - shortUs), dl = std::fabs(d - longUs);
            const bool isShort = ds < dl;
            if ((isShort && ds > shortUs * tol) || (!isShort && dl > longUs * tol)) { if (!rows.back().empty()) rows.emplace_back(); continue; }
            rows.back().push_back(uint8_t(isShort == shortIsOne));
        } else if (-d > rowGapUs && !rows.back().empty()) rows.emplace_back();
    }
    if (rows.back().empty()) rows.pop_back();
    return rows;
}

/** PPM: the SPACE after each mark carries the bit (short = 0, long = 1 unless `shortIsOne`); a longer space ends a row.
 *  The last mark of a row has no space to read and carries nothing. */
inline std::vector<Bits> slicePpm(const Runs& r, double shortGapUs, double longGapUs, double rowGapUs, bool shortIsOne = false,
                                  double tol = 0.35) {
    std::vector<Bits> rows(1);
    for (size_t i = 0; i < r.size(); ++i) {
        const int d = r[i];
        if (d > 0) continue;
        const double g = -d;
        if (g > rowGapUs) { if (!rows.back().empty()) rows.emplace_back(); continue; }
        const double ds = std::fabs(g - shortGapUs), dl = std::fabs(g - longGapUs);
        const bool isShort = ds < dl;
        if ((isShort && ds > shortGapUs * tol) || (!isShort && dl > longGapUs * tol)) { if (!rows.back().empty()) rows.emplace_back(); continue; }
        rows.back().push_back(uint8_t(isShort == shortIsOne));
    }
    if (rows.back().empty()) rows.pop_back();
    return rows;
}

/** NRZ: each run is round(d / bitUs) bits of its level (mark = 1). A run over `maxBits` ends a row. */
inline std::vector<Bits> sliceNrz(const Runs& r, double bitUs, int maxBits = 24) {
    std::vector<Bits> rows(1);
    for (int d : r) {
        const int nb = int(std::lround(std::abs(d) / bitUs));
        if (nb > maxBits || nb == 0) { if (!rows.back().empty()) rows.emplace_back(); continue; }
        for (int k = 0; k < nb; ++k) rows.back().push_back(d > 0 ? 1 : 0);
    }
    if (rows.back().empty()) rows.pop_back();
    return rows;
}

/** Runs → HALF-BIT levels for Manchester: each run is 1 or 2 half-bits (a short or a long); anything else ends a row.
 *  Separate short/long limits for marks and spaces, because many transmitters trim their pulses (Oregon: ~100 µs). */
struct HalfLimits { double markShortMax, markLongMax, spaceShortMax, spaceLongMax, markMin, spaceMin; };
inline HalfLimits halfLimits(double halfUs, double markTrimUs = 0) {
    return { 1.5 * halfUs - markTrimUs, 2.5 * halfUs - markTrimUs, 1.5 * halfUs + markTrimUs, 2.5 * halfUs + markTrimUs,
             0.4 * halfUs - markTrimUs * 0.5, 0.4 * halfUs };
}
inline std::vector<Bits> halfBits(const Runs& r, const HalfLimits& L) {
    std::vector<Bits> rows(1);
    for (int d : r) {
        const bool m = d > 0; const double a = std::abs(d);
        int k = 0;
        if (m) k = a < L.markMin ? -1 : a <= L.markShortMax ? 1 : a <= L.markLongMax ? 2 : 0;
        else   k = a < L.spaceMin ? -1 : a <= L.spaceShortMax ? 1 : a <= L.spaceLongMax ? 2 : 0;
        if (k <= 0) {
            // ★ a long run ends the row, but its first half-bit still belongs to it (the last bit of a frame is
            //   followed by silence: mark-then-nothing must still read as the bit's first half)
            if (k == 0 && !rows.back().empty()) { rows.back().push_back(m ? 1 : 0); rows.emplace_back(); }
            else if (!rows.back().empty()) rows.emplace_back();
            continue;
        }
        for (int i = 0; i < k; ++i) rows.back().push_back(m ? 1 : 0);
    }
    if (rows.back().empty()) rows.pop_back();
    return rows;
}
/** Half-bit levels → bits, pairs from `phase` (0/1). `hiFirstIsOne`: (1,0) = 1 (IEEE 802.3 calls that 0 — conventions
 *  differ by vendor; each decoder says which). Stops at the first violation (a pair of equal halves). */
inline Bits manchester(const Bits& h, int phase, bool hiFirstIsOne) {
    Bits out;
    for (size_t i = size_t(phase); i + 1 < h.size(); i += 2) {
        if (h[i] == h[i + 1]) break;
        out.push_back(uint8_t((h[i] == 1) == hiFirstIsOne));
    }
    return out;
}
/** Differential Manchester: a transition in the middle of every bit; bit = whether the level CHANGED at the bit's start
 *  (`changeIsZero`: a change at the boundary = 0, the common convention). */
inline Bits diffManchester(const Bits& h, int phase, bool changeIsZero = true) {
    Bits out;
    int prevSecond = -1;
    for (size_t i = size_t(phase); i + 1 < h.size(); i += 2) {
        if (h[i] == h[i + 1]) break;
        if (prevSecond >= 0) {
            const bool changed = h[i] != prevSecond;
            out.push_back(uint8_t(changed != changeIsZero));
        }
        prevSecond = h[i + 1];
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  BIT HELPERS + CHECKS
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline uint32_t bitsAt(const Bits& b, size_t pos, int n) {   ///< MSB-first
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 1) | (pos + size_t(i) < b.size() ? b[pos + size_t(i)] : 0);
    return v;
}
inline uint32_t bitsAtLsb(const Bits& b, size_t pos, int n) {   ///< LSB-first
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) if (pos + size_t(i) < b.size() && b[pos + size_t(i)]) v |= 1u << i;
    return v;
}
inline std::vector<uint8_t> bytesAt(const Bits& b, size_t pos, int nBytes) {
    std::vector<uint8_t> o(static_cast<size_t>(nBytes));
    for (int i = 0; i < nBytes; ++i) o[size_t(i)] = uint8_t(bitsAt(b, pos + size_t(i) * 8, 8));
    return o;
}
/** Find `pattern` (`n` bits, MSB-first) in `b` from `from`; npos if absent. */
inline size_t findBits(const Bits& b, uint32_t pattern, int n, size_t from = 0) {
    if (b.size() < size_t(n)) return size_t(-1);
    for (size_t i = from; i + size_t(n) <= b.size(); ++i) if (bitsAt(b, i, n) == pattern) return i;
    return size_t(-1);
}
inline Bits invert(Bits b) { for (auto& x : b) x ^= 1; return b; }
/** CRC-8, MSB-first, no reflection (the "CRC-8" of most sensor write-ups: poly 0x31 Dallas/Maxim-style, 0x07 CCITT). */
inline uint8_t crc8(const uint8_t* d, size_t n, uint8_t poly, uint8_t init = 0) {
    uint8_t c = init;
    for (size_t i = 0; i < n; ++i) { c ^= d[i]; for (int k = 0; k < 8; ++k) c = uint8_t(c & 0x80 ? (c << 1) ^ poly : c << 1); }
    return c;
}
/** CRC-16, MSB-first, no reflection (CRC-16-CCITT = poly 0x1021). */
inline uint16_t crc16(const uint8_t* d, size_t n, uint16_t poly, uint16_t init) {
    uint16_t c = init;
    for (size_t i = 0; i < n; ++i) { c ^= uint16_t(d[i]) << 8; for (int k = 0; k < 8; ++k) c = uint16_t(c & 0x8000 ? (c << 1) ^ poly : c << 1); }
    return c;
}

} // namespace vibeism
