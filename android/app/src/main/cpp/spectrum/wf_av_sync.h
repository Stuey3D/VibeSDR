// wf_av_sync.h — THE WATERFALL WAITS FOR THE SOUND (V12 2026-10-30). A LINE-FOR-LINE port of src/assets/wfAvSync.ts (the
// app's JS path uses that one); scripts/test_wf_avsync.ts proves the rule, test_spectrum_engine.cpp the engine with it.
// Change one, change both.
//
// ★★★ The audio plays a cushion behind its arrival (the AudioTrack + packet queue — VibeStreamService.audioPlayoutMs), the
//     clock's route ~80 ms: the picture ran ~140 ms AHEAD of the sound at rest. Each clocked frame now enters the clock
//     `hold` ms after it arrived (wf_row_clock.h clockArrive) — the audio's delay less the route's own — stepped, the
//     clock's repeats carrying a growth and its discards a shrink. Stuart (2026-10-30): "to prevent scrolling judder whilst
//     the jitter buffer enables we can simply duplicate the previous lines if needed and then the waterfall still reacts
//     to the audio in time but still stays smooth". ZERO while a tune or zoom is in hand (passthrough). NATIVE is not held
//     (no clock, no repeats — see the TS). Unknown audio (−1) = no hold, the waterfall as it was.
#pragma once
#include "wf_row_clock.h"
#include <algorithm>
#include <cmath>

namespace vibewf {

constexpr double AV_MAX_MS = 500, AV_DEAD_MS = 8, AV_SMOOTH_MS = 500;
// ★★★ 2026-10-31 a GROWTH RAMPS at AV_GROW ms per ms (wfAvSync.ts): stepped, it stopped the picture and the trace for the
//     whole step ~250 ms after every tune or zoom ("a noticeable pause after zooming or tuning", "the spectrum stops too").
constexpr double AV_GROW = 0.25;

struct AvSyncState { double aud = -1, audT = 0, hold = 0; bool pass = false; double holdT = 0; bool ramp = false; };

/** A sample of the audio's playout delay (ms; < 0 = unknown) at `now`. */
inline void avNoteAudio(AvSyncState& s, double ms, double now) {
    if (!(ms >= 0) || !std::isfinite(ms)) { s.aud = -1; s.audT = now; return; }
    if (s.aud < 0 || !(now > s.audT)) { s.aud = ms; s.audT = now; return; }
    s.aud += (ms - s.aud) * (1 - std::exp(-std::min(5000.0, now - s.audT) / AV_SMOOTH_MS));
    s.audT = now;
}
/** The hold for a frame arriving now: the audio's delay − `ownMs`, in [0, AV_MAX_MS] (changes under AV_DEAD_MS let be);
 *  0 in a passthrough. A shrink steps; a GROWTH ramps at AV_GROW × the time since the last call (`now` ≥ 0; < 0 = step,
 *  the rule tests' old form). */
inline double avHold(AvSyncState& s, double ownMs, bool passthrough, double now = -1) {
    s.pass = passthrough;
    const double dt = (now >= 0 && s.holdT > 0) ? std::max(0.0, std::min(1000.0, now - s.holdT)) : -1;
    if (now >= 0) s.holdT = now;
    if (passthrough) { s.hold = 0; s.ramp = false; return 0; }
    const double want = s.aud >= 0 ? std::max(0.0, std::min(AV_MAX_MS, s.aud - ownMs)) : 0;
    if (std::abs(want - s.hold) > AV_DEAD_MS || (want == 0 && s.hold > 0) || (s.ramp && want > s.hold))
        s.hold = (want > s.hold && dt >= 0) ? std::min(want, s.hold + AV_GROW * dt) : want;
    s.ramp = s.hold < want;
    return s.hold;
}
/** The clock's own route, arrival → screen: its presentation delay + the rows the UI holds in hand + its lean (20 ms). */
inline double clockOwnMs(double ivl, double rate) { return clockDelayMs(ivl) + traceLagMs(rate) + 20; }

}  // namespace vibewf
