# BRIEF — Input Controls, Shack Mode & the Free Mac App

Design session 2026-07-23 (Stuart, dictated). This is the authoritative capture of the whole
"scalable UI" design: how VibeSDR is controlled across every device, how it drives a TV, and how the
Mac app comes almost for free. Nothing here is built yet unless noted. Post-V10 unless folded into the
macOS/menu work. Ordering/priority is Stuart's call — this brief is the spec, not a schedule.

Umbrella positioning it all serves (KEEP THIS WORDING — it is an SEO asset; googling "vibesdr" surfaces
it and Google AI now recommends VibeSDR for mobile because of it — NEVER drop "mobile first"):

> **VibeSDR — an easy-to-use, mobile-first, fully scalable SDR client. Works from a 1" watch screen all
> the way up to a giant TV, and everything in between.**

---

## 1. The drums stay the default — and they're pointer-usable

The drums (VFO + zoom) remain the default control and are untouched. They are usable WITHOUT a
touchscreen:

- **Click-and-drag** — the current method, stays.
- **Two-finger trackpad scroll while hovering over a drum** → drives it, CONTINUOUS so the analogue
  feel is preserved. (Easy to add.)
- **Mouse scroll wheel while hovering** → also drives the drum, for plain non-multitouch mice. Discrete
  (notch = step) — more ratcheted than a smooth spin, but nobody is locked out.

Rules:
- **Hover decides the target:** pointer over the VFO drum → scroll tunes; over the zoom drum → scroll
  zooms. This alone makes a plain wheel-mouse fully usable with NO config.
- **Scroll is hover-scoped, not hijacked:** the wheel/trackpad scrolls normally everywhere (menus,
  lists, page) EXCEPT over a drum. A note in the menu surfaces this: *"Hover over the drum to control it
  with the scroll wheel."*

---

## 2. Button mode — the HiFi tuner (COMMITTED)

An alternative control mode to the drums. Decision: it IS being built (was "optional/reluctant", now
committed). Reasons: already designed; a genuine **accessibility** option (clear labelled targetable
control vs a gesture some users can't perform); and it maps to media controls.

**Per-control granularity (menu → Controls):** VFO and Zoom are INDEPENDENTLY drum-or-buttons — four
combos: both drums (default), both buttons, VFO-buttons + Zoom-drum, Zoom-buttons + VFO-drum. Two
separate toggles.

**Look — like a real digital HiFi separates tuner, NOT the app's standard buttons.** Occupies the space
the drums occupy. FOUR large buttons in a row, each control PAIR straddling its own static glyph:

```
[ < ]   ·radio glyph·   [ > ]        [ − ]   ·magnifier glyph·   [ + ]
   └────── VFO ──────┘                  └────── Zoom ──────┘
```

- 4 large BLACK BRUSHED-METAL button bodies (physical-key look). `<` `>` = VFO, `−` `+` = Zoom.
- A green LED-like glow SURROUNDING each button (halo) so they look SET INTO a real tuner's panel.
- Each button's icon is LASER-CUT and the green backlight glows THROUGH it (not a printed/coloured
  icon — light comes through the cut).
- TWO static glowing-green glyphs, one per pair, sitting BETWEEN that pair's two buttons: the RADIO
  glyph between `<` and `>` (tune), the MAGNIFYING-GLASS glyph between `−` and `+` (zoom). Non-
  interactive brand marks (VibeSDR's brand glyphs / crown-mode icons).
- Bespoke styled component, NOT the standard button style. Green is the brand colour throughout.

**Behaviour — acts like a HiFi tuner, one mechanism:**
- Every press = one step, fired immediately on press. Move the VFO by the current step. Rapid tapping =
  rapid single steps, no debounce/accumulation. Ten fast taps = ten steps.
- Click-and-HOLD = the only special behaviour: after ~350 ms of CONTINUOUS press, auto-repeat begins and
  ACCELERATES — starts slow (~3 steps/s), ramps SMOOTHLY (continuous/analogue, NOT discrete gears) to a
  capped fast scan (~20–25 steps/s) after ~2–3 s, holds at the ceiling. Release stops instantly.
- Fast clicks must NEVER read as a hold: the hold timer is ARMED on press, CANCELLED on EVERY release —
  only fires after 350 ms UNBROKEN contact. No cross-click debounce/merge (would break rapid-taps).
- This is a MANUAL accelerating band SWEEP (fast-forward), NOT auto-seek-to-station (that's the V11
  scanner). ★ TERMINOLOGY: user-facing copy says "SWEEP" / "fast-tune", never "scan" — reserve
  "scan/scanner/scanning" for the V11 auto-seek feature so users don't think it already exists.
- **Scope: UNIVERSAL.** The press/hold accelerating scan applies to the existing ◀▶ step buttons too,
  and STAYS ACTIVE EVEN IN DRUM MODE — not gated behind buttons mode. Great for the car (glanceable
  press-and-hold band sweep).

**Media-control mapping:** wire the tuner buttons into the system Now Playing / remote command centre so
EXTERNAL controls drive tuning — Bluetooth remotes, CAR steering-wheel/head-unit buttons, keyboard media
keys, AirPods stem, Control Centre transport. Natural map: NEXT/PREVIOUS → step up/down; press-and-hold
(where supported) inherits the scan. The watch already registers `MPRemoteCommandCenter`
(play/pause/toggle) — extend next/prev to tuning.

**Haptics on the tuner buttons:**
- TAP = a quick LIGHT haptic on press, so it feels like a physical key clicking in.
- HOLD = a slightly LARGER "thump" the moment the ~350 ms threshold fires and scanning BEGINS — a
  distinct heavier feedback so you FEEL the step→scan transition.
- Applies to the HiFi keys AND the standard ◀▶ step buttons. (iOS `UIImpactFeedbackGenerator`
  light/medium/heavy; Android haptic tick; watch `WKInterfaceDevice`. RN needs a haptics module.)

---

## 3. Pointer support (iPad + Android — NOT iPhone)

**Auto-switch on pointer connect/disconnect.** Detect pointing-device connection status and flip between
two user-set default layouts — a "handheld / no pointer" layout and a "pointer connected" layout.
Example: iPad handheld → drums; snap on a Magic Keyboard trackpad → buttons, automatically. Android
included AS A WHOLE (pointers work on all Android devices, not just tablets).

**★ ONE QUESTION, NOT A LAYOUT MATRIX (Stuart 2026-07-24 — simplifies the whole brief).** Drop the
per-device icon-layout picker. Ask the user exactly ONE thing:

> **"What should the scroll wheel do?"  →  Zoom (default)  ·  Tune**

That single vertical-axis choice DERIVES every other mapping — the user never configures the secondary
inputs, and a person with a basic wheel-only mouse never sees an option that doesn't apply to them:
- **Vertical scroll wheel / 2-finger vertical** → the chosen control (default **Zoom**).
- **The OPPOSITE control auto-maps to whatever orthogonal input the device happens to have** —
  horizontal tilt-wheel, AND/OR the side back/forward buttons. No separate setting; if the hardware
  has it, it does the other thing; if it doesn't, nothing is lost.
- **Trackpad (most Macs + iPads) mirrors the axes:** 2-finger up/down = the wheel choice (Zoom by
  default), 2-finger left/right = the other (Tune). So the default out-of-box trackpad feel is
  **vertical = zoom, horizontal = tune**.
- ★ Side buttons (or a tilt-wheel) acting as TUNE get the SAME HiFi tap/hold semantics (tap = step,
  hold = sweep).

Rationale: the vertical wheel is the ONE input every pointing device shares, so it's the only thing
worth asking about; everything else is orthogonal and can be assigned automatically. Wheel-only users
answer one question and stop; trackpad/tilt/side-button users get the second control for free.

Optional refinement (not required for v1): if the app has OBSERVED a horizontal-scroll or side-button
event, it can surface a tiny "your mouse also has X — it's mapped to Tune" confirmation pill rather than
a config screen. Learn-by-use, never a cold matrix.

**Detection reality (researched):** pure up-front layout detection is NOT reliable.
- Android (good): `MotionEvent` gives AXIS_VSCROLL + AXIS_HSCROLL and getButtonState()
  PRIMARY/SECONDARY/TERTIARY + BUTTON_BACK/FORWARD; `InputDevice.getMotionRange(AXIS_HSCROLL)` hints a
  horizontal wheel. Side-button tuning is feasible.
- iOS/iPadOS (limited): UIKit pointer gives scroll x/y + primary/secondary only; extra buttons generally
  NOT delivered. GameController `GCMouse` exposes scroll x/y + left/right/middle + auxiliaryButtons, but
  it's a capture path and aux-button availability varies by driver. So on iPad: horizontal scroll
  detectable, side buttons unreliable.
- Tilt-wheel: presents to the OS as HORIZONTAL SCROLL (indistinguishable from a side-scroll wheel) → the
  same left/right bucket, no separate detection. Usually discrete steps; some drivers remap to
  back/forward.
- Approach: HYBRID — detect PRESENCE reliably (drives the drum↔buttons switch); learn-by-use / light-up
  on observe for axes & buttons; the icon picker is a pre-filled OVERRIDE, not the sole source of truth.

RN exposes none of this — native-module work both sides (Android `onGenericMotionEvent` + button state;
iOS `GCMouse` / UIScrollView pointer interactions). Watch + Mac are separate input paths.

---

## 4. Keyboard controls (active when a hardware keyboard is present)

**Navigation:**
- ↑ / ↓ = zoom in / out. ← / → = tune, with the SAME HiFi method (tap = 1 step, hold = accelerating
  scan). Arrows are CONTEXT-DEPENDENT: zoom/tune when no panel is open; when a menu/box is open they
  NAVIGATE it.
- Enter = open the FREQUENCY box → type digits → Enter again tunes in the CURRENTLY-SET units. The freq
  box is a TWO-TAB panel; **Tab** switches:
  - Frequency-entry tab: digits, H/K/M switch the unit (Hz/kHz/MHz), Enter tunes; ← / → drive the VTS.
  - Bookmarks tab: ↑ ↓ ← → all navigate the bookmarks grid, Enter selects. ← / → do NOT touch the VTS.
  - RULE: ← / → control the VTS ONLY on the frequency-entry page.
  - H/K/M work ONLY inside the freq box (units); nothing elsewhere.

**Panels (letter opens; inside: arrows navigate, Enter selects, Esc closes):**
- D = demodulator box. M = menu. S = step-rate menu. A = audio menu. C = chat box.
- Esc = servers menu (top), ONLY when nothing is open.

**Esc precedence (single rule):** if a menu/box is OPEN → Esc closes it; if nothing is open → Esc opens
the SERVERS menu; Esc again closes the servers menu. Esc is universal back/close AND the servers opener.

**Conflict rules to bake in:**
1. M is double-booked (Menu vs MHz) → resolve by CONTEXT: freq box open → M = MHz; else M = menu.
2. Text-input focus WINS: while the freq box or chat box is focused, letter keys go to the text field,
   not global shortcuts (H/K/M in the freq box are the deliberate exception).
3. Arrows mode-switch by whether a panel is open.

**Cheat sheet:** a "Keyboard Shortcuts" button in the menu opens a CONTEXT-ORGANISED cheat sheet (grouped
by Waterfall screen / Freq box entry tab / Freq box bookmarks tab / Any open menu / Servers menu).
Ideally CONTEXT-AWARE — lead with the section for where the user is now, full reference underneath.
Doubles as onboarding.

**First-detect pills (both keyboard AND pointer):**
- Keyboard first-ever detect → "Keyboard detected — look in the menu for the keyboard shortcuts guide"
  (points at the cheat sheet).
- Pointer first-ever detect → "Mouse detected — set your controls in the menu" (points at the icon
  layout picker).
- Both ONE-TIME ONLY (separate persisted "seen" flags), non-intrusive, auto-dismissing.

Native handling per platform: iPadOS `UIKeyCommand`/`pressesBegan`, Android `onKeyDown`, Mac `NSEvent`.

---

## 5. Shack mode — VibeSDR full-screen on a TV

Make VibeSDR fill the whole TV as a DEDICATED external display (NOT mirroring) over BOTH AirPlay and
USB-C→HDMI, with the phone as a control surface and a Bluetooth keyboard driving everything via the
keyboard scheme. Stuart: "that with the keyboard shortcuts makes it a real swiss army knife." AirPlay's
~few-ms lag is acceptable.

**Why possible (not letterboxed):** iOS supports a dedicated external display, not just mirroring. AAA
games letterbox because they only mirror. Path: legacy `UIScreen.screens` second `UIWindow`, now the
`UIWindowScene` external-display scene. Wired (USB-C→HDMI) is rock-solid, low-latency, native-res.
AirPlay works for opt-in apps but less consistently + adds compression/latency (acceptable here).

**Architecture — "treat it like a video app":**
- TWO INDEPENDENT RENDER SURFACES, ONE SHARED STATE. The external scene is its own canvas — a BESPOKE
  big-screen composition (not a mirror/not a scaled screenshot). Both surfaces observe the same SDR
  client state; touch on the phone OR a keystroke mutates state → the TV view re-renders. One model,
  two views.

**Default layout:**
- **TV** = the RF panorama: big waterfall + spectrum (+ ticker/VTS/readouts). The heavy GPU waterfall
  renders ONCE, here.
- **iPhone** = controls + the AUDIO ANALYSIS screen (see §6). The phone does NOT render the RF waterfall,
  so its GPU goes to the lighter audio visualiser — no double-waterfall cost. Each surface shows what
  it's best at.

**Independent zoom per surface** (phone zoomed into a signal, TV showing the band overview):
- Flavour A — client-side display CROP: one received stream, each surface renders its own crop. Works on
  EVERY backend, zero extra server load. CATCH: the phone's deep zoom is a magnified crop, resolution
  capped by the stream's bins (goes soft when zoomed far). SHIP THIS FIRST (subscribe at the wide
  overview span, phone crops in).
- Flavour B — true independent-RESOLUTION zoom needs two server subscriptions → breaks on shared servers
  (VibeServer is single-occupant; UberSDR/Kiwi need a 2nd connection). 
- ★ VibeServer-only superpower (we own both ends): a "zoom-inset" second spectrum stream — fine detail
  for the phone while the main stream stays wide for the TV. UberSDR/Kiwi stay crop-only. Strengthens
  the VibeServer value prop.

**Reality:** RN does NOT support secondary displays out of the box — the external UIWindowScene must be
created + driven NATIVELY (native module). The waterfall is GPU/SkSL, so the native layer is well-placed
to render it there. Detect connect/disconnect → enter/exit shack mode automatically. Android parity
later via `DisplayManager` + `Presentation`.

---

## 6. Audio visualiser panel

UberSDR has an audio-domain analysis pane; VibeSDR should too. It is AUDIO-domain, not a second RF view:
- An oscilloscope of the DEMODULATED audio waveform (time domain, ms timebase, tone-freq readout, RMS/
  Peak, Auto-Sync/Auto-Scale/Markers/Pause).
- An AUDIO spectrum + spectrogram (0–few kHz, Peak/Floor/SNR + peak-freq).

100% CLIENT-SIDE, zero server cost/bandwidth — a scope + FFT on the decoded audio buffer we already have.
Works on EVERY backend. Genuinely useful (eyeball CW tones, tune SSB by eye, see RTTY/PSK shift, spot
hum). It's the natural phone-side panel in shack mode (§5). Does NOT provide independent RF zoom.

---

## 7. The Mac app — effectively free

The "Mac app" is the iOS/iPadOS VibeSDR app (Designed-for-iPad) running on Apple Silicon — the same
binary, on the Mac App Store with an opt-in checkbox. It runs PERFECTLY as a network client already.

- **Usable via the drums by scroll** (§1) — hover + two-finger scroll / wheel — so it's usable without
  touch. Keyboard + HiFi buttons + pointer options are ADDITIVE, not what rescues usability.
- The shared keyboard/pointer/button input work (§2–4) carries into the iOS-on-Mac runtime (UIKit APIs),
  so the Mac experience falls out of the iPad/Android effort for free.
- **Local RTL-SDR without fighting the sandbox:** the iOS-on-Mac runtime is sandboxed with NO IOKit/USB,
  so it can't drive a USB dongle directly (VERIFY with a ~20-min libusb-enumerate test; strong prior =
  fails). Instead, local SDR comes from the native VibeServer:
  - **VibeServer "this device only" mode:** bind loopback only + mDNS advertise OFF + PIN-optional
    (loopback is already PIN-exempt). Dongle served only to localhost (built-in web client OR the local
    VibeSDR app).
  - **VibeSDR app auto-detects the local VibeServer and lists it as an "ON THIS DEVICE" radio** — its own
    picker section above network servers, auto-connect, no PIN. Reuses the existing localhost port-scan
    (48000–48049), the loopback PIN-exemption, and the "discovered" section pattern. Reads as a built-in
    local radio; really client→localhost→VibeServer.
  - This means the earlier "graduate the Mac slot to Catalyst for USB" plan is UNNECESSARY — the client
    stays on the free Designed-for-iPad path; the dongle is VibeServer's job. Catalyst only if we ever
    want in-CLIENT USB specifically.
- Same client+local-VibeServer pairing works anywhere both can run (Mac primarily; Android too).
- Shack-mode external display is largely moot on Mac (just use a big monitor full-screen). Same iOS
  pointer limits apply on Mac (GCMouse extra-buttons iffy).

---

## 8. Related V10 sync item (separate track, noted here for completeness)

Part of the V10 iCloud KVS sync (favourites + spectrum/VFO colour + watch-flagged bookmarks): add
**last-tuned-station PER SERVER, not per device.** Today each device restores its own last freq per
server; Stuart wants it keyed to the SERVER across devices (tune 5 Live on the phone → open the watch on
that server → it resumes on 5 Live). Design: ONE KVS key = JSON map `serverKey → {freq, mode, ts}`; write
on tune (debounced), read on CONNECT only (never yank a live session — KVS is eventually-consistent).
serverKey = normalised host:port / lowercased-trimmed url, same both apps. Last-write-wins by ts; fall
back to local per-server memory. One blob (not key-per-server) for atomic sync + KVS limits; prune to
most-recent N. Works for every backend. See BRIEF/[[jr_bookmarks]] for the full sync deliverable.

---

## Cross-references (session memories)
buttons_hifi_tuner_design · external_display_shack_mode · audio_visualiser_panel · mac_native_build ·
vibeserver_mac_standalone · rtlsdr_mac_ipad_usb · jr_bookmarks · positioning_and_readme · v10_scope.
</content>

---

## ★★ POSITIONING — the two control schemes are two PORTABLE radios (Stuart, 2026-07-25)

Built, and the framing changed on contact with the result. The plan was to sell the keys as a "90s
HiFi separates tuner"; what they actually look like is CAR STEREO buttons — and that is better,
because a car stereo is PORTABLE, like a boombox, where a separates tuner is furniture. VibeSDR is
portable, so both controls now point the same way:

- **Drums — the 80s boombox.** Analogue, tactile; you hunt with your thumb.
- **Keys — the 90s car stereo.** Discrete, glanceable; works from real hardware buttons.

★ This is not styling. The keys are wired into the media-control path, so a car's steering-wheel or
head-unit skip buttons literally drive them — the VFO key and a car stereo's skip button are the
same code path. "Tune without looking" describes the implementation.

★ They also divide by JOB rather than by era, which is the honest reason to ship both: the drums win
for hunting across a signal, the keys for covering distance. Stuart, having been reluctant to offer
any alternative to the drums at all: *"these tuning buttons feel sublime for large tunes... I think I
actually prefer them to the drums"*, and *"we've not got 1 but 2 amazing feeling control schemes that
feel like their real life counterparts."*

★ The reference that landed closest was a Kenwood Excelon CAR head unit — symbols backlit through
the key faces — which is why the look went where it did rather than towards separates.

---

## ★★ SHACK MODE — AirPlay latency TESTED, and it is a non-issue (Stuart, 2026-07-25)

Impromptu test on real hardware: VibeSDR on an iPhone, **basic AirPlay MIRRORING** to an Apple TV on
a Sony set, driven by the hardware-keyboard controls built the same day. Verdict: *"everything was
super responsive... makes our shack mode so much more viable."*

★ **This removes the feature's biggest risk before a line of scene code was written.** The worry was
that AirPlay latency would make big-screen control feel disconnected and undermine the whole idea.
It does not — with a keyboard in the loop it is responsive. That could not have been settled by
reasoning; it needed the TV.

★★ **And there is a shippable shack mode TODAY with no code at all:** mirroring + a Bluetooth
keyboard already gives the big-screen panorama with hands-on-keys control. Worth DOCUMENTING as a
supported setup rather than leaving users to discover it. It also means the separate-scene work below
is an ENHANCEMENT on something already usable, not a prerequisite for anything working.

★ Keep the distinction straight: mirroring shows the SAME screen on both. Stuart's gate — RF
waterfall on the TV with the controls on the phone, INDEPENDENTLY — still needs a second
`UIWindowScene`. What is proven is that the transport is not the obstacle.

★ Implementation note for when that is built: `AppDelegate.swift` already has a `SceneDelegate`
conforming to `UIWindowSceneDelegate`, so the machinery is in the right shape. The blocker to check
FIRST is the `UIApplicationSceneManifest` in `Info.plist` — declaring an external-display scene means
editing the one file `expo prebuild` regenerates, and the pbxproj is hand-maintained here too.

### ★★ The real reason for a separate scene: POWER, not layout (Stuart, 2026-07-25)

*"The issue is power — the iPhone screen off to save battery and you lose the screen too. In theory
our shack mode with a separate waterfall could allow the iPhone screen to be turned off and the app
controlled purely by keyboard."*

★ Mirroring COUPLES the two displays: lose one, lose both. So the phone must stay lit to feed a TV
that is doing all the actual displaying — burning battery to show a screen nobody is looking at. A
separate `UIWindowScene` decouples them, and THAT is the justification for building it. Not a nicer
layout — a phone that costs almost nothing to leave running.

★ It also completes the keyboard work: if the phone does not need looking at, it does not need
lighting. The two features only make sense together.

**Two versions, and only one is certain — build the reliable one first:**

1. ★ **RELIABLE — phone foreground, screen BLACK and DIM.** A minimal control surface, or nothing
   but a status line. On an OLED iPhone an all-black screen at minimum brightness draws very little,
   so most of the saving is there, the session never drops, and the keyboard does everything.
2. ★ **UNCERTAIN — true device LOCK with the TV still rendering.** An external-display scene keeps
   rendering while the app is FOREGROUND; once the device locks the app is backgrounded and arbitrary
   GPU rendering is very likely suspended. Video apps are not a precedent — `AVPlayer` has explicit
   external-playback support, whereas we would be asking for continued Skia rendering. We already
   know from [[ios_background_audio]] that background audio keeps the app ALIVE without granting
   rendering, so the precedent is discouraging. TEST it once the scene exists; do not design for it.

★ Consequence for the design: treat "TV mode" as a UI state of the FOREGROUND app (dim/blank the
phone surface, disable the idle timer so it does not lock itself), not as something that survives
locking. If (2) turns out to work, it is a bonus on top.

### ★★ TV MODE on the phone — the detail (Stuart, 2026-07-25)

*"If we detect a user has not interacted with the iPhone screen when mirroring and only used the
keyboard, black the screen out but just put a small pill 'External display in use, tap to wake iPhone
display'... the iPhone stays awake with our app open anyway so no risk of accidental sleeping and
locking... however I'd like to make the onscreen element move about a bit to prevent OLED burn-in
during prolonged usage."*

**Entry condition** — all three, so it can never surprise someone who is using the phone:
- an external display is in use (mirroring or a separate scene), AND
- no touch on the phone for N seconds, AND
- recent KEYBOARD activity — proof someone is driving it another way.
Both signals already exist: `markInteract()` for touches, and the new key events.
★ VERIFY: does AirPlay MIRRORING report `UIScreen.screens.count > 1`? A separate scene certainly
does; mirroring historically did, but confirm before relying on it as the trigger.

**Exit** — any touch anywhere. The pill says so: *"External display in use — tap to wake iPhone
display."*

**No sleep risk.** The app already holds the idle timer, so the phone will not lock itself out from
under a session. Worth asserting that explicitly in TV mode rather than relying on it.

★★ **STOP RENDERING, do not merely cover.** A black overlay with our Skia canvas still drawing
underneath keeps ProMotion pinned high and the GPU busy — that saves the backlight and almost nothing
else. TV mode must UNMOUNT the phone-side waterfall/spectrum so the display genuinely idles and
ProMotion can fall to its floor. On a separate scene the TV keeps its own render; under mirroring
there is only one render, so this applies to the separate-scene version.

★★ **Dim it ourselves.** `UIScreen.brightness` is writable with no permission, so TV mode can drop
the phone to minimum and restore the user's value on wake. All-black OLED at minimum brightness is
close to off. Remember to restore on exit AND on backgrounding — leaving someone's phone at minimum
brightness would be a horrible bug.

★ **Burn-in: move the pill.** A static element on an otherwise black OLED for hours is exactly the
burn-in case. Reposition it periodically (a minute or two) within the safe area, and:
- ★ **Move it LIKE A SCREENSAVER** (Stuart) — a slow crossfade, on a timescale you do not
  consciously notice. That is the whole feel: ambient, unhurried, never an event. A two-second
  fade every minute would still read as *something happening* in the corner of a dark room;
  aim for a long dwell and a fade slow enough that you only notice it has moved, never that it
  is moving. Never a jump — a jump in peripheral vision reads as a glitch.
- Keep it DIM and low-contrast, not full white; brightness drives burn-in as much as duration.
- Vary the position properly (walk around the screen), rather than alternating between two spots.

### ★★★ WHERE THE CONTROLS LIVE — the three states (Stuart, 2026-07-25)

★ **The principle: exactly ONE control surface exists at any moment, and it lives wherever the user's
hands are.** Never both. TV space is valuable and duplicating the controls wastes it on something the
user is already touching somewhere else. The TV's UI grows precisely as the phone's shrinks.

| state | phone | TV |
|---|---|---|
| **1. Big screen only** (user chooses this deliberately) | — | waterfall **+ a compact control ISLAND**, sized proportionately, fully keyboard-driven |
| **2. Both screens** (phone in hand, TV for the panorama) | the **main island** | waterfall + **frequency and signal meter ONLY** — no controls |
| **3. Keyboard only** (detected → phone goes to the powersave mode above) | black, drifting pill | waterfall + frequency/signal bar **+ the 4 extra buttons fade IN around it** |

**State 1 — big screen only.** A size-proportionate island on the TV, fully controllable from the
keyboard. ★ **DROP the drums and the tuning keys**: they are touch controls and there is no touch on a
television, so they would be decoration occupying the most valuable space on screen. Keep the MIDDLE
section — frequency, mode, signal — and move the status sections UNDERNEATH it. A compact pill.

**State 2 — both screens.** The main island stays on the PHONE, where the hands are. The TV carries
only the frequency and the signal meter: the two things you glance at, and nothing you would reach
for.

**State 3 — keyboard only.** Detected as in the TV-mode spec above. The phone's controls **fade out**
as it blacks down, and the **4 extra buttons fade IN** around the TV's frequency/signal bar — step
rate, audio, menu, chat. So the TV gains exactly what the phone gave up, at the moment it gives it up.

### ★★★ The governing structure: a PERMANENT PILL, progressively FLANKED (Stuart)

*"On the TV at all times the SNR and Frequency box — it's a small self-contained pill, and depending
how the user is using the app is if the rest of the controls then flank it."*

★ This is the right structure, and it supersedes thinking of the three states as three layouts. There
is **one anchor that is always present** — the frequency + SNR pill, self-contained, centred — and the
rest of the controls **flank it** according to how the app is being used:

| how it is being used | what flanks the pill |
|---|---|
| phone in hand (state 2) | nothing — the pill alone |
| keyboard only (state 3) | the 4 extra buttons — step, audio, menu, chat |
| big screen only (state 1) | the full compact island: middle section, status underneath |

★★ Why this is better than switching layouts: **the pill never moves.** Nothing reflows, and nothing
appears where the eye was not already resting — which matters enormously on a display watched from
across a room. Transitions become purely ADDITIVE around a fixed centre: flanks fade in, flanks fade
out, anchor untouched. It also means there is only ever one thing to get right visually, and the
states differ by what surrounds it rather than by what it is.

★ So implement it as: always render the pill; render flank groups conditionally with a fade. Not a
state machine over layouts — a fixed anchor with optional neighbours.

### ★★ THE ONE HARD EXCLUSION: tuning and zoom controls NEVER go to the big screen (Stuart)

*"The only thing that should never go to the big screen is the tuning/zoom controls, since they are all
on keyboard and these are a dead element on non-touch screens or without a mouse."*

★ The drums AND the tuner keys are excluded from the TV in EVERY state, including big-screen-only.
They are direct-manipulation controls: a drum needs a drag, a key needs a press. A television has
neither, so on the TV they are a **dead element** — pixels that look interactive, are not, and occupy
the most valuable space on the screen. Worse than useless, because they invite a reach that cannot
land.

★ And nothing is lost, which is what makes the exclusion clean rather than a compromise: tuning and
zoom are ENTIRELY covered by the keyboard (arrows, with the same tap-step / hold-sweep law) and by a
pointer if one is present. The function moves to the input device; only the on-screen widget is
dropped.

★ RULE FOR ANY FUTURE TV FLANK: include it only if it is *readable* (status, frequency, meters) or
*actuable from a keyboard*. If its only affordance is touch or drag, it stays on the phone.

### ★★★ SETTLED: NO ANALOGUE STICKS — the D-pad does tune/zoom, like the arrow keys (Stuart)

★ Right for a stronger reason than simplicity: **it is the only mapping that satisfies Apple's floor
with ONE code path.** tvOS MANDATES that the Siri Remote works; the remote has no sticks, but its
clickpad reports as **`dpad` on `GCMicroGamepad`, the same element name `GCExtendedGamepad` uses.** So a
single D-pad handler covers the Siri Remote, every game controller, and the keyboard arrows — all on the
already-shipped, device-validated `createHoldSweep` law (tap = step, hold = ramp to sweep, with
`sweepTargetRate` step-awareness). The D-pad also drives menu navigation: one device, both jobs, no
modes.

★ Put the other way: had tune/zoom lived on the sticks, we would STILL have needed the D-pad path for
the remote. Sticks were additive complexity that removed no obligation — a second control law, a
deadzone to tune, and a new sweep curve to validate, all to duplicate something already working.

★ The door stays open rather than closed: a stick, if present, can MIRROR the D-pad, and a velocity law
(deflection = rate) could be added later as an enhancement rather than a redesign. Right-stick =
waterfall pan is worth having where a stick exists but can never be core, since panning has no Siri
Remote equivalent at all.

### ★★★ THE STICKS ARE A TUNING DIAL — rotate them, do not deflect them (Stuart)

★★ **I MISREAD THE ORIGINAL PROPOSAL and argued against the wrong thing.** I read "sticks for tune/zoom"
as deflection = rate, a velocity control. Stuart meant **CIRCLING the stick clockwise/anticlockwise —
angle accumulating into steps — to emulate an old tuning dial with the little finger dimple.**

★ That dissolves the main objection above. Rotation is **not a second control law: it is the DRUM law**,
which already exists and is already tuned — accumulate angular delta, convert to steps, with the
debounce settled on the GPU-waterfall prototype. Nothing new to validate.

★★ **AND IT UNIFIES WITH THE SIRI REMOTE.** The remote's clickpad reports continuous analogue x/y (see
the tvOS findings), so a finger circling it yields angular motion — **the SAME handler**: read x/y,
compute angle, accumulate delta. ONE implementation serves the remote's touch surface AND a stick,
rather than two features. Missed while thinking of sticks as velocity.

★ The nostalgia lineage now completes: **80s boombox drums → 90s car stereo keys → the tuning dial with
the finger dimple.** A third era of radio control, on the surface that suits it. Feeds the website story.

#### ★★★ THE WHOLE SCHEME IN ONE LINE (Stuart): D-PAD = THE KEYS. STICKS = THE DRUMS.

*"So D-pad will act like the keyboard arrows do now and map to the 90s car stereo buttons. Analogue
sticks map to the drums."*

★ The controller needs **NO NEW CONTROL IDENTITY** — it supplies the two schemes already built and
validated on device. D-pad = the tuner keys (discrete tap-step / hold-sweep, `createHoldSweep`);
sticks = the drums (rotational displacement, rim-engaged).
★ Worth noticing: **a controller is the only surface that offers BOTH schemes AT ONCE**, one under each
thumb — gross stepping and fine rotation simultaneously, with no toggle. More than the phone or the
keyboard gives alone.

★★ **OPEN TENSION, deliberately not resolved — Stuart's call.** The hard exclusion below says the drums
and keys never appear on the TV, its rule being *"include it only if it is readable or actuable from a
keyboard; if its only affordance is touch or drag it stays on the phone."* **A connected controller makes
them ACTUABLE**, so the rule's own logic would readmit them — and a drum visibly rotating on the TV in
step with the thumb on the rim would teach the mapping instantly and is the app's signature visual.
★ Against that: the principle settled for the flank set — **the TV layout must not change shape when a
controller connects or drops.** Drums appearing on a Bluetooth event is exactly the reflow that reads as
a glitch from across a room.
★ MY LEAN (not a decision): keep them OFF, and let the GLYPHS be the only controller-dependent element,
because glyphs are ADDITIVE AND SMALL where a drum is STRUCTURAL — and the drums' feedback is already
carried by the waterfall and the frequency readout moving. But it is a taste call about the signature
visual, so it is Stuart's.

★★ **AND THERE IS NO PREFERENCE TO ASK (Stuart).** *"The only thing we don't have to do is ask the user
what they prefer, as they have both at once."* A setting you never have to ask beats one you get right:
no preference UI, no persistence, no per-device default, no wrong answer to recover from. ★ The PHONE
only forces the drums-or-keys choice because of SCREEN SPACE — the controller shows that was always a
SPATIAL constraint, not a design one.
★★ **THIS COLLAPSES THE OPEN TENSION ABOVE.** The only real reason to put a drum on the TV would be to
show WHICH scheme is active — but there is no mode to communicate, both being live under both thumbs
permanently. No state, nothing to indicate, so the widget has no informational job left and the case for
pill-plus-glyphs only gets stronger. Stuart's own point argues the side I was leaning to.
★ ONE DISCOVERABILITY RISK, created by the clutch: a user who rotates WITHOUT pushing to the rim gets
nothing and may conclude it is broken. So the **first-detect pill** (already listed for keyboard and
pointer) needs a CONTROLLER variant teaching the rim explicitly — *"push the stick to the edge, then
rotate to tune."* The one place the clutch costs something, and a single pill pays for it.

★★ **HAPTIC DETENTS — feel the clicks as you rotate (Stuart).** Completes the metaphor: a real tuning
dial has detents. `GCController.haptics` gives a `GCDeviceHaptics`, from which you create a
`CHHapticEngine` per **locality** — and `GCHapticsLocality` has separate **`leftHandle` / `rightHandle`**
(tvOS 14+, iOS 14+, MacCatalyst 14+). ★ So the detent fires from the SIDE BEING ROTATED: left stick tune
clicks the LEFT grip, right stick zoom the RIGHT. The click feels like it comes from the dial under the
thumb rather than the whole controller buzzing — the difference between feeling real and feeling gimmicky.
★★ **IT ALSO SOLVES THE DISCOVERABILITY RISK ABOVE without the pill:** a distinct **ENGAGE CLUNK** when
the rim is reached tells the user the clutch has bitten. TWO distinct events — a firm clunk on
engage/release, a light tick per detent while rotating.
★★ **THE RATE POLICY ALREADY EXISTS AND IS BETTER THAN A RATE CAP** — `DrumWheel.tsx:122-142`:
`gap > 90ms → ImpactFeedbackStyle.Rigid` (deliberate, slow click), `else → selectionAsync()` (lighter
tick when moving fast). It DOWNGRADES rather than DROPS, so fast rotation becomes a fine TEXTURE instead
of a buzz or a silence. Maps straight onto CoreHaptics as sharpness/intensity on a
`CHHapticTransientEvent`, so the tuning already done carries over rather than being re-derived.
★★ **A REAL CATCH TO FIX WHEN BUILDING THIS:** the `✦ HAPTICS` toggle (`MenuSheet.tsx:1115`) is gated on
whether **THE DEVICE** has a haptic motor — which is why it is hidden on iPads (see AboutOverlay 5.2.2
notes). But **an iPad or Apple TV with a DualSense connected DOES have haptics — the motor is in the
CONTROLLER.** The visibility rule must become "device motor **OR** connected controller with a haptics
profile", or the toggle vanishes exactly where this feature is most used.

##### ★★ THE FIRST-DETECT PILL FOR THE STICK (Stuart's design)

*"If a user tries to tune by pushing the stick up/down/left/right they get a little pill with a stick
animation showing push to the edge then rotate, showing rotation clockwise and anticlockwise, and maybe
a simple number going up and down to show what each does."*

★ **The trigger is the right one**: it fires when the user has just revealed the misconception WITH THEIR
THUMB — an answer to a question they actually asked, rather than a tutorial on connect that nobody reads.

★★ **TRAP: the trigger needs a DWELL, or it fires on CORRECT use.** Pushing to the rim PASSES THROUGH a
cardinal deflection on the way, so "deflected in a direction" alone would pop the pill every time someone
engages properly. Condition must be **deflected + held + NO angular delta accumulating for ~400 ms** —
the user pushing and WAITING for something to happen. That is the actual signature of the misconception.

★ **The number is the cleverest part**, because rotation DIRECTION is the one thing a diagram of a circle
cannot convey on its own — clockwise-equals-up is arbitrary until something shows it, and a rising/falling
number needs no language at all (also dodges localisation).
★ Considered and REJECTED: showing the REAL frequency instead of an abstract number (would teach step
size too). Stuart's simple number is better here because **this pill also appears on the TV at sofa
distance**, where two or three legible digits beat a nine-digit readout with one digit moving.

★ **TWO VARIANTS, OR ONE COVERING BOTH — decide.** If the right stick zooms, the same misconception
applies there and a rising NUMBER would be the wrong illustration. Either one generic pill teaching "rim
then rotate", or a per-stick pill showing tune vs zoom.

★★ **TIE THE ANIMATION TO THE HAPTIC** — visual emphasis at the moment the stick reaches the rim, the same
instant the ENGAGE CLUNK fires. Seeing and feeling the engagement together is what makes the CLUTCH click
as a concept rather than being two separate things to learn.

★ Note it SIDESTEPS the drums-on-TV question rather than reopening it: a stick animation is TRANSIENT and
INSTRUCTIONAL, not a control you reach for, so it neither reflows the layout nor invites a touch that
cannot land.

#### ★★★ PUSH TO THE RIM, THEN ROTATE — and neutral is a CLUTCH (Stuart)

*"Push to the full edge of the stick's travel then rotate. Maximum movement for maximum precision. Also
acts like a lock — sticks in neutral, the control deactivates. You have to WANT to make it work, not
accidentally knock it and knock your tuning off."*

★ Better than the "minimum-radius gate" I had suggested: same physics used POSITIVELY rather than
defensively. Angular resolution for a given positional jitter is **ε/r**, so precision **scales with
radius** — at the rim you get the finest control the hardware can give. It earns three things at once:

1. ★★ **The hardware supplies a GUIDE RAIL.** Sticks have a circular gate at maximum travel, so circling
   at full deflection is a TACTILE motion — the thumb rides the rim and can feel it. Exactly what a real
   tuning dial's rim does, so the metaphor is physical, not merely visual.
2. **The deadzone problem disappears** — angle is never counted near centre, which is precisely where
   angle is meaningless.
3. ★★ **The CLUTCH is the best part.** Neutral = disengaged, so tuning requires deliberate intent and a
   knock cannot drift the frequency. Philosophically consistent with the existing VFO lock: the app does
   not let the band move by accident.

★ IMPLIED DIVISION OF LABOUR, state it explicitly: **D-pad = GROSS movement** (hold-sweep across a
band); **stick rim = PRECISION** (settling onto a signal). Sustained full-deflection circling would tire
a thumb over a long sweep, but that is not its job — same reasoning as drums-versus-keys.

★★ FOUR IMPLEMENTATION DETAILS THAT DECIDE WHETHER IT FEELS RIGHT:
1. ★ **Entry angle becomes the ZERO REFERENCE.** Engagement captures the current angle and only the
   DELTA from there counts — otherwise entering at the top would jump the tuning by 90 degrees.
2. **Shortest-arc unwrapping** at the ±180 degree crossing, or one lap sends you flying. The drums
   already solve this.
3. **Hysteresis on the threshold** — engage ~0.9 of travel, disengage ~0.7. A single threshold chatters
   on a slightly sloppy circle.
4. ★★ **Engagement is PER-DEVICE, rotation is SHARED.** The Siri Remote has no rim and no deflection, so
   there it is finger-down to engage, lift to release — the same explicit-intent pattern, a different
   trigger. So: ONE rotation handler taking an `engaged` flag, supplied by RADIUS on a stick and by TOUCH
   on the remote or a drum.

★ ONE REMAINING LIMIT, not fatal: **a stick SELF-CENTRES**, so there is no coasting or inertia —
displacement tuning, not a weighted flywheel you can spin. Fine: the drums are the same, and the
flick-coast was removed deliberately.

★ THE D-PAD DECISION STANDS as the FLOOR — it is what guarantees the minimum control set on any
hardware. Rotation is the ENHANCEMENT tier, now with a real reason to exist rather than being a fallback.

### ★★★ THE MINIMUM CONTROL SET: directions + confirm + back. The rest are ACCELERATORS.

Prompted by Stuart asking whether half a Nintendo Switch Joy-Con still gives full control. Apple has
supported single Joy-Cons since **iOS 16** (either half usable alone), and the wrinkle is that **each
half has one of the two things, not both**:

- **Left Joy-Con** — its four buttons ARE the directions, so a genuine D-pad, plus SL/SR and minus. But
  **no spare face buttons**, because the four ARE the D-pad.
- **Right Joy-Con** — ABXY, but **no D-pad at all**. Only the stick for directions — the exact case the
  no-sticks decision above designed out.

★ So the rule that makes "any controller works" actually true: **guarantee the app is fully operable
from directions + confirm + back alone**, and treat the shoulders (step rate) and the face buttons (the
three flanks) as SHORTCUTS. This is already sound, because audio, menu and chat are all reachable from
inside the menu — the flank buttons are accelerators to things that have another route.

★★ **Consequence: stick-mirrors-D-pad is NOT optional decoration — it is what makes the right Joy-Con
work at all.** Keep it, but as a COMPATIBILITY feature rather than a feel feature.

★ It also helps that iOS, iPadOS and tvOS all offer system-wide and per-app controller remapping, so
genuinely exotic hardware is something the user can fix themselves.

### ★★ SETTLED: step rate moves to L1/R1, so the flank set is THREE (Stuart, 2026-07-25)

The controller mapping (see below / memory `external_display_shack_mode`) forced a useful question: the
flank set is **step rate, audio, menu, chat** = four, but a tvOS controller has only THREE free face
buttons, because **B/circle is reserved by App Review** for backing out and exiting to Home (Play/Pause
maps to X). Stuart's call: **put step rate on the SHOULDERS (L1/R1)**, leaving audio / menu / chat →
A / X / Y with B untouched.

★ This is better than an arithmetic fix, because it splits by ACTION TYPE:
- **Step rate is a CYCLE through a fixed ladder** (five ordered rungs in StepPicker) — exactly what
  paired shoulder buttons exist for. L1 down a rung, R1 up.
- **Audio, menu and chat all OPEN A PANEL** — a momentary action, which is what a face button is for.

★ Bonus on a room-scale display: shoulders are reachable WITHOUT LOOKING, so the most-adjusted
parameter in the app never pulls focus off the waterfall. Step rate stays VISIBLE in the pill — it just
carries an L1/R1 glyph rather than being a button you navigate to.

★ CONSEQUENCE FOR THE TV LAYOUT: **flank group = three buttons, not four.** The four-button wording
above is superseded for the controller case. Keyboard-only (state 3) may still show all four as
on-screen buttons since a keyboard has no shoulders — decide whether the flank set is per-input-device
or fixed at three for consistency. ★ Recommend FIXED AT THREE with step rate shown as a readout in the
pill for both, so the TV layout does not change shape when a controller connects or drops.

★ **All transitions FADE.** Same reasoning as the drifting pill: this is a room-scale display being
watched from a sofa, and anything that snaps reads as a fault. Fading also means a brief overlap is
harmless, which keeps the state machine simple.

★ Needs a user-facing control for state 1 ("big screen only"), since it is deliberate rather than
detected. States 2 and 3 follow automatically from what the user is touching.
