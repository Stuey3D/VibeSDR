/**
 * transliterator.ts — installs the native ICU `Any-Latin; Latin-ASCII` transform as the faceplate
 * displays' transliterator (docs/BRIEF-faceplates.md §7 "Character folding").
 *
 * The native half is ONE synchronous method on the existing VibeLocalSDR module
 * (modules/vibe-local-sdr/VibeLocalSDR.mm, android/.../VibeLocalSdrModule.kt) — no new module, no
 * new pods. src/constants/displayText.ts sends it only the non-Latin runs and memoises every answer.
 *
 * ★ Guarded: an old binary without the method, Expo Go, web and tests have no
 *   `VibeLocalSDR.transliterate`, so nothing is installed and non-Latin names keep falling back to
 *   the frequency + Latin callsign, exactly as before.
 * ★ If the first call throws (a binary whose bridge refuses a sync call), the transliterator
 *   uninstalls itself rather than throwing on every name.
 */
import { NativeModules, Platform } from 'react-native';
import { setTransliterator } from '../constants/displayText';

let installed = false;

export function installNativeTransliterator(): void {
  if (installed || Platform.OS === 'web') return;
  installed = true;
  const fn = (NativeModules as { VibeLocalSDR?: { transliterate?: (s: string) => unknown } })
    .VibeLocalSDR?.transliterate;
  if (typeof fn !== 'function') return;
  let probed = false;
  setTransliterator((s: string) => {
    try {
      const r = fn(s);
      probed = true;
      return typeof r === 'string' ? r : s;
    } catch (e) {
      if (!probed) setTransliterator(null);   // it never worked here: stop asking
      return s;
    }
  });
}
