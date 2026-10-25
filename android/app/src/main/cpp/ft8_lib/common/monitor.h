#ifndef _INCLUDE_MONITOR_H_
#define _INCLUDE_MONITOR_H_

#ifdef __cplusplus
extern "C"
{
#endif

#include <ft8/decode.h>
#include <fft/kiss_fftr.h>

/// Configuration options for FT4/FT8 monitor
typedef struct
{
    float f_min;             ///< Lower frequency bound for analysis
    float f_max;             ///< Upper frequency bound for analysis
    int sample_rate;         ///< Sample rate in Hertz
    int time_osr;            ///< Number of time subdivisions
    int freq_osr;            ///< Number of frequency subdivisions
    ftx_protocol_t protocol; ///< Protocol: FT4 or FT8
} monitor_config_t;

/// FT4/FT8 monitor object that manages DSP processing of incoming audio data
/// and prepares a waterfall object
typedef struct
{
    float symbol_period; ///< FT4/FT8 symbol period in seconds
    int min_bin;         ///< First FFT bin in the frequency range (begin)
    int max_bin;         ///< First FFT bin outside the frequency range (end)
    int block_size;      ///< Number of samples per symbol (block)
    int subblock_size;   ///< Analysis shift size (number of samples)
    int nfft;            ///< FFT size
    float fft_norm;      ///< FFT normalization factor
    float* window;       ///< Window function for STFT analysis (nfft samples)
    float* last_frame;   ///< Current STFT analysis frame (nfft samples)
    ftx_waterfall_t wf;  ///< Waterfall object
    float max_mag;       ///< Maximum detected magnitude (debug stats)

    // KISS FFT housekeeping variables
    void* fft_work;        ///< Work area required by Kiss FFT
    kiss_fftr_cfg fft_cfg; ///< Kiss FFT housekeeping object
    // VibeSDR (2026-10-25): the STFT through PFFFT (NEON/SSE) when the FFT size allows it; Kiss FFT otherwise
    void* pf_setup;        ///< PFFFT_Setup*, or NULL = Kiss FFT
    float* pf_in;          ///< aligned nfft floats: the windowed frame
    float* pf_out;         ///< aligned nfft floats: the ordered spectrum
    float* pf_work;        ///< aligned nfft floats: PFFFT's scratch
    // VibeSDR (2026-10-25): the waterfall level by table (monitor.c, wf_quant_init) — no log10f per bin
    float wf_thr[256];     ///< wf_thr[k] = the smallest power that stores as k
    uint8_t wf_cell[4096]; ///< the stored level at the bottom of each (exponent, top 4 mantissa bits) cell
#ifdef WATERFALL_USE_PHASE
    int nifft;             ///< iFFT size
    void* ifft_work;       ///< Work area required by inverse Kiss FFT
    kiss_fft_cfg ifft_cfg; ///< Inverse Kiss FFT housekeeping object
#endif
} monitor_t;

void monitor_init(monitor_t* me, const monitor_config_t* cfg);
void monitor_reset(monitor_t* me);
void monitor_process(monitor_t* me, const float* frame);
void monitor_free(monitor_t* me);
/// VibeSDR (2026-10-25): a bin power's stored level — by table (what monitor_process uses) and by log10f (the
/// reference). For test-ft8-fast.
uint8_t monitor_level(const monitor_t* me, float x);
uint8_t monitor_level_ref(float x);

#ifdef WATERFALL_USE_PHASE
void monitor_resynth(const monitor_t* me, const candidate_t* candidate, float* signal);
#endif

#ifdef __cplusplus
}
#endif

#endif // _INCLUDE_MONITOR_H_