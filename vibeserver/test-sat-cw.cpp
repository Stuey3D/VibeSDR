// test-sat-cw — V12 beta90 1.1: a SATELLITE CW BEACON on a listener's audio, as a SATELLITE pick of a beacon runs it
// (decoders/cw_skimmer.h, the listener's CW decoder; docs/v12/SATELLITE-BEST-SERVER.md). With the Doppler lock on, what
// is left is the residual — a few Hz/s (an old element set, the clock): the beacon must still be read. With the lock OFF
// (the full Doppler rate near TCA) the figure is printed, not pinned — it is why AUTO DOPPLER defaults on.
// Synthetic Morse whose every character is known (benchsynth/cw_synth.h). Silent, deterministic.
#include "decoders/cw_skimmer.h"
#include "benchsynth/cw_synth.h"
#include <cstdio>
#include <map>
#include <string>

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

int main() {
    std::printf("§1 a satellite CW beacon through the listener's CW decoder\n");
    const double locked = (beacon(4, 3, 11) + beacon(-6, 3, 12) + beacon(5, 3, 13)) / 3;
    char b[200];
    std::snprintf(b, sizeof b, "20 WPM at 3 dB with a locked pass's residual (±4–6 Hz/s): CER %.1f %% (≤ 5 %%)", locked);
    ok(locked <= 5.0, b);
    const double fast = (beacon(5, 6, 21, 30) + beacon(-5, 6, 22, 30)) / 2;
    std::snprintf(b, sizeof b, "30 WPM at 6 dB, the same residual: CER %.1f %% (≤ 5 %%)", fast);
    ok(fast <= 5.0, b);
    std::printf("   [info] AUTO DOPPLER off (−60 Hz/s, 10 dB): CER %.1f %%\n", beacon(-60, 10, 14));
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
