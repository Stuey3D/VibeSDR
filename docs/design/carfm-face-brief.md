# CarFM — FM tuner face: design brief for Claude Design

Paste this whole file into Claude Design. It defines a single screen. Work in
**HTML/CSS** but stay inside the guardrails in §7 so the result ports cleanly to
**React Native** (this is a React Native / Expo app — the output is a *visual
spec*, not shipped code). When you're done, give me back the artifacts listed in
§8 so I can translate them into the existing `CarFmFace.tsx` component.

---

## 1. What this screen is
The permanent FM-radio face of an app that runs on an **Android car head unit**.
It tunes broadcast FM via an SDR dongle, decodes RDS, and plays through the car
audio. This is the only screen the driver looks at. It must be readable **at a
glance, in under a second, in a moving car, in sunlight**.

## 2. Canvas / device
- **Landscape**, head-unit aspect. Design at **1024 × 600** (also sanity-check
  1280 × 720). Not a phone.
- Assume touch only, gloved/imprecise taps, viewing distance ~60–80 cm.
- Dark cabin theme (the default). Also produce a **daylight/high-brightness**
  variant of the same layout.

## 3. Elements to lay out (all real, all present on screen at once)
1. **Frequency readout** — the hero. e.g. `101.1` with a smaller `MHz`. Tapping
   it opens a numpad (just show it as tappable).
2. **Station name (RDS PS)** — short, e.g. `BBC R2`. May be empty → show
   `Tuning…`.
3. **RadioText (RDS RT)** — a longer rolling line, e.g.
   `Now playing: Fleetwood Mac — Dreams`. May be empty → `Waiting for RadioText…`.
   Long text scrolls horizontally (ticker).
4. **Stereo / mono indicator** — a two-state badge.
5. **Signal strength** — a value in dB plus a visual meter.
6. **Transport controls** — four big buttons: previous preset, tune down, tune
   up, next preset. (Tune step label is `100 kHz` or `200 kHz`.)
7. **Presets** — a row of station chips (name + frequency); one may be the
   currently-playing one. Plus a **Save** action.
8. **Advanced** — a small, low-emphasis button that leaves this face for the full
   engineering UI. Must not compete for attention.
9. An **out-of-FM-band** warning state for the frequency (rare).

## 4. States you must show (make a variant board)
- Cold tune: PS empty (`Tuning…`), RT empty, mono, weak signal.
- Locked: PS + RT present, **stereo**, strong signal, one preset active.
- Long RT that needs to scroll.
- No presets saved yet (empty-state for the preset row).
Render these as separate frames so I can see each visual state.

## 5. Locked palette (do not invent new hues — these are the app's tokens)
Use these exact values; colour may only *reinforce* meaning, never carry it alone
(see §6).
- Background: `#05070A`  · Panel: `#0E141B` · Panel raised: `#151D28`
- **Frequency (amber, the one hot element): `#FFB833`**
- Primary text: `#F2F5F8` · Dimmed text/labels: `#8A94A2`
- **Active / selected / stereo (blue): `#3B9EFF`**
- Hairline border: `rgba(255,255,255,0.14)`
- Meter filled: `#FFB833` · Meter empty: `rgba(255,255,255,0.10)`
- Daylight variant: keep the same hues but raise background lightness and text
  contrast for sun readability; do **not** switch to a light theme.

## 6. Hard accessibility rules (non-negotiable — the user is red/green colourblind)
- **Never encode state with red vs green.** No red/green for tuned/untuned,
  signal, active preset, or stereo/mono.
- Every state must be legible without colour: use **position, shape, an icon,
  and a text label**. Colour is only a reinforcement.
  - Stereo/mono → different glyph (e.g. filled ◎ vs outline ○) **and** the word
    `STEREO`/`MONO`, blue tint when stereo.
  - Active preset → a leading filled marker (▶) **and** a heavier border/label,
    not just a colour swap.
  - Signal → meter **length/position** plus the **dB number**, not colour ramp.
- Minimum touch target **64 dp**; the four transport buttons ~72–76 dp tall.
- Aim for WCAG AA+ contrast on text against its background.

## 7. React-Native translatability guardrails (so this ports back losslessly)
Express everything in these terms — I'm mapping it to RN `View`/`Text`/`Pressable`
+ `StyleSheet`:
- **Layout: flexbox only.** No CSS grid, float, `position: sticky`, or table
  layout. `absolute` positioning is fine (RN supports it).
- **Sizes in `dp` (treat px == dp).** Give me concrete numbers: font sizes,
  paddings, gaps, border widths, corner radii, button heights.
- **Fonts:** system sans / the app already bundles *Atkinson Hyperlegible* —
  design with a legible sans; give a **type scale** (frequency, station, RT,
  labels, buttons) in dp with weights.
- Avoid what RN can't do: CSS `background` gradients are limited (prefer solid
  fills or call out a gradient explicitly as optional), no `::before/::after`
  pseudo-elements, no `box-shadow`-dependent meaning (use borders/fills),
  keep transitions simple (opacity/translate only — the RT ticker is a
  horizontal translate).
- The RT ticker = a single line translating horizontally on a loop; don't rely
  on CSS `marquee` or keyframe tricks I can't express as an animated translateX.

## 8. What to hand back to me (the integration deliverables)
1. The rendered screen(s) for the states in §4, dark + daylight.
2. A **layout spec**: the flexbox structure (rows/columns, flex ratios,
   alignment) as a short tree.
3. A **token table**: every colour (hex), font size + weight (dp), spacing/gap,
   border width, corner radius, button height actually used.
4. Per-element **state encodings** (how stereo, active preset, signal, out-of-band
   each look) in words, so I can reproduce them exactly.
5. Optionally **2–3 layout variants** of the hero/preset arrangement so we can
   choose.
Keep it as a spec I can read, not just an image — screenshots + the tables above.

## 9. Current implementation (match or beat this; here's the element inventory)
The RN component already renders, top→bottom: a top bar (`FM` badge · stereo pill ·
optional out-of-band pill · `ADVANCED` button); a centred hero (huge amber
frequency + `MHz`, then station name, then the RT ticker); a `SIGNAL` row (label ·
12-segment bar · `NN dB`); a transport row of 4 tall buttons (`⏮ PRESET`,
`− 100 kHz`, `＋ 100 kHz`, `⏭ PRESET`); a `PRESETS` header with a `＋ SAVE`
button; and a horizontally-scrolling row of preset chips (marker · name · freq,
active chip has a blue fill + heavier border). Treat this as the baseline to
refine — improve hierarchy, glanceability, and the daylight variant; keep every
element and every §6 rule.
```
