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

★★★ **THE RING IS ALWAYS LIVE FOR TUNE AND ZOOM** (Stuart, 2026-08-03) — highlight showing or
hidden, menu open or closed. **That is what makes the scheme modeless**, and it is the property the
whole design rests on: the two inputs never trade places, so there is never a state to remember or
a mode to escape.
- It also gives something no phone layout can: **you can tune while a menu or panel is open**, since
  the surface is busy with the highlight and the ring is still yours.
- ★ It is the reason a one-D-pad remote cannot run this design (§6.2b) — there, tune and navigate
  would have to share, and sharing means a mode.
- ★ Watch the one collision: while a **slider is grabbed** the SURFACE is captured for adjustment
  (§5), but the ring keeps tuning. That is consistent, and probably desirable — but it is the spot
  to check first if the remote ever feels ambiguous in the hand.

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

## 5b. ✅ WHAT IS NOT IN tvOS v1 — THE SCOPE, DECIDED
**Stuart, 2026-08-03: *"drop the maps and any webview stuff, so no UberSDR admin stuff (we grey
these out anyway in keyboard mode) no kiwi compatibility mode as it wont work due to navigation
again."*** The rule is simple and it falls out of §6.2: **if it needs a WebView, it is not in v1.**

| Cut | What it is today | Why |
|---|---|---|
| **Spot maps** | `MapOverlay.tsx` — Leaflet in a WebView | No webview on tvOS. See §6.2c/§6.2d. |
| **In-app browser** | `BrowserOverlay.tsx`, used twice by `SDRScreen` | Same. |
| **UberSDR admin** | web-based admin surface | Already greyed out in keyboard mode, so nothing is lost that a keyboard user has today. |
| **Kiwi compatibility mode** | `compatUrl` — *"Kiwi web UI in a WebView"* (`SDRScreen.tsx:779`), behind a "leaving VibeSDR" warning | Needs a webview AND free-form web navigation. Even given a browser, driving a Kiwi's own UI from a remote is not navigable — **the controls are the blocker, exactly as with the other TV platforms (§6.2b)**. |
| **Recordings** | audio/IQ capture written to app storage | **tvOS has no user-facing persistent storage.** An app's local data is a purgeable cache Apple may reclaim at any time — anything a user expects to keep must live in iCloud. A recordings feature that silently loses recordings is worse than none. |
| **Share** | share sheet | `UIActivityViewController` **does not exist on tvOS**. There is nowhere to share *to*. |

★★ **AND THE STORAGE RULE IS WHY ICLOUD CARRIES THE REST** (Stuart, 2026-08-03: *"the same iCloud
sync applies like the watch gets"*). Bookmarks and favourites are exactly the case tvOS is built
for: small, user-owned state that must survive, so it syncs rather than being stored locally.
- The app already has this — [[icloud_sync_shipped]] — and the **watch consumes it the same way**,
  which makes tvOS a third consumer of an existing mechanism rather than new work.
- ★ Carry over the rule that came with it: **MERGE, never last-writer-wins**
  (`BRIEF-icloud-sync.md`). A TV that quietly clobbers the bookmarks you set on your phone would be
  a far worse bug than anything on this page.
- ★ Watch for [[icloud_stale_tombstone_immortal]] — the zombie-favourite bug was diagnosed from
  source twice and shipped wrong twice; a third consumer is a third chance to meet it.

### ✅ The restricted-Kiwi consequence is ALREADY HANDLED — it is one flag, not a feature
Dropping compatibility mode removes the only way to use a Kiwi whose owner set `ext_api=0` — **120
of 847 public Kiwis** ([[kiwi_ext_api_10s_kick]]). Left alone they would be listed, connect, stream
for ten seconds and die with nothing on screen to explain it.

★ **But the picker already does the right thing** (Stuart, 2026-08-03: *"those kiwis are not shown
the same as we do with Jr now"*). In `InstancePickerScreen.tsx`:
- `blocksApps(i) => i.extApi === 0` (`:1193`) — and `undefined` deliberately means *unknown*, never
  *allowed*, so nothing is painted red on a guess;
- they are **hidden by default**: `.filter(i => showRestricted || !blocksApps(i))` (`:1219`);
- a SHOW/HIDE banner off `restrictedCount` (`:1343`) is the way back;
- `connect(..., blocksApps(inst))` (`:1689`) routes a revealed one straight to compatibility mode.

**So tvOS needs exactly one thing: `showRestricted` can never become true** — no SHOW toggle, no
compatibility route. The default behaviour is already correct, and it matches Jr, which hides them
for the same reason (no webview on a watch either).
★ Which is the AGENTS.md rule holding on a third platform: *never offer a control whose every use is
a no-op*. A dead-ending server list is that same fault.

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
tiles, used for the FT8 / HFDL spot maps. That is the only genuinely hard part of a tvOS port.

★★ **BE PRECISE ABOUT WHY, BECAUSE THE REASONS DIFFER PER PLATFORM** — an earlier draft of this
section led with "it needs the internet", and Stuart correctly pushed back: *"apple tv is going to
be online all the time anyway."* It is mains-powered on a home network. **Offline is not a tvOS
argument and should not be used as one.**
- **On tvOS the reason is absolute and has nothing to do with connectivity: there is no webview to
  run Leaflet in at all.** Being online does not conjure a `WKWebView`. Native or nothing.
- **On iOS/Android the offline + CDN points do stand**, but as a secondary benefit, not the driver:
  the map needs the internet to draw (CDN for the library, tile server for the imagery), so it is
  dead in a field setup — the exact situation a portable SDR is often used in — and it puts a
  third-party CDN in a shipping app's runtime path, at the cost of a WebView per map.

★★ **What these maps actually show is dots and great-circle paths on a world outline.** They do not
need street-level tiles. So the strongest option is to **draw them in Skia**, which the app already
depends on for the waterfall:
- a simplified coastline vector set (Natural Earth 110m is tiny) shipped in the bundle,
- an equirectangular or azimuthal projection — azimuthal centred on the receiver is arguably
  *better* for HF than a Mercator tile map, because it shows true bearing and distance,
- spots and paths drawn as Skia primitives, which is what the waterfall already does at 60 fps.

**On tvOS this is the only way to have maps at all.** That it also removes a webview and a runtime
CDN dependency, and works offline, is a bonus that lands on **phones** — the devices that actually
go out of signal. An Apple TV is the most reliably-online device we target, so do not sell this
change to that platform on offline behaviour; sell it on "there is no webview".

### ✅ 6.2d THE DECISION: NO MAPS ON tvOS v1 — AND THAT UNBLOCKS THE WHOLE PORT
**Stuart, 2026-08-03: *"we could even drop the maps on the apple tv."*** Take it. It is the right
call and it changes the shape of the work completely:
- The map views were **the only genuinely hard part** of the port (§6.2). Omitting them means tvOS
  v1 is *the control scheme plus the screens we already have* — no new rendering, no native module,
  no webview replacement.
- ★★ **The Skia map is therefore DECOUPLED, not cancelled.** It stops being a tvOS prerequisite and
  becomes an independent improvement justified on its own terms — for phones, where offline and the
  CDN-in-the-runtime-path actually bite. Do it when it earns its place, not to unblock a TV.
- ★ And it may be the better product anyway: a map of small dots read from a sofa is not obviously
  the right 10-foot UI. If spots are wanted on tvOS later, a **large, legible spot LIST** is cheaper
  and probably more readable than a map — decide that on its merits, once the app exists.

Alternatives, for completeness: **MapKit** exists on tvOS but `react-native-maps` support there is
doubtful and it would be a native module either way; or **omit maps on tvOS v1** and show spots as a
list, which is honest and cheap but leaves the offline/CDN problem in place everywhere else.

### 6.2b ★★★ WHY APPLE TV AND NOT THE "EASIER" TV PLATFORMS — THE REMOTE HAS TWO D-PADS
**Stuart, 2026-08-03: *"apple tv essentially has 2 D-Pads which is the exact navigation method
needed."*** That is the reason this design works, and it is worth stating before anyone proposes a
cheaper platform.

A Siri Remote gives **two directional inputs simultaneously**: the clickpad **ring PRESSES** like a
D-pad, and the touch surface **SWIPES** as a second one. This app needs exactly two — one to
tune/zoom, one to move the highlight — so it gets both **with no mode switch at all**.

★★ **Android TV / Fire TV remotes have ONE D-pad.** They are the easier platforms technically (real
browsers, so the web client would run), but the control scheme is the hard part, and there it breaks:
one D-pad cannot both tune and navigate without a **mode**, and modes are the thing this design
deliberately avoids. So "easier to reach" and "easier to control" point at different platforms, and
**the control scheme is what decides it.** Apple TV first.

★ *If* a single-D-pad platform is ever wanted, the sleep/wake behaviour in §3 already contains an
implicit mode that could carry it — highlight hidden ⇒ arrows tune, highlight visible ⇒ arrows
navigate, with a button to summon it. That is a real fallback, not a good one: it makes explicit and
constant something that on Apple TV is invisible and free.

★ The web client would still be the right vehicle **there**, since it is landscape-first, its
keyboard layer already turns a D-pad into arrow keys ([[keyboard_layer_shipped]]), and its waterfall
is the reference implementation. The blocker is the remote, not the rendering.

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
2. ✅ **ANSWERED — the ring is always live for tune/zoom**, highlight or no highlight. See §1.
3. **Is the tvOS app a separate App Store record or the same one?** Same record is normal for a
   universal app, but tvOS is a separate binary and platform either way.
4. **The decoders panel** (FT8/WEFAX/SSTV) — in or out for v1? Note the decoders themselves are
   native and fine; it is only their **spot MAP** that is cut (§5b), so a decoder panel without a
   map is a real option.
5. ★★ **Does `react-native-tvos` support Expo 57 / RN 0.86 with the New Architecture?** This is the
   one that can invalidate the plan — it decides whether tvOS is a target in this app or a separate
   one. Check before promising a build (§6.3).
