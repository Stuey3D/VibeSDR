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
 * ★★ WHERE THE SIGNALS COME FROM (no dependency added — expo-device / react-native-device-info are
 *   deliberately not installed):
 *     • iOS Reduce Transparency — AccessibilityInfo (react-native core), live.
 *     • Android API level, iOS major version — Platform.Version.
 *     • iPhone vs iPad — Platform.isPad.
 *     • RAM, model identifier, "this is a Mac" — ONE synchronous native getter,
 *       `NativeModules.VibeLocalSDR.deviceClass()` (iOS ProcessInfo.physicalMemory + utsname
 *       machine, the identifier VibeCrashLog reports; Android ActivityManager.MemoryInfo.totalMem +
 *       MANUFACTURER MODEL), read by src/services/deviceClass.ts and checked by `parseDeviceClass`
 *       below. An old binary, Expo Go, web and tests have no getter: those fields arrive null /
 *       false and decide nothing, exactly as before the getter existed.
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
  /** Physical RAM, bytes — from the native getter (header); null when there is none. */
  totalMemoryBytes:    number | null;
  /** iOS: Apple's model identifier, e.g. "iPhone11,8". Android: "MANUFACTURER MODEL" (read by no
   *  rule — Android is judged on memory and API level). Null when there is no getter. */
  modelId:             string | null;
  /**
   * ★★ The iOS app running on a Mac (Apple silicon "Designed for iPad", or Catalyst). A Mac is never
   * low-end here: only the user's own Reduce Transparency can turn its glass off — no memory, model or
   * version rule applies (the version it reports is not an iOS one, its identifier is not an iPhone's).
   */
  isMac:               boolean;
}

/** What the native `deviceClass()` getter hands back, checked. Anything malformed → that field
 *  decides nothing (null / false); a missing getter (undefined) → all three decide nothing. */
export function parseDeviceClass(raw: unknown):
    Pick<DeviceSignals, 'totalMemoryBytes' | 'modelId' | 'isMac'> {
  const o = (raw && typeof raw === 'object' ? raw : {}) as Record<string, unknown>;
  const mem = o.totalMemoryBytes;
  const model = o.model;
  return {
    totalMemoryBytes: typeof mem === 'number' && Number.isFinite(mem) && mem > 0 ? mem : null,
    modelId: typeof model === 'string' && model.trim() ? model.trim() : null,
    isMac: o.isMac === true,
  };
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
  if (d.isMac) return { transparency: 'on', reason: null };   // ★★ never downgraded (see isMac)
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
