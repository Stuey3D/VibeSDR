# BRIEF: Menu Relocation — decompose the mega `MenuSheet`

**Project:** VibeSDR (React Native / Expo — built from native projects, NO `expo prebuild --clean`)
**Author:** Stuart Carr (Stuey3D)
**Branch:** `experimental` (sits on top of the new "‹ Servers" back-to-instances control already in `MenuSheet`)
**Status:** Ready for implementation

---

## 1. Goal

Real-user feedback says the main controls sheet (`MenuSheet`) is a dumping ground. This brief breaks it up and moves each block to the place it logically belongs, so tuning/station work happens where you tune and signal-analysis work happens in the demodulator menu. `MenuSheet` is left as a small **display** menu.

**This is a relocation brief, not a redesign.** The overwhelming majority of the work is: pick an existing, working component up out of `MenuSheet` and render it under a new parent, behaviour unchanged. There are exactly **three** things that are *not* pure relocation (§5). Everything else is cut-and-paste.

**End state of `MenuSheet`:** SPECTRUM / WATERFALL (MIN / – ZOOM / + ZOOM / MAX), LOCKED, DISPLAY SETTINGS, HIDE CONTROLS. Nothing else.

---

## 2. Principle

Where an item moves, its internals do **not** change. Same props, same handlers, same DSP, same session start/teardown, same copy. If you find yourself rewriting a moved component's behaviour, stop — it's a relocation. Complexity stays in the engine; the menu just gets smaller.

---

## 3. Scope

### In scope

1. Move the VTS skip control into `FreqModal` (§4.1).
2. Move search + bookmarks into `FreqModal` as a second card mode (§4.2).
3. Move client decoders + their settings into `ModeSelector` (§4.3).
4. Move Server Maps into `ModeSelector` (§4.4).
5. Move the DAB speed control into the decoder-box header (§4.5).
6. Compose the mode readout label `USB` → `USB: RTTY` (§5.1).
7. Render the OWRX DAB service list in the existing decoder box (§5.2).
8. Re-anchor `FreqModal` to the keyboard, decoupled from card-open (§5.3, §6).

### Out of scope — do NOT implement or "improve" while you're in here

- Any change to decoder DSP, decoder settings contents, or `DecoderClient` start/teardown logic.
- Any change to the DAB **speed-correction option values or their explanatory subtitle** — these are already defined and working in `MenuSheet` today; you are moving the existing control verbatim, not authoring it.
- Any change to `MapOverlay` launch behaviour — the maps buttons fire the same overlay, just from a new parent.
- Any redesign of the VFO drum, `ControlsBar` layout, or `ModeSelector` mode grid / passband.

---

## 4. Relocations (pure cut-and-paste)

### 4.1 VTS skip control → `FreqModal` header — `FreqModal.tsx`, `VTSBar.tsx` / `VTSDisplay.tsx`, `MenuSheet.tsx`

Remove the NEARBY STATION card (station name flanked by ◄ ►) from `MenuSheet`. Render the same skip control as a row **above the frequency number** in `FreqModal`: `◄  <nearby station name>  ►`.

- The always-on-screen `VTSBar` is **untouched** — only the in-`MenuSheet` duplicate moves.
- **Known caveat (do not "fix"):** skip is deliberately disabled on FM-DX (one shared physical tuner — see README). The guard that hides/disables skip on FM-DX must travel with the relocated row. Do not re-enable it in its new home.
- This row is conditionally omitted on small layouts — see §7.

### 4.2 Search + bookmarks → `FreqModal` second mode — `FreqModal.tsx`, `MenuSheet.tsx`

`FreqModal` becomes a two-mode card. Add a `Bookmarks / search` button to the Tune layout. Pressing it swaps the card to Bookmarks mode:

- The numeric entry is replaced by the **search field** (bookmarks + band plan, live EiBi results) — the same field currently in `MenuSheet`.
- Beneath it: the **EiBi ON/OFF toggle** and the bookmark options — **save current / delete / import / export** — all lifted from `MenuSheet` as-is.
- Provide a clear way back to Tune mode. Recommended: a two-segment header `Tune | Bookmarks` so both directions are always visible (the single-toggling-button version hides the return path, which bites hardest on SE). Author's call, but pick one explicitly.

Remove the search field, EiBi toggle and BOOKMARKS button from `MenuSheet`.

Note: `ProfilePicker` already renders inside `FreqModal` when `profiles.length > 0` — it coexists with both modes; don't disturb it.

### 4.3 Client decoders + settings → `ModeSelector` — `ModeSelector.tsx`, `DecoderPanel.tsx`, `MenuSheet.tsx`

Move the CLIENT DECODERS buttons out of `MenuSheet` into `ModeSelector`, as a row **below the mode grid and passband**. Selecting a decoder expands its settings in an outlined callout that **points up at the pressed button** — reuse `Coachmark.tsx`, which already provides that pointer/outline. Selecting starts `DecoderClient`; toggling the same button off collapses the callout, tears the decoder down, and reverts the label (§5.1). Only one decoder active at a time (they are mutually exclusive submodes — unchanged from today).

Parent-mode coupling is unchanged: a decoder already sits on its correct demod (e.g. RTTY on USB) exactly as it does now. Do not add new coupling logic.

### 4.4 Server Maps → `ModeSelector` — `ModeSelector.tsx`, `MapOverlay.tsx`, `MenuSheet.tsx`

Move SERVER MAPS (HFDL / Digital / CW) out of `MenuSheet` into `ModeSelector`, **alongside the decoders row** — they're the same "what's on this signal" family and belong together. Each button fires the existing `MapOverlay` unchanged.

### 4.5 DAB speed control → decoder-box header — `MenuSheet.tsx`, `DecoderPanel.tsx`

Move the existing **DAB speed control** (its disclosure button, its options, and its explanatory subtitle) out of `MenuSheet` into the **header of the decoder box**, above the DAB service list (§5.2). This is a verbatim move of a control that already exists and works — same options, same copy. It sits collapsed and discloses on tap, same as it does now.

---

## 5. Rewires (NOT pure relocation — the only three)

### 5.1 Mode readout label composition — `ControlsBar.tsx`

The mode box in the frequency/signal readout composes the active decoder onto the demod:

- No decoder active → `USB` (as today).
- Decoder active → `USB: RTTY`, `USB: FAX`, etc. — `<parentMode>: <decoderName>`.
- On decoder toggle-off → revert to `<parentMode>`.

This is a label composition reading the existing active-decoder state, not a new state machine.

### 5.2 OWRX DAB service list in the decoder box — `DecoderPanel.tsx`, `OwrxAdapter.ts`, `StationLogo.tsx`, `VTSBar.tsx`

DAB on OWRX is **not** a client decoder (the server sends already-decoded PCM), so it does **not** go in the `ModeSelector` decoder grid. When an OWRX **DAB profile** is open, the existing decoder box shows the DAB **service list**:

- Scrollable list; each row = `StationLogo` + service name.
- Tap a row to switch service — the **same select call the Apple Watch DAB screen already makes**. The service list already arrives via `OwrxAdapter` (the watch consumes it); the phone simply hasn't been rendering it. Wire that existing data source into the decoder box; do not build a new fetch.
- The DAB speed control (§4.5) sits in this box's header, above the list.
- `VTSBar` stays in its normal place — **between the decoder box and the controls** — unchanged.

### 5.3 `FreqModal` keyboard-anchored + decoupled — see §6.

---

## 6. `FreqModal` keyboard behaviour (the one genuinely fiddly section)

### 6.1 Confirmed current state

- `FreqModal` is a real RN `<Modal>` (`transparent`, `animationType="fade"`), anchored bottom (`justifyContent: 'flex-end'`), with the numpad **auto-opening** on card-open.
- Android is a separate Modal window, so `adjustResize` doesn't shrink it — the code currently hand-tracks height with `keyboardDidShow`/`keyboardDidHide` and pads `paddingBottom: 16 + kbHeight`. This is the janky path.
- Confirmed available: `windowSoftInputMode=adjustResize` is set in `AndroidManifest.xml`; `react-native-reanimated@4.5.0` is installed (so `useAnimatedKeyboard()` is available).

### 6.2 Recommended structural change

**Convert `FreqModal` from RN `<Modal>` to an absolutely-positioned overlay in the app root**, and drive its vertical position with Reanimated `useAnimatedKeyboard()` (continuous frame value, matched easing, both platforms). Rationale: the centre↔anchor animation below tracks a keyboard that shows/hides on focus, and that is the historically flaky combination *inside* RN `Modal`. Going to an overlay makes the tracking straightforward **and retires the separate-window `keyboardDidShow` hack** the component currently carries. (Fallback if the overlay conversion is deferred: keep `Modal` but replace the `keyboardDidShow`/`Hide` tracking with `useAnimatedKeyboard`, re-declaring the keyboard provider inside the modal — accept residual jank.)

### 6.3 Decouple the keyboard from card-open

Remove the auto-focus. The card must open **keyboard-down**. The keyboard is raised only when the user taps a field.

Flows:
- Tune: tap frequency → card appears **centred, VTS row, no keyboard** → tap the number → numpad rises, card animates up to anchor.
- Bookmarks: tap frequency → centred card → `Bookmarks / search` → card swaps to Bookmarks layout, **still centred, still no keyboard** → tap search → alphanumeric keyboard rises, card anchors.

### 6.4 Two rest states

- **Keyboard down (home):** card centred on screen, topmost layer. This is where the card returns whenever the keyboard is dismissed. A bookmarks results list grows **upward out of the top of the card** and scrolls when it reaches the top of the screen. No keyboard maths in this state.
- **Keyboard up:** card leaves centre and anchors a small gap above the measured keyboard top. The bookmarks list grows upward out of the card, capped at `screen − keyboardHeight − cardChrome − gap − safeArea`, and scrolls past that cap. The input is pinned just above the keyboard and is **never** the element pushed off-screen — the list is the only thing that absorbs the squeeze.

### 6.5 Small-space degradation ladder (single ordered pass off the measured frame)

Evaluate in order; stop at the first that yields enough room:

1. **Full** — card fits above the keyboard. Normal behaviour.
2. **Drop VTS** — the skip row (§4.1) is the first thing sacrificed (`VTSBar` still covers skip). Re-measure. If now fits → normal behaviour resumes.
3. **Occluded** — if dropping VTS still doesn't clear the keyboard (e.g. half-height Gboard on a short screen), the card sits **under** the keyboard. A strip on the card shows a dismiss-to-reveal message ("Dismiss keyboard to see results" for bookmarks; the frequency equivalent). The results list is already populated behind the keyboard — dismissing just uncovers it.

Give the computed list `maxHeight` a floor: if the space left is below ~2 rows, clamp to that minimum and scroll rather than collapsing to nothing.

### 6.6 Enter / submit behaviour

One derived flag: `occluded = boxBottom > keyboardTop` (from the same frame maths). Submit handler:

- **Bookmarks mode** → always dismiss the keyboard, **keep the results on screen** (occluded or not). Dismissing returns the card to centre; the list switches from the anchored upward-list to the centred upward-list.
- **Frequency mode, not occluded** → **auto-tune**, no second tap (preserves today's one-tap behaviour when the box is visible).
- **Frequency mode, occluded** → dismiss the keyboard to reveal the box; the user then reads the entry and taps TUNE.

### 6.7 Android Gboard-huge note

Because the anchor is off the **measured** keyboard frame, a giant/floating/one-handed Gboard doesn't break placement — it just shrinks the room, which the ladder (§6.5) and list cap (§6.4) already handle by dropping VTS, then falling back to occluded, then scrolling. This requires `adjustResize` (confirmed set). **Do not** switch any activity to `adjustPan` — the frame maths lies under `adjustPan`.

---

## 7. Small-screen rules (consolidated — iPhone SE is the layout floor)

- **`FreqModal`** carries the keyboard, so its space-savers are the ladder in §6.5: drop the VTS skip row first, then fall back to the occluded/dismiss-to-reveal behaviour. `VTSBar` on the main screen always covers skip, so dropping the row strands nothing — **guard:** the omit only holds while `VTSBar` is actually present; if any state shows the modal without the on-screen VTS, the row must return.
- **`ModeSelector`** has **no** keyboard, so it owns full sheet height: when its content (mode grid + passband + decoders + maps + an expanded decoder callout) overflows on SE, the sheet **scrolls**. Nothing shrinks; it just becomes scrollable when it must. Larger screens render static as today.

---

## 8. Testing plan

Devices (per README floor): **iPhone SE (2nd gen)** — layout floor; **Moto G35** — set Gboard to max height + floating mode for §6.5/§6.7; **iPhone 17 Pro Max** — headroom case.

1. **Relocations present & functional in new homes:** skip row tunes/skips in `FreqModal`; search/EiBi/save/delete/import/export work in Bookmarks mode; decoders start/stop + settings callout points at the right button in `ModeSelector`; maps launch `MapOverlay`; DAB speed control discloses in the decoder-box header.
2. **`MenuSheet` residue:** only SPECTRUM/WATERFALL, LOCKED, DISPLAY SETTINGS, HIDE CONTROLS remain.
3. **Label:** decoder on → `USB: RTTY`; off → `USB`.
4. **DAB (OWRX):** service list renders with logos, tap switches service, VTS sits between box and controls, speed control in header.
5. **Keyboard decoupling:** card opens with no keyboard in both flows; keyboard rises only on field tap.
6. **Ladder:** force each tier on SE + huge-Gboard G35 — full / VTS-dropped / occluded-with-strip.
7. **Enter:** freq visible → auto-tune; freq occluded → dismiss-to-reveal then TUNE; bookmarks → dismiss keeps results, card returns to centre.
8. **FM-DX guard:** skip row disabled/hidden while connected to an FM-DX server.
9. **Regression:** `ProfilePicker` still renders in `FreqModal` when profiles exist.

---

## 9. Do NOT touch

- DAB speed values / subtitle copy (move verbatim).
- Decoder DSP, decoder settings contents, `DecoderClient` lifecycle.
- `MapOverlay`, VFO drum, mode grid / passband internals.
- `VTSBar` on the main screen (position and behaviour unchanged).
- Any activity's `windowSoftInputMode` other than confirming `adjustResize` stays.
