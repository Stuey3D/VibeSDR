# BRIEF: Keyboard panel navigation + game controller support

**Project:** VibeSDR (`src/components/`, `ios/VibeSDR/`)
**Author:** Stuart Carr (Stuey3D) with Claude, 2026-07-25
**Status:** DESIGNED, not built. Design settled over a long session — read §2 before writing code, it is
where most of the thinking went and several decisions reversed.
**Build order (Stuart):** *"Let's get keyboard controls working first… I will test and make sure keyboard
works properly and then do game controller. I have both."* Build the SHARED machinery once, test the two
surfaces separately.

---

## 1. Why this is one brief and not two

★★ **A game controller is NOT a tvOS feature.** `GCExtendedGamepad` works on **iPhone, iPad and Mac
today** — no new target, no scene manifest, no separate App Store submission. tvOS is merely the one
platform where controller/remote support is *mandatory*. So this ships in **V10 alongside the keyboard**,
not in V11 behind shack mode, and it was mis-filed under `external_display_shack_mode` while the design
was being worked out.

★ Stuart: *"It will genuinely be one codebase for all the Apple ecosystem barring the watch, which gets
Jr."* That is the point of doing it properly: iPhone, iPad, Mac and eventually Apple TV are one build with
one input layer, and the watch is a deliberate exception (see memory `watch_split_jr_buddy`).

★★ **And the controller needs NO NEW CONTROL IDENTITY** — it supplies the two schemes already built and
validated on device:

| controller | maps to | existing law |
|---|---|---|
| **D-pad** | the 90s car-stereo **tuner keys** | `createHoldSweep` — tap = step, hold = ramp to sweep |
| **Analogue sticks** | the **drums** | rim-engaged rotation → delta → the drum's send/detent pipeline |

★ A controller is the only surface offering **both schemes at once**, one under each thumb — gross
stepping and fine rotation together, no toggle. Which means, as Stuart put it, *"the only thing we don't
have to do is ask the user what they prefer, as they have both at once."* **No preference setting, no
persistence, no per-device default, no wrong answer to recover from.** The phone only forces the
drums-or-keys choice because of screen space, so that was always a spatial constraint, not a design one.

---

## 2. The design decisions, and the ones that reversed

Read this section before implementing. Three of these were arrived at by reversing an earlier choice, and
the reasoning is not recoverable from the code.

### 2.1 ★★★ NO analogue-stick VELOCITY. The D-pad does tune/zoom, exactly like the arrow keys.

The first proposal read "sticks for tune/zoom" as *deflection = rate*. That is wrong, and it was rejected
for a stronger reason than simplicity: **the D-pad path is the only mapping that satisfies Apple's floor
with one code path.** tvOS mandates that the Siri Remote works; the remote has no sticks, but its clickpad
reports as **`dpad` on `GCMicroGamepad` — the same element name `GCExtendedGamepad` uses.** So a single
D-pad handler covers the Siri Remote, every game controller, *and* the keyboard arrows.

★ Put the other way: had tune/zoom lived on the sticks, the D-pad path would STILL have been needed for
the remote. Velocity would have been additive complexity that removed no obligation — a second control
law, a deadzone, and a new sweep curve to validate, all to duplicate something already working.

### 2.2 ★★★ The sticks are a TUNING DIAL: rotate them, do not deflect them.

Stuart's actual intent, and the nostalgia is the point: **circle the stick clockwise/anticlockwise, angle
accumulating into steps, emulating an old tuning dial with the little finger dimple.** The lineage
completes as **80s boombox drums → 90s car-stereo keys → the tuning dial with the finger dimple** — a
third era of radio control, on the surface that suits it. Feeds the website story.

### 2.3 ★★★ Push to the RIM, then rotate — and neutral is a CLUTCH.

*"Push to the full edge of the stick's travel then rotate. Maximum movement for maximum precision. Also
acts like a lock — sticks in neutral, the control deactivates. You have to WANT to make it work, not
accidentally knock it and knock your tuning off."*

★ Angular resolution for a given positional jitter is **ε/r**, so precision **scales with radius** — at
the rim you get the finest control the hardware can give. This earns three things at once:

1. ★★ **The hardware supplies a guide rail.** Sticks have a circular gate at maximum travel, so
   rim-circling is *tactile* — the thumb rides the rim and can feel it, exactly as a real dial's rim does.
   The metaphor is physical, not merely visual.
2. **The centre-deadzone problem disappears** — angle is never counted where angle is meaningless.
3. ★★ **The clutch is the best part.** Neutral = disengaged, so tuning takes deliberate intent and a knock
   cannot drift the frequency. Consistent with the existing VFO lock philosophy: the band never moves by
   accident.

★ **Division of labour, state it explicitly:** **D-pad = GROSS** movement (hold-sweep across a band);
**stick rim = PRECISION** (settling onto a signal). Sustained full-deflection circling tires a thumb, but
that is not its job — the same reasoning as drums-versus-keys.

★ One limit, not fatal: a stick **self-centres**, so there is no coasting or inertia. This is displacement
tuning, not a weighted flywheel — fine, because the drums are the same and the flick-coast was removed
deliberately.

### 2.4 ★★ THE MINIMUM CONTROL SET: directions + confirm + back. Everything else is an accelerator.

Prompted by *"if a user is using say half a Nintendo Switch Joy-Con they still get full control"*. Apple
has supported single Joy-Cons since **iOS 16**, either half alone, and the wrinkle is that **each half has
one of the two things we rely on, not both**:

- **Left Joy-Con** — its four buttons ARE the directions, so a real D-pad plus SL/SR, but **no spare face
  buttons** (the four *are* the D-pad).
- **Right Joy-Con** — ABXY, but **no D-pad at all**. Only the stick — the exact case §2.1 designed out.

★ So **guarantee the app is fully operable from directions + confirm + back alone**, and treat the
shoulders and face buttons as SHORTCUTS. Already sound, because audio/menu/chat are all reachable *inside*
the menu — the flank buttons are accelerators to things that have another route.

★★ **Consequence: stick-mirrors-D-pad is not optional decoration — it is what makes the right Joy-Con work
at all.** Keep it, as a **compatibility** feature rather than a feel feature.

★ It also helps that iOS, iPadOS and tvOS all offer system-wide and per-app controller remapping, so
genuinely exotic hardware is something the user can fix themselves.

### 2.5 ★★ B / circle is MANDATED, and step rate moves to the shoulders.

On tvOS the Menu button **must** back out and ultimately exit to Home or you fail App Review; on a
controller that role lands on **B** (Play/Pause maps to X). So B was never poachable. That left three free
face buttons (A/X/Y) for four flanks (step rate, audio, menu, chat).

★★ **Stuart's call: step rate goes to L1/R1**, leaving audio/menu/chat → A/X/Y. Better than an arithmetic
fix, because it splits by **action type**: step rate is a *cycle through a fixed ladder* (five ordered
StepPicker rungs) = exactly what paired shoulders are for; audio/menu/chat all *open a panel* = a
momentary action = a face button. Bonus: shoulders work **without looking**, so the most-adjusted
parameter never pulls focus off the waterfall.

Full mapping (all Stuart's):

| input | action |
|---|---|
| D-pad ◀▶ | tune (tap = step, hold = sweep) |
| D-pad ▲▼ | zoom (same law) |
| left stick, rim-engaged | tune, rotary |
| right stick, rim-engaged | zoom, rotary |
| L1 / R1 | step rate down / up a rung |
| A (or X) | confirm / enter a panel |
| **B — RESERVED** | back out; must reach Home. Not assignable. |
| X, Y | two of audio / menu / chat |
| Start | frequency entry |
| Options / Select | demodulator |
| D-pad, in a panel | navigate the focus grid |

### 2.6 ★★ Glyphs: do NOT hand-draw them, and it is the same feature as the keyboard cheat sheet.

`GCControllerElement.sfSymbolsName` (plus `localizedName`), both iOS/tvOS 14+, returns the SF Symbol for
the **connected hardware** — a Horipad's A reports `a.circle`, a DualSense reports the shapes. So Xbox
ABXY vs PlayStation ×○△□ resolves itself, including Nintendo's physical A/B swap, and accessibility
labels come free. ★ Known bug to expect: stale values after input remapping is disabled, until app
restart.

★★ **That glyph layer IS the keyboard cheat sheet.** Both are "show the actuator beside the thing it
actuates, in context". Build **one mechanism with a glyph set per input device**, not two features that
drift apart. The cheat sheet is already on the V10 list, so controller support becomes a second glyph
*set*, not a second *system*.

### 2.7 ★★ Haptic detents, and the rate policy that already exists

Stuart: *"Could also activate the PlayStation haptics to make the user feel the clicks as they rotate."*
Completes the metaphor — a real dial has detents.

`GCController.haptics` → `GCDeviceHaptics` → a `CHHapticEngine` per **locality**, and `GCHapticsLocality`
has separate **`leftHandle` / `rightHandle`** (tvOS 14+, iOS 14+). ★ So the detent fires from the **side
being rotated**: left stick tune clicks the left grip, right stick zoom the right. The click feels like it
comes from the dial under the thumb rather than the whole controller buzzing — the difference between
feeling real and feeling gimmicky.

★★ **The rate policy already exists and is better than a rate cap** — `DrumWheel.tsx:122-145`,
`detentTick()`:
- accumulates distance, one tick per `LSV_PX_STEP` crossing (so slow deliberate tuning ticks *and* fast
  drags do not buzz — the old per-send gate failed both ways);
- whole crossings **collapse into one tick**, which is the ratchet feel;
- hard **28 ms cap** (~35 ticks/s);
- `gap > 90 ms → ImpactFeedbackStyle.Rigid` (deliberate mechanical click), else `selectionAsync()`
  (lighter tick) — it **downgrades rather than drops**, so a fast spin is a freewheeling ratchet, not a
  buzz.
- `settleTick()` = a `Soft` thunk when a flick finishes coasting.

Reuse all of it. On CoreHaptics the two styles become sharpness/intensity on a `CHHapticTransientEvent`,
so tuning already done on device carries over rather than being re-derived. **Engage/release gets its own
firmer "clunk"**, distinct from the per-detent tick.

★★ **A REAL CATCH TO FIX:** the `✦ HAPTICS` toggle (`MenuSheet.tsx:1115`) is gated on whether **the
device** has a haptic motor — which is why it is hidden on iPads (see the 5.2.2 notes in
`AboutOverlay.tsx`). But **an iPad, Mac or Apple TV with a DualSense connected DOES have haptics — the
motor is in the controller.** The rule must become "device motor **OR** connected controller with a
haptics profile", or the toggle vanishes exactly where this feature is most used.

### 2.8 ★★ Onboarding: the stick's first-detect pill

*"If a user tries to tune by pushing the stick up/down/left/right they get a little pill with a stick
animation showing push to the edge then rotate, showing rotation clockwise and anticlockwise, and maybe a
simple number going up and down to show what each does."*

★ The trigger is right because it fires when the user has just revealed the misconception **with their
thumb** — an answer to a question they actually asked, not a tutorial on connect that nobody reads.

★★ **TRAP: it must not fire on CORRECT use.** Pushing to the rim *passes through* a cardinal deflection on
the way, so "deflected in a direction" alone would pop the pill every time someone engages properly. A
dwell was proposed and is **not sufficient on its own**: a dwell cannot tell *"waiting for it to work"*
from *"paused mid-gesture"*, and someone who reaches the rim and pauses half a second to think before
rotating is doing it **correctly**.

★★★ **Stuart's rule: require the attempt REPEATED a couple of times.** Nobody pushes, gives up, and pushes
again twice unless it is not working. The two combine rather than compete:
- the **dwell defines a failed attempt** — pushed to a direction, held, released, never rotated;
- the **count is the confidence threshold** — fire on the 2nd–3rd such attempt.
- ★ **Reset the counter on any successful rotation.** Once someone has tuned they have understood; stray
  deflections later must not accumulate toward a lecture they no longer need.
- ★ **Do not require the same direction.** Someone pushing up, then left, then down is arguably the *most*
  confused user, systematically hunting for the one that works — same-direction-only would miss them.

★ Content notes: the **number is the cleverest part**, because rotation *direction* is the one thing a
diagram of a circle cannot convey — clockwise-equals-up is arbitrary until something shows it, and a
rising/falling number needs no language (also dodges localisation). Showing the *real* frequency instead
was considered and rejected: this pill also appears on the TV at sofa distance, where two or three legible
digits beat a nine-digit readout with one digit moving.

★★ **Tie the animation to the haptic** — visual emphasis at the instant the stick reaches the rim, the same
instant the engage clunk fires. Seeing and feeling engagement together is what makes the clutch click as a
concept rather than being two things to learn.

★ **GENERALISE THE MECHANISM.** The same trap applies to the keyboard and pointer first-detect pills
already planned: a pointer user scrolling once in the wrong axis, or a keyboard user pressing a dead key,
has not necessarily misunderstood anything. Put **detect-failed-attempt → require repetition → reset on
success** in the *shared* pill mechanism, not in the stick handler. Then the stick's variant is just a
different animation.

---

## 3. What already exists, and what is genuinely new

### 3.1 Reusable as-is

| what | where |
|---|---|
| Tap-step / hold-sweep law, step-rate aware | `TunerKeys.tsx` — `createHoldSweep`, `useHoldSweep`, `sweepTargetRate` |
| Throttled send at UberSDR's confirmed max (UPDATE_RATE=40 → 25 ms) | `DrumWheel.tsx` — `sendDelta` |
| Detent haptics: accumulate, collapse crossings, 28 ms cap, Rigid/selection downgrade | `DrumWheel.tsx:122-145` — `detentTick`, `settleTick` |
| Focus-grid navigation: `NavCtx` / `RowCtx` / `nextBtnId`, `styles.btnFocused` (`#7CFF9B`) | `MenuSheet.tsx:395-441`, provider at `:793-1256`, style `:1334` |
| Native event plumbing to JS: `static weak var shared`, `emitKey`, `emitScroll` | `VibePowerModule.swift:50-74` |
| Hardware key capture: `VibeKeyWindow`, `pressesBegan/Ended`, `installScrollRecognizer` | `AppDelegate.swift:56-146`, installed at `:160,178` |

### 3.2 ★ Genuinely new — and an honest correction

★★ **The drum is a LINEAR drag, not an angular one.** `DrumWheel` accumulates `dx` in *pixels*
(`:217-220`) and renders a cylinder so the motion *reads* as rotation. So an earlier claim in the design
discussion — that stick rotation "is the drum law, already built" — was **too strong**. What is shared is
everything *downstream* of the delta; the angle extraction itself is new.

New code, roughly 30 lines:
1. `atan2(y, x)` from the stick's x/y.
2. **Radius gate with hysteresis** — engage ≈ 0.9 of travel, disengage ≈ 0.7. A single threshold chatters
   on a slightly sloppy circle.
3. ★ **Entry angle becomes the ZERO REFERENCE.** Engagement captures the current angle and only the
   *delta* from there counts — otherwise engaging at the top would jump the tuning by 90°.
4. **Shortest-arc unwrapping** at the ±180° crossing, or one lap sends you flying.
5. Convert angular delta → the same **pixel units** the drum uses.

★★ **Do (5) deliberately: define DEGREES PER DETENT and convert into `LSV_PX_STEP` units.** Then the stick
inherits the drum's detent feel exactly, and **one constant tunes both**. This is the whole reason the
mapping is cheap.

Also new: `GCController` discovery/connect/disconnect, the glyph layer, the haptics bridge, and the pill.

### 3.3 ★★ iOS native constraint — NO NEW FILES

`expo prebuild` regenerates `ios/`, and the `pbxproj` is **hand-maintained**, so a new `.swift` file would
be silently dropped. Controller support goes into the **existing** files, exactly as the keyboard did:
- `AppDelegate.swift` — `GCController` notifications, `valueChangedHandler`, the rotation maths.
- `VibePowerModule.swift` — add `"VibeGamepad"` (and a connect/disconnect event) to `supportedEvents()`,
  plus a `static func emitGamepad(...)` beside `emitKey`/`emitScroll`.

★ Send **semantic deltas**, not raw axes. JS should receive "tune by N steps" / "engaged" / "detent", not
60 Hz x/y — that keeps the bridge quiet and the control law in one place.

---

## 4. Build order

### Phase 1 — keyboard panel navigation ✅ BUILT AND FIELD-TESTED 2026-07-25 (build 220)

Covered: MenuSheet, StepPicker, AudioSheet, ModeSelector, the SERVER PICKER (header chooser +
list + footer directories, one focus order), the servers chip, the frequency card (both tabs),
dropdowns, sliders and the squelch bar.

★ The tune card is COMPLETE as it stands: H/K/M reach the units, Enter tunes, and SHARE is
deliberately excluded because it hands off to a system sheet we do not control — Stuart's call
and the right one.

★★ WHAT THE BUGS IN THIS PHASE HAD IN COMMON, worth reading before the controller work:
- **Mounted is not on screen.** Twice. A guard on a mounted-but-blank overlay killed the
  keyboard app-wide; a picker still mounted behind the SDR screen connected to a stranger's
  receiver. Any screen-level key listener needs a focus gate.
- **Read the one line that calls it.** `onDrag(x)` was typed as a coordinate and passed a
  normalised value; believing the parameter name applied full squelch with no way back.
- **Check the method exists.** `measureLayout` with a node handle, then `getInnerViewRef`, then
  a ScrollView that has no `measureInWindow` at all — three builds spent assuming a component
  forwards something.
- **A list needs `extraData`, and DraggableFlatList passes `getIndex()` not `index`.** Focus was
  correct and invisible for several rounds because of these two.
- ★ **Measure, do not reason.** One on-screen debug line found in a single screenshot what three
  readings of the source had not. Reach for it sooner.

Done: `PanelNav.tsx` extracted (grid + list shapes, owner arbitration, measured reveal);
MenuSheet rewired onto it; **sliders fixed** (they were SKIPPED entirely — Stuart found it);
StepPicker, AudioSheet (incl. the squelch bar) and ModeSelector all navigable. `tsc` clean.
★ NOT yet verified on device — see §4.1 for what to try.

Main-screen keyboard control already ships. What is missing is navigation *inside* the panels.

1. ★ **Extract the nav machinery OUT of `MenuSheet.tsx` first** (`NavCtx`/`RowCtx`/`nextBtnId`/`btnFocused`
   → a shared module). Do this before touching any panel — otherwise it gets copied three times.
   ★ The Btn/BtnRow primitives already learned focus, so each panel's grid *derives from existing JSX*
   rather than needing a hand-written map.
2. `StepPicker.tsx` — 5 buttons. The easy one; prove the extraction here.
3. `AudioSheet.tsx` — 9 buttons.
4. `ModeSelector.tsx` — **29 buttons across 6 ScrollViews. This is the real work.**
5. **Fix scroll-to-focus** — currently estimated as `row * 46`, never measured. Measure it.
6. Build the **shared first-detect pill mechanism** here (§2.8), with the keyboard glyph set, so phase 2
   plugs a second glyph set and a second animation into it.

★ Test each panel as it lands. A crash-loop already cost a session once: `if (!open)` type-checked because
the DOM lib declares a global `open`, and the prop was `visible`. **A clean `tsc` is NOT proof a name is
in scope.**

#### 4.1 ★ What to test before phase 2

1. **MenuSheet** — arrows walk every row; **sliders now take focus** and left/right change
   them (they used to be skipped). Toggle waterfall AUTO/MANUAL and check the sliders that
   appear land in the right place in the order, not at the end.
2. **StepPicker / ModeSelector** — arrows walk the wrapped grids in reading order; CLOSE is
   the last stop. In ModeSelector open DIGITAL/DECODERS and walk the dropdown — focus there
   is a background tint, not a border, deliberately.
3. **AudioSheet** — the squelch bar takes focus and left/right nudges it 4% per press.
4. ★ **Scroll-to-focus** — the one part that could not be verified without a device. It now
   MEASURES instead of estimating; if focus moves without the panel scrolling to follow,
   `getInnerViewNode()` is unavailable and it is silently using the old estimate.
5. **One panel at a time owns the keys** — open a picker over the menu and confirm only the
   top one moves.

### Phase 2 — game controller (built on phase 1's machinery)

1. Native: `GCController` discovery + `VibeGamepad` events (§3.3), no new files.
2. D-pad → `createHoldSweep`. Should be nearly free; verify against the keyboard's behaviour.
3. Panel navigation from the D-pad — the same focus grid phase 1 extracted.
4. Stick rotation (§3.2): radius gate + hysteresis, entry-angle zero, unwrapping, degrees-per-detent.
5. Face/shoulder mapping (§2.5), with **B reserved**.
6. Glyph set via `sfSymbolsName` (§2.6), and fix the `✦ HAPTICS` visibility rule (§2.7).
7. Controller haptics: engage clunk + per-detent tick, per-handle locality.
8. The stick pill (§2.8).

★ **Verify the minimum control set (§2.4) explicitly** — unplug everything but a single Joy-Con half and
confirm the app is still fully operable.

---

## 4.2 ★ STILL TO BUILD (keyboard phase)

1. ★★ **THE FULL SHORTCUT LIST IN THE MAIN MENU** — Stuart, raised repeatedly and still not
   built: *"in the main menu there will be a full keyboard shortcut popup that can be scrolled
   with all the hints on it."* Everything else teaches ONE surface's keys at the point of use —
   the picker's subtitle, the decoder box's header line, the [H]z/[T]une/[P]/[D] caps. There is
   nowhere that shows the whole scheme, which is what someone wants after the first day rather
   than during the first minute.
   ★ **THE SHAPE (Stuart):** a BUTTON in the main menu, which opens a POPUP containing a
   SCROLLABLE LIST of the shortcuts. Not a settings page and not a panel to navigate — a
   reference you open, read, and dismiss.
   ★ It should be the reference the per-surface hints are NOT: scrollable, complete, and
   organised by WHERE a key works rather than alphabetically — the question being answered is
   "what can I do from here", not "what does K do".
   ★ It is itself keyboard-reachable, so it needs the same treatment as everything else: arrows
   scroll it, Esc closes it, and it should say so.
2. `GainSlider` (RTL-SDR gain) is a custom component, not an RN `Slider`, so none of the slider
   work reaches it. Reachable by the RTL-TCP path on iOS as well as local USB on Android.
3. FM-DX **Android Auto** controls — see `BRIEF-fmdx-backend-adapter.md`.

## 5. Open decisions — Stuart's calls, deliberately not made

1. **One stick pill or two?** If the right stick zooms, a rising *number* is the wrong illustration for it.
   One generic "rim then rotate" pill, or a per-stick variant?
2. **The dwell and count thresholds** (~400 ms, 2–3 attempts) are guesses. Same class as `HOLD_MS` — they
   want a thumb, not an argument.
3. **Is the flank set fixed at three, or per-device?** A keyboard has no shoulders, so keyboard-only mode
   *could* show all four buttons. Recommended: **fix at three** with step rate as a pill readout, so the TV
   layout does not change shape when a controller connects or drops — a layout that reflows on a Bluetooth
   event reads as a fault from across a room. But it means keyboard users reach step rate by shortcut
   rather than a visible button.
4. **Do the drums/keys return to the TV when a controller is connected?** See the exclusion in
   `BRIEF-inputs-shack-mode-mac.md`: they are barred as *dead elements* on a non-touch screen, but a
   controller makes them actuable. ★ Largely mooted by §1 — with both schemes always live there is no
   *mode* to communicate, so the widget has no informational job. Lean: keep them off, glyphs only.

---

## 6. Related documents

- `BRIEF-inputs-shack-mode-mac.md` — the pointer/keyboard/shack-mode design this grew out of; holds the
  permanent-pill TV layout and the tuning-controls exclusion.
- memory `external_display_shack_mode` — the tvOS findings (Siri Remote as a rotary device, the App Review
  floor) and where this design was worked out before being extracted here.
- memory `hifi_tuner_keys_built`, `feedback_zoom_drum` — the two control schemes being mapped onto.
- memory `watch_split_jr_buddy` — why the watch is out of scope for this one codebase.
