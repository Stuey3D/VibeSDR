// vibe_dab_rs.h — Reed-Solomon RS(120,110) over GF(2^8), as DAB+ uses it.
//
// TS 102 563 clause 6.1, quoted: "Reed-Solomon RS(120, 110, t = 5) shortened code, derived from
// the original systematic RS(255, 245, t = 5) code … Code generator polynomial G(x) = ∏(i=0..9)
// (x + α^i), Galois Field GF(2^8) with α = 2 using polynomial P(x) = x^8 + x^4 + x^3 + x^2 + 1 …
// The shortened Reed-Solomon code may be implemented by adding 135 bytes, all set to zero."
//
// ★★★ THIS IS ONE OF THE TWO PLACES WE INTEND TO BEAT THE FIELD. DAB-Radio's own TODO says it
//     does not do DAB+ error correction; without it a marginal multiplex stutters instead of
//     staying audible, because a single corrupted byte kills a whole 110-byte block. Five
//     correctable bytes per 120 is the difference between a mux you can listen to at the edge of
//     coverage and one you cannot.
#pragma once

#include <cstdint>
#include <cstring>

namespace vibedab {

/** GF(2^8) with the DVB/DMB field polynomial — tables built once, at first use. */
class Gf256 {
public:
    static const Gf256& get() { static Gf256 g; return g; }
    uint8_t mul(uint8_t a, uint8_t b) const {
        return (a == 0 || b == 0) ? 0 : exp_[int(log_[a]) + int(log_[b])];
    }
    uint8_t inv(uint8_t a) const { return exp_[255 - log_[a]]; }
    uint8_t pow(int i) const { return exp_[((i % 255) + 255) % 255]; }
    uint8_t logOf(uint8_t a) const { return log_[a]; }
private:
    Gf256() {
        // P(x) = x^8 + x^4 + x^3 + x^2 + 1 = 0x11D, α = 2.
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            exp_[i] = uint8_t(x);
            log_[uint8_t(x)] = uint8_t(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; ++i) exp_[i] = exp_[i - 255];
        log_[0] = 0;
    }
    uint8_t exp_[512]{}, log_[256]{};
};

/** Decode one RS(120,110) codeword in place.
 *  @return bytes corrected (0..5), or -1 when the errors exceed the code's power.
 *
 *  ★ Syndromes → Berlekamp-Massey → Chien search → Forney. Written out rather than pulled from a
 *    library so the DAB+ path links nothing: the licence argument in BRIEF-dab.md is that
 *    VibeServer contains no codec, and dragging in a general FEC library for ten bytes of parity
 *    would be a poor trade for that.
 */
inline int rsDecode120(uint8_t* cw) {
    const Gf256& gf = Gf256::get();
    constexpr int N = 120, NROOTS = 10;

    /* ★★ THE SHORTENING IS ONLY A LABELLING QUESTION. The code is RS(255,245) with 135 leading
     *  zero bytes, so our cw[j] is the coefficient of x^(119-j) in the full codeword. Syndromes
     *  therefore come straight from Horner over the 120 bytes we have — the absent zeros
     *  contribute nothing — but the error LOCATOR roots are X = α^(119-j), and getting that
     *  exponent wrong is the classic way a shortened RS decoder fails silently.
     *  ★ My first attempt used α^(135+j) and corrected NOTHING; the round-trip test against a
     *    matching encoder caught it on the first run. A decoder that cannot fix a single-byte
     *    error looks exactly like one whose syndromes are wrong, which is why it is worth having
     *    an encoder to test against rather than only live signal. */
    uint8_t syn[NROOTS];
    bool any = false;
    for (int i = 0; i < NROOTS; ++i) {
        uint8_t s = 0;
        for (int j = 0; j < N; ++j) s = uint8_t(cw[j] ^ (s ? gf.mul(s, gf.pow(i)) : 0));
        syn[i] = s;
        if (s) any = true;
    }
    if (!any) return 0;                       // clean

    // Berlekamp-Massey.
    uint8_t lambda[NROOTS + 1] = {1}, b[NROOTS + 1] = {1}, t[NROOTS + 1];
    int L = 0, m = 1; uint8_t bb = 1;
    for (int n = 0; n < NROOTS; ++n) {
        uint8_t d = syn[n];
        for (int i = 1; i <= L; ++i) d ^= gf.mul(lambda[i], syn[n - i]);
        if (d == 0) { ++m; continue; }
        std::memcpy(t, lambda, sizeof t);
        const uint8_t scale = gf.mul(d, gf.inv(bb));
        for (int i = 0; i + m <= NROOTS; ++i) lambda[i + m] ^= gf.mul(scale, b[i]);
        if (2 * L <= n) { L = n + 1 - L; std::memcpy(b, t, sizeof b); bb = d; m = 1; }
        else ++m;
    }
    if (L == 0 || L > NROOTS / 2) return -1;  // more errors than the code can carry

    // Ω(x) = S(x)·Λ(x) mod x^NROOTS.
    uint8_t omega[NROOTS] = {};
    for (int i = 0; i < NROOTS; ++i)
        for (int j = 0; j <= L && j <= i; ++j) omega[i] ^= gf.mul(syn[i - j], lambda[j]);

    // Chien search + Forney, in one pass over the positions we actually hold.
    int nerr = 0;
    for (int j = 0; j < N; ++j) {
        const int xe    = (119 - j) % 255;              // X = α^xe
        const int xinvE = (255 - xe) % 255;             // X^-1
        // Λ(X^-1)
        uint8_t v = 1, xp = 1;
        for (int i = 1; i <= L; ++i) { xp = gf.mul(xp, gf.pow(xinvE)); v ^= gf.mul(lambda[i], xp); }
        if (v != 0) continue;
        if (++nerr > NROOTS / 2) return -1;
        // Ω(X^-1)
        uint8_t num = 0; xp = 1;
        for (int i = 0; i < NROOTS; ++i) { num ^= gf.mul(omega[i], xp); xp = gf.mul(xp, gf.pow(xinvE)); }
        // Λ'(X^-1) — in GF(2) only the odd-degree terms survive differentiation.
        uint8_t den = 0; xp = 1;                        // xp = (X^-1)^(i-1)
        for (int i = 1; i <= L; i += 2) {
            den ^= gf.mul(lambda[i], xp);
            xp = gf.mul(xp, gf.mul(gf.pow(xinvE), gf.pow(xinvE)));
        }
        if (den == 0) return -1;
        // b = 0, so the magnitude is X·Ω(X^-1)/Λ'(X^-1).
        cw[j] ^= gf.mul(gf.pow(xe), gf.mul(num, gf.inv(den)));
    }
    if (nerr != L) return -1;                 // roots do not account for the degree

    /* ★★★ VERIFY THE CORRECTION — DO NOT TRUST IT. With more than t errors, Berlekamp-Massey can
     *  still produce a locator polynomial with the right number of roots, and the decoder then
     *  "corrects" a word into something that is not what was sent. The test caught exactly that
     *  with 8 errors. Recomputing the syndromes over the corrected word costs one more pass and
     *  rejects every mis-correction except the vanishingly rare case that lands on a DIFFERENT
     *  valid codeword — which no decoder can detect, and which the AU CRC above us catches anyway.
     *  ★ A receiver that silently hands on wrong audio bytes is worse than one that drops a frame:
     *    a gap is a gap, but wrong bytes make a decoder produce noise. */
    for (int i = 0; i < NROOTS; ++i) {
        uint8_t s2 = 0;
        for (int j = 0; j < N; ++j) s2 = uint8_t(cw[j] ^ (s2 ? gf.mul(s2, gf.pow(i)) : 0));
        if (s2 != 0) return -1;
    }
    return nerr;
}

}  // namespace vibedab
