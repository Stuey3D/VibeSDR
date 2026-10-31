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
/** ★ Experiment switches (test-acars only; the defaults are the shipped receiver). */
struct Tuning { bool repair = true; };
inline Tuning& tuning() { static Tuning t; return t; }
inline bool debug() { static const bool d = std::getenv("ACARS_DEBUG") != nullptr; return d; }

struct Stats { uint64_t syncs = 0, bcsFail = 0, good = 0, repaired = 0, dupes = 0; };

class Demod {
public:
    static constexpr int kPhases = 4;
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
        for (int p = 0; p < kPhases; ++p) { ph_[p].clk = double(p) / kPhases; }
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
            for (int p = 0; p < kPhases; ++p) {
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
        std::vector<float> soft;         // bits since SOH (signed: + = 1)
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
        std::vector<uint8_t> by(e + 3);
        for (size_t i = 0; i < e + 3; ++i) by[i] = charAt_(P.soft, i);
        // the suffix itself: force a damaged one to the nearer of ETX / ETB
        uint8_t& sf = by[e];
        if ((sf & 0x7F) != ETX && (sf & 0x7F) != ETB) {
            if (!repair) return false;
            sf = __builtin_popcount(unsigned(sf ^ withParity(ETX))) <= __builtin_popcount(unsigned(sf ^ withParity(ETB))) ? withParity(ETX) : withParity(ETB);
        }
        if (good_(by, e)) { emit_(by, e, 0, out); return true; }
        if (!repair) return false;
        // Chase: the failing characters (≤ 3), each one of its two weakest bits, × the BCS's two weakest bits
        std::vector<size_t> badc;
        for (size_t i = 0; i < e; ++i) if (!oddParity(by[i])) { badc.push_back(i); if (badc.size() > 3) return false; }
        auto weakest2 = [&](size_t bit0, size_t nbits, size_t& a, size_t& b) {
            a = b = bit0;
            float fa = 1e9f, fb = 1e9f;
            for (size_t j = bit0; j < bit0 + nbits; ++j) {
                const float v = std::fabs(P.soft[j]);
                if (v < fa) { b = a; fb = fa; a = j; fa = v; } else if (v < fb) { b = j; fb = v; }
            }
        };
        std::vector<std::pair<size_t, size_t>> cand(badc.size());
        for (size_t i = 0; i < badc.size(); ++i) weakest2(badc[i] * 8, 8, cand[i].first, cand[i].second);
        size_t ba, bb;
        weakest2((e + 1) * 8, 16, ba, bb);
        const size_t bcsOpt[3] = { SIZE_MAX, ba, bb };
        const unsigned combos = 1u << badc.size();
        for (unsigned m = 0; m < combos; ++m)
            for (int o = 0; o < 3; ++o) {
                if (m == 0 && o == 0) continue;
                std::vector<uint8_t> t = by;
                int flips = 0;
                for (size_t i = 0; i < badc.size(); ++i) {
                    const size_t bit = (m >> i) & 1 ? cand[i].second : cand[i].first;
                    t[bit / 8] ^= uint8_t(1u << (bit % 8)); ++flips;
                }
                if (bcsOpt[o] != SIZE_MAX) { t[bcsOpt[o] / 8] ^= uint8_t(1u << (bcsOpt[o] % 8)); ++flips; }
                if (good_(t, e)) { emit_(t, e, flips, out); st_.repaired++; return true; }
            }
        return false;
    }
    /** Every character's parity good and the BCS (over mode … suffix, then the two BCS bytes) leaves zero. */
    static bool good_(const std::vector<uint8_t>& by, size_t e) {
        for (size_t i = 0; i <= e; ++i) if (!oddParity(by[i])) return false;
        return bcs(by.data(), e + 3) == 0;
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
