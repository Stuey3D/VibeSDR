# BRIEF: VibeSDR on Apple TV — the Siri Remote control scheme

**Status: DESIGNED 2026-08-03 (Stuart), NOT STARTED.** This is Stuart's scheme, recorded as given,
plus the constraints and open questions that fall out of it. Nothing here is built.

★ **This is not new machinery.** It is phase 3 of [`BRIEF-controls-keyboard-and-gamepad.md`](BRIEF-controls-keyboard-and-gamepad.md):
the focus grid (`NavCtx` / `RowCtx` / `nextBtnId`, `styles.btnFocused`) is already built and
field-tested for keyboard panel navigation, and Stuart's rule for menus is *"the controls behave
exactly like the keyboard shortcuts do now"*. The Siri Remote is a third input into the same grid,
after the keyboard and the gamepad. **Read that brief first**; do not re-invent the focus engine.

---

## 1. The remote

| Input | Does |
|---|---|
| Clickpad ring — up/down/left/right | **tune / zoom** (same as the arrow keys and the D-pad) |
| Touch surface — swipe | **moves the highlight box** between controls and sections |
| Click (centre) | **enter / activate** the highlighted thing |
| Back (menu) | **step back** one level |

★★ **THE CRUX, AND IT IS THE ONE THING TO GET RIGHT: SWIPE AND CLICK ARE DIFFERENT INPUTS.** On the
Siri Remote the clickpad both *presses* like a D-pad and *senses touch*. Tuning is a RING PRESS;
moving the highlight is a SWIPE ACROSS THE SURFACE. Conflating them gives a remote that tunes when
you meant to move the highlight, which is the single most likely way this design fails in the hand.

## 2. The waterfall screen

- **No drums, no buttons.** Those are dead controls on a TV — nothing points at them, so nothing
  should offer them. Same rule as *"a control that only works in one scenario should not be there"*
  in AGENTS.md.
- **Only the frequency bar and its 4 buttons.**
- Layout is the **landscape view, with the status and clock rows directly underneath**.

## 3. The highlight, and how it sleeps

- After **a few seconds with no touch**, the highlight disappears. The screen is then just a
  waterfall — which is what a TV should look like from the sofa.
- **Touch the pad to wake it.** By default the wake lands on the **frequency box**.
- ★ **It remembers where it was, but only briefly.** If the pad is touched again within a short
  window, the highlight returns to where it was; after that it defaults back to the frequency bar.
- **A longer idle timeout returns the screen** to the waterfall / FM-DX screen as appropriate.

## 4. The tuning flow, end to end

Stuart's sequence, verbatim in effect:

1. Highlight is on the frequency box → **click**.
2. The **frequency / bookmarks box** pops up, highlight on **tune**.
3. **Scroll down one** to the frequency entry → **click**.
4. An **on-screen numpad** appears. Dial the digits.
5. **Scroll down to Hz / kHz / MHz** — ★ *these buttons ARE the confirm*. There is no separate OK,
   because the unit is information the app needs anyway, so asking for it twice would be a waste of
   a click.

★ **Bookmarks are one swipe away.** The highlight starts at the **very top** of the box, so a single
swipe right highlights the bookmarks pane; click switches to it.

## 5. Everything else

- **Sections, not just controls.** The highlight jumps between sections: swiping **up** from the
  frequency box goes to the **servers chip**.
- **Menus** behave exactly as the keyboard shortcuts do today: swipe to highlight, click to activate.
- **Sliders**: highlight → **click to grab** → **swipe left/right to adjust** → **click to set**, or
  **back to cancel**. ★ Note the grab/release model matches the gamepad brief's *clutch*: a
  continuous control needs an explicit engage, or every stray touch is an edit.

---

## 6. ★★★ CONSTRAINTS THAT FALL OUT OF THE PLATFORM (mine, not Stuart's — verify before building)

### 6.1 ★★★ AN APPLE TV HAS NO USB. IT IS A CLIENT, ALWAYS.
There is no dongle, no RSP, no Airspy. So the **entire local-hardware surface does not exist on
tvOS**: `LocalHardwarePanel`, gain / LNA / IF, bias-T, direct sampling, PPM calibration, Server mode
itself. That is a large simplification, and it is also a trap — those panels must be *absent*, not
merely disabled, per the AGENTS.md rule about controls that only work in one scenario.
→ tvOS talks to VibeServer / Kiwi / OpenWebRX / SpyServer / FM-DX over the network, and nothing else.

### 6.2 ★★ `react-native-webview` DOES NOT EXIST ON tvOS — there is no WKWebView on the platform.
Two screens use it today: **`MapOverlay.tsx`** and **`BrowserOverlay.tsx`**. Both need either a
native replacement or an honest absence on tvOS. This is the kind of thing that is cheap to plan for
and expensive to discover during a port.

### 6.3 ★ The build path needs checking, not assuming.
React Native's tvOS support lives in the `react-native-tvos` fork, and Expo's tvOS story has its own
config-plugin requirements. This project is Expo SDK 57 / RN 0.86 with the New Architecture locked
in. **Confirm the fork/plugin actually supports that combination before promising a build** — the
answer decides whether this is a target in the existing app or a separate one.

### 6.4 ★ tvOS has its own focus engine, and we already have ours.
UIKit wants to own focus on tvOS. The app has a working focus grid of its own. Driving OUR grid from
remote events is likely simpler and keeps one behaviour across keyboard, gamepad and remote — but it
means opting out of the native engine, which is a decision to take deliberately rather than by
accident.

### 6.5 ★★ A NEW UI SURFACE MEANS NEW TOUR COPY — and AGENTS.md's grep list must grow.
`sdrTour`, `pickerTour`, `AboutOverlay` and the watch tutorials all describe *where controls are*.
A tvOS layout with no drums and no buttons makes several of those sentences false on that platform.
Add the tvOS tour to that list the day it is written, not afterwards.

## 7. Open questions for Stuart

1. **The timeouts need numbers**: highlight hide (~"a few seconds"), the remember-my-position window
   ("within a certain time"), and the fall-back-to-waterfall idle.
2. **When the highlight is hidden, do ring presses still tune?** Assumed yes — that is the resting
   state and the reason the highlight hides at all.
3. **Is the tvOS app a separate App Store record or the same one?** Same record is normal for a
   universal app, but tvOS is a separate binary and platform either way.
4. **What happens to the decoders panel** (FT8/WEFAX/SSTV) on a TV — present, or out of scope for v1?
