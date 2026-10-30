// ★★★ VibeAGC FOR SDRplay, PROVED OFFLINE (2026-10-30) — vibe_rsp_vibeagc.h in a closed loop against a simulated
//     RSP, then its gain writes through the REAL capture smoother (gainstep.h) and the REAL RxPipeline.
//
// Stuart, 2026-10-30: "VibeAGC for SDRPlay a combined IF/RF AGC system completely controlled by us" — and, as a hard
// requirement: "the IF agc must not cause audio dropouts like the SDRPlay own IF agc currently does, if we can also
// make the RF AGC move silent too then that would be good".
//
// PART 1 — THE LOOP (pure, 0.1 s windows, as SdrplaySource::adcWindows closes them):
//   the simulated RSP1A: per-band LNA ladder (Band II = the 10-30 journal's measured rungs), IF reduction 20..59, an
//   ADC that clips at 0 dBFS, every signal in the capture adding to the peak, and an input-referred SINR for the
//   wanted station: LNA/mixer noise, ADC noise referred back through the IF (so a big reduction costs NF too), and
//   3rd-order intermod of the two strongest signals made at the mixer (3 dB per dB of RF gain). Cases:
//     A  Band II, a +20 dB neighbour (the Lenovo, 10-30), starting where it STUCK: full RF, IF railed at 59
//     B  a weak HF band: the RF must come all the way up (sensitivity kept)
//     C  DAB: the IF frozen inside its band once settled — no writes in the steady state
//     D  a strong signal appears, then goes: fast cut, no clipping run, RF back later, no hunting
//     E  the DAB rapid channel-step repro (a hop every 0.8 s, ±15 dB per block): writes/s within budget
//     F  a slow ±8 dB fade on HF: no LNA hunting
//     G  a rung bigger than its prior (25 dB where the table says 19): still no hunting
//     T  the take-over: the IF at its parking 59 walking down is not a loaded front end (on air 10-30)
//     H  no new ADC windows (a stalled stream): no writes at all
// PART 2 — THE SOUND (real gainstep.h + RxPipeline, the twin method of test-gain-step.cpp):
//     I  AM, eight 1-2 dB IF steps 0.25 s apart: before = flagged only (as under the hardware IF AGC today),
//        after = noteKnownStep + flag. Click re programme, at packet start and mid-packet.
//     J  WFM stereo, the same eight steps: click, pilot lock, CEQ, L−R corner.
//     K  an RF rung as ONE write with its IF counter-step (LNA lands 0.25 ms before the IF): click on WFM and AM,
//        against the plain rung the old walker takes.
//
//   build (from vibeserver/build): cmake --build . --target test-rsp-vibeagc
//   ★ Silent: no audio device is opened; the audio callback only stores samples.
#include "vibe_rsp_vibeagc.h"
#include "../android/app/src/main/cpp/vibedsp/vibedsp.h"
#include "../android/app/src/main/cpp/vibedsp/gainstep.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace vibersp;

static int fails = 0, passes = 0;
static void ok(bool c, const std::string& what) {
    if (c) { ++passes; std::printf("   ok   %s\n", what.c_str()); }
    else   { ++fails;  std::printf("   FAIL %s\n", what.c_str()); }
}
static std::string fmt(const char* f, double a, double b = 0, double c = 0, double d = 0) {
    char s[256]; std::snprintf(s, sizeof s, f, a, b, c, d); return s;
}

/* ═══════════════════════════════ PART 1 — THE LOOP ═══════════════════════════════════════════════════ */
struct SimRsp {
    int band = 1;
    std::vector<double> red;            // TRUE ladder (reduction per state)
    int lna = 0, gr = 59;
    std::vector<double> sigs;           // input powers of everything in the capture, dB
    double wanted = -60;                // the wanted station's input power
    double fadeDb = 0;                  // applied to every signal
    unsigned windows = 0;
    unsigned long long k = 0;
    static constexpr double kG = 92.0;  // peak = Ptot + kG - red - gr + 3 (programme peak) — Band II calibration
    double ptot() const { double s = 0; for (double p : sigs) s += std::pow(10.0, (p + fadeDb) / 10.0); return 10.0 * std::log10(s + 1e-30); }
    double rawPeak() const {
        static const double w[] = {0.0, 0.9, -0.7, 1.2, -1.0, 0.4, -1.3, 0.8, 0.2, -0.4, 1.0, -0.2};
        return ptot() + kG - red[lna] - gr + w[k % 12];
    }
    // ★ the converter's own noise peaks (~-72 dBFS) are the floor of what the peak can read
    double peak() const { return std::min(10.0 * std::log10(std::pow(10.0, rawPeak() / 10.0) + std::pow(10.0, -7.2)), 0.0); }
    double clip() const { return rawPeak() > -0.3 ? 0.01 : 0.0; }
    // ── the wanted station's SINR, input-referred at the mixer output ──
    double sinr() const { return sinrAt(lna, gr); }
    double sinrAt(int s, int g) const {
        const double grf = 30.0 - red[s];                     // LNA + mixer gain to the IF input
        const double S = wanted + fadeDb + grf;
        const double Nlna = -100.0 + grf, Nmix = -85.0;
        const double Nadc = -80.0 - (kG - 30.0) + g;           // ADC noise (−80 dBFS) referred back through the IF
        std::vector<double> v = sigs; std::sort(v.rbegin(), v.rend());
        double IM = -300;
        if (v.size() >= 2) IM = 2.0 * (v[0] + fadeDb + grf) + (v[1] + fadeDb + grf) - 2.0 * 10.5;
        auto p = [](double d) { return std::pow(10.0, d / 10.0); };
        return S - 10.0 * std::log10(p(Nlna) + p(Nmix) + p(Nadc) + p(IM));
    }
    /** The best SINR any state can give with the IF holding the -20 target (and never more than -14). */
    double oracle(int* bestS = nullptr) const {
        double best = -1e9; int bs = 0;
        for (int s = 0; s < (int)red.size(); ++s) {
            int g = (int)std::lround(ptot() + kG - red[s] + 20.0);
            g = std::clamp(g, 20, 59);
            if (ptot() + kG - red[s] - g > -14.0) continue;   // would clip on the next transient
            const double v = sinrAt(s, g);
            if (v > best) { best = v; bs = s; }
        }
        if (bestS) *bestS = bs;
        return best;
    }
};

static std::vector<double> ladder(int band) { std::vector<double> r; int n = band == 0 ? 7 : 10;
    for (int s = 0; s < n; ++s) r.push_back(vagcPriorRed(band, s)); return r; }

struct LoopStats {
    int lnaMoves = 0, ifWrites = 0, writes = 0, maxPerSec = 0, clipWindows = 0, reversals = 0;
    double lastLnaMoveT = -1; int finalLna = 0, finalGr = 0; double finalPeak = 0;
    double inBandFrac = 0;   // of the last 60 s, fraction of windows within target ± 6
    std::vector<double> lnaMoveT;
};

/** One closed-loop run. `script(t, rsp)` changes the world each window. */
template <class F>
static LoopStats runLoop(SimRsp& r, VagcBand b, double secs, F script, bool learn = true, bool verbose = false,
                         bool stall = false, double stallFrom = 1e9) {
    VibeAgcRsp agc; agc.setProfile(b); agc.adopt(r.lna, r.gr);
    LoopStats st; int lastDir = 0; int inBand = 0, inBandN = 0;
    std::vector<bool> seen(r.red.size(), false); seen[(size_t)r.lna] = true;
    std::vector<double> wt;
    for (double t = 0; t < secs; t += 0.1) {
        script(t, r);
        ++r.k;
        if (!(stall && t >= stallFrom)) ++r.windows;
        VagcIn in; in.now = t; in.windows = r.windows; in.peak = r.peak(); in.clipPct = r.clip();
        in.nStates = (int)r.red.size(); in.band = r.band;
        if (learn) for (size_t s = 0; s < r.red.size(); ++s) if (seen[s]) in.learnedRed[s] = r.red[s];
        if (in.clipPct > 0) ++st.clipWindows;
        if (t >= secs - 60) { ++inBandN; if (std::fabs(in.peak - agc.profile().targetPk) <= 6.0) ++inBand; }
        const VagcOut o = agc.tick(in);
        if (o.write) {
            ++st.writes; wt.push_back(t);
            while (!wt.empty() && t - wt.front() >= 1.0 - 1e-9) wt.erase(wt.begin());
            st.maxPerSec = std::max(st.maxPerSec, (int)wt.size());
            if (o.lna != r.lna) {
                const int dir = o.lna < r.lna ? +1 : -1;
                if (lastDir != 0 && dir != lastDir) ++st.reversals;
                lastDir = dir; ++st.lnaMoves; st.lastLnaMoveT = t; st.lnaMoveT.push_back(t);
                seen[(size_t)o.lna] = true;
            }
            if (o.gr != r.gr) ++st.ifWrites;
            if (verbose) std::printf("      t=%5.1f  %s\n", t, o.why);
            r.lna = o.lna; r.gr = o.gr;
        }
    }
    st.finalLna = r.lna; st.finalGr = r.gr; st.finalPeak = r.peak();
    st.inBandFrac = inBandN ? (double)inBand / inBandN : 0;
    return st;
}

static void caseA() {
    std::printf("\n── A  Band II, +20 dB neighbour, starting STUCK at full RF / IF 59 (the Lenovo, 10-30) ──\n");
    SimRsp r; r.band = 1; r.red = ladder(1); r.lna = 0; r.gr = 59;
    r.sigs = {-30.0, -33.0, -50.0}; r.wanted = -50.0;
    const double before = r.sinr(); int bestS; const double best = r.oracle(&bestS);
    const LoopStats s = runLoop(r, VagcBand::FmBroadcast, 120, [](double, SimRsp&) {}, true, true);
    const double after = r.sinr();
    std::printf("   -> before (stuck): LNA 0, IF 59, peak %.1f dBFS, SINR %.1f dB\n",
                SimRsp{1, ladder(1), 0, 59, {-30, -33, -50}, -50}.peak(), before);
    std::printf("   -> after: LNA %d, IF %d, peak %.1f dBFS, SINR %.1f dB (oracle best: LNA %d, %.1f dB); "
                "%d LNA moves, %d IF writes, max %d writes in any second\n",
                s.finalLna, s.finalGr, s.finalPeak, after, bestS, best, s.lnaMoves, s.ifWrites, s.maxPerSec);
    ok(s.finalLna >= 3, "the RF comes off full gain (LNA state >= 3)");
    ok(s.finalGr <= 52, "the IF reduction ends inside the window (<= 52), not railed");
    ok(after - before >= 6.0, fmt("wanted SINR up by >= 6 dB (%.1f -> %.1f)", before, after));
    ok(std::abs(s.finalLna - bestS) <= 1, fmt("within one rung of the oracle's best state (LNA %.0f vs %.0f)", s.finalLna, bestS));
    ok(s.lastLnaMoveT < 30.0, fmt("settled: no LNA move after 30 s (last at %.1f s)", s.lastLnaMoveT));
    ok(s.reversals == 0, "no RF reversals (no hunting)");
    ok(s.inBandFrac >= 0.95, fmt("ADC peak within target ±6 dB for %.0f %% of the last 60 s", s.inBandFrac * 100));
    ok(s.maxPerSec <= VibeAgcRsp::kMaxWritesPerSec, "never more than the write budget in any second");
}

static void caseB() {
    std::printf("\n── B  weak HF band: sensitivity kept (start at minimum RF, IF 59) ──\n");
    SimRsp r; r.band = 0; r.red = ladder(0); r.lna = 6; r.gr = 59;
    r.sigs = {-80.0, -83.0, -100.0}; r.wanted = -100.0;
    const double before = r.sinr(); int bestS; const double best = r.oracle(&bestS);
    const LoopStats s = runLoop(r, VagcBand::HfSsb, 120, [](double, SimRsp&) {}, true, true);
    std::printf("   -> LNA %d, IF %d, peak %.1f, SINR %.1f (start %.1f; oracle LNA %d %.1f), %d LNA moves, %d IF writes\n",
                s.finalLna, s.finalGr, s.finalPeak, r.sinr(), before, bestS, best, s.lnaMoves, s.ifWrites);
    ok(s.finalLna <= 1, "the RF comes up to (near) full gain on a weak band");
    ok(best - r.sinr() <= 1.0, fmt("SINR within 1 dB of the best any state gives (%.1f vs %.1f)", r.sinr(), best));
    ok(s.reversals == 0, "no RF reversals");
    ok(s.lastLnaMoveT < 45.0, fmt("settled from a cold start within 45 s (last LNA move %.1f s)", s.lastLnaMoveT));
}

static void caseC() {
    std::printf("\n── C  DAB: settle, then hold still ──\n");
    SimRsp r; r.band = 1; r.red = ladder(1); r.lna = 5; r.gr = 40;
    r.sigs = {-48.0, -60.0}; r.wanted = -48.0;
    int lateWrites = 0;
    VibeAgcRsp dummy; (void)dummy;
    LoopStats s;
    {
        VibeAgcRsp agc; agc.setProfile(VagcBand::Dab); agc.adopt(r.lna, r.gr);
        std::vector<bool> seen(r.red.size(), false);
        for (double t = 0; t < 120; t += 0.1) {
            ++r.k; ++r.windows;
            VagcIn in; in.now = t; in.windows = r.windows; in.peak = r.peak(); in.clipPct = r.clip(); in.nStates = 10; in.band = 1;
            for (size_t q = 0; q < seen.size(); ++q) if (seen[q]) in.learnedRed[q] = r.red[q];
            const VagcOut o = agc.tick(in);
            if (o.write) { ++s.writes; if (t > 30) ++lateWrites; if (o.lna != r.lna) { ++s.lnaMoves; seen[(size_t)o.lna] = true; }
                           std::printf("      t=%5.1f  %s\n", t, o.why); r.lna = o.lna; r.gr = o.gr; }
        }
    }
    std::printf("   -> LNA %d, IF %d, peak %.1f dBFS, %d writes (%d after 30 s)\n", r.lna, r.gr, r.peak(), s.writes, lateWrites);
    ok(lateWrites == 0, "no gain writes in the steady state (no steps inside OFDM symbols)");
    ok(r.peak() > -34 && r.peak() < -8, "ADC peak inside the DAB hold band");
    ok(s.writes <= 6, "settled in a handful of writes");
}

static void caseD() {
    std::printf("\n── D  a strong signal appears at 40 s (+30 dB) and goes at 100 s ──\n");
    SimRsp r; r.band = 1; r.red = ladder(1); r.lna = 0; r.gr = 40;
    r.sigs = {-58.0, -62.0, -70.0}; r.wanted = -62.0;
    int clipAfter = 0; double cutAt = -1, backAt = -1, firstAdd = -1; int lnaAt40 = -1;
    VibeAgcRsp agc; agc.setProfile(VagcBand::FmBroadcast); agc.adopt(r.lna, r.gr);
    std::vector<bool> seen(r.red.size(), false); seen[0] = true;
    int moves = 0, revs = 0, lastDir = 0;
    for (double t = 0; t < 200; t += 0.1) {
        if (t >= 40 && t < 100 && r.sigs.size() == 3) r.sigs.push_back(-28.0);
        if (t >= 100 && r.sigs.size() == 4) r.sigs.pop_back();
        if (lnaAt40 < 0 && t >= 39.95) lnaAt40 = r.lna;
        ++r.k; ++r.windows;
        VagcIn in; in.now = t; in.windows = r.windows; in.peak = r.peak(); in.clipPct = r.clip(); in.nStates = 10; in.band = 1;
        for (size_t q = 0; q < seen.size(); ++q) if (seen[q]) in.learnedRed[q] = r.red[q];
        if (t >= 40 && in.clipPct > 0) ++clipAfter;
        const VagcOut o = agc.tick(in);
        if (o.write) {
            if (o.lna != r.lna) { const int dir = o.lna < r.lna ? 1 : -1; if (lastDir && dir != lastDir) ++revs; lastDir = dir; ++moves;
                                  seen[(size_t)o.lna] = true;
                                  if (t >= 40 && t < 100 && cutAt < 0 && o.lna > lnaAt40) cutAt = t;
                                  if (t >= 100 && o.lna <= lnaAt40 && backAt < 0) backAt = t;
                                  if (t >= 100 && dir > 0 && firstAdd < 0) firstAdd = t; }
            std::printf("      t=%5.1f  %s\n", t, o.why);
            r.lna = o.lna; r.gr = o.gr;
        }
    }
    std::printf("   -> clipped windows after the arrival %d; first RF cut %.1f s after; first RF add %.1f s after it left; "
                "%d LNA moves (%d reversals), final LNA %d IF %d\n", clipAfter, cutAt - 40, firstAdd - 100, moves, revs, r.lna, r.gr);
    ok(clipAfter <= 3, "at most 3 clipped windows (0.3 s) after the strong signal arrives");
    ok(cutAt >= 0 && cutAt - 40 <= 2.0, "RF cut within 2 s of it arriving");
    ok(firstAdd >= 0 && firstAdd - 100 <= 30.0, "RF gain starts coming back within 30 s of it leaving");
    ok(r.gr >= 30 && r.gr <= 52, "and rests inside the IF window afterwards");
    ok(revs <= 1, "one reversal (the signal going) and no hunting");
}

static void caseE() {
    std::printf("\n── E  DAB rapid channel-step repro: a new block every 0.8 s for 60 s, ±15 dB apart ──\n");
    SimRsp r; r.band = 1; r.red = ladder(1); r.lna = 4; r.gr = 45;
    r.sigs = {-50.0}; r.wanted = -50.0;
    std::mt19937 rng(42); std::uniform_real_distribution<double> U(-65.0, -25.0);
    VibeAgcRsp agc; agc.setProfile(VagcBand::Dab); agc.adopt(r.lna, r.gr);
    std::vector<double> wt; int maxPs = 0, writes = 0, lnaW = 0; double lastLnaT = -9, minLnaGap = 99;
    double nextHop = 0.8; int hops = 0;
    int settleWrites = 0;
    for (double t = 0; t < 75; t += 0.05) {                    // the shim's ~20 Hz heartbeat; stepping stops at 60 s
        if (t >= nextHop && t < 60) { r.sigs = {U(rng)}; agc.noteRetune(t, false); nextHop += 0.8; ++hops; }
        r.k = (unsigned long long)(t * 10);
        r.windows = (unsigned)(t * 10.0);                       // a window closes every 0.1 s
        VagcIn in; in.now = t; in.windows = r.windows; in.peak = r.peak(); in.clipPct = r.clip(); in.nStates = 10; in.band = 1;
        for (int q = 0; q < 10; ++q) in.learnedRed[q] = r.red[(size_t)q];
        const VagcOut o = agc.tick(in);
        if (o.write) {
            ++writes; wt.push_back(t);
            while (!wt.empty() && t - wt.front() >= 1.0 - 1e-9) wt.erase(wt.begin());
            maxPs = std::max(maxPs, (int)wt.size());
            if (o.lna != r.lna) { ++lnaW; minLnaGap = std::min(minLnaGap, t - lastLnaT); lastLnaT = t; }
            if (t >= 70) ++settleWrites;
            r.lna = o.lna; r.gr = o.gr;
        }
    }
    std::printf("   -> after the stepping stops: LNA %d IF %d peak %.1f dBFS, %d writes in its last 5 s\n", r.lna, r.gr, r.peak(), settleWrites);
    ok(r.peak() > -34 && r.peak() < -8 && settleWrites == 0, "the last block settles inside the DAB band and holds still");
    std::printf("   -> %d hops, %d writes (%.2f/s avg), max %d in any second, %d LNA writes, min LNA gap %.2f s\n",
                hops, writes, writes / 75.0, maxPs, lnaW, minLnaGap);
    ok(maxPs <= VibeAgcRsp::kMaxWritesPerSec, "never more than 5 writes in any one second");
    ok(writes / 75.0 <= 2.0, "≤ 2 writes/s on average through 74 block changes");
    ok(lnaW == 0 || minLnaGap >= VibeAgcRsp::kLnaMinSec - 1e-6, "LNA writes at least 1.5 s apart");
}

static void caseF() {
    std::printf("\n── F  HF, a slow ±8 dB fade over 20 s, 300 s ──\n");
    SimRsp r; r.band = 0; r.red = ladder(0); r.lna = 2; r.gr = 40;
    r.sigs = {-55.0, -58.0, -70.0}; r.wanted = -70.0;
    const LoopStats s = runLoop(r, VagcBand::HfSsb, 300, [](double t, SimRsp& x) { x.fadeDb = 8.0 * std::sin(2 * M_PI * t / 20.0); });
    std::printf("   -> %d LNA moves (%d reversals), %d IF writes (%.2f/s), max %d/s\n",
                s.lnaMoves, s.reversals, s.ifWrites, s.ifWrites / 300.0, s.maxPerSec);
    ok(s.lnaMoves <= 2, "the RF does not follow a fade (≤ 2 LNA moves in 5 min)");
    ok(s.ifWrites / 300.0 <= 1.5, "the IF follows it in ≤ 1.5 writes/s");
    ok(s.maxPerSec <= VibeAgcRsp::kMaxWritesPerSec, "write budget held");
}

static void caseG() {
    std::printf("\n── G  a rung BIGGER than its prior (25 dB where the table says 19), 180 s ──\n");
    SimRsp r; r.band = 0; r.red = ladder(0); r.red[4] = r.red[3] + 25.0; r.red[5] = r.red[4] + 5; r.red[6] = r.red[5] + 19;
    // at state 3 the IF needs 54 (over the 52 load line): one cut, sized by the prior (19), lands 6 dB short
    r.lna = 3; r.gr = 54; r.sigs = {-40.0}; r.wanted = -60.0;
    const LoopStats s = runLoop(r, VagcBand::AmMw, 180, [](double, SimRsp&) {}, true, true);
    std::printf("   -> final LNA %d IF %d, %d LNA moves, %d reversals\n", s.finalLna, s.finalGr, s.lnaMoves, s.reversals);
    ok(s.lnaMoves >= 1 && s.finalLna == 4, "the cut through the big rung is taken");
    ok(s.reversals == 0, "and not undone: the rung, learned at 25 dB, leaves no room to come back (no hunting)");
    ok(s.lnaMoves <= 2, "≤ 2 LNA moves in 3 min");
}

static void caseT() {
    std::printf("\n── T  the take-over: IF parked at 59, the level under target (the Lenovo, 10-30, MW) ──\n");
    // the journal's numbers: LNA 5/6, IF 59, ADC peak -26 dBFS — the IF wants ~53, a walk of a few dB, not a loaded front end
    SimRsp r; r.band = 0; r.red = ladder(0); r.lna = 5; r.gr = 59; r.sigs = {-17.0}; r.wanted = -30.0;
    VibeAgcRsp agc; agc.setProfile(VagcBand::AmMw); agc.adopt(r.lna, r.gr);
    int cuts = 0;
    for (double t = 0; t < 10; t += 0.1) {
        ++r.k; ++r.windows;
        VagcIn in; in.now = t; in.windows = r.windows; in.peak = r.peak(); in.nStates = 7; in.band = 0;
        const VagcOut o = agc.tick(in);
        if (o.write) { if (o.lna > r.lna) ++cuts; std::printf("      t=%5.1f  %s\n", t, o.why); r.lna = o.lna; r.gr = o.gr; }
    }
    ok(cuts == 0, "no RF rung thrown away while the IF walks down from its parking value");
    ok(std::fabs(r.peak() + 20.0) <= 6.5, fmt("the level inside the dead band (+ wander) of target (%.1f dBFS), by the IF alone", r.peak()));
}

static void caseH() {
    std::printf("\n── H  the stream stalls (no new ADC windows) at 10 s ──\n");
    SimRsp r; r.band = 1; r.red = ladder(1); r.lna = 0; r.gr = 59; r.sigs = {-30, -33}; r.wanted = -50;
    VibeAgcRsp agc; agc.setProfile(VagcBand::FmBroadcast); agc.adopt(r.lna, r.gr);
    int late = 0;
    for (double t = 0; t < 40; t += 0.1) {
        if (t < 10) ++r.windows;
        VagcIn in; in.now = t; in.windows = r.windows; in.peak = r.peak(); in.nStates = 10; in.band = 1;
        const VagcOut o = agc.tick(in);
        if (o.write) { if (t >= 10.05) ++late; r.lna = o.lna; r.gr = o.gr; }
    }
    ok(late == 0, "no writes at all once the measurements stop");
}

/* ═══════════════════════════════ PART 2 — THE SOUND ══════════════════════════════════════════════════ */
using vibedsp::RxPipeline; using vibedsp::cf32; using vibedsp::GainStepSmoother;
namespace snd {
constexpr double kFs = 1000000.0;
constexpr int kPkt = 1008;                     // an RSP stream packet
constexpr double kT0 = 3.0;

/** A hardware gain event: at time t the level changes by db; `lagUs` > 0 = a second part (the IF counter-step)
 *  lands that much later with db2. */
struct Ev { double t, db; double lagUs = 0, db2 = 0; double expect = NAN; bool fade = false; };

struct Gen {
    bool am = false; double noise = 0.05; std::vector<Ev> ev;
    double phase = 0, t = 0; std::mt19937 rng{9}; std::normal_distribution<double> N{0, 1};
    /* ★ fade = the level a step + its glided correction leaves at the output: 0 at the step, reaching db with the
     *   smoother's own 0.4 s time constant — a natural fade, the reference a compensated step is judged against. */
    double gainAt(double tt) const { double g = 0; for (const Ev& e : ev) {
        if (e.fade) { if (tt >= e.t) g += e.db * (1.0 - std::exp(-(tt - e.t) / 0.4)); continue; }
        if (tt >= e.t) g += e.db; if (e.lagUs > 0 && tt >= e.t + e.lagUs * 1e-6) g += e.db2; } return g; }
    void fill(std::vector<cf32>& o, int n) {
        o.resize((size_t)n);
        for (int i = 0; i < n; ++i) {
            double re, im;
            if (am) { re = 0.3 * (1 + 0.5 * std::sin(2 * M_PI * 1000 * t)); im = 0; }
            else {
                const double L = std::sin(2 * M_PI * 1000 * t);
                const double mpx = 0.45 * L + 0.10 * std::sin(2 * M_PI * 19000 * t) + 0.45 * L * std::sin(2 * M_PI * 38000 * t);
                phase += 2 * M_PI * 75000 * mpx / kFs;
                if (phase > 1e6) phase -= 2 * M_PI * std::floor(phase / (2 * M_PI));
                re = 0.3 * std::cos(phase); im = 0.3 * std::sin(phase);
            }
            re += noise * 0.3 * N(rng); im += noise * 0.3 * N(rng);
            const double a = std::pow(10.0, gainAt(t) / 20.0);
            o[(size_t)i] = cf32((float)(re * a), (float)(im * a));
            t += 1 / kFs;
        }
    }
};
struct Sink { std::vector<float> L, R;
    static void audio(void* c, const float* p, int fr, int ch, int) { auto* s = (Sink*)c;
        for (int i = 0; i < fr; ++i) { s->L.push_back(p[i * ch]); s->R.push_back(ch > 1 ? p[i * ch + 1] : p[i * ch]); } } };
struct Run { Sink sink; unsigned pairs = 0; int unlockedBlocks = 0, ceqBlocks = 0; float minCut = 99999; unsigned known = 0, missed = 0, steps = 0, holds = 0, located = 0; };

enum class Mode { Raw, FlagOnly, Known };

/** `offset` = samples into the flagged packet where the change actually lands (0 = its first sample). */
static void run(Run& r, Gen g, double secs, Mode m, int offset = 0) {
    RxPipeline::Callbacks cb{}; cb.ctx = &r.sink; cb.audio = &Sink::audio;
    RxPipeline rx; rx.start(kFs, 1024, 10.0, 48000, cb);
    rx.setTune(0.0, g.am ? RxPipeline::Mode::AM : RxPipeline::Mode::WFM, g.am ? 10000.0 : 250000.0);
    GainStepSmoother sm; sm.configure(kFs);
    // shift each event so that it lands `offset` samples into a packet
    for (Ev& e : g.ev) { const long long at = (long long)(e.t * kFs); e.t = ((at / kPkt) * kPkt + offset) / kFs; }
    std::vector<cf32> b; const long long total = (long long)(secs * kFs);
    for (long long d = 0; d < total; d += kPkt) {
        bool flagged = false, knownSmall = false;
        for (const Ev& e : g.ev) {
            if (e.fade) continue;
            const long long at = (long long)std::llround(e.t * kFs);
            if (at >= d && at < d + kPkt) {
                flagged = true;
                if (m == Mode::Known) {
                    const double x = std::isfinite(e.expect) ? e.expect : e.db + e.db2;
                    if (e.lagUs == 0) { sm.noteKnownStep(x); knownSmall = std::fabs(x) < GainStepSmoother::kKnownDirectDb; }
                    // ★ an RF write: the rung as the LADDER predicts it (1 dB off the truth here) + the exact IF counter-step
                    else sm.noteKnownPair(e.db + 1.0, e.db2);
                }
            }
        }
        g.fill(b, kPkt);
        if (m != Mode::Raw) {
            if (flagged) sm.noteStepAtStart();
            const unsigned before = sm.steps();
            sm.process(b.data(), kPkt);
            const bool guard = (flagged && !knownSmall) || sm.steps() != before || sm.correctingFound(1.0) || sm.holding();
            if (guard) rx.noteGainStep(0.1);
        }
        rx.feed(b.data(), kPkt);
        if ((double)d / kFs > kT0) {
            if (!g.am && !rx.pilotLocked()) ++r.unlockedBlocks;
            if (!g.am && rx.ceqEngaged()) ++r.ceqBlocks;
            if (!g.am) r.minCut = std::min(r.minCut, rx.lmrHiCutHz());
        }
    }
    r.known = sm.knownApplied(); r.missed = sm.knownMissed(); r.steps = sm.steps(); r.holds = sm.holds(); r.located = sm.knownLocated(); r.pairs = sm.knownPairs();
}

/** Largest |b − a| within [-5, +25] ms of any event, dB re the programme's peak before the first. Smoother latency
 *  (4 ms) is the same in both twins, so they line up. */
static double click(const Run& a, const Run& b, const Gen& g) {
    const size_t n = std::min(a.sink.L.size(), b.sink.L.size());
    float ref = 0; const size_t s0 = (size_t)(kT0 * 48000);
    for (size_t i = s0 - 48000; i < s0; ++i) ref = std::max(ref, std::fabs(a.sink.L[i]));
    /* ★ The glide legitimately moves the level by a dB or two over a second, and the audio AGC follows it — that is a
     *   fade, not a click. So over the judged 30 ms the twin is first fitted to the step run with a gain that may DRIFT
     *   (k0 + k1·t, least squares): a fade is absorbed by that, a click — anything sharper than a 30 ms ramp — is not,
     *   and is what is left. */
    double d = 0;
    for (const Ev& e : g.ev) {
        const size_t c = (size_t)(e.t * 48000), i0 = c - 240, i1 = std::min(n, c + 1200);
        double s00 = 0, s01 = 0, s11 = 0, r0 = 0, r1 = 0;
        for (size_t i = i0; i < i1; ++i) {
            const double tt = ((double)i - c) / 1200.0;
            for (int ch = 0; ch < 2; ++ch) {
                const double x = ch ? a.sink.R[i] : a.sink.L[i], y = ch ? b.sink.R[i] : b.sink.L[i];
                s00 += x * x; s01 += x * x * tt; s11 += x * x * tt * tt; r0 += x * y; r1 += x * y * tt;
            }
        }
        const double det = s00 * s11 - s01 * s01;
        const double k0 = det != 0 ? (r0 * s11 - r1 * s01) / det : 1.0, k1 = det != 0 ? (s00 * r1 - s01 * r0) / det : 0.0;
        for (size_t i = i0; i < i1; ++i) {
            const double k = k0 + k1 * ((double)i - c) / 1200.0;
            d = std::max(d, std::max(std::fabs(b.sink.L[i] - k * a.sink.L[i]), std::fabs(b.sink.R[i] - k * a.sink.R[i])));
        }
    }
    return 20 * std::log10(d / ref + 1e-12);
}

static std::vector<Ev> ifSteps() {   // eight IF writes, 0.25 s apart, the sizes VibeAGC takes
    const double db[] = {-2, -1, -2, +1, -1, +2, +1, -2};
    std::vector<Ev> v; for (int i = 0; i < 8; ++i) v.push_back({kT0 + 0.25 * i + 0.0371 * i, db[i]}); return v;
}

static void caseI() {
    std::printf("\n── I  AM: eight 1-2 dB IF steps (VibeAGC's own), before vs after ──\n");
    for (int off : {0, 500}) {
        Gen g; g.am = true; g.ev = ifSteps();
        Gen a = g; a.ev.clear();
        Run ra; run(ra, a, 6.0, Mode::FlagOnly);
        Run rRaw, rFlag, rKnown;
        run(rRaw, g, 6.0, Mode::FlagOnly, off);        // ★ "before": today's handling of a flagged small step
        run(rKnown, g, 6.0, Mode::Known, off);
        // ★ the reference for "after": the SAME level trajectory arriving as a natural fade (no step, nothing flagged)
        Gen f = g; for (Ev& e : f.ev) { const long long at = (long long)(e.t * kFs); e.t = ((at / kPkt) * kPkt + off) / kFs; e.fade = true; }
        Run rk0; run(rk0, f, 6.0, Mode::Known);
        const double cb = click(ra, rRaw, g), ck = click(rk0, rKnown, g);
        std::printf("   .. step %3d samples into its packet: click before %6.1f dB, after %6.1f dB (re programme); "
                    "placed %u (found by level %u), missed %u\n", off, cb, ck, rKnown.known, rKnown.located, rKnown.missed);
        if (off == 0) {
            ok(ck <= -40.0, fmt("AM, step on the packet edge: click after <= -40 dB (%.1f)", ck));
            ok(cb - ck >= 10.0, fmt("…and at least 10 dB better than before (%.1f -> %.1f)", cb, ck));
        } else {
            ok(ck <= -35.0, fmt("AM, step 0.5 ms into its packet: click after <= -35 dB (%.1f)", ck));
        }
        ok(rKnown.known == 8 && rKnown.missed == 0, "every known step placed at its flagged packet");
    }
}

static void caseJ() {
    std::printf("\n── J  WFM stereo: the same eight IF steps ──\n");
    Gen g; g.am = false; g.ev = ifSteps();
    Gen a = g; a.ev.clear();
    Run ra; run(ra, a, 6.0, Mode::Known);
    Run rb; run(rb, g, 6.0, Mode::Known);
    Run rf; run(rf, g, 6.0, Mode::FlagOnly);
    Run raf; run(raf, a, 6.0, Mode::FlagOnly);
    const double ck = click(ra, rb, g), cf = click(raf, rf, g);
    std::printf("   .. click before %.1f dB, after %.1f dB; unlocked blocks after %d (twin %d); CEQ blocks %d (twin %d); "
                "L-R corner min %.0f Hz (twin %.0f)\n", cf, ck, rb.unlockedBlocks, ra.unlockedBlocks, rb.ceqBlocks,
                ra.ceqBlocks, rb.minCut, ra.minCut);
    ok(ck <= -40.0, fmt("WFM: click <= -40 dB (%.1f)", ck));
    ok(rb.unlockedBlocks == ra.unlockedBlocks, "pilot lock identical to the no-step twin");
    ok(rb.ceqBlocks == ra.ceqBlocks, "CEQ never engaged by the steps");
    ok(std::fabs(rb.minCut - ra.minCut) <= 600.0f, "L-R corner unmoved (stereo width kept)");
}

static void caseK() {
    std::printf("\n── K  an RF rung as ONE write with its IF counter-step (LNA lands 0.25 ms first) ──\n");
    for (bool am : {false, true}) {
        // the combined write: −18.9 dB rung, +19 dB IF counter-step 250 µs later (net +0.1)
        Gen gc; gc.am = am; gc.ev = {{kT0, -18.9, 250.0, +19.0}};
        // the old walker: the rung alone, the hardware IF AGC answering it 3 ms later by +12 dB
        Gen go; go.am = am; go.ev = {{kT0, -18.9, 3000.0, +12.0}};
        Gen a; a.am = am;
        Run ra; run(ra, a, 5.0, Mode::Known);
        Run rc; run(rc, gc, 5.0, Mode::Known);
        Run ro; run(ro, go, 5.0, Mode::FlagOnly);
        Run rr; run(rr, gc, 5.0, Mode::Raw);
        Run rar; run(rar, a, 5.0, Mode::Raw);
        const double cc = click(ra, rc, gc), co = click(ra, ro, go), cr = click(rar, rr, gc);
        std::printf("   .. %s: combined write %.1f dB (pairs placed %u, holds %u) · the old rung + IF AGC answer %.1f dB · combined, NO smoothing "
                    "%.1f dB%s\n", am ? "AM " : "WFM", cc, rc.pairs, rc.holds, co, cr,
                    am ? "" : fmt("; unlocked blocks %.0f vs twin %.0f", rc.unlockedBlocks, ra.unlockedBlocks).c_str());
        ok(cc <= -20.0, fmt(am ? "AM: combined RF write click <= -20 dB (%.1f)" : "WFM: combined RF write click <= -20 dB (%.1f)", cc));
        if (am) ok(cc < co - 15.0, fmt("…at least 15 dB under the old walker's rung + IF AGC answer (%.1f vs %.1f)", cc, co));
        if (!am) ok(rc.unlockedBlocks == ra.unlockedBlocks, "WFM: pilot lock kept through the RF write");
    }
}
}  // namespace snd

/* ═══════════════════════════════ THE READOUT'S RAIL (vagcRailDir) ══════════════════════════════════════
 * ★★ The DAB false warning (coordinator, 2026-10-30): DAB's profile freezes the IF while the peak is inside −34…−8 dBFS,
 *    so an IF parked at 59 with the level 6-14 dB under target is the design. rspstat's vrail must stay 0 there for a
 *    whole minute — the client goes amber at 10 s of rail, so 0 throughout is "never amber". */
static void caseR() {
    std::printf("\nR. readout rail: deliberate holds never count\n");
    VibeAgcRsp dab; dab.setProfile(VagcBand::Dab); dab.adopt(5, 59);
    int maxRail = 0; double since = 0; int dirNow = 0;
    for (int i = 0; i <= 600; ++i) {                                   // 60 s at 10 Hz
        const double t = i * 0.1;
        const double peak = -20.0 - 6.0 - 8.0 * (0.5 + 0.5 * std::sin(t * 0.7));   // 6..14 dB under the −20 target
        const int d = vagcRailDir(59, 5, 10, 0, 20, &dab, t, peak);
        if (d != dirNow) { dirNow = d; since = t; }
        maxRail = std::max(maxRail, std::abs(dirNow) * (int)(t - since));
    }
    ok(maxRail == 0, fmt("DAB, IF at 59, level 6-14 dB under target for 60 s: vrail stays 0 (max %.0f s) — never amber", maxRail));
    ok(vagcRailDir(59, 5, 10, 0, 20, &dab, 0, -4.0) == +1, "DAB, IF at 59 with the level OVER the freeze band: still a rail");
    VibeAgcRsp fm; fm.setProfile(VagcBand::FmBroadcast); fm.adopt(5, 59);
    ok(vagcRailDir(59, 5, 10, 0, 20, &fm, 0, -30.0) == +1, "FM (no freeze), IF at 59 with RF to cut: a rail");
    ok(vagcRailDir(59, 5, 10, 0, 20, nullptr, 0, -30.0) == +1, "official API (no VibeAGC), IF at 59 with RF to cut: a rail");
    ok(vagcRailDir(59, 9, 10, 0, 20, &fm, 0, -30.0) == 0, "IF at 59 with the LNA already at its last state: not a rail");
    ok(vagcRailDir(20, 0, 10, 0, 20, &fm, 100, -40.0) == 0, "IF at its floor at full RF gain: not a rail");
}

int main() {
    std::printf("VibeAGC for SDRplay — closed loop and sound\n");
    caseA(); caseB(); caseC(); caseD(); caseE(); caseF(); caseG(); caseT(); caseH();
    snd::caseI(); snd::caseJ(); snd::caseK(); caseR();
    std::printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
