/**
 * wfAvSync.ts — THE WATERFALL WAITS FOR THE SOUND (V12, 2026-10-30). One rule for the app's JS path (WaterfallView) and —
 * ported line for line — the Android engine (android/app/src/main/cpp/spectrum/wf_av_sync.h). Pure, so
 * scripts/test_wf_avsync.ts can drive it with simulated feeds, audio and drags.
 *
 * ★★★ WHY. The audio carries a cushion — iOS schedules 220 ms ahead (VibePowerModule.swift kPreRollSeconds), Android's
 *     AudioTrack and packet queue hold their own — and after a network hiccup it re-queues deeper (up to the 0.4 s live-edge
 *     bound). The fixed output clock's route is ~70–80 ms from arrival to screen (its presentation delay + the rows the
 *     release holds in hand). So the picture ran ~140 ms AHEAD of the sound at rest and ~320 ms after a hiccup: a signal
 *     drawn before it is heard. The web client has held its rows by the audio's depth since 2026-08-10
 *     (web/client/src/waterfall.ts setHoldMs) — "one change, two problems, and they pull in the SAME direction". Stuart
 *     approved the app doing the same on 2026-10-30, on one condition: nothing about HOW the waterfall runs changes.
 *
 * ★★★ SO ONLY *WHEN* A FRAME ENTERS THE CLOCK MOVES. The frame is handed over at its real arrival, stamped `hold` ms later
 *     (wfClock.ts clockArrive's holdMs): the clock's ticks, its rate, the interaction boost, the zoom fan and the tune ease
 *     are all exactly as they were — a held frame is simply presented later on the same schedule. The SPECTRUM TRACE rides
 *     its row's presentation (traceLagMs), so it is held with it for free.
 *
 * ★★★ THE HOLD STEPS STRAIGHT TO ITS TARGET — THE CLOCK'S REPEATS CARRY IT. Stuart (2026-10-30): "to prevent scrolling
 *     judder whilst the jitter buffer enables we can simply duplicate the previous lines if needed and then the waterfall
 *     still reacts to the audio in time but still stays smooth". When the hold GROWS (re-engaging after a tune or zoom, the
 *     audio ratcheting deeper after a hiccup) the clock keeps ticking at its rate and repeats the row on screen until the
 *     held frames are due — the path a late frame already takes — so the picture is back in step within the growth, and
 *     the scroll never stops. When it SHRINKS, the frames now due later than a newer one are discarded (over-interpolate
 *     and discard) — never a burst. A small deadband (AV_DEAD_MS) keeps the smoothed audio's last few ms of wander from
 *     moving it on every frame. The audio's measured depth is smoothed first (it saw-tooths by a packet as buffers are
 *     queued and played).
 *
 * ★★★ GET OUT OF THE WAY WHILE THE DIAL MOVES (the web's lesson, Stuart 2026-08-10: "when I tune the RSP everything freezes
 *     completely then resumes when I stop tuning"). A held row is `hold` ms from the screen, so a dial moving faster than
 *     that would never show a fresh row. While a tune or zoom is in hand the hold is ZERO — PASSTHROUGH, rows exactly as
 *     immediate as before any of this — and whatever was still held is discarded (a shrink to 0).
 *
 * ★★ NATIVE IS NOT HELD, and that is measured, not skipped (scripts/test_wf_avsync.ts). Native has no clock and no repeats
 *    by rule ("pure unmodified lines direct from the server"), so a hold there can only change by slowing or speeding the
 *    rows themselves — and its release (wfPace rowPace, one row every N display frames with occasional slips) loses its
 *    rhythm on a change of even 1 %: on a 23 fps feed at 120 Hz, 0 stalls became 7–25 at any slew from 15 % down to 1 %.
 *    And it does not need one: the jitter buffer + that release already put Native ~280–330 ms behind its arrival — level
 *    with or BEHIND a 220 ms audio cushion. Stuart's rule (no stalls) wins; Native runs exactly as it did.
 *
 * ★ Kill switch: an audio delay of −1 (unknown — a native build without the getter, no audio playing) is a target of 0,
 *   which is the waterfall exactly as it was.
 */

import { clockDelayMs } from './wfClock.ts';   // ★ .ts: node runs this file directly (the tests)
import { traceLagMs } from '../utils/wfPace.ts';

/** The longest hold (the web's ceiling too), and the change it ignores (ms). */
export const AV_MAX_MS = 500;
export const AV_DEAD_MS = 8;
/** The audio depth's smoothing time constant, ms (its samples saw-tooth by a packet, 20–40 ms). */
export const AV_SMOOTH_MS = 500;

export interface AvSyncState {
  aud: number;     // the audio's playout delay, smoothed (ms; < 0 = unknown)
  audT: number;    // when it was last sampled
  hold: number;    // the hold now, ms
  pass: boolean;   // in a passthrough (a tune or zoom in hand)
}
export const newAvSync = (): AvSyncState => ({ aud: -1, audT: 0, hold: 0, pass: false });

/** A sample of the audio's playout delay (ms from arrival to the speaker; < 0 = unknown) at `now`. */
export function avNoteAudio(s: AvSyncState, ms: number, now: number): void {
  if (!(ms >= 0) || !isFinite(ms)) { s.aud = -1; s.audT = now; return; }
  if (s.aud < 0 || !(now > s.audT)) { s.aud = ms; s.audT = now; return; }
  s.aud += (ms - s.aud) * (1 - Math.exp(-Math.min(5000, now - s.audT) / AV_SMOOTH_MS));
  s.audT = now;
}

/** What to hold a frame arriving now: the audio's delay − `ownMs` (what the clock's route already takes), ≥ 0,
 *  ≤ AV_MAX_MS, as a STEP (the clock repeats and discards around it); a change under AV_DEAD_MS is let be.
 *  `passthrough` (a tune or zoom in hand): 0. Returns the hold, ms. */
export function avHold(s: AvSyncState, ownMs: number, passthrough: boolean): number {
  s.pass = passthrough;
  if (passthrough) { s.hold = 0; return 0; }
  const want = s.aud >= 0 ? Math.max(0, Math.min(AV_MAX_MS, s.aud - ownMs)) : 0;
  if (Math.abs(want - s.hold) > AV_DEAD_MS || (want === 0 && s.hold > 0)) s.hold = want;
  return s.hold;
}

/** What the fixed clock's route already takes from arrival to screen, ms: its presentation delay (0.2 × the interval,
 *  ≤ 40 ms) + the rows the release holds in hand (traceLagMs) + the schedule's own lean past them (measured in
 *  scripts/test_wf_avsync.ts: ~20 ms at Default 30, ~5 at Smooth 60). `ivl` the feed interval, `rate` rows/s. */
export function clockOwnMs(ivl: number, rate: number): number {
  return clockDelayMs(ivl) + traceLagMs(rate) + 20;
}
