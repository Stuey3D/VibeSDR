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

/** Tell the native side the cap: 60, or 0 for the display's own maximum. */
export function applyFrameRateCap(hz: number): void {
  const fn = native()?.setFrameRateCap;
  if (typeof fn !== 'function') return;
  try { fn(hz); } catch { /* an old binary's bridge refusing is the same as no method */ }
}
