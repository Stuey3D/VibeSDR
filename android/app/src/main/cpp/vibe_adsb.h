// vibe_adsb.h — ADS-B / Mode S on 1090 MHz: demodulator, CRC, message decoding, CPR positions and the
// aircraft table. Header-only, like the DAB pieces beside it.
//
// ★★★ OUR OWN, FROM THE PUBLISHED STANDARD. ICAO Annex 10 Vol IV (the Mode S downlink: PPM at 1 Mb/s, the
//     8 µs preamble, CRC-24 with generator 0x1FFF409, address/parity overlay) and the DO-260B message
//     layouts as documented openly (Junzi Sun, "The 1090 Megahertz Riddle", 2nd ed., TU Delft, 2021 — the
//     test vectors in vibeserver/test-adsb.cpp are its worked examples). dump1090/readsb are GPL and are
//     NOT copied — they are a behaviour reference only (DECODER-RESEARCH.md principle 2).
//
// ★★ WHY THE WHOLE RADIO. A Mode S reply is 1 µs bits split into 0.5 µs halves: it needs ≥ 2 MS/s centred
//    on 1090 MHz, which is every sample the radio makes. So this is a whole-profile mode like DAB, not a
//    channel cut from a wider capture.
//
// ★★ WHAT IS BELIEVED, AND WHY. Noise produces thousands of preamble-shaped bumps a second. Only these
//    create an aircraft:
//      · DF17/18 extended squitter with a CRC remainder of 0 (or 1 bit repaired — never 2, which would
//        let noise in);
//      · DF11 all-call with a remainder of exactly 0.
//    Every other reply (DF0/4/5/16/20/21) carries its address XOR'd into the parity, so ANY 56/112 bits
//    yield SOME address — they are used only for an aircraft already heard by one of the two above.
//
// ★ Positions: CPR, globally from an even+odd pair 10 s apart at most, then locally from the aircraft's
//   own last position. Surface positions need a reference (the receiver's location, or the aircraft's last
//   airborne fix) — without one they are left out rather than guessed into the wrong quadrant.
#pragma once
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define VIBE_ADSB_NEON 1
#elif defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#define VIBE_ADSB_SSE2 1
#endif

namespace vibeadsb {

// ── CRC-24 (Annex 10 Vol IV 3.1.2.3.3) ───────────────────────────────────────────────────────────────
inline const std::array<uint32_t, 256>& crcTable() {
    static const std::array<uint32_t, 256> t = [] {
        std::array<uint32_t, 256> a{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i << 16;
            for (int k = 0; k < 8; ++k) c = (c & 0x800000) ? ((c << 1) ^ 0xFFF409) : (c << 1);
            a[i] = c & 0xFFFFFF;
        }
        return a;
    }();
    return t;
}

/** The CRC remainder XOR the transmitted parity: 0 for an intact DF11 (IID 0) / DF17 / DF18; the
 *  aircraft's address for the address/parity replies. `len` is 7 or 14 bytes. */
inline uint32_t syndrome(const uint8_t* m, int len) {
    const auto& t = crcTable();
    uint32_t c = 0;
    for (int i = 0; i < len - 3; ++i) c = ((c << 8) ^ t[((c >> 16) ^ m[i]) & 0xFF]) & 0xFFFFFF;
    return c ^ (uint32_t(m[len - 3]) << 16 | uint32_t(m[len - 2]) << 8 | m[len - 1]);
}

/** The syndrome of a single flipped bit in a 112-bit message → that bit. The CRC is linear, so a frame
 *  with one bad bit has exactly that bit's syndrome. Bits 0–4 (the DF) are never repaired: a "repair"
 *  that changes what kind of message it is would turn noise into a DF17. */
inline const std::unordered_map<uint32_t, int>& singleBitSyndromes() {
    static const std::unordered_map<uint32_t, int> m = [] {
        std::unordered_map<uint32_t, int> r;
        for (int b = 5; b < 112; ++b) {
            uint8_t e[14] = {};
            e[b >> 3] = uint8_t(0x80 >> (b & 7));
            r[syndrome(e, 14)] = b;
        }
        return r;
    }();
    return m;
}

inline int dfLengthBytes(int df) { return df >= 16 ? 14 : 7; }

// ── The demodulator ──────────────────────────────────────────────────────────────────────────────────
/** One reply as received: the bytes and the receive conditions. Not yet believed — see Tracker. */
struct Frame {
    uint8_t  b[14] = {};
    int      len = 0;          ///< 7 or 14
    float    level = 0;        ///< pulse power, dBFS (mean of the four preamble pulses; |IQ|² of 1.0 = 0 dBFS)
    uint64_t sample = 0;       ///< position on the fine grid (125 ns units), for ordering
};

/** |IQ|² for n samples, four at a time.
 *  ★★★ POWER ALL THE WAY, NO SQUARE ROOTS. The windows add power, not magnitude (an energy detector): on Stuart's
 *      sky at the FlightAware antenna that heard slightly MORE (26 726 messages vs 26 552, DF17 3 841 vs 3 810, the
 *      synthetic tests unchanged) for 16 % of a Pi 3 core instead of 18.6 % — 32-bit NEON has no vector square
 *      root, and a magnitude for every sample was the largest single cost left. */
inline void power(const std::complex<float>* iq, size_t n, float* pw) {
    size_t i = 0;
    const float* f = reinterpret_cast<const float*>(iq);
#if VIBE_ADSB_NEON
    for (; i + 4 <= n; i += 4) {
        const float32x4x2_t v = vld2q_f32(f + 2 * i);
        vst1q_f32(pw + i, vmlaq_f32(vmulq_f32(v.val[0], v.val[0]), v.val[1], v.val[1]));
    }
#elif VIBE_ADSB_SSE2
    for (; i + 4 <= n; i += 4) {
        const __m128 a = _mm_loadu_ps(f + 2 * i), b = _mm_loadu_ps(f + 2 * i + 4);
        const __m128 re = _mm_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0)), im = _mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1));
        _mm_storeu_ps(pw + i, _mm_add_ps(_mm_mul_ps(re, re), _mm_mul_ps(im, im)));
    }
#endif
    for (; i < n; ++i) pw[i] = f[2 * i] * f[2 * i] + f[2 * i + 1] * f[2 * i + 1];
}

/** max(p[k], p[k+1]) for n samples — a pulse's power whichever of two samples it smeared into. Stage 1 reads this,
 *  one load per pulse position instead of two. */
inline void pairMax(const float* p, size_t n, float* pm) {
    size_t i = 0;
#if VIBE_ADSB_NEON
    for (; i + 4 <= n; i += 4) vst1q_f32(pm + i, vmaxq_f32(vld1q_f32(p + i), vld1q_f32(p + i + 1)));
#elif VIBE_ADSB_SSE2
    for (; i + 4 <= n; i += 4) _mm_storeu_ps(pm + i, _mm_max_ps(_mm_loadu_ps(p + i), _mm_loadu_ps(p + i + 1)));
#endif
    for (; i < n; ++i) pm[i] = std::max(p[i], p[i + 1]);
}

/** Stage 1 for samples i..i+3 at once: a bit set for each lane whose four preamble pulse positions (each ± one
 *  sample: `pm` is pairMax) all reach `thr`. 0 for the overwhelming majority, a few instructions for four. */
inline unsigned pulseMask4(const float* pm, size_t i, size_t o1, size_t o2, size_t o3, float thr) {
#if VIBE_ADSB_NEON
    const float32x4_t t = vdupq_n_f32(thr);
    auto at = [&](size_t o) { return vcgeq_f32(vld1q_f32(pm + i + o), t); };
    const uint32x4_t m = vandq_u32(vandq_u32(at(0), at(o1)), vandq_u32(at(o2), at(o3)));
    const uint32x2_t h = vorr_u32(vget_low_u32(m), vget_high_u32(m));
    if (!vget_lane_u32(vpmax_u32(h, h), 0)) return 0;
    return (vgetq_lane_u32(m, 0) & 1) | (vgetq_lane_u32(m, 1) & 2) | (vgetq_lane_u32(m, 2) & 4) | (vgetq_lane_u32(m, 3) & 8);
#elif VIBE_ADSB_SSE2
    const __m128 t = _mm_set1_ps(thr);
    auto at = [&](size_t o) { return _mm_cmpge_ps(_mm_loadu_ps(pm + i + o), t); };
    return unsigned(_mm_movemask_ps(_mm_and_ps(_mm_and_ps(at(0), at(o1)), _mm_and_ps(at(o2), at(o3)))));
#else
    unsigned m = 0;
    for (unsigned l = 0; l < 4; ++l) {
        auto at = [&](size_t o) { return pm[i + l + o] >= thr; };
        if (at(0) && at(o1) && at(o2) && at(o3)) m |= 1u << l;
    }
    return m;
#endif
}

/** Sixteen candidates (i..i+15): pulseMask4 four times, one answer — see feedPiece. */
inline unsigned pulseMask16(const float* pm, size_t i, size_t o1, size_t o2, size_t o3, float thr) {
#if VIBE_ADSB_NEON
    const float32x4_t t = vdupq_n_f32(thr);
    auto m4 = [&](size_t j) {
        auto at = [&](size_t o) { return vcgeq_f32(vld1q_f32(pm + j + o), t); };
        return vandq_u32(vandq_u32(at(0), at(o1)), vandq_u32(at(o2), at(o3)));
    };
    const uint32x4_t a = m4(i), b = m4(i + 4), c = m4(i + 8), d = m4(i + 12);
    const uint32x4_t any = vorrq_u32(vorrq_u32(a, b), vorrq_u32(c, d));
    const uint32x2_t h = vorr_u32(vget_low_u32(any), vget_high_u32(any));
    if (!vget_lane_u32(vpmax_u32(h, h), 0)) return 0;
    unsigned m = 0;
    const uint32x4_t v[4] = {a, b, c, d};
    for (int g = 0; g < 4; ++g) {
        uint32_t l[4]; vst1q_u32(l, v[g]);
        for (int k = 0; k < 4; ++k) if (l[k]) m |= 1u << (4 * g + k);
    }
    return m;
#else
    return pulseMask4(pm, i, o1, o2, o3, thr) | pulseMask4(pm, i + 4, o1, o2, o3, thr) << 4 |
           pulseMask4(pm, i + 8, o1, o2, o3, thr) << 8 | pulseMask4(pm, i + 12, o1, o2, o3, thr) << 12;
#endif
}

/** The sum of 16 floats. */
inline float sum16(const float* p) {
#if VIBE_ADSB_NEON
    const float32x4_t s = vaddq_f32(vaddq_f32(vld1q_f32(p), vld1q_f32(p + 4)), vaddq_f32(vld1q_f32(p + 8), vld1q_f32(p + 12)));
    const float32x2_t h = vadd_f32(vget_low_f32(s), vget_high_f32(s));
    return vget_lane_f32(vpadd_f32(h, h), 0);
#elif VIBE_ADSB_SSE2
    const __m128 s = _mm_add_ps(_mm_add_ps(_mm_loadu_ps(p), _mm_loadu_ps(p + 4)), _mm_add_ps(_mm_loadu_ps(p + 8), _mm_loadu_ps(p + 12)));
    float l[4]; _mm_storeu_ps(l, s);
    return (l[0] + l[1]) + (l[2] + l[3]);
#else
    float s = 0; for (int k = 0; k < 16; ++k) s += p[k]; return s;
#endif
}

/**
 * PPM demodulation, measured on a FINE grid: every half-bit window (0.5 µs) is the mean |IQ|² over it, at
 * four starting points per half-bit (125 ns apart), with fractional edges.
 *
 * ★★★ WHY NOT ONE SAMPLE PER HALF-BIT. At exactly 2 MS/s, with a reply that happens to start on a sample,
 *     one sample per half-bit is perfect — and that is the only case. A reply starting half a sample late
 *     smears every pulse over two samples, and at 2.048 MS/s the grid drifts across the reply: the first
 *     attempt heard 2–22 % of clean frames there (test-adsb). Measuring each window at four phases finds the
 *     one where the pulses sit inside their windows. ★ Even so, 2.0/2.048 MS/s lose the replies that split
 *     50/50 — ADS-B mode asks for 2.4 MS/s or more.
 * ★★★ TWO STAGES, FOR THE PI 2. Measuring the fine grid everywhere is 8 million windows a second, nearly all
 *     of them noise: 6.5 % of an M-series core, so most of a Pi 2's. A cheap test on the raw samples first —
 *     four samples above the floor where the four preamble pulses would be, four candidates at once (NEON /
 *     SSE2), then the quiet zone — and the fine grid is measured only around a sample that passes.
 * ★★ Where several phases pass the preamble, the checksum chooses: a DF11/17/18 that comes out intact (or one
 *    bit from it) on that phase. An address/parity reply proves itself only by naming an aircraft already
 *    tracked (`known`), or a wrong phase's rubbish would be passed on and the real frame behind it skipped.
 */
class Demod {
public:
    explicit Demod(double sampleRate)
        : rate_(sampleRate), h_(sampleRate * 0.5e-6), q_(sampleRate * 0.125e-6),
          hf_(float(h_)), qf_(float(q_)), invH_(float(1.0 / h_)),
          span_(size_t(std::ceil(sampleRate * 121e-6)) + 4) {
        for (int k = 0; k < 4; ++k) pulseAt_[k] = int(std::lround(kPulseUs[k] * 1e-6 * sampleRate));
        for (int k = 0; k < 3; ++k) quietAt_[k] = int(std::lround((6.0 + 0.5 * k) * 1e-6 * sampleRate));
        gapAt_ = int(std::lround(2.5e-6 * sampleRate));
        lc_.assign(span_ + 8, 0.0f);
    }

    bool usable() const { return rate_ >= 1.99e6; }

    /** Feed raw IQ (any scale; levels are reported relative to 1.0 = full scale). */
    template <class Cb>
    void feed(const std::complex<float>* iq, size_t n, Cb&& onFrame) {
        auto none = [](uint32_t) { return false; };
        feed(iq, n, onFrame, none);
    }

    /** …with `known(address)`: is this an aircraft already tracked? (Tracker::known) — the only way an
     *  address/parity reply (DF0/4/5/16/20/21) can be told from noise. */
    template <class Cb, class Known>
    void feed(const std::complex<float>* iq, size_t n, Cb&& onFrame, Known&& known) {
        if (!usable()) return;
        // ★★★ IN PIECES THAT FIT THE CACHE. Fed 65 536 samples at a time, the buffers grew to ~2.6 MB — five times
        //     the Pi 3's L2 — and the SAME sky cost 32 % of a core instead of 6.5 % (every sample a trip to RAM).
        //     The server hands over whatever block the radio makes, so the decoder must not depend on it.
        for (size_t o = 0; o < n; o += kPiece) feedPiece(iq + o, std::min(kPiece, n - o), onFrame, known);
    }

    uint64_t preambles() const { return preambles_; }   ///< preamble-shaped bumps that went on to bit slicing
    uint64_t triggers() const { return triggers_; }     ///< samples that passed stage 1
    uint64_t slices() const { return slices_; }         ///< replies sliced (one per phase tried)
    /** The noise floor, dBFS (mean noise power). */
    double noiseDbfs() const { return noiseP_ > 0 ? 10.0 * std::log10(noiseP_) : -99.0; }

private:
    static constexpr size_t kPiece = 4096;

    template <class Cb, class Known>
    void feedPiece(const std::complex<float>* iq, size_t n, Cb& onFrame, Known& known) {
        // ★★ |IQ|², FOUR AT A TIME (power()). A scalar square root on every sample was most of the decoder's cost
        //    on the Pi 3 (~125 cycles a sample in 32-bit mode).
        const size_t was = pow_.size();
        pow_.resize(was + n);
        power(iq, n, pow_.data() + was);
        if (pow_.size() > 1) {
            pm_.resize(pow_.size() - 1);
            pairMax(pow_.data() + pmN_, pm_.size() - pmN_, pm_.data() + pmN_);
            pmN_ = pm_.size();
        }
        const float* pw = pow_.data();
        const float* pm = pm_.data();
        size_t pos = pos_;
        size_t end = pow_.size() > span_ ? pow_.size() - span_ : 0;
        end = pos + ((end > pos ? end - pos : 0) & ~size_t(15));  // whole groups of 16: blocks stay aligned
        const size_t o1 = size_t(pulseAt_[1]), o2 = size_t(pulseAt_[2]), o3 = size_t(pulseAt_[3]);
        float np = noiseP_;
        for (; pos < end; pos += 16) {
            // ★★★ THE NOISE FLOOR = THE QUIETEST OF THE LAST 16 BLOCKS of 256 samples (~1.7 ms at 2.4 MS/s), ×1.1
            //     for the bias of taking a minimum. Two running means came first and both were wrong: one crept up
            //     from nothing (~80 000 samples of stage 2 at every start: two-thirds of the Pi 3 benchmark), and a
            //     clipped one RUNS AWAY once more than a ninth of samples are pulses — busy airspace, or a burst of
            //     replies (test-adsb: 20 % heard). Replies always leave quiet gaps; the quietest block is the floor.
            if (warm_ < 32) for (size_t l = 0; l < 16 && warm_ < 32; ++l) { ++warm_; np += (pw[pos + l] - np) / float(warm_); }
            blockSum_ += sum16(pw + pos);
            if ((blockN_ += 16) == 256) {
                blocks_[blockAt_++ & 15] = blockSum_ / 256.0f;
                const int have = std::min<uint32_t>(blockAt_, 16);
                float lo = blocks_[0];
                for (int k = 1; k < have; ++k) lo = std::min(lo, blocks_[k]);
                np = lo * 1.1f;
                blockSum_ = 0; blockN_ = 0;
            }
            if (warm_ < 32 || pos + 15 < skip_) continue;
            // stage 1: power above the floor at each of the four pulse positions (± a sample for smear) —
            // 3.8 × the mean noise power is 2.2 × its mean magnitude. ★ Sixteen candidates per answer from the
            // vector unit: on 32-bit ARM moving a result out of NEON stalls the pipeline, so ask once, not four times.
            const float thr = np * 3.8f;
            unsigned lanes = pulseMask16(pm, pos, o1, o2, o3, thr);
            for (size_t l = 0; lanes; ++l, lanes >>= 1) {
                if (!(lanes & 1)) continue;
                const size_t i = pos + l;
                if (i < skip_) continue;
                // ★★★ …AND THE QUIET ZONE (6.0–7.0 µs) AND THE GAP AT 2.5 µs WELL BELOW THE WEAKEST PULSE. Without
                //     the quiet zone one sample in seven of a real sky passed stage 1 (366 000 a second on Stuart's
                //     FlightAware antenna: noise near the floor, Mode A/C pulse trains) and the decoder took 3¼ Pi 3
                //     cores. ★ Only 6.0–7.0 µs: the reply may start a sample after i, and a sample at 5.5 µs then
                //     caught the last pulse's smear, one at 7.5 µs the first data bit (−8 % of real messages).
                // ★ NOT "the four pulses within N dB of each other": at 6 dB it lost 1.7 % of a real sky's messages
                //   (smear and noise make a weak preamble's pulses unequal), at 10 dB it saved 0.2 % of a core.
                const float weakest = std::min(std::min(pm[i], pm[i + o1]), std::min(pm[i + o2], pm[i + o3])) * 0.5f;
                bool quiet = pw[i + size_t(gapAt_)] < weakest;
                for (int k = 0; k < 3 && quiet; ++k) quiet = pw[i + size_t(quietAt_[k])] < weakest;
                if (!quiet) continue;
                // stage 2: the fine grid around here
                noiseP_ = np;
                ++triggers_;
                const size_t used = tryAt(i, onFrame, known);
                if (used) skip_ = i + used;
            }
        }
        pos_ = pos;
        noiseP_ = np;
        trim();
    }

    static constexpr double kPulseUs[4] = {0.0, 1.0, 3.5, 4.5};
    // ★★ RELATIVE, AND LOOSE. Below 2.5 MS/s a 0.5 µs pulse straddles two samples and smears half into the gap
    //    beside it — the receiver's own filtering does the same — so the gaps of a clean preamble sit at 70–90 %
    //    of its pulses, not near zero. The CRC, not this test, is what keeps noise out.
    static constexpr float kLoMax = 0.85f, kGapMax = 0.72f;   // in power (≈ 0.92², 0.85² in magnitude)

    /** The mean |IQ|² over the half-bit starting `off` samples after the trigger's i0: two look-ups in the trigger's
     *  own running sum (lc_), the edge samples weighted by how much of them the window covers.
     *  ★★ ONE RUNNING SUM PER TRIGGER, BUILT ONLY AS FAR AS ASKED. Summing each window's samples afresh was a third
     *     of the decoder on the Pi 3 (1.4 million windows a second, nearly all of them slicing); a running sum kept
     *     for EVERY sample was worse still (a double stored per sample for windows almost never asked for). */
    float win(float off) const {                          // ★ the trigger's sum must already reach off + h
        return (cumFast(off + hf_) - cumFast(off)) * invH_;
    }
    float cumFast(float x) const {                       // cumAt, already extended far enough
        const int k = int(x);
        return lc_[size_t(k)] + (x - float(k)) * lm_[k];
    }
    /** Make lc_ reach index upTo. ★ Four lanes at a time on NEON: a running sum is a chain of dependent adds
     *  (4 cycles each on the A53), so each group of four is summed within itself and the carry added once. */
    void extend(int upTo) const {
        if (upTo > int(lc_.size()) - 1) upTo = int(lc_.size()) - 1;
        int k = lcN_;
#if VIBE_ADSB_NEON
        float carry = lc_[size_t(k - 1)];
        for (; k + 4 <= upTo + 1; k += 4) {
            float32x4_t v = vld1q_f32(lm_ + k - 1);                       // x0 x1 x2 x3
            v = vaddq_f32(v, vextq_f32(vdupq_n_f32(0), v, 3));            // x0, x0+x1, x1+x2, x2+x3
            v = vaddq_f32(v, vextq_f32(vdupq_n_f32(0), v, 2));            // running sums within the four
            v = vaddq_f32(v, vdupq_n_f32(carry));
            vst1q_f32(&lc_[size_t(k)], v);
            carry = vgetq_lane_f32(v, 3);
        }
#endif
        for (; k < upTo + 1; ++k) lc_[size_t(k)] = lc_[size_t(k - 1)] + lm_[k - 1];
        lcN_ = std::max(lcN_, upTo + 1);
    }
    void startTrigger(size_t i0) const { lm_ = pow_.data() + i0; lc_[0] = 0; lcN_ = 1; }

    /** Does a preamble start `off` samples after the trigger's i0? Returns its score (pulses minus gaps), or 0. Fine offsets
     *  (125 ns) via qf_. ★ The first pulse and its neighbours first: most triggers fail there, for 3 windows not 14. */
    float preambleAt(float off, float& hiOut) const {
        auto w = [&](int fine) { return win(off + float(fine) * qf_); };
        const float p0 = w(0), p4 = w(4), p8 = w(8);
        if (!(p0 > p4 && p8 > p4)) return 0;
        const float p12 = w(12), p24 = w(24), p28 = w(28), p32 = w(32), p36 = w(36);
        // pulses at 0, 1.0, 3.5, 4.5 µs → fine 0, 8, 28, 36; the half-bits between them must be lower
        if (!(p8 > p12 && p28 > p24 && p28 > p32 && p36 > p32)) return 0;
        const float hi = (p0 + p8 + p28 + p36) * 0.25f;
        if (hi < noiseP_ * 7.84f) return 0;              // ≈ 9 dB above the floor (power: 2.8²)
        const float lo = (p4 + p12 + p24 + p32) * 0.25f;
        if (lo > hi * kLoMax) return 0;
        const float gap = (w(16) + w(20) + w(44) + w(48) + w(52) + w(56)) / 6.0f;   // 2–3 µs and 5.5–7.5 µs
        if (gap > hi * kGapMax) return 0;
        hiOut = hi;
        return hi - lo - gap;
    }

    /** Slice the reply; false (and stop) as soon as the first byte is not a reply we decode. */
    bool slice(size_t i0, float off, float hi, Frame& f) const {
        ++slices_;
        f = Frame{};
        // ★★ A bit compares the half-bit [a, a+h) with [a+h, a+2h), and the next bit starts at a+2h: so each bit is
        //    TWO new look-ups in the running sum (C(a+h), C(a+2h)), not four — 2·C(a+h) > C(a) + C(a+2h). The sum is
        //    extended to the whole reply once, up front.
        extend(int(off + float(64 + 8 * 112) * qf_) + 3);
        float ca = cumFast(off + 64 * qf_);
        float x = off + 64 * qf_;
        auto bitNext = [&]() {
            const float cm = cumFast(x + hf_), cb = cumFast(x + 2 * hf_);
            const bool one = 2 * cm > ca + cb;
            ca = cb; x += 2 * hf_;
            return one;
        };
        for (int i = 0; i < 8; ++i) if (bitNext()) f.b[0] |= uint8_t(0x80 >> i);
        if (!plausibleDf(f.b[0] >> 3)) return false;
        f.len = dfLengthBytes(f.b[0] >> 3);
        for (int i = 8; i < f.len * 8; ++i) if (bitNext()) f.b[i >> 3] |= uint8_t(0x80 >> (i & 7));
        f.level = 10.0f * std::log10(std::max(hi, 1e-12f));   // hi is a mean POWER — dBFS, 1.0 = full scale
        f.sample = base_ + uint64_t(std::max(0.0, (double(i0) + off) / q_));
        return true;
    }

    /** Search the phases across two samples from one before i; returns the samples the reply used, or 0. */
    template <class Cb, class Known>
    size_t tryAt(size_t i, Cb& onFrame, Known& known) {
        if (i < 1) return 0;
        const size_t i0 = i - 1;
        startTrigger(i0);
        extend(int(2.0f + 57 * qf_ + hf_) + 3);           // ★ everything the preamble search reads, once
        struct Cand { float off, score, hi; };
        Cand c[24]; int nc = 0;
        for (float off = 0; off < 2.0f && nc < 24; off += qf_) {
            float h; const float s = preambleAt(off, h);
            if (s > 0) c[nc++] = {off, s, h};
        }
        if (!nc) return 0;
        ++preambles_;
        std::sort(c, c + nc, [](const Cand& a, const Cand& b) { return a.score > b.score; });
        Frame f;
        // ★★ The best four phases. Two (the best, and the best ≥ 250 ns from it) cost 4 % of a real sky's messages
        //    on the Pi 3 for 2 % of a core; with the running sum a slicing is cheap enough to keep the other two.
        for (int k = 0; k < std::min(nc, 4); ++k) {
            if (!slice(i0, c[k].off, c[k].hi, f)) continue;
            const int df = f.b[0] >> 3;
            const uint32_t s = syndrome(f.b, f.len);
            bool good = false;
            if (df == 11) good = (s & ~0x7Fu) == 0;
            else if (df == 17 || df == 18) good = s == 0 || singleBitSyndromes().count(s);
            else good = known(s);
            if (good) {
                onFrame(f);
                return size_t(c[k].off) + size_t((8 + f.len * 8) * 1e-6 * rate_);
            }
        }
        return 0;
    }

    void trim() {
        if (pos_ < 8192) return;                         // ★ small: see feed()
        const size_t drop = pos_ - 4;                    // keep a little behind the cursor for the ± search
        pow_.erase(pow_.begin(), pow_.begin() + long(drop));
        pm_.erase(pm_.begin(), pm_.begin() + long(drop));
        pmN_ -= drop;
        pos_ -= drop; skip_ = skip_ > drop ? skip_ - drop : 0;
        base_ += uint64_t(double(drop) / q_);
    }

    static bool plausibleDf(int df) {
        return df == 0 || df == 4 || df == 5 || df == 11 || df == 16 || df == 17 || df == 18 || df == 20 || df == 21;
    }

    double rate_, h_, q_;
    float hf_, qf_, invH_;                              // the same, single precision, for the windows
    mutable std::vector<float> lc_;                     // the trigger's running sum of |IQ|² (see win)
    mutable const float* lm_ = nullptr;
    mutable int lcN_ = 0;
    size_t span_;                                        // samples a whole reply needs ahead of its start
    int pulseAt_[4] = {};
    int quietAt_[3] = {};                               // 6.0, 6.5, 7.0 µs, in samples
    int gapAt_ = 0;                                     // 2.5 µs: between the second and third pulses
    std::vector<float> pow_;                            // |IQ|² per sample
    std::vector<float> pm_;                             // pairMax of pow_ (stage 1)
    size_t pmN_ = 0;
    size_t pos_ = 0, skip_ = 0;
    uint64_t base_ = 0, preambles_ = 0, triggers_ = 0;
    mutable uint64_t slices_ = 0;
    float noiseP_ = 0;                                  // mean noise power
    uint32_t warm_ = 0;                                 // samples in the first guess (to 32)
    float blockSum_ = 0; uint32_t blockN_ = 0, blockAt_ = 0;
    float blocks_[16] = {};                             // mean power of the last 16 blocks
};

// ── Field decoding helpers ───────────────────────────────────────────────────────────────────────────
/** Bits [first, last] of a byte array, 1-based as the standard numbers them. */
inline uint32_t bits(const uint8_t* m, int first, int last) {
    uint32_t v = 0;
    for (int i = first; i <= last; ++i) v = (v << 1) | ((m[(i - 1) >> 3] >> (7 - ((i - 1) & 7))) & 1);
    return v;
}

/** Gillham (Mode C) 100-ft altitude from the bits in the order D2 D4 A1 A2 A4 B1 B2 B4 C1 C2 C4.
 *  Returns false for a code that is not an altitude. */
inline bool gillhamToFeet(uint32_t d2d4a1a2a4b1b2b4, uint32_t c1c2c4, int& feet) {
    auto grayToInt = [](uint32_t g) { uint32_t n = g; while (g >>= 1) n ^= g; return n; };
    int n500 = int(grayToInt(d2d4a1a2a4b1b2b4));
    int n100 = int(grayToInt(c1c2c4));
    if (n100 == 0 || n100 == 5 || n100 == 6) return false;
    if (n100 == 7) n100 = 5;
    if (n500 & 1) n100 = 6 - n100;
    feet = n500 * 500 + n100 * 100 - 1300;
    return true;
}

/** The 13-bit AC field of DF0/4/16/20: C1 A1 C2 A2 C4 A4 M B1 Q B2 D2 B4 D4. */
inline bool altitude13(uint32_t ac, int& feet) {
    if (ac == 0) return false;
    if (ac & 0x40) return false;                         // M = 1: metric, not seen in practice
    if (ac & 0x10) {                                     // Q = 1: 25 ft steps
        const uint32_t n = ((ac & 0x1F80) >> 2) | ((ac & 0x20) >> 1) | (ac & 0x0F);
        feet = int(n) * 25 - 1000;
        return true;
    }
    auto b = [&](int i) { return (ac >> (12 - i)) & 1; };   // i = 0..12, left to right
    const uint32_t C1 = b(0), A1 = b(1), C2 = b(2), A2 = b(3), C4 = b(4), A4 = b(5), B1 = b(7), B2 = b(9),
                   D2 = b(10), B4 = b(11), D4 = b(12);
    const uint32_t g500 = D2 << 7 | D4 << 6 | A1 << 5 | A2 << 4 | A4 << 3 | B1 << 2 | B2 << 1 | B4;
    return gillhamToFeet(g500, C1 << 2 | C2 << 1 | C4, feet);
}

/** The 12-bit ALT field of an airborne position (the AC field without M). */
inline bool altitude12(uint32_t alt, int& feet) {
    const uint32_t ac = ((alt & 0xFC0) << 1) | (alt & 0x3F);  // re-insert M = 0
    return altitude13(ac, feet);
}

/** The 13-bit ID field (DF5/21, and TC 28): C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4 D4 → the 4-digit squawk. */
inline int squawk13(uint32_t id) {
    auto b = [&](int i) { return int((id >> (12 - i)) & 1); };
    const int A = b(5) << 2 | b(3) << 1 | b(1);
    const int B = b(11) << 2 | b(9) << 1 | b(7);
    const int C = b(4) << 2 | b(2) << 1 | b(0);
    const int D = b(12) << 2 | b(10) << 1 | b(8);
    return A * 1000 + B * 100 + C * 10 + D;
}

// ── CPR (DO-260B A.1.7) ─────────────────────────────────────────────────────────────────────────────
/** NL(lat): the number of longitude zones at a latitude, from the transition-latitude table rather than the
 *  closed form, so an aircraft exactly on a boundary gets the same answer every time. */
inline int cprNL(double lat) {
    static const std::array<double, 58> trans = [] {
        std::array<double, 58> t{};
        const double nz = 15.0, pi = 3.14159265358979323846;
        for (int nl = 59; nl >= 2; --nl)
            t[59 - nl] = 180.0 / pi * std::acos(std::sqrt((1 - std::cos(pi / (2 * nz))) / (1 - std::cos(2 * pi / nl))));
        return t;
    }();
    lat = std::fabs(lat);
    for (int nl = 59; nl >= 2; --nl) if (lat < trans[59 - nl]) return nl;
    return 1;
}

inline double cprMod(double a, double b) { const double r = a - b * std::floor(a / b); return r < 0 ? r + b : r; }

/** Globally unambiguous airborne (span 360) or surface (90, needs `refLat` to pick the quadrant) position from
 *  an even/odd pair; `oddNewer` picks which frame's time the answer is for. */
inline bool cprGlobal(double latE, double lonE, double latO, double lonO, bool oddNewer, bool surface,
                      double refLat, double refLon, double& lat, double& lon) {
    const double span = surface ? 90.0 : 360.0;
    latE /= 131072.0; lonE /= 131072.0; latO /= 131072.0; lonO /= 131072.0;
    const double dE = span / 60.0, dO = span / 59.0;
    const double j = std::floor(59 * latE - 60 * latO + 0.5);
    double rE = dE * (cprMod(j, 60) + latE), rO = dO * (cprMod(j, 59) + latO);
    if (surface) {
        // ★ four candidates 90° apart — the one on the receiver's side of the world
        auto near = [&](double v) { double best = v; for (double c : {v - 90, v, v + 90 - 180, v - 180}) if (std::fabs(c - refLat) < std::fabs(best - refLat)) best = c; return best; };
        rE = near(rE); rO = near(rO);
    } else {
        if (rE >= 270) rE -= 360;
        if (rO >= 270) rO -= 360;
    }
    if (rE < -90 || rE > 90 || rO < -90 || rO > 90) return false;
    if (cprNL(rE) != cprNL(rO)) return false;           // the pair straddles a zone boundary: wait for the next
    lat = oddNewer ? rO : rE;
    const int nl = cprNL(lat);
    const int ni = std::max(nl - (oddNewer ? 1 : 0), 1);
    const double m = std::floor(lonE * (nl - 1) - lonO * nl + 0.5);
    lon = (span / ni) * (cprMod(m, ni) + (oddNewer ? lonO : lonE));
    if (surface) {
        double best = lon;
        for (double c : {lon, lon + 90, lon + 180, lon + 270, lon - 90, lon - 180, lon - 270})
            if (c >= -180 && c < 180 && std::fabs(cprMod(c - refLon + 180, 360) - 180) < std::fabs(cprMod(best - refLon + 180, 360) - 180)) best = c;
        lon = best;
    }
    if (lon >= 180) lon -= 360;
    return true;
}

/** Locally unambiguous position from one frame and a reference within half a zone (≈ 180 NM airborne,
 *  45 NM surface). */
inline void cprLocal(double latC, double lonC, bool odd, bool surface, double refLat, double refLon,
                     double& lat, double& lon) {
    const double span = surface ? 90.0 : 360.0;
    latC /= 131072.0; lonC /= 131072.0;
    const double dLat = span / (odd ? 59.0 : 60.0);
    const double j = std::floor(refLat / dLat) + std::floor(0.5 + cprMod(refLat, dLat) / dLat - latC);
    lat = dLat * (j + latC);
    const int ni = std::max(cprNL(lat) - (odd ? 1 : 0), 1);
    const double dLon = span / ni;
    const double m = std::floor(refLon / dLon) + std::floor(0.5 + cprMod(refLon, dLon) / dLon - lonC);
    lon = dLon * (m + lonC);
    if (lon >= 180) lon -= 360;
    if (lon < -180) lon += 360;
}

/** Great-circle distance in nautical miles. */
inline double distanceNm(double la1, double lo1, double la2, double lo2) {
    const double r = 3.14159265358979323846 / 180.0;
    const double a = std::sin((la2 - la1) * r / 2), b = std::sin((lo2 - lo1) * r / 2);
    const double h = a * a + std::cos(la1 * r) * std::cos(la2 * r) * b * b;
    return 2 * 3440.065 * std::asin(std::min(1.0, std::sqrt(h)));
}

// ── The aircraft table ───────────────────────────────────────────────────────────────────────────────
/** One point of an aircraft's trail: where, and how high (kTrailNoAlt = unknown, 0 also = on the ground). */
struct TrailPt { float lat = 0, lon = 0; int32_t alt = 0; };
constexpr int32_t kTrailNoAlt = INT32_MIN;
/* ★★ THE TRAIL (Stuart, 2026-10-24: "clicking an aircraft should draw its track in as a rainbow coloured line to show its
 *  altitude … an aircraft approaching Heathrow I should see a line changing from purple down to orange"). Kept on the
 *  SERVER, so a listener who joins late, or taps an aircraft that has been in view for half an hour, sees all of it.
 *  A point every 0.3 NM or 250 ft of climb/descent; at 1 000 points every other one is dropped (the newest kept), so a
 *  long track keeps its shape at half the detail — 12 KB an aircraft at most. */
constexpr double kTrailStepNm = 0.3;
constexpr int kTrailStepFt = 250;
constexpr size_t kTrailMax = 1000;

struct Aircraft {
    uint32_t icao = 0;
    bool     nonIcao = false;          ///< DF18 CF=1: an anonymous/temporary address
    std::string callsign;
    int      category = 0;             ///< (TC << 4) | CA from identification, e.g. 0xA3 = A3 (large)
    bool     hasPos = false;  double lat = 0, lon = 0; bool onGround = false;
    bool     hasAlt = false;  int altBaro = 0;
    bool     hasGeo = false;  int altGeom = 0;
    bool     hasGs = false;   double gs = 0;  double track = 0;  bool hasTrack = false;
    bool     hasVr = false;   int vr = 0;
    bool     hasHdg = false;  double heading = 0;
    bool     hasAs = false;   int airspeed = 0; bool tas = false;
    int      squawk = -1;
    int      emergency = 0;            ///< TC 28 emergency state, 0 = none
    double   seen = 0, seenPos = 0;    ///< seconds (the tracker's clock) of the last message / last position
    double   trackedNm = 0;            ///< how far we have followed it: the sum of its position steps (setPos)
    std::vector<TrailPt> trail;        ///< where it has been — see kTrailStepNm
    uint32_t messages = 0;
    float    level = -99;              ///< dBFS, smoothed
    // CPR state
    bool     haveE = false, haveO = false, cprSurface = false;
    double   tE = 0, tO = 0; uint32_t latE = 0, lonE = 0, latO = 0, lonO = 0;
    double   pendLat = 0, pendLon = 0;   ///< a single-frame fix near the receiver, awaiting a second to agree
};

class Tracker {
public:
    /** The receiver's position, if the server knows it: needed for surface positions, and used to throw out
     *  a global decode that lands implausibly far away. */
    void setReceiver(double lat, double lon) { haveRx_ = true; rxLat_ = lat; rxLon_ = lon; }
    void setMaxRangeNm(double nm) { maxRangeNm_ = nm; }

    /** Judge and apply one frame. `now` in seconds. Returns true if it was believed. */
    bool accept(const Frame& f, double now) {
        const int df = f.b[0] >> 3;
        uint32_t s = syndrome(f.b, f.len);
        uint8_t m[14];
        std::memcpy(m, f.b, f.len);
        if (df == 17 || df == 18) {
            if (s != 0) {
                const auto& t = singleBitSyndromes();
                auto it = t.find(s);
                if (it == t.end()) { ++rejected_; return false; }
                m[it->second >> 3] ^= uint8_t(0x80 >> (it->second & 7));
                ++repaired_;
            }
            const uint32_t addr = uint32_t(m[1]) << 16 | uint32_t(m[2]) << 8 | m[3];
            if (df == 18) {
                const int cf = m[0] & 7;
                if (cf != 0 && cf != 1) { ++rejected_; return false; }   // TIS-B / ADS-R relays: not this aircraft's own
            }
            Aircraft& a = touch(addr, f, now);
            if (df == 18 && (m[0] & 7) == 1) a.nonIcao = true;
            extended(a, m + 4, now);
            ++good_;
            return true;
        }
        if (df == 11) {
            const uint32_t addr = uint32_t(m[1]) << 16 | uint32_t(m[2]) << 8 | m[3];
            if (s == 0 || ((s & ~0x7Fu) == 0 && known(addr, now))) { touch(addr, f, now); ++good_; return true; }
            ++rejected_; return false;
        }
        // address/parity replies: the syndrome IS the address
        if (!known(s, now)) { ++rejected_; return false; }
        Aircraft& a = touch(s, f, now);
        if (df == 0 || df == 4 || df == 16 || df == 20) {
            int ft; if (altitude13(bits(m, 20, 32), ft)) { a.hasAlt = true; a.altBaro = ft; }
            a.onGround = (df == 4 || df == 20) && (m[0] & 7) == 1 ? true : a.onGround;
        }
        if (df == 5 || df == 21) a.squawk = squawk13(bits(m, 20, 32));
        ++good_;
        return true;
    }

    /** Forget aircraft not heard for `age` seconds. */
    void expire(double now, double age = 60.0) {
        for (auto it = ac_.begin(); it != ac_.end();) {
            if (now - it->second.seen <= age) { ++it; continue; }
            Aircraft& a = it->second;
            if (a.trail.size() >= 2) gone_[it->first] = {std::move(a.trail), a.trackedNm, now};
            it = ac_.erase(it);
        }
        for (auto it = gone_.begin(); it != gone_.end();) it = (now - it->second.at > kGoneKeepSec) ? gone_.erase(it) : std::next(it);
        while (gone_.size() > kGoneMax) {
            auto oldest = gone_.begin();
            for (auto it = gone_.begin(); it != gone_.end(); ++it) if (it->second.at < oldest->second.at) oldest = it;
            gone_.erase(oldest);
        }
    }
    size_t goneTrails() const { return gone_.size(); }

    const std::unordered_map<uint32_t, Aircraft>& aircraft() const { return ac_; }
    /** {"type":"adsb_trail","hex":"…","pts":[[lat,lon,alt|null],…]} (alt 0 on the ground) — empty pts when unknown. `hex` as the
     *  report writes it (6 hex digits, "~" first for a non-ICAO address). */
    std::string trailJson(const std::string& hex) const {
        std::string h = hex;
        const bool non = !h.empty() && h[0] == '~';
        if (non) h.erase(0, 1);
        uint32_t icao = 0; bool ok = h.size() == 6;
        for (char c : h) {
            const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (v < 0) { ok = false; break; }
            icao = icao << 4 | uint32_t(v);
        }
        char b[64];
        std::snprintf(b, sizeof b, "{\"type\":\"adsb_trail\",\"hex\":\"%s%06x\",\"pts\":[", non ? "~" : "", ok ? icao : 0);
        std::string o = b;
        auto it = ok ? ac_.find(icao) : ac_.end();
        if (it != ac_.end() && it->second.nonIcao == non) {
            bool first = true;
            for (const TrailPt& p : it->second.trail) {
                if (!first) o += ',';
                first = false;
                if (p.alt == kTrailNoAlt) std::snprintf(b, sizeof b, "[%.4f,%.4f,null]", p.lat, p.lon);
                else std::snprintf(b, sizeof b, "[%.4f,%.4f,%d]", p.lat, p.lon, int(p.alt));
                o += b;
            }
        }
        return o + "]}";
    }
    uint64_t good() const { return good_; }
    uint64_t rejected() const { return rejected_; }
    uint64_t repaired() const { return repaired_; }

    /** The table as JSON, in the field names dump1090's aircraft.json made familiar (hex, flight, lat, lon,
     *  alt_baro, gs, track, baro_rate, squawk, seen, seen_pos, messages, rssi). Positions older than 60 s are
     *  dropped, not drawn where the aircraft used to be. */
    std::string json(double now) const {
        std::string o;
        o.reserve(96 + ac_.size() * 220);
        char t[64];
        std::snprintf(t, sizeof t, "{\"now\":%.1f,\"messages\":%llu,\"aircraft\":[", now, (unsigned long long)good_);
        o += t;
        bool first = true;
        for (const auto& kv : ac_) {
            const Aircraft& a = kv.second;
            if (!first) o += ',';
            first = false;
            char b[512];
            int n = std::snprintf(b, sizeof b, "{\"hex\":\"%s%06x\"", a.nonIcao ? "~" : "", a.icao);
#define VIBE_ADSB_ADD(...) (n += std::snprintf(b + n, sizeof b - size_t(n), __VA_ARGS__))
            if (!a.callsign.empty()) VIBE_ADSB_ADD(",\"flight\":\"%s\"", a.callsign.c_str());
            if (a.category) VIBE_ADSB_ADD(",\"category\":\"%c%d\"", "?DCBA"[std::min(a.category >> 4, 4)], a.category & 7);
            if (a.hasPos && now - a.seenPos <= 60) VIBE_ADSB_ADD(",\"lat\":%.5f,\"lon\":%.5f,\"seen_pos\":%.1f", a.lat, a.lon, now - a.seenPos);
            if (a.onGround) VIBE_ADSB_ADD(",\"alt_baro\":\"ground\"");
            else if (a.hasAlt) VIBE_ADSB_ADD(",\"alt_baro\":%d", a.altBaro);
            if (a.hasGeo) VIBE_ADSB_ADD(",\"alt_geom\":%d", a.altGeom);
            if (a.hasGs) VIBE_ADSB_ADD(",\"gs\":%.1f", a.gs);
            if (a.hasTrack) VIBE_ADSB_ADD(",\"track\":%.1f", a.track);
            if (a.hasHdg) VIBE_ADSB_ADD(",\"mag_heading\":%.1f", a.heading);
            if (a.hasAs) { if (a.tas) VIBE_ADSB_ADD(",\"tas\":%d", a.airspeed); else VIBE_ADSB_ADD(",\"ias\":%d", a.airspeed); }
            if (a.hasVr) VIBE_ADSB_ADD(",\"baro_rate\":%d", a.vr);
            if (a.squawk >= 0) VIBE_ADSB_ADD(",\"squawk\":\"%04d\"", a.squawk);
            if (a.emergency) VIBE_ADSB_ADD(",\"emergency\":%d", a.emergency);
            if (a.trackedNm >= 0.5) VIBE_ADSB_ADD(",\"tracked_nm\":%.0f", a.trackedNm);
            VIBE_ADSB_ADD(",\"seen\":%.1f,\"messages\":%u,\"rssi\":%.1f}", now - a.seen, a.messages, a.level);
            o.append(b, size_t(std::min(n, int(sizeof b) - 1)));
#undef VIBE_ADSB_ADD
        }
        o += "]}";
        return o;
    }

    /** An aircraft heard in the last minute — for Demod's address/parity check. */
    bool known(uint32_t addr, double now) const {
        auto it = ac_.find(addr);
        return it != ac_.end() && now - it->second.seen < 60;
    }

private:

    Aircraft& touch(uint32_t addr, const Frame& f, double now) {
        Aircraft& a = ac_[addr];
        if (a.messages == 0) {
            a.icao = addr; a.level = f.level;
            // ★ back within kGoneKeepSec: it carries on with the trail it had (see Gone)
            auto g = gone_.find(addr);
            if (g != gone_.end()) { a.trail = std::move(g->second.trail); a.trackedNm = g->second.trackedNm; gone_.erase(g); }
        }
        a.seen = now;
        ++a.messages;
        a.level += (f.level - a.level) * 0.25f;
        return a;
    }

    void extended(Aircraft& a, const uint8_t* me, double now) {
        const int tc = me[0] >> 3;
        if (tc >= 1 && tc <= 4) {
            static const char* cs = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";
            std::string s;
            for (int i = 0; i < 8; ++i) { const char c = cs[bits(me, 9 + 6 * i, 14 + 6 * i)]; if (c != '#') s += c; }
            while (!s.empty() && s.back() == ' ') s.pop_back();
            if (!s.empty()) a.callsign = s;
            a.category = (tc == 4 ? 0xA0 : tc == 3 ? 0xB0 : tc == 2 ? 0xC0 : 0xD0) | (me[0] & 7);
            return;
        }
        if ((tc >= 9 && tc <= 18) || (tc >= 20 && tc <= 22)) {
            const uint32_t alt = bits(me, 9, 20);
            if (tc <= 18) { int ft; if (altitude12(alt, ft)) { a.hasAlt = true; a.altBaro = ft; } }
            else if (alt) { a.hasGeo = true; a.altGeom = int(std::lround(alt * 3.28084)); }
            a.onGround = false;
            position(a, me, now, false);
            return;
        }
        if (tc >= 5 && tc <= 8) {
            a.onGround = true;
            const int mov = int(bits(me, 6, 12));
            double kt;
            if (groundSpeed(mov, kt)) { a.hasGs = true; a.gs = kt; }
            if (bits(me, 13, 13)) { a.hasTrack = true; a.track = bits(me, 14, 20) * 360.0 / 128.0; }
            position(a, me, now, true);
            return;
        }
        if (tc == 19) {
            const int st = int(bits(me, 6, 8));
            if (st == 1 || st == 2) {
                const int vew = int(bits(me, 15, 24)), vns = int(bits(me, 26, 35));
                if (vew && vns) {
                    const int k = st == 2 ? 4 : 1;
                    const double vx = (bits(me, 14, 14) ? -1 : 1) * (vew - 1) * k;
                    const double vy = (bits(me, 25, 25) ? -1 : 1) * (vns - 1) * k;
                    a.hasGs = true; a.gs = std::hypot(vx, vy);
                    a.hasTrack = true; a.track = cprMod(std::atan2(vx, vy) * 180.0 / 3.14159265358979323846, 360);
                }
            } else if (st == 3 || st == 4) {
                if (bits(me, 14, 14)) { a.hasHdg = true; a.heading = bits(me, 15, 24) * 360.0 / 1024.0; }
                const int as = int(bits(me, 26, 35));
                if (as) { a.hasAs = true; a.airspeed = (as - 1) * (st == 4 ? 4 : 1); a.tas = bits(me, 25, 25); }
            }
            const int vr = int(bits(me, 38, 46));
            if (vr) { a.hasVr = true; a.vr = (bits(me, 37, 37) ? -1 : 1) * (vr - 1) * 64; }
            return;
        }
        if (tc == 28 && bits(me, 6, 8) == 1) {
            a.emergency = int(bits(me, 9, 11));
            const uint32_t id = bits(me, 12, 24);
            if (id) a.squawk = squawk13(id);
        }
    }

    /** DO-260B surface movement → knots (piecewise linear between the standard's break points). */
    static bool groundSpeed(int mov, double& kt) {
        if (mov == 0 || mov > 124) return false;
        if (mov == 1) { kt = 0; return true; }
        if (mov == 124) { kt = 175; return true; }
        static const int mv[] = {2, 9, 13, 39, 94, 109, 124};
        static const double ks[] = {0.125, 1, 2, 15, 70, 100, 175};
        for (int i = 1; i < 7; ++i)
            if (mov < mv[i]) { kt = ks[i - 1] + (mov - mv[i - 1]) * (ks[i] - ks[i - 1]) / (mv[i] - mv[i - 1]); return true; }
        return false;
    }

    void position(Aircraft& a, const uint8_t* me, double now, bool surface) {
        const bool odd = bits(me, 22, 22);
        const uint32_t la = bits(me, 23, 39), lo = bits(me, 40, 56);
        if (surface != a.cprSurface) { a.haveE = a.haveO = false; a.cprSurface = surface; }
        if (odd) { a.haveO = true; a.tO = now; a.latO = la; a.lonO = lo; }
        else     { a.haveE = true; a.tE = now; a.latE = la; a.lonE = lo; }

        double lat, lon;
        const double localLimit = surface ? 45 : 180;
        // 1. Local, from this aircraft's own recent fix: the normal case once it is tracked.
        if (a.hasPos && now - a.seenPos < (surface ? 50 : 30)) {
            cprLocal(la, lo, odd, surface, a.lat, a.lon, lat, lon);
            const double moved = distanceNm(a.lat, a.lon, lat, lon);
            const double allowed = 2 + (now - a.seenPos) * (surface ? 0.1 : 0.4);   // ~1400 kt airborne
            if (moved <= allowed) { setPos(a, lat, lon, now); return; }
            // a jump: fall through to a fresh global fix rather than drawing it
        }
        // 2. Global, from an even/odd pair close in time.
        if (a.haveE && a.haveO && std::fabs(a.tE - a.tO) <= (surface ? 25 : 10)) {
            if (surface && !haveRx_ && !(a.hasPos)) return;
            const double rLat = haveRx_ ? rxLat_ : a.lat, rLon = haveRx_ ? rxLon_ : a.lon;
            if (cprGlobal(a.latE, a.lonE, a.latO, a.lonO, odd, surface, rLat, rLon, lat, lon)) {
                if (haveRx_ && distanceNm(rxLat_, rxLon_, lat, lon) > maxRangeNm_) return;
                setPos(a, lat, lon, now);
                return;
            }
        }
        // 3. Local from the receiver, for a lone frame near it (only inside half a zone, so it cannot alias).
        if (haveRx_ && !a.hasPos) {
            cprLocal(la, lo, odd, surface, rxLat_, rxLon_, lat, lon);
            if (distanceNm(rxLat_, rxLon_, lat, lon) < std::min(localLimit, maxRangeNm_) * 0.5) {
                // ★ not trusted alone: a fix from a single frame waits for a second one to agree
                if (a.pendLat != 0 || a.pendLon != 0) {
                    if (distanceNm(a.pendLat, a.pendLon, lat, lon) < 5) { setPos(a, lat, lon, now); a.pendLat = a.pendLon = 0; return; }
                }
                a.pendLat = lat; a.pendLon = lon;
            }
        }
    }

    /* ★ TRACKED FOR xx NM (Stuart, 2026-10-24) — the path we have followed it along, step by step. A step faster
     *  than 1 200 kt is a bad fix that slipped through, not flight, and is not added (the position still moves). */
    static void setPos(Aircraft& a, double lat, double lon, double now) {
        if (a.hasPos) {
            // ★★ dt is never under a second for this test: two fixes in one block share a time (dt 0 — that wiped the
            //    trail even for the SAME fix twice), and a block's stamp is only as fine as the block. A bad fix is
            //    miles out, far past 1 200 kt × 1 s = 0.33 NM, so the floor lets nothing real through.
            const double d = distanceNm(a.lat, a.lon, lat, lon), dt = std::max(1.0, now - a.seenPos);
            if (d / dt * 3600.0 <= 1200.0) a.trackedNm += d;
            else a.trail.clear();          // ★ a jump that is not flight: the old trail would draw a line across the map
        }
        a.hasPos = true; a.lat = lat; a.lon = lon; a.seenPos = now;
        const int32_t alt = a.onGround ? 0 : a.hasAlt ? int32_t(a.altBaro) : kTrailNoAlt;
        bool add = a.trail.empty();
        if (!add) {
            const TrailPt& l = a.trail.back();
            add = distanceNm(l.lat, l.lon, lat, lon) >= kTrailStepNm
               || (alt != kTrailNoAlt && l.alt != kTrailNoAlt && std::abs(alt - l.alt) >= kTrailStepFt);
        }
        if (add) {
            if (a.trail.size() >= kTrailMax) {
                size_t w = 0;
                for (size_t k = 0; k < a.trail.size(); k += 2) a.trail[w++] = a.trail[k];
                a.trail.resize(w);
            }
            a.trail.push_back({float(lat), float(lon), alt});
        }
    }

    std::unordered_map<uint32_t, Aircraft> ac_;
    /* ★★ TRAILS OUTLIVE A SHORT SILENCE (Stuart, 2026-10-25: "save the tracks so that I can come on and click a 157nm
     *  tracked aircraft and see its track drawn in"). An aircraft unheard for 60 s leaves the table; its trail waits
     *  here 20 min and is handed back if it is heard again — fading at range, or behind a hill, is not a new flight.
     *  At most 200 kept: ≤ 12 KB each, ~2.4 MB worst case. */
    struct Gone { std::vector<TrailPt> trail; double trackedNm; double at; };
    static constexpr double kGoneKeepSec = 20 * 60;
    static constexpr size_t kGoneMax = 200;
    std::unordered_map<uint32_t, Gone> gone_;
    bool haveRx_ = false; double rxLat_ = 0, rxLon_ = 0;
    double maxRangeNm_ = 400;
    uint64_t good_ = 0, rejected_ = 0, repaired_ = 0;
};

} // namespace vibeadsb
