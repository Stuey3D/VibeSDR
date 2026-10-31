// decoders/digimodes/ldpc_small.cpp — see ldpc_small.h.
#include "ldpc_small.h"
#include "vmath4.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace vibe {
namespace ldpc {

namespace {
constexpr float kPad = 40.f;          // the padding variable's message: tanh(20) rounds to 1, a product's identity
constexpr float kClampMsg = 40.f;
constexpr float kClampP = 0.99999f;   // |2·atanh| ≤ ≈ 12.2

struct Bits {                          // a small bitset (N ≤ 256)
    uint64_t w[4] = {0, 0, 0, 0};
    void set(int i) { w[i >> 6] |= 1ull << (i & 63); }
    bool get(int i) const { return (w[i >> 6] >> (i & 63)) & 1; }
    void flip(int i) { w[i >> 6] ^= 1ull << (i & 63); }
    void xorWith(const Bits& o) { for (int k = 0; k < 4; ++k) w[k] ^= o.w[k]; }
    int parityAnd(const Bits& o) const { uint64_t x = 0; for (int k = 0; k < 4; ++k) x ^= w[k] & o.w[k]; return __builtin_parityll(x); }
};
} // namespace

Code::Code(int n, int k, const std::vector<std::vector<int>>& checks) : n_(n), k_(k), m_((int)checks.size()), chk_(checks) {
    for (const auto& c : chk_) dmax_ = std::max(dmax_, (int)c.size());
    groups_ = (m_ + 3) / 4;
    evar_.assign((size_t)groups_ * dmax_ * 4, n_);
    vedge_.assign((size_t)n_, {});
    for (int r = 0; r < m_; ++r) {
        const int g = r / 4, l = r % 4;
        for (int s = 0; s < (int)chk_[(size_t)r].size(); ++s) {
            const int e = (g * dmax_ + s) * 4 + l, v = chk_[(size_t)r][(size_t)s];
            evar_[(size_t)e] = v;
            vedge_[(size_t)v].push_back(e);
        }
    }
    // ★ the systematic encoder: H = [A | B] (B on the last M columns), parity = B⁻¹·A·info
    std::vector<std::vector<uint8_t>> h((size_t)m_, std::vector<uint8_t>((size_t)n_, 0));
    for (int r = 0; r < m_; ++r) for (int v : chk_[(size_t)r]) h[(size_t)r][(size_t)v] ^= 1;
    bool ok = true;
    for (int c = 0; c < m_ && ok; ++c) {
        const int col = k_ + c;
        int p = -1;
        for (int r = c; r < m_; ++r) if (h[(size_t)r][(size_t)col]) { p = r; break; }
        if (p < 0) { ok = false; break; }
        std::swap(h[(size_t)c], h[(size_t)p]);
        for (int r = 0; r < m_; ++r)
            if (r != c && h[(size_t)r][(size_t)col])
                for (int j = 0; j < n_; ++j) h[(size_t)r][(size_t)j] ^= h[(size_t)c][(size_t)j];
    }
    if (ok) {
        enc_.assign((size_t)m_, std::vector<uint8_t>((size_t)k_, 0));
        for (int r = 0; r < m_; ++r) for (int i = 0; i < k_; ++i) enc_[(size_t)r][(size_t)i] = h[(size_t)r][(size_t)i];
    }
}

void Code::encode(const uint8_t* info, uint8_t* cw) const {
    for (int i = 0; i < k_; ++i) cw[i] = info[i] & 1;
    for (int r = 0; r < m_; ++r) {
        int x = 0;
        if (!enc_.empty()) for (int i = 0; i < k_; ++i) x ^= enc_[(size_t)r][(size_t)i] & cw[i];
        cw[k_ + r] = (uint8_t)x;
    }
}

int Code::unsatisfied(const uint8_t* cw) const {
    int bad = 0;
    for (const auto& c : chk_) { int x = 0; for (int v : c) x ^= cw[v]; bad += x; }
    return bad;
}
bool Code::checksOk(const uint8_t* cw) const { return unsatisfied(cw) == 0; }

int Code::bp(const float* llr, int maxIter, uint8_t* cw, int* unsatOut, int giveUp, float* post) const {
    using namespace q65v;
    const size_t E = evar_.size();
    thread_local std::vector<float> v2c, c2v, L, pre, suf;
    v2c.assign(E, kPad); c2v.assign(E, 0.f); L.resize((size_t)n_);
    pre.resize((size_t)dmax_ * 4 + 4); suf.resize((size_t)dmax_ * 4 + 4);
    for (int j = 0; j < n_; ++j) {
        const float x = -llr[j];                                           // ★ log P(0)/P(1) inside
        L[(size_t)j] = std::max(-kClampMsg, std::min(kClampMsg, x));
    }
    for (size_t e = 0; e < E; ++e) if (evar_[e] < n_) v2c[e] = L[(size_t)evar_[e]];
    if (post) for (int j = 0; j < n_; ++j) post[j] = 0.f;
    int best = m_ + 1, since = 0;
    const V one = set1(1.f), cmsg = set1(kClampMsg), cp = set1(kClampP), cpn = set1(-kClampP);
    for (int it = 1; it <= maxIter; ++it) {
        // ── check → variable, four checks a time ──
        for (int g = 0; g < groups_; ++g) {
            float* vb = &v2c[(size_t)g * dmax_ * 4];
            float* cb = &c2v[(size_t)g * dmax_ * 4];
            // t = tanh(x/2) = (1 − e^−|x|)/(1 + e^−|x|) with x's sign
            V acc = one;
            for (int s = 0; s < dmax_; ++s) {
                const V x = ld(vb + s * 4);
                const V a = vmin(vabs(x), cmsg);
                const V ex = expNeg(sub(set1(0.f), a));
                const V t = withSign(div(sub(one, ex), add(one, ex)), x);
                st(&pre[(size_t)s * 4], acc);
                acc = mul(acc, t);
                st(&suf[(size_t)s * 4], t);                  // t kept here for the suffix pass
            }
            V sacc = one;
            for (int s = dmax_ - 1; s >= 0; --s) {
                const V t = ld(&suf[(size_t)s * 4]);
                V p = mul(ld(&pre[(size_t)s * 4]), sacc);
                sacc = mul(sacc, t);
                p = vmax(cpn, vmin(cp, p));
                st(cb + s * 4, sub(vlog(add(one, p)), vlog(sub(one, p))));
            }
        }
        // ── variable → check, and the hard decision ──
        for (int j = 0; j < n_; ++j) {
            const std::vector<int>& ed = vedge_[(size_t)j];
            float tot = L[(size_t)j];
            for (int e : ed) tot = tot + c2v[(size_t)e];
            cw[j] = tot < 0.f ? 1 : 0;
            if (post) post[j] = post[j] - tot;                                  // ★ back to log P(1)/P(0)
            for (int e : ed) {
                const float m = tot - c2v[(size_t)e];
                v2c[(size_t)e] = std::max(-kClampMsg, std::min(kClampMsg, m));
            }
        }
        const int bad = unsatisfied(cw);
        if (bad == 0) { if (unsatOut) *unsatOut = 0; return it; }
        if (bad < best) { best = bad; since = 0; }
        else if (giveUp > 0 && ++since >= giveUp) { if (unsatOut) *unsatOut = bad; return 0; }
        if (unsatOut) *unsatOut = bad;
    }
    return 0;
}

bool Code::osd(const float* llr, int pairSpan, const std::function<bool(const uint8_t*)>& accept, uint8_t* cw,
               float* costOut, int* flipsOut, float maxCost, int maxTries) const {
    if (n_ > 256 || m_ > 256) return false;
    // reliability order, most reliable first (stable: deterministic on ties)
    std::vector<int> idx((size_t)n_);
    std::iota(idx.begin(), idx.end(), 0);
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return std::fabs(llr[a]) > std::fabs(llr[b]); });
    std::vector<int> posOf((size_t)n_);
    for (int p = 0; p < n_; ++p) posOf[(size_t)idx[(size_t)p]] = p;
    std::vector<Bits> rows((size_t)m_);
    for (int r = 0; r < m_; ++r) for (int v : chk_[(size_t)r]) rows[(size_t)r].flip(posOf[(size_t)v]);
    // eliminate from the least reliable end: the pivots are the parity set
    std::vector<int> piv;
    int rank = 0;
    for (int p = n_ - 1; p >= 0 && rank < m_; --p) {
        int f = -1;
        for (int r = rank; r < m_; ++r) if (rows[(size_t)r].get(p)) { f = r; break; }
        if (f < 0) continue;
        std::swap(rows[(size_t)rank], rows[(size_t)f]);
        for (int r = 0; r < m_; ++r) if (r != rank && rows[(size_t)r].get(p)) rows[(size_t)r].xorWith(rows[(size_t)rank]);
        piv.push_back(p);
        ++rank;
    }
    std::vector<uint8_t> isPiv((size_t)n_, 0);
    for (int p : piv) isPiv[(size_t)p] = 1;
    std::vector<float> w((size_t)n_);
    std::vector<uint8_t> hard((size_t)n_);
    Bits xinfo;
    for (int p = 0; p < n_; ++p) {
        const float l = llr[idx[(size_t)p]];
        w[(size_t)p] = std::fabs(l);
        hard[(size_t)p] = l > 0 ? 1 : 0;
        if (!isPiv[(size_t)p] && hard[(size_t)p]) xinfo.set(p);
    }
    // base parity, the rows that disagree with the hard decisions, and each info position's rows
    Bits D;
    float base = 0;
    for (int r = 0; r < rank; ++r) {
        const int par = rows[(size_t)r].parityAnd(xinfo);
        if (par != hard[(size_t)piv[(size_t)r]]) { D.set(r); base += w[(size_t)piv[(size_t)r]]; }
    }
    std::vector<int> info;                       // info positions, most reliable first
    for (int p = 0; p < n_; ++p) if (!isPiv[(size_t)p]) info.push_back(p);
    std::vector<Bits> colRows((size_t)n_);
    for (int r = 0; r < rank; ++r) for (int p : info) if (rows[(size_t)r].get(p)) colRows[(size_t)p].set(r);
    auto rowCost = [&](const Bits& d) {
        float c = 0;
        for (int k = 0; k < 4; ++k) {
            uint64_t x = d.w[k];
            while (x) { const int b = __builtin_ctzll(x); x &= x - 1; const int r = k * 64 + b; c += w[(size_t)piv[(size_t)r]]; }
        }
        return c;
    };
    struct Cand { float cost; int a, b; };       // flips of info positions a, b (−1 = none)
    std::vector<Cand> cands;
    cands.push_back({base, -1, -1});
    for (int p : info) { Bits d = D; d.xorWith(colRows[(size_t)p]); cands.push_back({w[(size_t)p] + rowCost(d), p, -1}); }
    const int span = std::min(pairSpan, (int)info.size());
    for (int i = 0; i < span; ++i)
        for (int j = i + 1; j < span; ++j) {
            const int a = info[info.size() - 1 - (size_t)i], b = info[info.size() - 1 - (size_t)j];   // the least reliable
            Bits d = D; d.xorWith(colRows[(size_t)a]); d.xorWith(colRows[(size_t)b]);
            cands.push_back({w[(size_t)a] + w[(size_t)b] + rowCost(d), a, b});
        }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.cost < y.cost; });
    std::vector<uint8_t> xp((size_t)n_), out((size_t)n_);
    int tried = 0;
    for (const Cand& c : cands) {
        if (maxCost > 0 && c.cost > maxCost) break;
        if (++tried > maxTries) break;
        Bits xi = xinfo;
        if (c.a >= 0) xi.flip(c.a);
        if (c.b >= 0) xi.flip(c.b);
        for (int p = 0; p < n_; ++p) xp[(size_t)p] = xi.get(p) ? 1 : 0;
        for (int r = 0; r < rank; ++r) xp[(size_t)piv[(size_t)r]] = (uint8_t)rows[(size_t)r].parityAnd(xi);
        for (int p = 0; p < n_; ++p) out[(size_t)idx[(size_t)p]] = xp[(size_t)p];
        if (!checksOk(out.data())) continue;     // (rank-deficient H only)
        if (!accept(out.data())) continue;
        std::memcpy(cw, out.data(), (size_t)n_);
        if (costOut) *costOut = c.cost;
        if (flipsOut) { int f = 0; for (int p = 0; p < n_; ++p) f += xp[(size_t)p] != hard[(size_t)p]; *flipsOut = f; }
        return true;
    }
    return false;
}

} // namespace ldpc
} // namespace vibe
