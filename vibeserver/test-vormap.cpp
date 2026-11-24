// test-vormap.cpp — V12 VOR / ILS MAP MODE (vibe_vormap_service.h, docs/v12/VOR-MAP.md). ★ Silent; deterministic; our own
// transmitter (decoders/nav_tx.h WideSynth — every navaid at an absolute frequency in one wide capture).
//
// ★★★ WHAT IS PINNED.
//   • THE PLAN: 200 channels on the 50 kHz grid; an RTL at 2.4 MS/s = 5 windows of 40, an RSP at 8 and 10 MS/s = 2; every
//     channel in exactly one window; every window centred BETWEEN two channels (its DC in no channel); every channel inside
//     90 % of the rate with its ±12.5 kHz; under 700 kS/s not offered (the HF+'s 912/768 kS/s are, as 14/16 windows — 2026-11-24).
//   • ONE WINDOW, EVERY CHANNEL AT ONCE, at an RTL rate (2.4 MS/s) and an RSP rate (8 MS/s): four VORs (two on ADJACENT
//     50 kHz channels) and an ILS localiser, each with its ident — every radial within ±2°, every ident read, the listed one
//     named, the localiser's DDM, NOTHING heard on the window's other 35 / 95 channels.
//   • THE SWEEP visits every window in turn (0 → 4 → 0) at its dwell; a beacon present ONLY in window 3 is found, with its
//     radial and ident, and is marked as heard in window 3.
//   • Fresh / faded: a beacon heard this sweep is fresh; one not heard for a whole sweep fades.
//   • SIMD = scalar: the same window decoded with the scalar twins reads the same radials.
//   • The cost per window (one core's share) is printed (the bench row dec_vormap is the figure the setup page reads).
#include "vibe_vormap_service.h"
#include "decoders/nav_tx.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using namespace vibevormap;
using vibe::nav::tx::WideSynth;

static int g_pass = 0, g_fail = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); c ? ++g_pass : ++g_fail; }
static double angDiff(double a, double b) { return std::fabs(std::fmod(a - b + 540.0, 360.0) - 180.0); }
static double cpuSec() { return (double)std::clock() / CLOCKS_PER_SEC; }

static void testPlan() {
    std::printf("\n── the window plan ──\n");
    struct C { double rate; int wins; };
    for (C c : { C{ 2.4e6, 5 }, C{ 2.048e6, 6 }, C{ 8e6, 2 }, C{ 10e6, 2 }, C{ 3.2e6, 4 }, C{ 1.024e6, 12 }, C{ 20e6, 1 } }) {
        Plan p;
        const bool got = plan(c.rate, p);
        bool every = got, between = got, inside = got;
        std::vector<int> owner(kChannels, 0);
        for (const auto& w : p.w) {
            for (int i = w.first; i <= w.last; ++i) {
                ++owner[(size_t)i];
                if (std::fabs((double)chanHz(i) - w.hwCentre) + kHalfWidthHz > 0.45 * c.rate) inside = false;
            }
            const long long dc = (long long)std::llround(w.hwCentre) % kChanStepHz;
            if (dc != kChanStepHz / 2) between = false;
        }
        for (int v : owner) if (v != 1) every = false;
        char b[200];
        std::snprintf(b, sizeof b, "%.3f MS/s: %d windows (want %d), each channel in one, centres between channels, inside 90 %% of the rate",
                      c.rate / 1e6, (int)p.w.size(), c.wins);
        ok(got && (int)p.w.size() == c.wins && every && between && inside, b);
    }
    Plan p;
    { Plan a, b; const bool okA = plan(912e3, a), okB = plan(768e3, b);
      ok(okA && okB && a.w.size() <= 20 && b.w.size() <= 20 && a.w.size() > 12 && !plan(0, p) && !plan(500e3, p),
         "★★ 2026-11-24 the HF+'s rates are offered, slower: 912 kS/s → " + std::to_string(a.w.size()) + " windows, 768 kS/s → "
         + std::to_string(b.w.size()) + " (≤ 20); 0 / 500 kS/s not"); }
    plan(2.4e6, p);
    ok(p.w[0].first == 0 && p.w[0].last == 39 && std::llround(p.w[0].hwCentre) == 108975000 && std::llround(p.w[4].hwCentre) == 116975000,
       "RTL 2.4 MS/s: 108.00–109.95 MHz centred 108.975, …, 116.00–117.95 centred 116.975");
    ok(chanKind(108000000) == 1 && chanKind(108050000) == 1 && chanKind(108100000) == 2 && chanKind(108150000) == 2 && chanKind(111950000) == 2
       && chanKind(112000000) == 1 && chanKind(112100000) == 1 && chanKind(117950000) == 1, "the channel plan: even tenths VOR, odd tenths LOC below 112, VOR above");
    ok(windowOf(p, 114350000) == 3 && windowOf(p, 108000000) == 0 && windowOf(p, 117950000) == 4, "the window of a channel");
}

/** A beacon in the test's windows. */
struct Want { double hz; bool ils; double brg; const char* ident; double amp; double ddm; const char* station; };

/** Drive the service like the DSP thread: blocks of 20 ms, the hop asked after each, tuned at once. Paced by the queue
 *  (lossless) so every sample is decoded. Returns the CPU seconds the whole process spent. */
static double drive(Service& s, WideSynth& syn, const Plan& p, double seconds, std::vector<int>* visited = nullptr) {
    const double rate = p.rate;
    const size_t blk = (size_t)(rate / 50);
    std::vector<std::complex<float>> buf;
    const double c0 = cpuSec();
    for (double t = 0; t < seconds; t += (double)blk / rate) {
        buf.clear();
        syn.block(s.windowCentre(), blk, buf);
        s.feed(buf.data(), buf.size());
        const int w = s.hopDue();
        if (w >= 0) { s.tunedTo(w); if (visited) visited->push_back(w); }
    }
    s.drain();
    return cpuSec() - c0;
}

static void testWindow(double rate, const char* who, bool scalar = false, double* radOut = nullptr) {
    std::printf("\n── one window, every channel at once — %s (%.1f MS/s%s) ──\n", who, rate / 1e6, scalar ? ", scalar" : "");
    Plan p; plan(rate, p);
    // ★ the window holding 110.00–111.95 at 2.4 MS/s (window 1); at 8 MS/s window 0 holds it all
    const int wi = windowOf(p, 110400000);
    const Want want[] = {
        { 110400000, false,  54.0, "PTH", 1.0, 0, "PTH" },   // listed (Perth): named from two readings
        { 110450000, false, 137.5, "TSA", 0.7, 0, nullptr }, // ★ the ADJACENT channel, 50 kHz up: its own radial
        { 111200000, false, 250.0, "TSB", 1.0, 0, nullptr },
        { 111850000, false, 311.0, "TSC", 0.8, 0, nullptr }, // near the window's top edge at 2.4 MS/s
        { 110900000, true,    0.0, "ILF", 0.9, 0.08, nullptr },   // an ILS localiser: 90 Hz 24 %, 150 Hz 16 % → DDM +0.08
    };
    WideSynth syn(rate, 34.0, 3);
    for (const auto& w : want) {
        WideSynth::Src s; s.hz = w.hz; s.ils = w.ils; s.amp = w.amp; s.bearing = w.brg; s.ident = w.ident; s.wpm = 8; s.identEvery = 6;
        s.identStart = 0.4 + 0.9 * (double)(&w - want);   // ★ staggered, as real stations are (an unlisted ident needs three readings)
        if (w.ils) { s.m90 = 0.20 + w.ddm / 2; s.m150 = 0.20 - w.ddm / 2; s.identDepth = 0.10; s.identEvery = 7; s.identStart = 1.0; }   // ★ a localiser's ident at Annex 10's 10 %
        syn.add(s);
    }
    Service s;
    Service::Opts o; o.lossless = true; o.forceScalar = scalar;
    // ★ Perth area: PTH is the nearest listed station on 110.40
    s.start(p, true, 56.40, -3.43, o);
    s.tunedTo(wi);
    const double cpuAll = drive(s, syn, p, kSettleSec + kDwellSec - 0.5);
    // ★ the transmitter's own share, measured apart (the same sources, the same samples, fed nowhere)
    double cpuSyn = 0;
    {
        WideSynth again(rate, 34.0, 3);
        for (const auto& w : want) { WideSynth::Src x; x.hz = w.hz; x.ils = w.ils; x.amp = w.amp; x.bearing = w.brg; x.ident = w.ident; again.add(x); }
        std::vector<std::complex<float>> buf;
        const size_t blk = (size_t)(rate / 50);
        const double c0 = cpuSec();
        for (double t = 0; t < kSettleSec + kDwellSec - 0.5; t += (double)blk / rate) { buf.clear(); again.block(p.w[(size_t)wi].hwCentre, blk, buf); }
        cpuSyn = cpuSec() - c0;
    }
    const double cpu = std::max(0.0, cpuAll - cpuSyn);
    s.buildReportNow();
    ok(s.window() == wi, std::string("the window held: ") + std::to_string(s.window()));
    for (const auto& w : want) {
        const Service::Seen g = s.seenForTest((int64_t)w.hz);
        char b[300];
        if (!w.ils) {
            std::snprintf(b, sizeof b, "VOR %.2f MHz %s: radial %.1f° (want %.1f, ±2°), ident \"%s\" (reading \"%s\")%s%s",
                          w.hz / 1e6, w.ident, g.radial, w.brg, g.ident.c_str(), g.reading.c_str(), w.station ? ", station " : "", w.station ? g.station.c_str() : "");
            ok(g.heard && g.held == 1 && std::isfinite(g.radial) && angDiff(g.radial, w.brg) <= 2.0 && g.ident == w.ident
               && (!w.station || g.station == w.station) && (w.station || g.station.empty()), b);
            if (radOut && w.hz == 110400000) *radOut = g.radial;
        } else {
            std::snprintf(b, sizeof b, "ILS localiser %.2f MHz %s: DDM %+.3f (want %+.3f ±0.02), ident \"%s\" (reading \"%s\")", w.hz / 1e6, w.ident, g.ddm, w.ddm, g.ident.c_str(), g.reading.c_str());
            ok(g.heard && g.held == 2 && std::fabs(g.ddm - w.ddm) <= 0.02 && g.ident == w.ident, b);
        }
    }
    // ★ nothing on the other channels of the window
    int falses = 0;
    const Window& W = p.w[(size_t)wi];
    for (int i = W.first; i <= W.last; ++i) {
        bool ours = false;
        for (const auto& w : want) if ((int64_t)w.hz == chanHz(i)) ours = true;
        if (!ours && s.seenForTest(chanHz(i)).heard) { ++falses; std::printf("      false: %.2f MHz\n", chanHz(i) / 1e6); }
    }
    ok(falses == 0, "nothing heard on the window's " + std::to_string(W.last - W.first + 1 - 5) + " empty channels");
    const std::string r = s.report();
    ok(r.find("{\"type\":\"vormap\"") == 0 && r.find("\"ident\":\"PTH\"") != std::string::npos && r.find("\"station\":{\"ident\":\"PTH\"") != std::string::npos
       && r.find("\"trueBrg\"") != std::string::npos && r.find("\"kind\":\"loc\"") != std::string::npos && r.find("\"heard\":5") != std::string::npos,
       "the report: PTH named with its true bearing, the localiser, heard 5");
    if (std::getenv("VORMAP_DEBUG")) std::printf("%s\n", r.c_str());
    ok(s.droppedBlocks() == 0, "nothing dropped (lossless)");
    std::printf("  cost (the transmitter's share taken out): %.1f %% of one core over %.1f s of a %d-channel window (channel rate %.0f Hz)\n",
                100.0 * cpu / (kSettleSec + kDwellSec - 0.5), kSettleSec + kDwellSec - 0.5, W.last - W.first + 1, s.chanRate());
    s.stop();
}

static void testSweep() {
    std::printf("\n── the sweep: every window in turn; a beacon only in window 3 ──\n");
    const double rate = 2.4e6;
    Plan p; plan(rate, p);
    WideSynth syn(rate, 34.0, 5);
    WideSynth::Src cpt; cpt.hz = 114350000; cpt.bearing = 290.0; cpt.ident = "CPT"; cpt.wpm = 10; cpt.identEvery = 5; cpt.identStart = 2.5;
    syn.add(cpt);
    Service s;
    Service::Opts o; o.lossless = true; o.dwellSec = 18.0;   // ★ a short dwell for the test; the ident every 5 s
    s.start(p, true, 51.75, -1.25, o);   // Oxford: CPT (Compton) the nearest 114.35
    s.tunedTo(0);
    std::vector<int> visited;
    drive(s, syn, p, 5 * (o.dwellSec + kSettleSec) + 3.0, &visited);
    std::string seq;
    for (int w : visited) seq += std::to_string(w) + " ";
    ok(visited.size() >= 5 && visited[0] == 1 && visited[1] == 2 && visited[2] == 3 && visited[3] == 4 && visited[4] == 0,
       "the windows in turn: 0 → " + seq + "(and round again)");
    ok(s.sweep() >= 1, "a sweep counted on coming back to window 0: " + std::to_string(s.sweep()));
    const Service::Seen g = s.seenForTest(114350000);
    char b[240];
    std::snprintf(b, sizeof b, "CPT 114.35 (window 3 only) found: radial %.1f° (want 290 ±2), ident \"%s\", station %s",
                  g.radial, g.ident.c_str(), g.station.c_str());
    ok(g.heard && angDiff(g.radial, 290.0) <= 2.0 && g.ident == "CPT" && g.station == "CPT", b);
    s.buildReportNow();
    const std::string r = s.report();
    ok(r.find("\"hz\":114350000") != std::string::npos && r.find("\"win\":3") != std::string::npos && r.find("\"heard\":1,") != std::string::npos,
       "the report: one beacon heard, in window 3");
    ok(r.find("\"fresh\":true") != std::string::npos, "…fresh (heard this sweep)");
    // ★ the beacon goes off the air: after a whole sweep more, it is faded (and still listed)
    WideSynth quiet(rate, 34.0, 9);
    drive(s, quiet, p, 5 * (o.dwellSec + kSettleSec) + o.dwellSec + 2.0);
    s.buildReportNow();
    const std::string r2 = s.report();
    ok(r2.find("\"hz\":114350000") != std::string::npos && r2.find("\"fresh\":false") != std::string::npos, "…not heard for a whole sweep: faded, still listed");
    int falses = 0;
    for (int i = 0; i < kChannels; ++i) if (chanHz(i) != 114350000 && s.seenForTest(chanHz(i)).heard) ++falses;
    ok(falses == 0, "nothing else heard anywhere on the band in two sweeps of noise (" + std::to_string(falses) + ")");
    s.stop();
}

int main() {
    std::printf("test-vormap — the VOR / ILS map mode (SIMD: %s)\n", vibe::nav::simdName());
    testPlan();
    double rSimd = NAN, rScalar = NAN;
    testWindow(2.4e6, "an RTL rate", false, &rSimd);
    testWindow(8.0e6, "an RSP rate");
    testWindow(2.4e6, "an RTL rate", true, &rScalar);
    ok(std::isfinite(rSimd) && std::isfinite(rScalar) && std::fabs(rSimd - rScalar) < 0.05, "SIMD and scalar read the same radial on PTH");
    testSweep();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
