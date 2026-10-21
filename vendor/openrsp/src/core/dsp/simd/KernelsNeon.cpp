// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the OpenRSP authors
//
// This file is part of OpenRSP.
//
// OpenRSP is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version.
//
// OpenRSP is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with OpenRSP. If
// not, see <https://www.gnu.org/licenses/>.

// NEON kernel table: aarch64 (always available) and armv7 (compiled with -mfpu=neon by
// cmake/DspSimd.cmake, selected only when the kernel reports HWCAP_NEON). Bit-exact with
// the scalar reference (Kernels.h). Self-contained: internal
// linkage only. Doubles: aarch64 uses float64x2 (separate fmul/fadd, the TU is built with
// -ffp-contract=off like the rest of the core); armv7 NEON has no double lanes, so the
// DC trackers run as two interleaved scalar chains there.
#include "core/dsp/Kernels.h"

#if defined(__aarch64__) || defined(_M_ARM64) || (defined(__arm__) && defined(__ARM_NEON))
#include <arm_neon.h>

#include "platform/Cpu.h"

#include <cstddef>
#include <cstdint>

namespace sdrplay {
namespace dsp {
namespace {

#include "ScalarRef.inc"

struct Ops {
    using V = int32x4_t;
    static constexpr unsigned W = 4;
    static V load(const std::int32_t *p) { return vld1q_s32(p); }
    static void store(std::int32_t *p, V v) { vst1q_s32(p, v); }
    static V set1(std::int32_t v) { return vdupq_n_s32(v); }
    static V zero() { return vdupq_n_s32(0); }
    static V add(V a, V b) { return vaddq_s32(a, b); }
    static V sub(V a, V b) { return vsubq_s32(a, b); }
    static V band(V a, V b) { return vandq_s32(a, b); }
    static V mul(V a, V b) { return vmulq_s32(a, b); } // low 32 bits (wraps)
    static V bor(V a, V b) { return vorrq_s32(a, b); }
    static bool allZero(V a)
    {
        const uint32x4_t u = vreinterpretq_u32_s32(a);
        const uint32x2_t m = vorr_u32(vget_low_u32(u), vget_high_u32(u));
        return (vget_lane_u32(m, 0) | vget_lane_u32(m, 1)) == 0;
    }
    static constexpr bool kMul16 = false; // native 32-bit multiply
    static V coef16(std::int32_t c) { return set1(c); }
    static V mul16(V v, V c) { return mul(v, c); }
    static V not16(V v) { return v; }
    static V sra(V a, unsigned s) { return vshlq_s32(a, vdupq_n_s32(-int(s))); }
    static V widen16(const std::int16_t *p) { return vmovl_s16(vld1_s16(p)); }
    static void deint2(const std::int32_t *src, std::int32_t *ev, std::int32_t *od)
    {
        const int32x4x2_t d = vld2q_s32(src);
        vst1q_s32(ev, d.val[0]);
        vst1q_s32(od, d.val[1]);
    }
    static void narrow16(std::int16_t *p, V v) { vst1_s16(p, vmovn_s32(v)); } // truncating = wrap
};

#include "Generic32.inc"

// 16-byte table lookup (pshufb without the zeroing bit; indices < 16)
inline uint8x16_t tbl16(uint8x16_t t, uint8x16_t idx)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return vqtbl1q_u8(t, idx);
#else
    uint8x8x2_t tt;
    tt.val[0] = vget_low_u8(t);
    tt.val[1] = vget_high_u8(t);
    return vcombine_u8(vtbl2_u8(tt, vget_low_u8(idx)), vtbl2_u8(tt, vget_high_u8(idx)));
#endif
}

// ---------------------------------------------------------------- unpack
// Payload words are read as bytes and reinterpreted (no int16* cast of the byte buffer:
// -Wcast-align on armv7; vld1 has no alignment requirement).
inline int16x8_t ld16(const std::uint8_t *p) { return vreinterpretq_s16_u8(vld1q_u8(p)); }

void unpack14(const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *xq)
{
    const std::uint8_t *p = pkt + 16;
    unsigned k = 0;
    for (; k + 8 <= 252; k += 8) {
        const int16x8x2_t d = vuzpq_s16(ld16(p + 4 * k), ld16(p + 4 * k + 16)); // I words | Q words
        vst1q_s16(xi + k, vshlq_n_s16(d.val[0], 2));
        vst1q_s16(xq + k, vshlq_n_s16(d.val[1], 2));
    }
    for (; k < 252; ++k) {
        xi[k] = r_wrap16(std::int32_t(r_rd16(p + 4 * k)) << 2);
        xq[k] = r_wrap16(std::int32_t(r_rd16(p + 4 * k + 2)) << 2);
    }
}

void unpack8(const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *xq)
{
    const std::uint8_t *p = pkt + 16;
    const int16x8_t hiMask = vdupq_n_s16(static_cast<std::int16_t>(0xFF00));
    for (unsigned k = 0; k < 504; k += 8) {
        const int16x8_t w = ld16(p + 2 * k);
        vst1q_s16(xi + k, vshlq_n_s16(w, 8));
        vst1q_s16(xq + k, vandq_s16(w, hiMask));
    }
}

void unpackReal(const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *)
{
    const std::uint8_t *p = pkt + 16;
    for (unsigned k = 0; k < 504; k += 8) vst1q_s16(xi + k, vshlq_n_s16(ld16(p + 2 * k), 2));
}

// 12-bit: vld3 splits the 3 bytes of 8 pairs; I = (b1:b0) << 4, Q = (b2:b1) & 0xFFF0.
void unpack12(const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *xq)
{
    const std::uint8_t *p = pkt + 16;
    const uint16x8_t qMask = vdupq_n_u16(0xFFF0);
    for (unsigned k = 0; k < 336; k += 8) { // 42 x 24 bytes = the 1008-byte payload
        const uint8x8x3_t b = vld3_u8(p + 3 * k);
        const uint16x8_t w0 = vorrq_u16(vmovl_u8(b.val[0]), vshll_n_u8(b.val[1], 8));
        const uint16x8_t w1 = vorrq_u16(vmovl_u8(b.val[1]), vshll_n_u8(b.val[2], 8));
        vst1q_s16(xi + k, vreinterpretq_s16_u16(vshlq_n_u16(w0, 4)));
        vst1q_s16(xq + k, vreinterpretq_s16_u16(vandq_u16(w1, qMask)));
    }
}

// 10-bit: per group of 4 pairs (10 bytes): I words at bytes 0,2,5,7 (<< 6,2,6,2), Q words
// at bytes 1,3,6,8 (<< 4,0,4,0); & 0xFFC0; >> sh (the group's exponent code).
void unpack10(const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *xq)
{
    const std::uint8_t *p = pkt + 16;
    static const std::uint8_t kIdx[16] = {0, 1, 2, 3, 5, 6, 7, 8, 1, 2, 3, 4, 6, 7, 8, 9};
    static const std::int16_t kSh[8] = {6, 2, 6, 2, 4, 0, 4, 0};
    const uint8x16_t idx = vld1q_u8(kIdx);
    const int16x8_t shl = vld1q_s16(kSh);
    const uint16x8_t mask = vdupq_n_u16(0xFFC0);
    for (unsigned b = 0; b < 6; ++b) {
        const std::uint8_t *blk = p + 164 * b;
        std::uint32_t codes = r_rd32(blk + 160);
        std::int16_t *oi = xi + 64 * b, *oq = xq + 64 * b;
        for (unsigned g = 0; g < 16; ++g, codes >>= 2) {
            const unsigned c = codes & 3u;
            const int sh = c == 3 ? 0 : int(2 - c);
            // reads blk + 10g .. +15 <= blk + 165: inside the 1024-byte packet
            const uint16x8_t w = vreinterpretq_u16_u8(tbl16(vld1q_u8(blk + 10 * g), idx));
            const int16x8_t v = vreinterpretq_s16_u16(vandq_u16(vshlq_u16(w, shl), mask));
            const int16x8_t r = vshlq_s16(v, vdupq_n_s16(static_cast<std::int16_t>(-sh)));
            vst1_s16(oi + 4 * g, vget_low_s16(r));
            vst1_s16(oq + 4 * g, vget_high_s16(r));
        }
    }
}

// ---------------------------------------------------------------- IQ correction
// (x * coef) >> 15, low 16 bits (vmovn wraps)
inline int16x8_t scaleQ15v(int16x8_t v, int16x4_t c)
{
    return vcombine_s16(vmovn_s32(vshrq_n_s32(vmull_s16(vget_low_s16(v), c), 15)),
                        vmovn_s32(vshrq_n_s32(vmull_s16(vget_high_s16(v), c), 15)));
}
// acc += 16-bit abs of v: abs(-32768) = 0x8000 (unsigned 32768)
inline uint32x4_t addAbs16(uint32x4_t acc, int16x8_t v) { return vpadalq_u16(acc, vreinterpretq_u16_s16(vabsq_s16(v))); }
inline std::uint32_t hsum32(uint32x4_t v)
{
    return vgetq_lane_u32(v, 0) + vgetq_lane_u32(v, 1) + vgetq_lane_u32(v, 2) + vgetq_lane_u32(v, 3);
}

// One pass per 8 samples. Rotation: a*K -/+ b*K with |a*K| < 2^30, exact in int32.
template <int M0, int M1>
void iqBalanceT(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, const int *mode, const int *coef, std::uint32_t *sums)
{
    const int16x4_t c0 = vdup_n_s16(static_cast<std::int16_t>(coef[0])), c1 = vdup_n_s16(static_cast<std::int16_t>(coef[1]));
    uint32x4_t s0 = vdupq_n_u32(0), s1 = s0, s2 = s0, s3 = s0;
    std::uint32_t k = 0;
    for (; k + 8 <= n; k += 8) {
        int16x8_t a = vld1q_s16(xi + k), b = vld1q_s16(xq + k);
        if (M0 == 1) a = scaleQ15v(a, c0);
        if (M0 == 2) b = scaleQ15v(b, c0);
        s0 = addAbs16(s0, a);
        s1 = addAbs16(s1, b);
        const int32x4_t al = vmull_n_s16(vget_low_s16(a), 23170), ah = vmull_n_s16(vget_high_s16(a), 23170);
        const int32x4_t dl = vshrq_n_s32(vmlsl_n_s16(al, vget_low_s16(b), 23170), 15);
        const int32x4_t dh = vshrq_n_s32(vmlsl_n_s16(ah, vget_high_s16(b), 23170), 15);
        const int32x4_t sl = vshrq_n_s32(vmlal_n_s16(al, vget_low_s16(b), 23170), 15);
        const int32x4_t sh = vshrq_n_s32(vmlal_n_s16(ah, vget_high_s16(b), 23170), 15);
        a = vcombine_s16(vmovn_s32(dl), vmovn_s32(dh));
        b = vcombine_s16(vmovn_s32(sl), vmovn_s32(sh));
        if (M1 == 1) a = scaleQ15v(a, c1);
        if (M1 == 2) b = scaleQ15v(b, c1);
        s2 = addAbs16(s2, a);
        s3 = addAbs16(s3, b);
        vst1q_s16(xi + k, a);
        vst1q_s16(xq + k, b);
    }
    sums[0] = hsum32(s0);
    sums[1] = hsum32(s1);
    sums[2] = hsum32(s2);
    sums[3] = hsum32(s3);
    r_iqBalance(xi + k, xq + k, n - k, mode, coef, sums);
}

// Modes other than 1 and 2 scale nothing (r_iqSample); a coef outside int16 takes the scalar path.
inline int iqMode(int m) { return m == 1 || m == 2 ? m : 0; }
inline bool iqCoefOk(int m, int coef) { return m == 0 || (coef >= -32768 && coef <= 32767); }

void iqBalance(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, const int mode[2], const int coef[2],
               std::uint32_t sums[4])
{
    const int m0 = iqMode(mode[0]), m1 = iqMode(mode[1]);
    if (!iqCoefOk(m0, coef[0]) || !iqCoefOk(m1, coef[1])) {
        sums[0] = sums[1] = sums[2] = sums[3] = 0;
        r_iqBalance(xi, xq, n, mode, coef, sums);
        return;
    }
    using Fn = void (*)(std::int16_t *, std::int16_t *, std::uint32_t, const int *, const int *, std::uint32_t *);
    static const Fn fns[9] = {iqBalanceT<0, 0>, iqBalanceT<0, 1>, iqBalanceT<0, 2>, iqBalanceT<1, 0>, iqBalanceT<1, 1>,
                              iqBalanceT<1, 2>, iqBalanceT<2, 0>, iqBalanceT<2, 1>, iqBalanceT<2, 2>};
    fns[m0 * 3 + m1](xi, xq, n, mode, coef, sums);
}

// ---------------------------------------------------------------- power
std::uint64_t sumSquares(const std::int16_t *xi, const std::int16_t *xq, std::uint32_t n, bool iOnly)
{
    int64x2_t acc = vdupq_n_s64(0);
    std::uint32_t k = 0;
    for (; k + 8 <= n; k += 8) {
        const int16x8_t a = vld1q_s16(xi + k);
        acc = vpadalq_s32(acc, vmull_s16(vget_low_s16(a), vget_low_s16(a)));   // each square <= 2^30
        acc = vpadalq_s32(acc, vmull_s16(vget_high_s16(a), vget_high_s16(a)));
        if (!iOnly) {
            const int16x8_t b = vld1q_s16(xq + k);
            acc = vpadalq_s32(acc, vmull_s16(vget_low_s16(b), vget_low_s16(b)));
            acc = vpadalq_s32(acc, vmull_s16(vget_high_s16(b), vget_high_s16(b)));
        }
    }
    std::uint64_t s = static_cast<std::uint64_t>(vgetq_lane_s64(acc, 0)) + static_cast<std::uint64_t>(vgetq_lane_s64(acc, 1));
    for (; k < n; ++k) {
        const std::int64_t a = xi[k];
        s += std::uint64_t(a * a);
        if (!iOnly) {
            const std::int64_t b = xq[k];
            s += std::uint64_t(b * b);
        }
    }
    return s;
}

// ---------------------------------------------------------------- DC
#if defined(__aarch64__) || defined(_M_ARM64)
// One 8-sample block of r_dcChannel in four float64x2_t (lanes 0-1, 2-3, 4-5, 6-7): the same
// multiplies and adds per lane, in the same order (no fused multiply-add: separate intrinsics
// + -ffp-contract=off); fcvtzs truncates like the scalar conversion; sqxtn saturates like
// sat16. Returns the new DC in both lanes.
inline float64x2_t dcBlock(std::int16_t *p, float64x2_t d, const r_DcCoef &K)
{
    const float64x2_t cc = vdupq_n_f64(K.c), a1 = vdupq_n_f64(K.a), a2 = vdupq_n_f64(K.a2), z = vdupq_n_f64(0.0);
    const int16x8_t v = vld1q_s16(p);
    const int32x4_t lo = vmovl_s16(vget_low_s16(v)), hi = vmovl_s16(vget_high_s16(v));
    float64x2_t r0 = vmulq_f64(vcvtq_f64_s64(vmovl_s32(vget_low_s32(lo))), cc);
    float64x2_t r1 = vmulq_f64(vcvtq_f64_s64(vmovl_s32(vget_high_s32(lo))), cc);
    float64x2_t r2 = vmulq_f64(vcvtq_f64_s64(vmovl_s32(vget_low_s32(hi))), cc);
    float64x2_t r3 = vmulq_f64(vcvtq_f64_s64(vmovl_s32(vget_high_s32(hi))), cc);
    const float64x2_t t0 = vextq_f64(z, r0, 1), t1 = vextq_f64(r0, r1, 1);
    const float64x2_t t2 = vextq_f64(z, r2, 1), t3 = vextq_f64(r2, r3, 1);
    r0 = vaddq_f64(r0, vmulq_f64(a1, t0));
    r1 = vaddq_f64(r1, vmulq_f64(a1, t1));
    r2 = vaddq_f64(r2, vmulq_f64(a1, t2));
    r3 = vaddq_f64(r3, vmulq_f64(a1, t3));
    r1 = vaddq_f64(r1, vmulq_f64(a2, r0));
    r3 = vaddq_f64(r3, vmulq_f64(a2, r2));
    const float64x2_t h3 = vdupq_laneq_f64(r1, 1);
    const float64x2_t p12 = vld1q_f64(K.p + 1), p34 = vld1q_f64(K.p + 3);
    const float64x2_t p56 = vld1q_f64(K.p + 5), p78 = vld1q_f64(K.p + 7);
    r2 = vaddq_f64(r2, vmulq_f64(p12, h3));
    r3 = vaddq_f64(r3, vmulq_f64(p34, h3));
    r0 = vaddq_f64(r0, vmulq_f64(p12, d));
    r1 = vaddq_f64(r1, vmulq_f64(p34, d));
    r2 = vaddq_f64(r2, vmulq_f64(p56, d));
    r3 = vaddq_f64(r3, vmulq_f64(p78, d));
    const int32x4_t i0 = vcombine_s32(vmovn_s64(vcvtq_s64_f64(r0)), vmovn_s64(vcvtq_s64_f64(r1)));
    const int32x4_t i1 = vcombine_s32(vmovn_s64(vcvtq_s64_f64(r2)), vmovn_s64(vcvtq_s64_f64(r3)));
    vst1q_s16(p, vcombine_s16(vqmovn_s32(vsubq_s32(lo, i0)), vqmovn_s32(vsubq_s32(hi, i1))));
    return vdupq_laneq_f64(r3, 1);
}

void dcCorrect(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, double *dcI, double *dcQ, double c)
{
    const r_DcCoef K = r_dcCoef(c);
    const std::uint32_t nb = n & ~7u;
    float64x2_t di = vdupq_n_f64(*dcI);
    if (!xq) {
        for (std::uint32_t k = 0; k < nb; k += 8) di = dcBlock(xi + k, di, K);
        *dcI = vgetq_lane_f64(di, 0);
        r_dcTail(xi + nb, n - nb, *dcI, c);
        return;
    }
    // The I and Q chains are independent, so interleaving their blocks lets the CPU overlap them.
    float64x2_t dq = vdupq_n_f64(*dcQ);
    for (std::uint32_t k = 0; k < nb; k += 8) {
        di = dcBlock(xi + k, di, K);
        dq = dcBlock(xq + k, dq, K);
    }
    *dcI = vgetq_lane_f64(di, 0);
    *dcQ = vgetq_lane_f64(dq, 0);
    r_dcTail(xi + nb, n - nb, *dcI, c);
    r_dcTail(xq + nb, n - nb, *dcQ, c);
}
#else
// armv7 NEON has no double lanes: the scalar reference per channel.
void dcCorrect(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, double *dcI, double *dcQ, double c)
{
    r_dcChannel(xi, n, *dcI, c);
    if (xq) r_dcChannel(xq, n, *dcQ, c);
}
#endif

void firDecimate(const std::int32_t *u, std::uint32_t nOut, std::uint32_t M, const std::int32_t *h, std::uint32_t L,
                 unsigned shift, std::int32_t *y)
{
    firDecimateT<Ops>(u, nOut, M, h, L, shift, y);
}
std::uint32_t mixTable(const std::int16_t *x, std::uint32_t n, const std::int32_t *tab, std::uint32_t period,
                       std::uint32_t phase, std::int32_t *out)
{
    return mixTableT<Ops>(x, n, tab, period, phase, out);
}
void mixFs4(const std::int16_t *x, std::uint32_t n, std::int32_t *mi, std::int32_t *mq) { mixFs4T<Ops>(x, n, mi, mq); }
void narrow16(const std::int32_t *src, std::uint32_t n, std::int16_t *dst) { narrow16T<Ops>(src, n, dst); }

const Kernels kNeon = {
    "neon",
    {unpack14, unpack12, unpack10, unpack8, unpackReal},
    iqBalance,
    firDecimate,
    sumSquares,
    dcCorrect,
    mixFs4,
    mixTable,
    narrow16,
};

} // namespace

namespace simd {
const Kernels *neonKernels()
{
    // armv7: only when the kernel reports HWCAP_NEON; aarch64: always (platform/Cpu.h).
    return platform::cpuHasArmNeon() ? &kNeon : nullptr;
}
} // namespace simd
} // namespace dsp
} // namespace sdrplay

#else
namespace sdrplay {
namespace dsp {
namespace simd {
const Kernels *neonKernels() { return nullptr; }
} // namespace simd
} // namespace dsp
} // namespace sdrplay
#endif
