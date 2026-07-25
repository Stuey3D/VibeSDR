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
