// vibe_acars.h — V12 VHF ACARS: every channel from one capture, coherent MSK, the block, the BCS, a soft repair.
// docs/v12/AVIATION.md. The front end is vibe_avi_front.h; the service (thread, list, JSON) vibe_avi_service.h.
//
// ★★★ WRITTEN FRESH FROM PUBLIC FACTS. The free Wavecom decoder documentation (the modulation: MSK on AM, 1200/2400 Hz
//     tones, 2400 bit/s; the block: pre-key, '+' '*' SYN SYN, SOH, mode, address, ack, label, block id, STX, text,
//     ETX/ETB, BCS, DEL; 7-bit ASCII + odd parity, LSB first). The BCS is CRC-16 with the reflected 0x1021 polynomial and a
//     zero preset ("KERMIT"), as libacars (MIT) has it — and confirmed on the air: a real block run through the register
//     with its BCS leaves zero (docs/v12/AVIATION.md §ACARS). No GPL decoder was read for this file.
//
// ★★ THE BIT CONVENTION, MEASURED ON A REAL CAPTURE (131.725 MHz, Pi 500, 2026-10-31): a 1200 Hz bit TOGGLES the data
//    bit, a 2400 Hz bit keeps it (the all-ones pre-key is one steady 2400 Hz tone). So a tone detector would have to
//    integrate its decisions — one wrong tone inverts every bit after it. Instead:
// ★★★ COHERENT MSK AS BPSK. Mix the AM audio down by 1800 Hz, filter with the MSK matched pulse (a half cosine two bits
//     long), sample at each bit instant k and turn the sample back by k quarter turns (the 600 Hz deviation): what is
//     left is ±e^{jθ} — BPSK whose sign IS the data bit (data = the XOR of the tones, exactly as measured). θ is
//     estimated data-free from the squared samples over ~16 bits and unwrapped; its π ambiguity is the polarity, which the
//     sync word settles. No error propagation, a real soft value per bit, ~3 dB better than a tone detector.
// ★★ TIMING: four bit clocks a quarter bit apart, each with its own θ and deframer (vibe_ais.h's answer); a block that
//    any of them gets through the BCS counts once.
// ★★ REPAIR (Chase): a parity failure says WHICH character is wrong, the soft values say which BIT; the two least reliable
//    bits of each failing character (≤ 3 characters) are tried, with the two weakest BCS bits, and only a BCS pass with
//    every parity good is accepted (≤ 24 tries against a 16-bit check: < 1 false block in 2700 bad ones, and a bad one
//    needs a sync word first).
#pragma once
#include "vibe_avi_front.h"
#include "vibedsp/simd_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace vibeacars {

using cf32 = vibedsp::cf32;
static constexpr double kBaud = 2400.0;
static constexpr double kSubHz = 1800.0;     ///< the MSK centre (1200 / 2400 Hz tones)

/** The ACARS BCS: CRC-16, reflected 0x1021 (0x8408), preset 0, no final XOR. A block run through it WITH its BCS (low
 *  byte first) leaves 0. */
inline uint16_t bcs(const uint8_t* d, size_t n, uint16_t c = 0) {
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int b = 0; b < 8; ++b) c = (c & 1) ? uint16_t((c >> 1) ^ 0x8408) : uint16_t(c >> 1);
    }
    return c;
}
inline bool oddParity(uint8_t b) { return (__builtin_popcount(b) & 1) == 1; }
inline uint8_t withParity(uint8_t c7) { c7 &= 0x7F; return oddParity(c7) ? c7 : uint8_t(c7 | 0x80); }

static constexpr uint8_t SOH = 0x01, STX = 0x02, ETX = 0x03, ETB = 0x17, DEL = 0x7F, SYN = 0x16, NAK = 0x15;

/** The 40 sync bits '+' '*' SYN SYN SOH (parity in place), LSB first, as a 40-bit word (first bit on air = bit 39). */
inline uint64_t syncWord() {
    const uint8_t s[5] = { withParity('+'), withParity('*'), withParity(SYN), withParity(SYN), withParity(SOH) };
    uint64_t w = 0;
    for (int i = 0; i < 5; ++i) for (int b = 0; b < 8; ++b) w = (w << 1) | ((s[i] >> b) & 1);
    return w;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  A BLOCK
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Block {
    std::vector<uint8_t> raw;      ///< mode … suffix, parity stripped (no SOH, no BCS)
    char mode = 0, ack = 0, blockId = 0;
    std::string reg, label, msgNo, flight, text;
    bool downlink = false, more = false;   ///< more = ETB (another block of this message follows)
    int repaired = 0;              ///< bits flipped by the repair
    float levelDb = -120, snrDb = 0;
    int channel = -1;
    double hz = 0;
    double t = 0;                  ///< seconds since the stream began (block end)
};

/** Parse a BCS-good block (`b` = parity-stripped mode … suffix). False = not a well-formed block. */
inline bool parse(const std::vector<uint8_t>& b, Block& out) {
    if (b.size() < 13) return false;
    out = Block();
    out.raw = b;
    out.mode = char(b[0]);
    out.reg.assign(reinterpret_cast<const char*>(&b[1]), 7);
    out.ack = char(b[8]);
    out.label.assign(reinterpret_cast<const char*>(&b[9]), 2);
    out.blockId = char(b[11]);
    const uint8_t suf = b.back();
    out.more = suf == ETB;
    // the address: '.' pads a registration on the left
    size_t p = out.reg.find_first_not_of('.');
    out.reg = p == std::string::npos ? std::string() : out.reg.substr(p);
    while (!out.reg.empty() && out.reg.back() == ' ') out.reg.pop_back();
    for (char c : out.reg) if (c < 0x20 || c > 0x7E) return false;
    out.downlink = out.blockId >= '0' && out.blockId <= '9';
    std::string txt;
    if (b[12] == STX) txt.assign(reinterpret_cast<const char*>(&b[13]), b.size() - 14);
    else if (b[12] != ETX && b[12] != ETB) return false;
    if (out.downlink && txt.size() >= 10) {
        out.msgNo = txt.substr(0, 4);
        out.flight = txt.substr(4, 6);
        txt = txt.substr(10);
    }
    out.text = txt;
    return true;
}

/** ★ Experiment switches (test-acars only; the defaults are the shipped receiver). */
struct Tuning { bool repair = true; bool twoSided = true; int W = 16; int phases = 4; int maxBad = 4, cand = 2; };
inline Tuning& tuning() { static Tuning t; return t; }

/** A repaired block must also LOOK like a block: `by` with parity, the suffix at `e`. Mode and label printable, the
 *  address of registration characters (A–Z 0–9 . -), a block id that is a digit or a letter, the text printable (CR LF
 *  allowed). A clean BCS pass is never second-guessed; this only gates the repair. */
inline bool plausible(const std::vector<uint8_t>& by, size_t e) {
    auto c = [&](size_t i) { return char(by[i] & 0x7F); };
    auto pr = [](char x) { return x >= 0x20 && x < 0x7F; };
    if (!pr(c(0))) return false;
    for (size_t i = 1; i <= 7; ++i) { const char x = c(i); if (!((x >= 'A' && x <= 'Z') || (x >= '0' && x <= '9') || x == '.' || x == '-')) return false; }
    if (!(pr(c(9)) || c(9) == DEL) || !(pr(c(10)) || c(10) == DEL)) return false;
    const char b = c(11);
    if (!((b >= '0' && b <= '9') || (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z'))) return false;
    if (e > 12) {
        if (c(12) != STX) return false;
        for (size_t i = 13; i < e; ++i) { const char x = c(i); if (!pr(x) && x != '\r' && x != '\n') return false; }
    }
    return true;
}

/** Every character's parity good and the BCS (over mode … suffix, then the two BCS bytes) leaves zero. */
inline bool good(const std::vector<uint8_t>& by, size_t e) {
    for (size_t i = 0; i <= e; ++i) if (!oddParity(by[i])) return false;
    return bcs(by.data(), e + 3) == 0;
}
/** ★ Chase, by syndrome: repair `by` (with parity, the suffix at `e`, its BCS after) from the soft bits `S` (8 a
 *  character, + = 1). True = repaired in place (`flipsOut` bits). Tuning::maxBad / cand bound the search. */
inline bool chaseRepair(std::vector<uint8_t>& by, const std::vector<float>& S, size_t e, int& flipsOut) {
        // ★ Chase, by syndrome. The BCS is linear (preset 0, no final XOR), so flipping bit j changes the residue by a
        //   fixed δ_j: each candidate's δ is computed once and a combination is an XOR of them. Candidates: the `cand`
        //   weakest bits of each character whose parity failed (≤ maxBad of them), and none / one of the two weakest BCS
        //   bits. ★ Accepted only when EXACTLY ONE combination clears the residue and the block it makes is plausible
        //   (printable, a well-formed address and label) — two solutions mean the flips are guessing.
        std::vector<size_t> badc;
        for (size_t i = 0; i < e; ++i) if (!oddParity(by[i])) { badc.push_back(i); if (int(badc.size()) > tuning().maxBad) return false; }
        const int K = std::max(1, std::min(3, tuning().cand));
        auto weakest = [&](size_t bit0, size_t nbits, int k, size_t* idx) {
            std::vector<std::pair<float, size_t>> v;
            for (size_t j = bit0; j < bit0 + nbits; ++j) v.push_back({ std::fabs(S[j]), j });
            std::partial_sort(v.begin(), v.begin() + k, v.end());
            for (int q = 0; q < k; ++q) idx[q] = v[size_t(q)].second;
        };
        const size_t len = e + 3;
        auto delta = [&](size_t bit) { std::vector<uint8_t> u(len, 0); u[bit / 8] = uint8_t(1u << (bit % 8)); return bcs(u.data(), len); };
        const uint16_t res = bcs(by.data(), len);
        std::vector<std::array<size_t, 3>> cb(badc.size());
        std::vector<std::array<uint16_t, 3>> cd(badc.size());
        for (size_t i = 0; i < badc.size(); ++i) { weakest(badc[i] * 8, 8, K, cb[i].data()); for (int q = 0; q < K; ++q) cd[i][size_t(q)] = delta(cb[i][size_t(q)]); }
        size_t bw[2]; weakest((e + 1) * 8, 16, 2, bw);
        const uint16_t bd[3] = { 0, delta(bw[0]), delta(bw[1]) };
        size_t combos = 1; for (size_t i = 0; i < badc.size(); ++i) combos *= size_t(K);
        int found = 0; size_t fc = 0; int fo = 0;
        for (size_t c = 0; c < combos && found < 2; ++c)
            for (int o = 0; o < 3; ++o) {
                if (badc.empty() && o == 0) continue;
                uint16_t r = res ^ bd[o];
                size_t m = c;
                for (size_t i = 0; i < badc.size(); ++i) { r ^= cd[i][m % size_t(K)]; m /= size_t(K); }
                if (r == 0) { if (++found == 1) { fc = c; fo = o; } else break; }
            }
        if (found != 1) return false;
        std::vector<uint8_t> t = by;
        int flips = 0;
        size_t m = fc;
        for (size_t i = 0; i < badc.size(); ++i) { const size_t bit = cb[i][m % size_t(K)]; m /= size_t(K); t[bit / 8] ^= uint8_t(1u << (bit % 8)); ++flips; }
        if (fo) { t[bw[fo - 1] / 8] ^= uint8_t(1u << (bw[fo - 1] % 8)); ++flips; }
        if (!good(t, e) || !plausible(t, e)) return false;
        by = t; flipsOut = flips;
        return true;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE SIMD KERNEL: the AM envelope, the carrier level taken out, mixed down by the MSK centre
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** out[i] = (|z[i]| − dc) · lo[i], lo = the 1800 Hz phasor table (re/im planar per call). The DC (the AM carrier) is a
 *  running value updated once a call from the block's mean, so the loop is pure lane work. NEON / SSE2 / scalar give the
 *  same values to float rounding (test-acars). */
inline void envMix(const cf32* z, int n, float dc, const float* loRe, const float* loIm, cf32* out, float* envSum) {
    const float* zf = reinterpret_cast<const float*>(z);
    float* of = reinterpret_cast<float*>(out);
    float sum = 0;
    int i = 0;
#if VIBE_NEON
    float32x4_t acc = vdupq_n_f32(0), vdc = vdupq_n_f32(dc);
    for (; i + 4 <= n; i += 4) {
        float32x4x2_t v = vld2q_f32(zf + 2 * i);
        float32x4_t p = vmlaq_f32(vmulq_f32(v.val[0], v.val[0]), v.val[1], v.val[1]);
        // sqrt: reciprocal-sqrt estimate + two Newton steps, × p (0 stays 0)
        float32x4_t r = vrsqrteq_f32(vmaxq_f32(p, vdupq_n_f32(1e-30f)));
        r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(p, r), r));
        r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(p, r), r));
        float32x4_t e = vmulq_f32(p, r);
        acc = vaddq_f32(acc, e);
        float32x4_t a = vsubq_f32(e, vdc);
        float32x4x2_t o;
        o.val[0] = vmulq_f32(a, vld1q_f32(loRe + i));
        o.val[1] = vmulq_f32(a, vld1q_f32(loIm + i));
        vst2q_f32(of + 2 * i, o);
    }
    sum = vaddvq_f32(acc);
#elif VIBE_SSE
    __m128 acc = _mm_setzero_ps(), vdc = _mm_set1_ps(dc);
    for (; i + 4 <= n; i += 4) {
        __m128 re, im;
        vibedsp::sseLoad2(zf + 2 * i, re, im);
        __m128 e = _mm_sqrt_ps(_mm_add_ps(_mm_mul_ps(re, re), _mm_mul_ps(im, im)));
        acc = _mm_add_ps(acc, e);
        __m128 a = _mm_sub_ps(e, vdc);
        vibedsp::sseStore2(of + 2 * i, _mm_mul_ps(a, _mm_loadu_ps(loRe + i)), _mm_mul_ps(a, _mm_loadu_ps(loIm + i)));
    }
    sum = vibedsp::sseAddv(acc);
#endif
    for (; i < n; ++i) {
        const float e = std::sqrt(zf[2 * i] * zf[2 * i] + zf[2 * i + 1] * zf[2 * i + 1]);
        sum += e;
        const float a = e - dc;
        of[2 * i] = a * loRe[i];
        of[2 * i + 1] = a * loIm[i];
    }
    *envSum = sum;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  ONE CHANNEL
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool debug() { static const bool d = std::getenv("ACARS_DEBUG") != nullptr; return d; }

struct Stats { uint64_t syncs = 0, bcsFail = 0, good = 0, repaired = 0, dupes = 0; };

class Demod {
public:
    static constexpr int kPhases = 8;   ///< the most bit clocks (Tuning::phases runs)
    explicit Demod(double fs) : fs_(fs), sps_(fs / kBaud) {
        // the 1800 Hz table: the shortest whole number of cycles (≤ 8192 samples), else a rotator refills it per call
        const double cyc = kSubHz / fs;
        loLen_ = 0;
        for (int L = 1; L <= 8192; ++L) { const double c = cyc * L; if (std::fabs(c - std::round(c)) < 1e-9) { loLen_ = L; break; } }
        if (loLen_ == 0) loLen_ = 4096;
        exact_ = std::fabs(cyc * loLen_ - std::round(cyc * loLen_)) < 1e-9;
        loRe_.resize(size_t(loLen_) * 2 + 4096); loIm_.resize(loRe_.size());
        fillLo_(0.0);
        // the MSK matched filter: cos(π t / 2T), |t| < T (its first null falls on the carrier, 1800 Hz away)
        const int half = int(std::floor(sps_));
        std::vector<float> taps;
        for (int k = -half; k <= half; ++k) taps.push_back(float(std::cos(M_PI * k / (2 * sps_))));
        float s = 0; for (float t : taps) s += t;
        for (float& t : taps) t /= s;
        mf_.reset(new vibedsp::FirDecimator(taps, 1));
        delay_ = half;
        nph_ = std::max(1, std::min(kPhases, tuning().phases));
        for (int p = 0; p < nph_; ++p) { ph_[p].clk = double(p) / nph_; }
        sync_ = syncWord();
    }
    /** `out(const Block&)` for every block that passed the BCS (repaired or not). */
    template <class Out> void feed(const cf32* z, int n, Out&& out) {
        if (n <= 0) return;
        mix_.resize(size_t(n)); y_.resize(size_t(n) + 4);
        // the 1800 Hz phasors for this call's samples
        const float* lr; const float* li;
        if (exact_) {
            if (loPos_ + n > int(loRe_.size())) loPos_ %= loLen_;
            lr = loRe_.data() + loPos_; li = loIm_.data() + loPos_;
            loPos_ = (loPos_ + n) % loLen_;
        } else {
            if (n > int(loRe_.size())) { loRe_.resize(size_t(n)); loIm_.resize(size_t(n)); }
            fillLo_(loPh_);
            loPh_ = std::fmod(loPh_ + kSubHz / fs_ * n, 1.0);
            lr = loRe_.data(); li = loIm_.data();
        }
        float esum = 0;
        envMix(z, n, dc_, lr, li, mix_.data(), &esum);
        // the carrier level (AM DC): a block mean, smoothed; it also normalises (AGC) below
        const float mean = esum / n;
        dc_ = dcInit_ ? dc_ + (mean - dc_) * std::min(1.0f, n / (0.004f * float(fs_))) : mean;
        dcInit_ = true;
        const int k = mf_->process(mix_.data(), n, y_.data());
        // noise floor: the slowest-falling minimum of the carrier level (seconds of quiet between bursts)
        const float lvl = dc_ * dc_ + 1e-20f;
        floor_ = floor_ <= 0 ? lvl : (lvl < floor_ ? floor_ + (lvl - floor_) * 0.2f : floor_ * (1.0f + 0.0005f * n / 1000.0f));
        const float inv = 1.0f / std::max(dc_, 1e-9f);
        for (int i = 0; i < k; ++i) {
            const cf32 v = y_[size_t(i)] * inv;
            for (int p = 0; p < nph_; ++p) {
                Path& P = ph_[p];
                P.clk += 1.0 / sps_;
                if (P.clk >= 1.0) {
                    P.clk -= 1.0;
                    const float back = float(P.clk * sps_);   // samples since the bit instant (0…1)
                    const cf32 x = v - (v - prev_) * back;
                    bit_(P, x, out);
                }
            }
            prev_ = v;
            ++t_;
        }
    }
    const Stats& stats() const { return st_; }
    /** When each BCS failure happened (seconds; for the offline tools' "lost burst" count). */
    const std::vector<double>& failTimes() const { return fails_; }
    float levelDb() const { return 10 * std::log10(dc_ * dc_ + 1e-20f); }
    float floorDb() const { return 10 * std::log10(floor_ + 1e-20f); }
    void setChannel(int c, double hz) { chan_ = c; hz_ = hz; }

private:
    struct Path {
        double clk = 0;
        int k = 0;                       // bit index (for the quarter-turn derotation)
        std::complex<float> sq{0, 0};     // smoothed w² (θ)
        float theta = 0;
        uint64_t sr = 0;                 // last 64 hard bits
        int have = 0;
        // frame collection
        bool inFrame = false; bool inv = false;
        std::vector<float> soft;         // bits since SOH (signed: + = 1), causal θ
        cf32 hist[64];                   // the last 64 derotated samples (the sync word and the pre-key before SOH)
        std::vector<cf32> wv;            // derotated samples: 64 before SOH, then every bit since
        std::vector<float> soft2;        // ★ the frame again with a TWO-SIDED θ (Tuning::twoSided)
    };
    static constexpr int kMaxChars = 240;

    void fillLo_(double ph0) {
        const double cyc = kSubHz / fs_;
        for (size_t i = 0; i < loRe_.size(); ++i) {
            const double a = -2 * M_PI * (ph0 + cyc * double(i));
            loRe_[i] = float(std::cos(a)); loIm_[i] = float(std::sin(a));
        }
    }

    template <class Out> void bit_(Path& P, cf32 x, Out& out) {
        // derotate by k quarter turns: (−j)^k
        cf32 w;
        switch (P.k & 3) { case 0: w = x; break; case 1: w = cf32(x.imag(), -x.real()); break;
                           case 2: w = -x; break; default: w = cf32(-x.imag(), x.real()); }
        P.k++;
        P.hist[P.k & 63] = w;
        if (P.inFrame) P.wv.push_back(w);
        // θ from w² (data-free), smoothed over ~16 bits, the half-angle unwrapped next to the last
        P.sq += (w * w - P.sq) * 0.0625f;
        float th = 0.5f * std::arg(P.sq);
        float d = th - P.theta;
        while (d > float(M_PI / 2)) { th -= float(M_PI); d -= float(M_PI); }
        while (d < -float(M_PI / 2)) { th += float(M_PI); d += float(M_PI); }
        P.theta = th;
        if (P.theta > float(4 * M_PI) || P.theta < -float(4 * M_PI)) P.theta = std::remainder(P.theta, float(2 * M_PI));
        const float amp = std::sqrt(std::abs(P.sq)) + 1e-9f;
        const float soft = (w.real() * std::cos(P.theta) + w.imag() * std::sin(P.theta)) / amp;
        const int hard = soft > 0;
        P.sr = (P.sr << 1) | uint64_t(hard);
        if (P.have < 64) P.have++;
        if (!P.inFrame) {
            if (P.have >= 40) {
                const uint64_t m = (1ull << 40) - 1, s = P.sr & m;
                const int e0 = __builtin_popcountll(s ^ sync_), e1 = __builtin_popcountll(s ^ (~sync_ & m));
                if (e0 <= 3 || e1 <= 3) {
                    P.inFrame = true; P.inv = e1 < e0; P.soft.clear(); st_.syncs++;
                    P.wv.clear();
                    for (int j = 63; j >= 0; --j) P.wv.push_back(P.hist[(P.k - j) & 63]);
                }
            }
            return;
        }
        P.soft.push_back(P.inv ? -soft : soft);
        const size_t nb = P.soft.size();
        if (nb % 8) return;
        // a character is complete
        const size_t nc = nb / 8;
        const uint8_t c = charAt_(P.soft, nc - 1);
        bool end = false;
        if (nc >= 16) {
            // count recent parity failures: the carrier has gone
            int bad = 0;
            for (size_t j = nc - 6; j < nc; ++j) bad += !oddParity(charAt_(P.soft, j));
            if (bad >= 4) end = true;
        }
        if (nc >= kMaxChars) end = true;
        // fast path: a suffix three characters back, its BCS complete, DEL now
        if (nc >= 16 && !end) {
            const uint8_t s3 = uint8_t(charAt_(P.soft, nc - 4) & 0x7F);
            if ((s3 == ETX || s3 == ETB) && (c & 0x7F) == DEL) {
                if (tryEnd_(P, nc - 4, false, out)) { P.inFrame = false; P.have = 0; return; }
            }
        }
        if (end) { finish_(P, out); P.inFrame = false; P.have = 0; }
    }
    static uint8_t charAt_(const std::vector<float>& s, size_t i) {
        uint8_t v = 0;
        for (int b = 0; b < 8; ++b) if (s[i * 8 + size_t(b)] > 0) v |= uint8_t(1u << b);
        return v;
    }
    /** At the end of a frame (the carrier gone, or too long): every plausible suffix position, with repair. */
    template <class Out> void finish_(Path& P, Out& out) {
        const size_t nc = P.soft.size() / 8;
        int tried = 0;
        for (size_t e = 12; e + 2 < nc && tried < 4; ++e) {
            const uint8_t s = uint8_t(charAt_(P.soft, e) & 0x7F);
            const bool delAfter = e + 3 < nc && __builtin_popcount(unsigned(charAt_(P.soft, e + 3) ^ DEL)) <= 1;
            if (s == ETX || s == ETB || delAfter) {
                ++tried;
                if (tryEnd_(P, e, tuning().repair, out)) return;
            }
        }
        st_.bcsFail++;
        if (fails_.size() < 100000) fails_.push_back(double(t_) / fs_);
        if (debug()) {
            int bad = 0; for (size_t j = 0; j < nc; ++j) bad += !oddParity(charAt_(P.soft, j));
            std::string txt; for (size_t j = 0; j < nc && j < 60; ++j) { const char ch = char(charAt_(P.soft, j) & 0x7F); txt += (ch >= 0x20 && ch < 0x7F) ? ch : '~'; }
            std::fprintf(stderr, "  [acars ch%d t=%.2f clk%d] BCS fail: %zu chars, %d parity bad, lvl %.1f snr %.1f |%s\n", chan_, double(t_) / fs_,
                         int(&P - ph_), nc, bad, levelDb(), levelDb() - floorDb(), txt.c_str());
        }
    }
    /** The block ends with its suffix at character `e`: check the BCS, repair if allowed. */
    template <class Out> bool tryEnd_(Path& P, size_t e, bool repair, Out& out) {
        const size_t nb = (e + 3) * 8;
        if (P.soft.size() < nb) return false;
        if (tuning().twoSided) { resoft_(P); if (tryEndOn_(P, P.soft2, e, repair, out)) return true; }
        return tryEndOn_(P, P.soft, e, repair, out);
    }
    /** ★ The frame's soft bits again, θ from w² summed over ±W bits around each (the causal estimate lags a drifting
     *  phase and is noisiest exactly where the frame starts). Unwrapped bit to bit; the polarity is the sync word's. */
    void resoft_(Path& P) {
        const int W = std::max(2, tuning().W);
        const size_t n = P.wv.size();
        P.soft2.assign(P.soft.size(), 0.f);
        std::vector<cf32> sq(n + 1, cf32(0, 0));
        for (size_t i = 0; i < n; ++i) sq[i + 1] = sq[i] + P.wv[i] * P.wv[i];
        float prevTh = 0; bool first = true;
        for (size_t i = 0; i < n; ++i) {
            const size_t a = i >= size_t(W) ? i - size_t(W) : 0, b = std::min(n, i + size_t(W) + 1);
            const cf32 m = sq[b] - sq[a];
            float th = 0.5f * std::arg(m);
            if (!first) {
                float d = th - prevTh;
                while (d > float(M_PI / 2)) { th -= float(M_PI); d -= float(M_PI); }
                while (d < -float(M_PI / 2)) { th += float(M_PI); d += float(M_PI); }
            }
            first = false; prevTh = th;
            if (i < 64) continue;
            const size_t j = i - 64;                       // bit j after SOH
            if (j >= P.soft2.size()) break;
            const float amp = std::sqrt(std::abs(m) / float(b - a)) + 1e-9f;
            const float v = (P.wv[i].real() * std::cos(th) + P.wv[i].imag() * std::sin(th)) / amp;
            P.soft2[j] = v;
        }
        // the polarity: whichever agrees with the causal decisions (which the sync word set)
        float agree = 0;
        for (size_t j = 0; j < P.soft.size() && j < P.soft2.size(); ++j) agree += (P.soft[j] > 0) == (P.soft2[j] > 0) ? 1.f : -1.f;
        if (agree < 0) for (auto& v : P.soft2) v = -v;
    }
    template <class Out> bool tryEndOn_(Path& P, const std::vector<float>& S, size_t e, bool repair, Out& out) {
        std::vector<uint8_t> by(e + 3);
        for (size_t i = 0; i < e + 3; ++i) by[i] = charAt_(S, i);
        // the suffix itself: force a damaged one to the nearer of ETX / ETB
        uint8_t& sf = by[e];
        if ((sf & 0x7F) != ETX && (sf & 0x7F) != ETB) {
            if (!repair) return false;
            sf = __builtin_popcount(unsigned(sf ^ withParity(ETX))) <= __builtin_popcount(unsigned(sf ^ withParity(ETB))) ? withParity(ETX) : withParity(ETB);
        }
        if (good(by, e)) { emit_(by, e, 0, out); return true; }
        if (!repair) return false;
        int flips = 0;
        if (!chaseRepair(by, S, e, flips)) return false;
        emit_(by, e, flips, out); st_.repaired++;
        return true;
    }
    template <class Out> void emit_(const std::vector<uint8_t>& by, size_t e, int flips, Out& out) {
        std::vector<uint8_t> b(e + 1);
        for (size_t i = 0; i <= e; ++i) b[i] = by[i] & 0x7F;
        // one block, four clocks: the same bytes within a second are one
        uint32_t h = 2166136261u;
        for (uint8_t v : b) h = (h ^ v) * 16777619u;
        for (auto& r : recent_) if (r.h == h && t_ - r.t < uint64_t(fs_)) { st_.dupes++; return; }
        recent_[rp_] = { h, t_ }; rp_ = (rp_ + 1) % 8;
        Block blk;
        if (!parse(b, blk)) return;
        blk.repaired = flips;
        blk.levelDb = levelDb();
        blk.snrDb = levelDb() - floorDb();
        blk.channel = chan_; blk.hz = hz_;
        blk.t = double(t_) / fs_;
        st_.good++;
        out(blk);
    }

    double fs_, sps_;
    int loLen_ = 0, loPos_ = 0; bool exact_ = true; double loPh_ = 0;
    std::vector<float> loRe_, loIm_;
    std::unique_ptr<vibedsp::FirDecimator> mf_;
    int delay_ = 0;
    std::vector<cf32> mix_, y_;
    float dc_ = 0, floor_ = 0; bool dcInit_ = false;
    cf32 prev_{0, 0};
    Path ph_[kPhases];
    int nph_ = 4;
    uint64_t sync_ = 0, t_ = 0;
    struct Rec { uint32_t h = 0; uint64_t t = 0; };
    Rec recent_[8]; int rp_ = 0;
    Stats st_;
    std::vector<double> fails_;
    int chan_ = -1; double hz_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE WHOLE CAPTURE: the shared channeliser + a demodulator per channel
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class Receiver {
public:
    Receiver(double rate, double hwCentreHz, const std::vector<double>& chanHz)
        : mc_(rate, hwCentreHz, chanHz, 12500, 15000) {
        for (size_t c = 0; c < chanHz.size(); ++c) {
            demod_.emplace_back(new Demod(mc_.chanRate()));
            demod_.back()->setChannel(int(c), chanHz[c]);
        }
    }
    template <class Out> void feed(const cf32* iq, size_t n, Out&& out) {
        mc_.feed(iq, n, [&](int c, const cf32* z, int k) { demod_[size_t(c)]->feed(z, k, out); });
    }
    const vibeavi::MultiChannel& front() const { return mc_; }
    const Demod& demod(size_t c) const { return *demod_[c]; }
    size_t channels() const { return demod_.size(); }

private:
    vibeavi::MultiChannel mc_;
    std::vector<std::unique_ptr<Demod>> demod_;
};

}  // namespace vibeacars
