# The Android server in its own process, the watchdog, and remote access to its settings (beta88)

Branch `b88-android` (off v12-experimental 9da8a5c0d), 2026-11-20. Main app (net.vibesdr.app) and VibeServer Lite.

## RESUME HERE

Built, unit-tested and compiled (see "What was tested"). **Not yet run on a phone.** Next:

1. Install the beta88 APK on the XCover (Stuart — never an agent: no adb to the XCover/Sony).
2. Run the phone checks in "What needs a real phone" in order; the first two prove item A; 4 and 9 prove the remote page.
3. Read `filesDir/server_trail.txt` (Diagnostics will show it once wired — not done, see Open) after any odd restart.

★ 2026-11-20 evening (branch `b88-remote-all`): **every** setting on the server screen is on the remote page — see
"Every setting remote" below for the inventory, the device-only list and the guard that keeps it that way.

## Why (the evidence)

XCover, 2026-11-20:

- 14:42:51 `am_kill … stop com.google.android.webview due to installPackageLI` — a Play update of System WebView stopped
  the whole app process, its embedded VibeServer included (also 10-28). Android stops every process that has *loaded*
  WebView; React Native's cookie handler (OkHttp's JavaNetCookieJar → ForwardingCookieHandler → CookieManager) loads it on
  the first `fetch`, so the app's process always had it. **Lite too** (React Native 0.73, same cookie handler).
- 15:18:10 full backup → SIGKILL of the app process (fixed separately: allowBackup=false, beb2b2f2d). 15:18:12
  `am_proc_start` for RtlTcpServerService (pid 3141) — and then nothing: no server, no log line, until the app was opened.

## Why onStartCommand "never ran / never logged" — what the code says

`onStartCommand` logged nothing on entry, and a restore that *succeeded* logged only at its end. Every failing path logged,
so the silence means one of: (a) `Application.onCreate` did not finish in pid 3141 (in the old layout it initialised React
Native, Expo modules and SoLoader in a background-restarted process); (b) onStartCommand ran, went foreground, and the
restore thread **blocked** before `applyAndStart` — candidates are the other-server probe (≤ 6 s), `openDevice`, an RSP or
Icom attach, the native library load; nothing after that point could have blocked silently for ever except those; (c) the
log capture filtered the tag. It cannot be settled from the code alone. What changed so it is settled next time and does
not matter meanwhile:

- `onCreate` and `onStartCommand` now log on entry with process, pid, kind of start and flags, and every step of a
  restore leaves a line in `filesDir/server_trail.txt` (last ~150 lines, survives the process).
- The server process no longer runs React Native's start-up at all (cause (a) is gone).
- A restore thread that is running is known (`restoreRunning`); the service retries a retryable failed restore once a
  minute (self-heal tick); the watchdog restarts an armed server that is not serving even when the service never started.

## The RAM trade-off (stated first, as required)

A second process costs its own Android runtime. **Estimate** (not measured — no phone was used): a process forked from
zygote that loads only plain Kotlin and the engine's library costs roughly **15–25 MB PSS** on a 32-bit 1 GB phone (zygote's
shared pages are split across processes; the private part is the Java heap, the class tables of our ~20 server classes,
and the binder/thread stacks). What does **not** double:

- the engine's own memory (capture buffers, DSP, decoders) **moves** to the server process; the UI process no longer holds
  it while serving;
- the native library's code pages are file-backed and shared between the two processes;
- the server process never loads React Native, Hermes, Expo modules or WebView (MainApplication returns before them) —
  React Native + Hermes + the JS bundle are the bulk of the UI's ~150–250 MB.

When nothing is being served, nothing keeps the server process: the UI binds to it only while asking something and lets
go after 60 s idle; a status poll for a server process that is not running is answered without binding. Android reclaims
an idle cached process first under memory pressure. **Net on a 1 GB phone:** ≈ +15–25 MB while serving with the app's
screens also open; **less** than before while serving with the screens closed (Android can drop the whole UI process,
which it could not before because the server lived in it).

To measure on the XCover (Stuart): `adb shell dumpsys meminfo net.vibesdr.app:server` while serving, screens closed, and
`dumpsys meminfo net.vibesdr.app` before/after — the TOTAL PSS lines.

## The process design (item A)

```
UI process  net.vibesdr.app                         server process  net.vibesdr.app:server
  React Native, Hermes, WebView                        no React Native / Hermes / WebView
  VibeLocalSdrModule ── VibeServerLink ──binder──►     VibeServerHost (bound, not exported, uid-checked)
    USB permission prompt, Icom mic dialog                 └─ VibeServerOps: start/stop/status/live setters/
    Local Hardware listening (its own engine copy)            rtl_tcp server/tunnel/radio attach/settings
  ◄── "settings" / "remote" events (one-way) ───────   RtlTcpServerService (FGS connectedDevice|microphone)
                                                         VibeServerRestore, VibeServerBoot, VibeTunnel, VibeGeoData,
                                                         VibeMapGL, VibeIcom/IcomAudio, VibeRsp, NSD, mDNS
                                                         receivers: update, boot, VibeWatchdogReceiver
```

- One request shape: `(method, JSON) → JSON` over raw `Binder.transact` (no AIDL step). Payloads over 200 KB (the
  station list) go through a file in `cacheDir/ipc` named in the transaction (binder's 1 MB limit).
- **Cross-process state:** SharedPreferences are read and written by one process only (the server's: armed flag, restore
  config, tunnel). "Start when power returns" moved from prefs to a one-byte file both processes read fresh
  (`vibe_start_on_power`; the old pref is carried over once). `MODE_MULTI_PROCESS` is not used.
- **Local Hardware** keeps working in the UI process; it now refuses while this phone's server holds the radio (before,
  starting it silently stopped the server and left its notification up).
- **Icom audio:** the dialog runs in the UI; its answer goes to the server (`icomMicGranted`). "Is the app in front" for the
  microphone foreground type is now asked of every process of the app (`VibeProcess.appVisible`) — the old
  getMyMemoryState only described the calling process, which in the server process is never visible.
- **NSD** registration moved to the server (it dies with the process that made it); the screen no longer registers it on
  Android, which would have renamed one of the two "(2)".
- Lite gets the same split (its manifest, MainApplication, SHARED list).

## The watchdog (item A, part 2)

`VibeServerWatchdog`: while the owner has a server armed, an inexact `setAndAllowWhileIdle` alarm every ~5 min (Doze
stretches it to ~9–15). On firing: armed and serving → re-arm; armed and not serving and no restore running → restore in
place if the service is alive, else `startForegroundService(EXTRA_RESTORE)`. Stop disarms and cancels it.

★ **Android 12+ background FGS rule (NickB G4IRX's history in onStartCommand):** an inexact alarm is not an exemption; the
exemption that applies is "battery optimisation off", which Start already asks for (`checkBackgroundAllowed`). Without it
the start throws `ForegroundServiceStartNotAllowedException` — **caught**, never a crash — and the tappable "VibeServer
stopped" notification is posted; the next check tries again. Exact alarms were rejected: SCHEDULE_EXACT_ALARM is denied by
default from Android 14 and USE_EXACT_ALARM is for alarm-clock/calendar apps under Play policy.

## Remote access to server settings (item B)

- **Switch:** "Remote access to server settings", under the admin password on the server screen. Default OFF. Refused
  without an admin password (the owner's typed one — nothing is generated). Kept in `filesDir/vibe_remote_setup`.
- **Off:** `/vibeserver/config` answers 501 as it always has on a phone; `/setup` is not served.
- **On:** the shim's existing route, gate and lockout serve it (local_sdr_shim.cpp — nonce + HMAC proof, `recordFail` only
  for real guesses, refused when no password is set; audit 2026-10-03). JNI handlers (`vibe_localsdr_jni.cpp`,
  `nativeEnable`) translate with `vibe_android_setup.h` and call Kotlin (`VibeRemoteSetup.configGetRaw/configSetPatch`).
  The admin password never goes to the page and can never be set from it.
- **One truth:** `filesDir/vibe_server_settings.json` in the server process = the config the server starts and restores
  from (the restore config follows every change). A Start writes it whole; the screen sends what its Start would send
  800 ms after edits; the page's save is a patch of the supported keys only; a page save asked to restart restarts the
  engine through the restore (one start path) and emits a `settings` event the screen redraws from.
- **The page on a phone:** `cfg.android.supported` lists the 28 controls the phone has; `androidPrune()` hides every other
  card and row (no dead controls); a read-only "This phone" card shows battery optimisation, start when power returns, the
  USB permission and keep radio alive, with where to change them. The Linux-only loaders (sdr-changes, Wi-Fi, Windows,
  serial, benchmark, directory status) are not called.
- **Supported controls:** every setting on the phone's server screen since 2026-11-20 — see the next section. (The first
  cut, the morning of 2026-11-20, carried 28.)

## Every setting remote (2026-11-20, branch b88-remote-all)

Stuart: *"all settings for android need to be on the remote setup page"*.

### How

- **One table** — `vibe_android_setup.h controlTable()`: each row is the page's control ids, the radios it belongs to and
  what the device must be. The radios follow ServerModeScreen exactly (`isRtl` = no radio or an RTL; `hasHwSetup` = not an
  HF+ and not an Icom; the SDRplay card only for an RSP; ppb only for an HF+; an Icom's own only for an Icom). Raw IQ is
  not on Lite, the battery pause not on a television, the public listing needs the tunnel binary (arm64), the DAB label
  scan is Lite's. `pageConfig()` sends the page `android.supported` for THIS phone and radio, and `android.advancedOnly`
  (the settings VibeServerBoot applies only in Advanced: listeners, ask-first, the locked range, tuning limits, gain rules,
  the AGC lock, trusted proxies). `settingsPatch()` now takes the phone's current raw too and **ignores** a control the
  radio is not offered, whatever the page posts.
- **The page** (`vibe_setup_page.h`): `androidPrune()` keeps a control by its id, or — for a generated input with no id
  (the modes tree, the background-decoding profiles, the per-band rows) — by its nearest ancestor's; for each control it
  does not keep it hides the largest thing around it that holds nothing kept (so the IF-filter select inside the shared
  per-band row goes, the row stays). It runs again when the radio's own rows arrive (`refreshHw`) and when Simple/Advanced
  changes — live, no reload. The page's own per-driver hiding (`renderHw`, caps) still applies on top.
- **Controls the Linux page never needed**, drawn only on a phone (`.androidOnly`): a "This phone's server" card (Serve
  the web client, Compress for the app, Simple/Advanced — no web client forces Simple, as on the screen), and on the radio
  tab the RTL's "Keep the tuner's IF filter selective" (`tunerBwAuto`), the fixed direct-sampling branch, and the
  converter's input low end. Posted only on a phone (`androidSrvCollect`, `androidRadioCollect` with `androidPage:true`, so
  the Linux page's always-0 converter low end can never wipe the phone's).
- **Kotlin** (`VibeRemoteSetup.kt`): device facts now say Lite, TV, tunnel, the listing's state and (for an Icom) the
  microphone permission. A page save of the place applies it in the server process at once (`setLocationJson` + the
  restore's cache; the screen re-publishes with the town and country when it next opens). The listing switch is an
  **action**, not a stored setting: only a flip acts (or a new name/length while listed — a republish), checked before
  anything is stored with the screen's own three refusals (a public name, a location, a running server), and after the
  restart when the save restarts (a restart re-lists by itself).
- **The screen** (`serverSettingsMirror.ts`): REMOTE_KEYS (66 plain keys) plus SPECIAL_KEYS (47, written in the screen's
  own shape — capture width, waterfall tier, ppm absent, direct sampling, converter in MHz, FM defaults / Icom / SDRplay
  card / antenna switch as one JSON value each, the location's three keys, the PIN). A value already equal is never written
  back. The start config now also carries `radioUse`, `serverLocation` and `publicName` (the store is replaced whole by
  every Start, so anything the page shows must be in it).
- ★ **Found by the new guard and fixed:** `rttySkimWorkers` and `antennaNames` were set on the screen and read by
  VibeServerBoot but never sent by `nativeServerConfig` — neither had ever reached the engine on a phone. Sent now.

### Inventory — every ServerModeScreen setting

| Setting on the server screen (storage key) | On the remote page | Radios / device |
|---|---|---|
| Local name (`vs_name`), mDNS (`vs_advertise`) | remote — `name`, `mdns` | all |
| Landing message, link, label | remote | all |
| Uncompressed audio (`vs_uncompressed`) | remote | all |
| Behind a reverse proxy (`vs_proxies`) | remote (Advanced) | all |
| One radio per address (`vs_oneradioperip`) | remote — `maxRadiosPerIp` (yes/no on a phone) | all |
| Decoders at once, RTTY skimmer workers | remote | all |
| Gallery size / public / keep listeners' | remote | all |
| Battery pause (`vs_batpause`; resume = pause + 20) | remote | not on a TV |
| Receiver location (`lsv_server_location`, `vs_locmode`, `vs_loc_moving`) | remote — place, lat/lon, country, follow | all |
| Advertise on vibesdr.net: public name, list, temporary length | remote — `dirName`, `dirList` (action), `dirShareSec` | tunnel builds |
| Serve the web client (`vs_webserver`) | remote — phone-only card | all |
| Compress for the app (`vs_compress`) | remote — phone-only card | all |
| Simple / Advanced (`vs_advanced`) | remote — phone-only card | all |
| SDR display name, antenna name | remote | all |
| PIN — who may connect (`vs_pin`, `vs_pinmode`) | remote — set / clear (a "random" PIN becomes a typed one) | all |
| How many listeners, ask before tuning | remote (Advanced) | all |
| How will it be used (`vs_radiouse`), locked centre | remote — single / locked, centre (Advanced) | not an Icom |
| Zoom detail (`vs_zoomspec`) | remote | not an Icom |
| Where listeners start (frequency, mode) | remote | not an Icom |
| Time limit, soft limit, idle kick | remote | all |
| Impulse noise blanker, VFO B | remote | not an Icom |
| When nobody is listening (`vs_idlegrace`) | remote | all |
| Spectrogram (`vs_spectrogram`) | remote (Advanced, locked) | not an Icom |
| Capture width (`vs_rate`), waterfall rate (`vs_fps`) | remote — `rate` + `rateLock`, `maxFps` (snapped to the screen's lists) | not an Icom |
| Modes offered (`vs_blockedmodes`) | remote — the modes tree | all |
| Rate boost for DAB etc. (`vs_dabboost`) | remote | not an Icom |
| ADS-B / ISM at 2.56 MS/s | remote | RTL |
| DAB label scan (`vs_dabscan`) | remote | Lite |
| Resting gain, VibeAGC, IF filter selective (`vs_restgain`, `vs_rtlagc`, `vs_tunerbwauto`) | remote | RTL |
| Lock VibeAGC on (`vs_agclock`) | remote (Advanced) | RTL |
| Per-band ceilings, locks, splits, curves | remote (Advanced) | not an Icom |
| Per-band IF filter (`vs_ifbwlimits`) | remote (Advanced) | RTL |
| Where listeners may tune (allow / block) | remote (Advanced) | all |
| DAB landing, ADS-B landing + keep running | remote | not an Icom |
| Broadcast FM defaults (`vs_fmdefaults`) | remote | not an Icom |
| Antenna icon, ranges, filters, port names | remote | all |
| External antenna switch (`vs_antswitch`) | remote (the page's switch card, shown as on Linux once one is set up) | all |
| RSP sockets by band + lock (in `vs_rsp`) | remote | RSP |
| Frequency correction (`vs_ppm`) | remote (0 on a radio left alone stays absent) | RTL, RSP |
| Calibration ppb (`vs_ppb`) | remote | HF+ |
| Bias-T (`vs_biast`) | remote (the page hides it on an RSP1) | RTL, R2/Mini, HackRF, RSP |
| Direct sampling: automatic, below, fixed branch | remote (fixed branch: phone-only row) | RTL |
| Converter: LO, direction, input high, input low | remote (input low: phone-only row) | not an HF+, not an Icom |
| Icom: baud, CI-V, scope speed, tune lock, power on, auto off, delay (`vs_icom`) | remote | Icom |
| Background decoding (`vs_bgdecode`), DX scanner (`vs_dxscan`) | remote | not an Icom |
| Raw IQ out (`vs_rawiq`, `vs_rawiqmax`, `vs_rawiqlanmaxhz`) | remote | not Lite, not an Icom |
| VibeAGC for SDRplay + the SDRplay card (`vs_rspvibeagc`, `vs_rsp`) | remote | RSP |
| Protocol rtl_tcp / VibeServer (`vs_proto`) | **device-only** — read-only row | main app |
| Keep radio alive (`vs_keepalive`) | **device-only** — read-only row | all |
| Admin password (`vs_adminpw`) | **never** on the page (security rule) | all |
| Start when power returns | **device-only** — read-only row | Android versions that allow it |
| Remote access to server settings | **device-only** — read-only row | all |

Not settings (no row): `vs_startonboot` (retired 2026-09-28), `vs_antports` and `vs_rspcaps` (caches of what the radio
reported). Actions, not settings, that stay on the phone's screen: Start / Stop, Scan all DAB blocks (the page's DAB
landing scans through `/vibeserver/dab-scan` itself), the benchmark run, the EiBi download, export logs, Lite's update
check, the PIN's ↻ (random) — a PIN set from the page is a typed one.

### Device-only — and why

Shown read-only on the page's "This phone" card, with where to change them:

- **Battery optimisation** — only Android's own dialog can exempt an app.
- **Start when power returns** — Android's boot permission and the one-byte file both processes read; a phone setting.
- **USB permission for the radio** — Android's own prompt, on the phone.
- **Keep radio alive** (`vs_keepalive`) — the phone holds the USB radio; whether it may wait for it is decided with the
  phone's own USB handling, and a wrong value from afar could leave a radio held that the owner cannot reach.
- **Microphone (an Icom's audio)** — Android's own permission prompt.
- **Protocol** (`vs_proto`, main app only) — rtl_tcp has no web page and no admin password: switching to it from the page
  would cut the page off with no way back.
- **Remote access to server settings** — switched off from the page, it would lock the owner out of the page.
- **The admin password** (`vs_adminpw`) — **never** sent to the page and **never** set from it (security rule, as on
  Linux: the page proves it with the nonce + HMAC challenge and never sees it). `configSetPatch` drops an
  `adminPassword` key whatever the patch says; `pageConfig`'s raw never contains it.

### The guard

`scripts/check-remote-keys.mjs` **fails** when a key of ServerModeScreen's K table (or the name/location keys it reads
beside it) is neither mirrored (REMOTE_KEYS / SPECIAL_KEYS) nor listed in DEVICE_ONLY / NOT_SETTINGS with a reason — so a
future setting cannot be forgotten on the page. It also holds: every key a page save sets is taken back by the screen and
vice versa; every mirrored storage key is one the screen reads; every mirrored key is in the config every Start writes
(else a Start would wipe it); the header's capture-width lists equal the screen's RATE_OPTIONS_*; each device-only key is
named in this doc; the page's read-only card and this doc name each device-only fact.

## What was tested, and how

Every-setting pass (2026-11-20 evening):

- `vibeserver/test-android-setup.cpp` — **115 checks, all pass** (also clean under ASan + UBSan): the JSON reader; the
  first cut's identity and rules; then EVERY setting on every radio — RTL, RSP, HF+, HackRF on Lite without the tunnel, an
  Icom on a TV — Android → page → Android is the identity, and every key not carried back is one that radio/device is not
  offered; which controls each radio is offered; saves of a control the radio lacks are dropped (RTL: SDRplay card, ppb,
  Icom; HF+: only ppb; Icom: no capture/blanker/locked range; no tunnel: no listing; TV: no battery); the new rules (rate
  snaps to the screen's list and an HF+ never gets a dongle's rate; waterfall 5/10/20; no web → not Advanced; never a DAB
  landing beside a DAB block; ADS-B keep-running needs the landing; location ranges, blank = off, grid square; ppm 0 on a
  radio left alone stays absent; the converter's low end only from a phone's page; antenna switch / background decoding
  must be JSON objects; raw-IQ LAN ceiling as text; Icom baud/delay snap; unknown de-emphasis = by region; listing is an
  action; a locked range without a centre keeps the phone's; the AGC lock is a yes/no, resting gain clamps to -1); every one
  of the 151 table ids has its value in the page config.
- `scripts/test-setup-android.mjs` (jsdom) — **77 checks, all pass**: for six radio/device cases every offered control is
  on the page and visible, nothing else is left, no other radio's control survives; Simple hides every Advanced-only
  control and keeps the rest; choosing Advanced shows them at once; no web client forces Simple; the device-only card
  (battery, power, USB, keep alive, microphone, protocol — not on Lite —, remote switch, admin password never sent); the
  phone's radio rows fill and post; sign-in stops before the Linux loaders; the prune re-runs after `renderHw`.
- `scripts/test-setup-android-e2e.mjs` — **32 checks, all pass**: the whole page signed in against a phone with the new
  settings, edited on both tabs, Apply → the posted body through the compiled translation with the phone's own raw.
- `scripts/check-remote-keys.mjs` — **450 checks, all pass** (see "The guard"); a fake K key was added by hand and it
  failed naming it, then the key was removed. Now registered in run-tests.sh (it never was).
- `VIBE_TESTS="test-android-setup" bash scripts/run-tests.sh` under the build lock: 1 suite built and passed (115).
- Gradle under the build lock: main app `:app:compileCurrentReleaseKotlin` OK; Lite `android/gradlew -p lite/android
  :app:compileReleaseKotlin -x :app:bundleJs` OK (the JS bundle step cannot resolve a symlinked node_modules in a
  worktree; Kotlin is what changed). Temporary symlinks to ~/VibeSDR-v12 (local.properties, keystore, lite node_modules,
  lite cloudflared) removed afterwards.
- `scripts/check-android-settings-parity.mjs` (183), `check-setup-page.mjs`, and the existing setup-page tests (tabswitch,
  maxfps, icom, radio-caps 71, filter-kind, bg*, bench*, dir-reach, governors, mode-rate, wifi*) pass.
  `check-icom-servermode.mjs` has ONE failure that predates this branch (it looks for `VibeIcom.prepareStart` in
  VibeLocalSdrModule.kt; the process split moved it to VibeServerOps/VibeServerRestore).
- `tsc --noEmit`: 45 errors, the base tree's 45 (handheld-ui/test, scripts/wefax-world); none in src/.

First cut (morning): `test-android-setup.cpp` 63, `test-setup-android.mjs` 78, `check-remote-keys.mjs` 52 — superseded by
the above.

## What needs a real phone (the XCover)

1. **WebView update:** serve; Play Store → Android System WebView → uninstall updates, then update it again (or wait for
   one). The app's screens may close; the server must keep serving (a listener stays connected), notification stays.
2. **Kill the server process:** `adb shell am kill net.vibesdr.app` does not reach a foreground service, so use
   Developer options → Running services, or `adb shell kill <pid of net.vibesdr.app:server>` (`adb shell pidof
   net.vibesdr.app:server`). Expect START_STICKY back within seconds; if not, the watchdog within ~5–15 min. Read
   `run-as` is closed on a user build — read `server_trail.txt` through Diagnostics once wired, or logcat tag
   `VibeServerRestore`/`RtlTcpServerService`/`VibeServerWatchdog`.
3. **Close the app:** swipe it away from recents while serving. ★ The server STOPS, as Stop would, and stays stopped —
   no watchdog restart, no restore (Stuart 11-20: "cant have server still running when app is closed down";
   RtlTcpServerService.onTaskRemoved). Settings → Force stop: the same.
4. **Remote settings:** set an admin password, switch "Remote access to server settings" on, Start. On a PC open
   `http://<phone>:<port>/setup`, sign in, change the landing message and listeners, Apply. The server restarts (listeners
   reconnect), and the phone's server screen shows the new values (open it, or watch it if open).
5. **Wrong password** five times → the lockout message, as on Linux. Switch off → `/setup` and `/vibeserver/config` gone.
6. **Local Hardware** with the server stopped still works; with it running it refuses with the sentence.
7. **Icom:** start serving an Icom, allow the microphone: audio starts (the answer crossed processes).
8. **RAM:** `dumpsys meminfo` as above.
9. **Every setting remote:** with an RTL, then an RSP (and an HF+ if to hand): on the page check only that radio's rows
   show (no SDRplay card on an RTL, no ppm on an HF+); switch Simple → Advanced on the page and see the sharing rows
   appear; change the capture width + pin, the waterfall rate, a gain ceiling, the FM defaults, the place, and Apply — the
   phone's screen shows each. List publicly from the page (with a place set) and unlist it. Change the antenna's port
   names and the RTTY workers and check the server applies them (they had never reached the engine before).

## Open (one line each)

- Diagnostics should show `server_trail.txt` (VibeServerRestore.trail exists; not yet wired into diagnostics.ts).
- ~~More controls for the remote page~~ — done 2026-11-20 (every setting). A new setting needs a table row + pageConfig +
  settingsPatch + the mirror; check-remote-keys.mjs fails until it has them or a DEVICE_ONLY reason.
- "Follow the phone" (moving) set from the page takes effect when the app runs (the UI process follows the GPS); the page
  sets the home place at once.
- The page's antenna-switch card stays behind its V11 gate (SW_SETUP false) as on Linux: shown only once a switch is set
  up. Rebuilding the screen's switch from the page's keeps relay order = antenna order (the phone's search result names
  are kept when nothing changed).
- The remote page restarts the server for every save with "Apply"; live-applicable settings could skip the restart.
- `maxRadiosPerIp` values above 1 map to "on" (the phone has one radio, so it is a yes/no there).
