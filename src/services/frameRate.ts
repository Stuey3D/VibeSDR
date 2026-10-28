/**
 * frameRate.ts — the FRAME RATE setting's native half (CONTROL CUSTOMISATION → FACEPLATE).
 *
 * Power audit, 2026-10-01: on a ProMotion iPhone the app held the panel at 120 Hz the whole time the
 * radio streamed (react-native-worklets' display link asks for 120, Info.plist has
 * CADisableMinimumFrameDurationOnPhone, and Skia / Reanimated follow). Stuart: "add a 60Hz option".
 *
 * Two methods on the existing VibeLocalSDR module (modules/vibe-local-sdr/VibeLocalSDR.mm,
 * android/.../VibeLocalSdrModule.kt) — no new module, as deviceClass.ts:
 *   • maxRefreshRate() → Promise<number>: the panel's top rate (iOS UIScreen.maximumFramesPerSecond,
 *     read on the main thread; Android the fastest display mode at the current resolution).
 *   • setFrameRateCap(hz): 0 = no cap, 60 = cap. ★ LIVE — no restart on either platform:
 *       iOS     every CADisplayLink in the process is clamped (one swizzle, installed at +load, so
 *               worklets / Reanimated / RN's own links are covered from their first frame), and the
 *               cap is kept in NSUserDefaults so the NEXT launch is capped before JS has run.
 *       Android the window's preferredDisplayModeId / preferredRefreshRate (API 23+), re-applied on
 *               every resume (a recreated Activity has a fresh window); SharedPreferences likewise.
 *
 * ★ Guarded like deviceClass.ts: an old binary without the methods, Expo Go, web and tests get
 *   null / a silent no-op — and frameRateChoices(null) hides the row, so nobody is offered a switch
 *   the binary cannot honour.
 */
import { NativeModules, Platform } from 'react-native';

type Native = {
  maxRefreshRate?: () => Promise<unknown>;
  setFrameRateCap?: (hz: number) => void;
  setSteadyRefresh?: (hz: number) => void;
  startFrameWatch?: () => void;
  stopFrameWatch?: () => void;
  takeFrameWatch?: () => unknown;
  isPowerSaveMode?: () => unknown;
};

function native(): Native | undefined {
  if (Platform.OS === 'web') return undefined;
  return (NativeModules as { VibeLocalSDR?: Native }).VibeLocalSDR;
}

let maxHz: Promise<number | null> | null = null;

/** The panel's top refresh rate in Hz, or null when the binary cannot say. Asked ONCE per process. */
export function readMaxRefreshRate(): Promise<number | null> {
  if (maxHz) return maxHz;
  const fn = native()?.maxRefreshRate;
  maxHz = typeof fn === 'function'
    ? Promise.resolve().then(() => fn()).then((v: unknown) => (typeof v === 'number' ? v : null), () => null)
    : Promise.resolve(null);
  return maxHz;
}

/** iOS/Mac: pin every display link at `hz` while the waterfall scrolls (VibeLocalSDR.mm gFpsSteady), 0 = release — a
 *  ProMotion panel left free to wander 60…120 put whole-row steps on an uneven beat (Stuart's Mac, 2026-10-28). No-op
 *  elsewhere and on an old binary. Sends only a CHANGE. */
let steadyHz = 0;
export function setSteadyRefresh(hz: number): void {
  if (hz === steadyHz) return;
  steadyHz = hz;
  const fn = native()?.setSteadyRefresh;
  if (typeof fn !== 'function') return;
  try { fn(hz); } catch { /* old binary */ }
}

export { steadyHzFor, STEADY_RATES, FEED_RATES } from '../utils/steadyHz';

/** Tell the native side the cap: 60 or 30, or 0 for the display's own maximum. */
export function applyFrameRateCap(hz: number): void {
  const fn = native()?.setFrameRateCap;
  if (typeof fn !== 'function') return;
  try { fn(hz); } catch { /* an old binary's bridge refusing is the same as no method */ }
}

// ── FRAME WATCH — what FRAME RATE → AUTO judges by (Android only; VibeLocalSdrModule.kt) ──────────────
/** True when this binary can count late frames (Android with the native half). iOS AUTO stays at the panel's maximum:
 *  every supported iPhone keeps up, and ProMotion already steps a quiet screen down by itself. */
export function frameWatchAvailable(): boolean {
  const n = native();
  return Platform.OS === 'android' && typeof n?.startFrameWatch === 'function' && typeof n?.takeFrameWatch === 'function';
}
export function startFrameWatch(): void { try { native()?.startFrameWatch?.(); } catch { /* old binary */ } }
export function stopFrameWatch(): void { try { native()?.stopFrameWatch?.(); } catch { /* old binary */ } }
/** Frames drawn and frames late since the last call, or null when there is no watch. */
export function takeFrameWatch(): { frames: number; late: number } | null {
  try {
    const r = native()?.takeFrameWatch?.() as { frames?: unknown; late?: unknown } | undefined;
    if (!r || typeof r.frames !== 'number' || typeof r.late !== 'number') return null;
    return { frames: r.frames, late: r.late };
  } catch { return null; }
}

/** ★ Android battery saver (VibeLocalSdrModule.isPowerSaveMode): AUTO runs at 30 while it is on. False on iOS (Low Power
 *  Mode already caps ProMotion at 60 for every app) and on an old binary. */
export function powerSaveOn(): boolean {
  try { return native()?.isPowerSaveMode?.() === true; } catch { return false; }
}
