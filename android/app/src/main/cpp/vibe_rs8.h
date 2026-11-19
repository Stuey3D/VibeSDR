// vibe_rs8.h — V12: a general Reed-Solomon codec over GF(2^8) for the packet FEC layers (FX.25 and IL2P).
// docs/v12/AX25-9600-FX25-IL2P.md.
//
// ★★★ WRITTEN FROM THE TEXTBOOK, our own code: systematic encoding by polynomial division; decoding by syndromes,
//     Berlekamp–Massey for the error locator, a Chien search for its roots and Forney's formula for the values
//     (Lin & Costello, "Error Control Coding", ch. 7; Blahut, "Algebraic Codes for Data Transmission", ch. 7).
//     The parameters are the published ones:
//       FX.25 (Stensat/TAPR draft 0.01.06 names the codes RS(255,239/223/191) and their shortened forms):
//             field x^8+x^4+x^3+x^2+1 (0x11D), α = 2, first consecutive root α^1.
//       IL2P  (Carrillo, IL2P draft v0.6): "The Galois Field is defined by reducing polynomial x^8+x^4+x^3+x^2+1 …
//             The RS encoder uses zero as its first root."
//     The first-root value for FX.25 is not in the FX.25 draft; it is the value every FX.25 transmitter on the air uses
//     (checked against a real off-air FX.25 code block in test-ax25hs: the block decodes, with its errors fixed, only
//     with first root 1).
//
// ★★ BYTE ORDER: block[0] is the coefficient of the HIGHEST power (the first byte on the air), the check bytes follow
//    the data. A shortened code (n < 255) is the full code with leading zeros that are never sent — nothing to do but
//    to treat block[0] as degree n−1.
// ★★ A DECODE IS BELIEVED ONLY WHEN IT IS SELF-CONSISTENT: the locator's degree must equal the roots found inside the
//    block (a root in the shortened, never-sent part is a failure), and the corrected block must have zero syndromes.
//    `maxFix` can lower the number of corrections accepted below ⌊nroots/2⌋ — a decode at the very limit of the code
//    is the one most likely to be a miscorrection (IL2P v0.6 "Optional Trailing CRC" says the same).
// ★ Per frame, never per sample: the cost is a few microseconds a code block, so it is scalar on purpose (the SIMD
//   rule applies to hot loops; this one runs once per correlation-tag or sync match).
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace vibers {

/** GF(2^8) with polynomial 0x11D and α = 2 — the field both FX.25 and IL2P use. */
class Gf {
public:
    static const Gf& get() { static Gf g; return g; }
    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp_[log_[a] + log_[b]] : 0; }
    uint8_t div(uint8_t a, uint8_t b) const { return a ? exp_[log_[a] + 255 - log_[b]] : 0; }   // b ≠ 0
    uint8_t pw(int e) const { e %= 255; if (e < 0) e += 255; return exp_[e]; }               // α^e
    int lg(uint8_t a) const { return log_[a]; }                                              // a ≠ 0
private:
    Gf() {
        int x = 1;
        for (int i = 0; i < 255; ++i) { exp_[i] = uint8_t(x); log_[x] = i; x <<= 1; if (x & 0x100) x ^= 0x11D; }
        for (int i = 255; i < 512; ++i) exp_[i] = exp_[i - 255];
        log_[0] = 0;
    }
    uint8_t exp_[512];
    int log_[256];
};

class Rs {
public:
    /** `nroots` check bytes; generator roots α^fcr … α^(fcr+nroots−1). */
    Rs(int nroots, int fcr) : nr_(nroots), fcr_(fcr) {
        const Gf& g = Gf::get();
        // g(x) = Π (x − α^(fcr+i)); gen_[0] is the highest power (monic, 1), gen_[nr] the constant
        gen_.assign(size_t(nr_ + 1), 0); gen_[0] = 1;
        for (int i = 0; i < nr_; ++i) {
            const uint8_t r = g.pw(fcr_ + i);
            for (int j = i + 1; j >= 1; --j) gen_[size_t(j)] = uint8_t(gen_[size_t(j)] ^ g.mul(gen_[size_t(j - 1)], r));
        }
    }
    int nroots() const { return nr_; }

    /** `k` data bytes → `nroots` check bytes (written to `parity`). k + nroots ≤ 255. */
    void encode(const uint8_t* data, int k, uint8_t* parity) const {
        const Gf& g = Gf::get();
        std::vector<uint8_t> r(size_t(nr_), 0);
        for (int i = 0; i < k; ++i) {
            const uint8_t fb = uint8_t(data[i] ^ r[0]);
            for (int j = 0; j + 1 < nr_; ++j) r[size_t(j)] = uint8_t(r[size_t(j + 1)] ^ g.mul(fb, gen_[size_t(j + 1)]));
            r[size_t(nr_ - 1)] = g.mul(fb, gen_[size_t(nr_)]);
        }
        std::memcpy(parity, r.data(), size_t(nr_));
    }

    /** Decode `n` bytes (data then check bytes) in place. Returns the number of bytes corrected (0…), or −1 when the
     *  block cannot be decoded (or would need more than `maxFix` corrections; −1 = the code's own limit). */
    int decode(uint8_t* block, int n, int maxFix = -1, int* where = nullptr) const {
        if (n <= nr_ || n > 255) return -1;
        const Gf& g = Gf::get();
        uint8_t S[256];
        if (!syndromes_(block, n, S)) return 0;
        // ── Berlekamp–Massey: Λ(x) = 1 + Λ1·x + … (index = power) ──
        uint8_t L[256] = { 1 }, B[256] = { 1 }, T[256];
        int Ldeg = 0, m = 1; uint8_t b = 1;
        for (int k = 0; k < nr_; ++k) {
            uint8_t d = S[k];
            for (int i = 1; i <= Ldeg; ++i) d ^= g.mul(L[i], S[k - i]);
            if (d == 0) { ++m; continue; }
            const uint8_t coef = g.div(d, b);
            if (2 * Ldeg <= k) {
                std::memcpy(T, L, sizeof T);
                for (int i = 0; i + m < 256; ++i) L[i + m] ^= g.mul(coef, B[i]);
                std::memcpy(B, T, sizeof B);
                Ldeg = k + 1 - Ldeg; b = d; m = 1;
            } else {
                for (int i = 0; i + m < 256; ++i) L[i + m] ^= g.mul(coef, B[i]);
                ++m;
            }
        }
        const int t = maxFix >= 0 ? (maxFix < nr_ / 2 ? maxFix : nr_ / 2) : nr_ / 2;
        if (Ldeg == 0 || Ldeg > t) return -1;
        while (Ldeg > 0 && L[Ldeg] == 0) --Ldeg;   // (cannot happen for a consistent locator, but keep the degree honest)
        // ── Ω(x) = S(x)·Λ(x) mod x^nroots ──
        uint8_t Om[256] = { 0 };
        for (int i = 0; i < nr_; ++i) { uint8_t v = 0; for (int j = 0; j <= Ldeg && j <= i; ++j) v ^= g.mul(L[j], S[i - j]); Om[i] = v; }
        // ── Chien search over the positions that exist (degree p = 0 … n−1), Forney for each value ──
        int found = 0; int pos[128]; uint8_t val[128];
        for (int p = 0; p < n && found <= Ldeg; ++p) {
            const uint8_t xinv = g.pw(-p);   // X⁻¹ = α^(−p)
            uint8_t s = 0, xp = 1;
            for (int i = 0; i <= Ldeg; ++i) { s ^= g.mul(L[i], xp); xp = g.mul(xp, xinv); }
            if (s) continue;
            // Ω(X⁻¹), Λ'(X⁻¹) (odd terms only, characteristic 2)
            uint8_t om = 0, dl = 0; xp = 1;
            for (int i = 0; i < nr_; ++i) { om ^= g.mul(Om[i], xp); xp = g.mul(xp, xinv); }
            const uint8_t x2 = g.mul(xinv, xinv); xp = 1;
            for (int i = 1; i <= Ldeg; i += 2) { dl ^= g.mul(L[i], xp); xp = g.mul(xp, x2); }
            if (!dl || found >= 128) return -1;
            // e = X^(1−fcr) · Ω(X⁻¹) / Λ'(X⁻¹)
            const uint8_t e = g.mul(g.pw(p * (1 - fcr_)), g.div(om, dl));
            pos[found] = n - 1 - p; val[found] = e; ++found;
        }
        if (found != Ldeg) return -1;            // roots outside the block, or a locator with repeated roots
        uint8_t save[128];
        for (int i = 0; i < found; ++i) { save[i] = block[pos[i]]; block[pos[i]] ^= val[i]; }
        if (syndromes_(block, n, S)) { for (int i = 0; i < found; ++i) block[pos[i]] = save[i]; return -1; }
        if (where) for (int i = 0; i < found; ++i) where[i] = pos[i];
        return found;
    }

private:
    /** True when any syndrome is non-zero. S_j = r(α^(fcr+j)), Horner from block[0] (the highest power). */
    bool syndromes_(const uint8_t* block, int n, uint8_t* S) const {
        const Gf& g = Gf::get();
        bool any = false;
        for (int j = 0; j < nr_; ++j) {
            const uint8_t a = g.pw(fcr_ + j);
            uint8_t s = 0;
            for (int i = 0; i < n; ++i) s = uint8_t(g.mul(s, a) ^ block[i]);
            S[j] = s; any |= s != 0;
        }
        return any;
    }
    int nr_, fcr_;
    std::vector<uint8_t> gen_;
};

} // namespace vibers
