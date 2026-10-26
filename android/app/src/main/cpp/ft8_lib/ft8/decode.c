#include "decode.h"
#include "constants.h"
#include "crc.h"
#include "ldpc.h"
#include "vibe_simd4.h"

#include <stdbool.h>
#include <math.h>

// #define LOG_LEVEL LOG_DEBUG
// #include "debug.h"

// Lookup table for y = 10*log10(1 + 10^(x/10)), where
//   y - increase in signal level dB when adding a weaker independent signal
//   x - specific relative strength of the weaker signal in dB
// Table index corresponds to x in dB (index 0: 0 dB, index 1: -1 dB etc)
static const float db_power_sum[40] = {
    3.01029995663981f, 2.53901891043867f, 2.1244260279434f, 1.76434862436485f, 1.45540463109294f,
    1.19331048066095f, 0.973227937086954f, 0.790097496525665f, 0.638920341433796f, 0.514969420252302f,
    0.413926851582251f, 0.331956199884278f, 0.265723755961025f, 0.212384019142551f, 0.16954289279533f,
    0.135209221080382f, 0.10774225511957f, 0.085799992300358f, 0.06829128312453f, 0.054333142200458f,
    0.043213737826426f, 0.034360947517284f, 0.027316043349389f, 0.021711921641451f, 0.017255250287928f,
    0.013711928326833f, 0.010895305999614f, 0.008656680827934f, 0.006877654943187f, 0.005464004928574f,
    0.004340774793186f, 0.003448354310253f, 0.002739348814965f, 0.002176083232619f, 0.001728613409904f,
    0.001373142636584f, 0.001090761428665f, 0.000866444976964f, 0.000688255828734f, 0.000546709946839f
};

/// Compute log likelihood log(p(1) / p(0)) of 174 message bits for later use in soft-decision LDPC decoding
/// @param[in] wf Waterfall data collected during message slot
/// @param[in] cand Candidate to extract the message from
/// @param[in] code_map Symbol encoding map
/// @param[out] log174 Output of decoded log likelihoods for each of the 174 message bits
static void ft4_extract_likelihood(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, float* log174);
static void ft8_extract_likelihood(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, float* log174);

/// Packs a string of bits each represented as a zero/non-zero byte in bit_array[],
/// as a string of packed bits starting from the MSB of the first byte of packed[]
/// @param[in] plain Array of bits (0 and nonzero values) with num_bits entires
/// @param[in] num_bits Number of bits (entries) passed in bit_array
/// @param[out] packed Byte-packed bits representing the data in bit_array
static void pack_bits(const uint8_t bit_array[], int num_bits, uint8_t packed[]);

static float max2(float a, float b);
static float max4(float a, float b, float c, float d);
static void heapify_down(ftx_candidate_t heap[], int heap_size);
static void heapify_up(ftx_candidate_t heap[], int heap_size);
static void heap_offer(ftx_candidate_t heap[], int* heap_size, int num_candidates, const ftx_candidate_t* candidate);

static void ftx_normalize_logl(float* log174);
static void ft4_extract_symbol(const WF_ELEM_T* wf, float* logl);
static void ft8_extract_symbol(const WF_ELEM_T* wf, float* logl);
static void ft8_decode_multi_symbols(const WF_ELEM_T* wf, int num_bins, int n_syms, int bit_idx, float* log174);

// VibeSDR (2026-10-26, FT2): the search's time range — ft8_lib's -10 .. 20 unless the waterfall sets its own.
static int wf_time_min(const ftx_waterfall_t* wf) { return (wf->time_min || wf->time_max) ? wf->time_min : -10; }
static int wf_time_max(const ftx_waterfall_t* wf) { return (wf->time_min || wf->time_max) ? wf->time_max : 20; }

static const WF_ELEM_T* get_cand_mag(const ftx_waterfall_t* wf, const ftx_candidate_t* candidate)
{
    int offset = candidate->time_offset;
    offset = (offset * wf->time_osr) + candidate->time_sub;
    offset = (offset * wf->freq_osr) + candidate->freq_sub;
    offset = (offset * wf->num_bins) + candidate->freq_offset;
    return wf->mag + offset;
}

static int ft8_sync_score(const ftx_waterfall_t* wf, const ftx_candidate_t* candidate)
{
    int score = 0;
    int num_average = 0;

    // Get the pointer to symbol 0 of the candidate
    const WF_ELEM_T* mag_cand = get_cand_mag(wf, candidate);

    // Compute average score over sync symbols (m+k = 0-7, 36-43, 72-79)
    for (int m = 0; m < FT8_NUM_SYNC; ++m)
    {
        for (int k = 0; k < FT8_LENGTH_SYNC; ++k)
        {
            int block = (FT8_SYNC_OFFSET * m) + k;          // relative to the message
            int block_abs = candidate->time_offset + block; // relative to the captured signal
            // Check for time boundaries
            if (block_abs < 0)
                continue;
            if (block_abs >= wf->num_blocks)
                break;

            // Get the pointer to symbol 'block' of the candidate
            const WF_ELEM_T* p8 = mag_cand + (block * wf->block_stride);

            // Weighted difference between the expected and all other symbols
            // Does not work as well as the alternative score below
            // score += 8 * p8[kFT8_Costas_pattern[k]] -
            //          p8[0] - p8[1] - p8[2] - p8[3] -
            //          p8[4] - p8[5] - p8[6] - p8[7];
            // ++num_average;

            // Check only the neighbors of the expected symbol frequency- and time-wise
            int sm = kFT8_Costas_pattern[k]; // Index of the expected bin
            if (sm > 0)
            {
                // look at one frequency bin lower
                score += WF_ELEM_MAG_INT(p8[sm]) - WF_ELEM_MAG_INT(p8[sm - 1]);
                ++num_average;
            }
            if (sm < 7)
            {
                // look at one frequency bin higher
                score += WF_ELEM_MAG_INT(p8[sm]) - WF_ELEM_MAG_INT(p8[sm + 1]);
                ++num_average;
            }
            if ((k > 0) && (block_abs > 0))
            {
                // look one symbol back in time
                score += WF_ELEM_MAG_INT(p8[sm]) - WF_ELEM_MAG_INT(p8[sm - wf->block_stride]);
                ++num_average;
            }
            if (((k + 1) < FT8_LENGTH_SYNC) && ((block_abs + 1) < wf->num_blocks))
            {
                // look one symbol forward in time
                score += WF_ELEM_MAG_INT(p8[sm]) - WF_ELEM_MAG_INT(p8[sm + wf->block_stride]);
                ++num_average;
            }
        }
    }

    if (num_average > 0)
        score /= num_average;

    return score;
}

static int ft4_sync_score(const ftx_waterfall_t* wf, const ftx_candidate_t* candidate)
{
    int score = 0;
    int num_average = 0;

    // Get the pointer to symbol 0 of the candidate
    const WF_ELEM_T* mag_cand = get_cand_mag(wf, candidate);

    // Compute average score over sync symbols (block = 1-4, 34-37, 67-70, 100-103)
    for (int m = 0; m < FT4_NUM_SYNC; ++m)
    {
        for (int k = 0; k < FT4_LENGTH_SYNC; ++k)
        {
            int block = 1 + (FT4_SYNC_OFFSET * m) + k;
            int block_abs = candidate->time_offset + block;
            // Check for time boundaries
            if (block_abs < 0)
                continue;
            if (block_abs >= wf->num_blocks)
                break;

            // Get the pointer to symbol 'block' of the candidate
            const WF_ELEM_T* p4 = mag_cand + (block * wf->block_stride);

            int sm = kFT4_Costas_pattern[m][k]; // Index of the expected bin

            // score += (4 * p4[sm]) - p4[0] - p4[1] - p4[2] - p4[3];
            // num_average += 4;

            // Check only the neighbors of the expected symbol frequency- and time-wise
            if (sm > 0)
            {
                // look at one frequency bin lower
                score += WF_ELEM_MAG_INT(p4[sm]) - WF_ELEM_MAG_INT(p4[sm - 1]);
                ++num_average;
            }
            if (sm < 3)
            {
                // look at one frequency bin higher
                score += WF_ELEM_MAG_INT(p4[sm]) - WF_ELEM_MAG_INT(p4[sm + 1]);
                ++num_average;
            }
            if ((k > 0) && (block_abs > 0))
            {
                // look one symbol back in time
                score += WF_ELEM_MAG_INT(p4[sm]) - WF_ELEM_MAG_INT(p4[sm - wf->block_stride]);
                ++num_average;
            }
            if (((k + 1) < FT4_LENGTH_SYNC) && ((block_abs + 1) < wf->num_blocks))
            {
                // look one symbol forward in time
                score += WF_ELEM_MAG_INT(p4[sm]) - WF_ELEM_MAG_INT(p4[sm + wf->block_stride]);
                ++num_average;
            }
        }
    }

    if (num_average > 0)
        score /= num_average;

    return score;
}

/* ★★ THE SYNC SEARCH, SIXTEEN FREQUENCIES AT ONCE (VibeSDR, 2026-10-25 — the FT8 CPU work; not in upstream ft8_lib).
 *  ft8_sync_score / ft4_sync_score above are the reference: for one candidate, the sum over its Costas symbols of
 *  (expected tone − a neighbour) in the uint8 waterfall, divided by the number of terms. WHICH terms are summed
 *  depends only on the candidate's time (the Costas pattern and the slot's edges), never on its frequency — and the
 *  waterfall stores neighbouring frequencies in neighbouring bytes. So for one time position the terms are listed
 *  once (as pairs of byte offsets), and sixteen consecutive frequency offsets are scored together: sixteen byte loads
 *  per term, widened to int16 (at most 84 terms of ±255 — the sum fits). Integer sums are exact in any order, and the
 *  division and the heap see the candidates in the original order, so the candidate list is the same, bit for bit
 *  (test-ft8-fast). The search was ~1.6 ms of every FT8 pass on an M5, the same on every pass. */
typedef struct
{
    int32_t a, b;    // byte offsets from the candidate's symbol-0 pointer: score += mag[a] - mag[b]
} sync_term_t;

static int sync_terms(const ftx_waterfall_t* wf, int time_offset, sync_term_t* terms)
{
    int n = 0;
    const int bs = wf->block_stride;
    if (wf->protocol == FTX_PROTOCOL_FT4)
    {
        for (int m = 0; m < FT4_NUM_SYNC; ++m)
            for (int k = 0; k < FT4_LENGTH_SYNC; ++k)
            {
                int block = 1 + (FT4_SYNC_OFFSET * m) + k;
                int block_abs = time_offset + block;
                if (block_abs < 0)
                    continue;
                if (block_abs >= wf->num_blocks)
                    break;
                const int base = block * bs, sm = kFT4_Costas_pattern[m][k];
                if (sm > 0) { terms[n].a = base + sm; terms[n].b = base + sm - 1; ++n; }
                if (sm < 3) { terms[n].a = base + sm; terms[n].b = base + sm + 1; ++n; }
                if ((k > 0) && (block_abs > 0)) { terms[n].a = base + sm; terms[n].b = base + sm - bs; ++n; }
                if (((k + 1) < FT4_LENGTH_SYNC) && ((block_abs + 1) < wf->num_blocks)) { terms[n].a = base + sm; terms[n].b = base + sm + bs; ++n; }
            }
    }
    else
    {
        for (int m = 0; m < FT8_NUM_SYNC; ++m)
            for (int k = 0; k < FT8_LENGTH_SYNC; ++k)
            {
                int block = (FT8_SYNC_OFFSET * m) + k;
                int block_abs = time_offset + block;
                if (block_abs < 0)
                    continue;
                if (block_abs >= wf->num_blocks)
                    break;
                const int base = block * bs, sm = kFT8_Costas_pattern[k];
                if (sm > 0) { terms[n].a = base + sm; terms[n].b = base + sm - 1; ++n; }
                if (sm < 7) { terms[n].a = base + sm; terms[n].b = base + sm + 1; ++n; }
                if ((k > 0) && (block_abs > 0)) { terms[n].a = base + sm; terms[n].b = base + sm - bs; ++n; }
                if (((k + 1) < FT8_LENGTH_SYNC) && ((block_abs + 1) < wf->num_blocks)) { terms[n].a = base + sm; terms[n].b = base + sm + bs; ++n; }
            }
    }
    return n;
}

/** The raw sums (before the division) for 16 consecutive frequency offsets starting at p. */
static void sync_sum16(const uint8_t* p, const sync_term_t* terms, int n, int16_t out[16])
{
#if VIBE_FT8_NEON
    int16x8_t lo = vdupq_n_s16(0), hi = vdupq_n_s16(0);
    for (int i = 0; i < n; ++i)
    {
        const uint8x16_t a = vld1q_u8(p + terms[i].a), b = vld1q_u8(p + terms[i].b);
        lo = vaddq_s16(lo, vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(a), vget_low_u8(b))));
        hi = vaddq_s16(hi, vreinterpretq_s16_u16(vsubl_u8(vget_high_u8(a), vget_high_u8(b))));
    }
    vst1q_s16(out, lo);
    vst1q_s16(out + 8, hi);
#elif VIBE_FT8_SSE2
    const __m128i z = _mm_setzero_si128();
    __m128i lo = z, hi = z;
    for (int i = 0; i < n; ++i)
    {
        const __m128i a = _mm_loadu_si128((const __m128i*)(p + terms[i].a));
        const __m128i b = _mm_loadu_si128((const __m128i*)(p + terms[i].b));
        lo = _mm_add_epi16(lo, _mm_sub_epi16(_mm_unpacklo_epi8(a, z), _mm_unpacklo_epi8(b, z)));
        hi = _mm_add_epi16(hi, _mm_sub_epi16(_mm_unpackhi_epi8(a, z), _mm_unpackhi_epi8(b, z)));
    }
    _mm_storeu_si128((__m128i*)out, lo);
    _mm_storeu_si128((__m128i*)(out + 8), hi);
#else
    int acc[16] = { 0 };
    for (int i = 0; i < n; ++i)
        for (int l = 0; l < 16; ++l)
            acc[l] += (int)p[terms[i].a + l] - (int)p[terms[i].b + l];
    for (int l = 0; l < 16; ++l)
        out[l] = (int16_t)acc[l];
#endif
}

static void heap_offer(ftx_candidate_t heap[], int* heap_size, int num_candidates, const ftx_candidate_t* candidate)
{
    // If the heap is full AND the current candidate is better than
    // the worst in the heap, we remove the worst and make space
    if ((*heap_size == num_candidates) && (candidate->score > heap[0].score))
    {
        --*heap_size;
        heap[0] = heap[*heap_size];
        heapify_down(heap, *heap_size);
    }

    // If there's free space in the heap, we add the current candidate
    if (*heap_size < num_candidates)
    {
        heap[*heap_size] = *candidate;
        ++*heap_size;
        heapify_up(heap, *heap_size);
    }
}

int ftx_find_candidates(const ftx_waterfall_t* wf, int num_candidates, ftx_candidate_t heap[], int min_score)
{
    int num_tones = (wf->protocol == FTX_PROTOCOL_FT4) ? 4 : 8;

    int heap_size = 0;
    ftx_candidate_t candidate;
    sync_term_t terms[FT4_NUM_SYNC * FT4_LENGTH_SYNC * 4 > FT8_NUM_SYNC * FT8_LENGTH_SYNC * 4 ? FT4_NUM_SYNC * FT4_LENGTH_SYNC * 4 : FT8_NUM_SYNC * FT8_LENGTH_SYNC * 4];

    // Here we allow time offsets that exceed signal boundaries, as long as we still have all data bits.
    // I.e. we can afford to skip the first 7 or the last 7 Costas symbols, as long as we track how many
    // sync symbols we included in the score, so the score is averaged.
    for (candidate.time_sub = 0; candidate.time_sub < wf->time_osr; ++candidate.time_sub)
    {
        for (candidate.freq_sub = 0; candidate.freq_sub < wf->freq_osr; ++candidate.freq_sub)
        {
            for (candidate.time_offset = wf_time_min(wf); candidate.time_offset < wf_time_max(wf); ++candidate.time_offset)
            {
                const int nt = sync_terms(wf, candidate.time_offset, terms);
                candidate.freq_offset = 0;
                const uint8_t* p0 = get_cand_mag(wf, &candidate);   // symbol 0, frequency offset 0
                const int f_end = wf->num_bins - num_tones + 1;     // freq_offset + num_tones - 1 < num_bins
                for (int f = 0; f < f_end; f += 16)
                {
                    int16_t raw[16];
                    const int lanes = (f_end - f < 16) ? f_end - f : 16;
                    if (lanes == 16)
                        sync_sum16(p0 + f, terms, nt, raw);
                    else
                        for (int l = 0; l < lanes; ++l)
                        {
                            int acc = 0;
                            for (int i = 0; i < nt; ++i)
                                acc += (int)p0[f + l + terms[i].a] - (int)p0[f + l + terms[i].b];
                            raw[l] = (int16_t)acc;
                        }
                    // ★ raw / nt < min_score exactly when raw < min_score · nt (min_score > 0, nt > 0, C's division
                    //   truncating toward zero): most frequencies stop there, before a divide — ARMv7-A has no
                    //   divide instruction, and the library call was 1 % of a whole decode on the Pi 2 build.
                    const int skip_below = (min_score > 0 && nt > 0) ? min_score * nt : -0x7fffffff;
                    for (int l = 0; l < lanes; ++l)
                    {
                        if (raw[l] < skip_below)
                            continue;
                        candidate.freq_offset = (int16_t)(f + l);
                        candidate.score = (int16_t)(nt > 0 ? raw[l] / nt : raw[l]);
                        if (candidate.score < min_score)
                            continue;
                        heap_offer(heap, &heap_size, num_candidates, &candidate);
                    }
                }
            }
        }
    }

    // Sort the candidates by sync strength - here we benefit from the heap structure
    int len_unsorted = heap_size;
    while (len_unsorted > 1)
    {
        // Take the top (index 0) element which is guaranteed to have the smallest score,
        // exchange it with the last element in the heap, and decrease the heap size.
        // Then restore the heap property in the new, smaller heap.
        // At the end the elements will be sorted in descending order.
        ftx_candidate_t tmp = heap[len_unsorted - 1];
        heap[len_unsorted - 1] = heap[0];
        heap[0] = tmp;
        len_unsorted--;
        heapify_down(heap, len_unsorted);
    }

    return heap_size;
}

/// The original search, one candidate at a time (ft8_lib's): the reference ftx_find_candidates is held to.
int ftx_find_candidates_ref(const ftx_waterfall_t* wf, int num_candidates, ftx_candidate_t heap[], int min_score)
{
    int (*sync_fun)(const ftx_waterfall_t*, const ftx_candidate_t*) = (wf->protocol == FTX_PROTOCOL_FT4) ? ft4_sync_score : ft8_sync_score;
    int num_tones = (wf->protocol == FTX_PROTOCOL_FT4) ? 4 : 8;
    int heap_size = 0;
    ftx_candidate_t candidate;
    for (candidate.time_sub = 0; candidate.time_sub < wf->time_osr; ++candidate.time_sub)
        for (candidate.freq_sub = 0; candidate.freq_sub < wf->freq_osr; ++candidate.freq_sub)
            for (candidate.time_offset = wf_time_min(wf); candidate.time_offset < wf_time_max(wf); ++candidate.time_offset)
                for (candidate.freq_offset = 0; (candidate.freq_offset + num_tones - 1) < wf->num_bins; ++candidate.freq_offset)
                {
                    candidate.score = sync_fun(wf, &candidate);
                    if (candidate.score < min_score)
                        continue;
                    heap_offer(heap, &heap_size, num_candidates, &candidate);
                }
    int len_unsorted = heap_size;
    while (len_unsorted > 1)
    {
        ftx_candidate_t tmp = heap[len_unsorted - 1];
        heap[len_unsorted - 1] = heap[0];
        heap[0] = tmp;
        len_unsorted--;
        heapify_down(heap, len_unsorted);
    }
    return heap_size;
}

static void ft4_extract_likelihood(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, float* log174)
{
    const WF_ELEM_T* mag = get_cand_mag(wf, cand); // Pointer to 4 magnitude bins of the first symbol

    // Go over FSK tones and skip Costas sync symbols
    for (int k = 0; k < FT4_ND; ++k)
    {
        // Skip either 5, 9 or 13 sync symbols
        // TODO: replace magic numbers with constants
        int sym_idx = k + ((k < 29) ? 5 : ((k < 58) ? 9 : 13));
        int bit_idx = 2 * k;

        // Check for time boundaries
        int block = cand->time_offset + sym_idx;
        if ((block < 0) || (block >= wf->num_blocks))
        {
            log174[bit_idx + 0] = 0;
            log174[bit_idx + 1] = 0;
        }
        else
        {
            ft4_extract_symbol(mag + (sym_idx * wf->block_stride), log174 + bit_idx);
        }
    }
}

static void ft8_extract_likelihood(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, float* log174)
{
    const WF_ELEM_T* mag = get_cand_mag(wf, cand); // Pointer to 8 magnitude bins of the first symbol

    // Go over FSK tones and skip Costas sync symbols
    for (int k = 0; k < FT8_ND; ++k)
    {
        // Skip either 7 or 14 sync symbols
        // TODO: replace magic numbers with constants
        int sym_idx = k + ((k < 29) ? 7 : 14);
        int bit_idx = 3 * k;

        // Check for time boundaries
        int block = cand->time_offset + sym_idx;
        if ((block < 0) || (block >= wf->num_blocks))
        {
            log174[bit_idx + 0] = 0;
            log174[bit_idx + 1] = 0;
            log174[bit_idx + 2] = 0;
        }
        else
        {
            ft8_extract_symbol(mag + (sym_idx * wf->block_stride), log174 + bit_idx);
        }
    }
}

static void ftx_normalize_logl(float* log174)
{
    // Compute the variance of log174
    float sum = 0;
    float sum2 = 0;
    for (int i = 0; i < FTX_LDPC_N; ++i)
    {
        sum += log174[i];
        sum2 += log174[i] * log174[i];
    }
    float inv_n = 1.0f / FTX_LDPC_N;
    float variance = (sum2 - (sum * sum * inv_n)) * inv_n;

    // Normalize log174 distribution and scale it with experimentally found coefficient
    float norm_factor = sqrtf(24.0f / variance);
    for (int i = 0; i < FTX_LDPC_N; ++i)
    {
        log174[i] *= norm_factor;
    }
}

bool ftx_decode_candidate(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, int max_iterations, ftx_message_t* message, ftx_decode_status_t* status)
{
    float log174[FTX_LDPC_N]; // message bits encoded as likelihood
    if (wf->protocol == FTX_PROTOCOL_FT4)
    {
        ft4_extract_likelihood(wf, cand, log174);
    }
    else
    {
        ft8_extract_likelihood(wf, cand, log174);
    }

    ftx_normalize_logl(log174);

    uint8_t plain174[FTX_LDPC_N]; // message bits (0/1)
    bp_decode(log174, max_iterations, plain174, &status->ldpc_errors);
    // ldpc_decode(log174, max_iterations, plain174, &status->ldpc_errors);

    if (status->ldpc_errors > 0)
    {
        return false;
    }

    // Extract payload + CRC (first FTX_LDPC_K bits) packed into a byte array
    uint8_t a91[FTX_LDPC_K_BYTES];
    pack_bits(plain174, FTX_LDPC_K, a91);

    // Extract CRC and check it
    status->crc_extracted = ftx_extract_crc(a91);
    // [1]: 'The CRC is calculated on the source-encoded message, zero-extended from 77 to 82 bits.'
    a91[9] &= 0xF8;
    a91[10] &= 0x00;
    status->crc_calculated = ftx_compute_crc(a91, 96 - 14);

    if (status->crc_extracted != status->crc_calculated)
    {
        return false;
    }

    // Reuse CRC value as a hash for the message (TODO: 14 bits only, should perhaps use full 16 or 32 bits?)
    message->hash = status->crc_calculated;

    if (wf->protocol == FTX_PROTOCOL_FT4)
    {
        // '[..] for FT4 only, in order to avoid transmitting a long string of zeros when sending CQ messages,
        // the assembled 77-bit message is bitwise exclusive-OR’ed with [a] pseudorandom sequence before computing the CRC and FEC parity bits'
        for (int i = 0; i < 10; ++i)
        {
            message->payload[i] = a91[i] ^ kFT4_XOR_sequence[i];
        }
    }
    else
    {
        for (int i = 0; i < 10; ++i)
        {
            message->payload[i] = a91[i];
        }
    }

    // LOG(LOG_DEBUG, "Decoded message (CRC %04x), trying to unpack...\n", status->crc_extracted);
    return true;
}

static float max2(float a, float b)
{
    return (a >= b) ? a : b;
}

static float max4(float a, float b, float c, float d)
{
    return max2(max2(a, b), max2(c, d));
}

static void heapify_down(ftx_candidate_t heap[], int heap_size)
{
    // heapify from the root down
    int current = 0; // root node
    while (true)
    {
        int left = 2 * current + 1;
        int right = left + 1;

        // Find the smallest value of (parent, left child, right child)
        int smallest = current;
        if ((left < heap_size) && (heap[left].score < heap[smallest].score))
        {
            smallest = left;
        }
        if ((right < heap_size) && (heap[right].score < heap[smallest].score))
        {
            smallest = right;
        }

        if (smallest == current)
        {
            break;
        }

        // Exchange the current node with the smallest child and move down to it
        ftx_candidate_t tmp = heap[smallest];
        heap[smallest] = heap[current];
        heap[current] = tmp;
        current = smallest;
    }
}

static void heapify_up(ftx_candidate_t heap[], int heap_size)
{
    // heapify from the last node up
    int current = heap_size - 1;
    while (current > 0)
    {
        int parent = (current - 1) / 2;
        if (!(heap[current].score < heap[parent].score))
        {
            break;
        }

        // Exchange the current node with its parent and move up
        ftx_candidate_t tmp = heap[parent];
        heap[parent] = heap[current];
        heap[current] = tmp;
        current = parent;
    }
}

// Compute unnormalized log likelihood log(p(1) / p(0)) of 2 message bits (1 FSK symbol)
static void ft4_extract_symbol(const WF_ELEM_T* wf, float* logl)
{
    // Cleaned up code for the simple case of n_syms==1
    float s2[4];

    for (int j = 0; j < 4; ++j)
    {
        s2[j] = WF_ELEM_MAG(wf[kFT4_Gray_map[j]]);
    }

    logl[0] = max2(s2[2], s2[3]) - max2(s2[0], s2[1]);
    logl[1] = max2(s2[1], s2[3]) - max2(s2[0], s2[2]);
}

// Compute unnormalized log likelihood log(p(1) / p(0)) of 3 message bits (1 FSK symbol)
static void ft8_extract_symbol(const WF_ELEM_T* wf, float* logl)
{
    // Cleaned up code for the simple case of n_syms==1
#if 1
    float s2[8];

    for (int j = 0; j < 8; ++j)
    {
        s2[j] = WF_ELEM_MAG(wf[kFT8_Gray_map[j]]);
    }

    logl[0] = max4(s2[4], s2[5], s2[6], s2[7]) - max4(s2[0], s2[1], s2[2], s2[3]);
    logl[1] = max4(s2[2], s2[3], s2[6], s2[7]) - max4(s2[0], s2[1], s2[4], s2[5]);
    logl[2] = max4(s2[1], s2[3], s2[5], s2[7]) - max4(s2[0], s2[2], s2[4], s2[6]);
#else
    float a[7] = {
        // (float)wf[7] - (float)wf[0], // 0: p(111) / p(000)
        (float)wf[5] - (float)wf[2], // 0: p(100) / p(011)
        (float)wf[3] - (float)wf[0], // 1: p(010) / p(000)
        (float)wf[6] - (float)wf[3], // 2: p(101) / p(010)
        (float)wf[6] - (float)wf[2], // 3: p(101) / p(011)
        (float)wf[7] - (float)wf[4], // 4: p(111) / p(110)
        (float)wf[4] - (float)wf[1], // 5: p(110) / p(001)
        (float)wf[5] - (float)wf[1]  // 6: p(100) / p(001)
    };
    float k = 1.0f;

    // logl[0] = k * (a[0] + a[2] + a[3] + a[5] + a[6]) / 5;
    // logl[1] = k * (a[0] / 4 + (a[1] - a[3]) * 5 / 24 + (a[5] - a[2]) / 6 + (a[4] - a[6]) / 24);
    // logl[2] = k * (a[0] / 4 + (a[1] - a[3]) / 24 + (a[2] - a[5]) / 6 + (a[4] - a[6]) * 5 / 24);
    logl[0] = k * (a[1] / 6 + a[2] / 3 + a[3] / 6 + a[4] / 6 + a[5] / 3 + a[6] / 6);
    logl[1] = k * (-a[0] / 4 + a[1] * 7 / 24 + (a[4] - a[3]) / 8 + a[5] / 3 + a[6] / 24);
    logl[2] = k * (-a[0] / 4 + (a[1] - a[6]) / 8 + a[2] / 3 + a[3] / 24 + a[4] * 7 / 24 - a[5] * 5 / 18);
#endif
    // for (int i = 0; i < 8; ++i)
    //     printf("%d ", WF_ELEM_MAG_INT(wf[i]));
    // for (int i = 0; i < 3; ++i)
    //     printf("%.1f ", logl[i]);
    // printf("\n");
}

// Compute unnormalized log likelihood log(p(1) / p(0)) of bits corresponding to several FSK symbols at once
static void ft8_decode_multi_symbols(const WF_ELEM_T* wf, int num_bins, int n_syms, int bit_idx, float* log174)
{
    const int n_bits = 3 * n_syms;
    const int n_tones = (1 << n_bits);

    float s2[n_tones];

    for (int j = 0; j < n_tones; ++j)
    {
        int j1 = j & 0x07;
        if (n_syms == 1)
        {
            s2[j] = WF_ELEM_MAG(wf[kFT8_Gray_map[j1]]);
            continue;
        }
        int j2 = (j >> 3) & 0x07;
        if (n_syms == 2)
        {
            s2[j] = WF_ELEM_MAG(wf[kFT8_Gray_map[j2]]);
            s2[j] += WF_ELEM_MAG(wf[kFT8_Gray_map[j1] + 4 * num_bins]);
            continue;
        }
        int j3 = (j >> 6) & 0x07;
        s2[j] = WF_ELEM_MAG(wf[kFT8_Gray_map[j3]]);
        s2[j] += WF_ELEM_MAG(wf[kFT8_Gray_map[j2] + 4 * num_bins]);
        s2[j] += WF_ELEM_MAG(wf[kFT8_Gray_map[j1] + 8 * num_bins]);
    }

    // Extract bit significance (and convert them to float)
    // 8 FSK tones = 3 bits
    for (int i = 0; i < n_bits; ++i)
    {
        if (bit_idx + i >= FTX_LDPC_N)
        {
            // Respect array size
            break;
        }

        uint16_t mask = (n_tones >> (i + 1));
        float max_zero = -1000, max_one = -1000;
        for (int n = 0; n < n_tones; ++n)
        {
            if (n & mask)
            {
                max_one = max2(max_one, s2[n]);
            }
            else
            {
                max_zero = max2(max_zero, s2[n]);
            }
        }

        log174[bit_idx + i] = max_one - max_zero;
    }
}

// Packs a string of bits each represented as a zero/non-zero byte in plain[],
// as a string of packed bits starting from the MSB of the first byte of packed[]
static void pack_bits(const uint8_t bit_array[], int num_bits, uint8_t packed[])
{
    int num_bytes = (num_bits + 7) / 8;
    for (int i = 0; i < num_bytes; ++i)
    {
        packed[i] = 0;
    }

    uint8_t mask = 0x80;
    int byte_idx = 0;
    for (int i = 0; i < num_bits; ++i)
    {
        if (bit_array[i])
        {
            packed[byte_idx] |= mask;
        }
        mask >>= 1;
        if (!mask)
        {
            mask = 0x80;
            ++byte_idx;
        }
    }
}

// VibeSDR local change (2026-10-05): the candidate's normalised bit log-likelihoods, exactly as
// ftx_decode_candidate hands them to the LDPC decoder — so a decoder whose BP fails can try OSD on
// the same evidence (decoders/ft8_decoder.cpp). Not in upstream ft8_lib.
void ftx_candidate_log174(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, float* log174)
{
    if (wf->protocol == FTX_PROTOCOL_FT4)
        ft4_extract_likelihood(wf, cand, log174);
    else
        ft8_extract_likelihood(wf, cand, log174);
    ftx_normalize_logl(log174);
}
