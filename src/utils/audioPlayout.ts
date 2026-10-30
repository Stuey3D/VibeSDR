/* audioPlayout.ts — ★★ V12 2026-10-30: how far behind its arrival the audio is being HEARD, ms (−1 = unknown / nothing
 * playing). The waterfall waits for the sound by it (src/assets/wfAvSync.ts). Both platforms expose the getter on
 * "VibePowerModule" (iOS VibePowerModule.swift audioPlayoutMs; Android VibeStreamModule.kt), synchronous and cheap.
 * A native build without it answers −1 — no hold, the waterfall exactly as it was. */
import { NativeModules } from 'react-native';

export function audioPlayoutMs(): number {
  const f = (NativeModules.VibePowerModule as { audioPlayoutMs?: () => number } | undefined)?.audioPlayoutMs;
  if (typeof f !== 'function') return -1;
  try {
    const v = f();
    return typeof v === 'number' && isFinite(v) ? v : -1;
  } catch { return -1; }
}
