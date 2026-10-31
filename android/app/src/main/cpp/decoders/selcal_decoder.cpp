// decoders/selcal_decoder.cpp — see selcal_decoder.h. SELCAL from ICAO Annex 10 Vol III Part II Ch 3 (Amendment 91).
#include "decoders/selcal_decoder.h"
#include "decoders/digimodes/q65_simd.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vibe {

namespace { constexpr double kWinSec = 0.2; const double kOffHz[3] = { -4.0, 0.0, 4.0 }; }

// ★ Table 3-1, in its own order (Red A … Red S, then the SELCAL32 tones Red T … Red 9)
const double SelcalDecoder::kHz[kTones] = {
    312.6, 346.7, 384.6, 426.6, 473.2, 524.8, 582.1, 645.7, 716.1, 794.3, 881.0, 977.2, 1083.9, 1202.3, 1333.5, 1479.1,
    329.2, 365.2, 405.0, 449.3, 498.3, 552.7, 613.1, 680.0, 754.2, 836.6, 927.9, 1029.2, 1141.6, 1266.2, 1404.4, 1557.8 };
const char SelcalDecoder::kName[kTones] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'J', 'K', 'L', 'M', 'P', 'Q', 'R', 'S',
    'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '1', '2', '3', '4', '5', '6', '7', '8', '9' };

int SelcalDecoder::indexOf(char c) { for (int i = 0; i < kTones; ++i) if (kName[i] == c) return i; return -1; }

SelcalDecoder::SelcalDecoder(double rateHz) {
    dec_ = std::max(1, (int)std::floor(rateHz / 8000.0 + 0.5));
    fs_ = rateHz / dec_;
    N_ = (int)std::floor(kWinSec * fs_ + 0.5);
    hop_ = (int)std::floor(0.05 * fs_ + 0.5);
    hann_.resize((size_t)N_); ring_.assign((size_t)N_, 0.f); win_.resize((size_t)N_);
    for (int i = 0; i < N_; ++i) hann_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / N_));
    // ★ three dial offsets (−4, 0, +4 Hz): a mistuned SSB dial moves BOTH tones by the same Hz, so the pair is judged at
    //   whichever offset holds it best — 96 Goertzels a window, still four a lane
    coef_.resize((size_t)kTones * 3);
    for (int o = 0; o < 3; ++o)
        for (int k = 0; k < kTones; ++k) coef_[(size_t)(o * kTones + k)] = (float)(2.0 * std::cos(2 * M_PI * (kHz[k] + kOffHz[o]) / fs_));
}

void SelcalDecoder::feed(const float* x, int n) {
    const float inv = 1.0f / (float)dec_;
    for (int i = 0; i < n; ++i) {
        decAcc_ += x[i];
        if (++decN_ < dec_) continue;
        const float v = decAcc_ * inv; decAcc_ = 0; decN_ = 0;
        ring_[(size_t)rPos_] = v;
        if (++rPos_ == N_) rPos_ = 0;
        if (filled_ < N_) ++filled_;
        t_ += 1.0 / fs_;
        if (++sinceHop_ >= hop_) { sinceHop_ = 0; if (filled_ == N_) window(); }
    }
}

void SelcalDecoder::window() {
    using namespace q65v;
    // unwrap the ring, remove the mean, weight
    double mean = 0;
    for (int i = 0; i < N_; ++i) mean += ring_[(size_t)i];
    mean /= N_;
    float e = 0;
    for (int i = 0; i < N_; ++i) {
        const float v = (ring_[(size_t)((rPos_ + i) < N_ ? rPos_ + i : rPos_ + i - N_)] - (float)mean) * hann_[(size_t)i];
        win_[(size_t)i] = v; e += v * v;
    }
    // ★ Goertzel, four tones a lane (24 groups: 32 tones × 3 offsets): s = x + c·s1 − s2; power = s1² + s2² − c·s1·s2
    float pw[kTones * 3];
    for (int g = 0; g < kTones * 3 / 4; ++g) {
        const V c = ld(&coef_[(size_t)g * 4]);
        V s1 = set1(0.f), s2 = set1(0.f);
        for (int i = 0; i < N_; ++i) {
            const V s = add(set1(win_[(size_t)i]), sub(mul(c, s1), s2));
            s2 = s1; s1 = s;
        }
        const V p = sub(add(mul(s1, s1), mul(s2, s2)), mul(mul(c, s1), s2));
        st(&pw[g * 4], p);
    }
    (void)e;
    /* ★★ A PULSE: at the best offset, two tones ≥ 9 dB over the floor (the median of the 32 at that offset), within 6 dB of
     *  each other, and the third ≥ 8 dB under the weaker. The floor is the tones' own — so a loud band (voice) raises it. */
    bool pulse = false; float snr = 0; int a = -1, b = -1;
    float bestScore = 0;
    for (int o = 0; o < 3; ++o) {
        const float* q = &pw[o * kTones];
        int x1 = -1, x2 = -1, x3 = -1;
        for (int k = 0; k < kTones; ++k) {
            if (x1 < 0 || q[k] > q[x1]) { x3 = x2; x2 = x1; x1 = k; }
            else if (x2 < 0 || q[k] > q[x2]) { x3 = x2; x2 = k; }
            else if (x3 < 0 || q[k] > q[x3]) x3 = k;
        }
        float srt[kTones]; for (int k = 0; k < kTones; ++k) srt[k] = q[k];
        std::nth_element(srt, srt + kTones / 2, srt + kTones);
        const float floor = srt[kTones / 2] + 1e-20f;
        const bool ok = q[x2] >= 8.0f * floor && q[x2] >= 0.25f * q[x1] && q[x3] <= 0.16f * q[x2];
        if (ok && q[x2] / floor > bestScore) {
            bestScore = q[x2] / floor; pulse = true; a = x1; b = x2;
            snr = 10.0f * std::log10(bestScore);
        }
    }
    if (pulse) { if (a > b) std::swap(a, b); step(a, b, snr); }
    else step(-1, -1, 0);
}

void SelcalDecoder::step(int a, int b, float snr) {
    const double tc = t_ - 0.5 * kWinSec;   // the window's centre
    const double hop = hop_ / fs_;
    auto closeRun = [&]() {
        if (curA_ < 0) return;
        const double dur = (curEnd_ - curStart_) + hop + 0.15;   // ★ windows mostly inside a pulse cover ≈ D − 0.15 s
        const int ra = curA_, rb = curB_;
        const double s0 = curStart_, s1 = curEnd_;
        const float snrMean = curN_ ? snrAcc_ / (float)curN_ : 0.f;
        curA_ = curB_ = -1; curN_ = 0; snrAcc_ = 0;
        // ★ a window spanning a short gap holds one tone of each pulse — a brief "pair" (≤ 0.4 s) there is the change-over,
        //   not a new pulse, and must not throw the first pulse away
        if (dur < 0.6) { if (dur > 0.4 + hop) havePulse_ = false; return; }
        if (dur > 1.6) { havePulse_ = false; return; }
        if (havePulse_) {
            const double gap = s0 - pEnd_;
            const bool disjoint = ra != pA_ && ra != pB_ && rb != pA_ && rb != pB_;
            if (gap >= 0.5 * hop && gap <= 0.75 && disjoint) {   // ★ a 0.1 s gap is often one hop between the runs
                Code c;
                c.code = std::string() + kName[pA_] + kName[pB_] + "-" + kName[ra] + kName[rb];
                c.pulse1 = (float)((pEnd_ - pStart_) + hop + 0.15); c.pulse2 = (float)dur; c.gap = (float)std::max(0.0, gap - 0.15);
                c.snrDb = snrMean;
                havePulse_ = false;
                if (c.code != last_ || t_ - lastT_ > 20.0) { last_ = c.code; lastT_ = t_; if (onCode) onCode(c); }
                else lastT_ = t_;
                return;
            }
        }
        pA_ = ra; pB_ = rb; pStart_ = s0; pEnd_ = s1; havePulse_ = true;
    };
    if (a >= 0 && a == curA_ && b == curB_) { curEnd_ = tc; ++curN_; snrAcc_ += snr; return; }
    if (a < 0 && curA_ >= 0 && tc - curEnd_ <= hop * 1.5) return;   // ★ one window lost to noise inside a pulse is bridged
    closeRun();
    if (havePulse_ && tc - pEnd_ > 0.9) havePulse_ = false;          // no second pulse came
    if (a >= 0) { curA_ = a; curB_ = b; curStart_ = curEnd_ = tc; curN_ = 1; snrAcc_ = snr; }
    const int st = curA_ >= 0 ? 1 : 0;
    if (st != state_) { state_ = st; if (onState) onState(st); }
}

} // namespace vibe
