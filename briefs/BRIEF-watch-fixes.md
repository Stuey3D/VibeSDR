# BRIEF: Watch Fixes — Spectrum Background Recovery + Volume Control + Link Diagnostics

**Branch:** `experimental` (analysis at HEAD `e2ecc40`)
**Files touched:** `src/services/UberSDRClient.ts`, `src/screens/SDRScreen.tsx`, `ios/VibeSDR/VibePowerModule.swift`, `ios/VibeSDR/VibeWatchModule.swift`, `src/services/watchProvider.ts`, `ios/VibeSDRWatch/WatchLink.swift`, `ios/VibeSDRWatch/ContentView.swift`, watch UI (Part B control surface TBD by implementer — likely `ControlMenu.swift`)

Three independent pieces of work; implement and test Part A first (it is the bug fix), then C (it reports on A's behaviour), then B.

- **Part A — Spectrum background recovery.** Severity: High — primary watch use case (locked phone in pocket, cellular) has no waterfall recovery path at all.
- **Part B — Watch volume control.** iPhone **system** volume readback + control from the wrist, replacing yesterday's failed app-gain attempt.
- **Part C — Watch link diagnostics.** Make the existing warning pill say WHICH hop is rough (phone↔server vs watch↔phone), and keep degraded-but-working spectrum visible behind a pill rather than a hard overlay.

---

# PART A — Spectrum Background Recovery (Watch / Locked-Phone / Cellular)

## A1. Symptom

Watch waterfall freezes mid-session and never recovers; crown tuning continues to work. Occurred on **cellular**. Foreground use self-heals; background/watch use does not. The watch shows `why = "idle"` (or just sits on the last frame) indefinitely.

## A2. Root cause — two holes, one asymmetry

The **audio** WS has a native watchdog that works in the background. The **spectrum** WS has recovery machinery that is almost entirely gated on an AppState `active` transition — which never fires while the phone is locked in a pocket.

### Foreground recovery (works, do not regress)

1. `UberSDRClient.ts` `ws.onclose` (~549) → `_scheduleReconnect()` (~863) → fixed 3s `_openSpectrumWs()`.
2. SDRScreen AppState handler (~2415–2534): on `active` → native `revive()` (instant audio zombie check) → 1.2s later `resumeSpectrum()` (~434), which **force-closes and opens a fresh socket** specifically because a half-open zombie never fires `onclose`.
3. Reinit watchdog (~2473) escalates to the `specFailed` UI escape hatch.

### Hole 1 — half-open zombie spectrum WS (matches the observed symptom)

On cellular, CGNAT rebinds on cell handover, RRC idle transitions, and IP changes — silently invalidating the TCP flow with no FIN/RST. The spectrum WS sits `OPEN` and starved forever:

- `_evalLink()` (`UberSDRClient.ts` ~667) already computes a `starving` condition (`now - lastFrameAt > max(2000, med*4)`) but **only downgrades reported link quality — it never acts**.
- `watchProvider.ts` (~558–562) flips `why` to `'idle'` after 2s and nothing escalates.
- The audio WS suffers the same rebind but `reviveIfDead` (`VibePowerModule.swift` ~524; 4s health timer at ~517, 8s staleness) reopens it natively with the same uuid → audio and tuning recover, waterfall stays dead. Hence "tuning works, waterfall frozen".

### Hole 2 — reconnect ordering race (when onclose *does* fire)

`_scheduleReconnect` reopens after a flat 3s with zero coordination with audio. The audio watchdog can take up to ~12s (8s staleness + 4s tick). If the server reaped the session during the outage, the spectrum reopens **first** against a nonexistent session: the WS connects fine, receives zero frames, and — being `OPEN` — never retries. `connect()` (~165–183) deliberately sequences `_checkConnection` → audio → **1s delay** → spectrum for exactly this reason; the background reconnect path has no such sequencing. Note `_openSpectrumWs` never re-POSTs `/connection` — only `connect()` does.

### Contributing: no network path monitoring anywhere

Grep confirms zero `NWPathMonitor` (native) and zero NetInfo usage (JS). WiFi→cellular handover or a mid-session cellular IP change produces no signal to any layer except "packets stopped".

---

## A3. Fix — four phases

### Phase 1 — Spectrum starvation watchdog in `UberSDRClient` (primary fix, transport-agnostic)

Add a self-contained staleness watchdog that force-resubscribes when the socket is open but dead. Catches CGNAT rebinds, WiFi zombies, and server session reaps identically.

**Detection (either condition):**
- **Frame staleness:** `spectrumWs.readyState === OPEN && !pausedByApp && !destroyed` and no frame (binary or JSON) for **10s** (well clear of the idle-saver divisor: at `IDLE_DIVISOR = 3` and rate divisors, worst-case legitimate inter-frame gap must be measured — compute the threshold as `max(10_000, expectedGapMs * 4)` using the same `med` logic `_evalLink` uses, so a rate-divided feed is never misread as dead).
- **Pong timeout (faster path):** the existing 5s ping (`_openSpectrumWs`, ~532) already tracks `pingSentAt`. Add an outstanding-pong check: if a ping was sent and no pong arrived within **12s** (two missed cycles + margin), the link is dead regardless of frame cadence. This catches WFM/full-rate cases in ~12s instead of waiting on frame-gap statistics.

**Action (`_forceResubscribe()`):**
1. Guard: `!destroyed && !pausedByApp`; rate-limit to **one attempt per 15s** (`lastForceReopenAt`).
2. Close the old socket defensively (`try { ws.close() } catch {}`), null it — mirror `resumeSpectrum()`'s fresh-socket semantics.
3. **Audio-first ordering:** do NOT reopen immediately. Emit a new callback `onSpectrumDead?()` and wait for audio confirmation (Phase 3). Fallback: if no audio-alive signal arrives within 5s, reopen anyway (the session may be fine and only the spectrum flow died).

The watchdog timer must be a plain `setInterval` in the client (JS stays alive in background via the audio session). Clear it in `pauseSpectrum()` and `destroy()`; arm it in `_openSpectrumWs()`.

### Phase 2 — `NWPathMonitor` in `VibePowerModule` (fast path-change recovery)

`VibePowerModule` already owns the audio WS lifecycle and runs in background.

1. Add an `NWPathMonitor` started alongside the audio engine (`startHealthTimer` vicinity). On `pathUpdateHandler` where the path identity changes (interface type change, or `path.status` transitions through `.unsatisfied` → `.satisfied`):
   - Call `reviveIfDead(staleAfter: 0)` — treat the audio socket as suspect immediately rather than waiting the 8s staleness window. Keep the existing debounce (`lastPacketAt = Date()`) so it can't loop.
   - Emit a `VibeNetworkPathChanged` event over the existing `NativeEventEmitter` bridge.
2. JS side (`UberSDRClient` or a thin listener in SDRScreen wired into the client): on `VibeNetworkPathChanged`, invoke the same `_forceResubscribe()` path from Phase 1 (respecting its rate limit and `pausedByApp`).
3. Ignore the very first path report on monitor start (it always fires once with the current path — not a change).
4. Guard everything on `isRunning`; stop the monitor in `stopAudioEngine`/teardown.

### Phase 3 — Audio-first sequencing for background spectrum reopens

Replicate `connect()`'s ordering in every recovery path:

1. Expose audio liveness to JS: `VibePowerModule` already tracks `lastPacketAt`. Add either an exported `isAudioAlive()` (packets within 2s) or emit a lightweight `VibeAudioAlive` event on the first packet after a revive.
2. In `_forceResubscribe()` and in `_scheduleReconnect()`'s timer body (when `appActiveRef` is false / a new `backgroundMode` flag on the client is set): before `_openSpectrumWs()`, check audio liveness. If dead → trigger `revive()` and wait for alive + **1s** (matching `connect()`'s registration delay), then open. If audio never confirms within 5s, open anyway (fallback).
3. Wire the flag: SDRScreen already flips `appActiveRef` and calls `watchProvider.setSpecPaused`; add `client.current?.setBackgroundMode(bool)` in the same AppState handler branches (~2436–2443 and the `active` branch) plus the cold-start-into-background init (the `AppState.currentState === 'active'` ref at ~688 — a watch cold launch fires no change event, so set it at client construction from the same check).
4. Applies to `serverType === 'ubersdr'` only. OWRX/Kiwi/FM-DX adapters are out of scope for this brief (their spectrum paths differ; see §A7).

### Phase 4 — Watch-side escalation (belt and braces, small)

In `watchProvider.ts`: the `computeWhy` path already knows when rows have stopped (`lastRowAt`). If `why` would be `'idle'` for **>15s** while the phone-side client still reports connected (audio alive), invoke the same force-resubscribe once (rate-limited, shared `lastForceReopenAt`). This covers any detection gap and makes the watch's own staleness knowledge actionable instead of merely displayed.

Do **not** change the watch app (`WatchLink.swift`) — its heartbeat/revive logic is healthy and out of scope.

---

## A4. Constraints (Part A)

- **Background-safe JS only** on every new phone-side path: plain timers + the existing native bridge. No Skia, no Reanimated, no per-frame React state (v6 regression lesson — see `watchProvider.ts` header).
- **Do not regress the foreground paths**: `pauseSpectrum`/`resumeSpectrum` semantics, the reinit watchdog, and the `specFailed` escape must behave identically. The new watchdog must be inert while `pausedByApp` is true.
- **Rate-limit every reopen** (15s) — a flapping cellular path must not thrash the server with session churn. The server counts connections.
- The starvation threshold must respect **legitimate low-rate feeds**: idle saver (`IDLE_DIVISOR = 3`, SDRScreen ~2545) and `setRate` divisors slow frames deliberately. Derive the threshold from expected cadence, not a bare constant.
- Same-uuid reopens throughout — decoders and the spectrum WS are keyed to it server-side (`reviveIfDead` comment, VibePowerModule ~545).
- GPL-3.0 + `APPSTORE-EXCEPTION.md`: all new code original, Stuart Carr copyright. No third-party additions needed.

## A5. Acceptance criteria / test matrix (Part A)

Primary device iPhone 17 Pro Max + watch; reference server: UberSDR instance (and the Pi 5 SpyServer target is out of scope here — this is the UberSDR client path).

1. **Cellular NAT rebind (the reported bug):** phone locked, watch showing waterfall, on cellular. Toggle airplane mode for 5s, re-enable. Waterfall must resume on the watch within ~15s of connectivity returning, with no foregrounding. `why` transitions `idle` → `live`.
2. **WiFi→cellular handover:** stream on WiFi with phone locked, walk out of range (or disable WiFi). Waterfall resumes on cellular within ~15s (Phase 2 fast path should make it near-instant).
3. **Server session reap:** kill connectivity for >60s (long enough for the server to reap), restore. Audio revives via native watchdog; spectrum must follow (audio-first ordering), not race ahead into a dead session. If the session cannot be re-attached at all, see open question §A6.
4. **Foreground unchanged:** lock/unlock cycles show the calm "reinitialising" notice and recover exactly as today; no double-reopens (watchdog must not fire during the resume window — `pausedByApp` covers the paused case; the 15s rate limit covers the rest).
5. **Idle saver false-positive check:** leave the app foregrounded and untouched >30s (idle divisor active), then locked with watch at rate divisor — watchdog must NOT force-reopen a healthy slow feed. Verify with debug logging over several minutes.
6. **Flap resistance:** rapid airplane-mode toggling must produce at most one reopen per 15s window.
7. **Cold start from watch on cellular** (`watchTargetPending` path): boot phone via watch, then induce a stutter — recovery must work in the never-foregrounded state (this is why `backgroundMode` must be initialised from `AppState.currentState`, not from a change event).

Add `dbg()` lines on every watchdog decision (detection reason: frames-stale / pong-timeout / path-change; action taken; suppression reason) — they route through `onDbg` and cost nothing in release.

## A6. Open questions (for Nathan / server-side, do not block implementation)

1. Does attaching `/ws/audio?user_session_id=<uuid>` **re-register** a reaped session server-side, or is a fresh `POST /connection` required? If the latter, `_forceResubscribe()` needs an escalation tier: after two failed reopens (socket opens, zero frames within 10s), run `_checkConnection()` before the next attempt. Implement this escalation defensively regardless — it's cheap and makes the answer moot.
2. Should `/ws/user-spectrum` close the socket when the session is unknown instead of accepting silently? A server-side `close(4001)` would make Hole 2 self-announcing for every client. Worth raising on Discord; not required for this fix.

## A7. Out of scope (Part A)

- OWRX / Kiwi / FM-DX adapter recovery paths (different pause semantics; separate brief if the symptom reproduces there).
- SpyServer work (`BRIEF-spyserver-and-network-performance.md`).
- Watch app (`ios/VibeSDRWatch/*`) — the WatchConnectivity-layer recovery is healthy and must not be touched by Part A. (Part B below does add to the watch app; keep the changes cleanly separated.)

---

# PART B — Watch Volume Control (iPhone system volume from the wrist)

## B1. Context — why yesterday's attempt failed

The previous attempt drove an **app-level gain** (AVAudioEngine/playerNode 0–1 scalar). Delivered loudness is `appGain × systemVolume` — two independent knobs, and the watch could only see and turn one. With the iPhone's system volume at 50%, the watch meter read "full" while delivering half loudness. The missing piece is readback and control of the **system** volume, not the app gain.

**Design rule for this part:** the watch controls exactly ONE knob — the iPhone system volume. Retire app gain from the watch path entirely (pin it at 1.0 for the watch, or leave it solely to phone-side UI if one exists). One knob, honestly reported.

## B2. Phone side — `VibePowerModule.swift`

### B2a. Readback (the piece that was missing)

- Observe `AVAudioSession.sharedInstance().outputVolume` via KVO. The session is already active whenever the engine runs, so observation is reliable; register the observer in `startAudioEngine` (or equivalent init) and remove it on teardown.
- On change, forward the new 0–1 value to the watch pipe (see §B3). This captures ALL volume changes — hardware buttons, Control Centre, Bluetooth headphone controls — not just watch-initiated ones, so the wrist meter always shows the truth.

### B2b. Control (system volume setter)

- iOS has no direct public setter. Use the sanctioned `MPVolumeView` technique: instantiate a hidden `MPVolumeView` (frame offscreen / alpha 0 / clipped container), add it to the key window's hierarchy **once at engine start** — not on demand — because the slider is not settable until ~100ms after insertion. Find its `UISlider` subview and set `.value` on the main thread.
- Setting via the slider does **not** trigger the system volume HUD.
- Bluetooth headphones: the slider maps through to absolute volume on the connected route — correct behaviour for the non-AirPods headphone case.
- Expose `@objc func setSystemVolume(_ v: Float)` (clamped 0–1) on the module for JS to call.
- iPhone volume is quantised to 1/16 steps; after setting, the KVO observer fires with the *snapped* value — that snapped truth is what gets echoed to the watch (this is the trailing-edge echo the wrist adopts).

## B3. Pipe — `watchProvider.ts` + `VibeWatchModule.swift`

- Add volume to the throttled `state` echo (new key `WK.vol = "vo"`, Double 0–1) — ride the existing `STATE_MS = 250` cadence, no new message type, no new channel load. Also include it in `flushAll()` so a (re)connecting watch gets the current volume immediately.
- New watch→phone command: `["cmd": "vol", "delta": Int]` — **deltas, never absolutes** (same direction-of-truth rule as tuning). Phone maps a detent to one 1/16 system-volume step: `setSystemVolume(current + delta/16)`.
- Route the command in `watchProvider.attach`'s switch (alongside `'tune'`/`'zoom'`) to a new handler that calls the native setter. It must work with **no SDR screen mounted** if feasible (the link belongs to the app — see the `detach()` comment ~397); if handler wiring makes that awkward, screen-mounted-only is acceptable for v1 since audio implies a session.

## B4. Watch side — `WatchLink.swift` + UI

Carbon-copy the crown-tuning architecture — every piece of it exists and has been debugged:

- **Coalesce** detents through the existing `pendingTune`-style accumulator (`pendingVol += delta`, flushed by `scheduleFlush`/`flushCrown` — DispatchQueue, not Timer, for the tracking-mode reason documented at ~339).
- **Predict while turning, adopt when still:** mirror `predictTune`/`armTuneSettle`. Prediction step = 1/16 per detent, clamped 0–1. On settle, adopt the phone's echoed `WK.vol` — it carries the 1/16-snapped truth, which may differ slightly from the prediction (same reason the tune settle waits for the clamped/snapped echo).
- `@Published var volume = 0.0` on `WatchLink`, updated from the `state` case in `apply()` — gated by the settle flag exactly as `frequency` is (`!tuning` equivalent: `!volAdjusting`).
- **UI:** a volume mode for the crown (e.g. via `ControlMenu`) or a dedicated control — implementer's choice, but it must show the mirrored system volume as the meter, and a mute toggle is worth including (`["cmd": "vol", "delta": -16]` is a poor mute; add `["cmd": "mute", "val": Bool]` mapped to the existing phone-side mute so unmute restores the prior level).

## B5. Constraints (Part B)

- No new WCSession message types at high rate — volume echoes ride the existing throttled `state` message; commands are coalesced. The channel budget lessons (8 msg/sec ceiling) are hard limits.
- The `MPVolumeView` must be in the hierarchy before first use and kept there; do not add/remove per adjustment.
- Do not touch app gain from the watch path.
- KVO observer lifecycle: register once, remove on teardown, guard against double-registration across engine restarts (`reviveIfDead` restarts the engine — observer must survive or re-register).

## B6. Acceptance criteria (Part B)

1. Phone at 50% system volume → watch meter shows 50% (not full).
2. Crank the crown to max on the watch → phone system volume reaches 100%; loudness genuinely doubles from the 50% start (the yesterday-bug regression test).
3. Press the phone's hardware volume buttons → watch meter follows within ~250ms.
4. Non-AirPods Bluetooth headphones on the phone: watch volume control adjusts headphone loudness; audio never routes to the watch.
5. Fast crown spin: no WCSession wedge (verify row feed stays live during and after), readout tracks the crown without backwards yank, settles to the phone's snapped value.
6. Phone locked in pocket: volume control still works (background path).
7. Mute from watch, unmute → prior level restored.

## B7. Out of scope (Part B)

- Audio playback on the watch itself — evaluated and rejected: WCSession cannot stream (16 msg/s wedges it), watchOS background audio requires watch-paired headphones, and the routing problem with phone-paired non-AirPods headphones has no API solution. The watch remains display + control; ALL audio is handled by the iPhone.
- App-gain UI of any kind on the watch.

---

# PART C — Watch Link Diagnostics (two-tier warnings, hop-specific)

## C1. Context and design intent

The watch already has the right two-tier structure (`ContentView.swift`):

- **Warning pill** (`linkWarning`, ~626): orange capsule over a still-rendering waterfall, currently a single generic "LINK ROUGH · SPECTRUM ERRATIC" driven only by a local row gap >1.2s.
- **Hard overlay** (`stalledMessage`, ~645): black centre message for genuine failure, already diagnosis-carrying via `why` / `phoneStatus` / state-freshness.

What it cannot do is tell the user **which hop** is rough, because the phone's own server-link quality never reaches the wrist. There are two links in series — iPhone↔server (WebSocket over WiFi/cellular) and watch↔iPhone (WCSession over BT/WiFi) — and they fail independently. Product intent, verbatim:

1. **Degraded but working** (jerky/laggy/stuttering spectrum) → pill only, waterfall stays fully visible, wording names the hop.
2. **Total failure** → the existing hard overlay, unchanged in style.
3. With Part A landed, pills should be fleeting — but they must exist so a stutter reads as "network conditions, being handled" rather than "garbage app".

## C2. New signal — phone's server-link quality crosses the watch link

- `UberSDRClient._evalLink()` (~667) already computes `q ∈ {0,1,2,3}` (down / poor / fair / good) and reports it via `onLink`. SDRScreen already consumes `onLink` — tee it into `watchProvider` (new `setLinkQuality(q)`).
- `watchProvider`: include it in the throttled `state` echo — new key `WK.link = "lk"` (Int 0–3) — and in `flushAll()`. Rides the existing 250ms cadence; no new message type.
- Additionally, while Part A's `_forceResubscribe()` is in flight, `computeWhy()` must return a new value `'reconnecting'` (between socket teardown and first frame after reopen). This is a fact the phone knows and the watch cannot infer.
- `WatchLink.swift`: `@Published var serverLink = 3`, updated in the `"state"` case of `apply()`; `why` already flows.

## C3. Watch-side logic — replace the single generic pill

Rewrite `linkWarning` in `ContentView.swift` to diagnose the hop. Inputs: local row gap (`lastRowAt`), state freshness (`lastStateAt` < 8s ⇒ the WCSession hop is alive), phone-reported `serverLink`, and `why`.

Priority order (first match wins; `stalledMessage != nil` still suppresses all pills):

1. **`why == "reconnecting"`** (state fresh) → pill: *reconnecting-to-server* diagram — shown even if rows have stopped, for up to the hard-overlay threshold (C4). This is Part A doing its job; do not black-screen a recovery in progress.
2. **State fresh AND `serverLink <= 1`** → pill: *server-hop-rough* diagram — shown **even when rows are still arriving** (this is the erratic-but-working case; the current gap-only trigger misses it entirely).
3. **Row gap > 1.2s AND state fresh AND `serverLink >= 2`** → the server side is fine, so the gap is the wrist hop → pill: *wrist-hop-weak* diagram.
4. **Row gap > 1.2s AND state stale** → cannot yet distinguish (whole WCSession pipe suspect) → show the *indeterminate* glyph until the hard overlay takes over.

**Tuning survives most of these, and the UI must say so.** Crown commands travel watch → WCSession → phone → audio WS; the audio WS has its own native watchdog (Part A §A2), so in cases 1–3 tuning (and phone audio) almost always still works even while the spectrum is degraded — only case 4 (WCSession itself suspect) puts tuning in doubt. Half the app still working must not read as the whole app broken. Pills are too small for a second clause, so convey it structurally: cases 1–3 use spectrum-scoped wording only (never "connection lost" or other whole-app language), and the pill's continued presence over a live-tuning waterfall is itself the message. The explicit advice goes on the hard overlays (C4).

**Pill design — GLYPHS, not text.** Each pill is a miniature diagram of the two-hop chain with the troubled link marked, built from SF Symbols in an HStack inside the existing orange capsule (same size/position/opacity). Reads at a glance, fits any watch size, and needs no localisation:

| Case | Pill content (left → right) |
|---|---|
| 1 — reconnecting to server | `arrow.triangle.2.circlepath` + `server.rack` (the circular-arrows glyph IS the universal "reconnecting" sign; optionally animate its rotation) |
| 2 — server hop rough | `server.rack` · `wifi.exclamationmark` · `iphone` |
| 3 — wrist hop weak | `iphone` · `wifi.exclamationmark` · `applewatch` |
| 4 — indeterminate | `wifi.exclamationmark` alone (nothing more is honestly known) |

Implementation notes:
- Glyphs at ~11pt, `.white`, spacing 3–4pt; the exclamation link glyph may pulse opacity gently (0.5–1.0, ~1s) to read as "live problem" — reuse an existing animation pattern, nothing per-frame/expensive.
- `wifi.exclamationmark` is the marked-link glyph for both hops even though hop 3 is BT/WCSession — it reads as "wireless link, troubled", which is the truth that matters. If the implementer prefers, `antenna.radiowaves.left.and.right` with a `.slash` variant or `exclamationmark` overlay is acceptable; consistency between rows 2 and 3 matters more than the exact glyph.
- Direction convention: the FURTHER device is always on the left, the wrist end on the right (server→iPhone→watch), so the two pills are visually parallel and the user learns the grammar once.
- Accessibility: give the HStack an `.accessibilityLabel` with the full sentence (localisable string) — VoiceOver users get the words the sighted user no longer needs.
- Keep the previous text strings in a comment beside each case as the canonical meaning, so future maintenance knows exactly what each diagram asserts.

Debounce/hysteresis: require the trigger condition to hold for ~0.7s before showing, and hold the pill for ≥2s once shown (per-cause), so a single late frame doesn't strobe it. The pill is driven from the frame clock like today — no new timer.

## C4. Hard overlay adjustments (minimal)

`stalledMessage` logic stays as-is, with two additions:

- **`why == "reconnecting"`** escalates to a hard overlay only if it persists **>20s** (Part A's recovery budget is ~15s): `("arrow.triangle.2.circlepath", "Reconnecting to server…\nTuning still works")`. Beyond ~45s fall through to the existing `"idle"` treatment.
- **`why == "idle"`** message: reword to name the hop AND preserve the working half — `"iPhone lost the server\nTuning may still work"` — since "iPhone isn't receiving" reads as a watch-side fault, and a user staring at a black overlay has no reason to try the crown unless told. (These `why`-driven overlays imply fresh state messages, i.e. the WCSession command path is alive — the "tuning works" claim is safe there. The `"Watch link lost"` and `"iPhone not responding"` overlays must NOT gain this line: in those cases the command path itself is the casualty.)

Everything else (`paused`, `phoneStatus` boot states, watch-link-lost split, placeholder) is already correct and must not change.

## C5. Constraints (Part C)

- Pill must NEVER obscure the waterfall beyond the existing top-strip capsule; total failure only gets the centre overlay (product requirement).
- No new WCSession traffic: `WK.link` and `why='reconnecting'` ride existing messages.
- All thresholds tunable constants at the top of the relevant file, commented with their rationale (house style).
- Do not regress the cold-boot placeholder flow — a boot is not an error (existing comment ~700 applies).

## C6. Acceptance criteria (Part C)

1. **Server hop erratic, rows flowing:** throttle the phone's connection (Network Link Conditioner, "Very Bad Network") → the server-hop pill (server · ⚠link · iPhone) appears over a live, jerky waterfall; clears within a few seconds of conditions improving.
2. **Wrist hop erratic:** walk the watch to the edge of BT range with the phone stationary on good WiFi → the wrist-hop pill (iPhone · ⚠link · watch), waterfall keeps drawing what arrives; the server glyph must not appear.
3. **Part A recovery in progress:** airplane-mode toggle on cellular (Part A test 1) → pill shows the reconnecting diagram (circular arrows + server) during the resubscribe, waterfall's last frames stay visible, pill clears when rows resume. Hard overlay only if recovery exceeds 20s.
4. **Total failure:** kill the phone app → existing hard overlay path unchanged.
5. **No strobing:** marginal link conditions must not flicker the pill (hysteresis holds).
6. **Tuning through degradation:** during tests 1 and 3, turn the crown — tuning must work, and no message on screen may claim or imply otherwise; the `idle`/`reconnecting` hard overlays must show the "tuning" line, and the watch-link-lost overlay must not.
7. Smallest watch layout: pill fits without clipping; glyphs legible at 40mm.
8. **Glyph comprehension sanity check:** each pill's meaning must be inferable without text (show the three diagrams to someone unfamiliar — the marked-link grammar should carry it); VoiceOver reads the full sentence via the accessibility label.

## C7. Out of scope (Part C)

- Any phone-screen UI changes (the phone already has link bars and the reinit/connLost notices).
- Haptics or complications for link state.
