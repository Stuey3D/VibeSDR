// test-digi-spots.cpp — DIGITAL SPOTS, V12 (2026-10-26): the mode registry, the band table, band stepping, the passband
// per band, Band mode's tuner, and a whole synthetic BAND decoded end to end through a DecoderHost — FT8 and FT4 on their
// own dials in one IQ slice, each spot out with its mode and its RF. Plus the lower sideband (an FT8 signal BELOW the FT8
// dial must not decode as one above it), and pure noise (no spot at all).
//
//   test-digi-spots          the tests
//   test-digi-spots cpu      + the CPU of Band mode vs Single mode on this machine (docs/v12/DIGITAL-SPOTS.md)
#include "vibe_decoder_host.h"
#include "vibe_clock.h"
#include "decoders/digi_bands.h"
#include "decoders/digi_spotter.h"
#include "decoders/digi_tuner.h"
#include "decoders/digimodes/wspr_decoder.h"
#include "../tools/wspr-bench/wspr_synth.h"
extern "C" {
#include "ft8/message.h"
#include "ft8/encode.h"
#include "ft8/constants.h"
}
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

using namespace vibe;
using cf32 = std::complex<float>;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
static double cpuSecs() {
    rusage u{}; getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6;
}

// ── signals ──────────────────────────────────────────────────────────────────────────────────────
/** One FT8 (or FT4) transmission's tones for `msg`, or empty. */
static std::vector<uint8_t> tonesFor(const char* msg, bool ft4) {
    ftx_message_t m; ftx_message_init(&m);
    if (ftx_message_encode(&m, nullptr, msg) != FTX_MESSAGE_RC_OK) return {};
    std::vector<uint8_t> t(ft4 ? FT4_NN : FT8_NN);
    if (ft4) ft4_encode(m.payload, t.data()); else ft8_encode(m.payload, t.data());
    return t;
}
/** Add one transmission to a complex slice at `rate`, `offHz` from the slice centre, starting `startSec` in. Continuous
 *  phase FSK — the tone spacing and symbol length of FT8 (6.25 Hz, 0.16 s) or FT4 (20.833 Hz, 0.048 s). */
static void addTx(std::vector<cf32>& x, double rate, double offHz, double startSec, const std::vector<uint8_t>& tones,
                  bool ft4, double amp) {
    const double sym = ft4 ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD, spacing = 1.0 / sym;
    const size_t s0 = (size_t)(startSec * rate), ns = (size_t)std::llround(sym * rate);
    double ph = 0;
    for (size_t k = 0; k < tones.size(); ++k)
        for (size_t j = 0; j < ns; ++j) {
            const size_t i = s0 + k * ns + j;
            if (i >= x.size()) return;
            ph += 2 * M_PI * (offHz + tones[k] * spacing) / rate;
            x[i] += cf32((float)(amp * std::cos(ph)), (float)(amp * std::sin(ph)));
        }
}
static void addNoise(std::vector<cf32>& x, double sigma, unsigned seed) {
    std::mt19937 g(seed); std::normal_distribution<float> n(0.0f, (float)sigma);
    for (auto& v : x) v += cf32(n(g), n(g));
}

/** A decoder socket that keeps the spot frames. */
struct Peer : DecoderPeer {
    std::mutex m; std::vector<std::string> txt;
    bool open() const override { return true; }
    void binary(const uint8_t*, size_t) override {}
    void text(const std::string& s) override { std::lock_guard<std::mutex> lk(m); txt.push_back(s); }
    std::vector<std::string> spots() { std::lock_guard<std::mutex> lk(m); std::vector<std::string> o; for (auto& t : txt) if (t.find("digital_spot") != std::string::npos) o.push_back(t); return o; }
};
static std::string field(const std::string& j, const char* k) {
    const std::string key = std::string("\"") + k + "\":";
    size_t p = j.find(key); if (p == std::string::npos) return "";
    p += key.size();
    if (j[p] == '"') { const size_t e = j.find('"', p + 1); return j.substr(p + 1, e - p - 1); }
    size_t e = p; while (e < j.size() && j[e] != ',' && j[e] != '}') ++e;
    return j.substr(p, e - p);
}

/** A mode with no engine of its own that never decodes — to cost spans with modes not yet built. */
static digi::ModeDef dummyMode(const char* name, float lo, float hi) {
    digi::ModeDef m; m.name = name; m.audioLoHz = lo; m.audioHiHz = hi;
    struct Quiet : digi::Engine { void process(const float*, int, double) override {} };
    m.make = [] { return std::unique_ptr<digi::Engine>(new Quiet()); };
    return m;
}

/** ★ `clip <out.cu8> <rate> <centreHz>`: 15 s of 20 m as an RTL delivers it (unsigned 8-bit IQ), UTC-ANCHORED — clip time
 *  t plays at UTC ≡ t (mod 15) under fake-rtl-tcp --hf-utc — for scripts/test-server-spots.mjs. FT8 at +0.5 s, FT4 at
 *  +0.5 and +8.0 s, and one FT8 station on the lower sideband of the FT8 dial. */
static int writeClip(const char* path, double rate, double centre) {
    std::vector<cf32> x((size_t)(rate * 15.0), cf32(0, 0));
    struct Tx { const char* msg; bool ft4; double rf, start; };
    const Tx txs[] = { { "CQ G4ABC IO92", false, 14074600, 0.5 }, { "CQ DL1XYZ JO31", false, 14075450, 0.5 }, { "CQ K1ABC FN42", false, 14076300, 0.5 },
                       { "CQ JA1QRS PM95", true, 14080800, 0.5 }, { "CQ EA4TUV IN80", true, 14081700, 8.0 },
                       { "CQ ZL3LSB RE66", false, 14072800, 0.5 } };
    for (const Tx& t : txs) addTx(x, rate, t.rf - centre, t.start, tonesFor(t.msg, t.ft4), t.ft4, 4.0);
    addNoise(x, 5.0, 21);
    std::vector<uint8_t> u(x.size() * 2);
    for (size_t i = 0; i < x.size(); ++i) {
        u[2 * i] = (uint8_t)std::max(0.0, std::min(255.0, std::round(127.5 + x[i].real())));
        u[2 * i + 1] = (uint8_t)std::max(0.0, std::min(255.0, std::round(127.5 + x[i].imag())));
    }
    FILE* f = std::fopen(path, "wb");
    if (!f) return 1;
    std::fwrite(u.data(), 1, u.size(), f);
    std::fclose(f);
    std::printf("wrote %s: 15 s at %.0f Hz centred on %.0f\n", path, rate, centre);
    return 0;
}

/** ★ `real <iq.wav> <start_utc> <centreHz> [swap]`: a REAL recording — a stereo int16 I/Q WAV (ubersdr-rec … iq48), its first
 *  sample's UTC — through Band mode, faster than real time, stamped by the recording's own clock. Prints every spot, then
 *  the per-mode counts (compared by hand against the receiver's own decodes for the same minutes). */
static int runReal(const char* path, double startUtc, double centre, bool swap, bool live) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("cannot open %s\n", path); return 3; }
    std::vector<uint8_t> raw; { uint8_t b[65536]; size_t k; while ((k = std::fread(b, 1, sizeof b, f)) > 0) raw.insert(raw.end(), b, b + k); }
    std::fclose(f);
    if (raw.size() < 44 || std::memcmp(raw.data(), "RIFF", 4) != 0) { std::printf("not a WAV\n"); return 1; }
    uint32_t rate = 0; uint16_t chans = 0; size_t data = 0, dlen = 0;
    for (size_t p = 12; p + 8 <= raw.size();) {
        const uint32_t len = raw[p + 4] | raw[p + 5] << 8 | raw[p + 6] << 16 | (uint32_t)raw[p + 7] << 24;
        if (!std::memcmp(&raw[p], "fmt ", 4)) { chans = raw[p + 10] | raw[p + 11] << 8; rate = raw[p + 12] | raw[p + 13] << 8 | raw[p + 14] << 16 | (uint32_t)raw[p + 15] << 24; }
        if (!std::memcmp(&raw[p], "data", 4)) { data = p + 8; dlen = std::min<size_t>(len, raw.size() - data); break; }
        p += 8 + len + (len & 1);
    }
    if (chans != 2 || !rate || !data) { std::printf("need stereo I/Q (got %u ch, %u Hz)\n", chans, rate); return 1; }
    const size_t n = dlen / 4;
    std::vector<cf32> x(n);
    for (size_t i = 0; i < n; ++i) {
        const int16_t a = (int16_t)(raw[data + 4 * i] | raw[data + 4 * i + 1] << 8), b = (int16_t)(raw[data + 4 * i + 2] | raw[data + 4 * i + 3] << 8);
        x[i] = swap ? cf32(b / 32768.0f, a / 32768.0f) : cf32(a / 32768.0f, b / 32768.0f);
    }
    std::map<std::string, int> per; std::mutex gm; std::vector<std::string> lines;
    digi::Spotter sp(digi::Spotter::Scope::Band, digi::modes(), [&](const digi::Spot& s, double rf) {
        char b[160]; std::snprintf(b, sizeof b, "%s %-10s %6s %+4d %.0f", s.mode.c_str(), s.call.c_str(), s.grid.c_str(), s.snr, rf);
        std::lock_guard<std::mutex> lk(gm); per[s.mode]++; lines.push_back(b); }, nullptr, nullptr);
    /* ★ `live`: fed in REAL TIME, its clock shifted by whole minutes so the slots still line up — the FT8 pass gate
     *  judges its time budget against the wall clock, so a recording fed minutes after it was made, or faster than real
     *  time, gets ONE pass (what a loaded server pays); fed live it gets what a live listener's would. */
    const double shift = live ? std::ceil((vibeUtcNow() - startUtc) / 60.0) * 60.0 : 0.0;
    const auto w0 = std::chrono::steady_clock::now();
    const double c0 = cpuSecs();
    for (size_t o = 0; o < n; o += 2048) {
        const int k = (int)std::min<size_t>(2048, n - o);
        if (live) std::this_thread::sleep_until(w0 + std::chrono::microseconds((int64_t)(1e6 * (double)(o + k) / rate)));
        sp.feedIq(&x[o], k, rate, centre, startUtc + shift + (double)(o + k) / rate);
        if (!live && (o / 2048) % 16 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::this_thread::sleep_for(std::chrono::seconds(4));
    std::printf("%s: %.0f s, engines %s, slots decoded %llu, skipped %llu, CPU %.2f s (%.3f %% of the recording's length)\n", path, n / (double)rate,
                sp.describe().c_str(), (unsigned long long)sp.slotsDecoded(), (unsigned long long)sp.slotsSkipped(), cpuSecs() - c0, 100.0 * (cpuSecs() - c0) / (n / (double)rate));
    for (auto& l : lines) std::printf("  %s\n", l.c_str());
    for (auto& kv : per) std::printf("%s: %d spots\n", kv.first.c_str(), kv.second);
    return 0;
}

/** ★ `realusb <usb.wav> <start_utc> <dialHz> [live]`: the SAME minutes as USB audio (ubersdr-rec … usb, 12 kHz mono) through
 *  SINGLE mode — the A/B for Band's tuner: identical engines, only the way the audio was made differs. */
static int runRealUsb(const char* path, double startUtc, double dial, bool live) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("cannot open %s\n", path); return 3; }
    std::vector<uint8_t> raw; { uint8_t b[65536]; size_t k; while ((k = std::fread(b, 1, sizeof b, f)) > 0) raw.insert(raw.end(), b, b + k); }
    std::fclose(f);
    size_t data = 0, dlen = 0; uint32_t rate = 0; uint16_t chans = 0;
    for (size_t p = 12; p + 8 <= raw.size();) {
        const uint32_t len = raw[p + 4] | raw[p + 5] << 8 | raw[p + 6] << 16 | (uint32_t)raw[p + 7] << 24;
        if (!std::memcmp(&raw[p], "fmt ", 4)) { chans = raw[p + 10] | raw[p + 11] << 8; rate = raw[p + 12] | raw[p + 13] << 8 | raw[p + 14] << 16 | (uint32_t)raw[p + 15] << 24; }
        if (!std::memcmp(&raw[p], "data", 4)) { data = p + 8; dlen = std::min<size_t>(len, raw.size() - data); break; }
        p += 8 + len + (len & 1);
    }
    if (chans != 1 || rate != 12000 || !data) { std::printf("need 12 kHz mono\n"); return 1; }
    const size_t n = dlen / 2;
    std::vector<float> a48(n * 4);
    for (size_t i = 0; i < n; ++i) { const float v = (int16_t)(raw[data + 2 * i] | raw[data + 2 * i + 1] << 8) / 32768.0f; for (int k = 0; k < 4; ++k) a48[4 * i + k] = v; }
    std::map<std::string, int> per; std::mutex gm; std::vector<std::string> lines;
    digi::Spotter sp(digi::Spotter::Scope::Single, digi::modes(), [&](const digi::Spot& s, double rf) {
        char b[160]; std::snprintf(b, sizeof b, "%s %-10s %6s %+4d %.0f", s.mode.c_str(), s.call.c_str(), s.grid.c_str(), s.snr, rf);
        std::lock_guard<std::mutex> lk(gm); per[s.mode]++; lines.push_back(b); }, [dial] { return dial; }, nullptr);
    const double shift = live ? std::ceil((vibeUtcNow() - startUtc) / 60.0) * 60.0 : 0.0;
    const auto w0 = std::chrono::steady_clock::now();
    for (size_t o = 0; o < a48.size(); o += 3840) {
        const int k = (int)std::min<size_t>(3840, a48.size() - o);
        if (live) std::this_thread::sleep_until(w0 + std::chrono::microseconds((int64_t)(1e6 * (double)(o + k) / 48000.0)));
        sp.feedAudio48k(&a48[o], k, startUtc + shift + (double)(o + k) / 48000.0);
        if (!live && (o / 3840) % 16 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::this_thread::sleep_for(std::chrono::seconds(4));
    std::printf("%s: Single, slots decoded %llu\n", path, (unsigned long long)sp.slotsDecoded());
    for (auto& l : lines) std::printf("  %s\n", l.c_str());
    for (auto& kv : per) std::printf("%s: %d spots\n", kv.first.c_str(), kv.second);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 4 && std::strcmp(argv[1], "realusb") == 0) return runRealUsb(argv[2], std::atof(argv[3]), std::atof(argv[4]), argc > 5 && !std::strcmp(argv[5], "live"));
    if (argc > 4 && std::strcmp(argv[1], "clip") == 0) return writeClip(argv[2], std::atof(argv[3]), std::atof(argv[4]));
    if (argc > 4 && std::strcmp(argv[1], "real") == 0) {
        bool swap = false, live = false;
        for (int i = 5; i < argc; ++i) { if (!std::strcmp(argv[i], "swap")) swap = true; if (!std::strcmp(argv[i], "live")) live = true; }
        return runReal(argv[2], std::atof(argv[3]), std::atof(argv[4]), swap, live);
    }
    const bool cpu = argc > 1 && std::strcmp(argv[1], "cpu") == 0;
    std::printf("── the registry ──\n");
    {
        const auto& ms = digi::modes();
        ok(ms.size() >= 2 && ms[0].name == "FT8" && ms[1].name == "FT4", "FT8 and FT4 are registered, FT8 first");
        ok(digi::modeByName("ft4") == &ms[1] && digi::modeByName("nope") == nullptr, "modeByName is case-insensitive and null for unknown");
        ok(ms[0].slotSec == 15.0 && ms[1].slotSec == 7.5 && ms[0].sampleRate == 12000, "FT8 15 s / FT4 7.5 s slots, 12 kHz audio");
        const digi::ModeDef* w = digi::modeByName("WSPR");
        ok(w && w->slotSec == 120.0 && w->sampleRate == 12000 && w->audioLoHz <= 1380 && w->audioHiHz >= 1620 && w->singleDialTolHz > 0,
           "WSPR is registered (digimodes/wspr.h): 120 s slots, 12 kHz, its 200 Hz sub-band, Single only at its dials");
        bool made = true;
        for (const auto& m : ms) { auto e = m.make(); made = made && e != nullptr; }
        ok(made, "every registered mode makes an engine");
    }

    std::printf("── the band table ──\n");
    {
        bool inside = true; std::string bad;
        for (const auto& d : digi::publishedDials()) {
            const digi::Band* b = digi::bandById(d.band);
            const bool narrow = std::string(d.mode) == "WSPR" || std::string(d.mode) == "FST4W";   // ★ 1400–1600 Hz above the dial
            if (!b || d.hz < b->loHz || d.hz + (narrow ? 1600 : 3000) > b->hiHz + 1) { inside = false; bad += std::string(d.mode) + "@" + d.band + " "; }
        }
        ok(inside, "every published dial (and its passband above it) lies inside its band " + bad);
        const auto& bs = digi::bands();
        ok(bs.size() == 16 && std::string(bs.front().id) == "2200m" && std::string(bs[11].id) == "10m" && std::string(bs[12].id) == "6m"
           && std::string(bs.back().id) == "70cm", "step order 2200 m … 10 m, then 6 m, 4 m, 2 m, 70 cm");
        ok(std::string(digi::bandAt(14074000)->id) == "20m" && digi::bandAt(14500000) == nullptr, "bandAt names the band a frequency is in");
        // ★ The spans with today's registry: FT8 + FT4 + WSPR.
        const auto& ms = digi::modes();
        const digi::Span s20 = digi::spanOf(*digi::bandById("20m"), ms);
        ok(s20.ok && near(s20.loHz, 14074000 + 100 - 250, 60) && near(s20.hiHz, 14095600 + 1650 + 250, 60) && s20.modes.size() == 3,
           "20 m with FT8+FT4+WSPR: " + std::to_string(s20.loHz) + "–" + std::to_string(s20.hiHz) + " (" + std::to_string((int)s20.widthHz()) + " Hz)");
        const digi::Span s2200 = digi::spanOf(*digi::bandById("2200m"), ms);
        ok(s2200.ok && s2200.modes.size() == 1 && s2200.modes[0] == "WSPR", "2200 m: WSPR alone now it is registered");
        // ★ The spans the brief worked out, with the modes being built now registered (dummies standing in).
        std::vector<digi::ModeDef> all = ms;
        // (a mode already registered is not doubled — WSPR is real now)
        for (auto d : { dummyMode("WSPR", 1400, 1600), dummyMode("JS8", 100, 3500), dummyMode("FT2", 100, 3500) })
            if (!digi::modeByName(d.name)) all.push_back(d);
        std::printf("    passband per band, FT8+FT4+FT2+JS8+WSPR registered:\n");
        std::map<std::string, double> w;
        for (const auto& b : digi::bands()) {
            const digi::Span s = digi::spanOf(b, all);
            if (!s.ok) continue;
            w[b.id] = s.widthHz();
            std::string md; for (auto& m : s.modes) md += m + " ";
            if (!s.outside.empty()) { md += "(outside: "; for (auto& m : s.outside) md += m + " "; md += ")"; }
            std::printf("      %-6s %10.4f–%10.4f MHz  %5.1f kHz  centre %.4f  %s\n", b.id, s.loHz / 1e6, s.hiHz / 1e6, s.widthHz() / 1e3, s.centreHz / 1e6, md.c_str());
        }
        ok(near(w["20m"], 23700, 400), "20 m spans FT8 14.074 → WSPR 14.0956+ (≈ 23 kHz): " + std::to_string((int)w["20m"]));
        ok(near(w["40m"], 42500, 1000), "40 m spans WSPR 7.0386 → JS8 7.078 (≈ 43 kHz): " + std::to_string((int)w["40m"]));
        ok(near(w["80m"], 12500, 1000), "80 m ≈ 13 kHz: " + std::to_string((int)w["80m"]));
        ok(w["30m"] > 10000 && w["30m"] < 19000, "30 m ≈ 11–18 kHz (JS8 10.130 → FT2 10.144): " + std::to_string((int)w["30m"]));
        {
            const digi::Span s15 = digi::spanOf(*digi::bandById("15m"), all), s2 = digi::spanOf(*digi::bandById("2m"), all);
            ok(std::find(s15.outside.begin(), s15.outside.end(), "FT4") != s15.outside.end() && s15.widthHz() <= digi::kMaxSpanHz,
               "15 m: FT4 (21.140) is 66 kHz from FT8 — outside the cluster, named, never a 74 kHz slice");
            ok(std::find(s2.outside.begin(), s2.outside.end(), "WSPR") != s2.outside.end()
               && std::find(s2.modes.begin(), s2.modes.end(), "FT4") != s2.modes.end(), "2 m: FT8 + FT4 together, WSPR (144.4905) outside");
        }
        ok(w.count("2200m") && w["2200m"] < 1000, "2200 m: WSPR alone, a narrow passband");
        double widest = 0; for (auto& kv : w) widest = std::max(widest, kv.second);
        ok(widest <= 48000, "no band's passband is wider than 48 kHz (" + std::to_string((int)widest) + ")");
    }

    std::printf("── band stepping ──\n");
    {
        const auto& ms = digi::modes();
        auto everywhere = [](double, double) { return true; };
        const auto spans = digi::reachableSpans(ms, everywhere);
        std::string ids; for (auto& s : spans) ids += std::string(s.band->id) + " ";
        ok(ids == "2200m 630m 160m 80m 60m 40m 30m 20m 17m 15m 12m 10m 6m 4m 2m 70cm ", "FT8/FT4/WSPR bands in step order: " + ids);
        auto at = [&](int i) { return i < 0 ? std::string("-") : std::string(spans[(size_t)i].band->id); };
        ok(at(digi::stepSpan(spans, 14074000, +1)) == "17m" && at(digi::stepSpan(spans, 14074000, -1)) == "30m", "20 m: up → 17 m, down → 30 m");
        ok(at(digi::stepSpan(spans, 28074000, +1)) == "6m", "10 m → 6 m (HF, then VHF)");
        ok(at(digi::stepSpan(spans, 432174000, +1)) == "70cm" && at(digi::stepSpan(spans, 136000, -1)) == "2200m", "clamped at both ends, never wrapped");
        ok(at(digi::stepSpan(spans, 1840000, -1)) == "630m", "160 m down → 630 m (WSPR's LF bands)");
        ok(at(digi::stepSpan(spans, 12000000, +1)) == "20m" && at(digi::stepSpan(spans, 12000000, -1)) == "30m", "between bands: the nearest one that way");
        ok(at(digi::stepSpan(spans, 0, +1)) == "2200m" && at(digi::stepSpan(spans, 9e9, -1)) == "70cm", "from nowhere: the first / the last");
        auto hfPlus = [](double lo, double hi) { return lo >= 0 && hi <= 260e6; };
        const auto hp = digi::reachableSpans(ms, hfPlus);
        ok(!hp.empty() && std::string(hp.back().band->id) == "2m", "an HF+ (≤ 260 MHz) reaches up to 2 m, never 70 cm");
        auto noHf = [](double lo, double) { return lo >= 24e6; };
        const auto rtl = digi::reachableSpans(ms, noHf);
        ok(!rtl.empty() && std::string(rtl.front().band->id) == "12m", "an RTL without direct sampling (≥ 24 MHz) starts at 12 m");
        auto none = [](double, double) { return false; };
        ok(digi::reachableSpans(ms, none).empty() && digi::stepSpan({}, 1, 1) == -1, "a radio that reaches no band: no spans (no Band button)");
    }

    std::printf("── the passband per band (DecoderHost::bandWidthFor) ──\n");
    {
        for (const char* id : { "160m", "80m", "40m", "20m", "10m", "6m", "2m" }) {
            const digi::Span s = digi::spanOf(*digi::bandById(id), digi::modes());
            const double w = DecoderHost::bandWidthFor(s.centreHz);
            ok(near(w, std::max(4000.0, s.widthHz()), 1.0), std::string(id) + ": the slice asked for at the band's centre is its span, " + std::to_string((int)w) + " Hz");
        }
        const digi::Span s = digi::spanOf(*digi::bandById("20m"), digi::modes());
        ok(DecoderHost::bandWidthFor(s.centreHz + 2000) > s.widthHz() + 3900, "a dial typed 2 kHz off-centre asks for a slice wide enough to hold the whole span");
        ok(DecoderHost::bandWidthFor(12.5e6) == 24000.0, "off any band: 24 kHz");
    }

    std::printf("── the tuner ──\n");
    {
        // A tone 1000 Hz above a dial 7 kHz below the slice centre, its mirror 1000 Hz BELOW the dial, and one 5 kHz up.
        const double rate = 75000, centre = 14078800, dial = 14074000;
        auto tone = [&](double rfHz) {
            std::vector<cf32> x((size_t)(rate * 4));
            for (size_t i = 0; i < x.size(); ++i) { const double p = 2 * M_PI * (rfHz - centre) * i / rate; x[i] = cf32((float)std::cos(p), (float)std::sin(p)); }
            digi::DigiTuner t(dial, 100, 3500, 12000);
            std::vector<float> out;
            for (size_t o = 0; o < x.size(); o += 1500) t.feed(&x[o], (int)std::min<size_t>(1500, x.size() - o), rate, centre, out);
            return std::make_pair(out, t.gain());
        };
        auto powAt = [](const std::vector<float>& a, double hz) {   // single-bin DFT over the last 2 s
            double re = 0, im = 0; const size_t n0 = a.size() - 24000;
            for (size_t i = n0; i < a.size(); ++i) { const double p = 2 * M_PI * hz * (double)i / 12000.0; re += a[i] * std::cos(p); im += a[i] * std::sin(p); }
            return (re * re + im * im) / (24000.0 * 24000.0);
        };
        auto [in, g1] = tone(dial + 1000);
        const double pIn = powAt(in, 1000);
        // ★ The level control applies its gain to every input alike only once settled — compare raw powers ÷ gain².
        auto [img, g2] = tone(dial - 1000);
        auto [up, g3] = tone(dial + 5000);
        const double pImg = powAt(img, 1000) / ((double)g2 * g2) * ((double)g1 * g1);
        const double pUp = (powAt(up, 1000) + powAt(up, 5000) + powAt(up, 7000)) / ((double)g3 * g3) * ((double)g1 * g1);
        ok(in.size() >= 47900 && in.size() <= 48100, "4 s of a 75 kHz slice → 4 s of 12 kHz audio (" + std::to_string(in.size()) + ")");
        ok(pIn > 1e-4, "a tone 1000 Hz above the dial is heard at 1000 Hz");
        ok(10 * std::log10(pIn / std::max(pImg, 1e-30)) > 50, "the lower sideband (1000 Hz BELOW the dial) is rejected > 50 dB: "
           + std::to_string((int)(10 * std::log10(pIn / std::max(pImg, 1e-30)))) + " dB");
        ok(10 * std::log10(pIn / std::max(pUp, 1e-30)) > 50, "a tone 5 kHz above the dial (outside 100–3500) is rejected > 50 dB");
        ok(digi::DigiTuner::fits(dial, 100, 3500, rate, centre) && !digi::DigiTuner::fits(dial - 40000, 100, 3500, rate, centre),
           "fits(): a dial inside the slice fits, one 40 kHz away does not");
        digi::DigiTuner t(dial, 100, 3500); std::vector<float> o; std::vector<cf32> blk(1000, cf32(0, 0));
        t.feed(blk.data(), 1000, rate, centre, o);
        std::printf("    taps: stage 1 %d at 75 kHz, stage 2 %d at 12 kHz\n", t.stage1Taps(), t.stage2Taps());
    }

    std::printf("── a whole band, end to end (DecoderHost, Band) ──\n");
    const digi::Span s20 = digi::spanOf(*digi::bandById("20m"), digi::modes());
    const double rate = 2400000.0 * 512 / 16384;   // ★ 75 kHz — what a 2.4 MS/s radio's slice is for 20 m's width
    {
        DecoderHost::Env env;
        double dialNow = s20.centreHz;
        env.dialHz = [&] { return dialNow; };
        DecoderHost h(env, "band-test");
        auto peer = std::make_shared<Peer>();
        h.addPeer(peer);
        ok(h.startSpots(DecoderHost::SpotScope::Band) == DecoderHost::Start::Ok && h.spotsScope() == "band", "Band spots start");
        ok(h.wantsIq() && !h.wantsAudio(), "Band mode wants the IQ slice, not the listener's audio");
        ok(near(h.iqWidthHz(), s20.widthHz(), 1), "…and asks for 20 m's span: " + std::to_string((int)h.iqWidthHz()) + " Hz");
        ok(DecoderHost::skimmersRunning().load() == 1, "the shim's one atomic gate counts it (no slice cut on a radio with none)");

        // ★ Two slots of FT8 (15 s) carry four FT8 stations and, in the same 30 s, four FT4 slots carry FT4 stations.
        const double secs = 31.0, slot0 = std::floor(vibeUtcNow() / 30.0) * 30.0 + 30.0;   // ★ a whole-minute-half boundary ahead
        std::vector<cf32> x((size_t)(secs * rate), cf32(0, 0));
        struct Tx { const char* msg; bool ft4; double rf; double start; };
        const double ft8Dial = 14074000, ft4Dial = 14080000;
        const Tx txs[] = {
            { "CQ G4ABC IO92", false, ft8Dial + 600,  0.5 },
            { "CQ DL1XYZ JO31", false, ft8Dial + 1450, 0.5 },
            { "CQ K1ABC FN42", false, ft8Dial + 2300, 15.5 },
            { "CQ VK2DEF QF56", false, ft8Dial + 3000, 15.5 },
            { "CQ JA1QRS PM95", true, ft4Dial + 800,  0.5 },
            { "CQ EA4TUV IN80", true, ft4Dial + 1700, 8.0 },
            { "CQ OH2WXY KP20", true, ft4Dial + 2500, 15.5 },
            // ★ The LOWER SIDEBAND of the FT8 dial: 1200 Hz below it. A USB receiver must never decode this at +1200.
            { "CQ ZL3LSB RE66", false, ft8Dial - 1200, 0.5 },
        };
        for (const Tx& t : txs) addTx(x, rate, t.rf - s20.centreHz, t.start, tonesFor(t.msg, t.ft4), t.ft4, 0.02);
        addNoise(x, 0.02, 7);
        const int blk = 4096;
        const double c0 = cpuSecs();
        // ★ The host's own feedIq stamps vibeUtcNow(); to run faster than real time the Spotter is driven directly with
        //   the capture times, exactly as the host's decode thread does (spotter_->feedIq(…, b.utc)).
        std::vector<std::string> got;
        std::mutex gm;
        digi::Spotter sp(digi::Spotter::Scope::Band, digi::modes(),
            [&](const digi::Spot& s, double rf) {
                char b[160]; std::snprintf(b, sizeof b, "%s %s %.0f %s", s.mode.c_str(), s.call.c_str(), rf, s.grid.c_str());
                std::lock_guard<std::mutex> lk(gm); got.push_back(b);
            }, [&] { return dialNow; }, nullptr);
        for (size_t o = 0; o < x.size(); o += blk) {
            const int n = (int)std::min<size_t>(blk, x.size() - o);
            sp.feedIq(&x[o], n, rate, s20.centreHz, slot0 + (double)(o + n) / rate);
            if ((o / blk) % 8 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));   // ★ let the slot workers run
        }
        const double t0 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        while (sp.slotsDecoded() < 6 && std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - t0 < 20)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        (void)c0;
        std::printf("    engines %zu (%s), slots decoded %llu, skipped %llu\n", sp.engines(), sp.describe().c_str(),
                    (unsigned long long)sp.slotsDecoded(), (unsigned long long)sp.slotsSkipped());
        auto has = [&](const std::string& mode, const std::string& call, double rf) {
            std::lock_guard<std::mutex> lk(gm);
            for (auto& g : got) {
                char m[16], c[16]; double f = 0;
                if (std::sscanf(g.c_str(), "%15s %15s %lf", m, c, &f) == 3 && mode == m && call == c && near(f, rf, 8)) return true;
            }
            return false;
        };
        for (const Tx& t : txs) {
            if (std::string(t.msg).find("LSB") != std::string::npos) continue;
            char call[16]; std::sscanf(t.msg, "CQ %15s", call);
            ok(has(t.ft4 ? "FT4" : "FT8", call, t.rf), std::string(t.ft4 ? "FT4 " : "FT8 ") + call + " spotted with its mode, at its RF");
        }
        bool lsb = false;
        { std::lock_guard<std::mutex> lk(gm); for (auto& g : got) if (g.find("ZL3LSB") != std::string::npos) lsb = true; }
        ok(!lsb, "a station 1200 Hz BELOW the FT8 dial (the lower sideband) is not decoded");
        { std::lock_guard<std::mutex> lk(gm); std::printf("    spots: %zu\n", got.size()); for (auto& g : got) std::printf("      %s\n", g.c_str()); }

        // ★ The host's spot frame (emitSpot: mode, call, RF) is checked through its Single path below.
        h.stopSpots();
        ok(!h.wantsIq() && DecoderHost::skimmersRunning().load() == 0, "stopping Band spots releases the IQ slice and the gate");
    }

    /* ★★ WSPR THROUGH BAND MODE (2026-10-27): one two-minute slot of 20 m in a 75 kHz slice — a WSPR station on the WSPR
     *  dial's sub-band beside FT8 traffic — through the Spotter and the real WSPR engine (digimodes/wspr.h): spotted as
     *  "WSPR" at its RF, with an SNR on the WSPRnet scale (white noise: the definition) within 2 dB. Generated a block at a
     *  time (two minutes at 75 kHz would be 74 MB). */
    std::printf("── WSPR in Band mode (one two-minute slot of 20 m) ──\n");
    {
        const double secs = 124.0, slot = std::ceil(vibeUtcNow() / 120.0) * 120.0 + 120.0, t0 = slot - 3.0;
        const double wDial = 14095600, wRf = wDial + 1523.0, ft8Rf = 14074000 + 1200;
        const double sigma = 0.02, snr = -20.0;
        // complex noise σ per rail → N0 = 2σ²/rate; a complex tone of amplitude A has power A²
        const double A = std::sqrt(std::pow(10.0, snr / 10.0) * 2 * sigma * sigma / rate * 2500.0);
        uint8_t tones[wspr::kSymbols];
        wspr::Message m; m.type = 1; m.call = "G4ABC"; m.grid = "IO92"; m.dbm = 37;
        wspr::encode(m, tones);
        const std::vector<uint8_t> ft8 = tonesFor("CQ M0FTE IO91", false);
        ok(ft8.size() == FT8_NN, "the FT8 neighbour encodes");
        std::mt19937 g(4242); std::normal_distribution<float> nd(0.0f, (float)sigma);
        std::vector<std::string> got; std::mutex gm;
        digi::Spotter sp(digi::Spotter::Scope::Band, digi::modes(),
            [&](const digi::Spot& s, double rf) {
                char b[200]; std::snprintf(b, sizeof b, "%s %s %.1f %s %d", s.mode.c_str(), s.call.c_str(), rf, s.grid.c_str(), s.snr);
                std::lock_guard<std::mutex> lk(gm); got.push_back(b);
            }, nullptr, nullptr);
        const double wStart = (slot + wspr::kStartSec - t0) * rate, sps = rate * 8192.0 / 12000.0;
        double wph = 0, fph = 0;
        const int blk = 4096;
        std::vector<cf32> x((size_t)blk);
        for (size_t o = 0; o < (size_t)(secs * rate); o += blk) {
            for (int i = 0; i < blk; ++i) {
                const double n = (double)(o + (size_t)i);
                cf32 v(nd(g), nd(g));
                const double k = (n - wStart) / sps;
                if (k >= 0 && k < wspr::kSymbols) {
                    const double f = wRf - 1.5 * wspr::kBaud + tones[(int)k] * wspr::kBaud - s20.centreHz;
                    wph += 2 * M_PI * f / rate;
                    v += cf32((float)(A * std::cos(wph)), (float)(A * std::sin(wph)));
                }
                const double ft = std::fmod(t0 + n / rate - 0.5, 15.0);   // FT8: every 15 s slot, from +0.5 s
                if (ft < 79 * FT8_SYMBOL_PERIOD) {
                    const int s = (int)(ft / FT8_SYMBOL_PERIOD);
                    fph += 2 * M_PI * (ft8Rf + ft8[(size_t)s] / FT8_SYMBOL_PERIOD - s20.centreHz) / rate;
                    v += cf32((float)(0.02 * std::cos(fph)), (float)(0.02 * std::sin(fph)));
                }
                x[(size_t)i] = v;
            }
            sp.feedIq(x.data(), blk, rate, s20.centreHz, t0 + (double)(o + blk) / rate);
            if ((o / blk) % 8 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const double w0 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        auto wsprSpot = [&] { std::lock_guard<std::mutex> lk(gm); for (auto& s : got) if (s.rfind("WSPR ", 0) == 0) return s; return std::string(); };
        while (wsprSpot().empty() && std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - w0 < 30)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::string ws = wsprSpot();
        char md[16] = {}, call[16] = {}, grid[16] = {}; double rf = 0; int sn = -99;
        std::sscanf(ws.c_str(), "%15s %15s %lf %15s %d", md, call, &rf, grid, &sn);
        std::printf("    engines %zu (%s); slots %llu; %s\n", sp.engines(), sp.describe().c_str(), (unsigned long long)sp.slotsDecoded(), ws.empty() ? "no WSPR spot" : ws.c_str());
        ok(std::string(call) == "G4ABC" && std::string(grid) == "IO92" && near(rf, wRf, 1.0),
           "WSPR G4ABC IO92 spotted as \"WSPR\" at its RF (dial 14.0956 + 1523 Hz): " + ws);
        ok(std::abs(sn - snr) <= 2, "…its SNR (WSPRnet scale) within 2 dB of the " + std::to_string((int)snr) + " dB it was sent at: " + std::to_string(sn));
        bool ft8ok = false; { std::lock_guard<std::mutex> lk(gm); for (auto& s : got) if (s.rfind("FT8 M0FTE", 0) == 0) ft8ok = true; }
        ok(ft8ok, "…while FT8 on its own dial in the same slice still decodes");
        ok(sp.slotsSkipped() == 0, "no slot skipped");
    }

    /* ★ WSPR IN SINGLE MODE: fed only while the listener sits on a WSPR dial — on the FT8 dial its 1400–1600 Hz is FT8
     *  traffic, and the engine is not even fed. The same 48 kHz audio both ways. */
    std::printf("── WSPR in Single mode: only at a WSPR dial ──\n");
    {
        const double slot = std::ceil(vibeUtcNow() / 120.0) * 120.0 + 120.0;
        std::vector<float> a((size_t)(48000 * 124), 0.f);
        wsprsynth::addNoise(a, 77, 0.05);
        wsprsynth::Sig s; s.msg = wsprsynth::msg1("DL1XYZ", "JO62", 30); s.snr = -22; s.centreHz = 1477;
        // wspr_synth's buffer starts kLeadSec before the minute; ours 3 s before: place it 1 s in
        std::vector<float> sig((size_t)(48000 * 122), 0.f);
        wsprsynth::addSignal(sig, 48000, s, 0.05, 5);
        for (size_t i = 0; i < sig.size(); ++i) a[i + 48000] += sig[i];
        auto run = [&](double dial) {
            std::vector<std::string> got; std::mutex gm;
            digi::Spotter sp(digi::Spotter::Scope::Single, digi::modes(),
                [&](const digi::Spot& o, double rf) { char b[120]; std::snprintf(b, sizeof b, "%s %s %.0f", o.mode.c_str(), o.call.c_str(), rf);
                    std::lock_guard<std::mutex> lk(gm); got.push_back(b); }, [dial] { return dial; }, nullptr);
            for (size_t o = 0; o < a.size(); o += 4800) {
                sp.feedAudio48k(&a[o], (int)std::min<size_t>(4800, a.size() - o), slot - 3.0 + (double)(o + 4800) / 48000);
                if ((o / 4800) % 16 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const double w0 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            for (;;) {
                { std::lock_guard<std::mutex> lk(gm); for (auto& g : got) if (g.rfind("WSPR", 0) == 0) return g; }
                if (std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - w0 > (dial == 14095600 ? 30 : 4)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return std::string();
        };
        const std::string at = run(14095600), off = run(14074000);
        ok(at.rfind("WSPR DL1XYZ 14097077", 0) == 0, "on the 20 m WSPR dial: WSPR DL1XYZ at dial + 1477 Hz: " + at);
        ok(off.empty(), "on the FT8 dial: no WSPR decode (the engine is not fed there)");
    }

    std::printf("── pure noise: no spot ──\n");
    {
        std::vector<cf32> x((size_t)(31.0 * rate), cf32(0, 0));
        addNoise(x, 0.02, 99);
        int n = 0;
        std::mutex gm;
        digi::Spotter sp(digi::Spotter::Scope::Band, digi::modes(), [&](const digi::Spot&, double) { std::lock_guard<std::mutex> lk(gm); ++n; },
                         nullptr, nullptr);
        const double slot0 = std::floor(vibeUtcNow() / 30.0) * 30.0 + 30.0;
        for (size_t o = 0; o < x.size(); o += 4096) {
            const int k = (int)std::min<size_t>(4096, x.size() - o);
            sp.feedIq(&x[o], k, rate, s20.centreHz, slot0 + (double)(o + k) / rate);
            if ((o / 4096) % 8 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const double t0 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        while (sp.slotsDecoded() < 6 && std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - t0 < 20)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ok(n == 0 && sp.slotsDecoded() >= 6, "31 s of noise on 20 m: " + std::to_string(sp.slotsDecoded()) + " slots decoded, " + std::to_string(n) + " spots");
    }

    std::printf("── the host: Single still FT8+FT4 on the listener's audio, and the spot frame names its mode ──\n");
    {
        DecoderHost::Env env;
        env.dialHz = [] { return 14074000.0; };
        DecoderHost h(env, "single-test");
        auto peer = std::make_shared<Peer>();
        h.addPeer(peer);
        ok(h.startSpots() == DecoderHost::Start::Ok && h.spotsScope() == "single" && h.wantsAudio() && !h.wantsIq(), "Single spots: the listener's audio, no slice");
        ok(h.startSpots(DecoderHost::SpotScope::Band) == DecoderHost::Start::Ok && h.spotsScope() == "band" && h.slotsHeld() == 1,
           "switching to Band replaces the run and keeps the ONE slot");
        ok(h.startSpots(DecoderHost::SpotScope::Band) == DecoderHost::Start::Joined, "asking for Band again joins it");
        h.stopSpots();
        ok(h.slotsHeld() == 0, "stopped: the slot is free");
        // ★ A FT8 band at 48 kHz on the listener's audio, captured in real time terms (stamped feed).
        h.startSpots();
        const double slot0 = std::floor(vibeUtcNow() / 15.0) * 15.0 + 15.0;
        std::vector<float> a((size_t)(48000 * 15.5), 0.0f);
        std::mt19937 g(3); std::normal_distribution<float> nz(0.0f, 0.02f);
        auto put = [&](const char* msg, double f0) {
            auto t = tonesFor(msg, false); double ph = 0;
            for (size_t k = 0; k < t.size(); ++k) for (int j = 0; j < 7680; ++j) {
                const size_t i = 24000 + k * 7680 + (size_t)j; if (i >= a.size()) return;
                ph += 2 * M_PI * (f0 + t[k] * 6.25) / 48000.0; a[i] += 0.1f * (float)std::sin(ph);
            }
        };
        put("CQ M0SGL IO91", 1200);
        for (auto& v : a) v += nz(g);
        for (size_t o = 0; o < a.size(); o += 960) {
            const int k = (int)std::min<size_t>(960, a.size() - o);
            h.feed(&a[o], k, 1, slot0 + (double)(o + k) / 48000.0);
            while (h.queued() > 96000) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::string spot;
        for (int i = 0; i < 200 && spot.empty(); ++i) { auto v = peer->spots(); if (!v.empty()) spot = v[0]; else std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        ok(field(spot, "mode") == "FT8" && field(spot, "callsign") == "M0SGL" && field(spot, "band") == "20m"
           && near(std::atof(field(spot, "frequency").c_str()), 14075200, 8), "Single: the spot frame names FT8, the call, 20m, dial + audio: " + spot);
        h.stopSpots();
    }

    if (cpu) {
        std::printf("── CPU: Band vs Single on the same busy 20 m (this machine; process CPU, every thread) ──\n");
        // ★ The same traffic both ways: 20 FT8 stations in two slots, and 10 FT4 stations. Single hears the FT8 dial's
        //   passband (the FT4 stations are 6 kHz up, out of it); Band hears both dials. Each figure = CPU ÷ 31 s of audio.
        const double secs = 31.0, slot0 = std::floor(vibeUtcNow() / 30.0) * 30.0 + 30.0;
        std::vector<cf32> x((size_t)(secs * rate), cf32(0, 0));
        char msg[32];
        for (int k = 0; k < 20; ++k) { std::snprintf(msg, sizeof msg, "CQ G4A%c%c IO9%d", 'A' + k / 26, 'A' + k % 26, k % 10);
            addTx(x, rate, 14074000 + 300 + 140 * k - s20.centreHz, k % 2 ? 15.5 : 0.5, tonesFor(msg, false), false, 0.01); }
        for (int k = 0; k < 10; ++k) { std::snprintf(msg, sizeof msg, "CQ DL4B%c JO3%d", 'A' + k, k % 10);
            addTx(x, rate, 14080000 + 300 + 280 * k - s20.centreHz, 0.5 + 7.5 * (k % 4), tonesFor(msg, true), true, 0.01); }
        addNoise(x, 0.02, 11);
        auto run = [&](digi::Spotter::Scope scope, int& spots) {
            std::mutex gm; spots = 0;
            digi::Spotter sp(scope, digi::modes(), [&](const digi::Spot&, double) { std::lock_guard<std::mutex> lk(gm); ++spots; },
                             [] { return 14074000.0; }, nullptr);
            // ★ Single's input: the FT8 dial's USB audio at 48 kHz — made by a tuner (not counted) and repeated ×4.
            std::vector<float> a48;
            if (scope == digi::Spotter::Scope::Single) {
                digi::DigiTuner t(14074000, 100, 3500); std::vector<float> a12;
                for (size_t o = 0; o < x.size(); o += 4096) t.feed(&x[o], (int)std::min<size_t>(4096, x.size() - o), rate, s20.centreHz, a12);
                a48.resize(a12.size() * 4); for (size_t i = 0; i < a48.size(); ++i) a48[i] = a12[i / 4];
            }
            const double c0 = cpuSecs();
            if (scope == digi::Spotter::Scope::Single) {
                for (size_t o = 0; o < a48.size(); o += 3840) {
                    const int k = (int)std::min<size_t>(3840, a48.size() - o);
                    sp.feedAudio48k(&a48[o], k, slot0 + (double)(o + k) / 48000.0);
                    if ((o / 3840) % 8 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            } else {
                for (size_t o = 0; o < x.size(); o += 4096) {
                    const int k = (int)std::min<size_t>(4096, x.size() - o);
                    sp.feedIq(&x[o], k, rate, s20.centreHz, slot0 + (double)(o + k) / rate);
                    if ((o / 4096) % 8 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            const double t0 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            while (sp.slotsDecoded() < 6 && std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - t0 < 30)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            return 100.0 * (cpuSecs() - c0) / secs;
        };
        int nS = 0, nB = 0;
        const double single = run(digi::Spotter::Scope::Single, nS);
        const double band = run(digi::Spotter::Scope::Band, nB);
        std::vector<float> out;
        digi::DigiTuner t8(14074000, 100, 3500), t4(14080000, 100, 3500);
        const double c1 = cpuSecs();
        for (size_t o = 0; o < x.size(); o += 4096) {
            const int k = (int)std::min<size_t>(4096, x.size() - o);
            out.clear(); t8.feed(&x[o], k, rate, s20.centreHz, out); t4.feed(&x[o], k, rate, s20.centreHz, out);
        }
        const double tuners = 100.0 * (cpuSecs() - c1) / secs;
        std::printf("    Single (FT8+FT4 engines on the 14.074 passband): %.3f %% of one core, %d spots\n", single, nS);
        std::printf("    Band 20 m (FT8 on 14.074, FT4 on 14.080, from a 75 kHz slice): %.3f %% of one core, %d spots\n", band, nB);
        std::printf("    of which the two tuners: %.3f %% (%.3f %% per dial)\n", tuners, tuners / 2);
    }

    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
