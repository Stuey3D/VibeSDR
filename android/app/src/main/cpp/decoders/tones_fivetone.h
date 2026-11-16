// decoders/tones_fivetone.h — V12 5-tone sequential selective calling (docs/v12/TONES.md), on 8 kHz audio.
//
// ★★★ THE TABLES (sixteen tones each: digits 0–9 and A–F; E is the REPEAT tone):
//     CCIR, EEA, EIA, ZVEI-1, ZVEI-2, ZVEI-3, DZVEI, PZVEI — as published in Wikipedia's "Selcall" article (Tone Sets
//     table, read 2026-11-16). The ZVEI family's figures were checked against the Signal Identification Wiki's "ZVEI Table"
//     (a transcription of a manufacturer's table, read the same day): ZVEI-1, ZVEI-2 and PZVEI agree; ★ ZVEI-3 and DZVEI
//     DO NOT — that table gives ZVEI-3 DZVEI's digits (2200 Hz for 0) with 2400 Hz as the repeat, and DZVEI's A–F
//     differently. We use Wikipedia's complete table and say so (TONES.md, decisions owed). The family is what matters:
//     digits from the same table read the same, whichever ZVEI variant is named.
// ★★ TONE PERIOD (each tone sent back to back, the next immediately after): ZVEI family 70 ± 15 ms (the ZVEI table above),
//    CCIR 100 ± 10 ms (Wikipedia, "CCIR (selcall)"). EEA 40 ms and EIA 33 ms are the periods radio programming manuals
//    give; we found no primary source for them, so the period only BREAKS TIES between tables with the same digits — it
//    never refuses a call. Any period from 25 to 130 ms is read.
// ★★ THE REPEAT RULE: a digit equal to the one before it is sent as E (so no two neighbouring tones are the same); E is read
//    back as the digit before it ("123E4" → 12334).
//
// ★★ THE RECEIVER: every 5 ms a 20 ms Hann-windowed block, measured on a grid of 204 Goertzel filters 400–2937.5 Hz, 12.5 Hz
//    apart (51 groups of 4 lanes — NEON / SSE2 / scalar, identical). A block holds ONE TONE when its strongest point (its
//    frequency refined between the grid points) carries ≥ 60 % of the block's energy. Blocks of one tone (each within
//    ±1.5 % of the run's mean) make a SEGMENT; segments one after another (≤ 45 ms apart — the blocks that straddle a
//    change hold two tones and are not counted) make a SEQUENCE, closed by 70 ms with no tone. A sequence is believed when:
//      • it has 5–12 tones (5-tone calls, with a status digit or two, ANI with a repeater opening);
//      • its tone period (the median spacing of the segments' centres) is 25–130 ms and every spacing is within ±35 % of it;
//      • every tone is within ±2 % of one table's tone, the whole sequence from ONE table (each table's tones are 6 % or
//        more apart, so a tone cannot sit within 2 % of two of one table's tones).
//    Of the tables that fit: the one that needs the fewest A–F tones other than its repeat (a call is digits); then the
//    nearest nominal period; then the order above. `pin` reads one table only (the manual control under AUTO).
// ★★ WHAT AUTO CANNOT TELL APART, by the tables themselves: ZVEI-1 and PZVEI (they differ only in A and C), ZVEI-2 and
//    ZVEI-3 (B and C), CCIR and EEA (A, C, F — the period decides), and DZVEI from the rest of the ZVEI family: DZVEI's digit
//    n is ZVEI's tone for n − 1, so every DZVEI call is also a ZVEI call one digit lower. AUTO names the first in the order
//    above and lists the others with what each reads (alsoFits / alsoDigits); a system known to be DZVEI is read right by
//    pinning DZVEI.
// ★ Bounded: at most 16 segments a sequence; a sequence of more than 12 is thrown away whole.
#pragma once
#include "decoders/tones_dsp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace vibe {
namespace tones {

struct ToneTable { const char* id; const char* name; double periodMs; double hz[16]; };

/** ★ In tie-break order. Wikipedia "Selcall", Tone Sets (read 2026-11-16). */
inline const ToneTable* toneTables() {
    static const ToneTable t[8] = {
        { "zvei1", "ZVEI-1", 70,  { 2400, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000, 2200, 2800, 810, 970, 885, 2600, 680 } },
        { "zvei2", "ZVEI-2", 70,  { 2400, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000, 2200, 885, 825, 740, 680, 970, 2600 } },
        { "zvei3", "ZVEI-3", 70,  { 2400, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000, 2200, 885, 810, 2800, 680, 970, 2600 } },
        { "dzvei", "DZVEI",  70,  { 2200, 970, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000, 825, 740, 2600, 885, 2400, 680 } },
        { "pzvei", "PZVEI",  70,  { 2400, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000, 2200, 970, 810, 2800, 885, 2600, 680 } },
        { "ccir",  "CCIR",   100, { 1981, 1124, 1197, 1275, 1358, 1446, 1540, 1640, 1747, 1860, 2400, 930, 2247, 991, 2110, 1055 } },
        { "eea",   "EEA",    40,  { 1981, 1124, 1197, 1275, 1358, 1446, 1540, 1640, 1747, 1860, 1055, 930, 2400, 991, 2110, 2247 } },
        { "eia",   "EIA",    33,  { 600, 741, 882, 1023, 1164, 1305, 1446, 1587, 1728, 1869, 2151, 2433, 2010, 2292, 459, 1091 } },
    };
    return t;
}
constexpr int kToneTables = 8;
inline int tableIndex(const std::string& id) {
    for (int i = 0; i < kToneTables; ++i) if (id == toneTables()[i].id) return i;
    return -1;
}

class FiveTone {
public:
    static constexpr int kN = 160, kHop = 40, kGrid = 204;
    static constexpr double kFs = 8000.0, kF0 = 400.0, kDf = 12.5;
    static constexpr double kFracMin = 0.6;

    struct Call {
        int table = -1;            ///< toneTables() index
        std::string raw;           ///< as sent ("123E4")
        std::string digits;        ///< the repeat rule applied ("12334")
        double periodMs = 0;
        std::vector<int> alsoFits; ///< other tables the same tones fit
        std::vector<std::string> alsoDigits;   ///< …and what each of them reads (DZVEI reads every ZVEI digit one higher)
        double startS = 0, endS = 0;
    };
    std::function<void(const Call&)> onCall;
    std::function<void(int)> onState;   ///< 1 tones sounding · 0 not
    bool scalar = false;
    int pin = -1;                       ///< −1 AUTO, else one table

    FiveTone() {
        for (int k = 0; k < kGrid; ++k) c_[k] = (float)(2 * std::cos(2 * M_PI * (kF0 + kDf * k) / kFs));
        for (int i = 0; i < kN; ++i) hann_[i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / kN));
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
    void flush() { closeSeg(); closeSeq(); }

private:
    struct Seg { double f = 0; int n = 0; double t0 = 0, t1 = 0; };

    void block() {
        float w[kN];
        double mean = 0;
        for (int i = 0; i < kN; ++i) mean += ring_[(size_t)((rp_ + i) % kN)];
        mean /= kN;
        for (int i = 0; i < kN; ++i) w[i] = (ring_[(size_t)((rp_ + i) % kN)] - (float)mean) * hann_[i];
        const float e = energy(w, kN);
        const double now = (t_ - kN / 2) / kFs;   // the block's centre
        double f = 0; bool tone = false;
        if (e > 1e-10f) {
            float pw[kGrid];
            if (scalar) goertzelScalar(w, kN, c_, kGrid, pw); else goertzel(w, kN, c_, kGrid, pw);
            int k = 0;
            for (int i = 1; i < kGrid; ++i) if (pw[i] > pw[k]) k = i;
            if (k > 0 && k < kGrid - 1) {
                const double a = std::log(pw[k - 1] + 1e-30), b = std::log(pw[k] + 1e-30), c = std::log(pw[k + 1] + 1e-30);
                const double v = vertex(a, b, c);
                const double peak = std::exp(b - 0.25 * (a - c) * v);
                f = kF0 + kDf * (k + v);
                // ★ a Hann-windowed tone carrying all the block's energy reads P = E·N/3
                tone = peak * 3.0 / ((double)e * kN) >= kFracMin;
            }
        }
        if (tone) {
            if (seg_.n > 0 && std::fabs(f - seg_.f) <= 0.015 * seg_.f) {
                seg_.f = (seg_.f * seg_.n + f) / (seg_.n + 1); ++seg_.n; seg_.t1 = now;
            } else {
                closeSeg();
                seg_.f = f; seg_.n = 1; seg_.t0 = seg_.t1 = now;
            }
            lastTone_ = now;
            setState(1);
        } else {
            if (seg_.n > 0 && now - seg_.t1 > 0.012) closeSeg();
            if (!segs_.empty() && now - lastTone_ > 0.070) closeSeq();
            if (segs_.empty() && seg_.n == 0) setState(0);
        }
    }
    void closeSeg() {
        if (seg_.n == 0) return;
        const Seg s = seg_; seg_ = Seg();
        if (s.n < 2) return;   // one block is a change-over, not a tone
        if (!segs_.empty() && s.t0 - segs_.back().t1 > 0.045) closeSeq();
        if (segs_.size() < kSegMax) segs_.push_back(s);
        else tooMany_ = true;
    }
    void closeSeq() {
        closeSeg();
        std::vector<Seg> s; s.swap(segs_);
        const bool over = tooMany_; tooMany_ = false;
        setState(0);
        if (over || s.size() < 5 || s.size() > 12) return;
        std::vector<double> sp;
        for (size_t i = 1; i < s.size(); ++i) sp.push_back(0.5 * (s[i].t0 + s[i].t1) - 0.5 * (s[i - 1].t0 + s[i - 1].t1));
        std::vector<double> srt = sp; std::sort(srt.begin(), srt.end());
        const double per = srt[srt.size() / 2];
        if (per < 0.025 || per > 0.130) return;
        for (double x : sp) if (std::fabs(x - per) > 0.35 * per) return;
        // ★ which table: every tone within 2 % of one of its tones
        int best = -1, bestExtra = 1 << 20; double bestDp = 1e9;
        std::vector<int> fits; std::vector<std::string> fitRaw; std::string bestRaw;
        for (int ti = 0; ti < kToneTables; ++ti) {
            if (pin >= 0 && ti != pin) continue;
            const ToneTable& tb = toneTables()[ti];
            std::string raw; int extra = 0, prevM = -1; bool ok = true;
            for (const Seg& g : s) {
                int m = -1;
                for (int d = 0; d < 16; ++d) if (std::fabs(g.f - tb.hz[d]) <= 0.02 * tb.hz[d]) { m = d; break; }
                // ★ an encoder never sends one tone twice running (that is what E is for): two segments on one tone are a
                //   wavering note, not a call
                if (m < 0 || m == prevM) { ok = false; break; }
                prevM = m;
                raw += "0123456789ABCDEF"[m];
                if (m >= 10 && m != 14) ++extra;
            }
            if (!ok) continue;
            fits.push_back(ti); fitRaw.push_back(raw);
            const double dp = std::fabs(per * 1000 - tb.periodMs);
            if (extra < bestExtra || (extra == bestExtra && dp < bestDp - 1e-9)) { best = ti; bestExtra = extra; bestDp = dp; bestRaw = raw; }
        }
        if (best < 0) return;
        Call c;
        c.table = best; c.raw = bestRaw; c.periodMs = per * 1000;
        c.digits = applyRepeats(bestRaw);
        for (size_t i = 0; i < fits.size(); ++i) if (fits[i] != best) { c.alsoFits.push_back(fits[i]); c.alsoDigits.push_back(applyRepeats(fitRaw[i])); }
        c.startS = s.front().t0; c.endS = s.back().t1;
        if (onCall) onCall(c);
    }
public:
    /** ★ The repeat rule: E is the digit before it ("123E4" → "12334"). */
    static std::string applyRepeats(const std::string& raw) {
        std::string out; char prev = 0;
        for (char ch : raw) { const char d = (ch == 'E' && prev) ? prev : ch; out += d; prev = d; }
        return out;
    }
private:
    void setState(int st) { if (st != state_) { state_ = st; if (onState) onState(st); } }

    static constexpr size_t kSegMax = 16;
    float c_[kGrid], hann_[kN];
    std::vector<float> ring_;
    int rp_ = 0, filled_ = 0, since_ = 0, state_ = 0;
    long long t_ = 0;
    Seg seg_;
    std::vector<Seg> segs_;
    bool tooMany_ = false;
    double lastTone_ = -1e9;
};

} // namespace tones
} // namespace vibe
