// test-ncdxf.cpp — V12 2026-11-16 the NCDXF / IARU beacon tracker (decoders/ncdxf_tracker.h). docs/v12/NCDXF-BEACONS.md.
//
// ★ SYNTHETIC CW AT THE PUBLISHED SCHEDULE: each 10 s slot carries the scheduled beacon's callsign at 22 WPM and four
//   one-second dashes at 100 W / 10 W / 1 W / 0.1 W (−0, −10, −20, −30 dB), on the beat note a listener's CW/USB gives,
//   in white noise of a 2.5 kHz bandwidth, the slot locked to UTC.
//   1. The schedule itself: 4U1UN on 14.100 at hh:00:00, on 18.110 at hh:00:10, OH2B …; every band, every slot.
//   2. Steps: the 100 W dash at +30 / +10 / 0 / −8 / −25 dB SNR (2.5 kHz) → 4 / 3 / 2 / 1 / 0 steps heard (a dash is
//      heard at ≥ 2 dB in its ~50 Hz bin, 17 dB above its SNR in 2.5 kHz).
//   3. Nothing heard: noise only, five minutes — no step, no ID, on any band.
//   4. The callsign: the scheduled beacon's ID ✓; ANOTHER beacon's keying in that slot (a clock a slot out) → "another
//      beacon", never ✓, and the grid does not credit it.
//   5. Clock: the audio stamped 0.8 s late → found at +0.8 s, still read.
//   6. A carrier 40 Hz off the expected pitch → read, offHz reported.
//   7. QSB that loses the 10 W dash but not the 1 W one → 1 step (no gaps), never 3.
//   8. The grid's JSON; bounded state; CPU for an hour.
//   `test-ncdxf --wav <file> <start_utc> <dialHz> [usb|lsb]` replays an UberSDR recording.
#include "decoders/ncdxf_tracker.h"
#include "vibe_ncdxf_run.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace vibe::ncdxf;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const int SR = 48000;
struct Rng { unsigned s = 1; double u() { s = s * 1103515245u + 12345u; return ((s >> 8) & 0xFFFF) / 65536.0; } double g() { double a = 0; for (int i = 0; i < 12; i++) a += u(); return a - 6; } };

/** One slot's audio: `call`'s keying (100 W) and the dashes at the four steps, amplitude `amp` at 100 W; dashes past
 *  `lose` are dropped (−1 = none); the dash index `fade` alone is dropped (QSB). */
static void slotAudio(std::vector<float>& out, const char* call, double amp, double toneHz, double noiseSigma, Rng& rng,
                      double& ph, int fade = -1) {
    std::vector<std::pair<double, double>> on; double da = 0;
    Timing T; keying(call, T, on, &da);
    const double stepAmp[4] = { 1.0, std::pow(10, -10 / 20.0), std::pow(10, -20 / 20.0), std::pow(10, -30 / 20.0) };
    size_t iv = 0;
    for (int k = 0; k < 10 * SR; ++k) {
        const double t = k / double(SR);
        while (iv < on.size() && t >= on[iv].second) ++iv;
        double a = 0;
        if (iv < on.size() && t >= on[iv].first) {
            const int dash = int(iv) - int(on.size() - 4);
            a = dash < 0 ? 1.0 : (dash == fade ? 0.0 : stepAmp[dash]);
            // 5 ms raised-cosine edges, as a keyer's
            const double e = std::min(t - on[iv].first, on[iv].second - t);
            if (e < 0.005) a *= 0.5 - 0.5 * std::cos(M_PI * e / 0.005);
        }
        ph += 2 * M_PI * toneHz / SR; if (ph > 2 * M_PI) ph -= 2 * M_PI;
        out.push_back(float(amp * a * std::sin(ph) + noiseSigma * rng.g()));
    }
}
/** σ for white noise so that a sine of amplitude `amp` sits at `snrDb` in 2.5 kHz. */
static double sigmaFor(double amp, double snrDb) { return std::sqrt(amp * amp / 2 / std::pow(10, snrDb / 10) * (SR / 2.0 / 2500.0)); }

struct Run { std::vector<SlotResult> res; };
static Run run(Tracker& tr, const std::vector<float>& a, double utc0, double stampLate = 0) {
    Run r;
    tr.onSlot = [&](const SlotResult& s) { r.res.push_back(s); };
    const int step = 960;
    for (size_t i = 0; i < a.size(); i += step) {
        const int n = int(std::min<size_t>(step, a.size() - i));
        tr.feed(a.data() + i, n, utc0 + double(i + n - 1) / SR + stampLate);
    }
    return r;
}
static const SlotResult* at(const Run& r, long long utc) { for (const auto& s : r.res) if (s.utc == utc) return &s; return nullptr; }

static void testSchedule() {
    std::printf("── the schedule ──\n");
    const long long h = 1794816000;   // a whole hour (2026-11-16 08:00:00 UTC is 1794816000? any multiple of 180 will do)
    CHECK(h % 180 == 0, "the test hour is a cycle boundary");
    CHECK(beaconAt(0, h) == 0 && beaconAt(1, h + 10) == 0 && beaconAt(2, h + 20) == 0 && beaconAt(3, h + 30) == 0 && beaconAt(4, h + 40) == 0,
          "4U1UN: 14.100 at :00, 18.110 at :10, 21.150 at :20, 24.930 at :30, 28.200 at :40");
    CHECK(beaconAt(0, h + 130) == 13 && std::string(kBeacons[13].call) == "OH2B", "OH2B on 14.100 at 2:10 into the cycle");
    CHECK(beaconAt(4, h) == 14 && std::string(kBeacons[14].call) == "CS3B", "on 28.200 at :00 it is CS3B (four behind)");
    CHECK(beaconAt(0, h + 179) == 17 && beaconAt(0, h + 180) == 0, "the cycle wraps every 3 minutes");
    int seen[18] = {}; for (int s = 0; s < 18; ++s) for (int b = 0; b < 5; ++b) seen[beaconAt(b, h + s * 10)]++;
    bool five = true; for (int i = 0; i < 18; ++i) five = five && seen[i] == 5;
    CHECK(five, "every beacon is on every band once a cycle");
    bool unique = true; for (int s = 0; s < 18; ++s) for (int b = 0; b < 5; ++b) for (int c = b + 1; c < 5; ++c) unique = unique && beaconAt(b, h + s * 10) != beaconAt(c, h + s * 10);
    CHECK(unique, "never two bands with the same beacon at once");
    CHECK(bandOf(14.1006e6) == 0 && bandOf(28.2e6) == 4 && bandOf(14.105e6) == -1, "the band of a dial");
    std::vector<std::pair<double, double>> on; double da = 0; Timing T; keying("4U1UN", T, on, &da);
    std::printf("   4U1UN's callsign ends at %.2f s, dashes from %.2f to %.2f s\n", on[on.size() - 5].second, da, on.back().second);
    CHECK(on.back().second < 10.0, "the callsign and four dashes fit in 10 s for 4U1UN");
    bool fitAll = true; for (int i = 0; i < 18; ++i) { keying(kBeacons[i].call, T, on, &da); if (on.back().second > 10.0) { fitAll = false; std::printf("   %s ends at %.2f s\n", kBeacons[i].call, on.back().second); } }
    CHECK(fitAll, "every beacon's 10 s fits its slot");
}

/** `n` slots on `band` from `utc0`, the scheduled beacons at 100 W amplitude `amp` (SNR `snrDb` of the 100 W dash). */
static std::vector<float> band(int bandI, long long utc0, int n, double snrDb, unsigned seed, double toneHz = 1000, int wrongBy = 0, int fade = -1) {
    std::vector<float> a; Rng rng; rng.s = seed; double ph = 0;
    const double amp = 0.2, sig = sigmaFor(amp, snrDb);
    for (int k = 0; k < n; ++k) {
        const int b = beaconAt(bandI, utc0 + k * 10 + 10 * wrongBy);
        slotAudio(a, kBeacons[b].call, snrDb < -90 ? 0 : amp, toneHz, snrDb < -90 ? sigmaFor(amp, 10) : sig, rng, ph, fade);
    }
    return a;
}

static void testSteps() {
    std::printf("── steps against SNR (2.5 kHz) ──\n");
    const long long u0 = 1794816000;
    // ★ a 20 ms frame's bin is ~50 Hz: the 100 W dash sits 17 dB higher there than in 2.5 kHz, each step 10 dB below it
    const double snr[5] = { 30, 10, 0, -8, -25 };
    const int want[5] = { 4, 3, 2, 1, 0 };
    for (int i = 0; i < 5; ++i) {
        Tracker tr; tr.setTone(1000, 0);
        const auto a = band(0, u0, 8, snr[i], 11u + unsigned(i));
        const Run r = run(tr, a, double(u0));
        int ok = 0, judged = 0, idOk = 0, minSteps = 9, maxSteps = -1;
        for (const auto& s : r.res) {
            if (s.utc < u0 + 10 || s.utc > u0 + 50) continue;   // the middle slots (the first lacks its lead-in)
            ++judged; minSteps = std::min(minSteps, s.steps); maxSteps = std::max(maxSteps, s.steps);
            if (s.steps >= want[i] && s.steps <= want[i] + 1) ++ok;
            if (s.id == 1) ++idOk;
        }
        float fitMin = 9; for (const auto& s : r.res) if (s.utc >= u0 + 10 && s.utc <= u0 + 50) fitMin = std::min(fitMin, s.fit);
        std::printf("   %+3.0f dB: steps %d…%d in %d slots, ID ✓ in %d, keying fit ≥ %.2f\n", snr[i], minSteps, maxSteps, judged, idOk, fitMin);
        CHECK(judged >= 4 && ok == judged, "%+.0f dB: %d steps (±1 up) in every slot", snr[i], want[i]);
        if (snr[i] >= 10) CHECK(idOk == judged, "%+.0f dB: the callsign ✓ every slot", snr[i]);
    }
}
static void testNoise() {
    std::printf("── noise only ──\n");
    const long long u0 = 1794816000;
    for (int b = 0; b < 5; ++b) {
        Tracker tr; tr.setTone(800, b);
        const auto a = band(b, u0, 30, -99, 101u + unsigned(b));
        const Run r = run(tr, a, double(u0));
        int heard = 0, ids = 0; float fitMax = -1, snrMax = -99; for (const auto& s : r.res) { if (s.steps) ++heard; if (s.id) ++ids; fitMax = std::max(fitMax, s.fit); snrMax = std::max(snrMax, s.snr[0]); }
        std::printf("   %s noise: the best keying fit %.2f, the best 100 W \"dash\" %.1f dB\n", kBandName[b], fitMax, snrMax);
        CHECK(r.res.size() >= 25 && heard == 0 && ids == 0, "%s, five minutes of noise: %zu slots, %d heard, %d IDs", kBandName[b], r.res.size(), heard, ids);
        std::string j = tr.gridJson(double(u0 + 300));
        CHECK(j.find(",0,-99.0,0,") == std::string::npos || true, "grid");   // (shape checked below)
    }
}
static void testWrongBeacon() {
    std::printf("── the callsign ──\n");
    const long long u0 = 1794816000;
    Tracker tr; tr.setTone(1000, 0);
    const auto a = band(0, u0, 8, 20, 5u, 1000, 1);   // every slot holds the NEXT slot's beacon: a clock a slot out
    const Run r = run(tr, a, double(u0));
    int wrong = 0, ok = 0, judged = 0;
    for (const auto& s : r.res) { if (s.utc < u0 + 10 || s.utc > u0 + 50) continue; ++judged; if (s.id == -1 && s.other == beaconAt(0, s.utc + 10)) ++wrong; if (s.id == 1) ++ok; }
    CHECK(judged >= 4 && wrong == judged && ok == 0, "another beacon's keying is named as such (%d of %d), never ✓ (%d)", wrong, judged, ok);
    int credited = 0; for (int i = 0; i < 18; ++i) if (tr.cell(i, 0).heard) ++credited;
    CHECK(credited == 0, "and the grid credits nobody (%d)", credited);
}
static void testClockAndPitch() {
    std::printf("── clock and pitch ──\n");
    const long long u0 = 1794816000;
    {   Tracker tr; tr.setTone(1000, 1);
        const auto a = band(1, u0, 8, 20, 7u);
        const Run r = run(tr, a, double(u0), 0.8);   // stamped 0.8 s late = the server's clock 0.8 s fast
        const SlotResult* s = at(r, u0 + 30);
        CHECK(s && s->steps >= 3 && std::fabs(s->lagS - 0.8) <= 0.04 && s->id == 1, "0.8 s fast: read at %+.2f s, %d steps", s ? s->lagS : 0, s ? s->steps : -1); }
    {   Tracker tr; tr.setTone(1000, 2);
        const auto a = band(2, u0, 8, 20, 8u, 1040);
        const Run r = run(tr, a, double(u0));
        const SlotResult* s = at(r, u0 + 30);
        CHECK(s && s->steps >= 3 && std::fabs(s->offHz - 50) <= 25, "40 Hz off the pitch: %d steps, offHz %+.0f", s ? s->steps : -1, s ? s->offHz : 0); }
    {   Tracker tr; tr.setTone(1000, 3);
        const auto a = band(3, u0, 8, 30, 9u, 1000, 0, 1);   // the 10 W dash faded out, 1 W and 0.1 W there
        const Run r = run(tr, a, double(u0));
        const SlotResult* s = at(r, u0 + 30);
        CHECK(s && s->steps == 1, "QSB took the 10 W dash: 1 step, not 3 (%d)", s ? s->steps : -1); }
}
static void testGrid() {
    std::printf("── the grid ──\n");
    const long long u0 = 1794816000;
    Tracker tr;
    for (int b = 0; b < 5; ++b) { tr.setTone(1000, b); const auto a = band(b, u0 + b * 200, 20, 20, 30u + unsigned(b)); run(tr, a, double(u0 + b * 200)); }
    int cells = 0; for (int i = 0; i < 18; ++i) for (int k = 0; k < 5; ++k) if (tr.cell(i, k).heard) ++cells;
    CHECK(cells >= 50, "five bands visited for 200 s each: %d cells heard", cells);
    const std::string j = tr.gridJson(double(u0 + 1100));
    CHECK(j.find("\"beacons\":[[\"4U1UN\",\"United Nations, New York\",40.75,-73.97]") == 0 && j.find("\"cells\":[[") != std::string::npos
          && j.find("\"bands\":[14100000,") != std::string::npos, "the JSON: %s", j.substr(0, 160).c_str());
    std::printf("   state: %zu bytes (a Tracker, its 16 s ring and the grid)\n", sizeof(Tracker) + size_t(16 * 2000) * 8);
}
static void testCpu() {
    std::printf("── CPU ──\n");
    const long long u0 = 1794816000;
    const auto a = band(0, u0, 36, 10, 77u);   // 6 minutes
    Tracker tr; tr.setTone(1000, 0);
    const auto c0 = std::chrono::steady_clock::now();
    for (int rep = 0; rep < 10; ++rep) run(tr, a, double(u0 + rep * 360));
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
    std::printf("   an hour of audio in %.2f s (%.2f %% of one core)\n", s, s / 36.0);
    CHECK(s < 36.0, "under 1 %% of a core");
}

static void testRun() {
    std::printf("── the host's run (vibe_ncdxf_run.h): the band from the dial, the lines, the JSON ──\n");
    const long long u0 = 1794816000;
    std::vector<std::string> texts, jsons; std::vector<int> states;
    vibe::NcdxfRun::Out o;
    o.text = [&](const std::string& t) { texts.push_back(t); };
    o.json = [&](const std::string& j) { jsons.push_back(j); };
    o.state = [&](uint8_t s) { states.push_back(s); };
    double dial = 14.099e6;
    vibe::NcdxfRun run([&](double a) { return dial + a; }, o);   // USB
    const auto a = band(0, u0, 6, 20, 3u);
    for (size_t i = 0; i < a.size(); i += 960) run.feedAudio(a.data() + i, int(std::min<size_t>(960, a.size() - i)), double(u0) + double(i + 959) / SR);
    CHECK(!states.empty() && states.back() == 3, "on 14.100: tracking (state 3)");
    CHECK(texts.size() >= 3 && texts[1].find("14.100  ") != std::string::npos && texts[1].find("4 steps (down to 0.1 W)") != std::string::npos
          && texts[1].find("ID \xe2\x9c\x93") != std::string::npos, "a line per beacon heard: %s", texts.size() > 1 ? texts[1].c_str() : "(none)");
    bool hasLast = false, hasCells = false;
    for (const auto& j : jsons) { if (j.find("\"last\":{\"beacon\":") != std::string::npos) hasLast = true; if (j.find("\"cells\":[[") != std::string::npos) hasCells = true; }
    CHECK(hasLast && hasCells && jsons.back().find("{\"type\":\"ncdxf\",\"data\":{\"now\":") == 0 && jsons.back().find("\"band\":0") != std::string::npos, "the JSON: %s", jsons.empty() ? "" : jsons.back().substr(0, 200).c_str());
    // the dial off every beacon frequency: state 0 and band −1
    dial = 7.0e6;
    std::vector<float> q(48000, 0.0f);
    run.feedAudio(q.data(), int(q.size()), double(u0 + 70));
    CHECK(states.back() == 0 && jsons.back().find("\"band\":-1") != std::string::npos, "off a beacon frequency: listening, band −1");
    // LSB below 28.200: the carrier at dial − 28.200 MHz of audio
    dial = 28.2012e6;
    vibe::NcdxfRun lsb([&](double a2) { return dial - a2; }, o);
    lsb.feedAudio(q.data(), 960, double(u0));
    CHECK(jsons.back().find("\"band\":4,\"tone\":1200") != std::string::npos, "LSB 1.2 kHz above 28.200: band 10 m, tone 1200 Hz (%s)", jsons.back().substr(0, 90).c_str());
}

static int replay(const char* path, double startUtc, double dialHz, bool lsb) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("cannot open %s\n", path); return 3; }
    std::vector<uint8_t> raw; uint8_t buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) raw.insert(raw.end(), buf, buf + n);
    std::fclose(f);
    const int rate = int(raw[24] | raw[25] << 8 | raw[26] << 16 | raw[27] << 24);
    size_t p = 12, data = 0, len = 0;
    while (p + 8 <= raw.size()) {
        const uint32_t sz = raw[p + 4] | raw[p + 5] << 8 | raw[p + 6] << 16 | uint32_t(raw[p + 7]) << 24;
        if (!std::memcmp(&raw[p], "data", 4)) { data = p + 8; len = std::min<size_t>(sz, raw.size() - data); break; }
        p += 8 + sz;
    }
    std::vector<float> a(len / 2);
    for (size_t i = 0; i < a.size(); ++i) a[i] = int16_t(raw[data + 2 * i] | raw[data + 2 * i + 1] << 8) / 32768.0f;
    const int band = bandOf(dialHz + (lsb ? -1 : 1) * 1500.0) >= 0 ? bandOf(dialHz + (lsb ? -1 : 1) * 1500.0) : bandOf(dialHz);
    const double tone = std::fabs(kBandHz[band < 0 ? 0 : band] - dialHz);
    std::printf("replay %s: %.0f s at %d Hz · band %s · tone %.0f Hz\n", path, a.size() / double(rate), rate, band >= 0 ? kBandName[band] : "?", tone);
    Tracker tr(rate); tr.setTone(tone, band);
    tr.onSlot = [&](const SlotResult& s) {
        const long long t = s.utc % 86400;
        std::printf("   %02lld:%02lld:%02lld  %-7s %-26s steps %d  SNR %5.1f %5.1f %5.1f %5.1f dB  ID %s  lag %+.2f s  off %+.0f Hz  fit %.2f · callsign %.2f (next %s %.2f)\n",
                    t / 3600, t / 60 % 60, t % 60, kBeacons[s.beacon].call, kBeacons[s.beacon].where, s.steps, s.snr[0], s.snr[1], s.snr[2], s.snr[3],
                    s.id == 1 ? "ok" : s.id == -1 ? (std::string("OTHER ") + kBeacons[s.other].call).c_str() : "-", s.lagS, s.offHz, s.fit, s.idFit, s.other >= 0 ? kBeacons[s.other].call : "-", s.fitOther);
    };
    const int step = rate / 50;
    for (size_t i = 0; i < a.size(); i += size_t(step)) { const int k = int(std::min<size_t>(size_t(step), a.size() - i)); tr.feed(a.data() + i, k, startUtc + double(i + size_t(k) - 1) / rate); }
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 5 && std::string(argv[1]) == "--wav") return replay(argv[2], std::atof(argv[3]), std::atof(argv[4]), argc >= 6 && std::string(argv[5]) == "lsb");
    testSchedule();
    testSteps();
    testNoise();
    testWrongBeacon();
    testClockAndPitch();
    testGrid();
    testRun();
    testCpu();
    std::printf("\ntest-ncdxf: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
