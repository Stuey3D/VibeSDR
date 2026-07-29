# BRIEF: "Servers" Chip + Instance Menu Extraction + Hamburger→Cog

**Project:** VibeSDR (React Native / Expo)
**Author:** Stuart Carr (Stuey3D)
**Status:** Draft for implementation. Pure client-side UI — no server/Nathan dependency.
**Targets to verify on:** iPhone SE (320/375dp, Display Zoom), iPhone 17 Pro Max (Dynamic Island, both landscape rotations), Moto G35, Galaxy Tab A9.

---

## 1. Goal

Make "return to the instance list" **discoverable**, and stop the menu button lying about what it is.

Two shipped changes:

1. **A "Servers" chip** top-left of the spectrum that taps open into a small dropdown. The dropdown's **header row is the back-to-list action**, with Favourite / Set-default beneath. The instance-level actions move out of `MenuSheet` and into this chip.
2. **Hamburger → cog** on the Row-2 menu button. Once the chip owns "leaving", that button is honestly just settings, and a cog says so.

---

## 2. Why (background — do not skip, it constrains the design)

Community feedback (OARC #sdr): two experienced users (Jeff M9XJW, Kristian G5KGG) could not find how to leave the waterfall and return to the SDR list. Jeff: *"I can't see an exit, I have to quit the app."* The back-to-list button exists but is buried at the **bottom of the INSTANCE section inside `MenuSheet`**, reached via a hamburger glyph that reads as "settings", not "exit".

This is compounded by an existing, deliberate decision: on the SDR screen the hardware/gesture back is **consumed** (`SDRScreen.tsx` ~L1534, `hardwareBackPress` returns `true`; iOS stack `gestureEnabled:false`). That was correct — a left-edge back-swipe is the same gesture primitive as a horizontal waterfall pan / VFO drag and was popping people out mid-tune. **Consequence: the on-screen affordance is the *only* exit on both platforms.** So burying it was the whole problem, and a visible, labelled control is the fix — not an embellishment.

Design principle carried through: the label does the work a clever icon couldn't. A word ("Servers" / "Back to instance list") is unambiguous where the hamburger was not.

---

## 3. Scope

### In scope
- New `ServersChip` component (collapsed chip + expandable dropdown) rendered on the SDR screen, top-left of the spectrum.
- Lift **Back-to-list, Favourite, Set-default** out of `MenuSheet`'s INSTANCE section into the chip dropdown.
- Full-screen dismiss catch-layer while the dropdown is expanded.
- Row-2 menu button glyph: hamburger → cog (preserving the `menuAsBack` face).
- Tour retarget + copy move; new chip coachmark; cog coachmark relabel.
- Accessibility labelling for the chip.

### Out of scope (do NOT do now)
- Moving Reset-interface-settings, Recordings, Replay-tutorial, or the footer identity block — **these stay in the cog menu** (see §5.4). Reset especially must NOT sit next to the quick exit.
- Any change to the back-swipe consumption or the drum gestures.
- Landscape re-layout of the controls island.

---

## 4. Interaction spec (the important bit)

Model the chip as a **stateful toggle with two independent taps** — NOT a double-tap gesture. No timing window, no latency, no gesture race.

**Collapsed** — chip shows `[instance glyph] ‹ Servers`.
- Tap → **expand** (does not navigate anywhere).

**Expanded** — dropdown grows down from the chip anchor:
1. **Header row = `‹ Back to instance list`** at the same x-anchor the chip occupied — the chip *becomes* this row. Tap → exit to the instance picker (`onBack`). This is what makes "tap-tap in the same spot" the muscle-memory exit.
2. hairline separator (stronger than the others — divides the destructive-ish exit from the toggles)
3. `♡ Favourite this server` — tap toggles; **stays expanded** so the state change is visible (♡ ↔ ♥).
4. `☆ Set as default` — tap toggles; stays expanded (☆ ↔ ★).
5. **Collapse handle** at the bottom: an up-chevron on a short grab-bar. Tap → collapse.

**Dismissal (three ways, all required):**
- the collapse up-chevron,
- **tap anywhere outside** the dropdown,
- (the header is NOT a collapse — it exits; the chip does not re-toggle while expanded).

> Because the header exits and the chip stops toggling once open, an accidental expand must have a non-exit escape — hence tap-outside + the collapse arrow are **not optional**. Without them an accidental open traps the user between "expand" and "exit".

**Accidental-tap safety** falls out for free: a stray tap only *expands a dismissable panel*. Nothing destructive happens until a second, deliberate tap on a clearly-labelled row. This is why we don't need gesture arbitration on the chip.

---

## 5. Implementation

### 5.1 New: `src/components/ServersChip.tsx`

Self-contained collapsed-chip + dropdown. Uses theme tokens (`useTheme()`), Nixie One via `t.font`, amber via `t.btnText` (`#ffb833`) with an amber border (`rgba(255,160,0,0.85)`), and **`SectionIcon name="instance"`** (the network-nodes glyph — same one that currently heads the INSTANCE section, giving a visual through-line).

Props:
```ts
serverName: string;
isFavourite: boolean;
isDefault: boolean;
onBack: () => void;
onToggleFavourite: () => void;
onSetDefault: () => void;
// visibility gating passed by parent (see 5.5)
```
Internal `expanded` state (`useState`). Row icons: network glyph (header), heart (fav), star (default), up-chevron (collapse). Heart/star toggle their filled/outline variant on active.

**Backing is mandatory and opaque-ish** (≈ `rgba(14,10,4,0.93)`): in waterfall-only mode the whole area behind the chip is live green trace, so bare text would be unreadable, and the dB labels behind it (`pointerEvents="none"`, purely visual) are simply occluded — see §6.

### 5.2 `src/screens/SDRScreen.tsx` — placement & anchoring

- Render `ServersChip` in the top-left of the spectrum/waterfall area.
- **Anchor to the panel top / safe-area top, NOT to `specTop`/`wfTop`.** Those move when `specShow` toggles; keying off them makes the chip jump when a user switches to waterfall-only. Fixed offset from the top inset instead.
- **Left margin: `left: Math.max(MARGIN, insets.left)`.** `useSafeAreaInsets()` is already imported (~L639) but `insets.left`/`insets.right` are currently applied nowhere. In landscape with the notch/Dynamic Island on the left, a bare `left: MARGIN` tucks under it. This one `Math.max` is the entire notch fix (the earlier bezel-cutout idea was dropped precisely to make this trivial — a floating, inset-respecting chip never meets the notch).
- **Dismiss catch-layer:** while `expanded`, mount a full-screen transparent `Pressable`/`View` *behind* the dropdown but *above* the waterfall touch surface. It (a) carries the semi-opaque backdrop tint, and (b) **swallows** the outside-tap so closing the menu does NOT also tune/pan the waterfall. One element, both jobs.
- Wire `onBack` to the existing exit path (whatever the buried button currently calls — `onBack ?? onClose` equivalent / stack pop to picker). Wire favourite/default to the existing handlers (~L2794 set-default, ~L2817 toggle-favourite).

### 5.3 `src/components/MenuSheet.tsx` — remove the lifted rows

In the INSTANCE section, **delete** the `← BACK TO INSTANCE LIST`, `♡ FAVOURITE`, and `☆ SET DEFAULT` buttons (currently ~L1352–1363). Keep everything else in that section (reset, recordings, replay tutorial, footer identity). The `tourRef('backToList')` wrapper moves to the chip (§5.6).

### 5.4 What stays in the cog menu (explicit)
`Reset interface settings` (destructive — must stay away from the exit), `Recordings`, `Replay tutorial`, and the footer identity/About block remain in the cog menu. Only the three "which server am I on / how do I leave" actions move to the chip.

### 5.5 `src/components/ControlsBar.tsx` — hamburger → cog

- Replace the `<Hamburger …/>` render in Row 2 (portrait, ~L657–665) **and** the landscape STEP/MENU column (~L799) with a cog glyph.
- **Preserve the `menuAsBack` branch** — FM-DX renders `‹ Back` there because it has no menu. Only the non-back face becomes a cog:
  ```tsx
  {menuAsBack ? <Text …>‹ Back</Text> : <Cog color={t.btnText} />}
  ```
- No `SectionIcon` cog exists (`admin` is a wrench = server-admin, wrong meaning — do not reuse). Add a small stroked gear component sized to match the chat/share icons (`ICON_SZ`), **white** stroke to match the row (those icons are white, not amber).

### 5.6 Tour / coachmarks
- Move `tourRef('backToList')` onto the chip.
- Move the tour copy at `SDRScreen.tsx` ~L3860 (*"…since Back is off, this is where you return to the server list"*) onto the **chip's** coachmark.
- The cog's coachmark now says **settings** (bandwidth, modes, NR, decoders, bookmarks). A tutorial pointing at a cog and calling it the exit would just re-create the original confusion.
- Add a first-run coachmark on the chip ("tap to switch receiver / return to the list").

---

## 6. Display-mode & scale behaviour (verify, don't assume)

- **Spectrum shown:** chip sits in the black band between the −58 dB and −73 dB labels. `dbLabels` are `pointerEvents="none"`; the chip's opaque backing occludes any it overlaps. **No dB-label dodge logic is required** given the opaque backing. *Optional polish only:* if at minimum `specFrac` the bunched labels bleed at the chip's edge and look messy, suppress `dbLabels` whose y intersects the chip bounds — but ship without this first and see if it's even visible.
- **Waterfall-only (`specShow=false`):** `dbLabels` returns `[]` (`specH < 40` guard), so there's nothing to collide with — the caveat self-resolves. The chip must be anchored to the panel top (§5.2) so it doesn't hop when the user toggles, and the opaque backing (§5.1) is what keeps it legible over live trace.

---

## 7. Edge cases
- **Landscape / Dynamic Island:** verify both rotations on the 17 Pro Max — the `insets.left` margin must keep the chip clear of the island. The dropdown grows *down* and never reaches the controls island, so no decoder-box interaction (that lives at `pillBottom`).
- **Decoder running:** no conflict — decoder box is `pillBottom`-anchored (bottom); chip is top.
- **FM-DX backend:** it has no instance-list concept and already uses `menuAsBack`. **Gate chip visibility off** for FM-DX (or wherever `onBack`→picker doesn't apply). Confirm which backends should show it (UberSDR / Kiwi / OWRX / local / RTL-TCP / SpyServer) vs hide.
- **Read-only receiver:** Back and Favourite/Default still apply; chip stays enabled.

---

## 8. Testing plan

**iPhone SE (portrait):** chip legible and tappable at 320/375dp; expand/collapse; tap-outside dismiss does not tune; shrink `specFrac` to minimum → chip still clean; switch to waterfall-only → chip doesn't jump, stays legible over trace.

**iPhone 17 Pro Max:** portrait Dynamic Island clear; **landscape both rotations** — chip clear of the island via `insets.left`; dropdown legible.

**Android (Moto G35 / Tab A9):** parity; confirm the consumed hardware-back still can't exit and the chip is the route out.

**Cross-cutting:** two-tap exit lands on the same spot; Favourite/Default toggle in place and stay open; cog replaces hamburger in both portrait and landscape; FM-DX still shows `‹ Back` (not a cog) and the chip is hidden there; tour points at the chip, cog coachmark says settings.

---

## 9. Visual reference
Two mockups rendered on the real `screenshots/03-waterfall-portrait.jpeg` (amber / Nixie One):
- `vibesdr-servers-chip-closed.png` — collapsed `‹ Servers` chip + cog in the button row.
- `vibesdr-servers-menu-open.png` — dropdown: `‹ Back to instance list` (header) / Favourite / Set-default / collapse arrow.

---

## 10. Future work (separate briefs — not now)
- Optional: surface persistent server identity (name) in the collapsed chip once localisation of long names is handled.
- Optional: long-press chip → jump straight to picker (power-user shortcut) — only if telemetry shows the two-tap is a friction point.
