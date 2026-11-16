/* vibe_rade_tx.c — the RADE V1 TRANSMITTER, for VibeSDR's tests and nothing else (see vibe_rade.h). Built by
 * vibeserver/linux/build-rade.sh into its own archive, librade_vibe_tx.a, so the encoder's weights and Opus's feature
 * extractor never reach the server binary. It needs librade_vibe.a beside it (vrade_text_eoo_bits).
 *
 * Copyright (c) 2026 VibeSDR. Licensed as VibeServer (GPL-3.0); the upstream code it calls keeps its own BSD licence.
 */
#include "vibe_rade.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rade_tx.h"
#include "rade_dsp.h"
#include "lpcnet.h"

#define VR_FEAT_OUT (RADE_NZMF * RADE_FRAMES_PER_STEP * RADE_NB_TOTAL_FEATURES)
#define VR_EOO_BITS ((RADE_NS - 1) * RADE_NC * 2)

/* ── the transmitter ─────────────────────────────────────────────────────────────────────────────────────────────── */
struct vrade_tx {
    rade_tx_state tx;
    LPCNetEncState* net;
    float feat[VR_FEAT_OUT];
    int nfeat;
    short frame[LPCNET_FRAME_SIZE];
    int nframe;
    RADE_COMP iq[RADE_NEOO];
};

vrade_tx* vrade_tx_open(void) {
    struct vrade_tx* t = (struct vrade_tx*)calloc(1, sizeof(struct vrade_tx));
    if (!t) return NULL;
    /* bottleneck 3, auxiliary data on, no transmit band-pass filter — rade_open()'s V1 transmitter */
    if (rade_tx_init(&t->tx, NULL, 3, 1, 0) != 0) { free(t); return NULL; }
    t->net = lpcnet_encoder_create();
    if (!t->net) { vrade_tx_close(t); return NULL; }
    return t;
}

void vrade_tx_close(vrade_tx* t) {
    if (!t) return;
    if (t->net) lpcnet_encoder_destroy(t->net);
    free(t);
}

int vrade_tx_frame_samples(void) { return RADE_NMF; }
int vrade_tx_eoo_samples(void) { return RADE_NEOO; }

int vrade_tx_speech(vrade_tx* t, const float* sp, int n, float* out, float* im) {
    int w = 0;
    for (int i = 0; i < n; i++) {
        float v = sp[i] * 32768.f;
        v = v > 32767.f ? 32767.f : v < -32767.f ? -32767.f : v;
        t->frame[t->nframe++] = (short)lrintf(v);
        if (t->nframe < LPCNET_FRAME_SIZE) continue;
        t->nframe = 0;
        lpcnet_compute_single_frame_features(t->net, t->frame, &t->feat[t->nfeat * NB_TOTAL_FEATURES], 0);
        if (++t->nfeat < RADE_NZMF * RADE_FRAMES_PER_STEP) continue;
        t->nfeat = 0;
        const int no = rade_tx_process(&t->tx, t->iq, t->feat);
        for (int k = 0; k < no; k++, w++) { out[w] = t->iq[k].real; if (im) im[w] = t->iq[k].imag; }
    }
    return w;
}

int vrade_tx_eoo(vrade_tx* t, const char* callsign, float* out, float* im) {
    float bits[VR_EOO_BITS];
    vrade_text_eoo_bits(callsign, bits);   /* ★ from librade_vibe.a: the LDPC code lives there once (C++ COMDAT, see build-rade.sh) */
    rade_tx_state_set_eoo_bits(&t->tx, bits);
    const int no = rade_tx_state_eoo(&t->tx, t->iq);
    for (int k = 0; k < no; k++) { out[k] = t->iq[k].real; if (im) im[k] = t->iq[k].imag; }
    return no;
}
