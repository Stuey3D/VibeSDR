// test-orbcomm-sky — the LIGHT ORBCOMM sky plot (vibe_orbcomm_sky.h, beta94): no packets, only where each bird is heard
// and how strongly. Synthetic, from the REAL FM116 elements of 11-24 (test/fixtures/orbcomm/) over the Pi 500's site:
// SDPSK carriers (vibe_orbcomm_tx.h) placed frame by frame at the bird's predicted Doppler.
//
// §1 the bird's own carrier (channel 265, a dongle 120 Hz low): named by its Doppler curve, dots at its az/el (SGP4, the
//    look in the dot's own 5 s) with the SNR it was sent at (Es/N0, ±3 dB), the looks that named it drawn too.
// §2 a carrier at the WRONG place — the same SDPSK held still (a spur that does not follow the Doppler), one between two
//    channels (raster + 1.25 kHz), one following half the Doppler — and pure noise: no dot, nothing named.
// §3 a second bird in the same plane 10° behind (a near-identical Doppler curve, shifted): the carrier is never drawn as it.
// §4 elements older than kMaxElementAgeSec: no dot.
// §5 two channels from one bird (FM116 sent on 137.6625 AND 137.800 on 11-24): both named, one dot per 5 s.
// §6 the cost: the light mode's frames for 60 s of a 2.4 MS/s capture against the full decoder (vibe_orbcomm_run.h) on
//    the same capture with FM116's carrier in it — far below.
// Silent. Run: scripts/run-tests.sh (VIBE_TESTS="test-orbcomm-sky").
#include "vibe_orbcomm_run.h"
#include "vibe_orbcomm_sky.h"
#include "vibe_orbcomm_tx.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) { g_pass++; } else { g_fail++; std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace vibeorb;

static const double kLat = 51.88, kLon = -0.42;
static double g_t0 = 0;   // 2026-11-24 16:53:19 UTC — FM116's pass

static bool loadTle(SkyBird& b, double maShiftDeg = 0, long long norad = 0) {
    std::ifstream in("test/fixtures/orbcomm/fm116-celestrak-20261124.tle");
    std::string n, l1, l2;
    if (!std::getline(in, n) || !std::getline(in, l1) || !std::getline(in, l2)) return false;
    vibe::sat::Elements e; std::string err;
    if (!vibe::sat::parseTle(l1, l2, e, err, "ORBCOMM FM116")) return false;
    if (maShiftDeg != 0) e.ma = std::fmod(e.ma + maShiftDeg * M_PI / 180 + 2 * M_PI, 2 * M_PI);
    b.norad = norad ? norad : e.norad; b.name = norad ? "ORBCOMM TWIN" : "ORBCOMM FM116"; b.ok = b.sgp.init(e); b.epochUnix = e.epochUnix();
    return b.ok;
}

struct Carrier { int dcn = 265; double radioErrHz = -120; double dopScale = 1.0; double extraHz = 0; bool still = false; double esn0 = 12; };

/** `seconds` of looks: 4 frames a second of N samples at fs, each with the carriers at their place at that moment. */
static void runPass(SkyWatch& sw, SkyBird bird, const std::vector<Carrier>& cs, double fs, double centre, double seconds, unsigned seed = 7) {
    const vibe::sat::Observer ob(kLat, kLon, 100);
    const int N = SkyWatch::fftFor(fs);
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0, 1);
    const double stillDop = vibe::sat::look(bird.sgp, ob, g_t0 + 60).dopplerHz(downlinkHz(265));
    for (double t = g_t0; t < g_t0 + seconds; t += 1.0 / SkyWatch::kFramesPerSec) {
        std::vector<std::complex<float>> x(size_t(N), { 0.f, 0.f });
        // noise: σ such that a carrier of amp 0.3 at Es/N0 = esn0 (tx::modulate's own scaling) — the noise at its rate
        const double es = 0.3 * 0.3 / kBaud, n0 = es / std::pow(10.0, 12.0 / 10.0), sigma = std::sqrt(n0 * fs / 2);
        for (auto& v : x) v = { float(sigma * g(rng)), float(sigma * g(rng)) };
        for (const Carrier& c : cs) {
            const double nominal = downlinkHz(c.dcn);
            const auto L0 = vibe::sat::look(bird.sgp, ob, t), L1 = vibe::sat::look(bird.sgp, ob, t + 1);
            const double d0 = c.still ? stillDop : c.dopScale * L0.dopplerHz(nominal), d1 = c.still ? stillDop : c.dopScale * L1.dopplerHz(nominal);
            if (!c.still && L0.elDeg < 0) continue;
            std::vector<uint8_t> bytes(size_t(N / (fs / kBaud) / 8) + 8);
            for (auto& b : bytes) b = uint8_t(rng());
            tx::Signal s; s.fs = fs; s.offsetHz = nominal + d0 + c.radioErrHz + c.extraHz - centre; s.slopeHzS = d1 - d0; s.amp = 0.3;
            s.esn0Db = 99; s.seed = rng();   // ★ the noise is added once, above, for every carrier alike
            const auto y = tx::modulate(bytes, s);
            const size_t off = (y.size() - size_t(N)) / 2;
            const float k = float(std::pow(10.0, (c.esn0 - 12.0) / 20.0));   // a carrier's own Es/N0 against the one noise
            for (int i = 0; i < N; i++) x[size_t(i)] += y[off + size_t(i)] * k;
        }
        sw.frame(x.data(), N, fs, centre, t);
    }
    sw.flush();
}

static SkyWatch::Opts opts(std::vector<SkyBird>& birds, std::vector<vibe::skyplot::Dot>& dots, std::vector<std::string>* logs = nullptr) {
    SkyWatch::Opts o;
    o.birds = [&birds] { return birds; };
    o.observer = [](vibe::sat::Observer& v) { v = vibe::sat::Observer(kLat, kLon, 100); return true; };
    o.dots = [&dots](const std::vector<vibe::skyplot::Dot>& d) { dots.insert(dots.end(), d.begin(), d.end()); };
    o.log = [logs](const std::string& l) { if (logs) logs->push_back(l); };
    return o;
}

int main() {
    { double jd; vibe::sat::parseIsoUtc("2026-11-24T16:53:19Z", jd); g_t0 = vibe::sat::unixFromJd(jd); }
    const double fs = 250000, centre = 137.65e6;
    SkyBird fm116;
    if (!loadTle(fm116)) { std::printf("fixture missing\n"); return 1; }
    const vibe::sat::Observer ob(kLat, kLon, 100);
    {
        auto L = vibe::sat::look(fm116.sgp, ob, g_t0 + 60);
        std::printf("   FM116 at +60 s: el %.1f°, az %.1f°, Doppler %.0f Hz\n", L.elDeg, L.azDeg, L.dopplerHz(downlinkHz(265)));
    }

    std::printf("§1 the bird's own carrier\n");
    {
        std::vector<SkyBird> birds{ fm116 }; std::vector<vibe::skyplot::Dot> dots; std::vector<std::string> logs;
        SkyWatch sw(opts(birds, dots, &logs));
        runPass(sw, fm116, { Carrier{} }, fs, centre, 240);
        const auto ch = sw.channelsOf(fm116.norad);
        CHECK(ch.size() == 1 && ch[0] == 265, "named on channel 265 (%zu channels%s)", ch.size(), ch.empty() ? "" : (", first " + std::to_string(ch[0])).c_str());
        CHECK(!logs.empty() && logs[0].find("named on 137.6625 MHz by its Doppler") != std::string::npos, "the log says how it was named (%s)", logs.empty() ? "-" : logs[0].c_str());
        CHECK(dots.size() >= 40 && dots.size() <= 49, "one dot per 5 s over 240 s, the naming looks included (%zu)", dots.size());
        double worst = 0, sumSnr = 0; int bad = 0;
        for (auto& d : dots) {
            double best = 1e9;
            for (double u = d.t; u < d.t + 5.01; u += 0.25) {
                const auto L = vibe::sat::look(fm116.sgp, ob, u);
                double daz = std::fabs(L.azDeg - d.azDeg); if (daz > 180) daz = 360 - daz;
                best = std::min(best, std::max(daz * std::cos(L.elDeg * M_PI / 180), std::fabs(L.elDeg - d.elDeg)));
            }
            worst = std::max(worst, best);
            sumSnr += d.snr;
            if (d.norad != uint32_t(fm116.norad) || d.kind != vibe::skyplot::kOrbcomm || d.count < 1 || d.count > 20) bad++;
        }
        CHECK(worst < 0.05, "each dot at the bird's az/el in its 5 s (worst %.3f°)", worst);
        CHECK(bad == 0, "every dot's bird, kind and count (%d bad)", bad);
        const double meanSnr = dots.empty() ? 0 : sumSnr / double(dots.size());
        CHECK(std::fabs(meanSnr - 12) <= 3, "the SNR it was sent at, 12 dB Es/N0 (mean %.1f dB)", meanSnr);
        std::printf("   %zu dots, mean SNR %.1f dB, worst place %.3f°\n", dots.size(), meanSnr, worst);
    }

    std::printf("§2 carriers at the wrong place, and noise\n");
    {
        struct Case { const char* what; Carrier c; };
        Carrier still; still.still = true;
        Carrier between; between.extraHz = 1250;
        Carrier half; half.dopScale = 0.5;
        Carrier none; none.esn0 = -99;
        for (const Case& k : { Case{ "held still (a spur)", still }, Case{ "between two channels", between }, Case{ "half the Doppler", half }, Case{ "noise only", none } }) {
            std::vector<SkyBird> birds{ fm116 }; std::vector<vibe::skyplot::Dot> dots;
            SkyWatch sw(opts(birds, dots));
            runPass(sw, fm116, { k.c }, fs, centre, 240, 11);
            CHECK(dots.empty() && sw.learnt().empty(), "%s: no dot (%zu), nothing named (%zu)", k.what, dots.size(), sw.learnt().size());
        }
    }

    std::printf("§3 a twin in the same plane, 10° behind\n");
    {
        SkyBird twin; loadTle(twin, -10, 99999);
        std::vector<SkyBird> birds{ fm116, twin }; std::vector<vibe::skyplot::Dot> dots;
        auto Lt = vibe::sat::look(twin.sgp, ob, g_t0 + 120);
        SkyWatch sw(opts(birds, dots));
        runPass(sw, fm116, { Carrier{} }, fs, centre, 240);
        int asTwin = 0, asFm = 0; for (auto& d : dots) (d.norad == 99999u ? asTwin : asFm)++;
        CHECK(asTwin == 0 && asFm >= 40, "drawn as FM116 only (FM116 %d, twin %d; twin el %.0f° at +120 s)", asFm, asTwin, Lt.elDeg);
    }

    std::printf("§4 elements too old\n");
    {
        SkyBird old = fm116; old.epochUnix = g_t0 - SkyWatch::kMaxElementAgeSec - 3600;
        std::vector<SkyBird> birds{ old }; std::vector<vibe::skyplot::Dot> dots;
        SkyWatch sw(opts(birds, dots));
        runPass(sw, fm116, { Carrier{} }, fs, centre, 120);
        CHECK(dots.empty(), "no dot from %.0f-day-old elements (%zu)", SkyWatch::kMaxElementAgeSec / 86400 + 0.04, dots.size());
    }

    std::printf("§5 two channels from one bird\n");
    {
        std::vector<SkyBird> birds{ fm116 }; std::vector<vibe::skyplot::Dot> dots;
        SkyWatch sw(opts(birds, dots));
        Carrier a; Carrier b; b.dcn = 320; b.esn0 = 9;
        runPass(sw, fm116, { a, b }, 800000, 137.5e6, 240);
        const auto ch = sw.channelsOf(fm116.norad);
        CHECK(ch.size() == 2, "both channels named (%zu)", ch.size());
        std::map<uint32_t, int> perBucket; for (auto& d : dots) perBucket[d.t]++;
        int dup = 0; for (auto& kv : perBucket) if (kv.second > 1) dup++;
        CHECK(dots.size() >= 40 && dup == 0, "one dot per 5 s, not one per carrier (%zu dots, %d doubled)", dots.size(), dup);
    }

    std::printf("§6 the cost\n");
    {
        using clk = std::chrono::steady_clock;
        // the light mode: 60 s of a 2.4 MS/s capture = 120 frames of 32768 points
        std::vector<SkyBird> birds{ fm116 }; std::vector<vibe::skyplot::Dot> dots;
        SkyWatch sw(opts(birds, dots));
        const int N = SkyWatch::fftFor(2.4e6);
        std::vector<std::complex<float>> fr(static_cast<size_t>(N));
        std::mt19937 rng(3); std::normal_distribution<float> g(0, 0.05f);
        for (auto& v : fr) v = { g(rng), g(rng) };
        auto a0 = clk::now();
        for (int i = 0; i < 60 * int(SkyWatch::kFramesPerSec); i++) sw.frame(fr.data(), N, 2.4e6, 137.5e6, g_t0 + i / SkyWatch::kFramesPerSec);
        const double light = std::chrono::duration<double>(clk::now() - a0).count() / 60.0;
        // the full decoder as the server runs it: its slice cut from the same 2.4 MS/s capture, FM116's carrier in it (6 s)
        const double sfs = 2.4e6, secs = 6;
        std::vector<uint8_t> bytes(size_t(secs * kBaud / 8));
        for (auto& b : bytes) b = uint8_t(rng());
        const auto L0 = vibe::sat::look(fm116.sgp, ob, g_t0 + 60), L1 = vibe::sat::look(fm116.sgp, ob, g_t0 + 60 + secs);
        tx::Signal s; s.fs = sfs; s.offsetHz = downlinkHz(265) + L0.dopplerHz(downlinkHz(265)) - 137.5e6;
        s.slopeHzS = (L1.dopplerHz(downlinkHz(265)) - L0.dopplerHz(downlinkHz(265))) / secs; s.esn0Db = 12;
        const auto y = tx::modulate(bytes, s);
        Run::Opts o; o.widthHz = 640000; o.birds = [] { return std::vector<Bird>(); }; o.observer = [](vibe::sat::Observer&) { return false; };
        Run run(o);
        auto b0 = clk::now();
        for (size_t i = 0; i < y.size(); i += 16384) run.feedIq(&y[i], int(std::min<size_t>(16384, y.size() - i)), sfs, 137.5e6);
        const double full = std::chrono::duration<double>(clk::now() - b0).count() / (double(y.size()) / sfs);
        std::printf("   light: %.2f ms per second of a 2.4 MS/s capture (%.3f %% of a core) · full decoder: %.1f ms per second of the capture (%.2f %%) — %.0f× less\n",
                    light * 1e3, light * 100, full * 1e3, full * 100, full / std::max(1e-9, light));
        CHECK(light * 5 < full, "the light mode costs far less than the full decoder (%.2f vs %.1f ms/s)", light * 1e3, full * 1e3);
        CHECK(light < 0.02, "under 2 %% of a core here (%.3f %%)", light * 100);
    }
    std::printf("test-orbcomm-sky: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
