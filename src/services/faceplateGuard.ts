/**
 * faceplateGuard — the app's one faceplate crash guard, wired to the disk, the clock and AppState.
 * The rules, and why, are in src/constants/faceplate.ts CRASH SAFETY (and its test).
 *
 * ★★★ THE MARK IS A FILE, WRITTEN SYNCHRONOUSLY (expo-file-system `File.write`, a synchronous native
 *   call): it is on disk before the line after `arm()` runs, so a faceplate that aborts the app on its
 *   very first frame is already recorded. AsyncStorage cannot promise that — its write lands on a
 *   native queue some time later, possibly after the abort.
 * ★ Every disk call is inside makeFaceplateGuard's try: a guard that cannot write must never be the
 *   thing that breaks the display.
 */

import { Alert } from 'react-native';
import { File, Paths } from 'expo-file-system';
import { makeFaceplateGuard, METER_CHOICES, type FaceplateGuard } from '../constants/faceplate';
import type { MeterKind } from '../constants/meters';

const MARK_NAME = 'faceplate-armed.json';
/** What the user had chosen when the app died with it — kept, never applied automatically. */
export const LAST_CRASHED_KEY = 'lsv_faceplate_last_crashed';

const markFile = () => new File(Paths.document, MARK_NAME);

export const faceplateGuard: FaceplateGuard = makeFaceplateGuard({
  write: (m) => { markFile().write(m); },
  read: () => { const f = markFile(); return f.exists ? f.textSync() : null; },
  remove: () => { const f = markFile(); if (f.exists) f.delete(); },
  now: () => Date.now(),
  setTimer: (fn, ms) => setTimeout(fn, ms),
  clearTimer: (t) => clearTimeout(t as ReturnType<typeof setTimeout>),
});

let launchMark: string | null | undefined;
/** The previous run's mark — read (and cleared) ONCE per process, before anything can arm. */
export function launchMarkOnce(): string | null {
  if (launchMark === undefined) launchMark = faceplateGuard.takeLaunchMark();
  return launchMark;
}

/** The one-line notice after a launch that fell back to the safe faceplate. */
export function explainFaceplateReset(): void {
  Alert.alert('Display settings reset',
    'Your display settings were reset after a crash. Choose them again from the cog menu: CONTROL CUSTOMISATION.');
}

/** A meter's frame callback caught its own throw (in session): the housing puts the BAR back. */
export function explainMeterFault(kind: MeterKind, message: string): void {
  console.error(`[meter] ${kind} frame callback threw: ${message}`);
  // ★ The pane's own label, so the notice names the meter exactly as the button the user pressed.
  const name = METER_CHOICES.find(c => c.value === kind)?.label ?? 'signal';
  Alert.alert('Signal meter reset',
    `The ${name} meter hit a fault, so the signal meter is back to BAR. You can choose it again from the cog menu: CONTROL CUSTOMISATION → SIGNAL METER.`);
}
