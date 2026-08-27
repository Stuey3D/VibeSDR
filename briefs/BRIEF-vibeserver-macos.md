# BRIEF: VibeServer for macOS — Menu-Bar Server App (Apple Silicon)

**Project:** VibeSDR / VibeServer
**Author:** Stuart Carr (Stuey3D)
**Depends on:** `BRIEF-vibeserver-protocol-foundations.md` (versioning, multi-client, control token, hardware policy) — implement that FIRST; this app ships proto 2 from day one.
**Primary dev/test machine:** M4 MacBook Air (also a target device). Apple Silicon ONLY — VibeDSP is NEON; no Intel/Rosetta build.
**Role in the roadmap:** the Mac app is the shim's native test harness AND a product. The C++ core debugged here in Xcode/Instruments is the same core the headless Pi daemon recompiles — Mac first, Pi inherits.

---

## 1. Product shape

A signed, notarised, direct-download macOS app (menu-bar resident) that turns any Apple Silicon Mac with an RTL-SDR into a VibeServer:

1. Launch app → GUI config window → plug in dongle → Start.
2. Close the window → the server lives in the **menu bar**, quietly serving.
3. Optional **start at login**, so a Mac Mini in a cupboard is a permanent receiver.
4. Config reachable three ways (§5): the GUI, the admin-gated web config page, and the JSON file over SSH.

On an M4-class machine the whole workload (one capture, ≤5 demod chains, one FFT) is negligible — the app should be proud of that: show live CPU% in the status view.

## 2. Core port (C++ shim → macOS)

- `local_sdr_shim.cpp` + VibeDSP compile natively for arm64 — NEON intrinsics are identical on Apple Silicon. Build as a static lib linked into the app; no behavioural changes to the shim beyond what the foundations brief specifies.
- **USB:** librtlsdr via libusb, vendored/statically linked (no Homebrew dependency for end users). macOS needs no driver blacklisting — libusb claims the dongle directly. Handle hot-plug via libusb hotplug callbacks: dongle unplug → server pauses with a clear menu-bar state, replug → auto-resume.
- The Android-only guard in `vibeServer.ts` (`vibeServerSupported`) is untouched — this is a separate native app, not a React Native target. Reuse the shim's contract verbatim: same auth endpoints, same WS paths, same wire format, same web client bundle served at `GET /`.
- **mDNS:** native `NWListener`/NetService Bonjour advertising — replace the Android advertise path with the platform one, same service type/TXT records so existing client discovery works unchanged.

## 3. macOS lifecycle (the platform-specific engineering)

- **Menu bar:** SwiftUI `MenuBarExtra`. Status view shows: running/paused, client count (n/max), controller/parked state, per-second spec+audio throughput (all already in `VibeServerStatus`), CPU%, server address, and Stop/Start. Icon reflects state (idle / serving / clients connected / dongle missing).
- **App Nap:** while serving, hold a `ProcessInfo.processInfo.beginActivity(.userInitiated, ...)` assertion so macOS never naps the process. Released when stopped.
- **Sleep prevention — explicit user toggle** ("Keep Mac awake while serving"): IOPMAssertion (`PreventUserIdleSystemSleep`) held while the server runs. The toggle MUST carry a plain warning: *"Your Mac will not sleep while VibeServer is running and will use more power."* Default ON for a server app, but the user decides. Display sleep is NOT prevented — screen off is fine, system sleep is the enemy.
- **Clamshell note (docs + a small in-app hint, not a blocker):** a closed-lid MacBook only stays awake under specific conditions (power connected, and clamshell rules vary) — lid-open laptops, desktops (Mac Mini has no such limitation and is the ideal always-on host), or external-display setups are the supported stances. One sentence in the status view when running on battery with the lid-closed risk: "On a MacBook, keep the lid open or stay on power."
- **Start at login:** `SMAppService.mainApp.register()` behind a checkbox. With `autoRestore` semantics from the existing config: if the server was running at quit/shutdown, it resumes serving on login without showing the window.
- **Network permission:** macOS local-network privacy prompt fires on first Bonjour/listen — preflight it with a friendly explanation screen so the system dialog isn't the first thing the user sees.

## 4. GUI (primary config surface)

One config window (SwiftUI), sections mirroring `VibeServerConfig` + the foundations additions:

- Identity: server name, port.
- Access: PIN (or open), admin password (§6 of foundations; UI enforces PIN ≠ admin password).
- Capacity: `maxClients` stepper (1–5), `maxSampleRate` ceiling, `lockedRate` pin (validation: pin ≤ ceiling), FPS cap.
- Hardware policy: per-control open/admin/locked pickers (gain, bias-T, direct sampling), gain min/max range slider, idle-defaults editor ("Restore these settings when everyone leaves" — prefilled from current state with a "capture current as defaults" button).
- Behaviour: `controlGraceSec`, web client on/off, web config page on/off, advertise (Bonjour) on/off, start at login, keep-awake toggle (with the power warning), auto-restore.
- Live status pane while running (same data as the menu bar, expanded).

GUI writes the config file (§5) and signals the shim to apply — the GUI is an EDITOR of the schema, not a private store.

## 5. One config schema, three editors

**The architectural decision this brief exists to lock in:** server configuration is a single portable JSON document — the `VibeServerConfig` shape extended per the foundations brief — stored at `~/Library/Application Support/VibeServer/config.json`, human-readable and hand-editable.

1. **GUI** (§4) — reads/writes the file, applies live.
2. **Web config page** — served by the shim (e.g. `/vibeserver/config`), **admin-auth-gated** (nonce/HMAC with the admin secret, same machinery as the foundations brief; hidden entirely if no admin password is set). Same fields as the GUI. This page is written ONCE here and inherited verbatim by the Pi daemon — it is the Pi's primary config UI, so build it as part of the shim/web-client bundle, not as Mac-specific code.
3. **SSH / file edit** — the file is the truth; the shim watches it (or offers a reload trigger) and applies changes atomically with validation (invalid file → keep running on last-good config + log, never crash-loop on a typo).

Precedence and concurrency: last write wins; the GUI and web page both re-read before write and refuse to clobber unseen changes (simple mtime check + "config changed elsewhere, reload?" prompt).

The Pi daemon (future brief) then needs only: this same shim, this same web config page, this same JSON file, systemd instead of MenuBarExtra. That is the entire point of doing the Mac app first.

## 6. Distribution & licensing

- **Direct download** (GitHub Releases) with Developer ID signing + notarisation; optionally a Homebrew cask later. NOT the Mac App Store — GPL-3.0 and MAS terms conflict, and unlike iOS there is no store requirement on macOS, so `APPSTORE-EXCEPTION.md` is NOT needed for this app (note this in the repo docs to avoid confusion; the exception file's scope statement should mention the Mac app is distributed outside any store).
- GPL-3.0, Stuart Carr copyright, consistent with the repo. Vendored librtlsdr/libusb licences (GPL-2.0-or-later / LGPL) documented in an acknowledgements pane.
- Universal-binary question answered in advance: arm64 only, stated on the download page ("Apple Silicon Macs, macOS 14+" or as toolchain dictates).

## 7. Out of scope

- The Pi/headless daemon (separate brief; inherits §5's web config page and file schema).
- Multi-receiver support (foundations §3.5 reservation; single dongle in v1).
- SDRplay or other front ends — RTL-SDR only in v1.
- Serving from iOS/iPadOS; any Mac App Store submission.
- Background decode suites (FT8 etc.) — network-vision scope, not this app.

## 8. Acceptance criteria

1. **Cross-client parity:** iPhone app, Android app, watch (when JR exists), and the bundled web client all connect to the Mac server exactly as they do to the Android VibeServer — same auth, same audio, same waterfall, same roster/token behaviour (foundations tests 1–13 re-run against the Mac host and pass).
2. **Menu-bar lifecycle:** start from GUI → close window → server keeps serving from the menu bar; Stop/Start from the menu works; quit cleanly closes client sessions.
3. **Nap/sleep:** with keep-awake ON, the Mac does not sleep during a 2-hour idle-keyboard serve; audio to a client never glitches from App Nap. With keep-awake OFF, the warning was shown and system sleep behaves normally.
4. **Start at login + auto-restore:** reboot the Mac → server is advertising and serving before any user interaction, window hidden.
5. **Dongle hot-plug:** unplug mid-serve → clients get a clean pause state (not a crash or zombie), menu-bar icon shows dongle-missing; replug → serving resumes, clients recover via their normal reconnect paths.
6. **Three-editor coherence:** change a value in the GUI, another via the web config page (admin-gated), another by editing the JSON over SSH — each is reflected in the other two, the mtime-conflict prompt fires when appropriate, and a deliberately corrupted JSON file leaves the server running on last-good config with a logged error.
7. **Load sanity:** 5 clients on 5 VFOs from the M4 Air — CPU% shown in the status view stays in single digits; no thermal ramp over 30 minutes.
8. **Notarisation:** a fresh Mac with default Gatekeeper settings opens the downloaded app without right-click workarounds.
