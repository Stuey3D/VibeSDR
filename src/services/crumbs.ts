/* ★★★ BOOT BREADCRUMBS — the JS half of a timeline whose other half is native.
 *
 *  Why a file and not a log: our NSLog is NOT readable off Stuart's device (three captures on
 *  2026-09-01 contained not one of our lines), and the unified-log archive is a ~20–45 second live
 *  buffer, so a LAUNCH — the only part that matters here — is exactly what you cannot catch. Six
 *  theories about the Buddy-driven black screen were built on no measurement at all, and one was
 *  drawn from the ABSENCE of a line that could never have appeared.
 *
 *  These go to the same `Documents/boot-crumbs.log` as the native crumbs, with the same clock, so
 *  the scene lifecycle and the React/connect sequence interleave in one readable order. That
 *  ordering IS the evidence: the open question is what happens between a scene connecting and a
 *  frame being drawn, and native is on one side of that gap and JS on the other.
 *
 * ★ Android has no such module and needs none — both open faults are iOS/Buddy-only. Absent
 *   module ⇒ no-op, never a throw: instrumentation that can break the app it is measuring is
 *   worse than no instrumentation.
 * ★ Never throws, never awaits. A crumb must not change the timing of the thing it is timing.
 */
import { NativeModules, Platform } from 'react-native';

const mod = NativeModules.VibePowerModule as { breadcrumb?: (l: string) => void;
                                               getCrumbs?: () => Promise<string>;
                                               clearCrumbs?: () => void } | undefined;

/** Milliseconds since the JS bundle was evaluated — the one clock native cannot see. */
const t0 = Date.now();

export function crumb(line: string): void {
  try { mod?.breadcrumb?.(`+${Date.now() - t0}ms ${line}`); } catch {}
}

/** Read the whole file back in-app (Diagnostics), for when the phone is to hand and the Mac is not. */
export async function readCrumbs(): Promise<string> {
  try { return (await mod?.getCrumbs?.()) ?? ''; } catch { return ''; }
}

export function clearCrumbs(): void {
  try { mod?.clearCrumbs?.(); } catch {}
}

/** True where the crumb file exists at all — used to hide the Diagnostics section elsewhere. */
export const crumbsAvailable = Platform.OS === 'ios' && !!mod?.breadcrumb;
