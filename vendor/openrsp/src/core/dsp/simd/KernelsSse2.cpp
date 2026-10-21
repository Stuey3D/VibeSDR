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

// SSE2 kernel table (x86-64 baseline: every x86-64 CPU has SSE2). Bit-exact with the
// scalar reference (Kernels.h). Self-contained: internal linkage
// only (see cmake/DspSimd.cmake).
#include "core/dsp/Kernels.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>

#include <cstddef>
#include <cstdint>

namespace sdrplay {
namespace dsp {
namespace {

#include "ScalarRef.inc"

struct Ops {
    using V = __m128i;
    static constexpr unsigned W = 4;
    static V load(const std::int32_t *p) { return _mm_loadu_si128(reinterpret_cast<const __m128i *>(p)); }
    static void store(std::int32_t *p, V v) { _mm_storeu_si128(reinterpret_cast<__m128i *>(p), v); }
    static V set1(std::int32_t v) { return _mm_set1_epi32(v); }
    static V zero() { return _mm_setzero_si128(); }
    static V add(V a, V b) { return _mm_add_epi32(a, b); }
    static V sub(V a, V b) { return _mm_sub_epi32(a, b); }
    static V band(V a, V b) { return _mm_and_si128(a, b); }
    static V mul(V a, V b) // low 32 bits of each product (no pmulld in SSE2)
    {
        const __m128i e = _mm_mul_epu32(a, b);
        const __m128i o = _mm_mul_epu32(_mm_srli_epi64(a, 32), _mm_srli_epi64(b, 32));
        return _mm_unpacklo_epi32(_mm_shuffle_epi32(e, _MM_SHUFFLE(0, 0, 2, 0)), _mm_shuffle_epi32(o, _MM_SHUFFLE(0, 0, 2, 0)));
    }
    static V bor(V a, V b) { return _mm_or_si128(a, b); }
    static bool allZero(V a) { return _mm_movemask_epi8(_mm_cmpeq_epi32(a, _mm_setzero_si128())) == 0xFFFF; }
    static constexpr bool kMul16 = true;
    static V coef16(std::int32_t c) { return _mm_set1_epi32(int(std::uint32_t(std::uint16_t(c)))); } // [c, 0] word pairs
    static V mul16(V v, V c) { return _mm_madd_epi16(v, c); } // low word * c + high word * 0 (exact for int16 v)
    static V not16(V v) { return _mm_xor_si128(_mm_srai_epi32(_mm_slli_epi32(v, 16), 16), v); }
    static V sra(V a, unsigned s) { return _mm_sra_epi32(a, _mm_cvtsi32_si128(int(s))); }
    static V widen16(const std::int16_t *p)
    {
        const __m128i x = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(p));
        return _mm_srai_epi32(_mm_unpacklo_epi16(x, x), 16);
    }
    static void deint2(const std::int32_t *src, std::int32_t *ev, std::int32_t *od)
    {
        const __m128i a = _mm_shuffle_epi32(load(src), _MM_SHUFFLE(3, 1, 2, 0));     // 0 2 1 3
        const __m128i b = _mm_shuffle_epi32(load(src + 4), _MM_SHUFFLE(3, 1, 2, 0)); // 4 6 5 7
        store(ev, _mm_unpacklo_epi64(a, b));
        store(od, _mm_unpackhi_epi64(a, b));
    }
    static void narrow16(std::int16_t *p, V v)
    {
        const __m128i w = _mm_srai_epi32(_mm_slli_epi32(v, 16), 16); // low 16 bits, sign-extended: packs is exact
        _mm_storel_epi64(reinterpret_cast<__m128i *>(p), _mm_packs_epi32(w, w));
    }
};

#include "Generic32.inc"

inline __m128i ld(const void *p) { return _mm_loadu_si128(static_cast<const __m128i *>(p)); }
inline void st(void *p, __m128i v) { _mm_storeu_si128(static_cast<__m128i *>(p), v); }
// 8 int32 (two vectors) -> 8 int16, wrapping
inline __m128i wrapPack(__m128i a, __m128i b)
{
    return _mm_packs_epi32(_mm_srai_epi32(_mm_slli_epi32(a, 16), 16), _mm_srai_epi32(_mm_slli_epi32(b, 16), 16));
}

// ---------------------------------------------------------------- unpack
// 14-bit IQ: 252 x {int16 I, int16 Q} -> (int16)(v << 2)
void unpack14(const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *xq)
{
    const std::uint8_t *p = pkt + 16;
    unsigned k = 0;
    for (; k + 8 <= 252; k += 8) {
        const __m128i a = ld(p + 4 * k), b = ld(p + 4 * k + 16);
        const __m128i ia = _mm_srai_epi32(_mm_slli_epi32(a, 16), 16), ib = _mm_srai_epi32(_mm_slli_epi32(b, 16), 16);
        const __m128i qa = _mm_srai_epi32(a, 16), qb = _mm_srai_epi32(b, 16);
        st(xi + k, _mm_slli_epi16(_mm_packs_epi32(ia, ib), 2));
        st(xq + k, _mm_slli_epi16(_mm_packs_epi32(qa, qb), 2));
    }
    for (; k < 252; ++k) {
        xi[k] = r_wrap16(std::int32_t(r_rd16(p + 4 * k)) << 2);
        xq[k] = r_wrap16(std::int32_t(r_rd16(p + 4 * k + 2)) << 2);
    }
}

// 8-bit IQ: 504 x {int8 I, int8 Q} -> v * 256: I = word << 8, Q = word & 0xFF00
void unpack8(const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *xq)
{
    const std::uint8_t *p = pkt + 16;
    const __m128i hiMask = _mm_set1_epi16(static_cast<short>(0xFF00));
    for (unsigned k = 0; k < 504; k += 8) { // 63 x 8 pairs; loads end exactly at the payload end
        const __m128i a = ld(p + 2 * k);
        st(xi + k, _mm_slli_epi16(a, 8));
        st(xq + k, _mm_and_si128(a, hiMask));
    }
}

// 14-bit real: 504 x int16 -> (int16)(v << 2), xq untouched
void unpackReal(const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *)
{
    const std::uint8_t *p = pkt + 16;
    for (unsigned k = 0; k < 504; k += 8) st(xi + k, _mm_slli_epi16(ld(p + 2 * k), 2));
}

void unpack12(const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *xq) { r_unpack12(pkt, xi, xq, 0, 336); }
void unpack10(const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *xq) { r_unpack10(pkt, xi, xq); }

// ---------------------------------------------------------------- IQ correction
// (x * coef) >> 15, low 16 bits: product P = hi:lo (16x16 -> 32 exact for a 16-bit coef);
// bits 15..30 of P = (hi << 1) | (lo >> 15).
inline __m128i scaleQ15v(__m128i v, __m128i c)
{
    return _mm_or_si128(_mm_slli_epi16(_mm_mulhi_epi16(v, c), 1), _mm_srli_epi16(_mm_mullo_epi16(v, c), 15));
}
// acc (4 x u32) += 16-bit abs of v: -32768 -> 0x8000 (unsigned 32768)
inline __m128i addAbs16(__m128i acc, __m128i v)
{
    const __m128i s = _mm_srai_epi16(v, 15), z = _mm_setzero_si128();
    const __m128i a = _mm_sub_epi16(_mm_xor_si128(v, s), s);
    return _mm_add_epi32(acc, _mm_add_epi32(_mm_unpacklo_epi16(a, z), _mm_unpackhi_epi16(a, z)));
}
inline std::uint32_t hsum32(__m128i v)
{
    alignas(16) std::uint32_t l[4];
    _mm_store_si128(reinterpret_cast<__m128i *>(l), v);
    return l[0] + l[1] + l[2] + l[3];
}

// One pass per 8 samples. The rotation: I' = ((I - Q) * 23170) >> 15, Q' = ((I + Q) * 23170) >> 15
// via pmaddwd on (I, Q) pairs: I*K - Q*K and I*K + Q*K are exact in int32 (|.| < 2^31).
template <int M0, int M1>
void iqBalanceT(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, const int *mode, const int *coef, std::uint32_t *sums)
{
    const __m128i c0 = _mm_set1_epi16(static_cast<short>(coef[0])), c1 = _mm_set1_epi16(static_cast<short>(coef[1]));
    const __m128i kd = _mm_set1_epi32(int(std::uint32_t(23170) | (std::uint32_t(std::uint16_t(-23170)) << 16)));
    const __m128i ks = _mm_set1_epi32(int(std::uint32_t(23170) | (std::uint32_t(23170) << 16)));
    __m128i s0 = _mm_setzero_si128(), s1 = s0, s2 = s0, s3 = s0;
    std::uint32_t k = 0;
    for (; k + 8 <= n; k += 8) {
        __m128i a = ld(xi + k), b = ld(xq + k);
        if (M0 == 1) a = scaleQ15v(a, c0);
        if (M0 == 2) b = scaleQ15v(b, c0);
        s0 = addAbs16(s0, a);
        s1 = addAbs16(s1, b);
        const __m128i lo = _mm_unpacklo_epi16(a, b), hi = _mm_unpackhi_epi16(a, b);
        a = wrapPack(_mm_srai_epi32(_mm_madd_epi16(lo, kd), 15), _mm_srai_epi32(_mm_madd_epi16(hi, kd), 15));
        b = wrapPack(_mm_srai_epi32(_mm_madd_epi16(lo, ks), 15), _mm_srai_epi32(_mm_madd_epi16(hi, ks), 15));
        if (M1 == 1) a = scaleQ15v(a, c1);
        if (M1 == 2) b = scaleQ15v(b, c1);
        s2 = addAbs16(s2, a);
        s3 = addAbs16(s3, b);
        st(xi + k, a);
        st(xq + k, b);
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
// pmaddwd(x, x) = x0^2 + x1^2 <= 2^31: exact as uint32; accumulate in uint64 lanes.
std::uint64_t sumSquares(const std::int16_t *xi, const std::int16_t *xq, std::uint32_t n, bool iOnly)
{
    const __m128i z = _mm_setzero_si128();
    __m128i acc = _mm_setzero_si128();
    std::uint32_t k = 0;
    for (; k + 8 <= n; k += 8) {
        const __m128i a = ld(xi + k);
        const __m128i sa = _mm_madd_epi16(a, a);
        acc = _mm_add_epi64(acc, _mm_add_epi64(_mm_unpacklo_epi32(sa, z), _mm_unpackhi_epi32(sa, z)));
        if (!iOnly) {
            const __m128i b = ld(xq + k);
            const __m128i sb = _mm_madd_epi16(b, b);
            acc = _mm_add_epi64(acc, _mm_add_epi64(_mm_unpacklo_epi32(sb, z), _mm_unpackhi_epi32(sb, z)));
        }
    }
    alignas(16) std::uint64_t l[2];
    _mm_store_si128(reinterpret_cast<__m128i *>(l), acc);
    std::uint64_t s = l[0] + l[1];
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
// One 8-sample block of r_dcChannel in four __m128d (lanes 0-1, 2-3, 4-5, 6-7): the same
// multiplies and adds per lane, in the same order. Returns the new DC in both lanes.
inline __m128d dcBlock(std::int16_t *p, __m128d d, const r_DcCoef &K)
{
    const __m128d cc = _mm_set1_pd(K.c), a1 = _mm_set1_pd(K.a), a2 = _mm_set1_pd(K.a2), z = _mm_setzero_pd();
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p));
    const __m128i sg = _mm_srai_epi16(v, 15);
    const __m128i lo = _mm_unpacklo_epi16(v, sg), hi = _mm_unpackhi_epi16(v, sg);
    __m128d r0 = _mm_mul_pd(_mm_cvtepi32_pd(lo), cc), r1 = _mm_mul_pd(_mm_cvtepi32_pd(_mm_srli_si128(lo, 8)), cc);
    __m128d r2 = _mm_mul_pd(_mm_cvtepi32_pd(hi), cc), r3 = _mm_mul_pd(_mm_cvtepi32_pd(_mm_srli_si128(hi, 8)), cc);
    const __m128d t0 = _mm_unpacklo_pd(z, r0), t1 = _mm_shuffle_pd(r0, r1, 1);
    const __m128d t2 = _mm_unpacklo_pd(z, r2), t3 = _mm_shuffle_pd(r2, r3, 1);
    r0 = _mm_add_pd(r0, _mm_mul_pd(a1, t0));
    r1 = _mm_add_pd(r1, _mm_mul_pd(a1, t1));
    r2 = _mm_add_pd(r2, _mm_mul_pd(a1, t2));
    r3 = _mm_add_pd(r3, _mm_mul_pd(a1, t3));
    r1 = _mm_add_pd(r1, _mm_mul_pd(a2, r0));
    r3 = _mm_add_pd(r3, _mm_mul_pd(a2, r2));
    const __m128d h3 = _mm_unpackhi_pd(r1, r1);
    const __m128d p12 = _mm_loadu_pd(K.p + 1), p34 = _mm_loadu_pd(K.p + 3);
    const __m128d p56 = _mm_loadu_pd(K.p + 5), p78 = _mm_loadu_pd(K.p + 7);
    r2 = _mm_add_pd(r2, _mm_mul_pd(p12, h3));
    r3 = _mm_add_pd(r3, _mm_mul_pd(p34, h3));
    r0 = _mm_add_pd(r0, _mm_mul_pd(p12, d));
    r1 = _mm_add_pd(r1, _mm_mul_pd(p34, d));
    r2 = _mm_add_pd(r2, _mm_mul_pd(p56, d));
    r3 = _mm_add_pd(r3, _mm_mul_pd(p78, d));
    const __m128i i0 = _mm_unpacklo_epi64(_mm_cvttpd_epi32(r0), _mm_cvttpd_epi32(r1));
    const __m128i i1 = _mm_unpacklo_epi64(_mm_cvttpd_epi32(r2), _mm_cvttpd_epi32(r3));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(p), _mm_packs_epi32(_mm_sub_epi32(lo, i0), _mm_sub_epi32(hi, i1)));
    return _mm_unpackhi_pd(r3, r3);
}

void dcChannel(std::int16_t *x, std::uint32_t n, double &dc, double c)
{
    const r_DcCoef K = r_dcCoef(c);
    const std::uint32_t nb = n & ~7u;
    __m128d d = _mm_set1_pd(dc);
    for (std::uint32_t k = 0; k < nb; k += 8) d = dcBlock(x + k, d, K);
    dc = _mm_cvtsd_f64(d);
    r_dcTail(x + nb, n - nb, dc, c);
}

void dcCorrect(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, double *dcI, double *dcQ, double c)
{
    if (!xq) {
        dcChannel(xi, n, *dcI, c);
        return;
    }
    // The I and Q chains are independent, so interleaving their blocks lets the CPU overlap them.
    const r_DcCoef K = r_dcCoef(c);
    const std::uint32_t nb = n & ~7u;
    __m128d di = _mm_set1_pd(*dcI), dq = _mm_set1_pd(*dcQ);
    for (std::uint32_t k = 0; k < nb; k += 8) {
        di = dcBlock(xi + k, di, K);
        dq = dcBlock(xq + k, dq, K);
    }
    *dcI = _mm_cvtsd_f64(di);
    *dcQ = _mm_cvtsd_f64(dq);
    r_dcTail(xi + nb, n - nb, *dcI, c);
    r_dcTail(xq + nb, n - nb, *dcQ, c);
}

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

const Kernels kSse2 = {
    "sse2",
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
const Kernels *sse2Kernels() { return &kSse2; }
} // namespace simd
} // namespace dsp
} // namespace sdrplay

#else
namespace sdrplay {
namespace dsp {
namespace simd {
const Kernels *sse2Kernels() { return nullptr; }
} // namespace simd
} // namespace dsp
} // namespace sdrplay
#endif
