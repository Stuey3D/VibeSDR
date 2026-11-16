// decoders/tones_dtmf.h — V12 DTMF receiver (docs/v12/TONES.md), on 8 kHz audio from decoders/tones_dsp.h.
//
// ★★★ THE STANDARD: ITU-T Q.23 (the tone pairs) and Q.24 (the receiver), both free from itu.int:
//     • low group 697, 770, 852, 941 Hz × high group 1209, 1336, 1477, 1633 Hz → 1 2 3 A / 4 5 6 B / 7 8 9 C / * 0 # D;
//     • tones within ±1.5 % MUST be accepted, ±3.5 % (and beyond) must not;
//     • a signal of 40 ms and more must be accepted, one under ~23 ms must not; a pause of 40 ms separates two digits, and
//       a break of 10 ms or less inside a digit must be bridged;
//     • twist (the level of the high-group tone minus the low-group tone) is an administration's choice in Q.24 — the
//       telephone figures are a few dB either way. ★ OURS: −4 … +8 dB (high − low). A sender lifts the high group ~2 dB and
//       over radio a pre-/de-emphasis mismatch tilts it a few dB more either way, so the high side is generous; the LOW side
//       is held to 4 dB because speech is the other way round — every speech-like false digit we measured had the low tone
//       4–7 dB louder (test-tones pins −3.5 and +7 dB read, −6 and +10 dB refused).
//
// ★★ THE RECEIVER: every 5 ms a 205-sample (25.6 ms) block of the 8 kHz audio, mean removed, measured by a Goertzel bank
//    — for each of the eight tones three points (its frequency and ±19.5 Hz, half a bin either side, so a tone ±1.5 % off
//    still reads full strength and its offset can be measured) and its second harmonic: 32 filters, 8 groups of 4 lanes.
//    A block is a DIGIT when, for the strongest tone of each group:
//      1. the two tones carry ≥ 75 % of the block's energy (talk-off: speech and music spread their energy; a dialled
//         string at 10 dB SNR in 3 kHz still reads every digit);
//      2. the next tone in each group is ≥ 8 dB down (one tone per group);
//      3. twist within −4 … +8 dB;
//      4. each tone's measured offset within ±2 % of its frequency (between Q.24's must-accept 1.5 % and must-reject
//         3.5 %);
//      5. each tone's second harmonic ≥ 10 dB down (voiced speech and instruments are rich in harmonics; DTMF is not), and
//         the high tone not at twice the low one (a note and its own harmonic — see below).
// ★★ TIMING FROM THE BLOCKS: a block counts as "on" for a digit when it passes AND holds at least half the strongest
//    block's power for that digit (a block only partly over the tone reads weaker). With 25.6 ms blocks every 5 ms, a tone of
//    D ms gives ≈ (D − 10.6) / 5 + 1 on blocks, so the tone's length is (on − 1) × 5 + 10.6 ms; a digit needs ≥ 30 ms by that
//    measure (40 ms tones pass, 25 ms ones do not). A run of off blocks inside a digit is a break of ≈ off × 5 − 10.6 ms:
//    under 20 ms it is bridged, longer and it is two digits (Q.24: ≤ 10 bridged, ≥ 40 separate).
// ★ One SEQUENCE = digits less than 1.5 s apart (what a person dials or a repeater is sent), reported when it ends; at most
//   40 digits (a longer run is reported and a new one started). Every structure is bounded.
#pragma once
#include "decoders/tones_dsp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vibe {
namespace tones {

class Dtmf {
public:
    static constexpr int kN = 205, kHop = 40;
    static constexpr double kFs = 8000.0;
    static constexpr double kLow[4] = { 697, 770, 852, 941 };
    static constexpr double kHigh[4] = { 1209, 1336, 1477, 1633 };
    static constexpr const char* kKeys = "123A456B789C*0#D";   // [low × 4 + high]
    static constexpr double kTwistLoDb = -4.0, kTwistHiDb = 8.0;
    double fracMin = 0.75;             ///< (tuning) the two tones' share of the block's energy
    double tolRel = 0.02;             ///< (tuning) each tone's offset allowed

    struct Sequence {
        std::string digits;
        double toneMs = 0, gapMs = 0;   ///< mean tone length and gap between digits (ms)
        double twistDb = 0;             ///< mean high − low (dB)
        double startS = 0, endS = 0;    ///< in the decoder's own clock (s since it started)
    };
    std::function<void(const Sequence&)> onSequence;
    std::function<void(int)> onState;   ///< 1 a digit is sounding · 0 not
    bool scalar = false;                ///< the test's scalar run (identical results)

    Dtmf() {
        const double half = 0.5 * kFs / kN;
        for (int t = 0; t < 8; ++t) {
            const double f = t < 4 ? kLow[t] : kHigh[t - 4];
            const double fr[4] = { f - half, f, f + half, 2 * f };
            for (int k = 0; k < 4; ++k) c_[t * 4 + k] = (float)(2 * std::cos(2 * M_PI * fr[k] / kFs));
        }
        ring_.assign(kN, 0.f);
    }

    void feed(const float* x, int n) {
        for (int i = 0; i < n; ++i) {
            ring_[(size_t)rp_] = x[i];
            if (++rp_ == kN) rp_ = 0;
            if (filled_ < kN) ++filled_;
            ++t_;
            if (++since_ >= kHop) { since_ = 0; if (filled_ == kN) block(); }
        }
    }
    /** Finish whatever is open (end of a test clip / the decoder stopping). */
    void flush() {
        if (cur_ >= 0) closeRun(run_.size());
        if (!seq_.empty()) emit();
    }

private:
    struct Win { int8_t d; float p; float twist; };

    void block() {
        float w[kN];
        double mean = 0;
        for (int i = 0; i < kN; ++i) mean += ring_[(size_t)((rp_ + i) % kN)];
        mean /= kN;
        for (int i = 0; i < kN; ++i) w[i] = ring_[(size_t)((rp_ + i) % kN)] - (float)mean;
        const float e = energy(w, kN);
        float pw[32];
        if (scalar) goertzelScalar(w, kN, c_, 32, pw); else goertzel(w, kN, c_, 32, pw);
        Win r{ -1, 0.f, 0.f };
        if (e > 1e-9f) {
            const double norm = (double)e * kN / 2;   // a pure tone carrying all the energy reads 1.0
            auto best = [&](int g, int& idx, double& p, double& off) {
                idx = -1; p = 0;
                for (int t = g * 4; t < g * 4 + 4; ++t) {
                    const double pk = std::max({ (double)pw[t * 4], (double)pw[t * 4 + 1], (double)pw[t * 4 + 2] });
                    if (pk > p) { p = pk; idx = t; }
                }
                const double a = std::log(pw[idx * 4] + 1e-30), b = std::log(pw[idx * 4 + 1] + 1e-30), c = std::log(pw[idx * 4 + 2] + 1e-30);
                const double d = a - 2 * b + c;
                double v = d < 0 ? 0.5 * (a - c) / d : (c > a ? 1.5 : -1.5);
                v = std::max(-1.5, std::min(1.5, v));
                off = v * 0.5 * kFs / kN;   // Hz
            };
            int lo, hi; double pl, ph, ol, oh;
            best(0, lo, pl, ol); best(1, hi, ph, oh);
            const double fl = kLow[lo], fh = kHigh[hi - 4];
            bool ok = (pl + ph) / norm >= fracMin;
            for (int t = 0; t < 4 && ok; ++t) if (t != lo && pw[t * 4 + 1] > 0.16 * pl) ok = false;
            for (int t = 4; t < 8 && ok; ++t) if (t != hi && pw[t * 4 + 1] > 0.16 * ph) ok = false;
            const double tw = 10 * std::log10((ph + 1e-30) / (pl + 1e-30));
            if (ok && (tw < kTwistLoDb || tw > kTwistHiDb)) ok = false;
            if (ok && (std::fabs(ol) > tolRel * fl || std::fabs(oh) > tolRel * fh)) ok = false;
            if (ok && (pw[lo * 4 + 3] > 0.1 * pl || pw[hi * 4 + 3] > 0.1 * ph)) ok = false;
            // ★ a note and its own second harmonic (G#5 830.6 Hz and 1661 Hz sit within 2.5 % of 852 / 1633): the measured
            //   high tone at twice the measured low one is an instrument, not a keypad — no DTMF pair is within 4 % of 2:1
            if (ok && std::fabs((fh + oh) / (fl + ol) - 2.0) < 0.03) ok = false;
            if (ok) { r.d = (int8_t)(lo * 4 + (hi - 4)); r.p = (float)(pl + ph); r.twist = (float)tw; }
        }
        step(r);
    }

    void step(const Win& w) {
        const double now = t_ / kFs;
        if (cur_ < 0) {
            if (w.d >= 0) { start(w); return; }
            if (!seq_.empty() && now - lastEnd_ > 1.5) emit();
            return;
        }
        if (run_.size() < kRunMax) run_.push_back(w);
        if (w.d == cur_) { peak_ = std::max(peak_, w.p); other_ = 0; if (w.p >= 0.5f * peak_) off_ = 0; else ++off_; return; }
        ++off_;
        if (w.d >= 0) {
            if (++other_ >= 2) {   // ★ another digit straight after: close this one before its first two blocks
                const size_t keep = run_.size() >= 2 ? run_.size() - 2 : 0;
                std::vector<Win> tail(run_.begin() + (long)keep, run_.end());
                closeRun(keep);
                start(tail[0]);
                for (size_t i = 1; i < tail.size(); ++i) step(tail[i]);
            }
            return;
        }
        other_ = 0;
        if (off_ >= kEndOff) closeRun(run_.size());
    }
    void start(const Win& w) {
        cur_ = w.d; run_.clear(); run_.push_back(w); peak_ = w.p; off_ = 0; other_ = 0;
        runStart_ = t_ / kFs;
        setState(1);
    }
    /** Split the run's first `n` blocks into digits (on blocks, breaks bridged under 20 ms) and keep those ≥ 30 ms. */
    void closeRun(size_t n) {
        const int d = cur_;
        cur_ = -1;
        setState(0);
        const double hopMs = 1000.0 * kHop / kFs, nMs = 1000.0 * kN / kFs;
        float pk = 0;
        for (size_t i = 0; i < n; ++i) if (run_[i].d == d) pk = std::max(pk, run_[i].p);
        std::vector<char> on(n);
        for (size_t i = 0; i < n; ++i) on[i] = run_[i].d == d && run_[i].p >= 0.5f * pk;
        size_t i = 0;
        while (i < n) {
            while (i < n && !on[i]) ++i;
            if (i >= n) break;
            size_t a = i, b = i;   // [a, b] — on blocks with bridged breaks
            double tw = 0; int ntw = 0;
            size_t j = i;
            while (j < n) {
                if (on[j]) { b = j; tw += run_[j].twist; ++ntw; ++j; continue; }
                size_t k = j; while (k < n && !on[k]) ++k;
                if (k < n && (k - j) * hopMs - 0.414 * nMs < 20.0) { j = k; continue; }
                break;
            }
            const double durMs = (double)(b - a) * hopMs + 0.414 * nMs;
            if (durMs >= 30.0) {
                // ★ the block index → time: block `a` of this run ended at runStart_ + a hops; the tone began ≈ 0.707 N before
                const double ts = runStart_ + a * hopMs / 1000.0 - 0.707 * nMs / 1000.0;
                const double te = ts + durMs / 1000.0;
                if (!seq_.empty()) gapAcc_ += std::max(0.0, ts - lastEnd_) * 1000.0, ++gapN_;
                else seqStart_ = ts;
                if (seq_.size() < kSeqMax) seq_ += kKeys[d];
                toneAcc_ += durMs; twAcc_ += ntw ? tw / ntw : 0; ++toneN_;
                lastEnd_ = te;
                if (seq_.size() >= kSeqMax) emit();
            }
            i = b + 1;
        }
    }
    void emit() {
        Sequence s;
        s.digits = seq_; s.toneMs = toneN_ ? toneAcc_ / toneN_ : 0; s.gapMs = gapN_ ? gapAcc_ / gapN_ : 0;
        s.twistDb = toneN_ ? twAcc_ / toneN_ : 0; s.startS = seqStart_; s.endS = lastEnd_;
        seq_.clear(); toneAcc_ = gapAcc_ = twAcc_ = 0; toneN_ = gapN_ = 0;
        if (onSequence && !s.digits.empty()) onSequence(s);
    }
    void setState(int s) { if (s != state_) { state_ = s; if (onState) onState(s); } }

    static constexpr size_t kRunMax = 600;   // 3 s of blocks — a held key is one digit
    static constexpr size_t kSeqMax = 40;
    static constexpr int kEndOff = 14;       // 70 ms of off blocks closes a digit
    float c_[32];
    std::vector<float> ring_;
    int rp_ = 0, filled_ = 0, since_ = 0;
    long long t_ = 0;
    int cur_ = -1, off_ = 0, other_ = 0, state_ = 0;
    float peak_ = 0;
    double runStart_ = 0;
    std::vector<Win> run_;
    std::string seq_;
    double seqStart_ = 0, lastEnd_ = -1e9, toneAcc_ = 0, gapAcc_ = 0, twAcc_ = 0;
    int toneN_ = 0, gapN_ = 0;
};

} // namespace tones
} // namespace vibe
