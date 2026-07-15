# CarFM design workflow (Claude Design → React Native)

The CarFM FM face is designed visually in **Claude Design** (or plain Artifacts)
and then hand-translated into the React Native component. Claude Design emits
web UI (HTML/CSS); this app is React Native, so its output is treated as a
**visual spec, not shippable code**.

## Files
- [`carfm-face-brief.md`](./carfm-face-brief.md) — the brief to paste into Claude
  Design. It pins the real data model, the app's palette tokens
  (`src/contexts/ThemeContext.tsx`), the accessibility rules, and — importantly —
  React-Native-translatability guardrails so the design ports back cleanly.

## The round-trip
1. Paste `carfm-face-brief.md` into Claude Design. If it can read this repo,
   point it at `src/contexts/ThemeContext.tsx` and
   `src/components/CarFmFace.tsx` so it extracts tokens directly.
2. Iterate on the look (dark + daylight variants, the required states).
3. Bring back the deliverables in the brief's §8: rendered states, the flexbox
   **layout tree**, a **token table** (hex / dp sizes / weights / spacing), and
   the per-state **visual encodings** — as a readable spec, not just images.
4. Fold that into [`src/components/CarFmFace.tsx`](../../src/components/CarFmFace.tsx).
   Because it's all in dp + flexbox terms, the translation is close to mechanical.
5. Verify in the emulator on the head-unit aspect (~1024×600), then on the DUDU7.

## Why the guardrails matter
React Native isn't the DOM: flexbox only (no CSS grid/float), `StyleSheet` not
cascading CSS, `View`/`Text`/`Pressable` primitives, sizes in density-independent
pixels, limited gradients/shadows, and animations expressed as opacity/translate.
The brief steers Claude Design away from web-only patterns so nothing designed is
un-buildable here.

## Accessibility is a hard constraint, not a nicety
The primary user is red/green colourblind and the screen is used while driving:
no state may be encoded by red-vs-green, every state must read via shape +
position + label (colour only reinforces), high contrast, big touch targets,
glanceable in under a second. See the brief's §6.
