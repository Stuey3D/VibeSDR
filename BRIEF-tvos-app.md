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

### 6.1 ★★★ NO USB MEANS IT CANNOT *HOST* — IT DOES NOT MEAN THE HARDWARE CONTROLS GO.
**CORRECTED 2026-08-03 (Stuart: *"they are required because of the VibeServer"*).** An earlier
version of this section said the whole hardware surface could be dropped on tvOS. That was wrong,
and the mistake was conflating *"this box has no dongle"* with *"nobody here needs gain controls"*.

- **Absent on tvOS:** Server mode itself, and anything that presumes a locally attached radio —
  there is no USB, so an Apple TV can never *be* a receiver.
- **REQUIRED on tvOS:** the gain / LNA / IF / AGC / notch / bias-T panels, as **REMOTE controls of
  the VibeServer you are connected to**. They travel over the wire (`rsp_control`, `ahf_control`)
  exactly as they do from the web client — which is precisely the surface reworked on 2026-08-03.
- ★★ So the tvOS app inherits the **shared-receiver gating** built that day and must mirror it:
  `sharedGate()` on the server, `rspRestricted()` on the client. On a receiver with a LOCKED centre
  a listener sees the read-only view — **system gain visible, IF slider visible but locked BECAUSE
  IT MOVES** — and an admin who unlocks gets the full set. See `memory/rsp_agc_zoom_emit_gate.md`.
- ★ **Admin unlock on a TV is a UX problem worth solving early.** Entering a password with a remote
  is miserable. Options: pair-from-phone, a one-time code, or Apple's automatic password entry —
  but do not ship the on-screen keyboard as the only route to your own radio's controls.

→ tvOS talks to VibeServer / Kiwi / OpenWebRX / SpyServer / FM-DX over the network. It is a client
in the sense that it cannot host — not in the sense that it cannot control.

### 6.2 ★★★ THERE IS NO WEBVIEW ON tvOS — AND THAT DECIDES THE WHOLE APPROACH.
Apple has never shipped `WKWebView` on tvOS; there is no Safari there either. `TVMLKit` (TVML/TVJS)
is a template system running on JavaScriptCore — **no DOM, no `<canvas>`, no WebGL**.

**Two consequences, and the second is the important one:**
1. `react-native-webview` cannot work on tvOS. Two screens use it today — **`MapOverlay.tsx`** and
   **`BrowserOverlay.tsx`** — so each needs a native replacement or an honest absence.
   ★ **THE MAP VIEWS ARE THE ONLY REAL PORTING PROBLEM** (Stuart, 2026-08-03: *"it's not the
   controls themselves that is the issue, it's the map views"*). The controls port fine and are
   required — see §6.1. See §6.2c for what to do about the maps.
2. ★★★ **THE WEB CLIENT CANNOT BE THE APPLE TV APP.** Stuart asked (2026-08-03) whether adapting the
   web client would be easier than porting the app — and everywhere else it would be, since the web
   client is already landscape-first, already has the keyboard layer a D-pad maps onto, and has the
   waterfall he calls perfect. But its waterfall is **WebGL** (`wfgl.ts`) and there is no surface on
   tvOS to run it in. **On Apple TV specifically, it has to be the native/RN app.**

★ **VERIFY THIS BEFORE ACTING ON IT.** It is settled as of tvOS 26 and I am confident, but my
knowledge ends May 2026 and this brief is being written in August — if Apple has since shipped a
webview on tvOS, the calculus above inverts completely and the web client becomes the cheap route.
Check Apple's current tvOS docs for `WKWebView` before committing either way.

### 6.2c ★★★ THE MAPS — AND WHY THIS IS WORTH DOING WHATEVER HAPPENS TO tvOS
`MapOverlay.tsx` is 978 lines wrapping **Leaflet 1.9.4 pulled from unpkg at runtime**, drawing OSM
tiles, used for the FT8 / HFDL spot maps. That is the only genuinely hard part of a tvOS port —
but look at what it implies **on the platforms we already ship**:
- the map needs the **INTERNET** to draw at all (CDN for the library, tile server for the imagery),
  so it is dead on an offline or field setup — the exact situation an SDR is often used in;
- it puts a **third-party CDN in the runtime path** of a shipping app;
- and it costs a whole WebView per map.

★★ **What these maps actually show is dots and great-circle paths on a world outline.** They do not
need street-level tiles. So the strongest option is to **draw them in Skia**, which the app already
depends on for the waterfall:
- a simplified coastline vector set (Natural Earth 110m is tiny) shipped in the bundle,
- an equirectangular or azimuthal projection — azimuthal centred on the receiver is arguably
  *better* for HF than a Mercator tile map, because it shows true bearing and distance,
- spots and paths drawn as Skia primitives, which is what the waterfall already does at 60 fps.

**That removes the webview, removes the CDN, works offline, is faster, and makes tvOS possible —
one change paying four ways.** It is the option I would take even if Apple TV never happens.

Alternatives, for completeness: **MapKit** exists on tvOS but `react-native-maps` support there is
doubtful and it would be a native module either way; or **omit maps on tvOS v1** and show spots as a
list, which is honest and cheap but leaves the offline/CDN problem in place everywhere else.

### 6.2b ★★ THE WEB-CLIENT ROUTE IS STILL RIGHT — JUST NOT FOR APPLE TV.
**Android TV, Fire TV and smart TVs all have browsers or real webviews.** There the web client runs
essentially as-is, and Stuart's whole control scheme maps onto it almost for free:
- the D-pad already arrives as **arrow keys**, and the web client already has a keyboard layer
  ([[keyboard_layer_shipped]]);
- the layout is already landscape with the frequency bar and status rows;
- the waterfall is the reference implementation, so it looks right on day one.

So the honest split is: **Apple TV = native app** (this brief), **every other TV = the web client**,
and the second is a fraction of the work. Worth doing first if reach matters more than the platform.

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
