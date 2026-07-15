# BRIEF: VibeSDR JR — Standalone Apple Watch SDR Client (Feasibility & Architecture)

**Project:** VibeSDR
**Author:** Stuart Carr (Stuey3D)
**Positioning:** "The world's smallest standalone SDR client." The existing watch app already decodes and renders the waterfall on-device; JR adds its own network transport and audio so the watch is a complete client with no iPhone in the loop.
**Depends on:** `BRIEF-watch-fixes.md` (ships first — JR reuses its recovery watchdog and glyph-diagnostic patterns), `BRIEF-vibeserver-protocol-foundations.md` (JR is a proto-2 client; the listener-role contract and hello/session machinery are defined there).
**Related:** `BRIEF-instance-picker-grouping.md` (grouping rules reused for the watch directory).

---

## 1. Two modes, one app

On launch the watch app resolves its mode:

- **VibeSDR Remote** (today's app): thin display/control head; iPhone owns the network and ALL audio. Subtitle: *"Opens VibeSDR on iPhone and controls it — audio plays on the iPhone. Uses less power."*
- **VibeSDR JR** (new): standalone client; the watch owns transport, decode, and audio. Subtitle: *"No iPhone — everything runs on the watch. Uses more power."*

### 1.1 Mode arbitration (the ping race)

1. On launch, if `WCSession` reports a paired companion (`isCompanionAppInstalled`), send a ping via `sendMessage` with a ~3s reply window — this WAKES a closed iPhone app (the existing `watchTargetPending` cold-boot flow proves the mechanism).
2. Reply within window → **Remote mode** (whether the phone was already open or just woke is indistinguishable and irrelevant).
3. Timeout / no companion / watch on LTE with phone absent → show the **mode picker** (two cards, subtitles above, Remote pre-selected when it might still work).
4. Settings override: "Always start in JR" for the phone-at-home-on-the-charger case — JR's raison d'être. A user in JR mode can switch to Remote from settings without relaunching (tears down JR session, runs the ping race).
5. If the phone app is detected as open, default straight to Remote with no picker (current behaviour preserved).

## 2. What actually ports (inventory)

| Piece | Source of truth today | Watch port |
|---|---|---|
| Session model (POST `/connection`, uuid, audio-first-then-spectrum ordering) | `UberSDRClient.ts` | Swift, `URLSessionWebSocketTask` (already used fluently at the WCSession layer) |
| Spectrum decode (binary8 → resample 256 → LUT) | `watchProvider.ts` (phone side) | Trivial arithmetic; moves onto the watch — the LUT/rows pipeline on the wrist is UNCHANGED, only the row source moves |
| Opus decode + playout | `VibePowerModule.swift` (libopus static + AVAudioEngine) | libopus compiles for watchOS; AVAudioEngine exists there. Real but known-shape work |
| IMA ADPCM decode | `src/services/imaAdpcm.ts` (shared with web client) | ~50 lines of Swift integer maths; simpler than Opus |
| VibeServer client (auth nonce/HMAC, hello, roles) | foundations brief + `web/client/src/auth.ts` | Swift port; CryptoKit HMAC-SHA256 |
| Recovery watchdog | `BRIEF-watch-fixes.md` Part A | SIMPLER here: one hop, no WCSession — frame-staleness + pong-timeout watchdog lives where the socket lives |
| Link diagnostics glyphs | watch-fixes Part C | Reused; the two-hop diagrams collapse to one hop (watch ⚠ server) |

**Not ported, ever:** client-side demod. JR is a thin network client by definition — the server sends finished audio. This is the admission test for backends (§4).

## 3. Audio routing — hardware-conditional, two device classes (verified July 2026)

watchOS audio routing changed materially with the Series 10 (Sept 2024) + watchOS 11, so JR must handle TWO device classes:

### 3a. Media-speaker-capable watches (Series 10 and later, Ultra 3; watchOS 11+)
These models support media playback through the built-in speaker, INCLUDING wrist-down/background (this is how Apple's own Podcasts app works on them). Requirements: `audio` background mode in the extension, `.playback` session (per Apple DTS, do NOT use the `.longFormAudio` policy for speaker output — though on watchOS 11+ even longForm sessions have been observed routing to the speaker without a picker). Also recommend the Settings → General → Return to Clock → "Return to App" note in onboarding docs so the app resurfaces on wrist-raise.
**The cost is brutal and must be surfaced:** Apple's own figure is ~10 minutes of speaker audio per HOUR of battery life, and speaker media playback is unavailable while charging. The battery icon (§8) shows a distinct "speaker mode" hint state, and the ≤10% mitigation prompt suggests headphones as well as the FPS drop.

### 3b. Older watches (Series 9 and earlier, SE, Ultra 1/2)
The original rules hold: speaker playback is FOREGROUND-ONLY; wrist-down/background audio requires the `.longFormAudio` policy, which routes exclusively to Bluetooth headphones paired to the WATCH (system route picker appears).

### 3c. Runtime behaviour (one state machine, capability-gated)
- Detect capability at session activation (route inspection after activating a background-capable playback session; maintain a small known-model allowlist as backstop).
- **Capable device:** audio simply continues wrist-down on whatever the active route is — speaker or headphones. Show the battery-cost hint when the route is the speaker.
- **Legacy device:** with headphones paired → `.longFormAudio`, survives wrist-down. Speaker → foreground-only, and wrist-down shows the glanceable "audio paused — raise wrist or pair headphones" state, NOT a mystery stall.
- Route selection: there is no `AVRoutePickerView` on watchOS — let the system handle destination via activation-time picker/Now Playing AirPlay control; do not build custom route UI.

JR's marketing line accordingly: full wrist-down speaker listening on Series 10+/Ultra 3; headphones for wrist-down on older models. Never promise background speaker audio universally.

## 4. Backends

**Admission test: does the server send finished (demodulated, compressed) audio?**

### v1 — the proving pair (deliberately bracketing both codecs and both discovery models)
- **UberSDR** — Opus + spectrum bins; the flagship; internet-range (WiFi/LTE); the session/recovery reference.
- **VibeServer** — IMA ADPCM + waterfall over one WS; LAN/routed; mDNS; proto-2 hello/roles/session-resume per the foundations brief. The watch honours the listener contract: VFO-free within the window, centre controls appear only when holding the token, clamp at window edges (the existing pan/clamp discipline), roster-driven "window set by another user" glyph state.

### Fast-follow (shared transport layer, shared ADPCM decoder)
- **KiwiSDR** — two-socket, ADPCM; HF.
- **OpenWebRX — window-locked guest mode:** tune-within-profile-window ONLY; profile switching absent from the client entirely (the etiquette problem solved by construction; same client contract as a VibeServer listener — "here is your window, roam inside it").

### Excluded, permanently
- RTL-TCP, SpyServer, anything raw-IQ/client-demod: fails the admission test and the battery. Not a restriction — a category boundary.
- FM-DX: revisit only if demand appears (MP3 via AVAudioConverter is possible but out of scope).

## 5. Discovery, onboarding, saved servers

- **No camera, minimal keyboard** — onboarding must not depend on QR or heavy typing.
- **mDNS:** `NWBrowser` for Bonjour; VibeServers on the local segment appear with name + occupancy (from `/vibeserver/info`: "3/5 · in use" before connecting). Zero typing.
- **Directory:** the UberSDR instance directory using the grouping rules from `BRIEF-instance-picker-grouping.md` — the collapsible-group model IS watchOS list navigation (grouping mode → group → receiver drill-down; land on the nearest bracket; distance mode uses the watch's own location). Reimplement the exported aggregation rules in Swift against the same directory JSON.
- **Saved servers:** first-class, synced from the phone when a companion exists (`transferUserInfo` of the favourites list) AND locally editable — host:port + PIN per entry, reconnect in one tap. This is the tunnelled-VibeServer path (mDNS doesn't cross tunnels).
- **Manual entry:** host/port via watch keyboard/dictation/scribble; PIN via digit pad. Clunky but once-only per server thanks to saved servers.

## 6. Connectivity truth (marketing + docs language)

**The watch only ever speaks plain WiFi/LTE.** watchOS has no VPN; all tunnel complexity lives at the server end or in routers. Three presentation-identical tiers:
1. **Local:** same LAN, mDNS auto-discovery.
2. **Travel router:** GL.iNet-class box + Android VibeServer + watch on its WiFi (2.4 GHz for broad watch compatibility; on-watch WiFi join needs no iPhone). The lunchbox station.
3. **Tunnelled server:** remote VibeServer WireGuards HOME; it appears local to everything on the home LAN including the watch. Saved-server entry, not mDNS.
Plus UberSDR/Kiwi/OWRX over ordinary internet on watch WiFi or LTE.

## 7. Link recovery

Part A of the watch-fixes brief, simplified to one hop: frame-staleness watchdog (cadence-aware threshold), pong-timeout fast path, `NWPathMonitor` on-watch for WiFi↔LTE transitions, audio-first resubscribe ordering for UberSDR, session-resume token for VibeServer (foundations §5.2 — a JR controller surviving a blip keeps the centre). Part C glyphs collapse to the single-hop diagram: `applewatch` · `wifi.exclamationmark` · `server.rack`.

## 8. Battery

- **Battery icon next to the clock** — REQUIRED, both modes (JR motivates it; Remote benefits too). `WKInterfaceDevice.current()` with `isBatteryMonitoringEnabled = true`; compact glyph + percent in the header row adjacent to the system clock; refresh on the existing state/UI cadence (no new timer). Colour: theme amber normally, red ≤20%. At ≤10% in JR, offer one-tap mitigation ("Drop to quarter FPS?" — plus "Switch to headphones?" when on the speaker). While the active route is the built-in speaker, show a subtle speaker-cost hint near the battery icon: Apple's own figure is ~10 min of speaker audio per hour of battery.
- Budget honesty for docs: continuous JR streaming (radio up, decode, screen, audio) is a heavy workload — expect a few hours, not a day. Mitigations already in the architecture: server-side FPS tiers (VibeServer `quarter` exists precisely for this), the amber-on-black OLED aesthetic, Opus/ADPCM low bitrates, and wrist-down pausing on speaker posture.
- Measure, don't guess: acceptance includes a drain benchmark (§11).

## 9. Project structure & the 99p standalone

- **Shared internal Swift package** (`VibeSDRJRCore` or similar): transport, session, decoders, recovery, directory/mDNS, saved servers, the JR UI surfaces.
- **Two thin targets:** (a) the existing embedded watch app gains JR mode (bundled with VibeSDR — the plan); (b) a **watch-only independent app** (separate app record + bundle ID, watchOS 6+ mechanism) for the standalone Apple Watch App Store listing at £0.99. You cannot sell an embedded watch app separately — the second target is mandatory for the listing.
- **Licensing:** GPL-3.0, Stuart Carr sole copyright; charging is explicitly permitted. `APPSTORE-EXCEPTION.md` gains the standalone bundle ID in its scope statement. The standalone target must be fully self-sufficient (no companion assumptions anywhere in its onboarding).

## 10. Sequencing

1. **Phase J1 — UberSDR JR** on the embedded target: transport + Opus + speaker/longForm audio + recovery + battery icon. Proves the concept end-to-end.
2. **Phase J2 — VibeServer JR:** ADPCM decoder, mDNS browse, proto-2 hello/roles/resume, PIN entry, saved servers. (Requires the foundations brief landed server-side.)
3. **Phase J3 — directory + mode picker polish:** grouped instance directory, arbitration edge cases, settings override.
4. **Phase J4 — standalone target + store listing.**
5. **Phase J5 (fast-follow) — Kiwi, then window-locked OWRX.**

## 11. Acceptance criteria

1. **No-iPhone proof:** iPhone powered off in a drawer; watch on home WiFi connects to UberSDR, waterfall + tuning + speaker audio work; same again to a LAN VibeServer via mDNS with PIN.
2. **Audio postures (both device classes):** on a media-speaker-capable watch (Series 10+/Ultra 3), speaker audio SURVIVES wrist-down and screen-off for 15+ minutes, with the speaker-cost hint visible; on a legacy watch, wrist-down on speaker → clean "audio paused" state with instant wrist-raise resume, and watch-paired AirPods → `.longFormAudio`, audio survives wrist-down 15+ minutes. Capability detection picks the right behaviour with no user configuration.
3. **Arbitration matrix:** phone app open → Remote, no picker. Phone closed but reachable → ping wakes it, Remote. Phone off/absent → picker within ~3.5s. "Always JR" override honoured. Mid-session switch JR→Remote works.
4. **VibeServer roles on the wrist:** as listener, VFO tunes freely and clamps at window edges; centre controls absent; roster glyph shows shared state; as controller, centre moves; crash-and-relaunch inside the grace window resumes controller via the session token.
5. **Recovery:** WiFi→LTE handover mid-stream recovers within ~15s with the single-hop glyph shown meanwhile (watchdog + NWPathMonitor working).
6. **Battery icon:** visible next to the clock in both modes, tracks system value, red ≤20%, FPS suggestion fires ≤10% in JR.
7. **Drain benchmark:** 30-minute continuous JR session (WFM on UberSDR, screen mostly on, speaker) on the primary test watch — record %/hour in the brief's results section; verify quarter-FPS tier measurably reduces it.
8. **Standalone target:** installs and onboards on a watch whose paired iPhone has never had VibeSDR; manual server entry and saved servers fully usable.
9. **Remote regression:** the entire existing Remote experience (watch-fixes brief acceptance suites) passes unchanged on the embedded target with JR code present.

## 12. Out of scope

- Watch-side demod of any kind; raw-IQ backends (permanent).
- Complications, Smart Stack widgets, background audio recording, VOX/scanner features on the watch.
- FM-DX backend; SpyServer anywhere near the watch.
- The Pi daemon and directory/registry service (separate roadmap items).
