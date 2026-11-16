/* vibe_rade.c — VibeSDR's wrapper round FreeDV's RADE V1 receiver (rade_c), FARGAN (Opus) and FreeDV's reliable text.
 * See vibe_rade.h. Built only by vibeserver/linux/build-rade.sh, together with the pinned upstream sources it names.
 *
 * Copyright (c) 2026 VibeSDR. Licensed as VibeServer (GPL-3.0); the upstream code it calls keeps its own BSD licence.
 */
#include "vibe_rade.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rade_rx.h"
#include "rade_dsp.h"
#include "fargan.h"
#include "lpcnet.h"
#include "rade_text.h"

#define VR_FEAT_OUT (RADE_NZMF * RADE_FRAMES_PER_STEP * RADE_NB_TOTAL_FEATURES)   /* 12 frames × 36 */
#define VR_EOO_BITS ((RADE_NS - 1) * RADE_NC * 2)                                  /* 180 */
#define VR_PCM_MAX (RADE_NZMF * RADE_FRAMES_PER_STEP * LPCNET_FRAME_SIZE)          /* 1920 */

struct vrade_rx {
    rade_rx_state rx;
    FARGANState fargan;
    int fargan_ready, cont_frames;
    float cont[5 * NB_TOTAL_FEATURES];
    float feat[VR_FEAT_OUT];
    float eoo[VR_EOO_BITS];
    RADE_COMP iq[RADE_NMF + RADE_M];
    rade_text_t text;
    char call[16];
    int call_new;
};

static void on_text(rade_text_t rt, const char* txt, int len, void* state) {
    (void)rt;
    struct vrade_rx* r = (struct vrade_rx*)state;
    if (len < 0) len = 0;
    if (len > 8) len = 8;
    memcpy(r->call, txt, (size_t)len);
    r->call[len] = 0;
    r->call_new = 1;
}

static int rx_init(struct vrade_rx* r) {
    /* bottleneck 3, auxiliary data on, the receive band-pass filter on — exactly as rade_open() sets V1 up */
    if (rade_rx_init(&r->rx, NULL, 3, 1, 1) != 0) return -1;
    r->rx.verbose = 0;
    fargan_init(&r->fargan);
    r->fargan_ready = 0;
    r->cont_frames = 0;
    return 0;
}

vrade_rx* vrade_rx_open(void) {
    struct vrade_rx* r = (struct vrade_rx*)calloc(1, sizeof(struct vrade_rx));
    if (!r) return NULL;
    if (rx_init(r) != 0) { free(r); return NULL; }
    r->text = rade_text_create();
    if (r->text) {
        rade_text_enable_stats_output(r->text, 0);   /* ★ its statistics use file-scope globals: off, for threads */
        rade_text_set_rx_callback(r->text, on_text, r);
    }
    return r;
}

void vrade_rx_close(vrade_rx* r) {
    if (!r) return;
    if (r->text) rade_text_destroy(r->text);
    free(r);
}

void vrade_rx_reset(vrade_rx* r) {
    if (!r) return;
    rx_init(r);
    r->call[0] = 0;
    r->call_new = 0;
}

int vrade_rx_nin(const vrade_rx* r) { return rade_rx_nin(&r->rx); }
int vrade_rx_nin_max(void) { return RADE_NMF + RADE_M; }
int vrade_rx_pcm_max(void) { return VR_PCM_MAX; }
int vrade_rx_sync(const vrade_rx* r) { return rade_rx_sync(&r->rx); }
float vrade_rx_snr_db(const vrade_rx* r) { return rade_rx_snrdB_3k_est(&r->rx); }
float vrade_rx_freq_offset(const vrade_rx* r) { return rade_rx_freq_offset(&r->rx); }
const char* vrade_rx_callsign(const vrade_rx* r) { return r->call; }

/* FARGAN: the first five feature frames prime it (fargan_cont), as rade_rx_wav and lpcnet_demo do; then one 10 ms frame
 * of speech per feature frame. */
static int synth(struct vrade_rx* r, const float* feat, float* pcm) {
    if (!r->fargan_ready) {
        memcpy(&r->cont[r->cont_frames * NB_TOTAL_FEATURES], feat, sizeof(float) * NB_TOTAL_FEATURES);
        if (++r->cont_frames >= 5) {
            float packed[5 * NB_FEATURES];
            float zeros[FARGAN_CONT_SAMPLES];
            for (int i = 0; i < 5; i++)
                memcpy(&packed[i * NB_FEATURES], &r->cont[i * NB_TOTAL_FEATURES], sizeof(float) * NB_FEATURES);
            memset(zeros, 0, sizeof(zeros));
            fargan_cont(&r->fargan, zeros, packed);
            r->fargan_ready = 1;
        }
        return 0;
    }
    fargan_synthesize(&r->fargan, pcm, feat);
    for (int i = 0; i < LPCNET_FRAME_SIZE; i++) {
        float v = pcm[i];
        pcm[i] = v > 1.f ? 1.f : v < -1.f ? -1.f : v;
    }
    return LPCNET_FRAME_SIZE;
}

int vrade_rx_process(vrade_rx* r, const float* in, float* pcm, int* info) {
    const int nin = rade_rx_nin(&r->rx);
    int flags = 0, npcm = 0;
    /* ★ real input, imaginary part zero: the OFDM carriers sit at 1062–1875 Hz and the correlators reject the negative-
     *   frequency mirror (rade_rx_wav does the same, with no Hilbert transform). The level does not matter: acquisition
     *   thresholds on its own noise estimate and the demodulator normalises the pilots' magnitude. */
    for (int i = 0; i < nin; i++) { r->iq[i].real = in[i]; r->iq[i].imag = 0.f; }
    const int wasSync = rade_rx_sync(&r->rx);
    const int uwBefore = r->rx.uw_errors;
    const int ret = rade_rx_process(&r->rx, r->feat, r->eoo, r->iq);
    if (ret & 0x1) {
        flags |= VRADE_INFO_FRAME;
        /* ★ This frame's unique-word errors. rade_rx_process clears its counter once a second (on the frame whose
         *   synced_count is a whole second) BEFORE adding the frame's own, so the frame's share is then the whole count. */
        const int reset = wasSync && r->rx.synced_count_one_sec > 0 && (r->rx.synced_count % r->rx.synced_count_one_sec) == 0;
        const int uw = reset ? r->rx.uw_errors : r->rx.uw_errors - uwBefore;
        if (uw > 0) flags |= VRADE_INFO_UWERR;
        for (int f = 0; f < RADE_NZMF * RADE_FRAMES_PER_STEP; f++)
            npcm += synth(r, &r->feat[f * RADE_NB_TOTAL_FEATURES], pcm + npcm);
    }
    if (ret & 0x2) {
        flags |= VRADE_INFO_EOO;
        if (r->text) {
            r->call_new = 0;
            rade_text_rx(r->text, r->eoo, VR_EOO_BITS / 2);
            if (r->call_new) flags |= VRADE_INFO_CALL;
        }
    }
    if (info) *info = flags;
    return npcm;
}

int vrade_text_eoo_bits(const char* callsign, float* bits) {
    rade_text_t t = rade_text_create();
    if (!t) return 0;
    rade_text_enable_stats_output(t, 0);
    const char* cs = callsign ? callsign : "";
    rade_text_generate_tx_string(t, cs, (int)strlen(cs), bits, VR_EOO_BITS);
    rade_text_destroy(t);
    return VR_EOO_BITS;
}

long vrade_bench_decode(int frames) {
    struct vrade_rx* r = vrade_rx_open();
    if (!r) return -1;
    float z[RADE_LATENT_DIM];
    float dec[RADE_FRAMES_PER_STEP * RADE_NUM_FEATURES_AUX];
    float feat[NB_TOTAL_FEATURES];
    float pcm[LPCNET_FRAME_SIZE];
    unsigned s = 12345u;
    long made = 0;
    for (int m = 0; m < frames; m++) {
        for (int c = 0; c < RADE_NZMF; c++) {
            for (int i = 0; i < RADE_LATENT_DIM; i++) { s = s * 1103515245u + 12345u; z[i] = ((float)((s >> 9) & 0xffff) / 32768.f - 1.f) * 0.7f; }
            rade_core_decoder(&r->rx.dec_state, &r->rx.dec_model, dec, z, 0);
            for (int k = 0; k < RADE_FRAMES_PER_STEP; k++) {
                memset(feat, 0, sizeof(feat));
                for (int j = 0; j < RADE_NUM_FEATURES; j++) feat[j] = dec[k * (RADE_NUM_FEATURES + 1) + j];
                made += synth(r, feat, pcm);
                made += pcm[0] != pcm[0];   /* keeps the vocoder's output live */
            }
        }
    }
    vrade_rx_close(r);
    return made;
}
