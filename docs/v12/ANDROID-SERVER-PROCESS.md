# The Android server in its own process, the watchdog, and remote access to its settings (beta88)

Branch `b88-android` (off v12-experimental 9da8a5c0d), 2026-11-20. Main app (net.vibesdr.app) and VibeServer Lite.

## RESUME HERE

Built, unit-tested and compiled (see "What was tested"). **Not yet run on a phone.** Next:

1. Install the beta88 APK on the XCover (Stuart — never an agent: no adb to the XCover/Sony).
2. Run the phone checks in "What needs a real phone" in order; the first two prove item A.
3. Read `filesDir/server_trail.txt` (Diagnostics will show it once wired — not done, see Open) after any odd restart.

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
- **Supported controls:** name, mDNS listing, landing message/link, uncompressed audio, trusted proxies, one radio per
  address, decoders at once, gallery (size/public/listeners'), battery pause; radio label, aerial name, listeners, ask
  before tuning, keep the spectrum sharp, landing frequency + mode, session limit + soft, idle kick, wide noise blanker,
  VFO B, idle grace, PIN (set/clear). Everything else stays phone-only for now (see Open).

## What was tested, and how

- `vibeserver/test-android-setup.cpp` (63 checks, all pass): the JSON reader (escapes, surrogates, refusal of truncation,
  control characters, >32 nesting), Android → page → Android identity for every supported control, unsupported fields and
  a second radio ignored, the phone's rules (resume = pause + 20, landing 0 removes the key, PIN digits ≤ 12, blank name and
  unknown demodulator refused, clamping, length cuts), the admin password never in the page config.
- `scripts/test-setup-android.mjs` (jsdom, 78 checks, all pass): every supported id exists on the real page; after the
  prune each is visible and **no other control is**; the device card is read-only; sign-in returns before the Linux
  loaders. Registered in run-tests.sh.
- `scripts/check-remote-keys.mjs` (52 checks): the header's patch keys, the screen mirror's keys and the screen's K table
  agree.
- Existing setup-page tests unchanged (tabswitch, maxfps, icom, radio-caps 71, filter-kind).
- `tsc --noEmit`: 45 errors, the same 45 as the base tree (handheld-ui/test, scripts/wefax-world — no @types/node); none
  in src/.
- Gradle: see the report (main app assembleCurrentRelease, Lite assembleRelease, merged manifests checked for `:server`).

## What needs a real phone (the XCover)

1. **WebView update:** serve; Play Store → Android System WebView → uninstall updates, then update it again (or wait for
   one). The app's screens may close; the server must keep serving (a listener stays connected), notification stays.
2. **Kill the server process:** `adb shell am kill net.vibesdr.app` does not reach a foreground service, so use
   Developer options → Running services, or `adb shell kill <pid of net.vibesdr.app:server>` (`adb shell pidof
   net.vibesdr.app:server`). Expect START_STICKY back within seconds; if not, the watchdog within ~5–15 min. Read
   `run-as` is closed on a user build — read `server_trail.txt` through Diagnostics once wired, or logcat tag
   `VibeServerRestore`/`RtlTcpServerService`/`VibeServerWatchdog`.
3. **Kill the UI only:** swipe the app away from recents while serving. Server keeps serving.
4. **Remote settings:** set an admin password, switch "Remote access to server settings" on, Start. On a PC open
   `http://<phone>:<port>/setup`, sign in, change the landing message and listeners, Apply. The server restarts (listeners
   reconnect), and the phone's server screen shows the new values (open it, or watch it if open).
5. **Wrong password** five times → the lockout message, as on Linux. Switch off → `/setup` and `/vibeserver/config` gone.
6. **Local Hardware** with the server stopped still works; with it running it refuses with the sentence.
7. **Icom:** start serving an Icom, allow the microphone: audio starts (the answer crossed processes).
8. **RAM:** `dumpsys meminfo` as above.

## Open (one line each)

- Diagnostics should show `server_trail.txt` (VibeServerRestore.trail exists; not yet wired into diagnostics.ts).
- More controls for the remote page (DAB landing, blocked modes, allow/block ranges, gain rules, bias-T…) — each needs the
  page id, the header mapping and the screen mirror (three lists, checked by check-remote-keys.mjs).
- The remote page restarts the server for every save with "Apply"; live-applicable settings could skip the restart.
- `maxRadiosPerIp` values above 1 map to "on" (the phone has one radio, so it is a yes/no there).
