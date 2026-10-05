// VibeSDR V4 — FT8 / FT4 decoder wrapper around ft8_lib (MIT).
#include "ft8_decoder.h"
#include "../vibe_clock.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>

extern "C" {
#include "ft8/decode.h"
#include "ft8/encode.h"
#include "ft8/message.h"
#include "ft8/constants.h"
#include "ft8/crc.h"
}

namespace vibe {

// ── The hashed-callsign table (see Ft8CallHashTable in the header) ───────────
void Ft8CallHashTable::save(const char* call, uint32_t n22) {
    if (!call || !call[0]) return;
    std::lock_guard<std::mutex> lk(mtx_);
    const uint64_t now = ++clock_;
    int freeAt = -1, oldest = -1;
    for (int i = 0; i < kCapacity; i++) {
        Entry& x = e_[i];
        if (!x.stamp) { if (freeAt < 0) freeAt = i; continue; }
        if (x.n22 == n22 && std::strncmp(x.call, call, 11) == 0) { x.stamp = now; return; }   // heard again
        if (oldest < 0 || x.stamp < e_[oldest].stamp) oldest = i;
    }
    Entry& x = e_[freeAt >= 0 ? freeAt : oldest];   // ★ full: the least recently used makes way
    std::strncpy(x.call, call, 11); x.call[11] = '\0';
    x.n22 = n22; x.stamp = now;
}
bool Ft8CallHashTable::lookup(int bits, uint32_t hash, char* out) {
    out[0] = '\0';
    const int shift = bits == 10 ? 12 : bits == 12 ? 10 : 0;
    std::lock_guard<std::mutex> lk(mtx_);
    int hit = -1;
    for (int i = 0; i < kCapacity; i++) {
        const Entry& x = e_[i];
        if (!x.stamp || (x.n22 >> shift) != hash) continue;
        // ★ Two DIFFERENT calls under one hash: either could be the sender, so neither is printed.
        if (hit >= 0 && std::strncmp(e_[hit].call, x.call, 11) != 0) return false;
        hit = i;
    }
    if (hit < 0) return false;
    e_[hit].stamp = ++clock_;
    /* ★ TERMINATED (audit 2026-10-03): strncpy of 11 leaves no NUL when the stored call is 11 long, and ft8_lib's
     *  caller strlen()s it. Its buffer is char[12] (message.c). */
    std::strncpy(out, e_[hit].call, 11);
    out[11] = '\0';
    return true;
}
void Ft8CallHashTable::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (Entry& x : e_) x = Entry{};
    clock_ = 0;
}
int Ft8CallHashTable::size() {
    std::lock_guard<std::mutex> lk(mtx_);
    int n = 0; for (const Entry& x : e_) n += x.stamp != 0; return n;
}
Ft8CallHashTable& ft8CallHashes() { static Ft8CallHashTable t; return t; }

bool Ft8Decoder::spotCallsign(const char* de, std::string& out) {
    out.clear();
    if (!de || !de[0]) return false;
    std::string s(de);
    if (s.front() == '<') {                       // a hashed call: "<...>" unknown, "<CALL>" resolved
        if (s.size() < 3 || s.back() != '>') return false;
        s = s.substr(1, s.size() - 2);
        if (s == "...") return false;
    }
    if (s.find_first_of("<>") != std::string::npos) return false;
    bool alnum = false;
    for (char ch : s) alnum |= (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
    if (!alnum) return false;                     // nothing the host's whitelist would keep
    out = s;
    return true;
}

namespace {
// ★ Shared by every decoder, and decodes run on worker threads (FT8 and FT4 can overlap) — the table locks itself.
bool ht_lookup(ftx_callsign_hash_type_t type, uint32_t hash, char* callsign) {
    const int bits = type == FTX_CALLSIGN_HASH_10_BITS ? 10 : type == FTX_CALLSIGN_HASH_12_BITS ? 12 : 22;
    return ft8CallHashes().lookup(bits, hash, callsign);
}
void ht_save(const char* callsign, uint32_t n22) { ft8CallHashes().save(callsign, n22); }
ftx_callsign_hash_interface_t g_hashIf = { ht_lookup, ht_save };

constexpr int kFreqOsr = 2;
constexpr int kTimeOsr = 2;
constexpr double kTwoPi = 6.283185307179586;

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

/* ★ A 4096-entry sine table for the subtraction's reference: 12 bits of phase is a 0.08 % amplitude error,
 *  -62 dB — far below anything the subtraction can take out — and a lookup instead of a sincosf per sample,
 *  which on a Pi 2 is the difference between 2 ms and 15 ms per decoded signal. */
struct SinTable {
    float s[4096], c[4096];
    SinTable() { for (int i = 0; i < 4096; ++i) { s[i] = (float)std::sin(kTwoPi * i / 4096); c[i] = (float)std::cos(kTwoPi * i / 4096); } }
};
const SinTable& sinTable() { static SinTable t; return t; }

/* ★★ ORDERED-STATISTICS DECODING, ORDER 1 (2026-10-05) — the "light OSD" of the decoder audit. When belief
 *  propagation gives up, take the 91 most reliable bits that are independent (Gaussian elimination on the
 *  generator, columns in order of |LLR|), re-encode from their hard decisions, and try every single flip among
 *  them: 92 codewords, each a valid LDPC codeword, the closest (least |LLR| disagreeing) one that passes the CRC
 *  wins. A codeword is only believed if it disagrees with the received hard bits in at most `maxErrors` places —
 *  the guard against the CRC-14 waving through a random codeword (WSJT-X guards its OSD the same way). */
struct Bits174 { uint64_t w[3]; };
inline void setBit(Bits174& b, int i) { b.w[i >> 6] |= 1ull << (i & 63); }
inline bool getBit(const Bits174& b, int i) { return (b.w[i >> 6] >> (i & 63)) & 1; }
const Bits174* generatorRows() {               // row k = the codeword of message bit k alone
    static Bits174 g[FTX_LDPC_K];
    static bool init = [] {
        for (int k = 0; k < FTX_LDPC_K; ++k) {
            g[k] = Bits174{{0, 0, 0}};
            setBit(g[k], k);
            for (int m = 0; m < FTX_LDPC_M; ++m)
                if (kFTX_LDPC_generator[m][k / 8] & (0x80u >> (k % 8))) setBit(g[k], FTX_LDPC_K + m);
        }
        return true;
    }();
    (void)init;
    return g;
}
/** OSD-1 on `llr` (log p(1)/p(0), codeword order). On success fills `a91` (packed message + CRC) and returns the
 *  number of hard-decision disagreements; -1 when no codeword passes the CRC within `maxErrors`. */
int osd1(const float* llr, int maxErrors, uint8_t a91[FTX_LDPC_K_BYTES]) {
    const Bits174* g = generatorRows();
    int perm[FTX_LDPC_N];
    for (int i = 0; i < FTX_LDPC_N; ++i) perm[i] = i;
    std::sort(perm, perm + FTX_LDPC_N, [&](int a, int b) { return std::fabs(llr[a]) > std::fabs(llr[b]); });
    int pos[FTX_LDPC_N];
    for (int p = 0; p < FTX_LDPC_N; ++p) pos[perm[p]] = p;
    Bits174 M[FTX_LDPC_K];
    for (int k = 0; k < FTX_LDPC_K; ++k) {
        M[k] = Bits174{{0, 0, 0}};
        for (int j = 0; j < FTX_LDPC_N; ++j) if (getBit(g[k], j)) setBit(M[k], pos[j]);
    }
    int pivot[FTX_LDPC_K]; int r = 0;
    for (int p = 0; p < FTX_LDPC_N && r < FTX_LDPC_K; ++p) {
        int i = r;
        while (i < FTX_LDPC_K && !getBit(M[i], p)) ++i;
        if (i == FTX_LDPC_K) continue;
        std::swap(M[i], M[r]);
        for (int q = 0; q < FTX_LDPC_K; ++q)
            if (q != r && getBit(M[q], p)) { M[q].w[0] ^= M[r].w[0]; M[q].w[1] ^= M[r].w[1]; M[q].w[2] ^= M[r].w[2]; }
        pivot[r++] = p;
    }
    if (r < FTX_LDPC_K) return -1;
    Bits174 hard{{0, 0, 0}};
    float w[FTX_LDPC_N];
    for (int p = 0; p < FTX_LDPC_N; ++p) { if (llr[perm[p]] > 0) setBit(hard, p); w[p] = std::fabs(llr[perm[p]]); }
    Bits174 c0{{0, 0, 0}};
    for (int q = 0; q < FTX_LDPC_K; ++q)
        if (getBit(hard, pivot[q])) { c0.w[0] ^= M[q].w[0]; c0.w[1] ^= M[q].w[1]; c0.w[2] ^= M[q].w[2]; }
    float bestD = 1e30f; int bestErr = -1;
    for (int f = -1; f < FTX_LDPC_K; ++f) {
        Bits174 c = c0;
        if (f >= 0) { c.w[0] ^= M[f].w[0]; c.w[1] ^= M[f].w[1]; c.w[2] ^= M[f].w[2]; }
        float d = 0; int err = 0;
        for (int x = 0; x < 3; ++x) {
            uint64_t diff = c.w[x] ^ hard.w[x];
            while (diff) { const int b = __builtin_ctzll(diff); d += w[x * 64 + b]; ++err; diff &= diff - 1; }
        }
        if (err > maxErrors || d >= bestD) continue;
        uint8_t m91[FTX_LDPC_K_BYTES] = {};
        for (int k = 0; k < FTX_LDPC_K; ++k) if (getBit(c, pos[k])) m91[k / 8] |= (uint8_t)(0x80u >> (k % 8));
        const uint16_t crcRx = ftx_extract_crc(m91);
        uint8_t t[FTX_LDPC_K_BYTES]; std::memcpy(t, m91, sizeof t);
        t[9] &= 0xF8; t[10] = 0; t[11] = 0;
        if (ftx_compute_crc(t, 96 - 14) != crcRx) continue;
        bestD = d; bestErr = err; std::memcpy(a91, m91, FTX_LDPC_K_BYTES);
    }
    return bestErr;
}
} // namespace

// ── One slot ────────────────────────────────────────────────────────────────────────────────────

/* ★★ THE WINDOW STARTS AT THE SLOT BOUNDARY (2026-10-05). It used to start at +0.8 s, by the decode thread's
 *  clock — 0.3 s after an on-time FT8 signal begins, so the first two Costas symbols were never seen. ft8_lib's
 *  candidate search reaches 1.6 s before the window and ~3 s into it, so a window from the boundary still takes a
 *  station whose clock is 1.5 s fast, and keeps an on-time signal's LAST Costas array too (it ends at 13.14 s;
 *  the window, 93 symbols long, ends at 14.88 s). Measured on the real hour in the commit message. */
float Ft8SlotDecoder::leadSec(bool ft4) { (void)ft4; return 0.0f; }

Ft8SlotDecoder::Ft8SlotDecoder(int sampleRate, bool ft4) : ft4_(ft4), rate_(sampleRate) {
    monitor_config_t cfg = {};
    cfg.f_min = 100.0f;
    cfg.f_max = 3500.0f;
    cfg.sample_rate = rate_;
    cfg.time_osr = kTimeOsr;
    cfg.freq_osr = kFreqOsr;
    cfg.protocol = ft4 ? FTX_PROTOCOL_FT4 : FTX_PROTOCOL_FT8;
    monitor_init(&mon_, &cfg);
    nn_ = ft4 ? FT4_NN : FT8_NN;
    nsps_ = mon_.block_size;
    windowSamples_ = mon_.wf.max_blocks * mon_.block_size;
    // GFSK frequency pulse (ft8_lib's gen_ft8.c / WSJT-X gen_ft8wave): BT 2.0 for FT8, 1.0 for FT4.
    const float bt = ft4 ? 1.0f : 2.0f, k = 5.336446f;   // pi * sqrt(2 / ln 2)
    pulse_.resize((size_t)3 * nsps_);
    for (int i = 0; i < 3 * nsps_; ++i) {
        const float t = i / (float)nsps_ - 1.5f;
        pulse_[(size_t)i] = (std::erf(k * bt * (t + 0.5f)) - std::erf(k * bt * (t - 0.5f))) / 2;
    }
    // ★ Only where the pulse is not ~0: FT8's (BT 2) is under 1e-6 for most of its 3 symbols — half the adds gone.
    pulseLo_ = 0; pulseHi_ = 3 * nsps_;
    while (pulseLo_ < pulseHi_ && pulse_[(size_t)pulseLo_] < 1e-6f) ++pulseLo_;
    while (pulseHi_ > pulseLo_ && pulse_[(size_t)(pulseHi_ - 1)] < 1e-6f) --pulseHi_;
    const size_t span = (size_t)(nn_ + 2) * nsps_;
    dphi_.resize(span);
    prodRe_.resize((size_t)nn_ * nsps_);
    prodIm_.resize((size_t)nn_ * nsps_);
}
Ft8SlotDecoder::~Ft8SlotDecoder() { monitor_free(&mon_); }

void Ft8SlotDecoder::buildWaterfall(const float* a, int n) {
    monitor_reset(&mon_);
    // ★ monitor_reset keeps the STFT's history frame; a fresh window must not start with the last one's tail.
    std::memset(mon_.last_frame, 0, sizeof(float) * (size_t)mon_.nfft);
    for (int pos = 0; pos + mon_.block_size <= n; pos += mon_.block_size) monitor_process(&mon_, a + pos);
}

namespace {
// uint8 waterfall value (0.5 dB steps from -120 dB) -> linear power
const float* magPow() {
    static float t[256];
    static bool init = [] { for (int i = 0; i < 256; ++i) t[i] = std::pow(10.0f, (i * 0.5f - 120.0f) / 10.0f); return true; }();
    (void)init;
    return t;
}
}

/* ★ One spectrogram column's mean power over the window. LAZY and cached: only the columns around a decoded signal
 *  are ever asked for, which keeps it off the per-pass bill. */
float Ft8SlotDecoder::colMean(size_t c) {
    if (noise_[c] >= 0) return noise_[c];
    const ftx_waterfall_t& wf = mon_.wf;
    const float* lut = magPow();
    double sum = 0;
    for (int b = 0; b < wf.num_blocks; ++b) sum += lut[wf.mag[(size_t)b * wf.block_stride + c]];
    return noise_[c] = (float)(wf.num_blocks ? sum / wf.num_blocks : 1e-12);
}

/* ★★ SNR, WSJT-X's DEFINITION (audit 2026-10-04: we showed `score*0.5 - 24`, a sync score in a dB costume). WSJT-X
 *  reports signal power over the noise power in 2500 Hz, the noise being a BASELINE of the slot's average spectrum
 *  (its lower envelope — the gaps between stations), not the signal's own bins. Here the same: the mean power in the
 *  DECODED tone of every symbol, over the 20th percentile of the column means within ±250 Hz, minus one (the noise in
 *  the signal's own bin), scaled from one tone bin to 2500 Hz. The signal's own bins cannot be the noise: with a
 *  two-symbol window a strong station leaks into all eight of them, and an SNR read that way squashed +20 dB to +4.
 *  kSnrCal is the one empirical constant, set against UberSDR's jt9 on the same hour (commit message). Clamped to
 *  -30..+40 as WSJT-X's own figures are. */
int Ft8SlotDecoder::snrDb(const ftx_candidate_t& c, const uint8_t* tones) {
    constexpr double kSnrCal = 3.0;
    const ftx_waterfall_t& wf = mon_.wf;
    const size_t row = ((size_t)c.time_sub * wf.freq_osr + c.freq_sub) * wf.num_bins;
    const size_t col0 = row + (size_t)c.freq_offset;
    const int span = (int)std::lround(250.0 * mon_.symbol_period);           // ±250 Hz in tone bins
    float v[256]; int m = 0;
    for (int f = std::max(0, c.freq_offset - span); f < std::min(wf.num_bins, c.freq_offset + span) && m < 256; ++f)
        v[m++] = colMean(row + (size_t)f);
    if (m < 4) return -30;
    std::nth_element(v, v + m / 5, v + m);
    const double noise = v[m / 5];
    const float* lut = magPow();
    double sig = 0; int cnt = 0;
    for (int k = 0; k < nn_; ++k) {
        const int b = c.time_offset + k;
        if (b < 0 || b >= wf.num_blocks) continue;
        sig += lut[wf.mag[(size_t)b * wf.block_stride + col0 + tones[k]]];
        ++cnt;
    }
    if (!cnt || noise <= 0) return -30;
    const double ratio = sig / cnt / noise - 1.0;
    const double binHz = 1.0 / mon_.symbol_period;
    double snr = 10.0 * std::log10(std::max(ratio, 1e-3)) + 10.0 * std::log10(binHz / 2500.0) + kSnrCal;
    snr = std::max(-30.0, std::min(40.0, snr));
    return (int)std::lround(snr);
}

/* ★★★ SUBTRACTION (WSJT-X's subtractft8, made cheap). A decoded signal is known exactly — its 79 (FT4: 105) tones
 *  — so it can be rebuilt and taken out of the audio, and what was under it decoded on the next pass.
 *  1. FINE TIME. The candidate's start is only known to half a symbol (80 ms): mix to baseband at its frequency,
 *     boxcar-decimate by 32 (375 Hz), and slide the known tones over ±0.6 symbol for the most energy.
 *  2. The reference: the exact GFSK waveform (ft8_lib's pulse), unit amplitude, at that start and frequency.
 *  3. FINE FREQUENCY from the reference itself: audio × conj(reference) is the signal's complex amplitude, and its
 *     phase drift from symbol to symbol is the frequency error (WSJT-X refines in its downsampler; this is cheaper).
 *  4. The amplitude is that product averaged over two symbols (a normalised boxcar — it follows HF fading), and
 *     2·Re(amplitude × reference) is subtracted.
 *  ★ Cost: a few passes over one signal's samples, no FFT. The bench measures it per pass. */
void Ft8SlotDecoder::subtract(float* a, int n, const uint8_t* tones, double start, double f0) {
    const int N = nn_ * nsps_;
    const double fs = rate_;
    const SinTable& tab = sinTable();
    const double T = mon_.symbol_period;
    double df = 0;

    // ── 1. fine time (and frequency) on a decimated baseband ──
    {
        const int D = 32, spsd = nsps_ / D;
        const int Md = (int)std::lround(0.6 * spsd);                  // ±0.6 symbol, in decimated samples
        const int i0 = (int)std::floor(start) - Md * D;
        const int nd = N / D + 2 * Md + 1;
        zr_.assign((size_t)nd, 0.0f); zi_.assign((size_t)nd, 0.0f);
        // mixer e^{-j 2π f0 idx/fs}, as a uint32 phase so it is exact across the slot
        const uint32_t step = (uint32_t)(int64_t)std::llround(f0 / fs * 4294967296.0);
        uint32_t ph = (uint32_t)((int64_t)i0 * (int64_t)step);
        for (int j = 0; j < nd; ++j) {
            float sr = 0, si = 0;
            const int b = i0 + j * D;
            if (b >= 0 && b + D <= n) {
                for (int q = 0; q < D; ++q, ph += step) {
                    const int t = (int)(ph >> 20);
                    sr += a[b + q] * tab.c[t];
                    si -= a[b + q] * tab.s[t];
                }
            } else {
                for (int q = 0; q < D; ++q, ph += step) {
                    if (b + q < 0 || b + q >= n) continue;
                    const int t = (int)(ph >> 20);
                    sr += a[b + q] * tab.c[t];
                    si -= a[b + q] * tab.s[t];
                }
            }
            zr_[(size_t)j] = sr; zi_[(size_t)j] = si;
        }
        const int ntones = ft4_ ? 4 : 8;
        if ((int)toneR_.size() != ntones * spsd) {                      // tone t: t cycles per symbol
            toneR_.resize((size_t)ntones * spsd); toneI_.resize((size_t)ntones * spsd);
            for (int t = 0; t < ntones; ++t)
                for (int j = 0; j < spsd; ++j) {
                    toneR_[(size_t)t * spsd + j] = (float)std::cos(kTwoPi * t * j / spsd);
                    toneI_[(size_t)t * spsd + j] = (float)std::sin(kTwoPi * t * j / spsd);
                }
        }
        // per-symbol correlation with the known tone; `pairs` also returns Σ c[k+1]·conj(c[k])
        auto corr = [&](int m, double* pr, double* pi) {
            double e = 0, prevR = 0, prevI = 0, sR = 0, sI = 0; bool have = false;
            for (int k = 0; k < nn_; ++k) {
                const int b = m + k * spsd;
                if (b < 0 || b + spsd > nd) { have = false; continue; }
                const float* r = &toneR_[(size_t)tones[k] * spsd]; const float* s = &toneI_[(size_t)tones[k] * spsd];
                const float* x = &zr_[(size_t)b]; const float* y = &zi_[(size_t)b];
                float cr = 0, ci = 0;
                for (int j = 0; j < spsd; ++j) { cr += x[j] * r[j] + y[j] * s[j]; ci += y[j] * r[j] - x[j] * s[j]; }
                e += (double)cr * cr + (double)ci * ci;
                if (pr) {
                    if (have) { sR += cr * prevR + ci * prevI; sI += ci * prevR - cr * prevI; }
                    prevR = cr; prevI = ci; have = true;
                }
            }
            if (pr) { *pr = sR; *pi = sI; }
            return e;
        };
        int best = Md; double bestE = -1;
        for (int m = 0; m <= 2 * Md; m += 3) { const double e = corr(m, nullptr, nullptr); if (e > bestE) { bestE = e; best = m; } }
        const int c0 = best;
        for (int m = std::max(0, c0 - 2); m <= std::min(2 * Md, c0 + 2); ++m) {
            if (m == c0) continue;
            const double e = corr(m, nullptr, nullptr); if (e > bestE) { bestE = e; best = m; }
        }
        start = (double)i0 + (double)best * D;
        /* ★ The frequency error: a continuous-phase signal advances 2π(f0 + df + tone·Δf)·T over a symbol, and the
         *  tone term is a whole number of turns — so symbol k+1's correlation leads symbol k's by 2π·df·T. */
        double pr = 0, pi = 0;
        corr(best, &pr, &pi);
        df = std::atan2(pi, pr) / (kTwoPi * T);
    }

    // ── 2. the reference: GFSK phase steps (ft8_lib's synth_gfsk), only where the pulse is not ~0 ──
    const int s0 = (int)std::lround(start);
    std::fill(dphi_.begin(), dphi_.end(), 0.0f);
    for (int i = 0; i < nn_; ++i) {
        if (!tones[i]) continue;
        float* d = &dphi_[(size_t)i * nsps_];
        const float tv = (float)tones[i];
        for (int j = pulseLo_; j < pulseHi_; ++j) d[j] += tv * pulse_[(size_t)j];
    }
    for (int j = 0; j < 2 * nsps_; ++j) {                      // the dummy symbols either side
        dphi_[(size_t)j] += pulse_[(size_t)(j + nsps_)] * tones[0];
        dphi_[(size_t)(j + nn_ * nsps_)] += pulse_[(size_t)j] * tones[nn_ - 1];
    }
    const uint32_t base = (uint32_t)(int64_t)std::llround((f0 + df) / fs * 4294967296.0);
    const float toneStep = (float)(1.0 / T / fs * 4294967296.0);
    const float* dp = &dphi_[(size_t)nsps_];                   // sample k of the waveform is dphi[k + nsps]

    // ── 3. audio × conj(reference) over the part of the signal inside the window ──
    const int kLo = std::max(0, -s0), kHi = std::min(N, n - s0);
    if (kHi <= kLo) return;
    uint32_t ph = 0;
    for (int k = 0; k < kLo; ++k) ph += base + (uint32_t)(int32_t)(dp[k] * toneStep);
    const uint32_t phLo = ph;
    for (int k = kLo; k < kHi; ++k) {
        const int t = (int)(ph >> 20);
        const float x = a[s0 + k];
        prodRe_[(size_t)k] = x * tab.c[t];
        prodIm_[(size_t)k] = -x * tab.s[t];
        ph += base + (uint32_t)(int32_t)(dp[k] * toneStep);
    }

    // ── 4. the amplitude (two-symbol normalised boxcar), and the subtraction ──
    const int half = nsps_;
    double sr = 0, si = 0; int cnt = 0;
    int lo = kLo, hi = kLo;
    ph = phLo;
    for (int k = kLo; k < kHi; ++k) {
        const int wantHi = std::min(kHi, k + half), wantLo = std::max(kLo, k - half);
        while (hi < wantHi) { sr += prodRe_[(size_t)hi]; si += prodIm_[(size_t)hi]; ++hi; ++cnt; }
        while (lo < wantLo) { sr -= prodRe_[(size_t)lo]; si -= prodIm_[(size_t)lo]; ++lo; --cnt; }
        const float ar = (float)(sr / cnt), ai = (float)(si / cnt);
        const int t = (int)(ph >> 20);
        a[s0 + k] -= 2.0f * (ar * tab.c[t] - ai * tab.s[t]);
        ph += base + (uint32_t)(int32_t)(dp[k] * toneStep);
    }
}

void Ft8SlotDecoder::decode(float* audio, int n, const Ft8SlotOptions& o, std::vector<Ft8Result>& out, Ft8SlotStats& st) {
    st = Ft8SlotStats{};
    const size_t first = out.size();
    struct Sub { uint8_t tones[FT4_NN > FT8_NN ? FT4_NN : FT8_NN]; double start; double f0; };
    std::vector<Sub> subs;
    if ((int)cands_.size() < o.maxCandidates) cands_.resize((size_t)o.maxCandidates);
    const auto t0 = std::chrono::steady_clock::now();
    const int passes = std::max(1, std::min(3, o.maxPasses));
    for (int pass = 1; pass <= passes; ++pass) {
        if (pass > 1) {
            if (subs.empty()) break;                               // nothing new to take out: the same search again
            if (o.mayRunPass && !o.mayRunPass(pass, msSince(t0), st.passMs[pass - 2])) break;
        }
        const auto tp = std::chrono::steady_clock::now();
        for (const Sub& s : subs) subtract(audio, n, s.tones, s.start, s.f0);
        subs.clear();
        buildWaterfall(audio, n);
        noise_.assign((size_t)mon_.wf.block_stride, -1.0f);     // ★ unknown until a decode asks (colMean)
        const ftx_waterfall_t* wf = &mon_.wf;
        const int nc = ftx_find_candidates(wf, o.maxCandidates, cands_.data(), o.minScore);
        int found = 0;
        for (int i = 0; i < nc; ++i) {
            if (o.abort && o.abort->load(std::memory_order_relaxed)) { st.passes = pass; return; }
            const ftx_candidate_t& c = cands_[(size_t)i];
            ftx_message_t msg; ftx_decode_status_t ds;
            bool viaOsd = false;
            if (!ftx_decode_candidate(wf, &c, o.ldpcIters, &msg, &ds)) {
                if (o.osdMaxErrors <= 0 || c.score < o.osdMinScore) continue;
                float llr[FTX_LDPC_N]; uint8_t a91[FTX_LDPC_K_BYTES];
                ftx_candidate_log174(wf, &c, llr);
                if (osd1(llr, o.osdMaxErrors, a91) < 0) continue;
                ftx_message_init(&msg);
                for (int b = 0; b < 10; ++b) msg.payload[b] = ft4_ ? (uint8_t)(a91[b] ^ kFT4_XOR_sequence[b]) : a91[b];
                msg.payload[9] &= 0xF8;
                uint8_t t[FTX_LDPC_K_BYTES]; std::memcpy(t, a91, sizeof t); t[9] &= 0xF8; t[10] = 0; t[11] = 0;
                msg.hash = ftx_compute_crc(t, 96 - 14);
                viaOsd = true;
            }
            bool dup = false;   // ★ one message per slot, whichever pass and candidate found it first
            for (size_t r = first; r < out.size() && !dup; ++r)
                dup = out[r].msg.hash == msg.hash && std::memcmp(out[r].msg.payload, msg.payload, sizeof msg.payload) == 0;
            if (dup) continue;
            Sub s;
            // ★ The tones as transmitted (ft4_encode applies FT4's XOR itself, as ftx_decode_candidate removed it).
            if (ft4_) ft4_encode(msg.payload, s.tones); else ft8_encode(msg.payload, s.tones);
            const float hz = (mon_.min_bin + c.freq_offset + (float)c.freq_sub / wf->freq_osr) / mon_.symbol_period;
            // ★ Start sample: the STFT frame of block b, sub s ENDS at (b·osr + s + 1)·subblock and is two symbols
            //   long, so a symbol fills it best when it starts at (b − 1)·block + s·subblock.
            const double startSample = (double)(c.time_offset - 1) * mon_.block_size + (double)c.time_sub * mon_.subblock_size;
            Ft8Result r;
            r.msg = msg;
            r.hz = hz;
            r.dt = (float)(leadSec(ft4_) + startSample / rate_ - 0.5);
            r.snr = snrDb(c, s.tones);
            r.pass = pass;
            r.osd = viaOsd;
            out.push_back(r);
            s.start = startSample; s.f0 = hz;
            subs.push_back(s);
            ++found;
        }
        st.passes = pass;
        st.passMs[pass - 1] = msSince(tp);
        st.passDecodes[pass - 1] = found;
    }
}

// ── The live decoder ────────────────────────────────────────────────────────────────────────────

Ft8Decoder::Ft8Decoder(int sampleRate, bool ft4_)
    : ft4(ft4_), rate(sampleRate), slot_(sampleRate, ft4_) {
    slotPeriod = ft4 ? FT4_SLOT_TIME : FT8_SLOT_TIME;
    window_ = slot_.windowSamples();
    // ★ The ring holds a window plus 3 s: a window is cut the moment its last sample lands, so this is slack
    //   for a late block, not history.
    ring_.assign((size_t)window_ + (size_t)(3 * rate), 0.0f);
    work_.assign((size_t)window_, 0.0f);
    ok = true;
    worker = std::thread([this] { workerLoop(); });
}

Ft8Decoder::~Ft8Decoder() {
    // ★ Abort a decode in progress (checked between candidates) rather than wait seconds for it:
    //   the caller — stopSpots — holds the lock the audio thread's feedSpots needs.
    abortDecode = true;
    { std::lock_guard<std::mutex> lk(wm); stop = true; }
    wcv.notify_all();
    if (worker.joinable()) worker.join();
}

void Ft8Decoder::workerLoop() {
    std::unique_lock<std::mutex> lk(wm);
    for (;;) {
        wcv.wait(lk, [this] { return pending || stop; });
        if (stop) return;
        pending = false; busy = true;
        lk.unlock();
        runDecode();
        lk.lock();
        busy = false;
    }
}

void Ft8Decoder::process(const int16_t* in, int count, double captureUtc) {
    if (!ok || count <= 0) return;
    if (!std::isfinite(captureUtc)) captureUtc = vibe::vibeUtcNow();
    const int64_t R = (int64_t)ring_.size();
    for (int i = 0; i < count; i++) ring_[(size_t)((total_ + i) % R)] = (float)in[i] / 32768.0f;
    total_ += count;

    /* ★★ THE ANCHOR: the capture UTC of sample 0, from this block's stamp. A stamp is late by however long the
     *  audio took to reach the decoder, never early — so a stamp EARLIER than the anchor is the better one and
     *  is taken at once, while a later one only leaks in slowly (30 s), following the sample clock's drift
     *  against UTC without chasing one late block. A stamp a whole second later means audio was LOST (the host
     *  drops what it is 4 s behind on): the count no longer says the time, so re-anchor and re-pick the slot. */
    const double est = captureUtc - (double)total_ / rate;
    if (!anchored_ || est - utc0_ > 1.0) {
        if (anchored_) reanchors.fetch_add(1);
        utc0_ = est; anchored_ = true; nextSlot_ = NAN;
        floor_ = total_ - count;       // ★ audio from before the gap has no true time any more: never cut from it
    } else if (est < utc0_) {
        utc0_ = est;
    } else {
        utc0_ += (est - utc0_) * std::min(1.0, (double)count / rate / 30.0);
    }
    const int64_t oldest = std::max<int64_t>(floor_, total_ - R);
    const double lead = Ft8SlotDecoder::leadSec(ft4);
    if (std::isnan(nextSlot_)) {
        // the first slot whose whole window is still ahead of (or in) what we hold
        const double oldestUtc = utc0_ + (double)oldest / rate;
        nextSlot_ = std::ceil((oldestUtc - lead) / slotPeriod) * slotPeriod;
    }
    for (;;) {
        const int64_t i0 = (int64_t)std::llround((nextSlot_ + lead - utc0_) * rate);
        if (i0 < oldest) { nextSlot_ += slotPeriod; continue; }      // its start is gone: wait for the next
        if (total_ < i0 + window_) break;                            // not complete yet
        {
            std::lock_guard<std::mutex> lk(wm);
            if (!busy && !pending) {
                for (int i = 0; i < window_; ++i) work_[(size_t)i] = ring_[(size_t)((i0 + i) % R)];
                workSlotEnd_ = captureUtc;     // ★ when the window was complete, on the capture clock
                pending = true;
                wcv.notify_one();
            } else {
                slotsSkipped.fetch_add(1);     // ★ the previous decode is still running: skip, never wait
            }
        }
        nextSlot_ += slotPeriod;
    }
}

void Ft8Decoder::runDecode() {
    const auto t0 = std::chrono::steady_clock::now();
    // ★ How late this decode starts after its window was complete (a queue that ran behind, a busy core).
    const double lagMs = std::max(0.0, std::min(slotPeriod * 1000.0, (vibe::vibeUtcNow() - workSlotEnd_) * 1000.0));
    Ft8SlotOptions o;
    o.maxPasses = std::max(1, std::min(3, maxPasses.load()));
    o.abort = &abortDecode;
    /* ★★ OSD only on spare CPU, like the passes: a loaded box decodes exactly as before (2026-10-05). 24 hard
     *  errors: on the real hour 30 let one garbage message through ("7Z1OSX ZF7IZT R QM53") and 36 two; 24
     *  none, for +1.7 % of jt9's decodes at +2 ms a slot on a Mac. */
    const bool loadedAtStart = loadProbe ? loadProbe() : false;
    o.osdMaxErrors = loadedAtStart || o.maxPasses < 2 ? 0 : kOsdMaxErrors;
    o.mayRunPass = [&](int, double elapsedMs, double lastMs) {
        const bool loaded = loadProbe ? loadProbe() : false;
        return passAllowed(slotPeriod * 1000.0, lagMs, elapsedMs, lastMs, loaded);
    };
    std::vector<Ft8Result> res;
    Ft8SlotStats st;
    slot_.decode(work_.data(), window_, o, res, st);
    lastPasses.store(st.passes);
    lastDecodeMs.store(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    slotsDecoded.fetch_add(1);
    if (abortDecode.load()) return;

    for (const Ft8Result& r : res) {
        char callTo[24] = {}, callDe[24] = {}, grid[24] = {};
        ftx_field_t fields[FTX_MAX_MESSAGE_FIELDS];
        ftx_message_rc_t rc = ftx_message_decode_std(&r.msg, &g_hashIf, callTo, callDe, grid, fields);
        if (rc != FTX_MESSAGE_RC_OK) continue;
        std::string deCall;
        if (!spotCallsign(callDe, deCall)) continue;   // ★ empty or an unresolved "<...>": not a spot
        // ★★ ft8_lib's third field is "grid OR report": "IO92", but also "-12", "R-12", "73", "RRR" and
        //    "RR73". Only a real 4-character locator is a grid (B10: the map warned about ~40 "unparseable
        //    grids" on every update, and every RR73 sign-off — a VALID-LOOKING square near the North Pole —
        //    was plotted in the Arctic). Anything else is sent as no grid at all.
        const bool realGrid = std::strlen(grid) == 4
            && grid[0] >= 'A' && grid[0] <= 'R' && grid[1] >= 'A' && grid[1] <= 'R'
            && grid[2] >= '0' && grid[2] <= '9' && grid[3] >= '0' && grid[3] <= '9'
            && std::strcmp(grid, "RR73") != 0;
        if (!realGrid) grid[0] = 0;
        if (onSpot) onSpot(callTo, deCall, grid, r.snr, r.hz);
    }
}

} // namespace vibe
