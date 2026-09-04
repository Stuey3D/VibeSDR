// vibe_dab_prs.h — the phase reference symbol (EN 300 401 clause 14.3.2), Mode I.
//
// ★★★ WHAT IT IS FOR. The symbol immediately after the null is a KNOWN sequence — the only known
//     thing a DAB transmitter sends. Everything you cannot get from the cyclic prefix comes from
//     correlating against it:
//       · the INTEGER frequency offset (the CP gives only the fraction of a carrier spacing);
//       · confirmation that the null we found really was a null and not a fade;
//       · a fine timing reference, and the channel impulse response — the SFN echo profile that
//         BRIEF-dab.md wants for the DX panel, which falls out of the same correlation for free.
//
// ★★ THE TABLES ARE MACHINE-EXTRACTED FROM THE SPEC PDF, not typed. 48 groups of (k', i, n) plus
//    a 4x32 h table is exactly the kind of transcription that goes wrong silently — one wrong
//    digit shifts one group of 32 carriers and the correlation peak flattens, which reads as "poor
//    signal" rather than "wrong table". The generator script parsed table 23's number stream and
//    checked every group spans 32 carriers and that all 48 tile -768..768 without DC.
//
// z(1,k) = exp( j * (pi/2) * (h[i][k - k'] + n) )
#pragma once

#include <cmath>
#include <complex>
#include <cstdint>

namespace vibedab {

struct PrsGroup { int kStart; int i; int n; };

// GENERATED from ETSI EN 300 401 V2.1.1 tables 23 and 24 by machine extraction from the
// PDF — not hand-typed. 48 groups x 32 carriers = 1536.
inline constexpr PrsGroup kPrsGroupsI[48] = {
    {  -768, 0, 1 },   // k = -768 .. -737
    {  -736, 1, 2 },   // k = -736 .. -705
    {  -704, 2, 0 },   // k = -704 .. -673
    {  -672, 3, 1 },   // k = -672 .. -641
    {  -640, 0, 3 },   // k = -640 .. -609
    {  -608, 1, 2 },   // k = -608 .. -577
    {  -576, 2, 2 },   // k = -576 .. -545
    {  -544, 3, 3 },   // k = -544 .. -513
    {  -512, 0, 2 },   // k = -512 .. -481
    {  -480, 1, 1 },   // k = -480 .. -449
    {  -448, 2, 2 },   // k = -448 .. -417
    {  -416, 3, 3 },   // k = -416 .. -385
    {  -384, 0, 1 },   // k = -384 .. -353
    {  -352, 1, 2 },   // k = -352 .. -321
    {  -320, 2, 3 },   // k = -320 .. -289
    {  -288, 3, 3 },   // k = -288 .. -257
    {  -256, 0, 2 },   // k = -256 .. -225
    {  -224, 1, 2 },   // k = -224 .. -193
    {  -192, 2, 2 },   // k = -192 .. -161
    {  -160, 3, 1 },   // k = -160 .. -129
    {  -128, 0, 1 },   // k = -128 .. -97
    {   -96, 1, 3 },   // k = -96 .. -65
    {   -64, 2, 1 },   // k = -64 .. -33
    {   -32, 3, 2 },   // k = -32 .. -1
    {     1, 0, 3 },   // k = 1 .. 32
    {    33, 3, 1 },   // k = 33 .. 64
    {    65, 2, 1 },   // k = 65 .. 96
    {    97, 1, 1 },   // k = 97 .. 128
    {   129, 0, 2 },   // k = 129 .. 160
    {   161, 3, 2 },   // k = 161 .. 192
    {   193, 2, 1 },   // k = 193 .. 224
    {   225, 1, 0 },   // k = 225 .. 256
    {   257, 0, 2 },   // k = 257 .. 288
    {   289, 3, 2 },   // k = 289 .. 320
    {   321, 2, 3 },   // k = 321 .. 352
    {   353, 1, 3 },   // k = 353 .. 384
    {   385, 0, 0 },   // k = 385 .. 416
    {   417, 3, 2 },   // k = 417 .. 448
    {   449, 2, 1 },   // k = 449 .. 480
    {   481, 1, 3 },   // k = 481 .. 512
    {   513, 0, 3 },   // k = 513 .. 544
    {   545, 3, 3 },   // k = 545 .. 576
    {   577, 2, 3 },   // k = 577 .. 608
    {   609, 1, 0 },   // k = 609 .. 640
    {   641, 0, 3 },   // k = 641 .. 672
    {   673, 3, 0 },   // k = 673 .. 704
    {   705, 2, 1 },   // k = 705 .. 736
    {   737, 1, 1 },   // k = 737 .. 768
};

/** Table 24: the h parameter, h[i][j], i = 0..3, j = 0..31. */
inline constexpr int kPrsH[4][32] = {
    { 0, 2, 0, 0, 0, 0, 1, 1, 2, 0, 0, 0, 2, 2, 1, 1, 0, 2, 0, 0, 0, 0, 1, 1, 2, 0, 0, 0, 2, 2, 1, 1 },
    { 0, 3, 2, 3, 0, 1, 3, 0, 2, 1, 2, 3, 2, 3, 3, 0, 0, 3, 2, 3, 0, 1, 3, 0, 2, 1, 2, 3, 2, 3, 3, 0 },
    { 0, 0, 0, 2, 0, 2, 1, 3, 2, 2, 0, 2, 2, 0, 1, 3, 0, 0, 0, 2, 0, 2, 1, 3, 2, 2, 0, 2, 2, 0, 1, 3 },
    { 0, 1, 2, 1, 0, 3, 3, 2, 2, 3, 2, 1, 2, 1, 3, 2, 0, 1, 2, 1, 0, 3, 3, 2, 2, 3, 2, 1, 2, 1, 3, 2 },
};

/** The phase reference carrier for k in [-768..768] \ {0}. Unit magnitude by construction — it is
 *  a CAZAC sequence, which is precisely what makes it a good correlation reference. */
inline std::complex<float> prsCarrier(int k) {
    for (const PrsGroup& g : kPrsGroupsI) {
        if (k >= g.kStart && k < g.kStart + 32) {
            const int j = k - g.kStart;
            const int q = (kPrsH[g.i][j] + g.n) & 3;      // multiples of pi/2
            switch (q) {
                case 0: return { 1.0f,  0.0f };
                case 1: return { 0.0f,  1.0f };
                case 2: return { -1.0f, 0.0f };
                default: return { 0.0f, -1.0f };
            }
        }
    }
    return { 0.0f, 0.0f };                                 // DC, or out of range
}

/** Fill 1536 carriers in DAB order (-768..-1, +1..+768). */
inline void prsSymbol(std::complex<float>* out) {
    int j = 0;
    for (int k = -768; k <= 768; ++k) if (k != 0) out[j++] = prsCarrier(k);
}

}  // namespace vibedab
