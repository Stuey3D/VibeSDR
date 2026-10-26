// VibeSDR V12 — JS8 decoder. See js8_decoder.h; the protocol is described there and in docs/v12/JS8-FT2.md.
#include "js8_decoder.h"
#include "js8_ldpc.h"
#include "../vibe_clock.h"
#include "../vibedsp/vibe_denormals.h"
#include "ft8/vibe_simd4.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace vibe {

namespace {
constexpr double kTwoPi = 6.283185307179586;
constexpr int kNN = 79;                 // channel symbols
constexpr int kTimeOsr = 2, kFreqOsr = 2;

// ★ Protocol facts (js8_decoder.h's table).
const Js8SubmodeInfo kInfo[4] = {
    { Js8Submode::Normal, "NORMAL", 'A', 1920, 15.0f, 0.5f, false },
    { Js8Submode::Fast,   "FAST",   'B', 1200, 10.0f, 0.2f, true },
    { Js8Submode::Turbo,  "TURBO",  'C',  600,  6.0f, 0.1f, true },
    { Js8Submode::Slow,   "SLOW",   'E', 3840, 30.0f, 0.5f, true },
};
const uint8_t kCostasFt8[7] = { 4, 2, 5, 6, 1, 3, 0 };
const uint8_t kCostasJs8[3][7] = { { 0, 6, 2, 3, 5, 4, 1 }, { 1, 5, 0, 2, 3, 6, 4 }, { 2, 5, 0, 6, 4, 1, 3 } };
constexpr int kSyncAt[3] = { 0, 36, 72 };

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// uint8 waterfall level (0.5 dB steps from -120 dB) -> linear power
const float* levelPower() {
    static float t[256];
    static bool init = [] { for (int i = 0; i < 256; ++i) t[i] = std::pow(10.0f, (i * 0.5f - 120.0f) / 10.0f); return true; }();
    (void)init;
    return t;
}

// … and -> linear amplitude
const float* levelAmplitude() {
    static float t[256];
    static bool init = [] { for (int i = 0; i < 256; ++i) t[i] = std::pow(10.0f, (i * 0.5f - 120.0f) / 20.0f); return true; }();
    (void)init;
    return t;
}

struct SinTable {
    float s[4096], c[4096];
    SinTable() { for (int i = 0; i < 4096; ++i) { s[i] = (float)std::sin(kTwoPi * i / 4096); c[i] = (float)std::cos(kTwoPi * i / 4096); } }
};
const SinTable& sinTable() { static SinTable t; return t; }

/* ★★ THE SYNC SUM, SIXTEEN FREQUENCY OFFSETS AT ONCE. A candidate's score is a sum of level differences
 *  (Costas tone minus its neighbour above, below, the symbol before and after) at fixed offsets from its first bin;
 *  the same offsets for the next frequency are the next byte — so sixteen candidates are one 16-byte load per term
 *  and a widening subtract. uint8 levels, int16 sums: at most 84 terms × 255 fits. */
void syncSum16(const uint8_t* p, const int32_t* ta, const int32_t* tb, int n, int16_t out[16]) {
#if VIBE_FT8_NEON
    int16x8_t lo = vdupq_n_s16(0), hi = vdupq_n_s16(0);
    for (int i = 0; i < n; ++i) {
        const uint8x16_t a = vld1q_u8(p + ta[i]), b = vld1q_u8(p + tb[i]);
        lo = vaddq_s16(lo, vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(a), vget_low_u8(b))));
        hi = vaddq_s16(hi, vreinterpretq_s16_u16(vsubl_u8(vget_high_u8(a), vget_high_u8(b))));
    }
    vst1q_s16(out, lo); vst1q_s16(out + 8, hi);
#elif VIBE_FT8_SSE2
    const __m128i z = _mm_setzero_si128();
    __m128i lo = z, hi = z;
    for (int i = 0; i < n; ++i) {
        const __m128i a = _mm_loadu_si128((const __m128i*)(p + ta[i])), b = _mm_loadu_si128((const __m128i*)(p + tb[i]));
        lo = _mm_add_epi16(lo, _mm_sub_epi16(_mm_unpacklo_epi8(a, z), _mm_unpacklo_epi8(b, z)));
        hi = _mm_add_epi16(hi, _mm_sub_epi16(_mm_unpackhi_epi8(a, z), _mm_unpackhi_epi8(b, z)));
    }
    _mm_storeu_si128((__m128i*)out, lo); _mm_storeu_si128((__m128i*)(out + 8), hi);
#else
    int acc[16] = {0};
    for (int i = 0; i < n; ++i)
        for (int l = 0; l < 16; ++l) acc[l] += (int)p[ta[i] + l] - (int)p[tb[i] + l];
    for (int l = 0; l < 16; ++l) out[l] = (int16_t)acc[l];
#endif
}
} // namespace

const Js8SubmodeInfo& js8Info(Js8Submode s) {
    for (const auto& i : kInfo) if (i.id == s) return i;
    return kInfo[0];
}

void js8Info87(const uint8_t* bits72, int flags, uint8_t* info) {
    for (int i = 0; i < 72; ++i) info[i] = bits72[i] & 1;
    info[72] = (uint8_t)((flags >> 2) & 1); info[73] = (uint8_t)((flags >> 1) & 1); info[74] = (uint8_t)(flags & 1);
    const uint16_t crc = js8::crc12(info);
    for (int i = 0; i < 12; ++i) info[75 + i] = (uint8_t)((crc >> (11 - i)) & 1);
}

void js8Tones(const uint8_t* info, Js8Submode s, uint8_t* tones) {
    uint8_t cw[js8::kN];
    js8::encode(info, cw);
    const bool mod = js8Info(s).modifiedCostas;
    for (int m = 0; m < 3; ++m)
        for (int k = 0; k < 7; ++k) tones[kSyncAt[m] + k] = mod ? kCostasJs8[m][k] : kCostasFt8[k];
    for (int d = 0; d < 58; ++d) {
        const int sym = d < 29 ? 7 + d : 43 + (d - 29);
        tones[sym] = (uint8_t)(cw[3 * d] << 2 | cw[3 * d + 1] << 1 | cw[3 * d + 2]);
    }
}

// ── One slot ──────────────────────────────────────────────────────────────────────────────────────

Js8SlotDecoder::Js8SlotDecoder(int sampleRate, Js8Submode s) : sub_(s), rate_(sampleRate) {
    const Js8SubmodeInfo& in = js8Info(s);
    T_ = (float)in.nsps12k / 12000.0f;
    monitor_config_t cfg = {};
    cfg.f_min = 100.0f;
    cfg.f_max = 3300.0f;
    cfg.sample_rate = rate_;
    cfg.time_osr = kTimeOsr;
    cfg.freq_osr = kFreqOsr;
    cfg.protocol = FTX_PROTOCOL_FT8;     // ★ only the frame-agnostic STFT is used; the search and LLRs are JS8's own
    cfg.symbol_period = T_;
    cfg.slot_time = in.period;
    monitor_init(&mon_, &cfg);
    nsps_ = mon_.block_size;
    windowSamples_ = mon_.wf.max_blocks * mon_.block_size;
    for (int m = 0; m < 3; ++m)
        for (int k = 0; k < 7; ++k) costas_[m][k] = in.modifiedCostas ? kCostasJs8[m][k] : kCostasFt8[k];
    /* ★ The time search: a clock up to 1.5 s fast or 2 s slow against the submode's start, never starting so early
     *  that the first Costas array and more is gone, nor so late that the last two are. */
    const int t0 = (int)std::lround(in.startDelay / T_);
    tMin_ = std::max(-7, t0 - (int)std::ceil(1.5 / T_));
    tMax_ = std::min(mon_.wf.max_blocks - 72 + 1, t0 + (int)std::ceil(2.0 / T_) + 1);
}
Js8SlotDecoder::~Js8SlotDecoder() { monitor_free(&mon_); }

void Js8SlotDecoder::buildWaterfall(const float* a, int n) {
    monitor_reset(&mon_);
    std::memset(mon_.last_frame, 0, sizeof(float) * (size_t)mon_.nfft);
    for (int pos = 0; pos + mon_.block_size <= n; pos += mon_.block_size) monitor_process(&mon_, a + pos);
}

float Js8SlotDecoder::colMean(size_t c) {
    if (colMean_[c] >= 0) return colMean_[c];
    const ftx_waterfall_t& wf = mon_.wf;
    const float* lut = levelPower();
    double sum = 0;
    for (int b = 0; b < wf.num_blocks; ++b) sum += lut[wf.mag[(size_t)b * wf.block_stride + c]];
    return colMean_[c] = (float)(wf.num_blocks ? sum / wf.num_blocks : 1e-12);
}

int Js8SlotDecoder::search(int maxCands, int minScore) {
    const ftx_waterfall_t& wf = mon_.wf;
    const int nb = wf.num_bins, bs = wf.block_stride, nblk = wf.num_blocks;
    const int fEnd = nb - 7;                         // tone 7 must be inside
    cands_.clear();
    if (fEnd <= 0) return 0;
    int32_t ta[84], tb[84];
    for (int t = tMin_; t < tMax_; ++t) {
        for (int ts = 0; ts < kTimeOsr; ++ts) {
            for (int fs = 0; fs < kFreqOsr; ++fs) {
                const int row = (ts * kFreqOsr + fs) * nb;
                int n = 0;
                for (int m = 0; m < 3; ++m)
                    for (int k = 0; k < 7; ++k) {
                        const int b = t + kSyncAt[m] + k;
                        if (b < 0 || b >= nblk) continue;
                        const int sm = costas_[m][k], at = b * bs + row + sm;
                        if (sm > 0) { ta[n] = at; tb[n] = at - 1; ++n; }
                        if (sm < 7) { ta[n] = at; tb[n] = at + 1; ++n; }
                        if (k > 0 && b > 0) { ta[n] = at; tb[n] = at - bs; ++n; }
                        if (k < 6 && b + 1 < nblk) { ta[n] = at; tb[n] = at + bs; ++n; }
                    }
                if (n < 24) continue;                    // fewer than ~6 sync symbols in the window
                const int below = minScore * n;          // raw / n < minScore ⇔ raw < minScore · n (no divide)
                for (int f = 0; f < fEnd; f += 16) {
                    int16_t raw[16];
                    const int lanes = std::min(16, fEnd - f);
                    if (lanes == 16 && f + 16 + 7 <= nb) syncSum16(wf.mag + f, ta, tb, n, raw);
                    else for (int l = 0; l < lanes; ++l) {
                        int acc = 0;
                        for (int i = 0; i < n; ++i) acc += (int)wf.mag[ta[i] + f + l] - (int)wf.mag[tb[i] + f + l];
                        raw[l] = (int16_t)acc;
                    }
                    for (int l = 0; l < lanes; ++l) {
                        if (raw[l] < below) continue;
                        cands_.push_back(Cand{ (int16_t)(raw[l] / n), (int16_t)t, (int16_t)(f + l), (uint8_t)ts, (uint8_t)fs });
                    }
                }
            }
        }
    }
    auto better = [](const Cand& a, const Cand& b) { return a.score > b.score; };
    if ((int)cands_.size() > maxCands) {
        std::nth_element(cands_.begin(), cands_.begin() + maxCands, cands_.end(), better);
        cands_.resize((size_t)maxCands);
    }
    std::sort(cands_.begin(), cands_.end(), better);
    return (int)cands_.size();
}

/* ★ Per-bit log-likelihoods from the eight tone AMPLITUDES of each data symbol (max-log: the best tone with the bit
 *  set against the best without), the bits MSB first, no Gray map. Amplitude rather than dB: measured on the synthetic
 *  band, it decodes a little more at the threshold (FAST 8/8 against 7/8 at -16 dB, NORMAL 6/8 against 5/8 at -18); symbols outside the window are erasures (0). Scaled
 *  so their spread is the same whatever the signal level — what the belief propagation was tuned at. */
void Js8SlotDecoder::likelihoods(const Cand& c, float* llr) {
    const ftx_waterfall_t& wf = mon_.wf;
    const float* amp = levelAmplitude();
    const int row = (c.ts * kFreqOsr + c.fs) * wf.num_bins + c.f;
    double s1 = 0, s2 = 0; int cnt = 0;
    for (int d = 0; d < 58; ++d) {
        const int sym = d < 29 ? 7 + d : 43 + (d - 29);
        const int b = c.t + sym;
        float* L = llr + 3 * d;
        if (b < 0 || b >= wf.num_blocks) { L[0] = L[1] = L[2] = 0; continue; }
        const uint8_t* p = wf.mag + (size_t)b * wf.block_stride + row;
        float v[8];
        for (int j = 0; j < 8; ++j) v[j] = amp[p[j]];
        L[0] = std::max(std::max(v[4], v[5]), std::max(v[6], v[7])) - std::max(std::max(v[0], v[1]), std::max(v[2], v[3]));
        L[1] = std::max(std::max(v[2], v[3]), std::max(v[6], v[7])) - std::max(std::max(v[0], v[1]), std::max(v[4], v[5]));
        L[2] = std::max(std::max(v[1], v[3]), std::max(v[5], v[7])) - std::max(std::max(v[0], v[2]), std::max(v[4], v[6]));
        for (int k = 0; k < 3; ++k) { s1 += L[k]; s2 += (double)L[k] * L[k]; }
        cnt += 3;
    }
    if (!cnt) return;
    const double mean = s1 / cnt, var = s2 / cnt - mean * mean;
    const float scale = (float)std::sqrt(24.0 / (var > 1e-6 ? var : 1e-6));
    for (int i = 0; i < js8::kN; ++i) llr[i] *= scale;
}

int Js8SlotDecoder::snrDb(const Cand& c, const uint8_t* tones) {
    constexpr double kSnrCal = 3.0;                   // the FT8 decoder's constant: true SNR in 2500 Hz …
    const ftx_waterfall_t& wf = mon_.wf;
    const size_t row = ((size_t)c.ts * kFreqOsr + c.fs) * wf.num_bins;
    const int span = std::max(4, (int)std::lround(250.0 * T_));
    float v[512]; int m = 0;
    for (int f = std::max(0, c.f - span); f < std::min(wf.num_bins, c.f + 8 + span) && m < 512; ++f) v[m++] = colMean(row + (size_t)f);
    if (m < 4) return -30;
    std::nth_element(v, v + m / 5, v + m);
    const double noise = v[m / 5];
    const float* lut = levelPower();
    double sig = 0; int cnt = 0;
    for (int k = 0; k < kNN; ++k) {
        const int b = c.t + k;
        if (b < 0 || b >= wf.num_blocks) continue;
        sig += lut[wf.mag[(size_t)b * wf.block_stride + row + (size_t)c.f + tones[k]]];
        ++cnt;
    }
    if (!cnt || noise <= 0) return -30;
    const double ratio = sig / cnt / noise - 1.0;
    double snr = 10.0 * std::log10(std::max(ratio, 1e-3)) + 10.0 * std::log10(1.0 / T_ / 2500.0) + kSnrCal + kSnrScaleDb;
    return (int)std::lround(std::max(-35.0, std::min(30.0, snr)));
}

/* ★★ SUBTRACTION (pass 2). JS8 is plain continuous-phase FSK, so a decoded signal is known exactly but for its
 *  start, its frequency to a fraction of a tone, and its (fading) amplitude and phase:
 *   1. mix to baseband at the candidate's frequency, boxcar-decimate to 24-32 samples a symbol, and slide the known
 *      tones over ±½ symbol and ±¼ tone for the most energy — the fine start and frequency;
 *   2. the reference e^{jφ(t)}, unit amplitude, phase-continuous across tones (a 4096-entry table, -62 dB);
 *   3. the amplitude: audio × conj(reference), a ±1-symbol box (block sums, interpolated), and 2·Re(A·ref) taken out.
 *  Cost: a few passes over one signal's samples. */
void Js8SlotDecoder::subtract(float* a, int n, const uint8_t* tones, double start, double f0) {
    const SinTable& tab = sinTable();
    const double fs = rate_, binHz = 1.0 / T_;
    const int spsd = (nsps_ % 32 == 0) ? 32 : 24;
    if (nsps_ % spsd) return;
    const int D = nsps_ / spsd;
    // ── 1. decimated baseband around [start - ½ symbol, end + ½ symbol]
    const int half = spsd / 2;
    const int i0 = (int)std::floor(start) - half * D;
    const int nd = kNN * spsd + 2 * half + 1;
    std::vector<float> zr((size_t)nd, 0.0f), zi((size_t)nd, 0.0f);
    {
        const uint32_t step = (uint32_t)(int64_t)std::llround(f0 / fs * 4294967296.0);
        for (int j = 0; j < nd; ++j) {
            const int b = i0 + j * D;
            float ur = 0, ui = 0;
            uint32_t ph = (uint32_t)((int64_t)b * (int64_t)step);
            for (int q = 0; q < D; ++q, ph += step) {
                const int x = b + q;
                if (x < 0 || x >= n) continue;
                const int t = (int)(ph >> 20);
                ur += a[x] * tab.c[t]; ui -= a[x] * tab.s[t];
            }
            zr[(size_t)j] = ur; zi[(size_t)j] = ui;
        }
    }
    int bestM = half; double bestDf = 0, bestE = -1;
    std::vector<float> tc((size_t)8 * spsd), tsn((size_t)8 * spsd);
    for (int dfi = -2; dfi <= 2; ++dfi) {
        const double df = dfi * 0.125 * binHz;
        for (int t = 0; t < 8; ++t)                  // tone t (+ df) over one symbol, at the decimated rate
            for (int j = 0; j < spsd; ++j) {
                const double ph = kTwoPi * (t * binHz + df) * j / (fs / D);
                tc[(size_t)(t * spsd + j)] = (float)std::cos(ph); tsn[(size_t)(t * spsd + j)] = (float)std::sin(ph);
            }
        for (int m = 0; m <= 2 * half; ++m) {
            double e = 0;
            for (int k = 0; k < kNN; ++k) {
                const float* c = &tc[(size_t)tones[k] * spsd]; const float* s = &tsn[(size_t)tones[k] * spsd];
                const float* x = &zr[(size_t)(m + k * spsd)]; const float* y = &zi[(size_t)(m + k * spsd)];
                float cr = 0, ci = 0;
                for (int j = 0; j < spsd; ++j) { cr += x[j] * c[j] + y[j] * s[j]; ci += y[j] * c[j] - x[j] * s[j]; }
                e += (double)cr * cr + (double)ci * ci;
            }
            if (e > bestE) { bestE = e; bestM = m; bestDf = df; }
        }
    }
    const int s0 = i0 + bestM * D;
    const int N = kNN * nsps_;
    const int kLo = std::max(0, -s0), kHi = std::min(N, n - s0);
    if (kHi <= kLo) return;
    // ── 2. the reference, phase-continuous
    std::vector<float> rc((size_t)N), rs((size_t)N);
    {
        uint32_t ph = 0;
        const double f00 = f0 + bestDf;
        for (int k = 0; k < kNN; ++k) {
            const uint32_t step = (uint32_t)(int64_t)std::llround((f00 + tones[k] * binHz) / fs * 4294967296.0);
            for (int j = 0; j < nsps_; ++j) {
                const int t = (int)(ph >> 20);
                rc[(size_t)(k * nsps_ + j)] = tab.c[t]; rs[(size_t)(k * nsps_ + j)] = tab.s[t];
                ph += step;
            }
        }
    }
    // ── 3. the amplitude per 16-sample block, a ±1-symbol box, interpolated; subtract
    const int B = 16, len = kHi - kLo, nbk = (len + B - 1) / B, H = std::max(1, nsps_ / B);
    std::vector<float> br((size_t)nbk), bi((size_t)nbk), ar((size_t)nbk), ai((size_t)nbk);
    const float* xa = a + s0;
    for (int bk = 0; bk < nbk; ++bk) {
        const int k0 = kLo + bk * B, k1 = std::min(kHi, k0 + B);
        float sr = 0, si = 0;
        for (int k = k0; k < k1; ++k) { sr += xa[k] * rc[(size_t)k]; si -= xa[k] * rs[(size_t)k]; }
        br[(size_t)bk] = sr; bi[(size_t)bk] = si;
    }
    {
        double sr = 0, si = 0; int lo = 0, hi = 0, cnt = 0;
        auto blen = [&](int bk) { return std::min(B, len - bk * B); };
        for (int bk = 0; bk < nbk; ++bk) {
            const int wHi = std::min(nbk, bk + H), wLo = std::max(0, bk - H);
            while (hi < wHi) { sr += br[(size_t)hi]; si += bi[(size_t)hi]; cnt += blen(hi); ++hi; }
            while (lo < wLo) { sr -= br[(size_t)lo]; si -= bi[(size_t)lo]; cnt -= blen(lo); ++lo; }
            ar[(size_t)bk] = (float)(sr / cnt); ai[(size_t)bk] = (float)(si / cnt);
        }
    }
    float* xo = a + s0;
    for (int bk = 0; bk < nbk; ++bk) {
        const float cR = ar[(size_t)bk], cI = ai[(size_t)bk];
        const float pR = bk > 0 ? ar[(size_t)bk - 1] : cR, pI = bk > 0 ? ai[(size_t)bk - 1] : cI;
        const float nR = bk + 1 < nbk ? ar[(size_t)bk + 1] : cR, nI = bk + 1 < nbk ? ai[(size_t)bk + 1] : cI;
        const float aR0 = 0.5f * (pR + cR), aI0 = 0.5f * (pI + cI);
        const float dR = (0.5f * (cR + nR) - aR0) / B, dI = (0.5f * (cI + nI) - aI0) / B;
        const int k0 = kLo + bk * B, k1 = std::min(kHi, k0 + B);
        for (int k = k0; k < k1; ++k) {
            const float f = (float)(k - k0) + 0.5f;
            const float r = aR0 + dR * f, i = aI0 + dI * f;
            xo[k] -= 2.0f * (r * rc[(size_t)k] - i * rs[(size_t)k]);
        }
    }
}

void Js8SlotDecoder::decode(float* audio, int n, const Js8SlotOptions& o, std::vector<Js8Result>& out, Js8SlotStats& st) {
    st = Js8SlotStats{};
    const size_t first = out.size();
    struct Sub { uint8_t tones[kNN]; double start, f0; };
    std::vector<Sub> subs;
    const auto t0 = std::chrono::steady_clock::now();
    const int passes = std::max(1, std::min(2, o.maxPasses));
    const Js8SubmodeInfo& in = js8Info(sub_);
    for (int pass = 1; pass <= passes; ++pass) {
        if (pass > 1) {
            if (subs.empty()) break;
            if (o.mayRunPass && !o.mayRunPass(pass, msSince(t0), st.passMs[pass - 2])) break;
        }
        const auto tp = std::chrono::steady_clock::now();
        auto lap = [t = tp]() mutable { const auto now = std::chrono::steady_clock::now();
                                        const double d = std::chrono::duration<double, std::milli>(now - t).count(); t = now; return d; };
        for (const Sub& s : subs) subtract(audio, n, s.tones, s.start, s.f0);
        subs.clear();
        st.subtractMs += lap();
        buildWaterfall(audio, n);
        colMean_.assign((size_t)mon_.wf.block_stride, -1.0f);
        st.waterfallMs += lap();
        const int nc = search(o.maxCandidates, o.minScore);
        st.searchMs += lap();
        st.candidates[pass - 1] = nc;
        int found = 0;
        for (int i = 0; i < nc; ++i) {
            if (o.abort && o.abort->load(std::memory_order_relaxed)) { st.passes = pass; return; }
            const Cand& c = cands_[(size_t)i];
            float llr[js8::kN];
            likelihoods(c, llr);
            uint8_t cw[js8::kN];
            int err = js8::bpDecode(llr, o.ldpcIters, cw);
            st.ldpcMs += lap();
            bool viaOsd = false;
            uint8_t info[87];
            auto crcOk = [&] {
                for (int k = 0; k < 87; ++k) info[k] = cw[87 + k];
                uint16_t rx = 0;
                for (int k = 75; k < 87; ++k) rx = (uint16_t)((rx << 1) | info[k]);
                return js8::crc12(info) == rx;
            };
            bool ok = err >= 0 && crcOk();
            if (!ok && o.osdMaxErrors > 0 && c.score >= o.osdMinScore) {
                ++st.osdTried[pass - 1];
                err = js8::osd1(llr, o.osdMaxErrors, cw);
                st.osdMs += lap();
                ok = err >= 0 && crcOk();
                viaOsd = ok;
            }
            if (!ok) continue;
            /* ★ Guards a CRC-12 needs (1 in 4,096 of random codewords passes it): never the all-zero codeword, and a
             *  converged BP no more than 40 hard decisions from what was received (test-js8's noise run). */
            bool allZero = true;
            for (int k = 0; k < js8::kN && allZero; ++k) allZero = cw[k] == 0;
            if (allZero || err > 40) continue;
            bool dup = false;
            for (size_t r = first; r < out.size() && !dup; ++r) dup = std::memcmp(out[r].info, info, 87) == 0;
            if (dup) continue;
            Js8Result r;
            const int flags = info[72] << 2 | info[73] << 1 | info[74];
            if (!js8::unpack(info, flags, r.frame)) continue;
            std::memcpy(r.info, info, 87);
            r.submode = sub_;
            Sub s;
            js8Tones(info, sub_, s.tones);
            const ftx_waterfall_t& wf = mon_.wf;
            r.hz = (mon_.min_bin + c.f + (float)c.fs / wf.freq_osr) / T_;
            const double startSample = (double)(c.t - 1) * mon_.block_size + (double)c.ts * mon_.subblock_size;
            r.dt = (float)(startSample / rate_ - in.startDelay);
            r.snr = snrDb(c, s.tones);
            r.pass = pass; r.osd = viaOsd; r.hardErrors = err;
            out.push_back(r);
            s.start = startSample; s.f0 = r.hz;
            subs.push_back(s);
            ++found;
        }
        st.passes = pass;
        st.passMs[pass - 1] = msSince(tp);
        st.passDecodes[pass - 1] = found;
    }
}

// ── The live decoder ──────────────────────────────────────────────────────────────────────────────

Js8Decoder::Js8Decoder(int sampleRate, unsigned mask) : rate_(sampleRate) {
    int longest = 0;
    for (Js8Submode s : kJs8Submodes) {
        if (!(mask & js8Bit(s))) continue;
        Lane l;
        l.sub = s;
        l.dec.reset(new Js8SlotDecoder(sampleRate, s));
        l.work.assign((size_t)l.dec->windowSamples(), 0.0f);
        longest = std::max(longest, l.dec->windowSamples());
        lanes_.push_back(std::move(l));
    }
    ring_.assign((size_t)longest + (size_t)(3 * rate_), 0);
    worker_ = std::thread([this] { workerLoop(); });
}

Js8Decoder::~Js8Decoder() {
    abort_ = true;
    { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void Js8Decoder::process(const int16_t* in, int count, double utc) {
    ingest(count, utc, [in](int i) { return in[i]; });
}
void Js8Decoder::process(const float* in, int count, double utc) {
    ingest(count, utc, [in](int i) {
        const float v = in[i] * 32767.0f;
        return (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)std::lrint(v));
    });
}

template <typename F>
void Js8Decoder::ingest(int count, double captureUtc, F sample) {
    if (count <= 0 || lanes_.empty()) return;
    if (!std::isfinite(captureUtc)) captureUtc = vibe::vibeUtcNow();
    const int64_t R = (int64_t)ring_.size();
    for (int i = 0; i < count; ++i) ring_[(size_t)((total_ + i) % R)] = sample(i);
    total_ += count;
    // ★ The capture-clock anchor, exactly the FT8 decoder's (ft8_decoder.cpp): an earlier stamp wins at once, a later
    //   one leaks in over 30 s, a whole second later means audio was lost — re-anchor.
    const double est = captureUtc - (double)total_ / rate_;
    if (!anchored_ || est - utc0_ > 1.0) {
        if (anchored_) reanchors.fetch_add(1);
        utc0_ = est; anchored_ = true;
        for (Lane& l : lanes_) l.nextSlot = NAN;
        floor_ = total_ - count;
    } else if (est < utc0_) {
        utc0_ = est;
    } else {
        utc0_ += (est - utc0_) * std::min(1.0, (double)count / rate_ / 30.0);
    }
    cutWindows(captureUtc);
}

void Js8Decoder::cutWindows(double captureUtc) {
    const int64_t R = (int64_t)ring_.size();
    const int64_t oldest = std::max<int64_t>(floor_, total_ - R);
    for (size_t li = 0; li < lanes_.size(); ++li) {
        Lane& l = lanes_[li];
        const double period = js8Info(l.sub).period;
        const int win = l.dec->windowSamples();
        if (std::isnan(l.nextSlot)) l.nextSlot = std::ceil((utc0_ + (double)oldest / rate_) / period) * period;
        for (;;) {
            const int64_t i0 = (int64_t)std::llround((l.nextSlot - utc0_) * rate_);
            if (i0 < oldest) { l.nextSlot += period; continue; }
            if (total_ < i0 + win) break;
            std::lock_guard<std::mutex> lk(m_);
            if (l.queued) {
                slotsSkipped.fetch_add(1);           // ★ its last window is still waiting or decoding: skip, never wait
            } else {
                for (int i = 0; i < win; ++i) l.work[(size_t)i] = ring_[(size_t)((i0 + i) % R)] * (1.0f / 32768.0f);
                l.slotUtc = l.nextSlot; l.readyUtc = captureUtc; l.queued = true;
                jobs_.push_back((int)li);
                cv_.notify_one();
            }
            l.nextSlot += period;
        }
    }
}

void Js8Decoder::workerLoop() {
    vibe::vibeFlushDenormals();
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        cv_.wait(lk, [this] { return stop_ || !jobs_.empty(); });
        if (stop_) return;
        const int li = jobs_.front(); jobs_.pop_front();
        lk.unlock();
        runLane(lanes_[(size_t)li]);
        lk.lock();
        lanes_[(size_t)li].queued = false;
    }
}

void Js8Decoder::runLane(Lane& l) {
    const auto t0 = std::chrono::steady_clock::now();
    const double periodMs = js8Info(l.sub).period * 1000.0;
    const double lagMs = std::max(0.0, std::min(periodMs, (vibe::vibeUtcNow() - l.readyUtc) * 1000.0));
    const bool loaded = loadProbe ? loadProbe() : false;
    Js8SlotOptions o;
    o.maxPasses = loaded ? 1 : std::max(1, std::min(2, maxPasses.load()));
    o.osdMaxErrors = (loaded || o.maxPasses < 2) ? 0 : 26;
    o.abort = &abort_;
    /* ★ A second pass only when it would still end inside 40 % of what is left of this period, the box is not loaded,
     *  and no other submode's window is waiting behind this one (they share the worker). */
    o.mayRunPass = [&](int, double elapsed, double last) {
        if (loadProbe && loadProbe()) return false;
        { std::lock_guard<std::mutex> lk(m_); if (!jobs_.empty()) return false; }
        return elapsed + 1.5 * last < 0.40 * (periodMs - lagMs);
    };
    std::vector<Js8Result> res;
    Js8SlotStats st;
    l.dec->decode(l.work.data(), (int)l.work.size(), o, res, st);
    lastDecodeMs.store(msSince(t0));
    slotsDecoded.fetch_add(1);
    if (abort_.load()) return;
    for (const Js8Result& r : res) {
        Js8Spot s;
        s.slotUtc = l.slotUtc; s.submode = l.sub; s.frame = r.frame;
        s.snr = r.snr; s.audioHz = r.hz; s.dt = r.dt;
        if (onDecode) onDecode(s);
    }
}

} // namespace vibe
