// decoders/selcal_decoder.h — SELCAL, the aeronautical selective call (V12, 2026-10-31).
//
// ★★★ THE STANDARD: ICAO Annex 10, Volume III, Part II, Chapter 3 as amended by Amendment 91 (adopted 6 November 2020,
//     applicable 3 November 2022) — its text reproduced in ICAO's own, freely published working paper "Amendment 91 to
//     Annex 10 Volume III on Selective Calling Codes" (APAC CNS SG/28, WP/21, 2024, icao.int), read 2026-10-31:
//       • "two consecutive tone pulses, with each pulse containing two simultaneously transmitted tones";
//       • pulses "1.0 plus or minus 0.25 seconds", "separated by an interval of 0.2 plus or minus 0.1 second";
//       • tones held to ±0.15 %; the two tones within 3 dB;
//       • Table 3-1: 32 tones, Red A–S (A–H, J–M, P–S — 312.6 … 1479.1 Hz) and, from 3 November 2022 ("SELCAL32"),
//         Red T–Z and 1–9 (329.2 … 1557.8 Hz), spaced by log⁻¹ 0.0225 (5.3 %) "to avoid … harmonic combinations".
//     The code is written as the two pulses' designations, "AB-CD". Within a pulse the order is ours: by Table 3-1's
//     order (A…S, then T…9). A designation does not repeat within a code (assignment practice; a pair with the same
//     tone twice cannot be a pulse anyway).
//
// ★★ THE RECEIVER (audio at any rate; the host gives 48 kHz): box-average to 8 kHz; every 50 ms a 200 ms Hann window
//    measured at the 32 tone frequencies (Goertzel at the 32 tones × three dial offsets (−4, 0, +4 Hz), 24 groups of 4 lanes — NEON / SSE2 / scalar, identical);
//    a window is a PULSE CANDIDATE when, at the best offset, its two strongest tones are ≥ 9 dB over the median of the 32,
//    within 6 dB of each other, and the third is ≥ 8 dB under the weaker; a pulse = the same pair for 0.6–1.5 s; a code = two
//    pulses with different tones, the gap between them 0.05–0.6 s (the windows smear the 0.1–0.3 s gap). One report per
//    code per 20 s (ground stations often call twice).
// ★★ THE DIAL: tones are measured at their exact frequencies; at 312 Hz the next SELCAL32 tone is 16.6 Hz away, so the
//    SSB dial must be within ≈ ±5 Hz (measured in the test). VibeServer dials are that good with a reference; noted in the hint.
// ★ COST: 96 Goertzels × 1600 samples × 20 windows a second ≈ 3 M multiply-adds a second, four tones a lane. No divide or
//   round per sample.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace vibe {

class SelcalDecoder {
public:
    struct Code { std::string code; float pulse1 = 0, pulse2 = 0, gap = 0; float snrDb = 0; };
    explicit SelcalDecoder(double rateHz);
    void feed(const float* x, int n);
    std::function<void(const Code&)> onCode;
    std::function<void(int)> onState;   ///< 0 listening · 1 a pulse is sounding

    static constexpr int kTones = 32;
    static const double kHz[kTones];
    static const char kName[kTones];    ///< 'A' … 'S', 'T' … 'Z', '1' … '9'
    /** Table 3-1's index of a designation, or −1. */
    static int indexOf(char c);

private:
    void window();
    void step(int a, int b, float snr);
    double fs_;
    int dec_ = 6, decN_ = 0; float decAcc_ = 0;
    int N_ = 2000, hop_ = 400;
    std::vector<float> hann_, ring_; int rPos_ = 0, filled_ = 0, sinceHop_ = 0;
    std::vector<float> coef_;           // 2 cos ω per tone (32)
    std::vector<float> win_;
    double t_ = 0;
    // pulse tracking
    int curA_ = -1, curB_ = -1; double curStart_ = 0, curEnd_ = 0; int curN_ = 0;
    int pA_ = -1, pB_ = -1; double pStart_ = 0, pEnd_ = 0; bool havePulse_ = false;
    float snrAcc_ = 0;
    std::string last_; double lastT_ = -1e9;
    int state_ = 0;
};

} // namespace vibe
