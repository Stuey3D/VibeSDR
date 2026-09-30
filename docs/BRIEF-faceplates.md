# BRIEF — Faceplates: chassis finishes, colours, display styles, meters, keys

**Status:** designed 2026-09-30 (Stuart + Claude), mockups approved, ready to build.
**Scope:** purely visual. No protocol, server or DSP changes. The watch and web client are out of scope.

> **Where the files live in the repo** (moved in from `BRIEF-faceplates.zip`, 2026-09-30). Paths below that say
> `assets/faceplates/…` for REFERENCE material (renders, mockups, GIFs, `wf-backdrop.jpg`, the `.woff2`) are in
> **`docs/faceplates/…`** — they are specs, never bundled. Only what ships is under `assets/`:
> `assets/faceplates/silver-brushed.jpg`, `assets/faceplates/black-brushed.jpg`,
> `assets/fonts/DSEG14Classic-BoldItalic.ttf` (+ `DSEG-LICENSE.txt`) and `assets/fonts/Doto-Black.ttf`
> (Doto 900, OFL, from `@expo-google-fonts/doto` 0.4.1; licence `Doto-OFL.txt`).

## The owner's words

> "I don't want colour changes to look like a palette swap on a piece of software, I want full
> skeuomorphism and the feeling that the user has switched a piece of hardware in a rack somewhere."

> "I want to feel like I can reach out and my screen will physically click in when pressing the buttons."

> "Nixie is warm analog ancient orange like the real thing. It's the only thing I will not budge on."

★★★ **BUILD IT EXACTLY AS THE MOCKUPS LOOK.** The renders are the spec, not a mood board. Every colour,
gradient stop and size here comes from the mockup source `assets/faceplates/Deck.mockup.dc.html`
(plain HTML/CSS; its `renderVals()` is the token logic). Where this brief and the mockup disagree,
**the mockup wins**; note it in the PR.

**Read first:** `assets/faceplates/annotated/A1…A4-*.jpg`. They label every part of the deck, the
landscape deck, the analogue meter and a decoder box, with the section of this brief that covers it.

---

## 0. Build order

Do these as separate commits, in this order. Each one ships on its own.

| # | Work | § |
|---|---|---|
| 0 | **Fix today's landscape key heights** (bug, independent of faceplates) | 11 |
| 1 | **Unify the decoder boxes** into one `DecoderShell` (today's drift) | 10.1 |
| 2 | Tokens + theme resolver; move every hard-coded colour out of `ControlsBar` / `DrumWheel`; migrate the font setting into Display | 1, 2, 12 |
| 3 | Chassis (silver, black) and dome keys | 3, 5 |
| 4 | Display styles + VTS + status display | 7, 8 |
| 5 | Meters: LED VU, then analogue | 4 |
| 6 | Drum wells + tuner-keys mode | 6 |
| 7 | Landscape | 9 |
| 8 | Decoder boxes (Transparent / Solid → since 2026-09-30 the app-wide TRANSPARENCY EFFECTS, §10.2) | 10 |
| 9 | Small-screen status row | 8.2 |
| 10 | Popups, menus and chat take the chassis (`PopupShell`) | 10.3 |

---

## 1. Settings

★★★ **Everything lives in a new `CONTROL CUSTOMISATION` pane** (Stuart's name). It opens from the cog menu
the way DISPLAY SETTINGS does: a pane that replaces the menu content, with a ‹ back row (ref `popups/P1`,
Control Customisation column). Today's **CONTROLS** section in `MenuSheet` becomes a single key,
`CONTROL CUSTOMISATION ›`. Everything controls-related moves into the pane, in three groups:

| Group | Rows (in this order) | Today |
|---|---|---|
| **FACEPLATE** | CHASSIS · DISPLAY · CONTROLS colour · TEXT colour · SIGNAL METER · TRANSPARENCY EFFECTS (On/Off) | new (this brief) |
| **TUNING & ZOOM** | TUNE drum/keys · ZOOM drum/keys · DRUM FEEL normal/precise · WHEEL zoom/tune · MEDIA ⏮⏭ tune step/bookmark | moved from CONTROLS |
| **FEEL** | HAPTICS off/on · STEADY LEDS off/on (§4.4) | haptics was a key on the DRUMS row |

- Every row is an input-selector row (§10.3): dome keys with LED pips. Colour rows show each colour as a lit
  LED dot. TEXT shows only the colours the current Display allows (§1 table). Under Nixie it's a note instead:
  *"Locked to neon by the Nixie display"*.
- HAPTICS becomes its own row, not a key squeezed onto the drums row, because it now covers every dome key (§5),
  not just the drums. Hide the row when `hapticsHardware` is false, as today.
- DRUM FEEL is today's DRUMS NORMAL/PRECISE, relabelled because the pane has several drum rows.
- Colour Map, VFO colour and the signal-meter **unit** (S-units/dB) stay in DISPLAY SETTINGS: they're display
  choices, not the faceplate.
- ★★ **TRAP (AGENTS.md: when a control moves, fix the copy that says where it is).**
  - `sdrTour` card `schemes` ("Pick your control scheme") in `SDRScreen.tsx` says Tune and Zoom drum/keys are *"under CONTROLS in the
    settings cog"*. Change it to *"under Control Customisation in the settings cog"*.
  - Grep the rest of the AGENTS.md list for "CONTROLS", "HAPTICS", "PRECISE", "WHEEL": `pickerTour`,
    `AboutOverlay`, the watch `TutorialSheet`s, `website/index.html`. When this brief was written they
    didn't give a menu location for these, but check the whole sentence.
  - The `MenuSheet` comment *"DISPLAY STYLE row removed — accessibility skin … amber/Nixie dropped"* is now
    wrong; Display is back, in this pane. Update it.

| Setting | Values | Default |
|---|---|---|
| **Chassis** | `default` (today's deck), `silver` (brushed silver), `black` (brushed black) | `default` |
| **Display** | `hyper` (Atkinson Hyperlegible), `nixie` (tubes), `dot` (dot-matrix VFD), `seg` (80s VCR) | the user's current font (§1 migration) |
| **Controls colour** | `green`, `red`, `amber`, `blue`, `white`, `neon` | `green` |
| **Text colour** | depends on Display (table below) | first allowed |
| **Signal meter** | `bar` (today's), `vu` ("LED VU"), `edge` ("Analogue") | `bar` |
| **Transparency effects** (was *Decoder background*) | `on`, `off` | `on`, or `off` on a low-end device until the user chooses (§10.2) |

★★★ **Two colours, not one.**
- **Controls colour** lights the key legends, the drum wells (window edges, seam glow, icon, ±,
  outer glow), tuner keys, and decoder accents.
- **Text colour** lights the frequency, mode box, VTS strip, SHARED TUNER banner and status display.

★★★ **Text colours are limited to what the real display technology came in.**

| Display | Allowed text colours | Notes |
|---|---|---|
| `nixie` | none: **locked neon** (§2) | text picker greyed: "Locked to neon by Nixie display" |
| `dot`, `seg` | `teal` (default), `green`, `blue`, `amber`, `red` | real VFD/LED colours. **Never white**: they never came in white |
| `hyper` | `green`, `red`, `amber`, `blue`, `white`, `teal` | accessibility font, so anything goes |

- Changing Display to one where the current text colour isn't allowed falls back to that display's
  first allowed colour. Remember the user's choice per display if cheap to do.
- Choosing `nixie` switches **controls to `neon`** so drums and keys match the tubes;
  the user can change it afterwards. Leaving Nixie while controls are `neon` resets them to `amber`.

Colour tokens (rgb / core / hot):

| name | rgb | core | hot |
|---|---|---|---|
| green | 61,255,114 | `#3dff72` | `#c9ffd6` |
| red | 255,58,46 | `#ff3a2e` | `#ffcbc6` |
| amber | 255,174,26 | `#ffae1a` | `#ffe4b3` |
| blue | 61,155,255 | `#3d9bff` | `#d0e7ff` |
| white | 215,228,255 | `#eef3ff` | `#ffffff` |
| teal (text only) | 70,255,215 | `#46ffd7` | `#c8fff2` |
| neon (controls only) | 255,106,20 | `#ff7a26` | `#ffc48a` |

Glow = core at α .70–.80; dims derive from the rgb triplet.

★★ Display **replaces today's font setting** (`ThemeContext`: Nixie One / Atkinson); don't add a second
font switch. Migration: Atkinson → `hyper`; Nixie One → `nixie`.
★★★ **There is no tubeless Nixie option.** Nixie means real tubes for the frequency, full stop ("nixies
are in tubes, so ours should be"). Nixie One the *font* still carries the mode box's neighbours
(VTS, banner, status display) in `nixie` mode; only the frequency is tubes. Per AGENTS.md, grep the tour, `AboutOverlay` and
the website for the old setting's location when it moves.

Reference: `colours/portrait/01…12-*.jpg` and `colours/landscape/01…12-*.jpg` show **every allowed
colour for every chassis × display**. Nixie sheets vary the controls colour (text is locked); the
others vary the text colour.

---

## 2. ★★★ The Nixie rule (non-negotiable)

**Anything drawn in Nixie One is neon orange. Always. The text colour setting is ignored.**

```
NEON_CORE  = #ffc48a
NEON_GLOW  = 0 0 1.5px #ff8a2e, 0 0 5px #ff6410, 0 0 11px rgba(255,80,10,0.6)
mode "AM"  = #ffb37a      S-reading = #ff9a55, glow 0 0 4px rgba(255,90,10,0.8)
```

- Applies with `nixie` to the whole text role: frequency, mode box, VTS, banner, status display.
- Default chassis with neon controls: all four key legends go neon too ("1k" and the icons, glow
  `drop-shadow(0 0 2.5px #ff6410) drop-shadow(0 0 6px rgba(255,80,10,0.45))`); otherwise they
  stay today's white.
- Enforce it in **one resolver**: `resolveTextColour(display, textColour)` returns neon for Nixie One,
  otherwise the text colour clamped to the display's allowed set. Components never read colour
  tokens directly.
- ★ **TRAP:** Nixie One is also used outside the deck in gold `#ffb833` (server menu, `FreqModal`
  hints, `SDRScreen`). Move those to neon or to Atkinson in the same PR, so the app doesn't
  contradict its own rule one tap away.
- ★ **TRAP:** Nixie One must never render white. On the default chassis with `dot`/`seg`, the "1k"
  label uses **Atkinson**, not Nixie One.

---

## 3. Chassis

### 3.1 `default`: today's deck, unchanged
Must match **today's app pixel for pixel** in its default settings (`all-states/02-default-hyper.jpg`
against a live screenshot). Controls colour drives the drum wells (as `GLOW_HUE` does now). Key
legends `#f1ede4` (pressed `#ffffff`); digits `#f4f1ea`, glow `0 0 6px rgba(255,255,255,.30)`,
except under the Nixie rule. Footer is today's two-line footer, including GAIN and IF.

### 3.2 `silver`: brushed silver (refs: Sony HCD-SE1, Panasonic stacking hi-fi)
- Plate `#c9c6bf`, border `1px #8b8983`, radius 16, shadow
  `inset 0 1px 0 rgba(255,255,255,.95), inset 0 2px 1px rgba(255,255,255,.35), inset 0 -2px 0 rgba(0,0,0,.20), 0 3px 10px rgba(0,0,0,.6)`.
- Texture `silver-brushed.jpg` (champagne tint, horizontal grain), shown at 620 pt wide.
- Lighting layer (Skia, separate from the texture):
  `linear 104°: rgba(255,255,255,0) 0, .10 18%, .34 36%, .08 52%, rgba(0,0,0,.06) 74%, .16 100%`
  + `radial 140%×70% at 28% −10%: rgba(255,250,240,.22) → 0 at 60%` + bottom 30% darkening to .10.
- Four corner screws, 9 pt, radial `#ffffff → #a7abb0 60% → #6d7176`, slots at different angles.
- Black glass windows for the frequency, meter and status display.

### 3.3 `black`: brushed black (ref: Stuart's own Yamaha RX-V583)
- Plate `#1b1c1e`, border `1px #3a3c40`, radius 16, top lip `inset 0 1px 0 rgba(255,255,255,.22)`.
  ★ Darkened a touch after build 356 (Stuart: "can be darkened ever so slightly"): plate `#161719`, a .14
  black veil over the grain, the lighting's white bands at ~.8, caps `#18191b` (`BLACK_CHASSIS`).
- Texture `black-brushed.jpg`; lighting as silver at about a third of the alpha.
- **Gloss acrylic display panel** behind banner, frequency and meter: `#111214 → #060607 55% → #0b0b0d`,
  hard diagonal reflection at 112° (38–56%, α .10 → .035), aluminium trim line beneath
  (`0 1px 0 #8e9196, 0 2px 0 #3b3d41, 0 3px 3px rgba(0,0,0,.6)`).
- **No screws** (the RX-V has none).

### 3.4 Performance
- ★★ Silver and black are **opaque**: no `BlurView` behind them (today's glass deck blurs the spectrum
  on iOS, the expensive case). Mark the plate opaque.
- Draw the plate (texture, lighting, screws, bezels) **once into a cached layer**. Everything that
  changes (meter, needles, digits, key presses) goes in layers above it.
- ★ **TRAP:** never bake lighting into the texture image; it must stay correct in landscape and on
  tablets. Brushed grain moirés when downsampled, so use mipmaps/linear filtering and test on Android.

---

## 4. Signal meters

### 4.1 ★★★ One deck height
"Make the deck one size so it's not growing or shrinking." Changing meter type or joining a shared
server **never changes the deck height**. The mockup measures identical heights for every meter ×
shared combination (scale 1: default 310, silver 313, black 325).

Build it as **a fixed deck height with the display area flexing inside it**, not per-state magic
numbers.

Portrait sizes (scale 1):

| | bar | LED VU | analogue |
|---|---|---|---|
| frequency window | today's (in the 72 pt bar) | 48 | 46 |
| same, shared VFO | today's (banner inside bar) | **38** | **35** |
| keys (slot / cap) | 58 / 54 | 44 / 40 | 44 / 40 |
| keys, shared VFO | 58 / 54 | **34 / 30**, legends 80% | **34 / 30** |
| SHARED TUNER banner | inside the bar, as today | 18 pt above the frequency, 5 pt gap | same |
| digits / tubes | today's | 32 / 22 × 36 | same; shared: 27 / 18 × 29 |

★ **TRAP:** 34 pt keys are below 44 pt. Extend `hitSlop` into the gaps.

### 4.2 `bar`: today's meter, unchanged
Squelch line as today (white halo + red line); the bar dims to α .38 while squelch mutes.

### 4.3 `vu`: ten rectangular 1980s LEDs
- 5 green / 3 orange / 2 red, labels `S1 S3 S5 S7 S9 +10 +20 +30 +40 +60` (S9 = top of green).
  **LED colours never follow either colour setting.**
- LED 13 pt high, radius 1.5, gap 4, in a black housing (`#030303 → #0b0b0b`, inset shadow).
  - **Lit:** matte top `rgba(255,255,255,.20) 0 → .07 30% → 0 55% → rgba(0,0,0,.10)` over
    `radial ellipse 78%×60% at 50% 54%: hot 0 → hi 24% → base 60% → dark 100%`; seat ring
    `0 0 0 1px rgba(0,0,0,.9)`; glow 7 + 14 pt; inner vignette `inset 0 0 2.5px rgba(0,0,0,.45)`.
  - **Unlit:** dead tinted plastic, the matte layer at a third over `radial offCentre → offEdge`.
  - hot / hi / base / dark / offCentre / offEdge:
    green `#e9ffe9 #7dff9c #22d24e #0c7a26 #1d3a23 #0b170e` ·
    orange `#fff3dc #ffc36b #ff8a12 #a34a05 #3d2811 #170f06` ·
    red `#fff0ee #ff8a80 #f2231a #8d0c07 #3e1613 #180807`.
- **Peak hold:** one segment above the level, full brightness, drops after ~1 s.
- **Squelch** (ref `07-squelch.jpg`): a **ring** (1.5 pt outline, 2 pt offset) on the threshold
  segment: **green `#3dff72` open, red `#ff3a2e` closed**, always full strength. While muting, lit
  LEDs dim (55% black overlay, glow cut to 4 pt at α .22). Squelch off (`-1`): no ring.
  ★ **TRAP:** place the ring with the same S-unit table the segments use, or it sits one LED off
  from where audio really opens.
- ★ Pre-render the six LED sprites (lit/unlit × 3 colours) once at device pixel ratio. Never draw
  ten live `BlurMask`s per meter update (Xcover 4S / Pi-class hosts).

### 4.4 ★★★ The edge LED: partial brightness, the real way
A real LM3915-style meter switches each LED fully on or off. The half-lit edge LED is the eye
averaging a signal that crosses the threshold faster than it can see: **brightness = fraction of time
above threshold.** Emulate that statistically (our meter updates at only 5–25 fps):

```
μ = smoothed level (dB)    σ = running std-dev over ~0.5 s (dB), floor 1.5
brightness(T) = Φ((μ − T) / σ)       Φ = normal CDF
```

Fading HF gives a soft, wide edge; a steady FM carrier a crisp one. Scale opacity and glow together
and cross-fade lit and unlit sprites, so the plastic stays visible under a dim LED.

★★★ **No flicker, ever** (Stuart is sensitive to PWM flicker):
- Never toggle a segment to fake a duty cycle. Every frame draws a steady brightness.
- One smoothing allowed: an **eye filter**, exponential, **τ ≈ 100 ms**, plus a limit of ~0.35 change per
  frame. At 5 fps on fading HF the edge LED must glide, not pulse.
- **"Steady LEDs"** accessibility option (on automatically with iOS Reduce Motion / Android "Remove
  animations"): the edge LED is solid on/off with ~1 dB hysteresis.
- While squelch mutes, draw the dim level only (no σ shimmer under the red ring).
- ★ **TRAP:** no fixed linear ramp between thresholds; a steady carrier would sit half-lit for ever.
- ★ **TRAP:** the eye filter is the *only* easing. Longer easing makes the meter lag and look like
  software.

### 4.5 `edge`: analogue edgewise meter (ref `all-states/E1…E4`, `annotated/A3`)
The slim moving-coil meter from cassette decks: scale on a curved drum, needle moving sideways.
**It takes the LED strip's space** (34 pt housing, 28 pt window).

- **Simple and vintage:** scale card `#e9e2cf`, black print `#16120d`, **small red zone +30 to +60**
  (ticks, labels and a 2.8 pt band from half a division before +30). "SIGNAL" top right. No colour
  setting touches it, on any chassis.
- **Incandescent lighting, deliberately uneven:** two bulbs, left brighter (`rgba(255,214,150,.55)`
  at 27% / 62%), right weaker (`.32` at 77% / 58%), shadowed corners, a dip between. Then drum
  curvature (top/bottom roll-off to α .62 / .70), 16 pt side falloff, 1 pt glass streak.
  ★ **TRAP:** don't even the lighting out; the unevenness is the style.
- Scale points at `8 + (i + 0.5) × (width − 16) / 10`, two minor ticks between.
- **Three hands:**
  1. **Signal needle:** 2.6 pt, near-black, pointed tip, soft shadow to its right, **rises from the
     bottom** with an 11 × 7 arrowhead at the lower edge.
  2. **Peak-decay needle:** 1.6 pt, translucent dark brown-grey (α .62), pointed tip, **rises from
     the bottom, no arrowhead**, stops just short of the top like the signal needle.
  3. **Red squelch hand:** 1.4 pt `#d0140a`, small arrowhead, **hangs from the top**, like the red
     alarm hand on a clock, parked at the threshold.
- **Signal ballistics:** real VU movement, critically damped, 99% in **300 ms**, ~1% overshoot
  (`withSpring` on the UI thread). ★ **TRAP:** drive it with the **raw** level; smoothing first and
  then springing doubles the lag.
- **Peak needle motion: the signal needle pushes it.** `peak = max(peak, signalNeedleAnimatedPos)`:
  the needle's *on-screen* position, never the raw level, so it can't jump ahead. When the signal
  falls away it **holds ~1 s**, then **drifts down ~6 dB/s**, easing in, until caught again. Both
  needles animate on the same UI thread, or a gap opens between them.
- **Squelch closed:** lamp dims 50%, needles fall (peak at its slow rate), red hand stays, SQL shows in the mode box.
- Reduce Motion: keep the needles moving (they are the reading) but remove the overshoot.

### 4.6 SQL in the mode box (all meters)
As today: the S-reading becomes a breathing "SQL" (`ControlsBar` `sqlClosed`), red `#ff4040`; on
`nixie` it's neon `#ff9a55` (the Nixie rule outranks red). The breathing stays slow (~1 s).

---

## 5. Keys: surface-mount tactile dome switches

> "Surface mount clicky buttons with no full push-in state … the metal dome underneath that pushes down and clicks."

★★★ **One key type on every chassis: a flat cap over a snap dome.** No long-travel keys. Default keeps
today's look at rest.
- Cap fills its recessed slot, inset 2 pt. Rest: top +1.5, cast `0 1.5px 1px rgba(0,0,0,.85)`,
  chamfer `inset 0 1px 0 rgba(255,255,255, hi×0.3)` (hi: default .10, silver .95, black .22),
  rim `inset 0 0 0 1px rgba(255,255,255,.07)`.
- **Clicked:** top +3.5 (2 pt travel), cast shadow gone, `inset 0 1px 3px rgba(0,0,0,.65)`,
  brightness .84 (default/silver) / .82 (black), legend glow 4 → 7 pt.
- ★★ **A snap, not a slide.** Press: 45 ms, `cubic-bezier(0.9, 0, 1, 0.6)` (resists, then collapses).
  Release: 35 ms, `cubic-bezier(0.2, 0.9, 0.3, 1.4)` (springs back). Reanimated `withTiming`, UI thread.
- Silver caps: brushed silver, legend in the controls colour with a dark shadow. Black caps: brushed black.
- ★★★ Depress, haptic and legend flare fire on **`onPressIn`**; the action fires on release.
- **Haptics:** a click on press **and** release. Press: `.rigid`, full, fired at the **end** of the
  45 ms curve (the snap). Release: `.light` ~0.5. iOS: a Core Haptics transient (sharpness ≈ 1);
  Android: `KEYBOARD_TAP` / `KEYBOARD_RELEASE`, never raw `vibrate()`.
- ★ **TRAP:** the 45 ms haptic delay is deliberate; don't "fix" it to touch-down. Check the timer
  isn't late under load on the Xcover 4S.
- ★★★ **Every dome key clicks, on every chassis, including `default`** (Stuart: "make sure the main
  buttons have haptics too"). That means:
  - the **four main keys** (1k, speaker, cog, chat) in portrait **and** landscape. `ControlsBar.tsx` has
    **no haptics on them today**; only `TunerKeys.tsx` and `DrumWheel.tsx` do;
  - the tuner keys (§6.2), whose existing Light/Medium impacts are replaced by this press/release pair;
  - the decoder header keys (§10), on every chassis, including `default`, where they keep today's look but
    still use `useDomeKey()`.
  Build it once, as a shared `useDomeKey()` (animation + haptic timing), so no key can be missed.
- The tuning and zoom drums already click. `DrumWheel.tsx`'s ratchet ticks (rigid/selection, soft settle
  after a flick) are unchanged: same feel on every chassis. Don't add key clicks to them.
- Gate every key haptic on the existing controls-haptics setting (`getControlHaptics()` in `DrumWheel.tsx`),
  the same switch the drums and tuner keys use. There is no separate toggle for keys.

---

## 6. Drum wells and tuner keys

### 6.1 Drum wells: port `DrumWheel.tsx`, don't redraw it
Keep the component and parameterise it:

| token | default / black | silver |
|---|---|---|
| panel face | today's `#101410 → #060707` | brushed silver + dark 1 pt gap + controls-colour glow ring |
| drum gradient | `#070807 #191a18 #232422 #181917 #050505` | `#4d4b46 #a9a69f #e4e2dc #a3a09a #393834` |
| ridge highlight | `rgba(160,160,150,.10)` | `rgba(255,255,255,.35)` |
| notches | light on dark | **dark cuts** `rgba(58,56,50,.40)` / `rgba(38,36,32,.70)` |

- ★★ **Remove the red index needle** on every chassis. In its place, only the LED glowing through
  from behind: a radial pool in the controls colour, `60% × 75% at 50% 18%`, α .16 → .05 at 55% → 0.
- ★★ **TRAP:** `G()` is `hsl(GLOW_HUE, 100, 45)` and cannot make white or neon, and blue/amber come out
  at the wrong brightness. Replace it with the controls-colour RGB triplet first.
- ★ **TRAP:** on aluminium the notch pair inverts (dark cut, white highlight below). Make notch
  colours *and* draw order chassis tokens.

### 6.2 Tuner-keys mode (`TunerKeys.tsx`; ref `tuner-keys/K1`, `K2`)
When the user has tuner keys instead of drums, the well keeps its face, border and glow, and holds
**two dome keys (larger versions of the keys above, §5) with the icon between them** in the controls
colour: `‹ [radio] ›` and `− [zoom] +`. Keys are 31% of the well wide (34% in landscape), full height,
same press, snap and haptics as §5. The existing sweep/hold logic is unchanged.

★★ **Keep the recess and the ring** (Stuart, 2026-09-30: "I really like on the silver how they look like
they are set into a slight recess to accommodate their larger size and … have a ring around them to
indicate their importance"). The tuner keys sit **inside the well's recessed face**, a step below the
plate, with each key in its own dark slot; the well's **outer ring glows in the controls colour**,
marking these as the primary controls. That's the drum well's own border and glow (§6.1), kept
exactly as it is when the drum is swapped for keys. Same on black and default; it reads best on silver.

---

## 7. Display styles
Applies to the frequency window, mode box and VTS strip (`VTSBar.tsx`, the live one; `VTSDisplay.tsx` is unused, since nothing imports it).

| Display | Frequency | Mode box | VTS | Colour |
|---|---|---|---|---|
| `hyper` | Atkinson, letter-spacing 2.5 | Atkinson | Atkinson | text colour (white on default) |
| `nixie` | fixed row of domed tubes, point in its own bulb | Barlow, neon | Nixie One | neon (§2) |
| `dot` | Doto 900 over a ghost-dot grid | Doto | Doto, UPPER CASE | text colour |
| `seg` | drawn 7-segment digits | sans | DSEG14 Classic Bold Italic, UPPER CASE, ghost `~` | text colour |

- **Nixie tubes** (ref `nixie/N1…N5`, `nixie-tune.gif`). Frequency only.
  - **Mounting:** domed side-view tubes (IN-14 style), each standing in its own socket on the floor of
    the window. They must never float. Per tube, top to bottom:
    - tip-off pip: 4 × 2, radius 2 2 0 0, `rgba(255,235,210,0.22)`;
    - glass: radius `48% 48% 3 3 / 26% 26% 3 3`, `#0a0504` with a cylinder highlight
      (vertical strips α .13 / .03 / 0 / .04 / .14), inner edge `inset 0 0 0 1px rgba(255,235,210,.10)`,
      floor shade `inset 0 -3 4 rgba(0,0,0,.8)`, faint neon bloom inside;
    - hex anode mesh: dot grid 3 × 2.6, offset by half, dots `rgba(150,140,125,.20)`;
    - socket collar: 4 tall, full tube width, gradient `#3a322c → #1a1511 → #070504`, 1 px top highlight.
  - ★★★ **TRAP: the tube shrinks, the window doesn't.** The stack (pip + glass + collar + 2 px top and
    bottom clearance) fills the window height. The glass is capped at its design height (table §4.1)
    and shrinks when the window does. Smallest case: analogue meter + shared banner = 35 pt window vs a
    37 pt stack. A fixed-height tube clips the domes there. Test that case first.
  - **Recess** (ref `nixie/N2`): the window is a cavity the tubes stand in, not a flat backing.
    - back wall: `#060403 → #15100c (28%) → #1e1610 (70%) → #2a1c12`, faint vertical grain
      (1 px lines α .012 on a 3 px pitch);
    - neon spill on the wall: ellipse 58 × 60% at 50% 58%, `rgba(255,100,30,.16)` → 0;
      on the floor: ellipse 70 × 40% at 50% 100%, `rgba(255,110,40,.14)` → 0;
    - lip shadows: top `inset 0 10 9 -3 rgba(0,0,0,.97)`, sides `inset ±10 0 10 -5 rgba(0,0,0,.92)`,
      bottom `inset 0 -4 5 -1 rgba(0,0,0,.8)`, front floor edge `inset 0 -1 0 rgba(255,200,160,.08)`;
    - each tube casts `4 3 3 rgba(0,0,0,.8)`, `7 6 10 rgba(0,0,0,.6)` and a neon halo
      `0 0 16 3 rgba(255,90,20,.10)` onto the wall. Clip it all to the window.
    - ★ **TRAP (perf):** the recess, glass, mesh, collars and shadows never change. Draw them once and
      cache the result as a layer. Per tune step, redraw only the cathodes and the decimal point.
  - **Cathode depth**, which is the "a little higher or a little set back" feel of a real tube:
    - The ten cathodes are stacked front to back in this order: `1 6 2 7 5 0 4 9 8 3`.
    - A lit digit at stack index *i* is drawn at `scale(1 − 0.028·i)`, `translateY(0.25·i)` and
      opacity `1 − 0.03·i`. So a 3 sits smaller, lower and dimmer than a 1.
    - Draw up to 3 unlit cathodes behind it as faint wire `rgba(150,130,110,.10)`.
    - Draw the 2 nearest cathodes in front of it as darker wire `rgba(70,48,34,.55)`, occluding the
      glow. All of these use the same per-index transform.
    - Digit `#ffc48a`, glow `0 0 1.5 #ff8a2e, 0 0 5 #ff6410, 0 0 11 rgba(255,80,10,.75),
      0 0 20 rgba(255,70,0,.35)`.
  - ★★★ **The decimal point is its own tube**, as on a real nixie clock (Stuart's reference photo). It is a
    small neon indicator bulb (INS-1 style), never a dot inside a digit tube:
    - width ≈ 0.45 × a digit tube, glass height ≈ 0.62 × a digit tube's glass;
    - its own tip-off pip, collar (tube width + 2) and cast shadow, and 1 pt of extra margin each side;
    - lit bead 4 × 5 at 58% height, `#fff1dc → #ffbd70 → #ff6a14`, glow
      `0 0 2 #ff8a2e, 0 0 6 #ff6410, 0 0 12 rgba(255,80,10,.7), 0 0 20 rgba(255,70,0,.3)`;
    - unlit, only the bare electrode shows (3 × 4, `rgba(150,130,110,.28)`), plus two lead wires
      below the bead, `rgba(160,140,120,.45)`.
  - ★★★ **Fixed tube layout: real hardware never adds or removes tubes** (ref `nixie/N5-units`):
    ```
    [MHz tubes] (bulb A) [3 kHz tubes] (bulb B) [3 Hz tubes]
    kHz  →  14 230•000   bulb B lit
    MHz  →  14•230 000   bulb A lit
    Hz   →  14 230 000   both bulbs OFF; the gap is intentional (Stuart)
    ```
    - The digits and tubes are identical in every unit. Only the lit bulb changes, so switching units
      never moves a digit.
    - MHz tubes = the digit count of the radio's top frequency in MHz:
      - network HF radios (`MAX_HZ` 30 MHz): 2, so 8 tubes;
      - local RTL-SDR (to 2 GHz): 4, so 10 tubes, narrowed to 8/10 of the normal tube width, with digits
        at 0.9 × the normal digit size, so the layout fits the same window;
      - the FM tuner screen (MHz, 3 dp): 3 + bulb + 3.
      The count is fixed for the connected radio, never for the current frequency.
    - ★ **TRAP: the bar-meter window is narrower.** Today's bar meter floats the frequency window inside the
      bar (12% inset each side), about 208 pt for tubes and unit in portrait. There, digit tubes are 16 pt wide
      (digits scaled to match, 1 pt gaps), in portrait and landscape; 10-tube radios narrow further by 8/10.
      The LED and analogue windows keep the full sizes in §4.1.
    - **Leading zeros are switched off**, not zero-filled: the tube is present, with its bare cathodes
      faint and no glow. Exception: the digit just left of a lit point always lights (0•648 000 MHz).
    - ★ **TRAP:** anchor the tube group in the window. Don't centre tubes and unit label together, or
      the tubes shift a couple of points whenever the label changes width (kHz / MHz / Hz).
  - **Neon afterglow:** a cathode that goes out fades over about one frame (≈ 38% brightness, then off)
    while the new one lights. It's a fade, never a flash, and there's no flicker at rest.
  - **Fast tune:** every intermediate frequency is shown, a real tube can't skip digits. The steps
    stay distinct, so digits change at the tuning rate, not per frame. See `nixie-tune.gif`
    (deliberately slowed to show the detail).
- **14-segment VTS scrolling is stepped, never smooth** (ref `vfd-scroll.gif`). A real segmented display
  can only shift a whole cell at a time. Advance one cell every ~300 ms, pause ~1.5 s at the start,
  and no sub-cell easing or pixel offsets. The dot-matrix VTS may step per column (1 dot).
- **7-segment:** polygons (points in the mockup), skewX −7°, unlit segments at text colour α .07.
  Not a font.
- **Ghost grid** (dot + status display): text colour α .10, 0.7–0.8 pt dots on a 3–3.4 pt pitch.
- ★ **TRAP:** tubes show digits only (the point is its own bulb) and 7-segment shows digits and a
  point; SCAN and similar go in the mode box or status display. The FM tuner screen has its own fixed layout, 3 tubes + bulb + 3 in MHz (` 96•600`); tubes are never added or removed while tuning (§7, fixed tube layout).
- ★★ **TRAP: units keep their case, always** ("the radio nerds will have our heads"). `dB`, `dBm`,
  `dBFS`, `Hz`, `kHz`, `MHz`, `k/s`, `fps` exactly so, on every display. Upper-case words with
  `toUpperDisplay()` and a unit whitelist, never `.toUpperCase()` over a whole string. Doto has
  lower case. The 14-segment VTS can't, so units never go through it.
- ★ Dot and 14-segment text scrolls **one whole character cell** per step, never by pixels (matters for
  Brazilian scrolling-PS). Build the ghost layer from the text length.
- ★★ **Character folding, like a real display ROM** (Stuart, 2026-09-30: EiBi names are full of accents).
  A real VFD could only show what its character ROM held, so fold to the nearest letter it has:
  - **14-segment (`seg`):** fold everything to plain upper-case A–Z / 0–9 and a few symbols.
    `NFD` → strip combining marks (`Ö→O`, `É→E`, `Ç→C`, `Ñ→N`), then special cases the decomposition
    misses: `ß→SS`, `Æ→AE`, `Œ→OE`, `Ø→O`, `Å→A`, `Ł→L`, `Đ→D`, `Þ→TH`, `ı→I`.
  - **Dot matrix (`dot`):** keep the accent if Doto has the glyph (real dot-matrix VFDs often had
    Latin-1 in ROM); fold only what Doto can't draw.
  - **Non-Latin scripts** (Cyrillic, Greek, Arabic, CJK…): transliterate with the platform's
    ICU transliterator (`Any-Latin; Latin-ASCII`: `CFStringTransform` on iOS,
    `android.icu.text.Transliterator` on Android 10+), then fold as above. If nothing usable comes
    back, show the frequency and callsign instead. Never show empty cells or tofu boxes.
  - Folding applies to the **display only**. Search, bookmarks and the Hyperlegible/Nixie displays
    keep the original text.
- ★★ **Every character takes exactly one cell, symbols included** (Stuart: "same with ?! @ etc").
  Measured from `DSEG14Classic-BoldItalic.ttf` (cell = 816 units):
  - **Full cell, use as-is:** `A–Z 0–9 ? @ & $ % ' " ( ) * + , - / < = > ^ _ \ | ``.
  - **Traps in DSEG itself:** `!` is **not** an exclamation mark; it is DSEG's *blank cell*.
    Space is only 200 wide, so **every space must be sent as `!`**, and a real `!` must be mapped
    (to `|`, the nearest a 14-segment cell can draw). `~` is DSEG's all-segments-on glyph (the ghost layer),
    so a real `~` maps to `-`.
  - **Zero or narrow width:** `.` (0) and `:` (200) squeeze in beside the character before them and push
    everything after them off the grid. `.` becomes that cell's decimal point, which is exactly how real
    14-segment displays did it (one DP per cell). `:` becomes `-` in its own cell.
  - **Missing:** `# ; [ ]` → `#`→`H`, `;`→`,`, `[`→`(`, `]`→`)`.
  - Anything left after folding and mapping becomes a blank cell (`!`), never a tofu box.
  - Build it as one pure `toSegCells(text): string` with a unit test covering every printable ASCII
    character and the accent list above. The ghost layer is `'~'.repeat(cellCount)`.
- **Dot matrix:** Doto is a real font with normal spacing, so symbols need no mapping; only unsupported
  glyphs fold. Still pad to whole cells for the ghost grid.
- ★ Hyperlegible never gets tube/dot/segment effects. Check contrast for every chassis × text colour.
- **Fonts to bundle (all OFL):** Doto (900) and DSEG14 Classic Bold Italic, alongside Nixie One and
  Atkinson. Load in the existing non-blocking `useFonts` path.

---

### 7.1 RDS mark and flag in the VTS (`VTSBar.tsx`; ref `rds/R1`, `R2`)
Today the VTS shows `assets/rds-logo.png` (fixed black-on-white bitmap) plus a flag emoji. Replace the bitmap with
the vector mark `assets/branding/rds/RDS_Logo.svg` (single-fill paths, react-native-svg), drawn 13 pt high,
≈ 53 pt wide. **Never the "ADVANCED" wording in the VTS.** That belongs to the Advanced RDS mode button and the
directory only.

| Display | RDS mark | Flag |
|---|---|---|
| `hyper` | the mark in the **resolved text colour**, with that colour's glow; only on an RDS signal | emoji, as today |
| `nixie` | the same, neon (§2) | emoji |
| `dot`, `seg` | **a pictogram built into the glass** (below) | ISO code in the display's own characters |

**Pictogram (dot / seg):** a real VFD annunciator is a fixed electrode, so:
- **It is always there.** When unlit it shows as a ghost (text colour α .10). It lights in the text colour, with
  `drop-shadow(0 0 2 glow)`, only while RDS is decoding.
- **It's cut into its electrodes.** There are non-emissive gaps through the ghost and the lit layer:
  - a line from (210,30) to (238,300), 16 units wide, splits the two loops;
  - a line from (30,193) to (440,193), 13 units wide, splits their upper and lower halves.
  These are logo units, viewBox `35.4 35.5 1063 260.3`.
- **It's seen through the VFD grid mesh:** a line pattern at 60°, 42 units pitch, bars 8 units wide at
  `rgba(0,0,0,.55)` and `.35`, filled over the mark.
- **Flag becomes country code.** A colour emoji can't exist on a VFD. The regional-indicator pair becomes its
  ISO code (🇬🇧 → `GB`):
  - `dot`: Doto 13 pt over its own ghost-dot grid;
  - `seg`: two DSEG14 cells at 11 pt over a `~~` ghost.
  - With no flag, the cells stay ghosted.
- ★ **TRAP:** the RDS mark is third-party artwork. Before shipping the recoloured and cut versions, check the RDS
  Forum's logo-use terms. The DAB+ toolkit, for comparison, forbids alterations (see `assets/branding/dabplus/README.md`).
  If recolouring isn't allowed, keep the mark monochrome in the text colour and drop the cuts.

## 8. Status row

### 8.1 Content and look
- **Default chassis:** today's footer, unchanged: clocks on line one; phone ⇄ bars ⇄ node, rate/fps,
  GAIN, IF on line two.
- **Silver/black:** a recessed sub-display (`#050505` + ghost grid, Doto 900 12 pt, text colour),
  **two lines in portrait**: `08:37 UTC 09:37 BST` / `[bars][node] 6k/s 5fps · GAIN ↓25.4dB · IF 2800k`.
  The gain arrow is drawn (Doto has no arrow glyph). One line in landscape.
- Landscape also carries the recording timer (a reserved slot, so nothing resizes), the DSP badges
  (NR / NB / AN), and **SHARED TUNER** (centre, replacing the node icon on shared servers).

### 8.2 ★★★ Small screens (designed on a 17 Pro Max; must fit an iPhone SE)
- All numbers here are at scale 1 and go through `s.r()` / `s.f()`. SE landscape is **×0.72**.
- ★★ **TRAP:** dot matrix and 14-segment fall apart below ~10 pt, so the status display has a
  **10 pt floor** that doesn't scale down. What doesn't fit is dropped, not squeezed.
- **Portrait never drops anything:** it is the full readout. The drop list applies to **landscape
  only**; rotating to portrait shows everything.
- **Landscape drop order** (measure with `onLayout`, never branch on device model). First to go:
  IF → GAIN/AGC → phone⇄node icons → local time → DSP badges → rate/fps → SHARED TUNER (shortens
  to `SHARED` first) → UTC → recording timer. **The connection meter is never dropped.**
- Optional polish: tapping the silver/black status window pages through clocks / link / gain·IF,
  like a hi-fi DISPLAY button. It never auto-cycles.

---

## 9. Landscape (ref `landscape/L0…L4`, `colours/landscape/`, `annotated/A2`)
Keeps today's `LandscapeBar` shape and **never gets taller than today's bar**:
`[VFO drum] [step / cog] [display] [audio / chat] [zoom drum]`, then the status row.

- Grid `minmax(0,1fr) 62 360 62 minmax(0,1fr)`; rows **28 / 28 / auto**; column gap 8, row gap 6.
  The control band is 62 pt.
- **Frequency on top, meter underneath**, as in portrait: LED VU = frequency window (digits 25 pt,
  tubes 15 × 27) + LED strip (LEDs 9 pt, labels 6.5 pt); analogue = frequency + 24 pt edgewise
  window with a full-width scale; bar = today's bar at 62 pt.
- Shared VFO: the banner lives in the status row, so the display column never grows.
- Drum wells stretch to their column: trapezoid 25 pt, drum from 24 pt.
- Keys: 24 pt caps in 28 pt slots, legends 78%. Under 44 pt as today, so keep `hitSlop`.
- Silver: 24 pt side padding so the screws clear the drums. Black: the gloss panel wraps frequency
  and meter together.
- ★ **TRAP:** on the iPhone SE (667 pt) the drums sit near `DrumWheel`'s 80 pt minimum. Check it, and
  below ~740 pt width show the LED strip without labels, or the bar.

---

## 10. Decoder boxes (ref `decoders/D1…D4`, `annotated/A4`)

> "The decoder box takes on the appearance of the main control deck, but the text remains
> Hyperlegible … it is very information dense."

Applies to every panel over the waterfall: `DabPanel`, `DecoderPanel` (spots, CW, RTTY…),
`AdvRdsPanel`, `AircraftPanel`, `LocalHardwarePanel`, and future ones.

### 10.1 ★★ Unify first (today's drift)
Each panel carries its own copy of the frame styling, and the copies have drifted:

| | `DecoderPanel` | `DabPanel` | `AdvRdsPanel` |
|---|---|---|---|
| body | 0.95 opaque, no blur | 0.94 + BlurView 24 | 0.72 glass (+ iOS blur 35 in small; BIG 0.62) |
| title | 10 pt, goldDim .70 | 11 pt, .86 | 11 pt, .86 |
| muted / key text | .38 | .60 | .60 |
| values | white / output colour | `#ffe566` | `#ffe566` |
| header / key padding | 12 / 8 | 10 / 7 | 12 / 8 |
| max width | 760 | 560 | 600 / 800 |
| white theme | follows it | ignores it | ignores it |
| font | `t.font` (**leaks Nixie One**) | Atkinson | Atkinson |

Extract **one `DecoderShell`** (frame, header, header keys, tokens) that all of them render inside.
Use the majority values (title 11 pt / .86, muted .60, padding 12 / 8), follow the theme everywhere,
pin **`Fonts.decoder = 'Atkinson Hyperlegible'`**, and justify or drop DAB's 560 cap.

### 10.2 Decoder background: `Transparent` (default) or `Solid`

> ★★★ **SUPERSEDED 2026-09-30 by ONE app-wide setting, TRANSPARENCY EFFECTS On / Off** (Stuart: "That
> solid/transparency toggle we added to the decoder box, apply that to the default controls too, then when app
> opened we detect low end hardware we default to transparency off … then user can customise afterwards", and
> "I thought solid would be 1.0 fully solid for max GPU savings").
> - **Off** = alpha **1.0 exactly** and **no `BlurView` anywhere**: the default deck (portrait + landscape), every
>   decoder box (default chassis: today's glass composited over black; silver/black: the metal look below), the
>   menu sheet, and row 10's `PopupShell` via `useSurfaceOpaque()` + `solidOver()` (FaceplateContext / faceplate.ts).
>   **Opaque panels over a live, undimmed waterfall:** no full-screen scrim (keep the invisible tap-to-close view),
>   no blurred drop shadow over the spectrum, and the waterfall never stops drawing under a panel. `useSurface()`
>   gives PopupShell `fill()` / `blur` / `scrimOpacity` / `dropShadow`. (The glass box's shadow was worse than it
>   looked — iOS RN only precomputes a `shadowPath` when that
>   view's own background is > 0.999 alpha; otherwise it renders the shadow per pixel, offscreen.)
> - Until the user picks, `autoTransparency()` (src/constants/transparency.ts) decides: Off for iOS Reduce
>   Transparency, Android API < 29, an iPhone on iOS < 17; RAM ≤ 3 GB and pre-A12 model identifiers are coded
>   and tested but have no JS source yet. A stored choice always wins; the auto value is never stored.
> - Subtitle: "Off · solid panels, easier to read and lighter on older devices".
> The table below is the original design; read "Transparent" as On and "Solid" as Off (at 1.0, not 0.95).

("I prefer transparent to see the signals, but someone may prefer solid for easier to read.")

| | Transparent (default) | Solid |
|---|---|---|
| default chassis | today's glass (0.72; BIG 0.62), unchanged | today's panel at 0.95 |
| silver / black | today's glass; **only** the header keys become dome keys and the border, glow, titles, labels and charts take the controls colour | brushed metal frame and header, data inside a **recessed dark window**; header text engraved (dark on silver, light grey on black) |
| blur | none over the spectrum (the expensive case) | none needed |

- Setting subtitle: "Solid · easier to read, hides the signals behind".
- **Text is always Atkinson Hyperlegible**, whatever the Display style: no Nixie One, tubes, dot
  matrix or segments, not even in RTTY/CW text ("teleprinter" looks are out).
- Labels use the controls/text colour; values stay near-white (`#f2efe8`; default keeps `#ffe566`).
  **≥ 4.5:1 contrast** in every chassis × colour; if one fails, lighten the same hue.
- Charts (constellation dots, impulse response bars) take the controls colour on silver/black.
- ★ **TRAP:** meaning colours never change: MER/warning pink `#ff8fa3`, SNR green `#55d98d` / amber,
  sync dot, error values. A red-controls user must still see good from bad.
- Keep `tabular-nums` wherever numbers line up in columns.

---

### 10.3 ★★★ Popups, menus and chat take the chassis (ref `popups/P1…P3`)
> "Popup boxes can they be styled to the controls please same with menus too … be weird having a light control
> scheme with black popups … and chat menu too."

**Scope:** `FreqModal` (Tune / Bookmarks), `AudioSheet`, `MenuSheet`, `ChatDrawer`, and the smaller modals that
share the look: `RecordingsOverlay`, `KeyboardShortcuts`, `PasswordModal`, `IdentModal`, `CityPickerModal`,
`AboutOverlay`. Build one `PopupShell` for all of them, the way `DecoderShell` works (§10.1).

| Chassis | Popups |
|---|---|
| `default` | **Unchanged.** Today's dark glass and gold selection, pixel for pixel. |
| `silver` / `black` | A solid brushed plate matching the deck, laid out as below |

- **Plate:** the same texture and lighting layer as the deck (§3), opaque.
  - `MenuSheet`'s two `BlurView`s go on silver and black, the same performance win as the deck.
  - Top corners 16 pt, a 1 pt chassis-edge border (silver `#8b8983`, black `#3a3c40`), and a top highlight.
  - The grab handle is an **engraved groove**, not a pill: silver `rgba(0,0,0,.38)` over a white 1 pt lip.
- **Labels and section headers are engraved** into the plate, in Atkinson Hyperlegible:
  - silver `#35332e` with `0 1px 0 rgba(255,255,255,.6)`;
  - black `#a3a6ac` with `0 -1px 0 rgba(0,0,0,.9)`.
  Popup text is always Atkinson, except the tune entry below.
- **Every button is a dome key** (§5): same cap, `useDomeKey()`, same haptics.
  - Legends are engraved: silver `#2a2824`, black `#cfd2d7`.
  - The primary action (TUNE ▶, chat send) has its legend lit in the **controls colour**.
- ★★ **Selection works like an input selector.** Every key in an exclusive or toggle group carries a
  4 × 4 pt **LED pip**, top-left:
  - lit: the controls colour with glow, and the legend also lit in the controls colour;
  - unlit: a dark pip (silver `rgba(0,0,0,.30)`, black `rgba(255,255,255,.07)`, both inset).
  Plain action keys (RECORDINGS, MIN/MAX, CLOSE) have no pip. This replaces every gold fill.
- **Data sits in recessed windows:** `#070605`, `inset 0 2 5 rgba(0,0,0,.95)`, and a 1 pt light lip below
  (silver α .8, black α .14). This covers:
  - the tune station and entry;
  - the SIGNAL readout;
  - the chat thread;
  - every text input.
  Readouts are in the **text colour** (neon under Nixie). Chat keeps its meaning colours: own name blue,
  others amber, system grey italic.
- **The tune entry follows the Display style**, like a readout on the deck: Hyperlegible, Doto, DSEG14,
  or **Nixie One in neon** for `nixie`. Tubes are for the deck's main frequency only.
- **Sliders become slide faders** (squelch, NR):
  - a recessed slot;
  - fill in the controls colour;
  - squelch's live signal level shown behind the fill at white α .22;
  - a brushed cap, 20 × 18 pt, with a lit index line.
- **Faceplate settings** (§1) live in the CONTROL CUSTOMISATION pane as input-selector rows. The colour keys show
  each colour as a lit LED dot (ref `popups/P1`, Control Customisation).
- ★★★ **TRAP: hard-coded gold.** `C.gold` / `#ffb833` / `rgba(255,184,51,…)` appear **83 times**:
  - `AudioSheet` 35, `MenuSheet` 38, `ChatDrawer` 9, `RecordingsOverlay` 1;
  - most are inline styles per button.
  Move them into a `usePopupTheme()` token set first, the same way §12 does for `ControlsBar`. The default
  chassis returns today's values, so it renders identically.
- ★★ **TRAP: silver means dark text on a light plate.** Any hard-coded white or cream text left outside a
  window will vanish. Examples: `ChatDrawer` input `#ffe0a0`, canned-reply text, `setupLbl`. Either it goes in a
  recessed window or it becomes an engraved label.

## 11. ★★★ Fix first: landscape key heights (today's bug)
On a live landscape screenshot the **"1k" key is shorter than the cog under it** and the audio/chat
pair differ. Likely cause: `lnd.lsBtn` sizes each key with `flex: 1` and the step `Text`
(`numberOfLines={2}`, `adjustsFontSizeToFit`, scaled `lineHeight`) and the cog's Skia `Canvas`
report different intrinsic heights.

**Fix:** explicit `KEY_H = (BAND_H − GAP) / 2` on every landscape key, no `flex: 1`, content
centred, `overflow: 'hidden'`. `adjustsFontSizeToFit` must shrink "100k" / "500Hz" to fit, not
grow the key. **Acceptance:** four identical heights on iPhone, iPad and Android, in every step size
and in the menu-as-back state.

---

## 12. Architecture
- `FaceplateTheme` (chassis tokens) + `ControlsColour` + `TextColour` (RGB triplet + derived alphas)
  + `DisplayStyle`, resolved once in a context, consumed by `ControlsBar`, `DrumWheel`, `TunerKeys`,
  `VTSBar` and `DecoderShell`.
- First, move every hard-coded colour out of `ControlsBar.tsx` (`#e04040`, `#33cc44`,
  `rgba(255,184,51,…)`, the meter gradient…) into tokens, or one finish will keep stray bits.
- Textures: `assets/faceplates/silver-brushed.jpg`, `black-brushed.jpg` (grain only).

## 13. Acceptance
1. Default chassis, bar, Hyperlegible, green: pixel-compare with today's build.
2. Every sheet in `colours/`, `all-states/`, `landscape/`, `tuner-keys/`, `decoders/` reproduced on an
   iPhone and on the Xcover 4S.
3. No Nixie One glyph anywhere in the app renders in any colour but neon.
4. No route (picker, migration, stored prefs) reaches white text on `dot`/`seg`.
5. Deck height identical across meter types and shared/not shared, per chassis.
6. Keys: snap visible on touch-down; click on press and release; no dropped frames with the waterfall running.
7. VU at 60 fps on the Xcover 4S with DAB playing; no visible flicker at 5 fps on fading HF.
8. iPhone SE portrait and landscape, recording on, shared server, every Display: nothing clips or
   overlaps; the connection meter is always visible.
9. Landscape keys identical in height (§11).
10. Every dome key (the four main keys in portrait and landscape, tuner keys, decoder keys) clicks on
   press and release on iOS and Android, and all fall silent when controls haptics is off.
11. Nixie: tuning 30 MHz → 10 kHz and switching kHz / MHz / Hz never adds, removes or moves a tube; only
    digits and the lit bulb change. Domes are never clipped in any window, including analogue + shared VFO.
12. RDS: no "ADVANCED" in the VTS. The mark follows the text colour, and is neon on Nixie. On dot and seg it is
    ghosted without RDS, and the flag shows as its ISO code.
13. Popups on silver and black: open Tune, Audio, the cog menu, chat and each small modal. Check that:
    - no gold appears anywhere;
    - no light text sits directly on the silver plate;
    - every selected state shows by its LED pip;
    - every button clicks;
    - on the default chassis, every popup is identical to today.
14. Cog menu: CONTROLS is a single `CONTROL CUSTOMISATION ›` key. Its pane holds every row in §1's table,
    each setting works from there, and the `schemes` tour card points to it.

## Reference assets (`assets/faceplates/`)
- `annotated/A1…A4` — **start here**: labelled portrait deck, landscape deck, analogue meter, decoder box.
- `Deck.mockup.dc.html`, `Decoder.mockup.dc.html`, `Picker.mockup.dc.html` — the source of every number.
- `colours/portrait/01…12`, `colours/landscape/01…12` — every allowed colour for every chassis × display.
- `all-states/01…12` — every chassis × display with bar and LED VU, squelch open and closed.
- `all-states/E1…E4` — the analogue meter on each chassis, plus a close-up.
- `landscape/L0…L4` — landscape, including one over a real screenshot.
- `tuner-keys/K1, K2` — tuner-keys mode, portrait and landscape.
- `decoders/D1…D4` — decoder boxes, Transparent and Solid.
- `nixie/N1…N5` — close-up, recess on every chassis, smallest windows, landscape, every unit. `nixie-tune.gif` (fast tune, slowed).
- `rds/R1, R2` — RDS mark and flag on every display, and the pictogram close-up. `vfd-scroll.gif`.
- `popups/P1…P3` — Tune, Audio, cog menu, Control Customisation and chat on silver, black and default.
- `Popup.mockup.dc.html` — the popup source.
- `01…10-*.jpg` — the overview set shared on Discord (08 Nixie tubes, 09 RDS, 10 popups).

All reference images are JPEG (quality 84, max 2600 px wide) to keep the repo light.
- `silver-brushed.jpg`, `black-brushed.jpg`, `wf-backdrop.jpg` (waterfall behind the decoder mockups).
- `DSEG14Classic-BoldItalic.ttf` + `DSEG-LICENSE.txt` (OFL): **bundle the TTF**. React Native can't load WOFF2;
  the `.woff2` is for the mockups only.
