# BRIEF — Light that moves with the device, a chosen light angle, and VFD filament wires

Follow-up to `BRIEF-faceplates.md` (built). Two polish items, plus the settings they need.

> "VFD glass wires and tilt related lighting … maybe if reduce animations is enabled the user can choose the
> lighting angle … on the Mac with no motion detector the lighting option is just visible by default …
> **but only if we can pull it off with minimal performance cost.**" — Stuart

★★★ **The performance condition is part of the spec.** §5 sets a budget and a fallback ladder. If tilt lighting
can't meet the budget on a device class, that class gets the fixed light angle instead. Don't ship it slow.

Reference: `assets/lighting/` (JPEG) and `Deck.mockup.dc.html` (props `lightAngle`, and the `wire` overlay).

---

## 0. Build order (separate commits)

| # | Work | § |
|---|---|---|
| 1 | VFD filament wires (static, no settings) | 1 |
| 2 | One shared light angle. Split `ChassisPlate`'s lighting into its own layer driven by it. | 2 |
| 3 | MOTION EFFECTS setting + `effectiveMotion()` (replaces direct `useReduceMotion()` use) | 3 |
| 4 | LIGHT ANGLE setting (fixed angles) | 4 |
| 5 | Tilt lighting, measured against the budget | 5 |

Item 1 and items 2–4 ship on their own. Item 5 only ships if it passes §5.

---

## 1. VFD filament wires (ref `lighting/W1`, `W2`)

A real VFD has a few hair-thin tungsten filaments stretched horizontally across the **whole** glass, in front
of everything. The mesh already on the RDS pictogram is the *grid*, which belongs to each lit area. That's
correct, and it stays. The filaments are separate.

- **Where:** every VFD window.
  - the VTS strip on `dot` and `seg`;
  - the frequency window on `dot` and `seg`;
  - the status display on `silver` and `black`, which is always dot-matrix there.
  Not on Nixie, not on the Hyperlegible frequency, not on the LED VU, not on the default chassis footer.
- **Count and position:** `n = max(2, round(h / 22))` wires, evenly spaced. For today's windows that's 2, at
  ⅓ and ⅔ of the window height. Space them by **height, never width**: the Mac's long landscape strip still
  gets 2.
- **Line:** exactly **one device pixel** (`1 / PixelRatio.get()` pt), `rgba(0,0,0,0.50)`, with a one-device-pixel
  highlight directly above at `rgba(255,255,255,0.06)`. Over a lit segment the dark line reads, because the wire
  shadows the phosphor. Over unlit glass the hairline highlight reads. Edge to edge of the glass, clipped by the
  window's corner radius.
- **Layering:** above the ghost layer, the lit segments and sprites, and the RDS pictogram. It's the frontmost
  thing in the window.
- **Cost: zero per frame.** One memoised Skia canvas per window, redrawn only when the window's size changes
  (reuse `useBoxSize()` from `VfdParts.tsx`). Put it in `VfdParts.tsx` as `VfdFilaments`.
- Static: Motion Effects doesn't touch it.

> ★★ **TRAP: layer order.** Don't add the wires to the existing ghost canvas (`GhostGrid` / the `SegDigits`
> static layer). Those sit *under* the lit sprites, so the glow would paint over the wires. That's the reverse
> of a real tube, where the wire shadows the glow. The wires need their own canvas on top.

> ★★ **TRAP: sub-pixel smear.** Positions like ⅓ of 40 pt on a @3x screen land between pixels, and Skia
> antialiases a 1-px line into a 2-px grey smear that reads as a scan line. Snap each wire's y to the device
> pixel grid: `Math.round(y * PixelRatio.get()) / PixelRatio.get()`.

> ★ **TRAP: restraint.** The mockup values are the maximum. If they read as lines at arm's length on a
> phone, lower the dark line's alpha, never raise it. You should only see them when you look for them.

---

## 2. One light angle for every lit surface

Today the sheen direction is baked into each surface:
- `ChassisPlate.tsx` uses `cssAngle(104, …)` for the plate and `cssAngle(112, …)` for black's gloss panel;
- the radial hot-spot is at a fixed `28% -10%`;
- the screw highlight offset is fixed.

A light that moves has to move **all of them together**, or the deck reads as parts from different photos.

- **One value:** `lightAngle` in degrees, in CSS gradient convention (0 = upward, 90 = rightward), owned by
  `FaceplateContext`. Default **104** = today's look exactly, light from the left (Stuart's Mac landscape bar).
- **Derived from it:**
  - plate sheen: `cssAngle(lightAngle)`;
  - black gloss reflection: `cssAngle(lightAngle + 8)`, keeping today's 8° offset;
  - radial hot-spot x: `50% − 22% × sin(lightAngle)`, with y unchanged at −10%. At 104° that gives today's 28%.
  - screw highlight centre: offset 0.35 r toward the light, i.e. in direction `lightAngle + 180°`. Today's
    `(0.35, 0.30)` comes from 104°.
- **Not derived:**
  - the bottom shade, because it's gravity, not the lamp;
  - the dome caps' top chamfer, because overhead room light always catches a cap's top edge;
  - the lips.
- **Surfaces:** everything that uses `ChassisPlate`: the deck, `DecoderShell`, `PopupShell` (including the
  CONTROL CUSTOMISATION pane), `DomeKey` caps and drum wells. Default chassis: no metal, so nothing changes.
- **Architecture: split the plate's lighting into its own layer.** `ChassisPlate` is one static canvas today
  (grain, lighting, lips, screws), drawn once and cached. Keep it, minus the lighting. Add a thin `PlateLight`
  canvas over it holding only the sheen gradient and the radial hot-spot. Its start, end and centre are
  `useDerivedValue`s of a Reanimated `SharedValue<number>` angle. Skia reads SharedValues on the UI thread, so a
  moving angle costs one gradient fill per frame and **no React render**. A still angle costs nothing.

> ★★★ **TRAP: never put a moving angle in React state.** At 30 Hz that re-renders every consumer of
> `FaceplateContext`, which is the whole deck. The **stored** setting (§4) is React state. The **live** angle
> (setting + tilt) is a SharedValue, and only `PlateLight` canvases read it.

---

## 3. MOTION EFFECTS setting

A new row in CONTROL CUSTOMISATION › **FEEL**, beside STEADY LEDS: `MOTION EFFECTS · ON / OFF`.

- **Follows the OS until the user picks:** iOS Reduce Motion, Android Remove animations. After a pick, the pick
  is stored and wins. This is exactly the Transparency Effects pattern:
  - `motionEffects` + `motionExplicit` in `FaceplateSettings`;
  - an `effectiveMotion()` resolver beside `effectiveTransparency()`;
  - the keys show what's on screen, and the auto value is never written back as if chosen.
- **OFF turns off decorative motion only:**
  - tilt lighting, which falls back to the fixed angle (§4);
  - the LED VU's edge shimmer, so steady LEDs are forced;
  - the needle's overshoot, so it's critically damped;
  - any future power-on effects.
- **It never touches motion that carries information:** the VFD's stepped scroll, needles following the signal,
  dome-key presses, Nixie digit changes and afterglow.
- STEADY LEDS stays its own row. MOTION OFF forces steady LEDs; steady LEDs don't force motion off. Someone
  sensitive to PWM may still want the tilt light.

> ★★ **TRAP: one resolver.** `LedVu.tsx` and `EdgeMeter.tsx` call `useReduceMotion()` directly today. Once the
> setting exists that bypasses the user's pick. Route both, and the tilt light, through `effectiveMotion()`
> from `FaceplateContext`. Keep `useReduceMotion()` only as the OS input to the resolver.

> ★★ **TRAP: Reanimated obeys the OS by itself.** `withSpring` / `withTiming` default to `ReduceMotion.System`.
> So with the OS setting on and the app set to MOTION ON, decorative animations stay flattened anyway. Pass
> `reduceMotion` explicitly on every decorative animation: `ReduceMotion.Never` when motion is on,
> `ReduceMotion.Always` when it's off. `EdgeMeter` already imports `ReduceMotion`; copy that.

---

## 4. LIGHT ANGLE setting (fixed angles) (ref `lighting/A1` silver, `A2` black)

A new row in CONTROL CUSTOMISATION › **FACEPLATE**, after SIGNAL METER:

| Key | CSS angle | Light from |
|---|---|---|
| LEFT (default) | 104° | the left, slightly above. **Today's look.** |
| TOP-LEFT | 135° | upper left |
| TOP | 180° | overhead |
| TOP-RIGHT | 225° | upper right |
| RIGHT | 256° | the right, slightly above. Mirror of LEFT. |

There are no angles from below: hardware is never lit from below, and it reads as wrong.

- **Shown only when it's the light in use:**
  - the chassis is `silver` or `black` (no metal on `default`; AGENTS.md says no dead controls), **and**
  - tilt isn't driving the light, because the device has no usable motion sensor **or** MOTION EFFECTS is off.
- **On the Mac it's simply there by default** (Stuart). `isMac` is already a device signal
  (`constants/transparency.ts`); treat Mac as "no motion sensor". The same goes for TV and any device whose
  sensor probe fails.
- With tilt on, the row is hidden and tilt swings around the stored angle (default LEFT). A user who picked
  TOP with motion off and then turns motion on gets tilt around TOP.
- Stored in `FaceplateSettings` as `lightAngle: 'left' | 'topLeft' | 'top' | 'topRight' | 'right'` and mapped to
  degrees in `faceplate.ts`. It's device-local like the rest of the faceplate.

---

## 5. Tilt lighting, and the performance budget

### 5.1 Behaviour
- **Input:** `expo-sensors` `DeviceMotion` (rotation + gravity) at 30 Hz (`setUpdateInterval(33)`).
  It's a new dependency; per AGENTS.md, read the Expo SDK 56 docs for it first.
- **Relative, not absolute:** the reference is how the phone is being *held*. Keep a baseline attitude that
  re-centres slowly (time constant ≈ 4 s). A phone lying on a desk or held steady settles to the stored angle
  instead of parking the highlight off the edge.
- **Mapping, subtle on purpose:**
  - roll (left–right tilt) relative to the baseline, ±20°, swings the angle ±30° around the stored angle;
  - pitch, ±20°, slides the sheen band ±10% along the gradient by offsetting the gradient positions.
  Clamp at those limits. It should feel like light catching metal, not a moving spotlight.
- **Smoothing:** a low-pass filter on the input (τ ≈ 150 ms). Write to the SharedValue only when the result has
  moved more than 0.5°. Cap writes at 30 Hz. A still hand then produces almost no frames.
- **What moves live:** the large plates only, i.e. the deck plate and whichever `PopupShell` / `DecoderShell`
  is open.
- **What follows once the light settles:** dome caps, drum wells and screws. Update them when the angle has
  changed by more than 3° and held for 200 ms. They're small and many, and the eye doesn't track their
  highlights live.
- **The sensor runs only while all of these hold:**
  - `silver` or `black`, MOTION EFFECTS on, and the sensor is available;
  - the app is in the foreground with the screen on;
  - the deck is visible (not HIDE CONTROLS).
  Unsubscribe otherwise. The sensor is never started on `default`.

### 5.2 Budget: measure on the slowest supported device of each platform
Use the Samsung Xcover 4S, the oldest supported iPhone, and a Mac. Compare against the same build with tilt off:

| Measure | Budget |
|---|---|
| UI-thread frame time while tilting | ≤ +0.5 ms per frame |
| GPU frame time while tilting | ≤ +0.5 ms per frame |
| JS thread | **no** per-frame work (0 React renders from tilt) |
| CPU, phone held still for 60 s | ≤ +1 % |
| Battery, 30 min, screen on, waterfall running | no measurable difference beyond noise |

### 5.2a How to measure (the harness, built 2026-10-02)
The same build is measured with tilt OFF and ON. Switch modes with a link (`services/tiltLight.ts`):
`vibesdr://debug/tilt/off`, `…/synthetic`, `…/auto`. `synthetic` feeds a deterministic slow tilt through the
**same** path as the sensor, so runs are repeatable without moving the phone. A relaunch is always `auto`.
While a mode is set, the app logs `[tilt] mode=… writes=… providerRenders=…` every 10 s:
`providerRenders` must not rise with tilt (the "0 React renders" line).

**Android (XCover):** `scripts/measure-tilt-android.sh [seconds] [adb-serial]`. Open the app on a receiver,
silver/black chassis, MOTION EFFECTS on, phone flat and still. It runs OFF, SYNTHETIC and AUTO (real sensor,
phone still) and prints each figure against the budget.

**iPhone (oldest supported) — Instruments, on a Release build from Xcode on the Mac:**
1. Open the app on a receiver as above, phone flat on the desk.
2. In Safari or Notes on the phone, open `vibesdr://debug/tilt/off`; switch back to the app.
3. Xcode ▸ Open Developer Tool ▸ Instruments ▸ **Animation Hitches** (UI/GPU frame time) — record 60 s.
   Then **Time Profiler** (CPU %) — record 60 s.
4. Open `vibesdr://debug/tilt/synthetic`; repeat step 3. Then `…/auto` with the phone still; repeat the CPU run.
5. Compare: hitch time ratio and frame durations (UI ≤ +0.5 ms, GPU ≤ +0.5 ms), CPU (still) ≤ +1 %.
   The `[tilt]` lines are in Xcode's console (or Console.app, process VibeSDR).
Record the rung each device ends on in the ladder comment in `src/constants/tiltLight.ts`.

### 5.3 Fallback ladder (apply in order until the budget is met)
1. Dome caps, drum wells and screws stop following tilt; only the large plates move.
2. Cap writes at 20 Hz.
3. Disable tilt for that device class. It gets the fixed LIGHT ANGLE row instead, as on the Mac.

Record which rung each tested device ended on, in a comment next to the device check.

> ★★ **TRAP: Android without a gyroscope.** Some cheap Android devices report `DeviceMotion` with gravity but
> no rotation rate. Use the gravity vector alone; it's smoother but laggier, and fine for light. If neither is
> available, treat the device as "no sensor" so the LIGHT ANGLE row shows.

> ★ **TRAP: orientation.** Roll and pitch swap meaning between portrait and landscape. Map them in screen
> coordinates (`useWindowDimensions` orientation) so tilting the right edge down always swings the light the
> same way on screen.

---

## 6. Acceptance
1. VFD wires: exactly one device pixel, crisp (no grey smear) on @2x and @3x, in front of lit segments, present
   on every VFD window in §1 and on nothing else. No change in frame time (static canvas).
2. With LIGHT ANGLE = LEFT and motion off, every surface is pixel-identical to today's build.
3. Changing LIGHT ANGLE moves the sheen, the radial hot-spot, the gloss reflection and the screw highlights on
   the deck, an open popup, a decoder box and the dome keys, all together. The bottom shade and cap chamfers
   don't move.
4. The LIGHT ANGLE row is visible on the Mac by default. On a phone it's hidden while tilt is on, and shown with
   MOTION EFFECTS off. It's never shown on the default chassis.
5. MOTION EFFECTS follows the OS until picked; after a pick, the pick wins in both directions, including over
   Reanimated's own OS handling.
6. Tilt meets the §5.2 budget on each tested device, or that device falls back per §5.3 and says so in code.
7. The sensor is not running (verify the subscription is gone) on default chassis, in the background, with the
   screen locked, with motion off, and with controls hidden.

## Reference assets (`assets/lighting/`)
- `A1-light-angles-silver.jpg`, `A2-light-angles-black.jpg`: the five fixed angles on each chassis.
- `W1-filaments-decks.jpg`: VCR, dot-matrix, and Hyperlegible (status display only).
- `W2-filaments-closeup.jpg`: the wires over lit and unlit glass at 2×.
- `Deck.mockup.dc.html`: `lightAngle` prop and the `wire` overlay, the source of the numbers.
