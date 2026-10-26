//
// LDPC decoder for FT8.
//
// given a 174-bit codeword as an array of log-likelihood of zero,
// return a 174-bit corrected codeword, or zero-length array.
// last 87 bits are the (systematic) plain-text.
// this is an implementation of the sum-product algorithm
// from Sarah Johnson's Iterative Error Correction book.
// codeword[i] = log ( P(x=0) / P(x=1) )
//

#include "ldpc.h"
#include "constants.h"
#include "ldpc_tables.h"
#include "vibe_simd4.h"

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

static int ldpc_check(uint8_t codeword[]);
static float fast_tanh(float x);
static float fast_atanh(float x);

// codeword is 174 log-likelihoods.
// plain is a return value, 174 ints, to be 0 or 1.
// max_iters is how hard to try.
// ok == 87 means success.
void ldpc_decode(float codeword[], int max_iters, uint8_t plain[], int* ok)
{
    float m[FTX_LDPC_M][FTX_LDPC_N]; // ~60 kB
    float e[FTX_LDPC_M][FTX_LDPC_N]; // ~60 kB
    int min_errors = FTX_LDPC_M;

    for (int j = 0; j < FTX_LDPC_M; j++)
    {
        for (int i = 0; i < FTX_LDPC_N; i++)
        {
            m[j][i] = codeword[i];
            e[j][i] = 0.0f;
        }
    }

    for (int iter = 0; iter < max_iters; iter++)
    {
        for (int j = 0; j < FTX_LDPC_M; j++)
        {
            for (int ii1 = 0; ii1 < kFTX_LDPC_Num_rows[j]; ii1++)
            {
                int i1 = kFTX_LDPC_Nm[j][ii1] - 1;
                float a = 1.0f;
                for (int ii2 = 0; ii2 < kFTX_LDPC_Num_rows[j]; ii2++)
                {
                    int i2 = kFTX_LDPC_Nm[j][ii2] - 1;
                    if (i2 != i1)
                    {
                        a *= fast_tanh(-m[j][i2] / 2.0f);
                    }
                }
                e[j][i1] = -2.0f * fast_atanh(a);
            }
        }

        for (int i = 0; i < FTX_LDPC_N; i++)
        {
            float l = codeword[i];
            for (int j = 0; j < 3; j++)
                l += e[kFTX_LDPC_Mn[i][j] - 1][i];
            plain[i] = (l > 0) ? 1 : 0;
        }

        int errors = ldpc_check(plain);

        if (errors < min_errors)
        {
            // Update the current best result
            min_errors = errors;

            if (errors == 0)
            {
                break; // Found a perfect answer
            }
        }

        for (int i = 0; i < FTX_LDPC_N; i++)
        {
            for (int ji1 = 0; ji1 < 3; ji1++)
            {
                int j1 = kFTX_LDPC_Mn[i][ji1] - 1;
                float l = codeword[i];
                for (int ji2 = 0; ji2 < 3; ji2++)
                {
                    if (ji1 != ji2)
                    {
                        int j2 = kFTX_LDPC_Mn[i][ji2] - 1;
                        l += e[j2][i];
                    }
                }
                m[j1][i] = l;
            }
        }
    }

    *ok = min_errors;
}

//
// does a 174-bit codeword pass the FT8's LDPC parity checks?
// returns the number of parity errors.
// 0 means total success.
//
static int ldpc_check(uint8_t codeword[])
{
    int errors = 0;

    for (int m = 0; m < FTX_LDPC_M; ++m)
    {
        uint8_t x = 0;
        for (int i = 0; i < kFTX_LDPC_Num_rows[m]; ++i)
        {
            x ^= codeword[kFTX_LDPC_Nm[m][i] - 1];
        }
        if (x != 0)
        {
            ++errors;
        }
    }
    return errors;
}


/* ★★★ THE FAST BELIEF PROPAGATION (VibeSDR, 2026-10-25 — the FT8 CPU work; not in upstream ft8_lib).
 *  Belief propagation was the biggest single cost of a decode pass: every candidate the search returns runs it, and
 *  the ones that fail (most of them, on a busy band) run all their iterations. bp_decode_ref below is ft8_lib's
 *  original; this is the SAME arithmetic in the SAME order — every sum, product, tanh and atanh rounded as it was —
 *  re-laid so it vectorises:
 *   • by EDGE, not by searching: the original found each edge's partner by scanning kFTX_LDPC_Nm / kFTX_LDPC_Mn with
 *     a compare per step (a third of its instructions); ldpc_tables.h has every edge's place precomputed.
 *   • four variables at a time for variable→check (the three sums, the tanh), four checks at a time for check→
 *     variable (the leave-one-out products, the atanh). Checks have 6 or 7 edges; a 6-edge check's seventh slot holds
 *     1.0, and x·1.0 is exact, so the products are unchanged.
 *   • the parity check on packed bits (three 64-bit masks per check), not 522 byte loads.
 *   • the last iteration's message update, which the original computed and never read, is skipped.
 *  Result: the same `plain` and `ok` as bp_decode_ref for every input (test-ft8-fast checks it on real candidates).
 *  ★ On ARMv7 the tanh/atanh divides are a refined reciprocal (vibe_simd4.h v4_div: no NEON divide there, and the
 *    Pi 2's VFP divide was half of BP's time) — the messages move by ~2 ulp, the outcome has not been seen to. */
static inline v4f v4_fast_tanh(v4f x)
{
    const v4f x2 = v4_mul(x, x);
    const v4f a = v4_mul(x, v4_madd(x2, v4_add(x2, v4_set1(105.0f)), v4_set1(945.0f)));
    const v4f b = v4_madd(x2, v4_madd(x2, v4_set1(15.0f), v4_set1(420.0f)), v4_set1(945.0f));
    v4f r = v4_div(a, b);
    r = v4_sel_lt_negc(x, -4.97f, v4_set1(-1.0f), r);
    return v4_sel_gt_posc(x, 4.97f, v4_set1(1.0f), r);
}
/** -2·fast_atanh(x) */
static inline v4f v4_m2_fast_atanh(v4f x)
{
    const v4f x2 = v4_mul(x, x);
    const v4f a = v4_mul(x, v4_madd(x2, v4_madd(x2, v4_set1(64.0f), v4_set1(-735.0f)), v4_set1(945.0f)));
    const v4f b = v4_madd(x2, v4_madd(x2, v4_set1(225.0f), v4_set1(-1050.0f)), v4_set1(945.0f));
    return v4_mul(v4_div(a, b), v4_set1(-2.0f));
}
static inline unsigned parity64(uint64_t x)
{
    x ^= x >> 32; x ^= x >> 16; x ^= x >> 8; x ^= x >> 4;
    return (0x6996u >> (unsigned)(x & 0xF)) & 1u;
}

void bp_decode(float codeword[], int max_iters, uint8_t plain[], int* ok)
{
    bp_decode_ex(codeword, max_iters, 0, plain, ok);
}

/* ★ VibeSDR (2026-10-26, the sensitivity work): bp_decode with an optional STALL exit. `stall` > 0 stops once the
 *  count of unsatisfied checks has not reached a new low for `stall` iterations — belief propagation that has stopped
 *  improving very rarely converges later, and the second look runs BP three times on every candidate it measures.
 *  stall = 0 is bp_decode exactly (the waterfall path stays bit-identical; test-ft8-fast). */
void bp_decode_ex(float codeword[], int max_iters, int stall, uint8_t plain[], int* ok)
{
    int since_best = 0;
    float cw[LDPC_NP];
    float tov[3][LDPC_NP];          // variable n's message from its k-th check
    float toc[LDPC_TOC_SIZE];       // check m's message from its j-th variable, at j * LDPC_MP + m
    uint64_t hard[3] = { 0, 0, 0 };
    int min_errors = FTX_LDPC_M;

    memcpy(cw, codeword, sizeof(float) * FTX_LDPC_N);
    cw[FTX_LDPC_N] = cw[FTX_LDPC_N + 1] = 0.0f;
    memset(tov, 0, sizeof tov);
    for (int i = 0; i < LDPC_TOC_SIZE; ++i)
        toc[i] = 1.0f;              // ★ the padding slots must stay 1.0: they sit inside every check's product

    for (int iter = 0; iter < max_iters; ++iter)
    {
        // Hard decision: ((codeword + tov0) + tov1) + tov2 > 0, as the original sums it
        hard[0] = hard[1] = hard[2] = 0;
        for (int n = 0; n < LDPC_NP; n += 4)
        {
            const v4f s = v4_add(v4_add(v4_add(v4_load(cw + n), v4_load(tov[0] + n)), v4_load(tov[1] + n)), v4_load(tov[2] + n));
            hard[n >> 6] |= (uint64_t)v4_gt0_mask(s) << (n & 63);
        }
        hard[2] &= (1ull << (FTX_LDPC_N - 128)) - 1;     // the two padding variables are not bits
        if (!(hard[0] | hard[1] | hard[2]))
            break;                  // message converged to all-zeros, which is prohibited

        int errors = 0;
        for (int m = 0; m < FTX_LDPC_M; ++m)
            errors += (int)parity64((hard[0] & kLdpcChkMask[m][0]) ^ (hard[1] & kLdpcChkMask[m][1]) ^ (hard[2] & kLdpcChkMask[m][2]));
        if (errors < min_errors)
        {
            min_errors = errors;
            since_best = 0;
            if (errors == 0)
                break;
        }
        else if (stall > 0 && ++since_best >= stall)
            break;
        if (iter + 1 == max_iters)
            break;                  // ★ the original's last update is never read

        // Variable → check: the sum of the codeword and the OTHER two checks' messages, through tanh(-x/2)
        // ★ pointers walked, not indices: ARMv7 NEON loads take no offset, and GCC rebuilt each address from sp
        {
            const float* pc = cw; const float* p0 = tov[0]; const float* p1 = tov[1]; const float* p2 = tov[2];
            const uint16_t* e0 = kLdpcVarEdge[0]; const uint16_t* e1 = kLdpcVarEdge[1]; const uint16_t* e2 = kLdpcVarEdge[2];
            const v4f h = v4_set1(-0.5f);
            for (int n = 0; n < LDPC_NP; n += 4, pc += 4, p0 += 4, p1 += 4, p2 += 4, e0 += 4, e1 += 4, e2 += 4)
            {
                const v4f c = v4_load(pc), t0 = v4_load(p0), t1 = v4_load(p1), t2 = v4_load(p2);
                v4_scatter(toc, e0, v4_fast_tanh(v4_mul(v4_add(v4_add(c, t1), t2), h)));
                v4_scatter(toc, e1, v4_fast_tanh(v4_mul(v4_add(v4_add(c, t0), t2), h)));
                v4_scatter(toc, e2, v4_fast_tanh(v4_mul(v4_add(v4_add(c, t0), t1), h)));
            }
        }

        // Check → variable: the product of the check's OTHER messages (in edge order), through -2·atanh
        {
            float* const tv = &tov[0][0];
            for (int m = 0; m < LDPC_MP; m += 4)
            {
                v4f t[7];
                const float* q = toc + m;
                for (int j = 0; j < 7; ++j, q += LDPC_MP)
                    t[j] = v4_load(q);
                const uint16_t* out = &kLdpcChkOut[0][m];
                for (int i = 0; i < 7; ++i, out += LDPC_MP)
                {
                    v4f p = t[i == 0 ? 1 : 0];
                    for (int j = (i == 0 ? 2 : 1); j < 7; ++j)
                        if (j != i)
                            p = v4_mul(p, t[j]);
                    v4_scatter(tv, out, v4_m2_fast_atanh(p));
                }
            }
        }
    }

    for (int n = 0; n < FTX_LDPC_N; ++n)
        plain[n] = (uint8_t)((hard[n >> 6] >> (n & 63)) & 1);
    *ok = min_errors;
}

void bp_decode_ref(float codeword[], int max_iters, uint8_t plain[], int* ok)
{
    float tov[FTX_LDPC_N][3];
    float toc[FTX_LDPC_M][7];

    int min_errors = FTX_LDPC_M;

    // initialize message data
    for (int n = 0; n < FTX_LDPC_N; ++n)
    {
        tov[n][0] = tov[n][1] = tov[n][2] = 0;
    }

    for (int iter = 0; iter < max_iters; ++iter)
    {
        // Do a hard decision guess (tov=0 in iter 0)
        int plain_sum = 0;
        for (int n = 0; n < FTX_LDPC_N; ++n)
        {
            plain[n] = ((codeword[n] + tov[n][0] + tov[n][1] + tov[n][2]) > 0) ? 1 : 0;
            plain_sum += plain[n];
        }

        if (plain_sum == 0)
        {
            // message converged to all-zeros, which is prohibited
            break;
        }

        // Check to see if we have a codeword (check before we do any iter)
        int errors = ldpc_check(plain);

        if (errors < min_errors)
        {
            // we have a better guess - update the result
            min_errors = errors;

            if (errors == 0)
            {
                break; // Found a perfect answer
            }
        }

        // Send messages from bits to check nodes
        for (int m = 0; m < FTX_LDPC_M; ++m)
        {
            for (int n_idx = 0; n_idx < kFTX_LDPC_Num_rows[m]; ++n_idx)
            {
                int n = kFTX_LDPC_Nm[m][n_idx] - 1;
                // for each (n, m)
                float Tnm = codeword[n];
                for (int m_idx = 0; m_idx < 3; ++m_idx)
                {
                    if ((kFTX_LDPC_Mn[n][m_idx] - 1) != m)
                    {
                        Tnm += tov[n][m_idx];
                    }
                }
                toc[m][n_idx] = fast_tanh(-Tnm / 2);
            }
        }

        // send messages from check nodes to variable nodes
        for (int n = 0; n < FTX_LDPC_N; ++n)
        {
            for (int m_idx = 0; m_idx < 3; ++m_idx)
            {
                int m = kFTX_LDPC_Mn[n][m_idx] - 1;
                // for each (n, m)
                float Tmn = 1.0f;
                for (int n_idx = 0; n_idx < kFTX_LDPC_Num_rows[m]; ++n_idx)
                {
                    if ((kFTX_LDPC_Nm[m][n_idx] - 1) != n)
                    {
                        Tmn *= toc[m][n_idx];
                    }
                }
                tov[n][m_idx] = -2 * fast_atanh(Tmn);
            }
        }
    }

    *ok = min_errors;
}

// Ideas for approximating tanh/atanh:
// * https://varietyofsound.wordpress.com/2011/02/14/efficient-tanh-computation-using-lamberts-continued-fraction/
// * http://functions.wolfram.com/ElementaryFunctions/ArcTanh/10/0001/
// * https://mathr.co.uk/blog/2017-09-06_approximating_hyperbolic_tangent.html
// * https://math.stackexchange.com/a/446411

static float fast_tanh(float x)
{
    if (x < -4.97f)
    {
        return -1.0f;
    }
    if (x > 4.97f)
    {
        return 1.0f;
    }
    float x2 = x * x;
    // float a = x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2)));
    // float b = 135135.0f + x2 * (62370.0f + x2 * (3150.0f + x2 * 28.0f));
    // float a = x * (10395.0f + x2 * (1260.0f + x2 * 21.0f));
    // float b = 10395.0f + x2 * (4725.0f + x2 * (210.0f + x2));
    float a = x * (945.0f + x2 * (105.0f + x2));
    float b = 945.0f + x2 * (420.0f + x2 * 15.0f);
    return a / b;
}

static float fast_atanh(float x)
{
    float x2 = x * x;
    // float a = x * (-15015.0f + x2 * (19250.0f + x2 * (-5943.0f + x2 * 256.0f)));
    // float b = (-15015.0f + x2 * (24255.0f + x2 * (-11025.0f + x2 * 1225.0f)));
    // float a = x * (-1155.0f + x2 * (1190.0f + x2 * -231.0f));
    // float b = (-1155.0f + x2 * (1575.0f + x2 * (-525.0f + x2 * 25.0f)));
    float a = x * (945.0f + x2 * (-735.0f + x2 * 64.0f));
    float b = (945.0f + x2 * (-1050.0f + x2 * 225.0f));
    return a / b;
}
