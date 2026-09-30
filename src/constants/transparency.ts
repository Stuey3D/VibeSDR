/**
 * transparency.ts — TRANSPARENCY EFFECTS: what a device gets before its owner has chosen.
 *
 * Stuart, 2026-09-30: "when app opened we detect low end hardware we default to transparency off.
 * So app looks the same as now just not see through, then user can customise afterwards."
 *
 * ★★★ ONE PURE FUNCTION DECIDES (`autoTransparency`), and ONE decides what is on screen
 *   (`effectiveTransparency`). FaceplateContext only GATHERS the signals and calls them; the
 *   rules are here so scripts/test_transparency.ts can drive every one of them.
 * ★★★ A STORED CHOICE ALWAYS WINS, and the auto default is NEVER SAVED AS IF CHOSEN: it is worked
 *   out afresh on every launch, so a restored backup on a newer phone is not left solid, and an OS
 *   Reduce Transparency switched off again brings the glass back — unless the user picked.
 *
 * ★★ WHAT THE APP CAN ACTUALLY SEE TODAY (no native dependency added — expo-device is not
 *   installed and nothing we ship exposes RAM or the model identifier to JS):
 *     • iOS Reduce Transparency — AccessibilityInfo (react-native core), live.
 *     • Android API level, iOS major version — Platform.Version.
 *     • iPhone vs iPad — Platform.isPad.
 *   `totalMemoryBytes` and `modelId` are rules WITHOUT A SOURCE yet: a native getter (iOS
 *   ProcessInfo.physicalMemory + the utsname machine VibeCrashLog already reads; Android
 *   ActivityManager.MemoryInfo.totalMem) would supply them. Until then they arrive null and decide
 *   nothing — the tests prove the rules so the getter is the only missing piece.
 *
 * Pure: no React, no imports — Node runs the test straight from this file.
 */

export type Transparency = 'on' | 'off';

export interface DeviceSignals {
  os:                  'ios' | 'android' | 'other';
  /** iOS: the major version (17 for 17.5). Android: the API level (29 = Android 10). */
  osVersion:           number | null;
  isPad:               boolean;
  isTV:                boolean;
  /** The OS accessibility setting (iOS only — Android has no equivalent; it arrives false). */
  reduceTransparency:  boolean;
  /** Physical RAM, bytes. ★ No source yet (see header) — null. */
  totalMemoryBytes:    number | null;
  /** Apple's model identifier, e.g. "iPhone11,8". ★ No source yet (see header) — null. */
  modelId:             string | null;
}

export type AutoReason = 'reduceTransparency' | 'lowMemory' | 'oldModel' | 'oldAndroid' | 'oldIos';

export interface AutoTransparency {
  transparency: Transparency;
  /** Why it is OFF (the first rule that fired), or null when ON. Shown under the pane's row. */
  reason:       AutoReason | null;
}

/** ≤ ~3 GB. Marketing "3 GB" phones report a little under 3 GiB (the kernel keeps some), so the
 *  line sits at 3.5 GiB: every 3 GB device is under it, every 4 GB device is over it. */
export const LOW_MEMORY_BYTES = 3.5 * 1024 ** 3;
/** Android 10. Below it: 2019-and-older phones, most with 2–3 GB and a weak GPU. */
export const MIN_ANDROID_API = 29;
/**
 * ★★ iOS 17 dropped every iPhone older than the XS / XR (A11: iPhone 8, 8 Plus, X). So an iPhone
 * on iOS 16 MAY be one of those — the only way to see Stuart's "older than roughly XS/XR" line
 * without a model identifier. An A12 phone left on 16 gets solid panels too; it can turn them on.
 * ★★ NOT APPLIED TO iPad: the Mac runs the iPad app, and the version an iOS app sees on a Mac is
 *   not an iPadOS version — a Mac on macOS 15 must not read as an old iPad. iPads wait for modelId.
 */
export const MIN_IPHONE_IOS = 17;

/** First iPhone / iPad / iPod generation (the number before the comma) that is A12 or newer. */
const A12_FIRST_GEN: Record<string, number> = { iPhone: 11, iPad: 8, iPod: 99 };

/** "iPhone10,6" → true (A11). Unknown or unparseable → false: an identifier we cannot read decides
 *  nothing (a new family, the simulator's "arm64"). */
export function isPreA12Model(modelId: string | null): boolean {
  if (!modelId) return false;
  const m = modelId.match(/^(iPhone|iPad|iPod)(\d+),\d+$/);
  if (!m) return false;
  return Number(m[2]) < A12_FIRST_GEN[m[1]];
}

/**
 * ★★★ THE DECISION. OFF if ANY signal says the device is small or old; ON otherwise. Order is the
 * order the reason is reported in — the OS setting first, because it is the user's own words.
 */
export function autoTransparency(d: DeviceSignals): AutoTransparency {
  const off = (reason: AutoReason): AutoTransparency => ({ transparency: 'off', reason });
  if (d.reduceTransparency) return off('reduceTransparency');
  if (typeof d.totalMemoryBytes === 'number' && d.totalMemoryBytes > 0 && d.totalMemoryBytes <= LOW_MEMORY_BYTES) {
    return off('lowMemory');
  }
  if (d.os === 'ios' && isPreA12Model(d.modelId)) return off('oldModel');
  if (d.os === 'android' && d.osVersion != null && d.osVersion > 0 && d.osVersion < MIN_ANDROID_API) {
    return off('oldAndroid');
  }
  if (d.os === 'ios' && !d.isPad && !d.isTV && d.osVersion != null && d.osVersion > 0 && d.osVersion < MIN_IPHONE_IOS) {
    return off('oldIos');
  }
  return { transparency: 'on', reason: null };
}

/** What is on screen: the user's choice if they made one, else the device's default. */
export function effectiveTransparency(
  s: { transparency: Transparency; transparencyExplicit: boolean }, auto: AutoTransparency,
): Transparency {
  return s.transparencyExplicit ? s.transparency : auto.transparency;
}

/** The pane's second line when the DEVICE switched it off (never shown once the user has chosen). */
export const AUTO_REASON_NOTE: Record<AutoReason, string> = {
  reduceTransparency: 'Off because Reduce Transparency is on in your device settings',
  lowMemory:          'Off automatically — this device has 3 GB of memory or less',
  oldModel:           'Off automatically for this model',
  oldAndroid:         'Off automatically for this Android version',
  oldIos:             'Off automatically for this iOS version',
};

/** iOS "17.5.1" / Android 34 → the number the rules compare. */
export function parseOsVersion(v: string | number | null | undefined): number | null {
  if (typeof v === 'number') return Number.isFinite(v) ? v : null;
  if (typeof v !== 'string') return null;
  const n = parseInt(v, 10);
  return Number.isFinite(n) ? n : null;
}
