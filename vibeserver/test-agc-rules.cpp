// ★★★ THE THREE RULES FROM THE SONY'S TWO DROPS (vibe_agc_rules.h, 2026-10-02 21:23 and 21:25).
// Pure functions, so the evening's own numbers are the test: the auto-IF hold after a new start, the
// gross-overload shed, and the band key that carries a settled gain across a retune — plus the retune-forget
// distance (§4), pinned on Stuart's two-SSB-signals case.
//   build: c++ -std=c++17 -I ../android/app/src/main/cpp -o /tmp/tagc test-agc-rules.cpp
#include "vibe_agc_rules.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <cstdlib>

static int fails = 0, passes = 0;
static void ok(bool cond, const char* what) {
    if (cond) { ++passes; printf("   ok   %s\n", what); }
    else      { ++fails;  printf("   FAIL %s\n", what); }
}

int main() {
    using namespace vibeagc;
    printf("\nAuto-IF hold after a new start\n");
    // 21:23:02.5 DAB off; adjacent narrow fired at 21:23:03.9 (1.4 s) and was undone at 21:23:05.9.
    const double start = 1000.0, quiet = start + kAutoIfSettleSec;
    ok(autoIfHeld(start + 1.4, quiet), "1.4 s after the retune: held (the 21:23 narrowing could not happen)");
    ok(autoIfHeld(start + 3.4, quiet), "3.4 s: still held (its reversal was on numbers just as unsettled)");
    ok(!autoIfHeld(start + 6.0, quiet), "6 s: free — the ifGain EMA's ~5 s has passed");
    ok(!autoIfHeld(start + 600.0, quiet), "steady state: never held (nothing changes without a new start)");
    ok(!autoIfHeld(start, 0.0), "no new start yet (quietUntil 0): never held");

    printf("\nGross overload — one move, not 6 dB rungs\n");
    // 21:25: peak pinned at 0.0 dBFS, target -6 → computed shed 6 dB, while 56.6 % sat on the rail.
    ok(grossOverloadShedDb(6.0, 56.6) >= 18.0, "56.6 % on the rail: at least 18 dB at once (was 6.2)");
    ok(grossOverloadShedDb(6.0, 10.0) >= 18.0, "exactly 10 %: gross");
    ok(std::fabs(grossOverloadShedDb(6.0, 1.473) - 6.0) < 1e-9,
       "1.47 % (the 21:25:36 tail): the peak is a measurement again — computed shed stands");
    ok(std::fabs(grossOverloadShedDb(25.0, 56.6) - 25.0) < 1e-9, "a larger computed shed is never reduced");
    ok(std::fabs(grossOverloadShedDb(0.5, 0.0) - 0.5) < 1e-9, "no clipping: unchanged");
    // The evening's case end to end: 49.6 dB → 18 dB shed lands ≤ 31.6, then one sized move to 22.9.
    ok(49.6 - grossOverloadShedDb(6.0, 56.6) <= 31.7, "49.6 dB → ≤ 31.6 dB in the first write (four writes became two)");

    printf("\nBand keys — a settled gain belongs to its band\n");
    ok(agcBandKey(96.6e6) != agcBandKey(128.1167e6), "96.6 (FM) and 128.117 (airband) are different bands — the 21:25 jump crosses");
    ok(agcBandKey(96.6e6) == agcBandKey(107.7e6), "96.6 and 107.7 share the FM band — no seed within it");
    ok(agcBandKey(128.1167e6) == agcBandKey(118.0e6), "airband is one band");
    ok(agcBandKey(225.648e6) != agcBandKey(96.6e6), "Band III is not FM");
    ok(agcBandKey(87.5e6) == agcBandKey(96.6e6) && agcBandKey(87.49e6) != agcBandKey(96.6e6),
       "the FM band starts at 87.5 MHz");
    ok(agcBandKey(1.25e6) != agcBandKey(7.1e6), "MW and HF are different bands");
    ok(agcBandKey(1090e6) == agcBandKey(1766e6), "above 1 GHz is one band");

    printf("\nRetune — the channel always, the front end only on a real move (Stuart, 2026-10-20)\n");
    {
        // The widths the shim passes: max(2.5 kHz, the mode's bandwidth) — vibe_agc_rules.h §4.
        const double ssb = 2700.0, cw = 2500.0, am = 10000.0, nfm = 12500.0, wfm = 200000.0;
        ok(std::fabs(agcRetuneForgetHz(ssb) - 1350.0) < 1e-9, "SSB: the CHANNEL is a new one past 1.35 kHz (08-25 figure, unchanged)");
        ok(std::fabs(agcRetuneForgetHz(cw)  - 1250.0) < 1e-9, "CW: 1.25 kHz (the 2.5 kHz floor)");
        ok(std::fabs(agcRetuneForgetHz(am)  - 5000.0) < 1e-9 && std::fabs(agcRetuneForgetHz(nfm) - 6250.0) < 1e-9, "AM 5 kHz, NFM 6.25 kHz");
        ok(std::fabs(agcRetuneForgetHz(wfm) - 100000.0) < 1e-9, "WFM: 100 kHz");

        // The Portable: RTL V4, 2.4 MS/s, auto IF (0 = the capture), the tuner following the dial 15 kHz up.
        const double rate = 2.4e6, ifEff = agcIfEffectiveHz(0.0, rate), off = 15000.0;
        ok(std::fabs(ifEff - 2.4e6) < 1e-6 && std::fabs(agcIfEffectiveHz(1.4e6, rate) - 1.4e6) < 1e-6,
           "IF effective: auto = the capture, a set filter = itself");
        const double a = 14.200e6, b = 14.203e6;
        // ★ THE REPORT: 14.200 -> 14.203 USB, inside the window, tuner follows 3 kHz. Was a FULL forget.
        RetuneForget d = agcRetuneDecision(a, b, ssb, a + off, b + off, ifEff, false, false);
        ok(d == RetuneForget::Channel, "14.200 -> 14.203 USB: the front end is KEPT (channel only) — was a full forget");
        d = agcRetuneDecision(b, a, ssb, a + off, a + off, ifEff, false, false);
        ok(d == RetuneForget::Channel, "...and back again: kept");
        ok(agcRetuneDecision(a, a + 1000.0, ssb, a + off, a + 1000.0 + off, ifEff, false, false) == RetuneForget::None,
           "a 1 kHz touch-up: nothing forgotten at all");
        ok(agcRetuneDecision(a, b, wfm, a + off, b + off, ifEff, false, false) == RetuneForget::None, "3 kHz on WFM: nothing");
        // Hopping across a whole SSB segment, 40 hops of 3 kHz, tuner following: no full forget until the tuner
        // has travelled a quarter of the filter (600 kHz) from where the front end was learned.
        {
            double learned = a, anchor = a + off, f = a; int full = 0, chan = 0;
            for (int k = 0; k < 40; ++k) {
                const double nf = f + ((k & 1) ? -3000.0 : 3000.0) + 100.0;   // to and fro, drifting up 100 Hz a hop
                const RetuneForget r = agcRetuneDecision(learned, nf, ssb, anchor, nf + off, ifEff, false, false);
                if (r == RetuneForget::Full) { ++full; anchor = nf + off; }
                if (r != RetuneForget::None) { learned = nf; if (r == RetuneForget::Channel) ++chan; }
                f = nf;
            }
            ok(full == 0 && chan == 40, "40 hops between two close SSB signals: 40 channel re-learns, ZERO front-end forgets");
        }
        // Real moves.
        ok(agcRetuneDecision(a, 7.1e6, ssb, a + off, 7.1e6 + off, ifEff, true, false) == RetuneForget::Full,
           "20 m -> 40 m (the dial leaves the capture): full forget");
        ok(agcRetuneDecision(29.990e6, 30.010e6, ssb, 29.990e6 + off, 30.010e6 + off, ifEff, false, true) == RetuneForget::Full,
           "across a band edge (30 MHz): full forget, even 20 kHz away");
        ok(agcRetuneDecision(a, a + 700e3, ssb, a + off, a + 700e3 + off, ifEff, false, false) == RetuneForget::Full,
           "700 kHz inside a 2.4 MHz window, tuner following: past a quarter of the filter — full forget");
        ok(agcRetuneDecision(a, a + 700e3, ssb, a + off, a + off, ifEff, false, false) == RetuneForget::Channel,
           "the same 700 kHz with MANUAL IF (the tuner stays put): channel only");
        ok(agcRetuneDecision(a, a + 400e3, ssb, a + off, a + 400e3 + off, agcIfEffectiveHz(1.4e6, rate), false, false)
               == RetuneForget::Full,
           "a narrower 1.4 MHz filter: 400 kHz is past its quarter (350 kHz) — full");
        ok(agcRetuneDecision(96.1e6, 96.3e6, wfm, 96.1e6 + off, 96.3e6 + off, ifEff, false, false) == RetuneForget::Channel,
           "FM 96.1 -> 96.3: re-learn the station, keep the overload memory");
        ok(agcRetuneDecision(0.0, a, ssb, 0.0, a + off, ifEff, false, false) == RetuneForget::Full,
           "first tune ever (nothing learned): full");
        // A drag in 100 Hz steps re-learns the CHANNEL every 1.4 kHz, as before.
        { double learned = a; int n = 0, first = -1;
          for (int k = 1; k <= 30; ++k) { const double f = a + 100.0 * k;
              if (agcChannelMoved(learned, f, ssb)) { ++n; if (first < 0) first = k; learned = f; } }
          ok(first == 14 && n == 2, "100 Hz drag: channel re-learned at +1.4 and +2.8 kHz"); }
    }

    // ── synthetic SSB speech, deterministic ─────────────────────────────────────────────────────────
    struct Lcg { unsigned long long s; double u() { s = s * 6364136223846793005ull + 1442695040888963407ull;
                                                    return ((s >> 11) & ((1ull << 53) - 1)) / 9007199254740992.0; }
                 double g() { double x = 0; for (int i = 0; i < 12; ++i) x += u(); return x - 6.0; } };
    // Per-frame channel level, dB: syllables at -60 +/- 5 dB, gaps, and 1-3 s phrase pauses at the -90 floor.
    auto speech = [](Lcg& r, int F, double secs) {
        std::vector<float> out; const size_t want = (size_t)(F * secs);
        while (out.size() < want) {
            if (r.u() < 0.12) { const int n = (int)(r.u() * 2 * F + F); out.insert(out.end(), n, -90.0f); }
            else {
                const int n = std::max(1, (int)((0.15 + r.u() * 0.25) * F)); const double lvl = -60.0 + 5.0 * r.g();
                for (int i = 0; i < n; ++i) out.push_back((float)(lvl + 1.5 * r.g()));
                out.insert(out.end(), (size_t)((0.05 + r.u() * 0.25) * F), -90.0f);
            }
        }
        out.resize(want); return out;
    };

    printf("\nBursty channel level (AM/NFM) — the same statistic before and after a move (§6)\n");
    {
        const int F = 10, trials = 1500;
        const int move = 40 * F;
        Lcg r{12345};
        double oldSum = 0, newSum = 0; int oldWorse = 0, oldBetter = 0, newWorse = 0, newBetter = 0;
        for (int t = 0; t < trials; ++t) {
            const std::vector<float> x = speech(r, F, 60.0);
            // ── the OLD hold: 0.5 dB/s decay, reset at the move, fed through the settle, judged 4.0 s on.
            float held = -200.0f, before = 0; const float dec = 0.5f / F;
            const int verdictOld = move + (int)(4.0 * F);
            float dOld = 0;
            for (int i = 0; i <= verdictOld; ++i) {
                if (i == move) { before = held; held = -200.0f; }
                float h = held - dec; if (x[(size_t)i] > h || h < -199.0f) h = x[(size_t)i]; held = h;
                if (i == verdictOld) dOld = held - before;
            }
            // ── the NEW window: max over 2.5 s; "before" = the 1 s average of full windows ahead of the move;
            //    after: reset at the move, not fed for the 1.3 s settle, averaged only once the window is full.
            WindowMax w; w.configure((int)std::lround(kBurstyWindowSec * F));
            double accB = 0; int nB = 0;
            for (int i = move - 6 * F; i < move; ++i) { w.push(x[(size_t)i]); if (w.full() && i >= move - F) { accB += w.max(); ++nB; } }
            w.reset();
            double accA = 0; int nA = 0; const int settle = move + (int)(1.3 * F);
            for (int i = settle; i < (int)x.size() && nA < F; ++i) { w.push(x[(size_t)i]); if (w.full()) { accA += w.max(); ++nA; } }
            const double dNew = accA / nA - accB / nB;
            oldSum += dOld; newSum += dNew;
            if (dOld < -0.5) ++oldWorse; else if (dOld > 0.5) ++oldBetter;
            if (dNew < -0.5) ++newWorse; else if (dNew > 0.5) ++newBetter;
        }
        const double oldMean = oldSum / trials, newMean = newSum / trials;
        printf("        no real change, %d trials: old hold mean %+.2f dB (worse %d / better %d), "
               "equal windows %+.2f dB (worse %d / better %d)\n", trials, oldMean, oldWorse, oldBetter, newMean, newWorse, newBetter);
        ok(oldMean < -1.0 && oldWorse > oldBetter + trials / 10,
           "the OLD hold reads a move as worse when nothing changed (the fault, reproduced)");
        ok(std::fabs(newMean) < 0.5, "equal windows: no bias (|mean| < 0.5 dB)");
        ok(std::abs(newWorse - newBetter) < trials / 20, "equal windows: 'worse' and 'better' as likely as each other");
        // ★ ...and why SSB is not judged at all: even unbiased, speech decides most verdicts on its own.
        ok(newWorse + newBetter > trials / 2, "speech alone moves the verdict past 0.5 dB in most trials — why SSB/CW are headroom-only (§5)");
    }

    printf("\nCarrierless headroom loop (SSB) — speech pauses do not move the gain; an overload is still cut (§5)\n");
    {
        // A model of the headroom rules as the shim applies them: tick every 2 s; climb one rung when two seconds are
        // clean, 3 s since the last move, and the predicted landing (reference peak + step) clears -1 dBFS; cut one rung
        // when the live one-second peak has been over -1 dBFS for two seconds running. The reference peak is the
        // one-second peak (OLD, climbing to -1 dBFS) or the loudest second of the last kHeadroomPeakHoldSec
        // (NEW, PeakHold, climbing no higher than the -6 dBFS operating point). The SSB station is the loudest
        // thing in the capture, the worst case for this rule.
        static const int kGains[] = { 0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297,
                                      328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496 };   // V4, tenths
        const int nG = (int)(sizeof kGains / sizeof kGains[0]);
        auto run = [&](bool held, unsigned long long seed, double overloadAt, int* movesAfter, double* cutAfter) {
            Lcg r{seed};
            const int F = 10; const double secs = 600.0;
            const std::vector<float> x = speech(r, F, secs);
            int g = 15;            // 28.0 dB: loudest syllables land around -4 dBFS
            PeakHold ph; int hot = 0, clean = 0; double lastMove = -10; int moves = 0, movesLate = 0; double cutAt = -1;
            for (int sec = 0; sec < (int)secs; ++sec) {
                float lvl = -200.0f; for (int k = 0; k < F; ++k) lvl = std::max(lvl, x[(size_t)(sec * F + k)]);
                // channel -60 dB (speech) at 28 dB gain sits at -4 dBFS; the floor is -40 dBFS there.
                double pk = std::max((double)lvl + 56.0, -40.0) + (kGains[g] - 280) / 10.0;
                if (overloadAt > 0 && sec >= overloadAt) pk = std::max(pk, 15.0 + (kGains[g] - 280) / 10.0);   // a blowtorch arrives, 19 dB over at 28 dB
                const bool rail = pk > 0.0; if (pk > 0.0) pk = 0.0;
                hot = pk > -1.0 ? hot + 1 : 0; clean = rail ? 0 : clean + 1;
                ph.push(pk);
                if (sec % 2) continue;                               // the 2 s housekeeping tick
                if (hot >= 2 && g > 0) {                             // the cut (unchanged)
                    --g; lastMove = sec; ++moves; if (sec >= 120) ++movesLate; hot = 0;
                    if (overloadAt > 0 && sec >= overloadAt && cutAt < 0) cutAt = sec - overloadAt;
                    continue;
                }
                const double ref = held ? std::max(ph.max(), pk) : pk;
                const double land = held ? -6.0 : -1.0;
                if (clean >= 2 && sec - lastMove >= 3 && g + 1 < nG && ref + (kGains[g + 1] - kGains[g]) / 10.0 <= land) {
                    ++g; lastMove = sec; ++moves; if (sec >= 120) ++movesLate;
                }
            }
            if (movesAfter) *movesAfter = movesLate;
            if (cutAfter) *cutAfter = cutAt;
            return moves;
        };
        int oldLate = 0, newLate = 0;
        for (unsigned long long seed = 1; seed <= 20; ++seed) {
            int m; run(false, seed, -1, &m, nullptr); oldLate += m;
            run(true, seed, -1, &m, nullptr);  newLate += m;
        }
        printf("        20 x 10 min of SSB speech: OLD (1 s peak) %d gain moves after the first 2 min, NEW (held) %d\n", oldLate, newLate);
        ok(oldLate > 20 * 10, "OLD: the gain pumps with the talker — dozens of moves per 10 minutes (the fault, reproduced)");
        ok(newLate <= 30, "NEW: about one move per 10 minutes at most once settled — pauses do not ask for gain");
        double cut = -1; int dummy = 0;
        run(true, 7, 300.0, &dummy, &cut);
        ok(cut >= 0 && cut <= 4.0, "NEW: a real overload arriving mid-QSO is still cut within 4 s");
        PeakHold ph; for (int k = 0; k < 10; ++k) ph.push(-3.0); for (int k = 0; k < 3; ++k) ph.push(-40.0);
        ok(std::fabs(ph.max() + 3.0) < 1e-9, "PeakHold: a 3 s pause does not lower the reference");
        for (int k = 0; k < kHeadroomPeakHoldSec; ++k) ph.push(-40.0);
        ok(std::fabs(ph.max() + 40.0) < 1e-9, "...a whole hold of silence does (the band really has gone quiet)");
    }
    printf("\nagc rules: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
