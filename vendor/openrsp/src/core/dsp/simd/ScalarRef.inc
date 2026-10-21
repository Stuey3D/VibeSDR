// Scalar helpers for the SIMD kernel files (tails, fallbacks). Included INSIDE an
// anonymous namespace of each src/core/dsp/simd/*.cpp so every copy has internal
// linkage and is compiled with that file's ISA flags only (no COMDAT leaks into
// baseline code). No #includes here. Same arithmetic as src/core/dsp/Kernels.cpp.

inline std::int16_t r_wrap16(std::int32_t v) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(static_cast<std::uint32_t>(v))); }
inline std::int32_t r_wrap32(std::int64_t v) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(v))); }
inline std::int16_t r_sat16(std::int32_t v) { return v > 32767 ? std::int16_t(32767) : v < -32768 ? std::int16_t(-32768) : static_cast<std::int16_t>(v); }
inline std::int32_t r_asr(std::int32_t v, unsigned s) { return v >> s; } // s < 32
inline std::int32_t r_q15Round(std::int64_t acc, unsigned shift) { return r_asr(r_wrap32(acc + 0x4000), shift); }
inline std::uint16_t r_rd16(const std::uint8_t *p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }
inline std::uint32_t r_rd32(const std::uint8_t *p)
{
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}

inline std::int32_t r_firOne(const std::int32_t *u, std::uint32_t j, std::uint32_t M, const std::int32_t *h,
                             std::uint32_t L, unsigned shift)
{
    const std::int32_t *p = u + std::size_t(M) * j;
    std::uint32_t acc = 0; // wrap-around sum == the int64 sum truncated (Z/2^32 is a ring)
    for (std::uint32_t i = 0; i < L; ++i)
        acc += static_cast<std::uint32_t>(h[i]) * static_cast<std::uint32_t>(p[-static_cast<std::ptrdiff_t>(i)]);
    return r_q15Round(acc, shift);
}

inline std::int32_t r_mixOne(std::int32_t tab, std::int16_t x) { return r_q15Round(std::int64_t(tab) * x, 15); }

// IQ imbalance correction (Kernels::iqBalance), one sample.
inline std::int16_t r_scaleQ15(std::int16_t x, std::int32_t coef) { return r_wrap16(r_asr(r_wrap32(std::int64_t(x) * coef), 15)); }
inline std::uint32_t r_abs16(std::int16_t v) { return static_cast<std::uint16_t>(v < 0 ? -v : v); } // |-32768| = 32768
inline void r_iqSample(std::int16_t &i, std::int16_t &q, const int mode[2], const int coef[2], std::uint32_t sums[4])
{
    if (mode[0] == 1) i = r_scaleQ15(i, coef[0]);
    else if (mode[0] == 2) q = r_scaleQ15(q, coef[0]);
    sums[0] += r_abs16(i);
    sums[1] += r_abs16(q);
    const std::int32_t a = i, b = q;
    i = r_wrap16(r_asr((a - b) * 23170, 15));
    q = r_wrap16(r_asr((a + b) * 23170, 15));
    if (mode[1] == 1) i = r_scaleQ15(i, coef[1]);
    else if (mode[1] == 2) q = r_scaleQ15(q, coef[1]);
    sums[2] += r_abs16(i);
    sums[3] += r_abs16(q);
}
inline void r_iqBalance(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, const int mode[2], const int coef[2],
                        std::uint32_t sums[4])
{
    for (std::uint32_t k = 0; k < n; ++k) r_iqSample(xi[k], xq[k], mode, coef, sums);
}

// DC offset correction (see dcChannel in Kernels.cpp): 8-sample blocks, then the per-sample
// recursion for the tail.
struct r_DcCoef {
    double c, a, a2;
    double p[9]; // p[k] = a^k
};
inline r_DcCoef r_dcCoef(double c)
{
    r_DcCoef k;
    k.c = c;
    k.a = 1.0 - c;
    k.p[0] = 1.0;
    for (int i = 1; i < 9; ++i) k.p[i] = k.p[i - 1] * k.a;
    k.a2 = k.p[2];
    return k;
}
inline void r_dcTail(std::int16_t *x, std::uint32_t n, double &dc, double c)
{
    double d = dc;
    for (std::uint32_t k = 0; k < n; ++k) {
        const std::int32_t v = x[k];
        const double diff = static_cast<double>(v) - d;
        const double step = diff * c;
        d = d + step;
        x[k] = r_sat16(v - static_cast<std::int32_t>(d));
    }
    dc = d;
}
inline void r_dcChannel(std::int16_t *x, std::uint32_t n, double &dc, double c)
{
    const r_DcCoef K = r_dcCoef(c);
    double d = dc;
    std::uint32_t k = 0;
    for (; k + 8 <= n; k += 8) {
        std::int32_t v[8];
        double h[8];
        for (int j = 0; j < 8; ++j) {
            v[j] = x[k + j];
            h[j] = static_cast<double>(v[j]) * K.c;
        }
        for (int r = 0; r < 8; r += 4) {
            const double s[4] = {0.0, h[r], h[r + 1], h[r + 2]}; // the half shifted by one lane
            for (int j = 0; j < 4; ++j) h[r + j] = h[r + j] + K.a * s[j];
            h[r + 2] = h[r + 2] + K.a2 * h[r];
            h[r + 3] = h[r + 3] + K.a2 * h[r + 1];
        }
        for (int j = 4; j < 8; ++j) h[j] = h[j] + K.p[j - 3] * h[3];
        for (int j = 0; j < 8; ++j) {
            const double dj = h[j] + K.p[j + 1] * d;
            x[k + j] = r_sat16(v[j] - static_cast<std::int32_t>(dj));
            h[j] = dj;
        }
        d = h[7];
    }
    dc = d;
    r_dcTail(x + k, n - k, dc, c);
}

// Packet formats with plain byte formulas (equal to unpackScalar's generic bit reader):
// 12-bit: pair k = bytes b0 b1 b2 at 3k: I = (int16)((b1:b0) << 4), Q = (int16)((b2:b1) & 0xFFF0).
inline void r_unpack12(const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *xq, unsigned from, unsigned to)
{
    const std::uint8_t *p = pkt + 16;
    for (unsigned k = from; k < to; ++k) {
        const std::uint8_t *q = p + 3 * k;
        xi[k] = static_cast<std::int16_t>(static_cast<std::uint16_t>(r_rd16(q) << 4));
        xq[k] = static_cast<std::int16_t>(static_cast<std::uint16_t>(r_rd16(q + 1) & 0xFFF0u));
    }
}
// 10-bit: per 164-byte block 128 values (4 per 5 bytes) + u32 codes; value m of a 5-byte
// group sits at byte m (m=0..3) bit 2m, so (int16)(v << 6) = (rd16(byte m) << (6 - 2m)) & 0xFFC0.
inline void r_unpack10(const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *xq)
{
    const std::uint8_t *p = pkt + 16;
    for (unsigned b = 0; b < 6; ++b) {
        const std::uint8_t *blk = p + 164 * b;
        std::uint32_t codes = r_rd32(blk + 160);
        for (unsigned g = 0; g < 16; ++g, codes >>= 2) {
            const unsigned c = codes & 3u;
            const unsigned sh = c == 3 ? 0u : 2u - c;
            const std::uint8_t *q = blk + 10 * g;
            for (unsigned k = 0; k < 4; ++k) {
                const unsigned vi = 2 * k, vq = 2 * k + 1; // value index within the 8-value group
                const std::uint8_t *pi = q + 5 * (vi >> 2) + (vi & 3), *pq = q + 5 * (vq >> 2) + (vq & 3);
                const std::int16_t a = static_cast<std::int16_t>(static_cast<std::uint16_t>((r_rd16(pi) << (6 - 2 * (vi & 3))) & 0xFFC0u));
                const std::int16_t bq = static_cast<std::int16_t>(static_cast<std::uint16_t>((r_rd16(pq) << (6 - 2 * (vq & 3))) & 0xFFC0u));
                xi[64 * b + 4 * g + k] = static_cast<std::int16_t>(a >> sh);
                xq[64 * b + 4 * g + k] = static_cast<std::int16_t>(bq >> sh);
            }
        }
    }
}
