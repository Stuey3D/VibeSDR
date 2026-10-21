// Width-generic int32 kernels for the SIMD tables. Included INSIDE an anonymous
// namespace of each ISA file after ScalarRef.inc and after the ISA's `Ops` struct:
//   using V; static constexpr unsigned W;           // lanes of int32
//   V load(const int32_t*); void store(int32_t*, V); V set1(int32_t); V zero();
//   V add(V,V); V sub(V,V); V mul(V,V) (low 32 bits); V band(V,V); V bor(V,V); V sra(V, unsigned)
//   bool allZero(V)
//   kMul16: true if mul16 is cheaper than mul; then V coef16(int32 c) (c fits int16),
//   V mul16(V v, V coef16(c)) == v*c for v, c in int16 range, V not16(V) (nonzero lanes
//   where the int32 is outside int16 range)
//   V widen16(const int16_t*)   W int16 -> W int32 (sign extend)
//   void narrow16(int16_t*, V)  W int32 -> W int16 (wrap, i.e. low 16 bits)
//   void deint2(const int32_t *src, int32_t *ev, int32_t *od)  2W inputs -> W even, W odd
// All arithmetic is mod 2^32, so the results equal the scalar reference exactly for any
// evaluation order (Kernels.h).

// ---------------------------------------------------------------- firDecimate
// Polyphase form: with i = M*a - r (0 <= r < M, a = ceil(i/M)), u[M*j - i] = ph_r[j - a]
// where ph_r[k] = u[M*k + r]. Each tap is then a contiguous load over j. Symmetric tap
// pairs (h[i] == h[L-1-i]) are folded: h*(x_a + x_b) == h*x_a + h*x_b mod 2^32.
// Zero taps are skipped (they add 0).
struct FirTerm {
    std::int32_t c;
    std::uint32_t ra, aa, rb, ab;
    bool pair;
};

// y[j0 .. j0+cnt) (cnt a multiple of W) from per-term base pointers: output j reads
// pp[t][j] (+ pq[t][j]) for the pair terms and ps[t][j] for the single terms.
template <class O>
inline void firRun(std::uint32_t cnt, const std::int32_t *const *pp, const std::int32_t *const *pq,
                   const typename O::V *cp, std::uint32_t np, const std::int32_t *const *ps, const typename O::V *cs,
                   std::uint32_t ns, unsigned shift, std::int32_t *y)
{
    using V = typename O::V;
    constexpr unsigned W = O::W;
    const V rnd = O::set1(0x4000);
    std::uint32_t j = 0;
    for (; j + 2 * W <= cnt; j += 2 * W) { // two independent accumulators
        V a0 = O::zero(), a1 = O::zero();
        for (std::uint32_t t = 0; t < np; ++t) {
            const std::int32_t *a = pp[t] + j, *b = pq[t] + j;
            a0 = O::add(a0, O::mul(O::add(O::load(a), O::load(b)), cp[t]));
            a1 = O::add(a1, O::mul(O::add(O::load(a + W), O::load(b + W)), cp[t]));
        }
        for (std::uint32_t t = 0; t < ns; ++t) {
            const std::int32_t *a = ps[t] + j;
            a0 = O::add(a0, O::mul(O::load(a), cs[t]));
            a1 = O::add(a1, O::mul(O::load(a + W), cs[t]));
        }
        O::store(y + j, O::sra(O::add(a0, rnd), shift));
        O::store(y + j + W, O::sra(O::add(a1, rnd), shift));
    }
    for (; j < cnt; j += W) {
        V a0 = O::zero();
        for (std::uint32_t t = 0; t < np; ++t)
            a0 = O::add(a0, O::mul(O::add(O::load(pp[t] + j), O::load(pq[t] + j)), cp[t]));
        for (std::uint32_t t = 0; t < ns; ++t) a0 = O::add(a0, O::mul(O::load(ps[t] + j), cs[t]));
        O::store(y + j, O::sra(O::add(a0, rnd), shift));
    }
}

// Same as firRun for ISAs with a cheap exact 16x16->32 multiply (O::kMul16): every input
// and tap fits int16 (checked by the caller), so O::mul16(v, c) = v*c exactly; pair terms
// are two products (their sum may exceed 16 bits).
template <class O>
inline void firRun16(std::uint32_t cnt, const std::int32_t *const *pp, const std::int32_t *const *pq,
                     const typename O::V *cp, std::uint32_t np, const std::int32_t *const *ps, const typename O::V *cs,
                     std::uint32_t ns, unsigned shift, std::int32_t *y)
{
    using V = typename O::V;
    constexpr unsigned W = O::W;
    const V rnd = O::set1(0x4000);
    std::uint32_t j = 0;
    for (; j + 2 * W <= cnt; j += 2 * W) {
        V a0 = O::zero(), a1 = O::zero();
        for (std::uint32_t t = 0; t < np; ++t) {
            const std::int32_t *a = pp[t] + j, *b = pq[t] + j;
            a0 = O::add(a0, O::add(O::mul16(O::load(a), cp[t]), O::mul16(O::load(b), cp[t])));
            a1 = O::add(a1, O::add(O::mul16(O::load(a + W), cp[t]), O::mul16(O::load(b + W), cp[t])));
        }
        for (std::uint32_t t = 0; t < ns; ++t) {
            const std::int32_t *a = ps[t] + j;
            a0 = O::add(a0, O::mul16(O::load(a), cs[t]));
            a1 = O::add(a1, O::mul16(O::load(a + W), cs[t]));
        }
        O::store(y + j, O::sra(O::add(a0, rnd), shift));
        O::store(y + j + W, O::sra(O::add(a1, rnd), shift));
    }
    for (; j < cnt; j += W) {
        V a0 = O::zero();
        for (std::uint32_t t = 0; t < np; ++t)
            a0 = O::add(a0, O::add(O::mul16(O::load(pp[t] + j), cp[t]), O::mul16(O::load(pq[t] + j), cp[t])));
        for (std::uint32_t t = 0; t < ns; ++t) a0 = O::add(a0, O::mul16(O::load(ps[t] + j), cs[t]));
        O::store(y + j, O::sra(O::add(a0, rnd), shift));
    }
}

// true if every int32 in p[0..n) is in int16 range
template <class O>
inline bool fits16(const std::int32_t *p, std::uint32_t n)
{
    using V = typename O::V;
    constexpr unsigned W = O::W;
    V bad = O::zero();
    std::uint32_t k = 0;
    for (; k + W <= n; k += W) bad = O::bor(bad, O::not16(O::load(p + k)));
    for (; k < n; ++k)
        if (p[k] < -32768 || p[k] > 32767) return false;
    return O::allZero(bad);
}

template <class O>
void firDecimateT(const std::int32_t *u, std::uint32_t nOut, std::uint32_t M, const std::int32_t *h, std::uint32_t L,
                  unsigned shift, std::int32_t *y)
{
    using V = typename O::V;
    constexpr unsigned W = O::W;
    if (M < 1 || M > 4 || L < 1 || L > 128 || nOut < W) {
        for (std::uint32_t j = 0; j < nOut; ++j) y[j] = r_firOne(u, j, M, h, L, shift);
        return;
    }
    // Terms: symmetric pairs folded, zero taps dropped. i = M*a - r (0 <= r < M) so that
    // u[M*j - i] = ph_r[j - a] with ph_r[k] = u[M*k + r].
    FirTerm terms[128];
    std::uint32_t nt = 0;
    const auto phase = [M](std::uint32_t i, std::uint32_t &r, std::uint32_t &a) {
        r = (M - i % M) % M;
        a = (i + r) / M;
    };
    for (std::uint32_t i = 0; i < L; ++i) {
        const std::uint32_t i2 = L - 1 - i;
        if (i2 < i) break;
        if (i2 != i && h[i] == h[i2]) {
            if (h[i] == 0) continue;
            FirTerm t{};
            t.c = h[i];
            t.pair = true;
            phase(i, t.ra, t.aa);
            phase(i2, t.rb, t.ab);
            terms[nt++] = t;
            continue;
        }
        const std::uint32_t ks[2] = {i, i2};
        for (std::uint32_t q = 0; q < (i2 == i ? 1u : 2u); ++q) { // the centre tap once
            if (h[ks[q]] == 0) continue;
            FirTerm t{};
            t.c = h[ks[q]];
            phase(ks[q], t.ra, t.aa);
            terms[nt++] = t;
        }
    }
    const std::uint32_t nVec = nOut - nOut % W;
    const std::int32_t *pp[128], *pq[128], *ps[128];
    V cp[128], cs[128];
    bool taps16 = O::kMul16;
    for (std::uint32_t t = 0; t < nt; ++t) taps16 = taps16 && terms[t].c >= -32768 && terms[t].c <= 32767;
    const auto coefV = [](std::int32_t c, bool m16) { return m16 ? O::coef16(c) : O::set1(c); };

    if (M == 1) { // ph_0 = u itself (u[-(L-1)] .. u[nOut-1] are readable)
        const bool m16 = taps16 && fits16<O>(u - std::ptrdiff_t(L - 1), nVec + L - 1);
        std::uint32_t np = 0, ns = 0;
        for (std::uint32_t t = 0; t < nt; ++t) {
            const FirTerm &tm = terms[t];
            if (tm.pair) {
                pp[np] = u - std::ptrdiff_t(tm.aa);
                pq[np] = u - std::ptrdiff_t(tm.ab);
                cp[np++] = coefV(tm.c, m16);
            } else {
                ps[ns] = u - std::ptrdiff_t(tm.aa);
                cs[ns++] = coefV(tm.c, m16);
            }
        }
        if (m16) firRun16<O>(nVec, pp, pq, cp, np, ps, cs, ns, shift, y);
        else firRun<O>(nVec, pp, pq, cp, np, ps, cs, ns, shift, y);
    } else {
        constexpr std::uint32_t CH = 512;
        const std::uint32_t A = (L - 1 + M - 1) / M; // max a
        alignas(64) std::int32_t ph[4][CH + 72];     // A <= ceil(127/2) for M >= 2
        const std::int64_t lo = -std::int64_t(L - 1), hi = std::int64_t(M) * (nOut - 1);
        for (std::uint32_t j0 = 0; j0 < nVec; j0 += CH) {
            const std::uint32_t c = (nVec - j0 < CH) ? nVec - j0 : CH;
            // ph_r[k] = u[M*(j0 - A + k) + r] for k < c + A; 0 outside [lo, hi] (never
            // read by a valid output). Interior k (all phases in range) without checks.
            const std::int64_t base = std::int64_t(M) * (std::int64_t(j0) - A);
            const std::uint32_t kEnd = c + A;
            std::uint32_t kA = 0, kB = kEnd;
            while (kA < kEnd && base + std::int64_t(M) * kA < lo) ++kA;
            while (kB > kA && base + std::int64_t(M) * (kB - 1) + (M - 1) > hi) --kB;
            for (std::uint32_t r = 0; r < M; ++r) {
                for (std::uint32_t k = 0; k < kA; ++k) {
                    const std::int64_t idx = base + std::int64_t(M) * k + r;
                    ph[r][k] = (idx >= lo && idx <= hi) ? u[idx] : 0;
                }
                for (std::uint32_t k = kB; k < kEnd; ++k) {
                    const std::int64_t idx = base + std::int64_t(M) * k + r;
                    ph[r][k] = (idx >= lo && idx <= hi) ? u[idx] : 0;
                }
            }
            {
                const std::int32_t *src = u + (base + std::int64_t(M) * kA);
                std::uint32_t k = kA;
                if (M == 2)
                    for (; k + W <= kB; k += W, src += 2 * W) O::deint2(src, ph[0] + k, ph[1] + k);
                for (; k < kB; ++k, src += M)
                    for (std::uint32_t r = 0; r < M; ++r) ph[r][k] = src[r];
            }
            // A phase that is all zero in this chunk (e.g. the odd samples of an fs/4-mixed
            // branch) contributes h*0 = 0: its loads/multiplies are dropped (exact).
            bool zero[4] = {false, false, false, false};
            for (std::uint32_t r = 0; r < M; ++r) {
                V o = O::zero();
                std::uint32_t k = 0;
                for (; k + W <= kEnd; k += W) o = O::bor(o, O::load(ph[r] + k));
                std::int32_t tail = 0;
                for (; k < kEnd; ++k) tail |= ph[r][k];
                zero[r] = tail == 0 && O::allZero(o);
            }
            bool m16 = taps16;
            for (std::uint32_t r = 0; r < M && m16; ++r) m16 = fits16<O>(ph[r], kEnd);
            std::uint32_t np = 0, ns = 0;
            for (std::uint32_t t = 0; t < nt; ++t) {
                const FirTerm &tm = terms[t];
                const bool za = zero[tm.ra], zb = tm.pair && zero[tm.rb];
                const std::int32_t *pa = ph[tm.ra] + A - tm.aa;
                if (tm.pair && !za && !zb) {
                    pp[np] = pa;
                    pq[np] = ph[tm.rb] + A - tm.ab;
                    cp[np++] = coefV(tm.c, m16);
                } else if (!za) {
                    ps[ns] = pa;
                    cs[ns++] = coefV(tm.c, m16);
                } else if (tm.pair && !zb) {
                    ps[ns] = ph[tm.rb] + A - tm.ab;
                    cs[ns++] = coefV(tm.c, m16);
                }
            }
            if (m16) firRun16<O>(c, pp, pq, cp, np, ps, cs, ns, shift, y + j0);
            else firRun<O>(c, pp, pq, cp, np, ps, cs, ns, shift, y + j0);
        }
    }
    for (std::uint32_t j = nVec; j < nOut; ++j) y[j] = r_firOne(u, j, M, h, L, shift);
}

// ---------------------------------------------------------------- mixers / narrowing
template <class O>
std::uint32_t mixTableT(const std::int16_t *x, std::uint32_t n, const std::int32_t *tab, std::uint32_t period,
                        std::uint32_t phase, std::int32_t *out)
{
    using V = typename O::V;
    constexpr unsigned W = O::W;
    const V rnd = O::set1(0x4000);
    std::uint32_t k = 0;
    while (k < n) {
        std::uint32_t span = period - phase;
        if (span > n - k) span = n - k;
        const std::uint32_t vec = span - span % W;
        std::uint32_t s = 0;
        for (; s < vec; s += W)
            O::store(out + k + s, O::sra(O::add(O::mul(O::load(tab + phase + s), O::widen16(x + k + s)), rnd), 15));
        for (; s < span; ++s) out[k + s] = r_mixOne(tab[phase + s], x[k + s]);
        k += span;
        phase += span;
        if (phase >= period) phase = 0;
    }
    return phase;
}

template <class O>
void mixFs4T(const std::int16_t *x, std::uint32_t n, std::int32_t *mi, std::int32_t *mq)
{
    using V = typename O::V;
    constexpr unsigned W = O::W; // a multiple of 4: lane l has k & 3 == l & 3
    alignas(64) std::int32_t m[4][W];
    for (unsigned l = 0; l < W; ++l)
        for (unsigned s = 0; s < 4; ++s) m[s][l] = (l & 3u) == s ? -1 : 0;
    const V m0 = O::load(m[0]), m1 = O::load(m[1]), m2 = O::load(m[2]), m3 = O::load(m[3]);
    const std::uint32_t vec = n - n % W;
    for (std::uint32_t k = 0; k < vec; k += W) {
        const V w = O::widen16(x + k);
        O::store(mi + k, O::sub(O::band(w, m0), O::band(w, m2)));
        O::store(mq + k, O::sub(O::band(w, m1), O::band(w, m3)));
    }
    for (std::uint32_t k = vec; k < n; ++k) {
        const std::int32_t v = x[k];
        switch (k & 3u) {
        case 0: mi[k] = v; mq[k] = 0; break;
        case 1: mi[k] = 0; mq[k] = v; break;
        case 2: mi[k] = -v; mq[k] = 0; break;
        default: mi[k] = 0; mq[k] = -v; break;
        }
    }
}

template <class O>
void narrow16T(const std::int32_t *src, std::uint32_t n, std::int16_t *dst)
{
    constexpr unsigned W = O::W;
    const std::uint32_t vec = n - n % W;
    for (std::uint32_t k = 0; k < vec; k += W) O::narrow16(dst + k, O::load(src + k));
    for (std::uint32_t k = vec; k < n; ++k) dst[k] = r_wrap16(src[k]);
}
