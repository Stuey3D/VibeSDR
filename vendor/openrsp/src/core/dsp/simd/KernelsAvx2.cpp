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

// AVX2 kernel table (x86-64; compiled with -mavx2 by cmake/DspSimd.cmake, selected only
// when the CPU and OS support AVX2). Bit-exact with the scalar reference (Kernels.h).
// Self-contained: internal linkage only, so nothing compiled
// with -mavx2 can be picked by the linker for baseline code.
#include "core/dsp/Kernels.h"

#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__AVX2__) || defined(_MSC_VER))
#include <immintrin.h>

#include <cstddef>
#include <cstdint>

namespace sdrplay {
namespace dsp {
namespace {

#include "ScalarRef.inc"

struct Ops {
    using V = __m256i;
    static constexpr unsigned W = 8;
    static V load(const std::int32_t *p) { return _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p)); }
    static void store(std::int32_t *p, V v) { _mm256_storeu_si256(reinterpret_cast<__m256i *>(p), v); }
    static V set1(std::int32_t v) { return _mm256_set1_epi32(v); }
    static V zero() { return _mm256_setzero_si256(); }
    static V add(V a, V b) { return _mm256_add_epi32(a, b); }
    static V sub(V a, V b) { return _mm256_sub_epi32(a, b); }
    static V band(V a, V b) { return _mm256_and_si256(a, b); }
    static V mul(V a, V b) { return _mm256_mullo_epi32(a, b); }
    static V bor(V a, V b) { return _mm256_or_si256(a, b); }
    static bool allZero(V a) { return _mm256_testz_si256(a, a) != 0; }
    static constexpr bool kMul16 = false; // native 32-bit multiply
    static V coef16(std::int32_t c) { return set1(c); }
    static V mul16(V v, V c) { return mul(v, c); }
    static V not16(V v) { return v; }
    static V sra(V a, unsigned s) { return _mm256_sra_epi32(a, _mm_cvtsi32_si128(int(s))); }
    static V widen16(const std::int16_t *p) { return _mm256_cvtepi16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i *>(p))); }
    static void deint2(const std::int32_t *src, std::int32_t *ev, std::int32_t *od)
    {
        const __m256i idx = _mm256_setr_epi32(0, 2, 4, 6, 1, 3, 5, 7);
        const __m256i a = _mm256_permutevar8x32_epi32(load(src), idx);     // e0-3 | o0-3
        const __m256i b = _mm256_permutevar8x32_epi32(load(src + 8), idx); // e4-7 | o4-7
        store(ev, _mm256_permute2x128_si256(a, b, 0x20));
        store(od, _mm256_permute2x128_si256(a, b, 0x31));
    }
    static void narrow16(std::int16_t *p, V v)
    {
        const __m256i sh = _mm256_setr_epi8(0, 1, 4, 5, 8, 9, 12, 13, -1, -1, -1, -1, -1, -1, -1, -1, 0, 1, 4, 5, 8, 9, 12,
                                            13, -1, -1, -1, -1, -1, -1, -1, -1);
        const __m256i t = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(v, sh), 0x08); // qwords 0, 2
        _mm_storeu_si128(reinterpret_cast<__m128i *>(p), _mm256_castsi256_si128(t));
    }
};

#include "Generic32.inc"

inline __m128i ld(const void *p) { return _mm_loadu_si128(static_cast<const __m128i *>(p)); }
inline void st(void *p, __m128i v) { _mm_storeu_si128(static_cast<__m128i *>(p), v); }
inline __m256i ld2(const void *p) { return _mm256_loadu_si256(static_cast<const __m256i *>(p)); }
inline void st2(void *p, __m256i v) { _mm256_storeu_si256(static_cast<__m256i *>(p), v); }
inline __m256i wrapPack(__m256i a, __m256i b) // in-lane: [a.l0 b.l0 | a.l1 b.l1]
{
    return _mm256_packs_epi32(_mm256_srai_epi32(_mm256_slli_epi32(a, 16), 16), _mm256_srai_epi32(_mm256_slli_epi32(b, 16), 16));
}

// ---------------------------------------------------------------- unpack
void unpack14(const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *xq)
{
    const std::uint8_t *p = pkt + 16;
    unsigned k = 0;
    for (; k + 16 <= 252; k += 16) {
        const __m256i a = ld2(p + 4 * k), b = ld2(p + 4 * k + 32);
        const __m256i ia = _mm256_srai_epi32(_mm256_slli_epi32(a, 16), 16), ib = _mm256_srai_epi32(_mm256_slli_epi32(b, 16), 16);
        const __m256i qa = _mm256_srai_epi32(a, 16), qb = _mm256_srai_epi32(b, 16);
        st2(xi + k, _mm256_slli_epi16(_mm256_permute4x64_epi64(_mm256_packs_epi32(ia, ib), 0xD8), 2));
        st2(xq + k, _mm256_slli_epi16(_mm256_permute4x64_epi64(_mm256_packs_epi32(qa, qb), 0xD8), 2));
    }
    for (; k + 8 <= 252; k += 8) {
        const __m128i a = ld(p + 4 * k), b = ld(p + 4 * k + 16);
        const __m128i ia = _mm_srai_epi32(_mm_slli_epi32(a, 16), 16), ib = _mm_srai_epi32(_mm_slli_epi32(b, 16), 16);
        st(xi + k, _mm_slli_epi16(_mm_packs_epi32(ia, ib), 2));
        st(xq + k, _mm_slli_epi16(_mm_packs_epi32(_mm_srai_epi32(a, 16), _mm_srai_epi32(b, 16)), 2));
    }
    for (; k < 252; ++k) {
        xi[k] = r_wrap16(std::int32_t(r_rd16(p + 4 * k)) << 2);
        xq[k] = r_wrap16(std::int32_t(r_rd16(p + 4 * k + 2)) << 2);
    }
}

void unpack8(const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *xq)
{
    const std::uint8_t *p = pkt + 16;
    const __m256i hiMask = _mm256_set1_epi16(static_cast<short>(0xFF00));
    unsigned k = 0;
    for (; k + 16 <= 504; k += 16) {
        const __m256i a = ld2(p + 2 * k);
        st2(xi + k, _mm256_slli_epi16(a, 8));
        st2(xq + k, _mm256_and_si256(a, hiMask));
    }
    const __m128i a = ld(p + 2 * k); // 8 left
    st(xi + k, _mm_slli_epi16(a, 8));
    st(xq + k, _mm_and_si128(a, _mm256_castsi256_si128(hiMask)));
}

void unpackReal(const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *)
{
    const std::uint8_t *p = pkt + 16;
    unsigned k = 0;
    for (; k + 16 <= 504; k += 16) st2(xi + k, _mm256_slli_epi16(ld2(p + 2 * k), 2));
    st(xi + k, _mm_slli_epi16(ld(p + 2 * k), 2));
}

// 12-bit: 4 pairs per 12 bytes; I_k = word(3k) << 4, Q_k = word(3k+1) & 0xFFF0.
void unpack12(const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *xq)
{
    const std::uint8_t *p = pkt + 16;
    const __m256i shuf = _mm256_setr_epi8(0, 1, 3, 4, 6, 7, 9, 10, 1, 2, 4, 5, 7, 8, 10, 11, 0, 1, 3, 4, 6, 7, 9, 10, 1, 2, 4,
                                          5, 7, 8, 10, 11);
    const __m256i qMask = _mm256_set1_epi16(static_cast<short>(0xFFF0));
    unsigned g = 0;
    for (; g + 2 <= 82; g += 2) { // loads end at 12*(g+1)+16 <= 1008
        const __m256i x = _mm256_inserti128_si256(_mm256_castsi128_si256(ld(p + 12 * g)), ld(p + 12 * g + 12), 1);
        const __m256i w = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(x, shuf), 0xD8); // [I0-3 I4-7 | Q0-3 Q4-7]
        st(xi + 4 * g, _mm256_castsi256_si128(_mm256_slli_epi16(w, 4)));
        st(xq + 4 * g, _mm256_extracti128_si256(_mm256_and_si256(w, qMask), 1));
    }
    r_unpack12(pkt, xi, xq, 4 * g, 336);
}

// 10-bit: per group of 4 pairs (10 bytes): I words at bytes 0,2,5,7 (<< 6,2,6,2), Q words
// at bytes 1,3,6,8 (<< 4,0,4,0); & 0xFFC0; then >> sh (the group's exponent code).
void unpack10(const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *xq)
{
    const std::uint8_t *p = pkt + 16;
    const __m128i shuf = _mm_setr_epi8(0, 1, 2, 3, 5, 6, 7, 8, 1, 2, 3, 4, 6, 7, 8, 9);
    const __m128i mulv = _mm_setr_epi16(64, 4, 64, 4, 16, 1, 16, 1);
    const __m128i mask = _mm_set1_epi16(static_cast<short>(0xFFC0));
    for (unsigned b = 0; b < 6; ++b) {
        const std::uint8_t *blk = p + 164 * b;
        std::uint32_t codes = r_rd32(blk + 160);
        std::int16_t *oi = xi + 64 * b, *oq = xq + 64 * b;
        for (unsigned g = 0; g < 16; g += 2, codes >>= 4) {
            const unsigned c0 = codes & 3u, c1 = (codes >> 2) & 3u;
            const int s0 = c0 == 3 ? 0 : int(2 - c0), s1 = c1 == 3 ? 0 : int(2 - c1);
            // loads read up to blk + 10*15 + 15 = blk + 165: inside the 1024-byte packet
            const __m128i v0 = _mm_sra_epi16(_mm_and_si128(_mm_mullo_epi16(_mm_shuffle_epi8(ld(blk + 10 * g), shuf), mulv), mask),
                                             _mm_cvtsi32_si128(s0));
            const __m128i v1 = _mm_sra_epi16(_mm_and_si128(_mm_mullo_epi16(_mm_shuffle_epi8(ld(blk + 10 * g + 10), shuf), mulv), mask),
                                             _mm_cvtsi32_si128(s1));
            st(oi + 4 * g, _mm_unpacklo_epi64(v0, v1));
            st(oq + 4 * g, _mm_unpackhi_epi64(v0, v1));
        }
    }
}

// ---------------------------------------------------------------- IQ correction
// (x * coef) >> 15, low 16 bits: bits 15..30 of the 32-bit product = (hi << 1) | (lo >> 15).
inline __m256i scaleQ15v(__m256i v, __m256i c)
{
    return _mm256_or_si256(_mm256_slli_epi16(_mm256_mulhi_epi16(v, c), 1), _mm256_srli_epi16(_mm256_mullo_epi16(v, c), 15));
}
// acc (8 x u32) += 16-bit abs of v: -32768 -> 0x8000 (unsigned 32768)
inline __m256i addAbs16(__m256i acc, __m256i v)
{
    const __m256i a = _mm256_abs_epi16(v), z = _mm256_setzero_si256();
    return _mm256_add_epi32(acc, _mm256_add_epi32(_mm256_unpacklo_epi16(a, z), _mm256_unpackhi_epi16(a, z)));
}
inline std::uint32_t hsum32(__m256i v)
{
    alignas(32) std::uint32_t l[8];
    _mm256_store_si256(reinterpret_cast<__m256i *>(l), v);
    std::uint32_t s = 0;
    for (unsigned i = 0; i < 8; ++i) s += l[i];
    return s;
}

// One pass per 16 samples; the rotation as in KernelsSse2.cpp (pmaddwd on (I, Q) pairs).
template <int M0, int M1>
void iqBalanceT(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, const int *mode, const int *coef, std::uint32_t *sums)
{
    const __m256i c0 = _mm256_set1_epi16(static_cast<short>(coef[0])), c1 = _mm256_set1_epi16(static_cast<short>(coef[1]));
    const __m256i kd = _mm256_set1_epi32(int(std::uint32_t(23170) | (std::uint32_t(std::uint16_t(-23170)) << 16)));
    const __m256i ks = _mm256_set1_epi32(int(std::uint32_t(23170) | (std::uint32_t(23170) << 16)));
    __m256i s0 = _mm256_setzero_si256(), s1 = s0, s2 = s0, s3 = s0;
    std::uint32_t k = 0;
    for (; k + 16 <= n; k += 16) {
        __m256i a = ld2(xi + k), b = ld2(xq + k);
        if (M0 == 1) a = scaleQ15v(a, c0);
        if (M0 == 2) b = scaleQ15v(b, c0);
        s0 = addAbs16(s0, a);
        s1 = addAbs16(s1, b);
        const __m256i lo = _mm256_unpacklo_epi16(a, b), hi = _mm256_unpackhi_epi16(a, b);
        a = wrapPack(_mm256_srai_epi32(_mm256_madd_epi16(lo, kd), 15), _mm256_srai_epi32(_mm256_madd_epi16(hi, kd), 15));
        b = wrapPack(_mm256_srai_epi32(_mm256_madd_epi16(lo, ks), 15), _mm256_srai_epi32(_mm256_madd_epi16(hi, ks), 15));
        if (M1 == 1) a = scaleQ15v(a, c1);
        if (M1 == 2) b = scaleQ15v(b, c1);
        s2 = addAbs16(s2, a);
        s3 = addAbs16(s3, b);
        st2(xi + k, a);
        st2(xq + k, b);
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
    const __m256i z = _mm256_setzero_si256();
    __m256i acc = _mm256_setzero_si256();
    std::uint32_t k = 0;
    for (; k + 16 <= n; k += 16) {
        const __m256i a = ld2(xi + k);
        const __m256i sa = _mm256_madd_epi16(a, a); // <= 2^31 per lane, read as uint32
        acc = _mm256_add_epi64(acc, _mm256_add_epi64(_mm256_unpacklo_epi32(sa, z), _mm256_unpackhi_epi32(sa, z)));
        if (!iOnly) {
            const __m256i b = ld2(xq + k);
            const __m256i sb = _mm256_madd_epi16(b, b);
            acc = _mm256_add_epi64(acc, _mm256_add_epi64(_mm256_unpacklo_epi32(sb, z), _mm256_unpackhi_epi32(sb, z)));
        }
    }
    alignas(32) std::uint64_t l[4];
    _mm256_store_si256(reinterpret_cast<__m256i *>(l), acc);
    std::uint64_t s = l[0] + l[1] + l[2] + l[3];
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

// I and Q blocks together: the low 128-bit lane carries I and the high one Q, so every shuffle
// of dcBlock stays within a lane. Returns (dcI, dcI, dcQ, dcQ).
inline __m256d dcBlockIq(std::int16_t *pi, std::int16_t *pq, __m256d d, const r_DcCoef &K)
{
    const __m256d cc = _mm256_set1_pd(K.c), a1 = _mm256_set1_pd(K.a), a2 = _mm256_set1_pd(K.a2), z = _mm256_setzero_pd();
    const __m256i vi = _mm256_cvtepi16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i *>(pi)));
    const __m256i vq = _mm256_cvtepi16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i *>(pq)));
    const __m128i loI = _mm256_castsi256_si128(vi), hiI = _mm256_extracti128_si256(vi, 1);
    const __m128i loQ = _mm256_castsi256_si128(vq), hiQ = _mm256_extracti128_si256(vq, 1);
    __m256d r0 = _mm256_mul_pd(_mm256_cvtepi32_pd(_mm_unpacklo_epi64(loI, loQ)), cc);
    __m256d r1 = _mm256_mul_pd(_mm256_cvtepi32_pd(_mm_unpackhi_epi64(loI, loQ)), cc);
    __m256d r2 = _mm256_mul_pd(_mm256_cvtepi32_pd(_mm_unpacklo_epi64(hiI, hiQ)), cc);
    __m256d r3 = _mm256_mul_pd(_mm256_cvtepi32_pd(_mm_unpackhi_epi64(hiI, hiQ)), cc);
    const __m256d t0 = _mm256_unpacklo_pd(z, r0), t1 = _mm256_shuffle_pd(r0, r1, 5);
    const __m256d t2 = _mm256_unpacklo_pd(z, r2), t3 = _mm256_shuffle_pd(r2, r3, 5);
    r0 = _mm256_add_pd(r0, _mm256_mul_pd(a1, t0));
    r1 = _mm256_add_pd(r1, _mm256_mul_pd(a1, t1));
    r2 = _mm256_add_pd(r2, _mm256_mul_pd(a1, t2));
    r3 = _mm256_add_pd(r3, _mm256_mul_pd(a1, t3));
    r1 = _mm256_add_pd(r1, _mm256_mul_pd(a2, r0));
    r3 = _mm256_add_pd(r3, _mm256_mul_pd(a2, r2));
    const __m256d h3 = _mm256_unpackhi_pd(r1, r1);
    const __m256d p12 = _mm256_broadcast_pd(reinterpret_cast<const __m128d *>(K.p + 1));
    const __m256d p34 = _mm256_broadcast_pd(reinterpret_cast<const __m128d *>(K.p + 3));
    const __m256d p56 = _mm256_broadcast_pd(reinterpret_cast<const __m128d *>(K.p + 5));
    const __m256d p78 = _mm256_broadcast_pd(reinterpret_cast<const __m128d *>(K.p + 7));
    r2 = _mm256_add_pd(r2, _mm256_mul_pd(p12, h3));
    r3 = _mm256_add_pd(r3, _mm256_mul_pd(p34, h3));
    r0 = _mm256_add_pd(r0, _mm256_mul_pd(p12, d));
    r1 = _mm256_add_pd(r1, _mm256_mul_pd(p34, d));
    r2 = _mm256_add_pd(r2, _mm256_mul_pd(p56, d));
    r3 = _mm256_add_pd(r3, _mm256_mul_pd(p78, d));
    const __m128i c0 = _mm256_cvttpd_epi32(r0), c1 = _mm256_cvttpd_epi32(r1);
    const __m128i c2 = _mm256_cvttpd_epi32(r2), c3 = _mm256_cvttpd_epi32(r3);
    const __m128i oi = _mm_packs_epi32(_mm_sub_epi32(loI, _mm_unpacklo_epi64(c0, c1)), _mm_sub_epi32(hiI, _mm_unpacklo_epi64(c2, c3)));
    const __m128i oq = _mm_packs_epi32(_mm_sub_epi32(loQ, _mm_unpackhi_epi64(c0, c1)), _mm_sub_epi32(hiQ, _mm_unpackhi_epi64(c2, c3)));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(pi), oi);
    _mm_storeu_si128(reinterpret_cast<__m128i *>(pq), oq);
    return _mm256_unpackhi_pd(r3, r3);
}

void dcCorrect(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, double *dcI, double *dcQ, double c)
{
    if (!xq) {
        dcChannel(xi, n, *dcI, c);
        return;
    }
    const r_DcCoef K = r_dcCoef(c);
    const std::uint32_t nb = n & ~7u;
    __m256d d = _mm256_setr_pd(*dcI, *dcI, *dcQ, *dcQ);
    for (std::uint32_t k = 0; k < nb; k += 8) d = dcBlockIq(xi + k, xq + k, d, K);
    alignas(32) double out[4];
    _mm256_store_pd(out, d);
    *dcI = out[0];
    *dcQ = out[2];
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

const Kernels kAvx2 = {
    "avx2",
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
const Kernels *avx2Kernels()
{
#if defined(__GNUC__)
    // libgcc/compiler-rt check CPUID and that the OS saves the YMM state (XGETBV).
    static const bool ok = __builtin_cpu_supports("avx2");
#else
    static const bool ok = false; // MSVC: not wired (no __cpuid/_xgetbv probe yet)
#endif
    return ok ? &kAvx2 : nullptr;
}
} // namespace simd
} // namespace dsp
} // namespace sdrplay

#else
namespace sdrplay {
namespace dsp {
namespace simd {
const Kernels *avx2Kernels() { return nullptr; }
} // namespace simd
} // namespace dsp
} // namespace sdrplay
#endif
