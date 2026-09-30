/**
 * meterGuard — a signal meter must never be able to lock someone out of the app.
 *
 * ★★★ WHY (11 B7): choosing LED VU crashed the app on its first frame, and because the meter is a
 *   stored setting, EVERY entry into a receiver crashed it again — "I cannot now get back into a
 *   server". The only way out was deleting the app. The bug is fixed (meters.ts, WORKLET DEFAULTS);
 *   this is the guarantee that the NEXT one cannot do the same:
 *
 *   1. ARMED ON THE WAY IN. Before an LED / analogue meter is mounted, `vibesdr.meterArmed` is
 *      WRITTEN (and the meter waits for the write to land — an abort a frame later must find it on
 *      disk). It is cleared after METER_ARM_MS of the meter running, when the app leaves the
 *      foreground (a swipe-away from the switcher is not a crash), and when the meter unmounts.
 *   2. CHECKED ON THE WAY BACK. At launch FaceplateContext reads it: still there means the app died
 *      with that meter starting, so the meter goes back to BAR — stored — and the user is told once,
 *      with where to choose it again (`meterAfterUncleanExit` decides; the test pins it).
 *   3. IN SESSION: the meters' frame callbacks catch their own throw and hand it here
 *      (`reportMeterFault`), so a fault that shows up later than the armed window falls back to the
 *      bar instead of aborting the app.
 */

import { useEffect, useState } from 'react';
import { Alert, AppState } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import type { MeterKind } from '../constants/meters';
import { METER_CHOICES } from '../constants/faceplate';

export const METER_ARMED_KEY = 'vibesdr.meterArmed';
/** Long enough for a crash on mount, the first bus update, and the first squelch transition. */
export const METER_ARM_MS = 10_000;

/** Meters mounted and armed right now (one key, so disarm only when the last one is done). */
let armedCount = 0;

async function arm(kind: MeterKind): Promise<void> {
  armedCount++;
  try { await AsyncStorage.setItem(METER_ARMED_KEY, kind); } catch { /* storage down: mount anyway */ }
}
function disarm(): void {
  if (armedCount > 0) armedCount--;
  if (armedCount === 0) AsyncStorage.removeItem(METER_ARMED_KEY).catch(() => {});
}

/** At launch: the meter the previous run died starting, or null — and the mark is cleared. */
export async function takeUncleanMeterExit(): Promise<string | null> {
  try {
    const k = await AsyncStorage.getItem(METER_ARMED_KEY);
    if (k != null) await AsyncStorage.removeItem(METER_ARMED_KEY);
    return k;
  } catch { return null; }
}

/**
 * Arms the guard for a fancy meter and says when it may be mounted: false until the armed mark is on
 * disk (a meter that aborts on its first frame must already be recorded). The bar is never armed.
 */
export function useMeterArming(kind: MeterKind): boolean {
  // ★ Ready FOR A KIND: switching LED VU → analogue must not mount the new meter for the one render
  //   before its own mark is written.
  const [readyFor, setReadyFor] = useState<MeterKind | null>(null);
  useEffect(() => {
    if (kind === 'bar') return;
    let live = true, isArmed = false;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const off = () => {
      if (timer) { clearTimeout(timer); timer = undefined; }
      if (!isArmed) return;
      isArmed = false;
      disarm();
    };
    const on = () => {
      if (isArmed) return;
      isArmed = true;
      arm(kind).finally(() => {
        if (!live) return;
        setReadyFor(kind);
        if (isArmed) timer = setTimeout(off, METER_ARM_MS);
      });
    };
    // Mounted in the background (a headless launch): nothing will draw until it is foregrounded,
    // and coming to the foreground arms it.
    if (AppState.currentState === 'active' || AppState.currentState == null) on();
    else setReadyFor(kind);
    const sub = AppState.addEventListener('change', st => { if (st === 'active') on(); else off(); });
    return () => { live = false; off(); sub.remove(); };
  }, [kind]);
  return kind === 'bar' || readyFor === kind;
}

/** Tell the user, once, why their meter changed — and where to choose it again. */
export function explainMeterFallback(kind: string, why: 'crash' | 'fault'): void {
  // ★ The pane's own label, so the notice names the meter exactly as the button the user pressed.
  const name = METER_CHOICES.find(c => c.value === kind)?.label ?? 'signal';
  Alert.alert('Signal meter reset',
    (why === 'crash'
      ? `VibeSDR closed while the ${name} meter was starting, so the signal meter is back to BAR.`
      : `The ${name} meter hit a fault, so the signal meter is back to BAR.`)
    + ' You can choose it again from the cog menu: CONTROL CUSTOMISATION → SIGNAL METER.');
}

/** An in-session fault from a meter's frame callback (on the JS thread, via scheduleOnRN). */
export function reportMeterFault(kind: MeterKind, message: string): void {
  console.error(`[meter] ${kind} frame callback threw: ${message}`);
}
