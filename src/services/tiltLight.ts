/**
 * tiltLight — the TILT LIGHT controller (BRIEF-lighting-and-vfd-glass §5) and its measurement switch.
 *
 * ★★★ NO REACT ON THE HOT PATH. The sensor (or the synthetic source) calls back on the JS thread ~30 times a
 *   second; each call runs the pure maths in constants/tiltLight.ts and, when the write gate opens, sets
 *   Reanimated SharedValues — which Skia reads on the UI thread. No setState, no context change: a moving light
 *   costs no React render anywhere (the brief's "0 React renders from tilt"). FaceplateContext only learns
 *   whether tilt is DRIVING (to hide the LIGHT ANGLE row), which changes when the sensor starts or stops.
 *
 * ★★ THE MEASUREMENT SWITCH (§5.2). `tiltDebugMode`:
 *     'auto'      — normal: the real sensor, when tiltShouldRun says so (the default, and what ships)
 *     'off'       — tilt forced off (the baseline run): fixed light, no subscription
 *     'synthetic' — a deterministic slow tilt (syntheticGravity) through the SAME path as the sensor, no sensor
 *   Set it with a link the deep-link handler intercepts before anything else sees it:
 *     vibesdr://debug/tilt/off   ·   vibesdr://debug/tilt/synthetic   ·   vibesdr://debug/tilt/auto
 *   (adb: `adb shell am start -a android.intent.action.VIEW -d vibesdr://debug/tilt/synthetic com.vibesdr.app`;
 *   iPhone: open the link in Safari or Notes.) Not persisted — a relaunch is 'auto' again, so a forgotten
 *   measurement can never ship to a user. While it is not 'auto', a line is logged every 10 s:
 *     [tilt] mode=synthetic writes=… providerRenders=… sec=10
 *   so a run can be checked for zero React renders from tilt (providerRenders is FaceplateProvider's render count).
 */
import { AppState, Platform } from 'react-native';
import type { SharedValue } from 'react-native-reanimated';
import {
  TILT, lightFromTilt, lowPass, rungWriteMs, screwStep, shouldWrite, syntheticGravity, tiltFromGravity,
  type ScrewState, type TiltRung, type Vec3,
} from '../constants/tiltLight';

export type TiltDebugMode = 'auto' | 'off' | 'synthetic';
let mode: TiltDebugMode = 'auto';
const modeSubs = new Set<() => void>();
export function tiltDebugMode(): TiltDebugMode { return mode; }
export function setTiltDebugMode(m: TiltDebugMode) {
  if (m === mode) return;
  mode = m;
  modeSubs.forEach(f => f());
}
export function onTiltDebugMode(f: () => void): () => void { modeSubs.add(f); return () => { modeSubs.delete(f); }; }

/** vibesdr://debug/tilt/<mode> → handled (true) or not a tilt link (false). */
export function handleTiltDebugLink(url: string): boolean {
  const m = /^(?:vibesdr|sdr):\/\/debug\/tilt\/(auto|off|synthetic)\b/i.exec(url);
  if (!m) return false;
  setTiltDebugMode(m[1].toLowerCase() as TiltDebugMode);
  return true;
}

/** FaceplateProvider bumps this on every render — the harness's evidence that tilt causes none. */
export const tiltProbe = { providerRenders: 0, writes: 0 };

/** The axis convention of accelerationIncludingGravity per platform — see tiltFromGravity's ▶ note. */
const SX = 1;
const SY = Platform.OS === 'ios' ? -1 : 1;

export interface TiltTargets {
  lightSv: SharedValue<number>; shiftSv: SharedValue<number>; screwSv: SharedValue<number>;
}

/**
 * Start driving the light around `baseDeg`. Returns a stop function that unsubscribes and puts the light back
 * at `baseDeg`. `source` is the real sensor or the synthetic one; both feed `feed()` below.
 */
export function startTilt(t: TiltTargets, baseDeg: number, rung: TiltRung, source: 'sensor' | 'synthetic'): () => void {
  let lr = 0, lp = 0, br = 0, bp = 0, primed = false, lastAt = 0;
  let wDeg = baseDeg, wShift = 0, wAt = 0;
  let screws: ScrewState = { shown: baseDeg, cand: baseDeg, since: 0 };
  const minWriteMs = rungWriteMs(rung);
  const feed = (g: Vec3, orientation: number, sx: number, sy: number, now: number) => {
    const tilt = tiltFromGravity(g, orientation, sx, sy);
    if (!primed) { lr = br = tilt.roll; lp = bp = tilt.pitch; primed = true; lastAt = now; return; }
    const dt = now - lastAt; lastAt = now;
    lr = lowPass(lr, tilt.roll, dt, TILT.lpTauMs);
    lp = lowPass(lp, tilt.pitch, dt, TILT.lpTauMs);
    // The baseline follows the HELD attitude slowly, so a still phone settles back to the stored angle.
    br = lowPass(br, lr, dt, TILT.baseTauMs);
    bp = lowPass(bp, lp, dt, TILT.baseTauMs);
    const l = lightFromTilt(lr - br, lp - bp, baseDeg);
    if (shouldWrite(wDeg, wShift, l.deg, l.shift, wAt, now, minWriteMs)) {
      wDeg = l.deg; wShift = l.shift; wAt = now;
      t.lightSv.value = l.deg;
      t.shiftSv.value = l.shift;
      tiltProbe.writes++;
    }
    // Rung 1+: the screws stay put — only the large plates move.
    if (rung === 0) {
      const s = screwStep(screws, l.deg, now);
      screws = s.st;
      if (s.emit !== null) t.screwSv.value = s.emit;
    }
  };

  let stop: () => void;
  if (source === 'synthetic') {
    const t0 = Date.now();
    const id = setInterval(() => { const now = Date.now(); feed(syntheticGravity(now - t0), 0, 1, 1, now); }, TILT.intervalMs);
    stop = () => clearInterval(id);
  } else {
    // ★ Required lazily: the module is only loaded where tilt can actually run (never on the Mac / TV, which
    //   are rung 3 and never get here), and a binary without it simply has no tilt.
    let sub: { remove: () => void } | null = null;
    try {
      const { DeviceMotion } = require('expo-sensors') as typeof import('expo-sensors');
      DeviceMotion.setUpdateInterval(TILT.intervalMs);
      sub = DeviceMotion.addListener((m) => {
        const g = m.accelerationIncludingGravity;
        if (!g) return;
        feed(g, typeof m.orientation === 'number' ? m.orientation : 0, SX, SY, Date.now());
      });
    } catch { sub = null; }
    stop = () => { sub?.remove(); };
  }
  return () => {
    stop();
    t.lightSv.value = baseDeg; t.shiftSv.value = 0; t.screwSv.value = baseDeg;
  };
}

/** Is a motion sensor there at all? (Cached; false on any failure — the LIGHT ANGLE row then shows.) */
let sensorKnown: Promise<boolean> | null = null;
export function tiltSensorAvailable(): Promise<boolean> {
  if (!sensorKnown) {
    sensorKnown = (async () => {
      try {
        const { DeviceMotion } = require('expo-sensors') as typeof import('expo-sensors');
        return await DeviceMotion.isAvailableAsync();
      } catch { return false; }
    })();
  }
  return sensorKnown;
}

/** The harness's 10 s log line, only while a measurement mode is set. */
let logTimer: ReturnType<typeof setInterval> | null = null;
let lastLog = { renders: 0, writes: 0 };
function syncLogger() {
  if (mode === 'auto') { if (logTimer) { clearInterval(logTimer); logTimer = null; } return; }
  if (logTimer) return;
  lastLog = { renders: tiltProbe.providerRenders, writes: tiltProbe.writes };
  logTimer = setInterval(() => {
    const r = tiltProbe.providerRenders - lastLog.renders, w = tiltProbe.writes - lastLog.writes;
    lastLog = { renders: tiltProbe.providerRenders, writes: tiltProbe.writes };
    // eslint-disable-next-line no-console
    console.log(`[tilt] mode=${mode} writes=${w} providerRenders=${r} sec=10 app=${AppState.currentState}`);
  }, 10_000);
}
onTiltDebugMode(syncLogger);
