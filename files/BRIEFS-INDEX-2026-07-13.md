# VibeSDR — Brief Index, 13 July 2026

All briefs produced across today's two planning sessions, in recommended implementation order with dependencies. Analysis baseline: `experimental` @ `e2ecc40` (VibeServer surface verified identical to shipped `v8.0.0`).

## From this session (files alongside this index)

| # | Brief | Scope | Depends on |
|---|---|---|---|
| 1 | `BRIEF-watch-fixes.md` | **Part A:** spectrum background recovery — starvation watchdog + pong-timeout, `NWPathMonitor`, audio-first resubscribe (fixes the cellular waterfall freeze). **Part C:** glyph-based hop-diagnostic pills on the watch (server·⚠·iPhone / iPhone·⚠·watch), tuning-still-works messaging. **Part B:** iPhone SYSTEM volume readback (KVO `outputVolume`) + control (hidden `MPVolumeView`) from the crown. Implement A → C → B. | — (first; it's the bug fix) |
| 2 | `BRIEF-vibeserver-protocol-foundations.md` | Proto versioning (`/vibeserver/info`, WS hello, close 4426, forward-compat rules), up to 5 clients with per-user VFOs on one capture (Kiwi model), centre-control token with 60s parked-grace + session-resume, admin role (second HMAC secret), hardware policy (open/admin/locked per control, gain range clamp, idle restore to launch defaults), server-side enforcement, FFT window/VFO clamps. Reserved: `registry`, `multi-sdr` + `rx` hello field. | — (keystone for 3, 4, Pi daemon) |
| 3 | `BRIEF-vibeserver-macos.md` | Menu-bar Mac server (Apple Silicon only): shim compiled native, librtlsdr/libusb vendored, hot-plug, App Nap assertion, keep-awake toggle WITH power warning, start-at-login, Bonjour. **One config schema, three editors** (GUI / admin-gated web config page / JSON over SSH) — the web config page is written here and inherited by the Pi daemon. Direct-download + notarisation (no MAS, no APPSTORE-EXCEPTION needed). | 2 |
| 4 | `BRIEF-vibesdr-jr-feasibility.md` | Standalone watch client. Mode arbitration ping race (Remote vs JR picker, "Always JR" override); port inventory (transport/Opus/ADPCM/recovery); audio routing for TWO device classes (Series 10+/Ultra 3 = background SPEAKER audio works, ~10 min audio ≈ 1 hr battery, surfaced in UI; older = `.longFormAudio`/headphones for wrist-down); backends v1 UberSDR + VibeServer (mDNS), fast-follow Kiwi + window-locked OWRX; saved servers for tunnelled hosts; battery icon next to the clock (both modes, red ≤20%, mitigation prompt ≤10%); shared Swift package + separate standalone target for the 99p watch-App-Store listing. Phases J1–J5. | 1 (patterns), 2 (proto-2 client contract) |

## From this morning's session ("Advanced SSB scanner for V10" chat — files live there)

| Brief | Scope |
|---|---|
| `BRIEF-scanner.md` | v10 scanner: Band SNR sweep (band-plan picker), Bookmark scan, Manual SSB QSO hopper (headline differentiator); three-technique detection engine auto-selected per tag; hang time; OWRX out-of-panSpan safety invariant; priority stars; found-signal log; record-on-stop via existing AAC pipeline; starter presets; VoiceOver from day one. M1/M2/M3 = build order within one release. |
| `BRIEF-instance-picker-grouping.md` | Grouped directory: collapsible distance/country/SNR cards, locale-derived country (zero-permission Hermes `Intl` fallback), exported aggregation service function (reused conceptually by JR's watch directory, §5 of brief 4), map view cloning `MapOverlay.tsx`. |
| Copy pack (same chat) | README WebSDR + digital-modes notes; About overlay Limitations (WebSDR / codec licensing / FM-DX shared tuner); FM-DX "SHARED RECEIVER" Now Playing artwork stamp with `refreshArtwork` anchors. |

## Suggested overall order

1. **watch-fixes** (bug fix; primary use case broken on cellular)
2. **scanner** + **instance-picker-grouping** (v10 features, independent of the server work)
3. **vibeserver-protocol-foundations** (keystone — everything server-side inherits it)
4. **vibeserver-macos** (doubles as the shim test harness for 3; precedes the Pi daemon)
5. **vibesdr-jr** phases J1–J5 (J2 requires 3's server-side landing)

## Not yet written (agreed roadmap, briefs to follow)

- Pi/headless VibeServer daemon (inherits macOS brief's config page + JSON schema; multi-receiver semantics incl. per-receiver tokens, idle-receiver auto-assign, allowed-range enforcement with FFT-bin masking of blocked spectrum, per-receiver hardware policy)
- Directory/registry + heartbeat (the `registry` cap reservation)
- Protocol version handshake is IN brief 2 — deliberately pulled forward so it ships before any third-party server exists.

73!
