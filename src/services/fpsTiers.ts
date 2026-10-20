/** Waterfall frame-rate tiers for a serving phone — pure (no React Native), so scripts/test_fps_tiers.ts can
 *  prove the labels and the stored-key mapping. Re-exported by vibeServer.ts. */
// Waterfall frame-rate tiers. ★★ 10 fps IS THE DEFAULT (Stuart, 2026-10-20 — like OWRX and UberSDR's
// shipping default); 20 is the owner's option. Listeners' waterfalls scroll at the same steady speed on
// any of them (repeated real rows), so the lower rate costs detail in time, not smoothness.
// ★★★ THE KEYS ARE STORED and stay as they were: 'full' / 'half' / 'quarter' are what an owner's saved
//     settings hold, and they still mean 20 / 10 / 5. Only the LABELS changed — "Full / Half / Quarter"
//     stopped making sense once 10 was the default:
//       'full'    → "Smoother · 20 fps"   (a saved 'full' still serves 20)
//       'half'    → "Default · 10 fps"    (what a new server starts on)
//       'quarter' → "Half · 5 fps"
export type FpsTier = 'full' | 'half' | 'quarter';
export const FPS_TIERS: { key: FpsTier; label: string; fps: number }[] = [
  { key: 'full',    label: 'Smoother · 20 fps', fps: 20 },
  { key: 'half',    label: 'Default · 10 fps',  fps: 10 },
  { key: 'quarter', label: 'Half · 5 fps',      fps: 5  },
];
/** The tier a NEW server starts on (nothing saved). */
export const DEFAULT_FPS_TIER: FpsTier = 'half';
export const fpsForTier = (t: FpsTier) => FPS_TIERS.find(x => x.key === t)?.fps ?? 10;
