# BRIEF: Controller Keyboard

**Status:** Refined against the V12 code (2026-10-20). Stuart's draft text is kept, and his decisions stand. Where the
code changes a detail, a **Refined:** note says what and why. Where his draft conflicts with his own pad scheme
(`gamepad_mapping`), the conflict is **flagged for his decision** (§6, §13), never silently overridden.
**Priority:** **MUST.** Stuart confirmed on device (2026-10-20): *"right now even with the on screen keyboard there is no
way of controlling the system keyboard with the controller."* Every text field is a dead end for a pad today. This is
what unblocks text entry, not a nice-to-have.
**Platforms:** iOS, Android, macOS, the web page (web/client), tvOS (future native port)
**Goes with:** [CONTROLLER-AUDIT.md](CONTROLLER-AUDIT.md), which lists every surface. Phase 1 below is its MUST list.

## Problem

Game controller support lets VibeSDR be driven from the sofa (e.g. iPhone AirPlay-mirrored to a TV), but text entry still needs the touchscreen:

- **iOS/iPadOS:** the system keyboard cannot be operated by a game controller (Apple has confirmed there is no built-in support). **Refined:** confirmed on device by Stuart; and in our code nothing sends pad input to it — while a field has the cursor only Enter and Back pass (`src/utils/keyActions.ts:89`).
- **Android:** gamepad behaviour with the system keyboard varies by device and keyboard app, so it can't be relied on. **Refined:** our bridge consumes every pad key in the activity (`VibeGamepad.kt`), so a pad never types into the IME either.
- **macOS:** there is no on-screen keyboard a controller can drive.
- **tvOS:** the system keyboard works, but should match the other platforms for consistency.
- **Refined — the web page too:** the handheld goal (Anbernic/R36S in a `cog`/WPE kiosk) has **no on-screen keyboard at all**, and the web pad layer just calls `el.focus()` on a field (`web/client/src/gamepad.ts:200`). The web needs this as much as the app.

## Goal

A shared, controller-driven on-screen keyboard component with two layouts, which looks and behaves the same on every platform.

## Core rule: when to show it

| Last input source | Text entry uses |
|---|---|
| Touch | **System keyboard**, unchanged |
| Physical keyboard | Typed straight into the field or overlay |
| Game controller / Siri Remote | **ControllerKeyboard** overlay |
| tvOS, password field | **System secure keyboard**, always (see tvOS) |

The custom keyboard **never** replaces the system keyboard for touch users. The system keyboard is what provides accessibility sizing, VoiceOver/TalkBack typing, dictation, swipe typing, third-party keyboards, password AutoFill and input methods for other scripts.

Track the last input source globally, e.g. `lastInputSource: 'touch' | 'controller' | 'keyboard'`.

**Refined — this already half exists; extend it, do not add a second one:**
- `PanelNav.tsx:83` keeps a global `keyboardMode` boolean: set by any key (a pad counts), cleared by
  `noteTouchInteraction()`; read through `useKeyboardMode()` by ~15 surfaces. Every bus event already carries
  `pad: true` when it came from a controller (`keyActions.ts:75`); the server list already tells the two apart for its
  hint (`InstancePickerScreen.tsx:1925`).
- **Change:** replace the boolean with `inputSource: 'touch' | 'keyboard' | 'pad'` in PanelNav, set centrally in
  `hardwareKeys.ts` (`bus.dispatch` knows `pad`). `useKeyboardMode()` stays as `inputSource !== 'touch'` so nothing
  else changes; add `useInputSource()`.
- **Trap found:** touch is detected only where a surface calls `noteTouchInteraction` from its own `onTouchStart`.
  A touch on anything else leaves the app in "pad" mode, and the next field would open ControllerKeyboard under a
  hand. Add one root-level capture in `App.tsx` (`onStartShouldSetResponderCapture` that notes the touch and returns
  false) so touch is global.
- **Android "Physical keyboard" row is false today:** a hardware keyboard never reaches the bus on Android
  (`VibeGamepad.kt:96-99`), so there it simply types into the TextInput (fine) — but the one-scheme navigation is
  iOS/Mac only. Separate SHOULD in the audit.

## Component

```tsx
<ControllerKeyboard
  layout="numeric" | "qwerty"
  mode="frequency" | "search" | "text" | "password"
  initialValue?: string
  onChange?: (v: string) => void   // live updates (search)
  onSubmit: (v: string) => void
  onCancel: () => void
/>
```

- Shown as a full-screen modal overlay, so it mirrors cleanly over AirPlay.
- Displays the value in a `<Text>` with a drawn caret, **never** a `TextInput`, so the system keyboard can't appear.
- Layouts are defined as data (`layouts/numeric.ts`, `layouts/qwerty.ts`) and shared by all platforms. Only the input adapter differs per platform.
- Styled to the active faceplate theme, with key labels in the selected display font (see Keycap fonts).

**Refined — where the pieces go, reusing what exists:**

| Brief piece | Lives in | Reuses |
|---|---|---|
| Layouts as data | `src/utils/kbLayouts.ts` (pure, no RN imports — the web imports it, like `padGuide.ts`) | Key boxes in key units → `pickNext` (`src/utils/gamepadNav.ts:18`) does focus movement by geometry, so wide keys need no special case (§ Input handling). |
| Editing rules | `src/utils/controllerKeyboard.ts`: a pure reducer `kbReduce(state, action)` — caret, insert, backspace, shift / caps lock (double press), symbols layer, accents, numeric rules | FreqModal's own rules: digits and one point (`FreqModal.tsx:315`), units, `lockUnit`, the range message. |
| The overlay | `src/components/ControllerKeyboard.tsx`, drawn with `PopupShell` (plate, `PopupKey`, `PopupWindow`) like every pop-up | `useNavFocus()` ring (white on green controls, focus green otherwise), `NAV_ADJUST` amber. |
| Keys in | `useRepeatingKeys` at a priority above PadGuide's 100 (e.g. 1000), consuming every key | Repeat law `REPEAT_DELAY_MS 380 / REPEAT_MS 110 / REPEAT_FAST_MS 55` (PanelNav). |
| Opening it | A `usePadField()` helper: the field's nav slot calls `open()` → ControllerKeyboard when `inputSource === 'pad'`, else `ref.focus()` exactly as today | The precedent is `TvTextInput.tsx` — a box showing the value that opens an editor on OK. ControllerKeyboard is the pad's version of that sheet. (TvTextInput must stay Lite-safe, so it gets an injection point rather than importing PopupShell.) |

**Refined — "never a TextInput" checked against the real fields:**
- It holds *inside the overlay*. The page's own field stays a TextInput for touch users; the overlay hands its result
  back through the field's `onChangeText` (live for search) and then the field's commit (FreqModal's `confirm`, a
  PIN's submit, a search's results).
- **FreqModal frequency** (`:732`) is a live `decimal-pad` TextInput; Enter reaches it from the native stream while
  typing (`:441`). The overlay plugs into one place: the `'field'` case of `tuneRun` (`:466`), which today calls
  `inputRef.current?.focus()`. Submit = `setValue` + `confirmRef.current()`, so out-of-range is refused with the
  existing `rangeMsg` and the card stays open, as the brief wants.
- **Bookmark search** (`:941`) and **bookmark name** (`:1086`) are `bmSlot`s whose press focuses the field (`:940`).
  Same switch. Search uses live `onChange` so results update as you type, and the sticky search still works.
- **★ Trap:** if any TextInput still has the cursor when the overlay opens, `typing` is true and `toHwKey` drops every
  key but Enter/Escape — the overlay would be deaf. Opening must first let go of the cursor
  (`Keyboard.dismiss()` + `VibePowerModule.leaveTextField`).
- **★ Physical keys into the overlay:** the iOS window reports only our characters (`AppDelegate.swift:95-107`,
  `ourCharacters`), so letters never reach JS today. For "the overlay also accepts physical keystrokes" (macOS note
  below), the native side must report every printable character while the overlay is open (a flag set by the overlay),
  on a separate `VibeKeyChar` event so the one-scheme bus is untouched.

## Keycap fonts

The two layouts follow different rules.

- **Numeric pad:** its keys are large, so the digits are drawn exactly as they appear in the frequency readout, in whatever display style is active.
- **QWERTY:** its keys are small, so labels use a readable text font. That is Nixie One when either Nixie display is active, and Atkinson Hyperlegible for every other display.

**Refined — the real display options.** There are four (`src/constants/faceplate.ts:29` `DisplayStyle`, labels at
`:427-430`). "Nixie One" and "Nixie tubes" are **one** option: the frequency is drawn as tubes, other text uses Nixie
One. "VCR segments" is `seg`, labelled VCR: 7-segment digits with a 14-segment unit. All four fonts are bundled
(`assets/fonts`, loaded in `App.tsx:882-892`); Atkinson is Regular only (no Bold).

| Display (id / label) | Numeric pad — reuse | QWERTY |
|---|---|---|
| `nixie` / NIXIE | **Tube digits, orange only** — `NixieCathodes` inside `NixieTubes.tsx` (traced IN-14 wires; colours fixed, and `TEXT_ALLOWED.nixie` is `[]` so it is always orange). `.` = the INS-1 bulb tube (`constants/nixie.ts:449-453`). | Nixie One |
| `hyper` / HYPER | Plain `Text`, Atkinson Hyperlegible (as `ControlsBar.tsx:1569-1581`) | Atkinson Hyperlegible |
| `dot` / DOT | Doto `Text` over `GhostGrid` (`VfdParts.tsx:53`), as the readout (`ControlsBar.tsx:1133-1145`); kHz/MHz via `DotField` | Atkinson Hyperlegible |
| `seg` / VCR | `SegDigits` (`VfdParts.tsx:188`) — one cell per key, its DP electrode for `.`; kHz/MHz via `SegField` (DSEG14), as `SegUnit` | Atkinson Hyperlegible |

- Reuse the frequency readout's rendering code for the numeric pad rather than duplicating it, so the two always match.
  **Refined — the exact component:** the readout is not one component but a per-display switch inside ControlsBar
  (`:1100-1152`, drawing `NixieTubes` / Doto `Text`+`GhostGrid` / `SegDigits` / `Text`). **Extract that switch into
  `src/components/FreqDigits.tsx` (`text`, `size`, `display`) and use it in both the readout and the pad.** One thing
  needs exporting: `NixieCathodes` (today internal to `NixieTubes.tsx`, which takes a frequency number, not a string).
  `DisplayFontText` is the fallback for anything FreqDigits cannot draw.
- **TRAP:** tube and segment displays have no glyph for `.`, ⌫, ↵ or kHz/MHz. Each needs a matching treatment: a tube-style dot cathode or segment DP for `.`, and icons styled to suit each display for the others. **Refined:** `.` is solved (bulb tube; SegDigits DP; Doto has one). kHz/MHz is solved on DOT and VCR (`DotField`/`SegField`, as their units already are) and as Atkinson text on NIXIE/HYPER (the nixie readout's unit already uses Atkinson). ⌫ and ↵ need drawing for every display — on VCR, `SegField`'s `extras` paths can carry them.
- **TRAP:** check legibility at TV viewing distance, and offer a "Hyperlegible keycaps" override for accessibility, independent of the display font.
- Accented characters and symbols must be present in each font used, or fall back per character. **Refined:** add a cmap check to `scripts/test_faceplate_screenfont.ts` for the accent set against Nixie One and Atkinson.

## Layout 1: Numeric (frequency entry)

```
[ 7 ] [ 8 ] [ 9 ]   [ ⌫ ]
[ 4 ] [ 5 ] [ 6 ]   [kHz/MHz]
[ 1 ] [ 2 ] [ 3 ]   [ Clear ]
[ . ] [ 0 ] [ ↵ ]
```

- Only one decimal point allowed, with a digit limit for the current unit.
- Validated against the server's tuning range when the user submits, with an inline error.
- **TRAP:** always use `.` internally and parse with our own code, not locale-aware parsing (comma-decimal locales such as Brazil and much of Europe).
- **Refined:** the unit key follows FreqModal's unit (it has Hz/kHz/MHz and an optional `lockUnit`); on the pad it toggles kHz ↔ MHz and is absent when the unit is locked. PINs use this layout with `.` and the unit key blanked (digits only, as `SDRScreen.tsx:10856` filters).

## Layout 2: QWERTY

```
[1][2][3][4][5][6][7][8][9][0]
[q][w][e][r][t][y][u][i][o][p]
[a][s][d][f][g][h][j][k][l][⌫]
[⇧][z][x][c][v][b][n][m][-][↵]
[ #+= ][      space      ][Done]
```

- Layers: lower case, upper case (⇧; a double press gives caps lock) and symbols (`#+=`).
- Accented characters (é, ç, ã, ñ, etc.) by long-pressing A on the base letter. **Refined:** "A" means *enter* in Stuart's scheme, which is R on a no-stick pad. Long-press of enter needs press/release timing that the bus already gives (down and up are separate events). URL mode needs `.`, `/`, `:` on the base layer — see modes.

### Modes

| Mode | Used for | Behaviour |
|---|---|---|
| `search` | Bookmark search | Live `onChange`; an empty submit is allowed |
| `text` | Bookmark names | Accents enabled |
| `password` | Server or admin passwords | Full character set, no auto-capitalisation, show/hide toggle |

**Refined — two more modes the app needs** (from §8): `url` (custom server address, host, landing link: base layer
swaps `-` for `.`, adds `/` and `:`, no capitals) and `pin` (numeric layout, digits only, masked). `frequency` stays as
the numeric mode.

### Password mode

- **TRAP:** over AirPlay the focus highlight shows which keys are pressed, so masking the field alone doesn't protect the password. In password mode, use a neutral focus style with no key-press animation, and show a one-line hint offering touchscreen entry instead.
- **TRAP:** never log keyboard state or values (analytics, crash breadcrumbs, debug overlays). **Refined:** the app writes breadcrumbs freely (`crumb(...)`, the protocol log in the diagnostics report). The reducer and the overlay must never pass a value to either; the test checks the reducer has no logging import.

## Button mapping (a single shared table)

| Action | Controller | Siri Remote (tvOS) |
|---|---|---|
| Move focus | D-pad / left stick | Swipe / click edges |
| Press key | A | Click |
| Cancel | B | Menu / Back |
| Backspace | X | (key) |
| Space | Y | (key) |
| Cursor left/right | LB / RB | (keys) |
| Shift | LT | (key) |
| Symbols layer | RT | (key) |
| Submit | Menu / Start | Play/Pause |
| kHz/MHz toggle (numeric) | LB / RB | Play/Pause long press (TBC) |

### ★ Refined — checked against Stuart's existing scheme (`gamepad_mapping`, `padActions.ts`). FOR STUART TO DECIDE.

His scheme has **two profiles**, buttons by position:
- **Two sticks:** D-pad = move · ✕/A (south) = enter · ○/B (east) = back · left stick turned = tune · right stick turned = zoom. West, north, L1, R1 are **unused**.
- **No sticks:** D-pad = move · **R1 = enter · L1 = back** · north = zoom in · south = zoom out · west = tune down · east = tune up. **No button is spare.**
- Both: SELECT = controller guide · HOLD START (~0.9 s) = switch profile.

| # | Draft says | Conflict | Recommendation |
|---|---|---|---|
| 1 | A = press, B = cancel | Matches two-stick. On no-stick pads A is *zoom out* and B is *tune up*; enter/back are R1/L1. | Define the keyboard in terms of the bus keys **Enter** and **Back**, not letters, so each profile keeps its own enter and back (and a physical keyboard gets the same for free). |
| 2 | X = backspace, Y = space | Free on two-stick (west/north unused). On no-stick X is *tune down*, Y *zoom in*. | Two-stick: west = ⌫, north = space, as drafted. No-stick: on-screen keys only — see #6 for the tune buttons. |
| 3 | LB / RB = cursor | Free on two-stick. **On no-stick LB is BACK and RB is ENTER** — a hard clash. | Move the caret with the **tune** action instead (#6). Two-stick L1/R1 then become shift / symbols (#4). |
| 4 | LT / RT = shift / symbols | **Triggers are not reported** — `PAD_BIT` has no L2/R2 and neither native bridge reads them. | Avoid native work: two-stick L1 = shift, R1 = symbols; on-screen ⇧ and #+= keys on both profiles. (If Stuart wants triggers, add bits 4096/8192 to both bridges and `PAD_BIT`, with a test.) |
| 5 | Menu / Start = submit | START is **HOLD to switch profile**; a tap does nothing today. | Tap START (released before 0.9 s) = submit; **HOLD START is ignored while the keyboard is open** — switching profile mid-word would change what every button means. |
| 6 | Left stick = move focus | Left stick **turned** = tune, the jog Stuart signed off (10-18). | D-pad moves focus. The **tune** action (left stick turned on two-stick; west/east on no-stick; `<` `>` on a keyboard) moves the **caret** — tuning is "moving along", and the jog keeps its one meaning of turn-to-move. Zoom actions do nothing in the keyboard. |
| 7 | LB / RB = kHz/MHz | As #3 on no-stick. | Two-stick: on the numeric pad there is no shift, so L1/R1 = kHz/MHz there. No-stick: the on-screen key. |
| 8 | Cancel = B | Inside a text box, should Back delete a character or leave? | Back = **cancel one layer**, as everywhere ("back peels one layer at a time"). ⌫ is a key (and west on two-stick). Leaving with a changed value asks nothing — the field keeps its old value. |
| 9 | Siri Remote column | tvOS is a future native port on its own focus engine. | Keep the column as a note for that port; nothing here depends on it. |

Proposed table, if Stuart agrees:

| Action | Two sticks | No sticks | Keyboard |
|---|---|---|---|
| Move focus | D-pad | D-pad | arrows / WASD |
| Press key | ✕ / A (south) | R1 | Enter |
| Cancel | ○ / B (east) | L1 | Backspace / Escape *(only when not typing physical letters — §Input handling)* |
| Caret left / right | left stick turned | west / east (□ / ○ positions) | `<` `>` |
| Backspace | □ / X (west) | ⌫ key | Backspace while typing letters |
| Space | △ / Y (north) | space key | Space |
| Shift / Symbols | L1 / R1 | ⇧ / #+= keys | Shift |
| kHz/MHz (numeric) | L1 / R1 | unit key | — |
| Submit | tap START, or ↵ | tap START, or ↵ | Enter on ↵ |
| Controller guide | SELECT | SELECT | — |

The controller guide (`src/utils/padGuide.ts`) gets a third tab, KEYBOARD, generated from the same table, and
`KeyboardShortcuts.tsx` GAME CONTROLLERS gains a paragraph (AGENTS.md: fix the copy that says where a control is).

## Input handling

- Add a global `inputMode: 'normal' | 'keyboard'`. While the keyboard is open, **all** controller events go to it and none reach tuning or scrolling.
  **Refined — no new global needed:** the bus already does this. ControllerKeyboard listens at a priority above every
  panel (PadGuide is 100; the screens are -1) and returns `true` for every key, which stops dispatch
  (`keyActions.ts:117`) — so tuning, zoom, panels and the screen hear nothing. It also pushes a PanelNav owner and
  registers with the overlay contract (audit group A).
  **★ Trap found:** panels beneath re-arm their 10 s idle-close only when *they* hear a key (`PanelNav.tsx:122`). A
  consumed key never reaches them, so **the FreqModal under the keyboard would time out and close mid-typing.** Add a
  `holdIdleClose()` counter in PanelNav that the overlay holds while open.
- Track focus as a `{row, col}` index. Handle wrap-around and wide keys (decide what "down" from a wide key lands on).
  **Refined:** give each key a box in key units in the layout data and move with `pickNext` — the same geometric
  picker the receiver screen and the web page use. "Down from space" then lands on the key whose centre is nearest
  below, with no rule to write. Wrap: when `pickNext` returns -1, wrap to the far edge of the same row/column (a small
  pure addition, tested).
- **TRAP:** the D-pad needs key repeat (about 400 ms initial delay, then an 80 ms repeat). Analogue sticks need a deadzone and edge detection.
  **Refined:** both exist. Repeat: `useRepeatingKeys` (380 ms, then 110 ms, 55 ms after 1.2 s — tuned on Stuart's
  rubber keyboard); use it, not a second law. Sticks: the native bridges dead-zone at 0.12; the jog engages at 0.6
  and counts only near the rim (`gamepadNav.ts` `Jog`).
- **TRAP:** check that the focus ring is visible at TV distance in every faceplate theme, especially Nixie orange on orange.
  **Refined:** `useNavFocus()` is green on every controls colour but green, where it is white; Nixie's tubes are
  orange, so green reads. Check on silver and black chassis too (feedback: no visible polish gaps).

## Platform notes

### iOS / Android
- Input via the GameController framework or its Android equivalent. **Refined:** already built — `VibeGamepad.swift` / `VibeGamepad.kt` → `padActions.ts` → the bus.
- Touch on a field opens the system keyboard as normal.
- **Refined — iOS PIN:** the iOS VibeServer PIN uses `Alert.prompt` (`InstancePickerScreen.tsx:962`), a native alert a pad cannot answer. When a pad is in use, iOS takes `VibePinPrompt` like Android, with the numeric layout.

### macOS
- The overlay also accepts physical keystrokes. **Refined:** needs the native printable-character report (§Component).
- Only opened when the controller was the last input.

### tvOS (future)
- Use the `react-native-tvos` focus engine **or** take over input while the overlay is open, but not both.
- **TRAP:** Apple requires tvOS apps to be fully usable with the Siri Remote, so don't assume a controller.
- **TRAP:** password fields always use the system secure keyboard, to keep "type on your iPhone", password AutoFill and dictation.

### Android TV / no-touch boxes (Refined — new)
- `TvTextInput` already turns a field into "a box + an edit sheet" on TV and no-touchscreen devices. When a pad is
  the input, its sheet should be ControllerKeyboard; with a TV remote, it stays the system sheet.

## Out of scope

- Replacing the system keyboard for touch users.
- Predictive text or autocorrect.
- Keyboards for non-Latin scripts.
- **Refined:** pasting (the bookmark import box) — touch-only by nature.

## 8. Every text field, and the mode it would use (Refined — new)

"No-typing path" = can a pad user get what they came for without typing, today or with an audit fix. Where there is
none, ControllerKeyboard is the only way in. **Bold = on the sofa path.**

| Field | Where | Mode | No-typing path |
|---|---|---|---|
| **Frequency** | `FreqModal.tsx:732` | numeric `frequency` | Partly: tune by stick/keys, STEP, VTS ◄ ►, the receiver's station list. Big jumps (MW → 2 m) need the pad. |
| **Bookmark search** | `FreqModal.tsx:941` | `search` | Yes: the saved list and the receiver's station list browse without typing. |
| **Bookmark name** | `FreqModal.tsx:1086` | `text` | **None** — SAVE is disabled while blank (`:1106`). Fix: default to RDS PS / station name / the frequency. |
| OWRX magic key | `FreqModal.tsx:850` | `password` | None |
| Bookmark import | `FreqModal.tsx:1190` | — (paste) | Out of scope |
| **Custom server URL** | `InstancePickerScreen.tsx:2559` | `url` | Only the directory, discovered and saved lists. A friend's private server needs the pad. |
| **Directory search** | `InstancePickerScreen.tsx:2658` | `search` | Yes, once sort and scrolling are reachable (audit). |
| Add server: name / host / port | `InstancePickerScreen.tsx:2437 / 2440 / 2443` | `text` / `url` / numeric | Name defaults; host none; port has a default per protocol. |
| Edit favourite: name / address | `InstancePickerScreen.tsx:2468 / 2471` | `text` / `url` | None |
| **VibeServer PIN** | `VibePinPrompt.tsx:105`; iOS `Alert.prompt` `InstancePickerScreen.tsx:962` | `pin` | None (a saved PIN pre-fills) |
| KiwiSDR call sign | `IdentModal.tsx:68` | `text` (capitals) | None |
| **Receiver password** | `PasswordModal.tsx:62` | `password` | None |
| **Radio PIN** | `SDRScreen.tsx:10850` | `pin` | None |
| **Admin password (radio chooser)** | `SDRScreen.tsx:10890` | `password` | None (optional) |
| **Admin password (take over)** | `SDRScreen.tsx:12099` | `password` | None |
| Admin password (menu) | `MenuSheet.tsx:2012` | `password` | None |
| Admin password (local hardware) | `LocalHardwarePanel.tsx:631` | `password` | None |
| LO / converter range | `LocalHardwarePanel.tsx:1487 / 1505 / 1512` | numeric | None |
| Chat handle | `ChatDrawer.tsx:444` | `text` | None |
| Chat message | `ChatDrawer.tsx:732` | `text` | Canned phrases in shared-dial mode (unreachable today) |
| Chat manual frequency | `ChatDrawer.tsx:665` | numeric `frequency` | Share the current frequency |
| City search | `CityPickerModal.tsx:95` | `search` | Scroll the list (once rows register) |
| Server screen (Android, 21 fields) | `ServerModeScreen.tsx:626…3373` — names, admin password `:2253` (**required to Start**), PIN, location, proxies, landing message/URL/label, radio label, kHz/MHz values, ppm, antenna, counts | `text` / `password` / `pin` / `url` / numeric | PIN pills, ± counts, antenna chips, defaults. Admin password: none — add "generate". |
| RTL-TCP server name | `RtlTcpServerScreen.tsx:238` | `text` | Has a default |
| Antenna / band editors | `AntennaBandsEditor.tsx:67`, `AntennaSwitchSetup.tsx:42`, `BandLimitEditor.tsx:373 / 379 / 386` | numeric / `text` | Band chips in BandLimitEditor |

## 9. Open questions — recommendations for Stuart

1. **Digit-step VFO tuning alongside the numeric pad?** *Recommend: yes, later (Phase 3), and cheaply* — as the
   existing ADJUST mode (`PanelNav` `adjust`, used by every slider): Enter on the readout's highlight → amber,
   left/right pick a digit, up/down change it, Enter/Back finish. Same pattern the pad already knows; no new scheme.
   Not before the numeric pad, which covers big jumps.
2. **Landscape layout?** *Recommend:* follow the window, never the device (Stuart's rule). Numeric: value on the left,
   pad on the right when the window is wider than tall. QWERTY: always full width, value strip on top; on a phone in
   landscape the keys shrink to fit five rows, never scroll.
3. **Siri Remote kHz/MHz?** *Recommend:* an on-screen key only; no long-press gestures. Decide with the tvOS port.
4. **New — the button conflicts** in § Button mapping: accept the proposed table (#1–#8)?
5. **New — Back inside the keyboard:** cancel one layer (recommended), or delete a character until empty?
6. **New — START:** tap = submit, hold ignored while open (recommended)?
7. **New — open on Enter, or on arrival?** *Recommend:* only on Enter on the field (or A on it). Arriving on a field
   by D-pad never opens anything — the highlight must be able to pass through.
8. **New — AirPlay password hint:** show "you can type this on the touchscreen" in password mode (recommended, as
   drafted), and also when the pad starts in password mode on a mirrored screen only? (Mirroring is detectable on
   iOS — `UIScreen.screens.count > 1` — but the hint is cheap enough to show always.)

## 10. Implementation plan

Tests follow the repo's pattern: pure logic in `src/utils/*.ts` with no React Native imports, a
`scripts/test_*.ts` file with the local `ok()` helper and `process.exit(fails ? 1 : 0)`, registered in
`scripts/run-tests.sh` as `npx tsx scripts/test_….ts`. Keep runs niced and silent; one build at the end of a phase.
Each phase ends with a device check by Stuart: iPhone + PS pad, XCover + pad, Mac, and AirPlay to a TV.

### Phase 1 — the audit's MUST list (no ControllerKeyboard yet)
1. **Overlay contract** in PanelNav: `useOverlay(visible, { onBack })` — a global count that replaces the hand-kept
   `anyPanelOpen` (`SDRScreen.tsx:10321`) and `listNavActive` (`InstancePickerScreen.tsx:1965`), a nav owner, and
   Back routed to the surface. *Test:* `scripts/test_overlay_stack.ts` on the pure core (stacking, count, Back to the
   top surface, unmount mid-dispatch).
2. **Radio chooser** (audit §1) on `useListNav`; Back = `goBack()`. **Refusal card** likewise; `closeAll` stops being
   the only Back for surfaces it does not know.
3. **Group B on the sofa path:** connection cards (one shared card component + nav), tap banners, PasswordModal,
   VibePinPrompt buttons (and iOS → VibePinPrompt when a pad is in use), IdentModal, `tcpModal` / `editFav`, both
   tours (keys once, in `Coachmark.tsx`), the first-launch splash, DabPanel as a grabbed region, MapOverlay,
   TlsTrustPrompt when it merges from main (start on CANCEL).
4. **`appAlert()`** — `Alert.alert`'s signature; touch → native, unchanged; keyboard/pad → in-app dialog on
   `useListNav` (Back = the cancel button); a counter mutes the bus while a native alert shows. Migrate the sofa-path
   calls. *Test:* `scripts/test_app_alert.ts` (button order → focus order, the cancel button found by `style`, a
   destructive button never the default focus).
5. **Group C on the sofa path:** `PopupKey` takes an optional nav slot; AudioSheet `Toggle` and DSP TYPE; MenuSheet SDR
   CONTROLS / VFO lock / About / admin unlock; server-list row buttons as a 2-D row, header buttons, directory
   search/sort/Retry; FreqModal SHARE. *Test:* `scripts/test_nav_coverage.ts` — a static scan that fails when a file
   using PanelNav renders a `TouchableOpacity` / `Pressable` / `Switch` / `Slider` with neither a nav registration nor
   a `skipNav` with a reason.
6. **BrowserOverlay:** Back closes even while shortcuts are suppressed.
7. **Picker bugs:** Back index (`:2048`), Enter = tap (`blocksApps`, `isFull`, SpyServer). *Test:* extract the
   index-to-zone mapping as a pure function → `scripts/test_picker_nav.ts`.

### Phase 2 — ControllerKeyboard
1. `inputSource` in PanelNav + the root touch capture in `App.tsx`. *Test:* extend `scripts/test_key_actions.ts`
   (a pad event sets `pad`, a key `keyboard`, a touch `touch`).
2. `src/utils/kbLayouts.ts` + `src/utils/controllerKeyboard.ts` (reducer). *Tests:* `scripts/test_kb_layouts.ts`
   (every key reachable from every other in four directions via `pickNext`; wide keys; wrap; numeric and QWERTY;
   landscape boxes), `scripts/test_controller_keyboard.ts` (one point; digit limit per unit; kHz↔MHz conversion;
   `.` internal whatever the locale; caps on double ⇧; symbols layer; accents; caret moves; PIN digits only; empty
   search submit allowed; no logging import).
3. The keyboard actions per profile (the agreed table) as a pure map beside `padKey`. *Test:* extend
   `scripts/test_pad_actions.ts` (each profile's keyboard keys; tap START = submit, hold START ignored while open).
4. `FreqDigits` extracted from ControlsBar; `NixieCathodes` exported. *Test:* extend `scripts/test_faceplate_nixie.ts`
   / `_segfield` / `_dotfield` for single-glyph and `.` rendering inputs; cmap check in `test_faceplate_screenfont.ts`.
5. `ControllerKeyboard.tsx` + `usePadField()`, `holdIdleClose()`, cursor release on open.
6. Wire **numeric first** — frequency, VibeServer PIN, radio PIN — then **QWERTY**: every password box, custom URL and
   Add server, directory search, bookmark search and name, chat, call sign. Bookmark default name ships alongside.
7. Guide tab and KeyboardShortcuts copy; About release note.

### Phase 3 — the audit's SHOULD list
ChatDrawer and canned phrases; LocalHardwarePanel on `usePanelNav`; About, SpecRatio, CityPicker, AdvRds;
Recordings row actions; FM-DX chips and paused banner; receiver-screen chips; decoder spots; horizontal reveal; the
remaining alerts; ServerMode / RTL-TCP pad support; Android hardware keyboard into the bus; digit-step tuning (Q1).

## 11. The web client (web/client) — Refined, new

- **Already shared:** `gamepadNav.ts` (`pickNext`, `Jog`, `padProfile`), `keyActions.ts`, `padGuide.ts` — imported
  from `src/utils` (`web/client/src/gamepad.ts:40-42`).
- **Duplicated, should be shared:** the web re-implements `padActions.ts` — button indices, `SWITCH_HOLD_MS`, jog
  degrees 30/45, the per-profile button→action table (`gamepad.ts:338-354`), the profile notice text, the storage
  key. `padActions.ts` is pure (it imports only `gamepadNav`), so the web can import `padKey` / `profileNotice` /
  the constants directly. Do this before adding the keyboard rows, or the table forks a third time.
- **Shared for the keyboard:** `kbLayouts.ts` and `controllerKeyboard.ts` are pure on purpose; the web draws its own
  DOM overlay from them (as it draws the guide from `padGuide.ts`), and uses its `[data-navscope]` / `backStack` for
  ownership.
- **What the web needs:** the overlay itself (the handheld kiosk has no system keyboard at all); pad Back must blur a
  focused field (today the highlight walks away while the input keeps focus — `gamepad.ts` poll path never checks
  typing); a field's Enter opens the overlay when the last input was a pad, else focuses as now. Web fields: frequency
  box, bookmark search, chat, server password / PIN, admin panel.

## 12. Files this touches (for the implementer)

`src/components/PanelNav.tsx`, `src/utils/hardwareKeys.ts`, `src/utils/keyActions.ts`, `src/utils/padActions.ts`,
`src/utils/padGuide.ts`, new `src/utils/kbLayouts.ts`, `src/utils/controllerKeyboard.ts`,
`src/components/ControllerKeyboard.tsx`, `src/components/FreqDigits.tsx`, `src/components/NixieTubes.tsx` (export),
`src/components/ControlsBar.tsx` (use FreqDigits), `src/components/FreqModal.tsx`, `src/components/PopupShell.tsx`,
`src/components/TvTextInput.tsx` (injection point), `App.tsx`, `ios/VibeSDR/AppDelegate.swift` (printable characters
while open), `web/client/src/gamepad.ts`, plus the screens in the audit.

## 13. Decisions Stuart needs to make (summary)

| # | Decision | Recommendation |
|---|---|---|
| 1 | Button table conflicts (§ Button mapping #1–#8) | Accept the proposed per-profile table: enter/back stay each profile's own; tune moves the caret; two-stick W/N/L1/R1 do ⌫/space/shift/symbols; no-stick edits with on-screen keys. |
| 2 | Triggers (LT/RT) | Not needed — use L1/R1 on two-stick, on-screen keys on no-stick. |
| 3 | START | Tap = submit; hold ignored while the keyboard is open. |
| 4 | Back in the keyboard | Cancel one layer; ⌫ is a key. |
| 5 | When it opens | Enter on a field only, never on arrival. |
| 6 | Digit-step tuning | Yes, Phase 3, as ADJUST mode on the readout. |
| 7 | Landscape | Follow the window: numeric side-by-side when wide; QWERTY full width. |
| 8 | Siri Remote kHz/MHz | On-screen key; decide with the tvOS port. |
| 9 | Bookmark default name | Default to RDS PS / station name / frequency so saving needs no typing. |
| 10 | iOS PIN with a pad | Use `VibePinPrompt` instead of `Alert.prompt` whenever a pad is in use. |
| 11 | ServerMode admin password | Add a "generate" key (the one forced typing step to start a server). |
