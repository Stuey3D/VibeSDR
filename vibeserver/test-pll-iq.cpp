// test-pll-iq.cpp — beta79: AN RTL's TUNING STEP TAKEN OUT OF THE CAPTURE FOR EVERY LISTENER (vibe_afc.h captureShiftHz,
// pllLeftInReading, pllToRemoveHz; vibe_r82xx_if.h r82xxTuneOffsetHz), through the real DSP pieces the server runs in that
// order: the capture cleaner (IqCleaner — DC and image, on the raw capture), then ONE NCO mix pass (afcMix), then a
// listener's channel (NCO to the VFO, FIR decimator) and a frequency measurement on what comes out.
//   A. a tone at a known RF through a modelled R820T tune (the step + a 0.6 ppm crystal the correction has learned) lands
//      within 1 Hz of its true frequency in the channel output — DAB 7D / 11D / 12B tunes, an FM tune with the 15 kHz
//      offset tuning, an airband tune — and is tens of Hz out without the step removed
//   B. the offset tune: the step must be the one at the PHYSICAL LO (dial + 15 kHz), not the dial's
//   C. the DC spike: removed by the cleaner BEFORE the mix, and what is left lands outside the channel after it
//   D. a retune changes the step mid-stream: the mix stays phase-continuous (no jump beyond the frequency change)
//   E. the DAB reading: nothing subtracted twice (removed from the IQ → 0 left; left in → all of it)
//   F. the gate: rtl_tcp, direct sampling, auto-PPM off or VIBE_PLL_IQ=0 → nothing removed
//   G. the cost of the one mix pass on this host at 2.048 / 2.4 / 3.2 MS/s (SIMD path, no allocation per block)
#include "vibe_afc.h"
#include "vibe_r82xx_if.h"
#include "vibedsp/vibedsp.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

using vibedsp::cf32;
static int g_pass = 0, g_fail = 0;
static void ok(bool c, const std::string& w) { std::printf("   %s %s\n", c ? "ok  " : "FAIL", w.c_str()); c ? ++g_pass : ++g_fail; }
static std::string f(const char* fmt, double a, double b = 0, double c = 0) { char s[240]; std::snprintf(s, sizeof s, fmt, a, b, c); return s; }

static constexpr uint32_t kX = 28800000u;
static constexpr double kEps = 0.6;   // ppm: the crystal's error, which auto-PPM has learned exactly
static constexpr int kBlock = 65536;

struct Case { const char* name; double dial, stationHz, rate; int32_t filter; double offset; };

/** The capture a dongle tuned to physical centre `C` delivers for a station at `X`: the tuning step `P` (+ = lands above)
 *  and the crystal (lands ε·C low); plus a DC spike. Then the server's chain; returns the channel output's tone (Hz). */
struct Run { double toneHz = 0, spikeOnlyDb = 0, toneDb = 0, dcI = 0, dcQ = 0; };
static double measureHz(const std::vector<cf32>& y, double rate) {
    std::complex<double> acc(0, 0);
    for (size_t i = y.size() / 2; i < y.size(); ++i) acc += std::complex<double>(y[i]) * std::conj(std::complex<double>(y[i - 1]));
    return std::arg(acc) * rate / (2 * M_PI);
}
static double powerDb(const std::vector<cf32>& y) {
    double s = 0; for (size_t i = y.size() / 2; i < y.size(); ++i) s += std::norm(y[i]);
    return 10 * std::log10(s / double(y.size() / 2) + 1e-30);
}
static Run run(const Case& k, double stepUsedHz, bool tone, double seconds = 1.0) {
    const double C = k.dial + k.offset;
    const double P = vibertl::r82xxTuneOffsetHz((uint32_t)std::llround(C), k.filter > 0 ? vibertl::r82xxIntFreqForBw(k.filter) : 1625000, kX, kX, 0);
    const double fbb = (k.stationHz - C) + P - kEps * 1e-6 * C;
    vibedsp::IqCleaner clean; clean.configure(k.rate);
    vibedsp::NCO mix, vfo;
    const double shift = vibeafc::captureShiftHz(kEps, C, stepUsedHz);
    mix.setFreq(-shift / k.rate);
    const double outHz = 1000.0;                       // the channel puts the station 1 kHz up (a CW/SSB-style offset)
    vfo.setFreq((k.stationHz - outHz - C) / k.rate);   // the VFO's offset from the capture centre, in the TUNER's frame
    int decim = (int)std::lround(k.rate / 48000.0); if (decim < 1) decim = 1;
    const double outRate = k.rate / decim;
    vibedsp::FirDecimator dec(vibedsp::designLowpass(6000.0 / k.rate, 3000.0 / k.rate), decim);
    std::vector<cf32> blk(kBlock), out((size_t)dec.maxOut(kBlock)), y;
    const int64_t N = (int64_t)(seconds * k.rate);
    double ph = 0; const double dph = 2 * M_PI * fbb / k.rate;
    for (int64_t s0 = 0; s0 < N; s0 += kBlock) {
        const int n = (int)std::min<int64_t>(kBlock, N - s0);
        for (int i = 0; i < n; ++i) {
            const cf32 t = tone ? cf32((float)(0.3 * std::cos(ph)), (float)(0.3 * std::sin(ph))) : cf32(0, 0);
            blk[i] = t + cf32(0.05f, -0.03f);           // ★ the tuner's DC spike
            ph += dph; if (ph > M_PI) ph -= 2 * M_PI;
        }
        clean.process(blk.data(), n);   // ① the capture cleaner — DC out, on the raw capture (cleanCapture)
        mix.mix(blk.data(), blk.data(), n);   // ② afcMix: the ONE correction pass
        vfo.mix(blk.data(), blk.data(), n);   // ③ a listener's channel
        const int m = dec.process(blk.data(), n, out.data());
        y.insert(y.end(), out.begin(), out.begin() + m);
    }
    Run r;
    r.toneHz = measureHz(y, outRate) - outHz;   // 0 = the station is exactly where it should be
    r.toneDb = powerDb(y);
    r.dcI = clean.dcI(); r.dcQ = clean.dcQ();
    return r;
}

int main() {
    const Case cases[] = {
        {"DAB 7D", 194.064e6, 194.064e6 + 123456.0, 2048000, 2048000, 0},
        {"DAB 11D", 222.064e6, 222.064e6 + 123456.0, 2048000, 2048000, 0},
        {"DAB 12B", 225.648e6, 225.648e6 + 123456.0, 2048000, 2048000, 0},
        {"FM 97.65 (offset tune +15 kHz)", 97.650e6, 97.650e6, 2400000, 1400000, 15000},
        {"airband 118.700 (offset tune +15 kHz)", 118.700e6, 118.700e6, 2400000, 500000, 15000},
    };
    std::printf("A. a tone at a known RF lands on it, through the modelled R820T tune and the server's chain\n");
    for (const Case& k : cases) {
        const double C = k.dial + k.offset;
        const double P = vibertl::r82xxTuneOffsetHz((uint32_t)std::llround(C), vibertl::r82xxIntFreqForBw(k.filter), kX, kX, 0);
        const Run on = run(k, P, true), off = run(k, 0.0, true);
        ok(std::fabs(on.toneHz) < 1.0, std::string(k.name) + f(": the step (%+.1f Hz) removed — the station is %+.3f Hz from its RF", P, on.toneHz));
        ok(std::fabs(off.toneHz - P) < 1.0, f("   …left in, it is %+.1f Hz out (the step, as on air)", off.toneHz));
    }

    std::printf("B. the offset tune: the step is the PHYSICAL LO's (dial + 15 kHz)\n");
    {
        const Case& k = cases[3];
        const int32_t intF = vibertl::r82xxIntFreqForBw(k.filter);
        const double atLo = vibertl::r82xxTuneOffsetHz((uint32_t)std::llround(k.dial + k.offset), intF, kX, kX, 0);
        const double atDial = vibertl::r82xxTuneOffsetHz((uint32_t)std::llround(k.dial), intF, kX, kX, 0);
        ok(std::fabs(atLo - atDial) > 2.0, f("97.65 MHz: the step at the tuner's 97.665 is %+.1f Hz, at the dial's 97.650 %+.1f Hz — not the same", atLo, atDial));
        const Run wrong = run(k, atDial, true);
        ok(std::fabs(wrong.toneHz) > 1.0, f("…using the dial's figure leaves the station %+.1f Hz out; the server uses what the writer PUT ON THE TUNER", wrong.toneHz));
    }

    std::printf("C. the DC spike: out before the mix, outside the channel after it\n");
    {
        const Case& k = cases[3];
        const double C = k.dial + k.offset;
        const double P = vibertl::r82xxTuneOffsetHz((uint32_t)std::llround(C), vibertl::r82xxIntFreqForBw(k.filter), kX, kX, 0);
        const Run t = run(k, P, true), dc = run(k, P, false);
        ok(std::fabs(t.dcI - 0.05) < 0.01 && std::fabs(t.dcQ + 0.03) < 0.01,
           f("the cleaner (first, on the raw capture) measured the spike at %.3f%+.3fj (injected 0.050-0.030j)", t.dcI, t.dcQ));
        ok(dc.toneDb < t.toneDb - 40, f("the spike alone through the corrected chain: %.1f dB against the station's %.1f dB — outside the channel", dc.toneDb, t.toneDb));
    }

    std::printf("D. a retune changes the step mid-stream — the one mix pass stays phase-continuous\n");
    {
        const double rate = 2048000, f0 = 5000.0;
        vibedsp::NCO mix;
        std::vector<cf32> a(kBlock), b(kBlock);
        double ph = 0;
        auto fill = [&](std::vector<cf32>& v) { for (auto& z : v) { z = cf32((float)std::cos(ph), (float)std::sin(ph)); ph += 2 * M_PI * f0 / rate; } };
        const double s1 = vibeafc::captureShiftHz(kEps, 194.064e6, -45.0), s2 = vibeafc::captureShiftHz(kEps, 225.648e6, 5.8);
        fill(a); mix.setFreq(-s1 / rate); mix.mix(a.data(), a.data(), kBlock);
        fill(b); mix.setFreq(-s2 / rate); mix.mix(b.data(), b.data(), kBlock);
        auto inc = [&](cf32 x, cf32 y) { return std::arg(std::complex<double>(y) * std::conj(std::complex<double>(x))); };
        const double before = inc(a[kBlock - 2], a[kBlock - 1]), across = inc(a[kBlock - 1], b[0]), after = inc(b[0], b[1]);
        const double want1 = 2 * M_PI * (f0 + s1) / rate, want2 = 2 * M_PI * (f0 + s2) / rate;
        ok(std::fabs(before - want1) < 1e-4 && std::fabs(after - want2) < 1e-4, f("the increment is the old shift's before and the new one's after (%.0f → %.0f Hz)", s1, s2));
        ok(std::fabs(across - want1) < 1e-4 || std::fabs(across - want2) < 1e-4,
           f("…and across the boundary it is one of the two (%.6f rad, a jump would be anything else) — no click", across));
    }

    std::printf("E. the DAB reading: nothing subtracted twice\n");
    {
        ok(vibeafc::pllLeftInReading(-45.0, -45.0) == 0.0, "the step removed from the IQ: 0 left to take out of the reading");
        ok(vibeafc::pllLeftInReading(-45.0, 0.0) == -45.0, "left in the IQ (VIBE_PLL_IQ=0, auto-PPM off): all of it taken out of the reading");
        ok(vibeafc::pllLeftInReading(std::nan(""), 0.0) == 0.0 && vibeafc::pllLeftInReading(std::nan(""), std::nan("")) == 0.0,
           "a radio it does not model: nothing either way");
        // the reading the receiver measures on the corrected IQ, then the reading auto-PPM is given: the crystal only
        const double f7 = 194.064e6, P7 = vibertl::r82xxTuneOffsetHz((uint32_t)f7, 1625000, kX, kX, 0), a = 0.4;
        const double measuredFixed = -(kEps - a) * 1e-6 * f7 + P7 - P7;   // the step already out of the IQ
        const double measuredRaw = -(kEps - a) * 1e-6 * f7 + P7;          // left in
        const double r1 = measuredFixed - vibeafc::pllLeftInReading(P7, P7), r2 = measuredRaw - vibeafc::pllLeftInReading(P7, 0.0);
        ok(std::fabs(vibeafc::absolutePpm(vibeafc::Reference{"", f7, r1}, a) - kEps) < 1e-6
           && std::fabs(vibeafc::absolutePpm(vibeafc::Reference{"", f7, r2}, a) - kEps) < 1e-6,
           f("7D (step %+.1f Hz): either way auto-PPM is given the crystal's %.2f ppm, never the step twice", P7, kEps));
    }

    std::printf("F. the gate\n");
    {
        vibeafc::PllGate g; g.usbR82xx = true; g.autoOn = true;
        ok(vibeafc::pllToRemoveHz(g, -45.0) == -45.0, "a USB R820T in tuner mode, auto-PPM on: removed");
        vibeafc::PllGate t = g; t.tcp = true;            ok(vibeafc::pllToRemoveHz(t, -45.0) == 0.0, "rtl_tcp: untouched");
        vibeafc::PllGate d = g; d.directSampling = true; ok(vibeafc::pllToRemoveHz(d, -45.0) == 0.0, "direct sampling (the tuner bypassed): untouched");
        vibeafc::PllGate o = g; o.autoOn = false;        ok(vibeafc::pllToRemoveHz(o, -45.0) == 0.0, "auto-PPM off or suspended: the radio exactly as set");
        vibeafc::PllGate e = g; e.enabled = false;       ok(vibeafc::pllToRemoveHz(e, -45.0) == 0.0, "VIBE_PLL_IQ=0: untouched");
        vibeafc::PllGate n = g; n.usbR82xx = false;      ok(vibeafc::pllToRemoveHz(n, -45.0) == 0.0, "another tuner (E4000, FC0012 …): untouched");
        ok(vibeafc::pllToRemoveHz(g, std::nan("")) == 0.0, "no figure: nothing");
        ok(vibeafc::captureShiftHz(0.0, 97.665e6, 0.0) == 0.0, "nothing learned and no step: shift 0 — afcMix alters not one sample");
    }

    std::printf("G. the cost of the one mix pass (this host; the SIMD path; one %d-sample block, no allocation)\n", kBlock);
    {
        std::vector<cf32> z(kBlock, cf32(0.1f, 0.2f));
        vibedsp::NCO m; m.setFreq(-37.5 / 2.4e6);
        for (int i = 0; i < 50; ++i) m.mix(z.data(), z.data(), kBlock);   // warm
        const int reps = 400;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < reps; ++i) m.mix(z.data(), z.data(), kBlock);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / reps;
        for (double rate : {2.048e6, 2.4e6, 3.2e6}) {
            const double pct = us / (kBlock / rate * 1e6) * 100.0;
            std::printf("      %.3f MS/s: %.1f us per block, %.2f %% of one core\n", rate / 1e6, us, pct);
        }
        ok(us > 0, f("measured: %.1f us per block (%.2f ns a sample)", us, us * 1000.0 / kBlock));
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
