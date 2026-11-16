/* vibe_rade.h — VibeSDR's small C face on FreeDV's RADE V1 receiver (docs/v12/FREEDV.md, "RADE").
 *
 * RADE ("Radio Autoencoder", Rowe and Valin) is what nearly every FreeDV station sends today. The receiver itself is
 * FreeDV's C port, rade_c (BSD-2-Clause), with Opus's neural-network code and the FARGAN vocoder (BSD-3-Clause), and
 * the end-of-over callsign is FreeDV's own reliable-text layer from freedv-backend (BSD-2-Clause). None of that source
 * is in this tree: vibeserver/linux/build-rade.sh fetches pinned, SHA-256-checked copies and builds them, with this
 * file's .c files, into ONE static archive (librade_vibe.a) whose only global symbols are the vrade_* functions below.
 * Everything else in it is made local, so its copy of Opus's DNN code can never collide with the libopus the server
 * already links.
 *
 * ★ Receive is the product. The transmitter (vrade_tx_*) is here for the tests and the benchmark's proof only.
 * ★ Not thread-safe per object; one object per thread. Different objects are independent.
 */
#ifndef VIBE_RADE_H
#define VIBE_RADE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Rates: the modem runs at 8 kHz (real samples, as an SSB receiver's audio), the speech comes out at 16 kHz. */
#define VRADE_MODEM_RATE 8000
#define VRADE_SPEECH_RATE 16000

/* vrade_rx_process' info bits */
#define VRADE_INFO_FRAME   0x1   /* a modem frame was decoded into speech features */
#define VRADE_INFO_EOO     0x2   /* an end-of-over frame was received */
#define VRADE_INFO_UWERR   0x4   /* that frame's unique word (the auxiliary bits) had errors */
#define VRADE_INFO_CALL    0x8   /* a callsign passed its LDPC and CRC: vrade_rx_callsign() has it */

typedef struct vrade_rx vrade_rx;

/** One receiver (about 0.5 MB, most of it the decoder's and FARGAN's state). NULL on allocation failure. */
vrade_rx* vrade_rx_open(void);
void vrade_rx_close(vrade_rx* r);
/** Back to the search, decoder and vocoder state cleared (a retune). */
void vrade_rx_reset(vrade_rx* r);

/** How many 8 kHz samples the next vrade_rx_process call takes (it changes as the timing loop slips). */
int vrade_rx_nin(const vrade_rx* r);
/** The most vrade_rx_nin ever asks for. */
int vrade_rx_nin_max(void);
/** The most 16 kHz speech samples one vrade_rx_process call can write. */
int vrade_rx_pcm_max(void);

/** Feed exactly vrade_rx_nin(r) real samples (any consistent scale: the receiver normalises its own level). Writes up to
 *  vrade_rx_pcm_max() speech samples (±1 floats, 16 kHz) to pcm and returns how many; *info gets VRADE_INFO_* bits. */
int vrade_rx_process(vrade_rx* r, const float* in, float* pcm, int* info);

int vrade_rx_sync(const vrade_rx* r);          /* 1 in sync */
float vrade_rx_snr_db(const vrade_rx* r);      /* SNR in 3 kHz (valid in sync) */
float vrade_rx_freq_offset(const vrade_rx* r); /* Hz (valid in sync) */
/** The last callsign that passed the end-of-over check ("" before any). Up to 8 characters. */
const char* vrade_rx_callsign(const vrade_rx* r);

/** FreeDV's reliable text for an end-of-over frame: `callsign` (up to 8 of A-Z 0-9 / and a few symbols) as the 180 soft
 *  bits (±1) the transmitter sends — CRC-8 and LDPC(112,56), interleaved. In the receiver's archive because it shares the
 *  C++ LDPC code with the decoder (the transmitter's archive has none of its own). Returns the bit count. */
int vrade_text_eoo_bits(const char* callsign, float* bits);

/** ★ The benchmark's honest worst case without a transmitter: the decoder network and FARGAN run on `frames` modem
 *  frames of synthetic latents, exactly the work a receiver in sync does per frame (both are dense networks, so the cost
 *  does not depend on the values). Returns the speech samples made, so the work cannot be optimised away. */
long vrade_bench_decode(int frames);

/* ── the transmitter (tests and the benchmark only) ─────────────────────────────────────────────────────────────── */
typedef struct vrade_tx vrade_tx;
vrade_tx* vrade_tx_open(void);
void vrade_tx_close(vrade_tx* t);
/** Samples per modem frame (960) and per end-of-over frame (1152), at 8 kHz. */
int vrade_tx_frame_samples(void);
int vrade_tx_eoo_samples(void);
/** Push n speech samples (16 kHz, ±1). Each completed 120 ms modem frame is appended at 8 kHz — the real part to re (what
 *  an SSB transmitter sends; nominal peak about ±1) and, when im is not NULL, the imaginary part (re + j·im is the analytic
 *  signal: every carrier is above 0 Hz). Returns how many samples were written; each buffer must hold
 *  vrade_tx_frame_samples() * (1 + n / 1920). */
int vrade_tx_speech(vrade_tx* t, const float* speech, int n, float* re, float* im);
/** The end-of-over frame carrying `callsign` (FreeDV's reliable text: up to 8 of A-Z 0-9 / and a few symbols), as above.
 *  Writes vrade_tx_eoo_samples() samples and returns that count. */
int vrade_tx_eoo(vrade_tx* t, const char* callsign, float* re, float* im);

#ifdef __cplusplus
}
#endif
#endif
