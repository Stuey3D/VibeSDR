/**
 * tiltLight.ts — TILT LIGHTING (BRIEF-lighting-and-vfd-glass §5) as pure maths, so scripts/test_faceplate_lighting.ts
 * can hold every rule down without a phone: the gravity → screen tilt, the two low-pass filters, the mapping onto
 * the light, the write gate, the screws' settle, the synthetic source the measurement uses, and the per-device
 * fallback ladder.
 *
 * ★★★ The PERFORMANCE CONDITION is part of the spec (Stuart: "only if we can pull it off with minimal performance
 *     cost"). Nothing here touches React: services/tiltLight.ts runs it in the sensor callback and writes only
 *     Reanimated SharedValues (FaceplateContext lightSv / shiftSv / screwSv), which Skia reads on the UI thread.
 * ★★ ONE INPUT PATH: the gravity vector (DeviceMotion.accelerationIncludingGravity, m/s²). It exists on every
 *    device — including the cheap Androids with no gyroscope the brief warns about — and its units are stated by
 *    the docs, where `rotation`'s are not consistent across platforms. The low-pass below takes out hand shake and
 *    the user's own acceleration; for LIGHT that is smooth enough (the brief: "smoother but laggier, fine for
 *    light"). The synthetic source (measurement) produces a gravity vector too, so it travels the SAME path.
 * ✗ Not worklets: this runs on the JS thread in the sensor callback, never on the UI thread.
 */

/** The brief's numbers (§5.1), named so the tests and the controller read the same ones. */
export const TILT = {
  /** DeviceMotion.setUpdateInterval — 30 Hz. */
  intervalMs: 33,
  /** Low-pass on the input (hand shake, the user's own acceleration). */
  lpTauMs: 150,
  /** The baseline (how the phone is being HELD) re-centres this slowly, so a phone on a desk settles to the
   *  stored angle instead of parking the highlight off the edge. */
  baseTauMs: 4000,
  /** Roll ±rollRange° relative to the baseline swings the angle ±angleSwing° around the stored angle. */
  rollRange: 20, angleSwing: 30,
  /** Pitch ±pitchRange° slides the sheen band ±shiftSwing (fraction of the gradient length). */
  pitchRange: 20, shiftSwing: 0.10,
  /** The write gate: a SharedValue write only when the light has moved this much… */
  minDeg: 0.5, minShift: 0.005,
  /** …and no more often than this (rung 2 of the fallback ladder: 50 ms = 20 Hz). */
  minWriteMs: 33,
  /** The screws follow once the light has moved ≥ screwStep° and held there for screwHoldMs. */
  screwStep: 3, screwHoldMs: 200,
} as const;

const G = 9.80665;
const RAD = 180 / Math.PI;

export interface Vec3 { x: number; y: number; z: number }
export interface Tilt { roll: number; pitch: number }

/**
 * Gravity (device axes) → tilt in SCREEN coordinates, degrees. `roll` = the screen's right edge going down is
 * positive; `pitch` = the screen's top edge going down (away from you) is positive.
 * ★★ TRAP (brief §5): roll and pitch swap meaning between portrait and landscape. The device-frame tilt is
 *   rotated by the screen orientation (DeviceMotion's `orientation`: 0 portrait, 90 right landscape,
 *   -90 left landscape, 180 upside down) so "tilt the right edge down" swings the light the same way on screen
 *   in every orientation.
 * ▶ The two signs (`sx`, `sy`) are the platform's axis convention for accelerationIncludingGravity. They are the
 *   one thing the bench cannot prove — check on the XCover and an iPhone (tilt the right edge down: the sheen
 *   should swing toward TOP-RIGHT's look) and flip here if not.
 */
export function tiltFromGravity(g: Vec3, orientation: number, sx: number, sy: number): Tilt {
  const x = sx * g.x, y = sy * g.y, z = g.z;
  const r = Math.atan2(x, Math.sqrt(y * y + z * z)) * RAD;
  const p = Math.atan2(y, Math.sqrt(x * x + z * z)) * RAD;
  const t = (orientation * Math.PI) / 180;
  const c = Math.round(Math.cos(t)), s = Math.round(Math.sin(t));   // orientations are exact quarter turns
  return { roll: r * c + p * s, pitch: -r * s + p * c };
}

/** One step of a first-order low-pass with time constant `tauMs`, over `dtMs` (frame-rate independent). */
export function lowPass(prev: number, x: number, dtMs: number, tauMs: number): number {
  if (!(dtMs > 0)) return prev;
  return prev + (x - prev) * (1 - Math.exp(-dtMs / tauMs));
}

const clamp = (v: number, lo: number, hi: number) => (v < lo ? lo : v > hi ? hi : v);

/** Tilt relative to the baseline → the live light: the angle around `baseDeg` and the sheen band's shift. */
export function lightFromTilt(rollRel: number, pitchRel: number, baseDeg: number): { deg: number; shift: number } {
  const r = clamp(rollRel, -TILT.rollRange, TILT.rollRange);
  const p = clamp(pitchRel, -TILT.pitchRange, TILT.pitchRange);
  return { deg: baseDeg + (r / TILT.rollRange) * TILT.angleSwing, shift: (p / TILT.pitchRange) * TILT.shiftSwing };
}

/** The write gate: true when the light has moved enough AND the last write is old enough. */
export function shouldWrite(lastDeg: number, lastShift: number, deg: number, shift: number,
                            lastAtMs: number, nowMs: number, minWriteMs: number): boolean {
  if (nowMs - lastAtMs < minWriteMs) return false;
  return Math.abs(deg - lastDeg) > TILT.minDeg || Math.abs(shift - lastShift) > TILT.minShift;
}

/** The screws' settle (brief: "update them when the angle has changed by more than 3° and held for 200 ms"). */
export interface ScrewState { shown: number; cand: number; since: number }
export function screwStep(st: ScrewState, deg: number, nowMs: number): { st: ScrewState; emit: number | null } {
  if (Math.abs(deg - st.shown) < TILT.screwStep) return { st: { shown: st.shown, cand: deg, since: nowMs }, emit: null };
  // A candidate is HELD while it stays within the step of where it started.
  if (Math.abs(deg - st.cand) >= TILT.screwStep) return { st: { shown: st.shown, cand: deg, since: nowMs }, emit: null };
  if (nowMs - st.since >= TILT.screwHoldMs) return { st: { shown: deg, cand: deg, since: nowMs }, emit: deg };
  return { st, emit: null };
}

/**
 * ★★ THE SYNTHETIC SOURCE (measurement harness): a deterministic, slow tilt as a GRAVITY VECTOR, so it goes
 *   through tiltFromGravity, both filters, the gate and the writes exactly as the sensor does — and a tilt-ON run
 *   is identical every time, without anyone moving the phone. Roll ±15° over 6 s, pitch ±10° over 9 s: inside
 *   the clamps, always moving, so the gate writes at its full rate (the worst case the budget is for).
 *   Device-frame, portrait; pass it with orientation 0 and signs (1, 1).
 */
export function syntheticGravity(tMs: number): Vec3 {
  const r = (15 * Math.sin((2 * Math.PI * tMs) / 6000)) / RAD;
  const p = (10 * Math.sin((2 * Math.PI * tMs) / 9000)) / RAD;
  const sr = Math.sin(r), sp = Math.sin(p);
  return { x: G * sr, y: G * sp, z: -G * Math.sqrt(Math.max(0, 1 - sr * sr - sp * sp)) };
}

/**
 * ★★★ THE FALLBACK LADDER (brief §5.3), per device class. Apply in order until the §5.2 budget is met:
 *   0  full tilt — plates live, screws follow on settle, writes ≤ 30 Hz
 *   1  screws (and caps/wells) stop following; only the large plates move
 *   2  writes capped at 20 Hz
 *   3  tilt OFF for this class — it gets the fixed LIGHT ANGLE row, as the Mac does
 * ▶▶ RECORD THE RUNG EACH TESTED DEVICE ENDED ON HERE (brief §5.3), with the numbers (scripts/measure-tilt-android.sh,
 *    the iPhone Instruments procedure in the brief §5.2):
 *      Samsung XCover 4S (Android, slowest supported) — rung 3 PROVISIONALLY (B19, Stuart 2026-10-02: option 2):
 *        the emulator proved 0 React renders and the write gate, but its frame-time/CPU figures were noise
 *        (two identical runs swung +100 % / −45 % CPU). ALL Android is held at rung 3 until the XCover is
 *        measured: scripts/measure-tilt-android.sh 60 192.168.86.111:36408, twice. Then set the rung here.
 *      oldest supported iPhone                        — rung ?, not yet measured
 *      Mac (iPad app on Apple silicon)                — rung 3 by definition: no motion sensor (Stuart)
 *      Apple TV / Android TV                          — rung 3 by definition: no motion sensor
 */
export type TiltRung = 0 | 1 | 2 | 3;
/** ★ Android held at rung 3 until the XCover is measured (see the ladder above). Flip to false then. */
export const ANDROID_TILT_UNMEASURED = true;
export function tiltRungFor(d: { isMac?: boolean; isTV?: boolean; modelId?: string | null; os?: string }): TiltRung {
  if (d.isMac || d.isTV) return 3;
  if (d.os === 'android' && ANDROID_TILT_UNMEASURED) return 3;
  return 0;
}
/** The write interval a rung allows. */
export function rungWriteMs(rung: TiltRung): number { return rung >= 2 ? 50 : TILT.minWriteMs; }

/**
 * May the sensor run at all? (brief §5.1 — "Unsubscribe otherwise. The sensor is never started on default.")
 * `screenOn` and `foreground` are both the app being ACTIVE; `deckVisible` is a deck mounted (HIDE CONTROLS
 * unmounts it).
 */
export function tiltShouldRun(o: { chassis: string; motionOn: boolean; sensor: boolean; active: boolean;
                                   deckVisible: boolean; rung: TiltRung }): boolean {
  return (o.chassis === 'silver' || o.chassis === 'black') && o.motionOn && o.sensor && o.active
      && o.deckVisible && o.rung < 3;
}
