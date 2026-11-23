// test-sat-cw — V12 beta90 1.1: a SATELLITE CW BEACON on a listener's audio, as a SATELLITE pick of a beacon runs it
// (decoders/cw_skimmer.h, the listener's CW decoder; docs/v12/SATELLITE-BEST-SERVER.md). With the Doppler lock on, what
// is left is the residual — a few Hz/s (an old element set, the clock): the beacon must still be read. With the lock OFF
// (the full Doppler rate near TCA) the figure is printed, not pinned — it is why AUTO DOPPLER defaults on.
// Synthetic Morse whose every character is known (benchsynth/cw_synth.h). Silent, deterministic.
// ★★ beta92 §2: a beacon pick now runs the CW SKIMMER over 24 kHz centred on the Doppler-corrected prediction (vibe_cw_run.h
//    aimed — Stuart's RS-44 pass 11-23, the beacon ~2 kHz below the figure): found, read, and its offset measured.
#include "decoders/cw_skimmer.h"
#include "benchsynth/cw_synth.h"
#include "vibe_cw_run.h"
#include "vibe_band_slice.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>

using namespace cwsynth;
namespace cw = vibe::cw;
static int g_pass = 0, g_fail = 0;
static void ok(bool c, const char* what) { std::printf("   %s %s\n", c ? "ok  " : "FAIL", what); (c ? g_pass : g_fail)++; }

static double beacon(double driftHzS, double snr, uint64_t seed, double wpm = 20) {
    Rng rng(seed);
    Keying k; k.wpm = wpm;
    std::string sent; while (sent.size() < 200) sent += "RS44 RS44 BEACON TLM 123 456 789 ";
    Tone t; t.hz = 800; t.snrDb = snr; t.driftHzPerSec = driftHzS; t.env = keyEnvelope(sent, k, 12000, rng);
    std::vector<Tone> ts{ t }; std::vector<cf> iq; std::vector<float> re;
    synth(ts, 12000, false, t.env.size() / 12000.0, rng, iq, re);
    cw::Skimmer::Config c; c.rate = 12000; c.complexIn = false; c.loHz = 150; c.hiHz = 3600; c.maxSignals = 24;
    cw::Skimmer sk(c);
    std::map<int, std::string> text;
    sk.onText = [&](int id, double, const std::string& s) { text[id] += s; };
    for (size_t i = 0; i < re.size(); i += 512) sk.feedReal(re.data() + i, (int)std::min<size_t>(512, re.size() - i));
    std::string got; for (auto& kv : text) if (kv.second.size() > got.size()) got = kv.second;
    int e, n; cer(sent, got, e, n);
    return 100.0 * e / std::max(1, (int)squash(sent).size());
}

/* ★★ beta92 THE BEACON PICK'S SKIMMER (vibe_cw_run.h + vibe_band_slice.h skimAimCentre — the shim's cut, reproduced). A
 * pass segment through TCA: the prediction (the aim) moves with a Doppler curve of up to 150 Hz/s (steeper than a LEO's
 * ~80 Hz/s at 70 cm), the beacon sits `errHz` from it (catalogue figure / dongle error), and the slice is cut on whole bins
 * of a 2.4 MS/s, 16384-point FFT held until the aim drifts (as feedSkimFromBins cuts it), so CwRun's mix carries the rest. */
struct BeaconSkim { bool found = false, aimed = false; double off = 0, cer = 100, secs = 0, cpu = 0, rate = 0; int len = 0, recuts = 0; std::string text; };
static std::string jStr(const std::string& j, size_t at, const char* key) {
    const std::string k = std::string("\"") + key + "\":\"";
    const size_t p = j.find(k, at); if (p == std::string::npos) return "";
    const size_t e = j.find('"', p + k.size()); return j.substr(p + k.size(), e - p - k.size());
}
static double jNum(const std::string& j, size_t at, const char* key) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t p = j.find(k, at); return p == std::string::npos ? NAN : std::atof(j.c_str() + p + k.size());
}
static BeaconSkim beaconSkim(double errHz, double snr, uint64_t seed) {
    BeaconSkim r;
    const double fs = 2.4e6, binHz = fs / 16384.0, width = 24000;
    const int bins = vibe::bandSliceBins(fs, 16384, width);
    const double rate = bins * binHz, hw = 435.0e6, nominal = 435605000.0;
    r.rate = rate;
    Rng rng(seed);
    Keying k; k.wpm = 20;
    std::string sent; while (sent.size() < 260) sent += "RS44 RS44 BEACON TLM 123 456 789 ";
    const std::vector<float> env = keyEnvelope(sent, k, rate, rng);
    const size_t n = env.size();
    r.secs = n / rate;
    const double T = r.secs;
    auto dop = [&](double t) { return -9000.0 * std::tanh((t - T / 2) / 60.0); };   // ≤ 150 Hz/s at TCA
    const double pc = std::pow(10.0, snr / 10.0) * 2500.0 / rate, amp = std::sqrt(pc), ns = std::sqrt(0.5);
    std::string last;
    vibe::CwRun::Out o;
    o.json = [&](const std::string& j) { if (j.find("\"cw_signals\"") != std::string::npos) last = j; };
    vibe::CwRun run(true, width, nullptr, o);
    double held = 0, prevCut = 0, ph = rng.uni() * 6.283;
    std::vector<vibe::CwRun::cf32> blk(1024);
    double cpu = 0;
    for (size_t i = 0; i < n; i += blk.size()) {
        const double t0 = i / rate;
        const double aim = nominal + dop(t0);                                     // the tracker's aim (25 Hz — per block here)
        const double c = vibe::skimAimCentre(held, aim, rate, width);
        const double cut = hw + std::round((c - hw) / binHz) * binHz;             // a whole bin, as the shim cuts it
        if (prevCut != 0 && cut != prevCut) ++r.recuts;
        prevCut = cut;
        const int m = (int)std::min(blk.size(), n - i);
        for (int s = 0; s < m; ++s) {
            const double t = (i + s) / rate;
            const double f = nominal + dop(t) + errHz - cut;                      // the beacon's place in this slice
            ph += 2 * M_PI * f / rate;
            const double a = amp * env[i + s];
            blk[(size_t)s] = vibe::CwRun::cf32((float)(a * std::cos(ph) + ns * rng.gauss()), (float)(a * std::sin(ph) + ns * rng.gauss()));
        }
        const auto c0 = std::chrono::steady_clock::now();
        run.feedIq(blk.data(), m, rate, cut, aim);
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
    }
    r.cpu = cpu;
    // ★ the list is sent at most once a second on the wall clock — one more block after a pause makes it send what it has
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    run.feedIq(blk.data(), 64, rate, prevCut, nominal + dop(T));
    r.aimed = last.find("\"aim\":true") != std::string::npos;
    const double centre = jNum(last, 0, "centre");
    for (size_t p = last.find("{\"id\":"); p != std::string::npos; p = last.find("{\"id\":", p + 1)) {
        const std::string tx = jStr(last, p, "text");
        if (tx.size() < (size_t)r.len) continue;
        r.found = true; r.len = (int)tx.size(); r.text = tx; r.off = jNum(last, p, "hz") - centre;
        int e = 0, nr = 0; cer(sent, tx, e, nr);
        r.cer = 100.0 * e / std::max(1, (int)squash(tx).size());
    }
    return r;
}

int main() {
    std::printf("§1 a satellite CW beacon through the listener's CW decoder\n");
    const double locked = (beacon(4, 3, 11) + beacon(-6, 3, 12) + beacon(5, 3, 13)) / 3;
    char b[400];
    std::snprintf(b, sizeof b, "20 WPM at 3 dB with a locked pass's residual (±4–6 Hz/s): CER %.1f %% (≤ 5 %%)", locked);
    ok(locked <= 5.0, b);
    const double fast = (beacon(5, 6, 21, 30) + beacon(-5, 6, 22, 30)) / 2;
    std::snprintf(b, sizeof b, "30 WPM at 6 dB, the same residual: CER %.1f %% (≤ 5 %%)", fast);
    ok(fast <= 5.0, b);
    std::printf("   [info] AUTO DOPPLER off (−60 Hz/s, 10 dB): CER %.1f %%\n", beacon(-60, 10, 14));

    std::printf("\n§2 beta92: the beacon through the CW SKIMMER, 24 kHz centred on the corrected prediction (aimed)\n");
    {
        // RS-44's beacon 2 kHz BELOW the prediction (Stuart's pass, 11-23): a one-signal CW passband (±600 Hz) misses it
        const BeaconSkim r = beaconSkim(-2000, 4, 31);
        std::snprintf(b, sizeof b, "the skimmer finds a signal %.0f Hz from the prediction (want −2000 ± 60)", r.off);
        ok(r.found && std::fabs(r.off + 2000) <= 60, b);
        std::snprintf(b, sizeof b, "its text is read: CER %.1f %% over the last %d characters (≤ 8 %%) — \"%s\"", r.cer, r.len, r.text.c_str());
        ok(r.found && r.len >= 40 && r.cer <= 8.0, b);
        ok(r.aimed, "the list says the window is aimed (\"aim\":true) — each signal's hz − centre is its offset from the prediction");
        std::snprintf(b, sizeof b, "the slice was re-cut %d times over a %.0f s pass segment at up to 150 Hz/s (whole-bin cuts held, ≤ 12)", r.recuts, r.secs);
        ok(r.recuts >= 1 && r.recuts <= 12, b);
        const BeaconSkim hi = beaconSkim(+3500, 4, 32);
        std::snprintf(b, sizeof b, "one 3.5 kHz ABOVE: found at %.0f Hz, CER %.1f %%", hi.off, hi.cer);
        ok(hi.found && std::fabs(hi.off - 3500) <= 60 && hi.cer <= 8.0, b);
        std::printf("   [info] CPU: the 24 kHz skimmer (slice %.1f kS/s) ran %.1f s of IQ in %.3f s of CPU — %.2f %% of one M5 Pro core\n",
                    r.rate / 1000.0, r.secs, r.cpu, 100.0 * r.cpu / r.secs);
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
