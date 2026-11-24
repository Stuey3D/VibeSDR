// ★★★ THE SATELLITE PASS'S IF FILTER (vibe_sat_if.h, V12 beta92) — pure, no radio, silent.
//   The occupied width per kind of downlink, the Doppler allowance, the rule "never narrower than the signal + its Doppler,
//   measured from the tuner's centre", and the rung each radio gets: the RTL's (350 kHz floor) and the RSP's (200 / 300 /
//   600 kHz, only ever narrower than the rate's own). Stuart's METEOR-M2 4 pass on the Pi 500's V4 (2026-11-23) is the
//   first case: the view-driven filter sat at 1000 kHz when the gain was measured; 350 kHz holds the whole pass.
//   build: c++ -std=c++17 -O2 -I ../android/app/src/main/cpp -o /tmp/tsi test-sat-if.cpp -lpthread
#include "vibe_sat_if.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cmath>
#include <vector>

static int fails = 0, passes = 0;
static void ok(bool cond, const char* what) {
    if (cond) { ++passes; printf("   ok   %s\n", what); }
    else      { ++fails;  printf("   FAIL %s\n", what); }
}
static vibe::sat::Transmitter tx(const char* label, double hz, const char* mode, const char* consumer = "", double baud = 0, double hiHz = 0) {
    vibe::sat::Transmitter t; t.label = label; t.hz = hz; t.mode = mode; t.consumer = consumer; t.baud = baud; t.hiHz = hiHz; return t;
}

int main() {
    using namespace vibesatif;
    const std::vector<int> rtl = rtlRungs(), rsp = rspRungs();
    const double cap = 2400000.0;              // the Pi 500's V4 at 2.4 MS/s
    const double rspRate = 1536000.0;          // the RSP's own filter at 2 MS/s (bandwidthKHzForRate)

    printf("\nThe occupied width, by kind of downlink\n");
    {
        std::string w;
        ok(occupiedHz(tx("LRPT", 137.9e6, "lrpt", "lrpt", 72000), &w) == 150000.0 && w == "LRPT", "METEOR LRPT 72 k: 150 kHz");
        ok(occupiedHz(tx("LRPT 80k", 137.9e6, "lrpt", "lrpt", 80000)) == 160000.0, "LRPT 80 k: 160 kHz (2 x the symbol rate)");
        ok(occupiedHz(tx("Mode B", 435.61e6, "usb", "", 0, 435.67e6), &w) == 63000.0 && w == "transponder", "a 60 kHz linear transponder: its passband + 3 kHz");
        ok(occupiedHz(tx("Beacon", 435.605e6, "cw"), &w) == 24000.0, "a skimmed CW beacon: the skimmer's 24 kHz");
        ok(occupiedHz(tx("FM repeater", 437.8e6, "nfm"), &w) == 20000.0 && w == "FM", "FM voice: 20 kHz");
        ok(occupiedHz(tx("APRS", 145.825e6, "nfm", "ax25-1200"), &w) == 20000.0 && w == "APRS", "APRS 1200: 20 kHz");
        ok(occupiedHz(tx("9k6", 435.3e6, "nfm", "ax25-9600")) == 25000.0, "9600 packet: 25 kHz");
        ok(occupiedHz(tx("Telemetry", 145.935e6, "usb", "fc-bpsk")) == 3000.0, "FUNcube BPSK telemetry: 3 kHz");
    }

    printf("\nThe Doppler allowance\n");
    {
        ok(std::fabs(dopplerHz(137.9e6) - 6223.3) < 1.0, "137.9 MHz: 2.7e-5 x f + the 2.5 kHz trim clamp = 6.2 kHz");
        ok(std::fabs(dopplerHz(437.8e6) - 14320.6) < 1.0, "437.8 MHz: 14.3 kHz");
    }

    printf("\nMETEOR-M2 4, Stuart's pass (11-23): the RTL's 350 kHz, the RSP's 300 kHz\n");
    {
        Region r = regionOf(tx("LRPT", 137.9e6, "lrpt", "lrpt", 72000), 12500.0);
        ok(r.why == "LRPT 150 kHz + Doppler \xc2\xb1" "6.2 kHz", r.why.c_str());
        const double rf = 137.9e6 + 20000.0;   // the tuner just off the dial (the DC kept off the channel)
        const double w = 2.0 * halfNeeded({r}, rf);
        ok(std::fabs(w - 2.0 * (20000.0 + 75000.0 + 6223.3)) < 1.0, "width = 2 x (|signal - tuner| + occupied/2 + Doppler) = 202 kHz");
        ok(narrowestRung(w, rtl, cap) == 350000, "RTL: 350 kHz (was 1000 kHz from the view when the gain was measured)");
        ok(narrowestRung(w, rsp, rspRate) == 300000, "RSP: 300 kHz");
        // ★ NEVER NARROWER THAN THE SIGNAL + DOPPLER: the tuner left 300 kHz away needs a wider rung, not a cut satellite
        const double w2 = 2.0 * halfNeeded({r}, 137.9e6 + 300000.0);
        ok(narrowestRung(w2, rtl, cap) == 1000000, "the tuner 300 kHz off the downlink: 1000 kHz (it holds the edge + Doppler), not 350");
        for (double off : {0.0, 15000.0, 120000.0, 250000.0, 400000.0}) {
            const double ww = 2.0 * halfNeeded({r}, 137.9e6 + off);
            const int g = narrowestRung(ww, rtl, cap);
            if (g) ok(g * 0.5 >= off + r.halfHz, "the rung's half-width covers |offset| + occupied/2 + Doppler");
        }
    }

    printf("\nThe other kinds, around a tuner 15 kHz off the dial\n");
    {
        auto rtlFor = [&](const vibe::sat::Transmitter& t) { const Region r = regionOf(t); return narrowestRung(2.0 * halfNeeded({r}, t.centre() + 15000.0), rtl, cap); };
        auto rspFor = [&](const vibe::sat::Transmitter& t) { const Region r = regionOf(t); return narrowestRung(2.0 * halfNeeded({r}, t.centre() + 15000.0), rsp, rspRate); };
        const auto iss = tx("FM repeater", 437.8e6, "nfm");
        ok(rtlFor(iss) == 350000 && rspFor(iss) == 200000, "ISS FM voice (70 cm): RTL 350 kHz (its narrowest), RSP 200 kHz");
        const auto rs44 = tx("Mode B", 435.61e6, "usb", "", 0, 435.67e6);
        ok(rtlFor(rs44) == 350000 && rspFor(rs44) == 200000, "RS-44's 60 kHz transponder: RTL 350, RSP 200 (2 x (15 + 31.5 + 14.3) = 122 kHz)");
        const auto bpsk = tx("Telemetry", 145.935e6, "usb", "fc-bpsk");
        ok(rtlFor(bpsk) == 350000 && rspFor(bpsk) == 200000, "BPSK telemetry: the narrowest each has");
    }

    printf("\nWider than the radio can usefully narrow: left alone\n");
    {
        ok(narrowestRung(2500000.0, rtl, cap) == 0, "a need past the capture: no rung (the filter is not touched)");
        ok(narrowestRung(1600000.0, rsp, rspRate) == 0, "RSP: nothing narrower than the rate's own 1536 kHz: left alone");
        ok(narrowestRung(400000.0, rsp, rspRate) == 600000, "RSP: 400 kHz needed -> 600 kHz");
    }

    printf("\nTwo picks at once (two listeners, two birds): the filter holds both\n");
    {
        Region a = regionOf(tx("LRPT", 137.9e6, "lrpt", "lrpt", 72000)); a.owner = "s1";
        Region b = regionOf(tx("LRPT", 137.1e6, "lrpt", "lrpt", 72000)); b.owner = "s2";
        const double rf = 137.5e6;
        const double h = halfNeeded({a, b}, rf);
        ok(std::fabs(h - (400000.0 + 75000.0 + dopplerHz(137.9e6))) < 1.0, "the half-width is the farther one's");
        ok(narrowestRung(2.0 * h, rtl, cap) == 1000000, "-> 1000 kHz");
        ok(halfNeeded({}, rf) == 0.0, "no pass: nothing needed");
    }

    {   // ★★ beta93 the opening view (viewSpanHz — Stuart 11-24: ~420 kHz across was "about this level" for a METEOR)
        const double lrpt = viewSpanHz(tx("LRPT", 137.9e6, "lrpt", "lrpt", 72000));
        ok(lrpt > 380000.0 && lrpt < 440000.0, std::string("METEOR LRPT opens about 410 kHz wide (" + std::to_string((int)lrpt) + ")").c_str());
        const double fm = viewSpanHz(tx("FM", 145.8e6, "fm", "nfm", 0));
        ok(fm >= kViewMinSpanHz && fm < 120000.0, std::string("a voice FM downlink opens narrower (" + std::to_string((int)fm) + ")").c_str());
        const double cw = viewSpanHz(tx("Beacon", 435.6e6, "cw", "cw", 0));
        ok(cw >= kViewMinSpanHz, std::string("never under the 50 kHz floor (" + std::to_string((int)cw) + ")").c_str());
    }

    {   // ★★ beta93 a pass never narrower than 500 kHz, and VFO B's downlink widens it (Stuart 11-24)
        const Region m = regionOf(tx("LRPT", 137.9e6, "lrpt", "lrpt", 72000));
        ok(narrowestRung(2.0 * halfForPass({m}, 137.9e6), rtl, cap) == 500000, "METEOR alone: 500 kHz on an RTL (was 350)");
        ok(narrowestRung(2.0 * halfForPass({m}, 137.9e6), rsp, rspRate) == 600000, "METEOR alone: 600 kHz on an RSP");
        ok(halfForPass({}, 137.9e6) == 0.0, "no pass: no floor");
        const Region tr = regionOf(tx("Transponder", 435.64e6, "usb", "", 0, 435.67e6));
        const Region bc = regionOf(tx("Beacon", 435.57e6, "cw", "cw", 0));
        const double two = 2.0 * halfForPass({tr, bc}, 435.64e6);
        ok(two > 2.0 * (70000.0), "a beacon 70 kHz away in VFO B is inside the width");
    }
    {   // ★★ 2026-11-24 ORBCOMM: the pick's VIEW holds the decoder's whole slice (+ margin), the IF its search edges
        vibe::sat::Transmitter band = tx("ORBCOMM", 137.2e6, "orbcomm", "orbcomm", 0, 137.8e6);
        vibe::sat::Transmitter one = tx("ORBCOMM", 137.6625e6, "orbcomm", "orbcomm", 0);
        std::string p; vibe::sat::decoderOf(band, p);
        const double slice = std::atof(p.c_str() + p.find(':') + 1);
        ok(slice == 640000, "a band pick's slice is 640 kHz");
        ok(viewSpanHz(band) >= slice && viewSpanHz(band) <= slice + 100000, "an ORBCOMM band pick's view holds the whole slice (+ a little)");
        // ★★ Stuart 11-24: EVERY ORBCOMM pick decodes the whole band — a one-channel bird too (its listing goes stale)
        std::string p1; vibe::sat::decoderOf(one, p1);
        ok(p1.find("\"width\":640000") != std::string::npos && p1.find("\"centre\":137500000") != std::string::npos, "a one-channel ORBCOMM pick gets the BAND slice, fixed on 137.5 MHz");
        ok(viewSpanHz(one) >= 640000 && viewSpanHz(one) <= 740000, "…and the band's view (the slice + a little)");
        ok(std::fabs(regionOf(one).centreHz - 137.5e6) < 1 && regionOf(one).halfHz >= 320000, "…and the IF holds the band, not the listed channel");
        // 137.800 at +3.5 kHz Doppler with its ±3.4 kHz is 137.807 MHz: inside the slice (centred 137.5) and the IF's region
        const Region rb = regionOf(band);
        ok(rb.centreHz + rb.halfHz > 137.8075e6 && rb.centreHz - rb.halfHz < 137.1925e6, "the IF region holds 137.200 and 137.800 with their width and Doppler");
        ok(137.5e6 + slice / 2 > 137.8075e6 && 137.5e6 - slice / 2 < 137.1925e6, "…and so does the slice");
    }

    printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
