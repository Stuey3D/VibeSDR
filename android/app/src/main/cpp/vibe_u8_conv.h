// vibe_u8_conv.h — an 8-bit IQ buffer (RTL-SDR, rtl_tcp) to floats, and the ADC statistics VibeAGC reads (the peak and
// the samples on the rail). One function, three paths: NEON (64- and 32-bit ARM), SSE2 (x86), scalar. ★ All three must
// give the same statistics — vibeserver/test-u8-conv.cpp checks it (the SSE2 path once dropped them: VibeAGC blind on x86).
#pragma once
#include <cstdint>
#if defined(__ARM_NEON)
  #include <arm_neon.h>
#elif defined(__SSE2__)
  #include <emmintrin.h>
#endif

struct AdcStats {
    uint32_t rails = 0;    // samples at 0 or 255
    uint32_t total = 0;    // samples examined
    uint8_t  maxV  = 0;    // highest sample seen
    uint8_t  minV  = 255;  // lowest sample seen
};

namespace vibe {

inline void convU8ToF32Dg(const uint8_t* in, float* out, int nF, float dg, AdcStats* st = nullptr) {
#if defined(__ARM_NEON)
    const float32x4_t bias = vdupq_n_f32(127.4f), inv = vdupq_n_f32(dg / 128.0f);
    const uint8x16_t hiRail = vdupq_n_u8(254), loRail = vdupq_n_u8(1), one = vdupq_n_u8(1);
    uint32_t rails = 0; uint8_t mx = 0, mn = 255;
    // ★★★ THE REDUCTIONS COME OUT OF THE LOOP. This measured the ADC with THREE horizontal vector
    //     reductions per 16 bytes — vaddvq_u8 for the rail count, vmaxvq_u8 and vminvq_u8 for the
    //     extremes — and a horizontal reduction serialises the pipeline, so on the little cores
    //     this phone schedules us on they cost MORE THAN THE CONVERSION THEY WERE WRAPPED AROUND.
    //     At 2.4 MSPS that is 300k of them a second, for a figure the AGC reads ONCE A SECOND.
    //  ★★ Stuart spotted it from the outside — "the agc is hammering the CPU", with a measured
    //     111% of one core — while the DSP-thread instrumentation said the demodulator was
    //     innocent (lock 0 ms). It was: this runs on the CAPTURE thread, so it never appeared in
    //     dspWorkMs at all, and it stole the cycles the DSP thread needed anyway.
    //  ★ Max and min accumulate LANEWISE (vmaxq/vminq are ordinary cheap vector ops) and the rail
    //    hits into a byte accumulator, all reduced once at the end. Identical results.
    uint8x16_t vmax = vdupq_n_u8(0), vmin = vdupq_n_u8(255);
    uint32x4_t railAcc = vdupq_n_u32(0);
    uint8x16_t railBlk = vdupq_n_u8(0);
    int blk = 0;
    int i = 0;
    for (; i + 16 <= nF; i += 16) {
        const uint8x16_t b = vld1q_u8(in + i);
        if (st) {
            const uint8x16_t hit = vorrq_u8(vcgeq_u8(b, hiRail), vcleq_u8(b, loRail));
            railBlk = vaddq_u8(railBlk, vandq_u8(hit, one));
            vmax = vmaxq_u8(vmax, b);
            vmin = vminq_u8(vmin, b);
            // ★ Each lane gains at most 1 per iteration, so 128 iterations cannot overflow a byte.
            //   Widen and bank it before it can.
            if (++blk == 128) {
                railAcc = vaddq_u32(railAcc, vpaddlq_u16(vpaddlq_u8(railBlk)));
                railBlk = vdupq_n_u8(0); blk = 0;
            }
        }
        const uint16x8_t lo = vmovl_u8(vget_low_u8(b)), hi = vmovl_u8(vget_high_u8(b));
        const float32x4_t f0 = vcvtq_f32_u32(vmovl_u16(vget_low_u16(lo)));
        const float32x4_t f1 = vcvtq_f32_u32(vmovl_u16(vget_high_u16(lo)));
        const float32x4_t f2 = vcvtq_f32_u32(vmovl_u16(vget_low_u16(hi)));
        const float32x4_t f3 = vcvtq_f32_u32(vmovl_u16(vget_high_u16(hi)));
        vst1q_f32(out + i,      vmulq_f32(vsubq_f32(f0, bias), inv));
        vst1q_f32(out + i + 4,  vmulq_f32(vsubq_f32(f1, bias), inv));
        vst1q_f32(out + i + 8,  vmulq_f32(vsubq_f32(f2, bias), inv));
        vst1q_f32(out + i + 12, vmulq_f32(vsubq_f32(f3, bias), inv));
    }
    if (st) {
        // ★ The one reduction per CALL that replaces 300k per second.
        railAcc = vaddq_u32(railAcc, vpaddlq_u16(vpaddlq_u8(railBlk)));
        rails += vaddvq_u32(railAcc);
        mx = vmaxvq_u8(vmax);
        mn = vminvq_u8(vmin);
    }
    for (; i < nF; ++i) {
        if (st) { const uint8_t v = in[i];
                  if (v >= 254 || v <= 1) ++rails;
                  if (v > mx) mx = v; if (v < mn) mn = v; }
        out[i] = ((float)in[i] - 127.4f) * (dg / 128.0f);
    }
    if (st) { st->rails += rails; st->total += (uint32_t)nF;
              if (mx > st->maxV) st->maxV = mx; if (mn < st->minV) st->minV = mn; }
#elif defined(__SSE2__)
    /* ★ x86 had NO vector path here — every RTL-SDR sample on the Lenovo and any desktop server
     *  went through the scalar loop below (2026-09-16). Same shape as the NEON kernel: convert
     *  sixteen bytes, accumulate the ADC stats lane-wise, reduce once per call. */
    const __m128 bias = _mm_set1_ps(127.4f), inv = _mm_set1_ps(dg / 128.0f);
    const __m128i zero = _mm_setzero_si128();
    uint32_t rails = 0; uint8_t mx = 0, mn = 255;
    __m128i vmax = _mm_setzero_si128(), vmin = _mm_set1_epi8((char)255), railBlk = _mm_setzero_si128();
    // ★ unsigned compare via the max trick: b >= 254  <=>  max(b,254) == b ; b <= 1 <=> min(b,1) == b
    const __m128i hiRail = _mm_set1_epi8((char)254), loRail = _mm_set1_epi8(1), one = _mm_set1_epi8(1);
    uint32_t railAcc = 0; int blk = 0;
    int i = 0;
    for (; i + 16 <= nF; i += 16) {
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + i));
        if (st) {
            const __m128i hit = _mm_or_si128(_mm_cmpeq_epi8(_mm_max_epu8(b, hiRail), b),
                                             _mm_cmpeq_epi8(_mm_min_epu8(b, loRail), b));
            railBlk = _mm_add_epi8(railBlk, _mm_and_si128(hit, one));
            vmax = _mm_max_epu8(vmax, b);
            vmin = _mm_min_epu8(vmin, b);
            if (++blk == 128) {
                const __m128i w = _mm_sad_epu8(railBlk, zero);      // two 64-bit sums of the bytes
                railAcc += (uint32_t)_mm_cvtsi128_si32(w) + (uint32_t)_mm_cvtsi128_si32(_mm_srli_si128(w, 8));
                railBlk = zero; blk = 0;
            }
        }
        const __m128i lo = _mm_unpacklo_epi8(b, zero), hi = _mm_unpackhi_epi8(b, zero);
        const __m128 f0 = _mm_cvtepi32_ps(_mm_unpacklo_epi16(lo, zero));
        const __m128 f1 = _mm_cvtepi32_ps(_mm_unpackhi_epi16(lo, zero));
        const __m128 f2 = _mm_cvtepi32_ps(_mm_unpacklo_epi16(hi, zero));
        const __m128 f3 = _mm_cvtepi32_ps(_mm_unpackhi_epi16(hi, zero));
        _mm_storeu_ps(out + i,      _mm_mul_ps(_mm_sub_ps(f0, bias), inv));
        _mm_storeu_ps(out + i + 4,  _mm_mul_ps(_mm_sub_ps(f1, bias), inv));
        _mm_storeu_ps(out + i + 8,  _mm_mul_ps(_mm_sub_ps(f2, bias), inv));
        _mm_storeu_ps(out + i + 12, _mm_mul_ps(_mm_sub_ps(f3, bias), inv));
    }
    if (st) {
        const __m128i w = _mm_sad_epu8(railBlk, zero);
        railAcc += (uint32_t)_mm_cvtsi128_si32(w) + (uint32_t)_mm_cvtsi128_si32(_mm_srli_si128(w, 8));
        rails += railAcc;
        alignas(16) uint8_t t[16];
        _mm_store_si128(reinterpret_cast<__m128i*>(t), vmax); for (int q = 0; q < 16; ++q) if (t[q] > mx) mx = t[q];
        _mm_store_si128(reinterpret_cast<__m128i*>(t), vmin); for (int q = 0; q < 16; ++q) if (t[q] < mn) mn = t[q];
    }
    for (; i < nF; ++i) {
        if (st) { const uint8_t v = in[i];
                  if (v >= 254 || v <= 1) ++rails;
                  if (v > mx) mx = v; if (v < mn) mn = v; }
        out[i] = ((float)in[i] - 127.4f) * (dg / 128.0f);
    }
    /* ★★★ HAND THE STATISTICS BACK (2026-11-14). This branch counted them and never wrote them to *st, so on every
     *  x86 server with an RTL-SDR the ADC peak read "no measurement" (-99) and the clip rate 0 from 2026-09-16 on:
     *  VibeAGC was blind to overload and never cut. Mad Radio DXer's VirtualBox server sat at 48 dB on his strong
     *  local 12B ("strong multiplexes suffer"). test-u8-conv holds all three paths to the same answer. */
    if (st) { st->rails += rails; st->total += (uint32_t)nF;
              if (mx > st->maxV) st->maxV = mx; if (mn < st->minV) st->minV = mn; }
#else
    uint32_t rails = 0; uint8_t mx = 0, mn = 255;
    for (int i = 0; i < nF; ++i) {
        if (st) { const uint8_t v = in[i];
                  if (v >= 254 || v <= 1) ++rails;
                  if (v > mx) mx = v; if (v < mn) mn = v; }
        out[i] = ((float)in[i] - 127.4f) * (dg / 128.0f);
    }
    if (st) { st->rails += rails; st->total += (uint32_t)nF;
              if (mx > st->maxV) st->maxV = mx; if (mn < st->minV) st->minV = mn; }
#endif
}

}  // namespace vibe
