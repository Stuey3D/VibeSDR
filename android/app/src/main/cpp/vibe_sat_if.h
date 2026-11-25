#pragma once
/**
 * vibe_sat_if.h — THE SATELLITE PASS'S IF FILTER (V12 beta92, Stuart 11-23).
 *
 * ★★★ WHY. Stuart's Pi 500, RTL-SDR Blog V4 on a YouLoop, METEOR-M2 4 on 2026-11-23: the pass's gain was measured with the
 *     tuner's IF filter at 1000 kHz (the view was wide), and the strong neighbours inside it filled the 8-bit converter —
 *     peak -13.5 dBFS at 14.4 dB, so the hold set 19.7 dB. He then zoomed in, the view-driven auto filter went 1000 → 700
 *     → 500 → 350 kHz, and at the SAME gain the peak fell from -11.3 to -20.6 dBFS: 9 dB of headroom the pass had been
 *     denied. Re-measured behind 350 kHz the gain went to 32.8 dB and the pass decoded 728 lines, his best on that
 *     receiver. "i think what might be good for satellites is an auto narrowing IF filter".
 *
 * ★★★ THE RULE — never narrower than the signal plus its whole Doppler swing, measured from where the filter is centred
 *     (the TUNER, not the dial):
 *
 *         width ≥ 2 × ( |signal centre − tuner centre| + occupied / 2 + Doppler )
 *         Doppler = 2.7e-5 × f  (the largest a low orbit gives: 7.5 km/s of range rate / c, with margin)
 *                 + 2.5 kHz      (the Doppler lock's own trim clamp, Engine::kTrimClampHz)
 *
 *     rounded UP to the narrowest width the radio offers; a radio whose narrowest is not narrower than its capture's own
 *     filter is left alone. The occupied width per kind of downlink (occupiedHz):
 *       · METEOR LRPT        150 kHz (72 k symbols; 2 × the symbol rate for 80 k)
 *       · a linear transponder  its passband + 3 kHz (an SSB channel at either edge)
 *       · a skimmed CW beacon   24 kHz (the skimmer's window, kBeaconSkimHz)
 *       · AX.25 9600 (G3RUH)    25 kHz
 *       · FM voice / APRS 1200 / SSTV   20 kHz (5 kHz deviation + 3 kHz audio, Carson's 16 kHz with margin)
 *       · SSB / CW / BPSK telemetry      3 kHz
 *       · ORBCOMM (2026-11-24)           its decoder slice: 40 kHz, or the listed band + 40 kHz
 *     The listening demodulator's own channel is a floor too (the shim passes its width).
 *
 * Pure: no radio, no clock — vibeserver/test-sat-if.cpp drives it.
 */
#include "vibe_sat_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace vibesatif {

constexpr double kLeoDopplerFrac = 2.7e-5;   // ★ the same figure the Doppler lock's reach checks use (local_sdr_shim satLockOp)

/** The width the downlink's signal occupies, Hz, and what it is (for the log). */
inline double occupiedHz(const vibe::sat::Transmitter& t, std::string* what = nullptr) {
    auto say = [&](const char* w) { if (what) *what = w; };
    if (t.consumer == "lrpt" || t.mode == "lrpt") {
        say("LRPT");
        return std::max(150000.0, 2.0 * (t.baud > 0 ? t.baud : 72000.0));
    }
    // ★ 2026-11-24 ORBCOMM: the decoder's own slice (one channel ± its neighbours, or the band + the same margin)
    if (t.orbcomm()) { say("ORBCOMM"); return vibe::sat::kOrbMaxSliceHz; }   // ★ always the whole band's slice (decoderOf)
    // ★★★ beta94 the transponder finder's span: the passband AND its beacon (one slice — vibe_sat_engine.h linkXpdr)
    if (t.xpdr()) { say("transponder + beacon"); return (t.xpdrHi - t.xpdrLo) + 3000.0; }
    if (t.passband()) { say("transponder"); return (t.hiHz - t.hz) + 3000.0; }
    if (vibe::sat::beaconSkimmed(t)) { say("CW beacon (skimmed)"); return vibe::sat::kBeaconSkimHz; }
    if (t.consumer == "ax25-9600") { say("9600 packet"); return 25000.0; }
    if (t.mode == "nfm") {
        say(t.consumer.rfind("ax25", 0) == 0 ? "APRS" : vibe::sat::sstvOf(t) ? "FM voice / SSTV" : "FM");
        return 20000.0;
    }
    if (t.mode == "am") { say("AM"); return 10000.0; }
    if (t.consumer == "fc-bpsk" || t.consumer == "fox-bpsk") { say("BPSK telemetry"); return 3000.0; }
    if (t.mode == "usb" || t.mode == "lsb" || t.mode == "cw") { say(t.mode == "cw" ? "CW" : "SSB"); return 3000.0; }
    say("downlink");
    return 25000.0;
}
/** The largest Doppler one side of the nominal over a pass at `hz`, plus the lock's trim clamp. */
inline double dopplerHz(double hz) { return std::fabs(hz) * kLeoDopplerFrac + vibe::sat::Engine::kTrimClampHz; }

/** One downlink the filter must hold. */
inline constexpr double kViewSpanX = 2.5;          ///< the opening view: this many times the region (viewSpanHz)
inline constexpr double kViewMinSpanHz = 50000.0;  ///< …and never narrower than this
inline constexpr double kOrbViewMarginHz = 60000.0; ///< ★ an ORBCOMM pick's view: its decoder slice + this

struct Region {
    double centreHz = 0;   // where the signal sits (its nominal centre; the Doppler is in halfHz)
    double halfHz = 0;     // occupied / 2 + Doppler
    std::string why;       // "LRPT 150 kHz + Doppler ±6.2 kHz"
    std::string owner;     // the session whose pick it is ("" = the room's dial / an unattended pass)
};
inline Region regionOf(const vibe::sat::Transmitter& t, double demodBwHz = 0.0) {
    Region r;
    std::string what;
    const double occ = std::max(occupiedHz(t, &what), demodBwHz);
    const double dop = dopplerHz(t.centre());
    r.centreHz = t.orbcomm() ? vibe::sat::orbcommBandCentreHz(t) : t.xpdr() ? t.xpdrCentre() : t.centre();   // ★ ORBCOMM: the band; ★★★ beta94 a finder: its span
    r.halfHz = occ * 0.5 + dop;
    char b[160];
    std::snprintf(b, sizeof b, "%s %.0f kHz + Doppler \xc2\xb1%.1f kHz", what.c_str(), occ / 1e3, dop / 1e3);
    r.why = b;
    return r;
}
/** ★★ The span a client's view OPENS at when this downlink is picked (Stuart 11-24, a METEOR on the Pi 500's V4 with the
 *  pass's 350 kHz IF filter: "we need to start zoomed in, not all the way but enough to see the signal and neighbours" —
 *  his chosen level was ~420 kHz across). 2.5 × what the IF filter has to hold (the signal + its whole Doppler swing):
 *  METEOR LRPT 150 kHz + ±3.4 kHz → ~410 kHz; the ISS's FM → ~60 kHz; a CW beacon or telemetry → the 50 kHz floor. The
 *  client caps it at its capture. */
inline double viewSpanHz(const vibe::sat::Transmitter& t) {
    /* ★★ ORBCOMM (Stuart 11-24: "its different to every other satellite which is why i manually tuned"): the view is the
     *  decoder's WHOLE slice and a little margin — the shaded "every carrier decoded here" band and every carrier on screen
     *  at once (a band pick: 640 kHz → 700 kHz), not 2.5 × it. */
    if (t.orbcomm()) return std::max(kViewMinSpanHz, occupiedHz(t) + kOrbViewMarginHz);
    return std::max(kViewMinSpanHz, kViewSpanX * 2.0 * regionOf(t).halfHz);
}
/** Half the filter width the regions need around a tuner centred at `rfHz` (0 = none). */
inline double halfNeeded(const std::vector<Region>& rs, double rfHz) {
    double h = 0.0;
    for (const auto& r : rs) h = std::max(h, std::fabs(r.centreHz - rfHz) + r.halfHz);
    return h;
}
/** ★★ beta93 NEVER NARROWER THAN 500 kHz FOR A PASS (Stuart 11-24: "maybe a 500khz if filter is needed as the satellites
 *  with beacons and transponders on 2 vfo's will need a wider filter"). The RTL's 350 kHz rung is its tuner's floor — the
 *  narrowest, least even filter it has — and a 150 kHz METEOR plus Doppler sits close to its edges; 500 kHz leaves room
 *  for a second downlink and the filter's own skirts. Strong out-of-band stations stay out either way. */
inline constexpr double kPassMinWidthHz = 500000.0;
/** Half the filter width a pass needs around `rfHz`: what the regions need, and never under kPassMinWidthHz. 0 = no pass. */
inline double halfForPass(const std::vector<Region>& rs, double rfHz) {
    if (rs.empty()) return 0.0;
    return std::max(0.5 * kPassMinWidthHz, halfNeeded(rs, rfHz));
}
/** The narrowest rung ≥ widthHz from an ascending list; 0 when none is (or none is narrower than `captureHz`). */
inline int narrowestRung(double widthHz, const std::vector<int>& rungs, double captureHz) {
    for (int r : rungs) {
        if (r < widthHz) continue;
        if (captureHz > 0 && r >= captureHz) return 0;   // ★ no narrower than the capture itself: nothing to gain
        return r;
    }
    return 0;
}
/** ★ The RTL (R820T/R828D) widths the server can set — kIfRungs in the shim, 350 kHz the tuner's real floor. */
inline std::vector<int> rtlRungs() { return {350000, 500000, 700000, 1000000, 1400000, 2000000, 2800000, 4000000}; }
/** ★ The SDRplay API's IF filters (sdrplay_api_Bw_MHzT), kHz → Hz. Zero-IF: the capture's own filter is
 *  SdrplaySource::bandwidthKHzForRate; a pass only ever chooses one NARROWER than that. */
inline std::vector<int> rspRungs() { return {200000, 300000, 600000, 1536000, 5000000, 6000000, 7000000, 8000000}; }

}  // namespace vibesatif
