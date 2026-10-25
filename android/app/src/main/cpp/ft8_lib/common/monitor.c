#include "monitor.h"
#include <common/common.h>

#define LOG_LEVEL LOG_INFO
#include <ft8/debug.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../vibedsp/third_party/pffft/pffft.h"
#include "../ft8/vibe_simd4.h"

static float hann_i(int i, int N)
{
    float x = sinf((float)M_PI * i / N);
    return x * x;
}

// static float hamming_i(int i, int N)
// {
//     const float a0 = (float)25 / 46;
//     const float a1 = 1 - a0;

//     float x1 = cosf(2 * (float)M_PI * i / N);
//     return a0 - a1 * x1;
// }

// static float blackman_i(int i, int N)
// {
//     const float alpha = 0.16f; // or 2860/18608
//     const float a0 = (1 - alpha) / 2;
//     const float a1 = 1.0f / 2;
//     const float a2 = alpha / 2;

//     float x1 = cosf(2 * (float)M_PI * i / N);
//     float x2 = 2 * x1 * x1 - 1; // Use double angle formula

//     return a0 - a1 * x1 + a2 * x2;
// }

static void waterfall_init(ftx_waterfall_t* me, int max_blocks, int num_bins, int time_osr, int freq_osr)
{
    // VibeSDR local change (CodeQL cpp/integer-multiplication-cast-to-long): widen to size_t BEFORE
    // multiplying, so the int product cannot overflow on its way into a size_t. Not in upstream ft8_lib.
    size_t mag_size = (size_t)max_blocks * time_osr * freq_osr * num_bins * sizeof(me->mag[0]);
    me->max_blocks = max_blocks;
    me->num_blocks = 0;
    me->num_bins = num_bins;
    me->time_osr = time_osr;
    me->freq_osr = freq_osr;
    me->block_stride = (time_osr * freq_osr * num_bins);
    me->mag = (WF_ELEM_T*)malloc(mag_size);
    LOG(LOG_DEBUG, "Waterfall size = %zu\n", mag_size);
}

static void waterfall_free(ftx_waterfall_t* me)
{
    free(me->mag);
}

/* ★★ THE WATERFALL LEVEL WITHOUT A LOG PER BIN (VibeSDR, 2026-10-25 — the FT8 CPU work; not in upstream ft8_lib).
 *  Each bin's power x is stored as (int)(2·10·log10f(x) + 240), clamped to 0..255 — a log10f per bin, ~200,000 a
 *  pass, which on a Pi 2 (glibc's log10f, ~60 instructions) was 6 % of the whole decode. That function is monotonic
 *  in x, so it is fully described by the 255 smallest x at which it reaches 1, 2 … 255 — found ONCE, at init, by
 *  bisecting the float bit patterns through the very same expression (wf_quant_ref), on this platform, with this
 *  compiler. A bin is then one table lookup on its top 13 bits (exponent + 4 mantissa bits: at most a 6.25 % range,
 *  while a step is 12.2 %, so at most one threshold falls inside — 3 bits would be 12.5 %, too wide) and one compare. Same answer as log10f for every x
 *  (test-ft8-fast checks all 255 edges and random levels). */
static uint8_t wf_quant_ref(float x)
{
    float db = 10.0f * log10f(x);
    int scaled = (int)(2 * db + 240);
    return (uint8_t)((scaled < 0) ? 0 : ((scaled > 255) ? 255 : scaled));
}
static uint32_t f2u(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static float u2f(uint32_t u) { float x; memcpy(&x, &u, 4); return x; }
static void wf_quant_init(monitor_t* me)
{
    // threshold[k] = the smallest positive float x with wf_quant_ref(x) >= k (positive floats order as their bits)
    for (int k = 1; k < 256; ++k)
    {
        uint32_t lo = 1, hi = 0x7f800000u;          // smallest positive denormal .. +inf
        if (wf_quant_ref(u2f(hi - 1)) < k) { me->wf_thr[k] = INFINITY; continue; }
        while (lo < hi)
        {
            const uint32_t mid = lo + (hi - lo) / 2;
            if (wf_quant_ref(u2f(mid)) >= k) hi = mid; else lo = mid + 1;
        }
        me->wf_thr[k] = u2f(lo);
    }
    me->wf_thr[0] = 0.0f;
    for (uint32_t c = 0; c < 4096; ++c)             // the level at the bottom of each cell (positive floats only)
        me->wf_cell[c] = c ? wf_quant_ref(u2f(c << 19)) : 0;
}
static inline uint8_t wf_quant(const monitor_t* me, float x)
{
    // x is 1e-12 + |X|^2: positive and finite
    const uint8_t v = me->wf_cell[(f2u(x) >> 19) & 4095u];
    return (uint8_t)(v + (v < 255 && x >= me->wf_thr[v + 1]));
}

uint8_t monitor_level(const monitor_t* me, float x) { return wf_quant(me, x); }
uint8_t monitor_level_ref(float x) { return wf_quant_ref(x); }

void monitor_init(monitor_t* me, const monitor_config_t* cfg)
{
    float slot_time = (cfg->protocol == FTX_PROTOCOL_FT4) ? FT4_SLOT_TIME : FT8_SLOT_TIME;
    float symbol_period = (cfg->protocol == FTX_PROTOCOL_FT4) ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD;
    // Compute DSP parameters that depend on the sample rate
    me->block_size = (int)(cfg->sample_rate * symbol_period); // samples corresponding to one FSK symbol
    me->subblock_size = me->block_size / cfg->time_osr;
    me->nfft = me->block_size * cfg->freq_osr;
    me->fft_norm = 2.0f / me->nfft;
    // const int len_window = 1.8f * me->block_size; // hand-picked and optimized

    me->window = (float*)malloc(me->nfft * sizeof(me->window[0]));
    for (int i = 0; i < me->nfft; ++i)
    {
        // window[i] = 1;
        me->window[i] = me->fft_norm * hann_i(i, me->nfft);
        // me->window[i] = blackman_i(i, me->nfft);
        // me->window[i] = hamming_i(i, me->nfft);
        // me->window[i] = (i < len_window) ? hann_i(i, len_window) : 0;
    }
    me->last_frame = (float*)calloc(me->nfft, sizeof(me->last_frame[0]));

    LOG(LOG_INFO, "Block size = %d\n", me->block_size);
    LOG(LOG_INFO, "Subblock size = %d\n", me->subblock_size);

    size_t fft_work_size = 0;
    kiss_fftr_alloc(me->nfft, 0, 0, &fft_work_size);
    me->fft_work = malloc(fft_work_size);
    me->fft_cfg = kiss_fftr_alloc(me->nfft, 0, me->fft_work, &fft_work_size);

    /* ★★ THE STFT THROUGH PFFFT (VibeSDR, 2026-10-25 — the FT8 CPU work; not in upstream ft8_lib). The spectrogram is
     *  rebuilt on every pass, and Kiss FFT was 1.5 of its 1.95 ms (an M5, 186 real FFTs of 3840). PFFFT — already in
     *  every build, for DAB — does the same transform with NEON/SSE butterflies. Same transform, different rounding:
     *  the waterfall stores 0.5 dB steps, so a bin changes only when its level sits within ~1e-6 dB of a step edge
     *  (measured: the commit message). PFFFT needs N a multiple of 32 of the form 2^a 3^b 5^c — FT8 at 12 kHz is
     *  3840, FT4 1152; any other size keeps Kiss FFT. */
    me->pf_setup = NULL;
    me->pf_in = me->pf_out = me->pf_work = NULL;
    if ((me->nfft % 32) == 0)
    {
        int r = me->nfft;
        while (r % 2 == 0) r /= 2;
        while (r % 3 == 0) r /= 3;
        while (r % 5 == 0) r /= 5;
        if (r == 1)
            me->pf_setup = pffft_new_setup(me->nfft, PFFFT_REAL);
    }
    if (me->pf_setup)
    {
        me->pf_in = (float*)pffft_aligned_malloc(sizeof(float) * (size_t)me->nfft);
        me->pf_out = (float*)pffft_aligned_malloc(sizeof(float) * (size_t)me->nfft);
        me->pf_work = (float*)pffft_aligned_malloc(sizeof(float) * (size_t)me->nfft);
    }

    LOG(LOG_INFO, "N_FFT = %d\n", me->nfft);
    LOG(LOG_DEBUG, "FFT work area = %zu\n", fft_work_size);

#ifdef WATERFALL_USE_PHASE
    me->nifft = 64; // Gives 200 Hz sample rate for FT8 (160ms symbol period)

    size_t ifft_work_size = 0;
    kiss_fft_alloc(me->nifft, 1, 0, &ifft_work_size);
    me->ifft_work = malloc(ifft_work_size);
    me->ifft_cfg = kiss_fft_alloc(me->nifft, 1, me->ifft_work, &ifft_work_size);

    LOG(LOG_INFO, "N_iFFT = %d\n", me->nifft);
    LOG(LOG_DEBUG, "iFFT work area = %zu\n", ifft_work_size);
#endif

    // Allocate enough blocks to fit the entire FT8/FT4 slot in memory
    const int max_blocks = (int)(slot_time / symbol_period);
    // Keep only FFT bins in the specified frequency range (f_min/f_max)
    me->min_bin = (int)(cfg->f_min * symbol_period);
    me->max_bin = (int)(cfg->f_max * symbol_period) + 1;
    const int num_bins = me->max_bin - me->min_bin;

    waterfall_init(&me->wf, max_blocks, num_bins, cfg->time_osr, cfg->freq_osr);
    me->wf.protocol = cfg->protocol;

    me->symbol_period = symbol_period;

    me->max_mag = -120.0f;
    wf_quant_init(me);
}

void monitor_free(monitor_t* me)
{
    if (me->pf_setup)
    {
        pffft_destroy_setup((PFFFT_Setup*)me->pf_setup);
        pffft_aligned_free(me->pf_in);
        pffft_aligned_free(me->pf_out);
        pffft_aligned_free(me->pf_work);
    }
    waterfall_free(&me->wf);
    free(me->fft_work);
    free(me->last_frame);
    free(me->window);
}

void monitor_reset(monitor_t* me)
{
    me->wf.num_blocks = 0;
    me->max_mag = -120.0f;
}

// Compute FFT magnitudes (log wf) for a frame in the signal and update waterfall data
void monitor_process(monitor_t* me, const float* frame)
{
    // Check if we can still store more waterfall data
    if (me->wf.num_blocks >= me->wf.max_blocks)
        return;

    int offset = me->wf.num_blocks * me->wf.block_stride;
    int frame_pos = 0;

    // Loop over block subdivisions
    for (int time_sub = 0; time_sub < me->wf.time_osr; ++time_sub)
    {
        kiss_fft_scalar timedata[me->nfft];
        kiss_fft_cpx freqdata[me->nfft / 2 + 1];

        // Shift the new data into analysis frame (★ memmove/memcpy: the C library's are vectorised; GCC will not
        // vectorise a float loop on ARMv7 without -ffast-math — 2026-10-25)
        memmove(me->last_frame, me->last_frame + me->subblock_size, sizeof(float) * (size_t)(me->nfft - me->subblock_size));
        memcpy(me->last_frame + me->nfft - me->subblock_size, frame + frame_pos, sizeof(float) * (size_t)me->subblock_size);
        frame_pos += me->subblock_size;

        // Do DFT of windowed analysis frame
        if (me->pf_setup)
        {
            // ★ PFFFT's ordered real output: [0] = DC, [1] = Nyquist, then (re, im) of bins 1 .. nfft/2 - 1
            int pos = 0;
            for (; pos + 4 <= me->nfft; pos += 4)
                v4_store(me->pf_in + pos, v4_mul(v4_load(me->window + pos), v4_load(me->last_frame + pos)));
            for (; pos < me->nfft; ++pos)
                me->pf_in[pos] = me->window[pos] * me->last_frame[pos];
            pffft_transform_ordered((PFFFT_Setup*)me->pf_setup, me->pf_in, me->pf_out, me->pf_work, PFFFT_FORWARD);
            freqdata[0].r = me->pf_out[0]; freqdata[0].i = 0;
            freqdata[me->nfft / 2].r = me->pf_out[1]; freqdata[me->nfft / 2].i = 0;
            memcpy(&freqdata[1], me->pf_out + 2, sizeof(float) * (size_t)(me->nfft - 2));
        }
        else
        {
            for (int pos = 0; pos < me->nfft; ++pos)
            {
                timedata[pos] = me->window[pos] * me->last_frame[pos];
            }
            kiss_fftr(me->fft_cfg, timedata, freqdata);
        }

#ifdef WATERFALL_USE_PHASE
        // Loop over possible frequency OSR offsets
        for (int freq_sub = 0; freq_sub < me->wf.freq_osr; ++freq_sub)
        {
            for (int bin = me->min_bin; bin < me->max_bin; ++bin)
            {
                int src_bin = (bin * me->wf.freq_osr) + freq_sub;
                float mag2 = (freqdata[src_bin].i * freqdata[src_bin].i) + (freqdata[src_bin].r * freqdata[src_bin].r);
                float db = 10.0f * log10f(1E-12f + mag2);
                // Save the magnitude in dB and phase in radians
                float phase = atan2f(freqdata[src_bin].i, freqdata[src_bin].r);
                me->wf.mag[offset].mag = db;
                me->wf.mag[offset].phase = phase;
                ++offset;
                if (db > me->max_mag)
                    me->max_mag = db;
            }
        }
#else
        // Loop over possible frequency OSR offsets — the level quantised by table, not log10f (wf_quant_init)
        const float* spec = me->pf_setup ? me->pf_out : (const float*)freqdata;   // (re, im) pairs either way
        float peak = 0.0f;
        for (int freq_sub = 0; freq_sub < me->wf.freq_osr; ++freq_sub)
        {
            for (int bin = me->min_bin; bin < me->max_bin; ++bin)
            {
                int src_bin = (bin * me->wf.freq_osr) + freq_sub;
                const float re = spec[2 * src_bin], im = spec[2 * src_bin + 1];
                float mag2 = (im * im) + (re * re);
                const float x = 1E-12f + mag2;
                me->wf.mag[offset] = wf_quant(me, x);
                ++offset;
                if (x > peak)
                    peak = x;
            }
        }
        if (peak > 0.0f)
        {
            const float db = 10.0f * log10f(peak);   // ★ the debug statistic, once a frame: log10 is monotonic
            if (db > me->max_mag)
                me->max_mag = db;
        }
#endif
    }

    ++me->wf.num_blocks;
}

#ifdef WATERFALL_USE_PHASE
void monitor_resynth(const monitor_t* me, const candidate_t* candidate, float* signal)
{
    const int num_ifft = me->nifft;
    const int num_shift = num_ifft / 2;
    const int taper_width = 4;
    const int num_tones = 8;

    // Starting offset is 3 subblocks due to analysis buffer loading
    int offset = 1;                          // candidate->time_offset;
    offset = (offset * me->wf.time_osr) + 1; // + candidate->time_sub;
    offset = (offset * me->wf.freq_osr);     // + candidate->freq_sub;
    offset = (offset * me->wf.num_bins);     // + candidate->freq_offset;

    WF_ELEM_T* el = me->wf.mag + offset;

    // DFT frequency data - initialize to zero
    kiss_fft_cpx freqdata[num_ifft];
    for (int i = 0; i < num_ifft; ++i)
    {
        freqdata[i].r = 0;
        freqdata[i].i = 0;
    }

    int pos = 0;
    for (int num_block = 1; num_block < me->wf.num_blocks; ++num_block)
    {
        // Extract frequency data around the selected candidate only
        for (int i = candidate->freq_offset - taper_width - 1; i < candidate->freq_offset + 8 + taper_width - 1; ++i)
        {
            if ((i >= 0) && (i < me->wf.num_bins))
            {
                int tgt_bin = (me->wf.freq_osr * (i - candidate->freq_offset) + num_ifft) % num_ifft;
                float weight = 1.0f;
                if (i < candidate->freq_offset)
                {
                    weight = ((i - candidate->freq_offset) + taper_width) / (float)taper_width;
                }
                else if (i > candidate->freq_offset + 7)
                {
                    weight = ((candidate->freq_offset + 7 - i) + taper_width) / (float)taper_width;
                }

                // Convert (dB magnitude, phase) to (real, imaginary)
                float mag = powf(10.0f, el[i].mag / 20) / 2 * weight;
                freqdata[tgt_bin].r = mag * cosf(el[i].phase);
                freqdata[tgt_bin].i = mag * sinf(el[i].phase);

                int i2 = i + me->wf.num_bins;
                tgt_bin = (tgt_bin + 1) % num_ifft;
                float mag2 = powf(10.0f, el[i2].mag / 20) / 2 * weight;
                freqdata[tgt_bin].r = mag2 * cosf(el[i2].phase);
                freqdata[tgt_bin].i = mag2 * sinf(el[i2].phase);
            }
        }

        // Compute inverse DFT and overlap-add the waveform
        kiss_fft_cpx timedata[num_ifft];
        kiss_fft(me->ifft_cfg, freqdata, timedata);
        for (int i = 0; i < num_ifft; ++i)
        {
            signal[pos + i] += timedata[i].i;
        }

        // Move to the next symbol
        el += me->wf.block_stride;
        pos += num_shift;
    }
}
#endif
