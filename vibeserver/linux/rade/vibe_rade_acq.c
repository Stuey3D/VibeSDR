/* vibe_rade_acq.c — RADE V1's pilot SEARCH, reorganised so the compiler vectorises it (NEON / SSE2; scalar on ARMv6).
 *
 * ★★ WHY (measured 2026-11-16): a RADE receiver that is SEARCHING — the state it is in whenever nobody is transmitting,
 *    which is most of the time — cost 3.5 % of an M5 core, three times what it costs IN SYNC decoding speech (1.1 %). The
 *    search is rade_c's rade_acq_detect_pilots: every modem frame (120 ms) it correlates the received signal with the pilot
 *    symbol at each of 960 timing offsets × 40 frequency offsets, 160 samples each, twice (this frame and the next) —
 *    12.3 million complex multiply-adds a frame, ~100 million a second, as scalar complex arithmetic on RADE_COMP structs.
 *    That is the RADE cost that decides whether a small ARM board can run it at all.
 * ★★ WHAT CHANGES: nothing in the arithmetic — the same sums, each over n in the same order, the same maximum (time outer,
 *    frequency inner, first strict maximum), the same threshold. Only the loop order: for each t, eight frequency bins at
 *    a time are summed over all 160 samples from a pilot table stored [n][f] as separate real and imaginary rows — a plain
 *    float loop the compiler vectorises with SSE2 (x86), its eight running sums held in registers; NEON (ARMv7 and AArch64)
 *    is written out by hand, because GCC vectorised neither (see the loop). The |Dt| sums for the noise threshold are taken
 *    in the same pass (same order as upstream's second pass) instead of a second sweep of the grid. Off-air (rade_c's own
 *    5-minute recording) the decode is the same as upstream's: the same syncs, frames and callsigns.
 * ★ rade_c's own function is still built (renamed vrade_upstream_acq_detect_pilots by build-rade.sh) and used if the
 *   frequency grid is ever not the 40 bins this was written for.
 *
 * Copyright (c) 2026 VibeSDR. Licensed as VibeServer (GPL-3.0). Derived from rade_c's rade_acq.c (BSD-2-Clause,
 * Copyright (C) 2024 David Rowe / 2026 Peter B Marks), whose notice ships with VibeServer as LICENSE.rade_c.
 */
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "rade_acq.h"
#include "rade_ofdm.h"

int vrade_upstream_acq_detect_pilots(rade_acq* acq, const RADE_COMP* rx, int* tmax, float* fmax);
int vrade_upstream_acq_check_pilots(rade_acq* acq, const RADE_COMP* rx, int tmax, float fmax, int* valid, int* endofover);

#define VR_NF RADE_ACQ_NFREQ
#define VR_FB 8   /* frequency bins per pass (VR_NF is a multiple) */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define VR_NEON 1
/* ★ AArch64: the FUSED multiply-add (fmla), one instruction. vmlaq there is defined as an unfused multiply then add, and
 *   GCC 12 emits it as two (measured: 283 M instructions a second for the search on arm64, more than armhf's single vmla). */
#if defined(__aarch64__)
#define VR_MLA(a, b, v, l) vfmaq_lane_f32(a, b, v, l)
#define VR_MLS(a, b, v, l) vfmsq_lane_f32(a, b, v, l)
#else
#define VR_MLA(a, b, v, l) vmlaq_lane_f32(a, b, v, l)
#define VR_MLS(a, b, v, l) vmlsq_lane_f32(a, b, v, l)
#endif
#else
#define VR_NEON 0
#endif

/* The frequency-shifted pilots p_w[f][n] (rade_acq_init), transposed to [n][f] and split into real and imaginary rows.
 * Identical for every receiver — it depends only on RADE's constants — so it is built once. */
static float g_pre[RADE_M][VR_NF] __attribute__((aligned(16))), g_pim[RADE_M][VR_NF] __attribute__((aligned(16)));
static int g_nf = 0;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void build_table(void) {
    static rade_ofdm ofdm;
    static rade_acq acq;
    rade_ofdm_init(&ofdm, 3);
    rade_acq_init(&acq, &ofdm, RADE_ACQ_FRANGE, RADE_ACQ_FSTEP);
    if (acq.n_fcoarse != VR_NF) return;   /* not the grid this was written for: g_nf stays 0, upstream is used */
    for (int n = 0; n < RADE_M; n++)
        for (int f = 0; f < VR_NF; f++) { g_pre[n][f] = acq.p_w[f][n].real; g_pim[n][f] = acq.p_w[f][n].imag; }
    g_nf = VR_NF;
}

/* ★ One timing offset t, all 40 frequency bins: Dt1 = Σn conj(rx[t+n])·p_w[f][n] and Dt2 the same one modem frame later. */
static void corr_t(const RADE_COMP* rx, int t, int Nmf, float* a1r, float* a1i, float* a2r, float* a2i) {
    const RADE_COMP* x = rx + t;
    const RADE_COMP* y = rx + t + Nmf;
    /* ★ 8 frequency bins at a time, all 160 samples, so the eight running sums (4 × 2 vectors) stay in registers —
     *   with all 40 bins at once they would be 160 floats, re-read and re-written for every sample. */
    for (int fb = 0; fb < VR_NF; fb += VR_FB) {
#if VR_NEON
        /* ★★ NEON by hand, ARMv7 and AArch64 alike: GCC will not vectorise float on 32-bit NEON at all (NEON flushes
         *    denormals, so it is not IEEE without -funsafe-math-optimizations) and left AArch64's loop scalar too
         *    (measured 2026-11-16 under qemu: 1160 M instructions a second on arm64, 495 M on armhf). The four received
         *    values (x, y, imaginary parts negated) ride in ONE register and are used by lane, so nothing is moved between
         *    register files in the loop, and the eight sums stay in registers. Each sum is still over n in order; the one
         *    difference from the C below is that s += xr·pr − xi·pi is two multiply-accumulates (s + xr·pr, then − xi·pi),
         *    a last-bit rounding difference (fused on AArch64, so a little nearer the exact sum). */
        const float32x4_t sgn = {1.f, -1.f, 1.f, -1.f};
        float32x4_t s1r0 = vdupq_n_f32(0), s1r1 = s1r0, s1i0 = s1r0, s1i1 = s1r0, s2r0 = s1r0, s2r1 = s1r0, s2i0 = s1r0, s2i1 = s1r0;
        for (int n = 0; n < RADE_M; n++) {
            const float32x4_t xy = vmulq_f32(vcombine_f32(vld1_f32(&x[n].real), vld1_f32(&y[n].real)), sgn);
            const float32x2_t X = vget_low_f32(xy), Y = vget_high_f32(xy);   /* {xr, −xi}, {yr, −yi} */
            const float32x4_t pr0 = vld1q_f32(&g_pre[n][fb]), pr1 = vld1q_f32(&g_pre[n][fb + 4]);
            const float32x4_t pi0 = vld1q_f32(&g_pim[n][fb]), pi1 = vld1q_f32(&g_pim[n][fb + 4]);
            s1r0 = VR_MLS(VR_MLA(s1r0, pr0, X, 0), pi0, X, 1);
            s1r1 = VR_MLS(VR_MLA(s1r1, pr1, X, 0), pi1, X, 1);
            s1i0 = VR_MLA(VR_MLA(s1i0, pi0, X, 0), pr0, X, 1);
            s1i1 = VR_MLA(VR_MLA(s1i1, pi1, X, 0), pr1, X, 1);
            s2r0 = VR_MLS(VR_MLA(s2r0, pr0, Y, 0), pi0, Y, 1);
            s2r1 = VR_MLS(VR_MLA(s2r1, pr1, Y, 0), pi1, Y, 1);
            s2i0 = VR_MLA(VR_MLA(s2i0, pi0, Y, 0), pr0, Y, 1);
            s2i1 = VR_MLA(VR_MLA(s2i1, pi1, Y, 0), pr1, Y, 1);
        }
        vst1q_f32(&a1r[fb], s1r0); vst1q_f32(&a1r[fb + 4], s1r1); vst1q_f32(&a1i[fb], s1i0); vst1q_f32(&a1i[fb + 4], s1i1);
        vst1q_f32(&a2r[fb], s2r0); vst1q_f32(&a2r[fb + 4], s2r1); vst1q_f32(&a2i[fb], s2i0); vst1q_f32(&a2i[fb + 4], s2i1);
#else
        float s1r[VR_FB] = {0}, s1i[VR_FB] = {0}, s2r[VR_FB] = {0}, s2i[VR_FB] = {0};
        for (int n = 0; n < RADE_M; n++) {
            /* conj(rx) · p_w, as rade_cmul(rade_cconj(rx), p_w) */
            const float xr = x[n].real, xi = -x[n].imag, yr = y[n].real, yi = -y[n].imag;
            const float* pr = &g_pre[n][fb];
            const float* pi = &g_pim[n][fb];
            for (int f = 0; f < VR_FB; f++) {
                s1r[f] += xr * pr[f] - xi * pi[f];
                s1i[f] += xr * pi[f] + xi * pr[f];
                s2r[f] += yr * pr[f] - yi * pi[f];
                s2i[f] += yr * pi[f] + yi * pr[f];
            }
        }
        for (int f = 0; f < VR_FB; f++) { a1r[fb + f] = s1r[f]; a1i[fb + f] = s1i[f]; a2r[fb + f] = s2r[f]; a2i[fb + f] = s2i[f]; }
#endif
    }
}

int rade_acq_detect_pilots(rade_acq* acq, const RADE_COMP* rx, int* tmax, float* fmax) {
    pthread_once(&g_once, build_table);
    const int M = acq->m, Nmf = acq->nmf;
    if (g_nf != VR_NF || acq->n_fcoarse != VR_NF || M != RADE_M || VR_NF % VR_FB)
        return vrade_upstream_acq_detect_pilots(acq, rx, tmax, fmax);

    float Dtmax12 = 0.0f;
    int f_ind_max = 0, t_max = 0;
    float f_max = 0.0f;
    float sum_abs_Dt1 = 0.0f, sum_abs_Dt2 = 0.0f;

    for (int t = 0; t < Nmf; t++) {
        float a1r[VR_NF], a1i[VR_NF], a2r[VR_NF], a2i[VR_NF];
        corr_t(rx, t, Nmf, a1r, a1i, a2r, a2i);
        for (int f = 0; f < VR_NF; f++) {
            acq->Dt1[t][f].real = a1r[f]; acq->Dt1[t][f].imag = a1i[f];
            acq->Dt2[t][f].real = a2r[f]; acq->Dt2[t][f].imag = a2i[f];
            const float m1 = sqrtf(a1r[f] * a1r[f] + a1i[f] * a1i[f]);
            const float m2 = sqrtf(a2r[f] * a2r[f] + a2i[f] * a2i[f]);
            sum_abs_Dt1 += m1;
            sum_abs_Dt2 += m2;
            const float Dt12 = m1 + m2;
            if (Dt12 > Dtmax12) { Dtmax12 = Dt12; f_ind_max = f; f_max = acq->fcoarse_range[f]; t_max = t; }
        }
    }

    const int count = Nmf * VR_NF;
    const float sigma_r1 = (sum_abs_Dt1 / count) / sqrtf((float)M_PI / 2.0f);
    const float sigma_r2 = (sum_abs_Dt2 / count) / sqrtf((float)M_PI / 2.0f);
    const float sigma_r = (sigma_r1 + sigma_r2) / 2.0f;
    acq->Dthresh = 2.0f * sigma_r * sqrtf(-logf(acq->Pacq_error1 / 5.0f));
    acq->Dtmax12 = Dtmax12;
    acq->f_ind_max = f_ind_max;
    *tmax = t_max;
    *fmax = f_max;
    return (Dtmax12 > acq->Dthresh) ? 1 : 0;
}

/* ★★ IN SYNC, rade_acq_check_pilots (once a modem frame) refreshes 5 % of the search grid — 48 random timing offsets × 40
 *    bins, the noise estimate behind the threshold — the same correlation as the search, so the same vectorised corr_t.
 *    The rest is rade_c's function line for line (rand() drawn the same way, the grid's |Dt| summed in the same order). */
int rade_acq_check_pilots(rade_acq* acq, const RADE_COMP* rx, int tmax, float fmax, int* valid, int* endofover) {
    pthread_once(&g_once, build_table);
    const int M = acq->m, Ncp = acq->ncp, Nmf = acq->nmf;
    const float Fs = (float)acq->fs;
    if (g_nf != VR_NF || acq->n_fcoarse != VR_NF || M != RADE_M || VR_NF % VR_FB)
        return vrade_upstream_acq_check_pilots(acq, rx, tmax, fmax, valid, endofover);

    const int Nupdate = (int)(0.05f * Nmf);
    for (int i = 0; i < Nupdate; i++) {
        const int t = rand() % Nmf;
        float a1r[VR_NF], a1i[VR_NF], a2r[VR_NF], a2i[VR_NF];
        corr_t(rx, t, Nmf, a1r, a1i, a2r, a2i);
        for (int f = 0; f < VR_NF; f++) {
            acq->Dt1[t][f].real = a1r[f]; acq->Dt1[t][f].imag = a1i[f];
            acq->Dt2[t][f].real = a2r[f]; acq->Dt2[t][f].imag = a2i[f];
        }
    }

    float sum_abs_Dt1 = 0.0f, sum_abs_Dt2 = 0.0f;
    for (int t = 0; t < Nmf; t++)
        for (int f = 0; f < VR_NF; f++) {
            sum_abs_Dt1 += rade_cabs(acq->Dt1[t][f]);
            sum_abs_Dt2 += rade_cabs(acq->Dt2[t][f]);
        }
    const int count = Nmf * VR_NF;
    const float sigma_r1 = (sum_abs_Dt1 / count) / sqrtf((float)M_PI / 2.0f);
    const float sigma_r2 = (sum_abs_Dt2 / count) / sqrtf((float)M_PI / 2.0f);
    const float sigma_r = (sigma_r1 + sigma_r2) / 2.0f;
    acq->Dthresh = 2.0f * sigma_r * sqrtf(-logf(acq->Pacq_error2 / 5.0f));
    const float Dthresh_eoo = 2.0f * sigma_r * sqrtf(-logf(acq->Pacq_error1 / 5.0f));

    /* the correlation at the current timing and frequency, normal pilots and end-of-over pilots — as upstream */
    const float w = 2.0f * (float)M_PI * fmax / Fs;
    RADE_COMP w_vec[RADE_M];
    for (int n = 0; n < M; n++) w_vec[n] = rade_cexp(-w * n);
    RADE_COMP Dt1 = rade_czero(), Dt2 = rade_czero();
    for (int n = 0; n < M; n++) {
        RADE_COMP rs = rade_cmul(w_vec[n], rx[tmax + n]);
        Dt1 = rade_cadd(Dt1, rade_cmul(rade_cconj(rs), acq->p[n]));
        rs = rade_cmul(w_vec[n], rx[tmax + Nmf + n]);
        Dt2 = rade_cadd(Dt2, rade_cmul(rade_cconj(rs), acq->p[n]));
    }
    const float Dtmax12 = rade_cabs(Dt1) + rade_cabs(Dt2);
    acq->Dtmax12 = Dtmax12;
    RADE_COMP Dt1_eoo = rade_czero(), Dt2_eoo = rade_czero();
    for (int n = 0; n < M; n++) {
        RADE_COMP rs = rade_cmul(w_vec[n], rx[tmax + M + Ncp + n]);
        Dt1_eoo = rade_cadd(Dt1_eoo, rade_cmul(rade_cconj(rs), acq->pend[n]));
        rs = rade_cmul(w_vec[n], rx[tmax + Nmf + n]);
        Dt2_eoo = rade_cadd(Dt2_eoo, rade_cmul(rade_cconj(rs), acq->pend[n]));
    }
    const float Dtmax12_eoo = rade_cabs(Dt1_eoo) + rade_cabs(Dt2_eoo);
    acq->Dtmax12_eoo = Dtmax12_eoo;
    *valid = (Dtmax12 > acq->Dthresh) ? 1 : 0;
    *endofover = (Dtmax12_eoo > Dthresh_eoo) ? 1 : 0;
    return *valid;
}
