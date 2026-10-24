// test-adsb-gain.cpp — the ADS-B gain rule (vibe_adsb_service.h GainRule), after Stuart's FlightAware box (dump1090-fa
// --gain 60 --adaptive-range, ADAPTIVE_BURST=no): the top of the list, down only for dynamic range, never for strong
// aircraft; an SDRplay (levelHeld) only down, and only for replies clipping.
#include "vibe_adsb_service.h"
#include <cstdio>

static int fails = 0, passes = 0;
static void ok(bool c, const char* what) { if (c) passes++; else { fails++; std::printf("FAIL %s\n", what); } }
static vibeadsb::Health H(double noise, double strong, bool valid = true) {
    vibeadsb::Health h; h.noiseDbfs = noise; h.strongFrac = strong; h.valid = valid; h.msgsPerSec = 300; return h;
}

int main() {
    vibeadsb::GainRule r;
    ok(r.decide(H(-25, 0)) == -1, "noise -25 dBFS (25 dB of range, under 30) -> down");
    ok(r.decide(H(-30.5, 0)) == 0, "noise -30.5 (the FlightAware box's settled -30.6) -> stay");
    ok(r.decide(H(-31.7, 0.08)) == 0, "the Pi 500 V4L's -31.7 with 8 % strong -> stay (strong is no reason to cut)");
    ok(r.decide(H(-42, 0.11)) == +1, "the Pi 2's -42 at 11 % strong -> up (the shim clamps at the top)");
    ok(r.decide(H(-32, 0)) == 0, "-32: inside the 3 dB of spare -> stay, no climb");
    ok(r.decide(H(-25, 0, false)) == 0, "no evidence yet -> stay");
    ok(r.decide(H(-20, 0.60)) == -1, "a hot front end is cut for its noise floor, whatever the strong share");

    vibeadsb::GainRule s; s.levelHeld = true;
    ok(s.decide(H(-43, 0.02)) == 0, "SDRplay: quiet and clean -> stay (never climbs)");
    ok(s.decide(H(-43, 0.10)) == -1, "SDRplay: 10 % clipping -> down");
    ok(s.decide(H(-20, 0.0)) == 0, "SDRplay: its noise floor is its AGC's, not ours -> stay");

    std::printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", passes + fails, fails);
    return fails ? 1 : 0;
}
