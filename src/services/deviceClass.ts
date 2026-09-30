/**
 * deviceClass.ts — the device's RAM, model identifier and "is this a Mac", for Transparency
 * effects' low-end default (src/constants/transparency.ts).
 *
 * The native half is ONE synchronous method on the existing VibeLocalSDR module
 * (modules/vibe-local-sdr/VibeLocalSDR.mm, android/.../VibeLocalSdrModule.kt) — no new module, no
 * new dependency (expo-device / react-native-device-info are deliberately not installed).
 *
 * ★ Guarded like transliterator.ts: an old binary without the method, Expo Go, web and tests have
 *   no `VibeLocalSDR.deviceClass`, and a sync call a bridge refuses throws — every one of those
 *   arrives as `undefined`, silently, and parseDeviceClass() turns that into "decides nothing".
 * ★ Read ONCE per process: RAM and model do not change while the app runs.
 */
import { NativeModules, Platform } from 'react-native';

let cached: { v: unknown } | null = null;

export function readNativeDeviceClass(): unknown {
  if (cached) return cached.v;
  let v: unknown;
  if (Platform.OS !== 'web') {
    const fn = (NativeModules as { VibeLocalSDR?: { deviceClass?: () => unknown } })
      .VibeLocalSDR?.deviceClass;
    if (typeof fn === 'function') {
      try { v = fn(); } catch { v = undefined; }
    }
  }
  cached = { v };
  return v;
}
