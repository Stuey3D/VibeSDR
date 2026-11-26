# "Use as a server" — three ways, remote first-time setup, device-only password / reset (beta96)

Branch `b96-serverchoice` (off `b96-integrate`, merged back in twice). Stuart, 2026-11-26.

## RESUME HERE

**b96-appserver2 (2026-11-26, Stuart's decisions 1–7 + the three-state refinement) — built, tested off-device; never run on a
phone, TV or radio.** What changed since the section below was written:

| Decision | What is built | Where | Tested |
|---|---|---|---|
| 1. Entry labels | exactly "VibeServer Simple (Local Network Only)", "VibeServer Full", "RTL-TCP (Compatibility)" (`CHOICE_WORDS` titles, `ENTRY_LABELS`; the screens' headings use them) | `src/utils/serverChoice.ts` | test_server_choice |
| 2a. Simple PIN | optional, digits only, ≤ 12 (`pinProblem` / `pinTyped` = the server PIN's rule); stored as **`simplePin`** (its own key: Full's PIN is never overwritten, and the other way round); the Simple view runs it as `pin` (Kotlin `simplePinOk` twin) | serverChoice.ts, VibeServerBoot.kt, SimpleServerScreen | test_server_choice |
| 2b. Home network only | `lanOnly: true` in every Simple view → `VibeLocalSDR.setVibeServerLanOnly` → `LocalSdrShim::setVibeServerLanOnly` → **every request** from a peer that fails the RTL-TCP door's `isPrivateIp` (private IPv4, 169.254, IPv6 link-local / ULA / the box's own prefixes) — judged on the REAL socket peer — or that came through the tunnel is answered 403 "local network only", logged once a minute; `vibeserver.json` `lanOnly:{refused}`. Daemon twin: `--lan-only` (tests) | local_sdr_shim.cpp `handleConnection`, vibe_localsdr_jni.cpp, main.cpp | **test-server-lanonly.mjs 14/14** (e2e, the RTL-TCP door test's shape: `VIBE_TEST_LANONLY_PEER`) |
| 2c. Simple RAW IQ | Full's RAW IQ OUT as Off / Local only (no public choice: dead on a LAN-only server); `simpleRawIq` → rawIq 0/1, rawIqMax 1; not on Lite or an Icom (as Full) | serverChoice.ts, SimpleServerScreen | test_server_choice |
| 3. RTL-TCP | unchanged single screen (heading now "RTL-TCP (Compatibility)") | RtlTcpServerScreen | — |
| 4. Full tabbed | **SERVER \| RADIO** (`SetupTabs`, `FULL_TABS`, the radio tab named after the radio); START bar (serverChoiceTop) first, then the tabs, OPEN/CLOSE ALL, the tab's sections in Stuart's order; RTL-TCP protocol = no radio tab | ServerModeScreen, SetupSection.tsx, setupSections.ts | test_setup_sections, test_nav_coverage (START → tabs → all-row → headers; each tab a focusable stop with its ring) |
| 5. Red "!" | **only on a refused START**: `startBlocks` (admin password, flight-tracker entry, background profile, Icom CI-V) → `startRefusal` → the first blocker's tab selected, every blocker marked tried (red, opened — `reopenFor` undoes a by-hand close), one line under START ("Not started — <section>: <reason>"); red until fixed (`triedPrune`). Both START keys are green always. **Three states** (Stuart's refinement): green / amber (location, PSK Reporter no callsign, remote settings off; a blocker before any START reads amber) / red | setupSections.ts `secState` | test_setup_sections 89/0 |
| 5b. The web page | **changed too** (it shared the rule, not the code): `SEC_MUST` → `SEC_BLOCK` / `SEC_ADVISE` + `secSaveStops` gate on both Save buttons — red only after a Save it stopped, on its own tab (`secGoTab`), opened, saveErr line, red until fixed. PSK Reporter with **no** callsign is amber and no longer refuses the save (the server just sends nothing); a **typed invalid** name still refuses (red). A radio never set up turns red at the master Save without refusing it. `SEC_RULES` = the app's `RULES.web` (tested word for word) | vibe_setup_page.h | test-setup-sections.mjs 63/0 |
| 6. Full's Simple / Advanced | **removed**: `advanced = webServer` (the admin page needs the web client — as the old toggle forced); one listener or many = **Listeners** + **Operating mode** on the RADIO tab. The remote page's `androidAdvanced` select is gone too (`advanced` follows the web client) | ServerModeScreen, vibe_setup_page.h, vibe_android_setup.h | test-setup-android 76/0, parity 217/0 |
| 7. Maritime boost | see MARITIME-COMBO.md ("ais+8m") | | maritime e2e 50/0 |

**Owed:** the Sony + a phone (tabs with the remote, a refused START landing on the right tab, Simple's PIN typed on a TV,
a non-LAN client really refused — e.g. a phone on mobile data at a port-forwarded Simple server); `npx tsx
scripts/test_new_modes_clients.ts` (no tsx in this worktree); Lite gradle (no Lite gradlew here).

**Builds (b96-appserver2):** vibeserver + test-radio-caps (lock) ✓; tsc whole project — 45 errors, identical to
b96-integrate's (none new); app gradle — see the final report.

### Stuart's answers (11-26) — applied
1 separate keys: as built. 2 PSK Reporter with no callsign = amber "PSK Reporter: no callsign, so no spots are sent" (app +
web); the uploader now refuses an INVALID name too (vibe_spot_report.h `receiverNameOk`, the page's 3–20 / a-letter rule —
test-spot-report 103/0). 3 kept as built. 4 kept. 5 dec_aisdsc auto-ticks ais+8m (MARITIME-COMBO.md). 6 kept.

### Open decisions for Stuart (b96-appserver2, one line each — answered above)
- Simple's PIN and RAW IQ are their own keys (`simplePin`, `simpleRawIq`) — not Full's PIN. OK?
- PSK Reporter with no callsign no longer blocks the web Save (amber, your example); a typed name that is not valid still does.
- A radio never set up: red at the master Save but the Save still goes through (the other radios go on air). OK, or refuse?
- With the web client OFF, Full still hides the admin-page parts (listeners count, gain limits, banning) exactly as the old toggle did.
- Simple and RTL-TCP got no section marks: Simple's only blocker (the PIN) cannot be typed wrong (the box keeps digits only), RTL-TCP has nothing to warn about.

(Everything below is b96-serverchoice's record, still true except where the table above says otherwise.)

### Earlier resume list (b96-serverchoice)

1. **Device test on the Sony (TV remote) and a phone (touch + a pad)**: first open → SET UP HERE / REMOTE SETUP; Simple
   start/stop; the remembered choice; Simple → Full hand-over while running; RTL-TCP's short screen; REMOTE SETUP end to
   end with a laptop (code → password twice → setup → Finish with and without the remote-access tick); Change admin
   password / PIN; RESET (wrong password, right password → first-run screen).
2. **Gradle** — see "Builds" below for what was and was not compiled.
3. Open decisions for Stuart (end of this file).

## The entry (both the app and VibeServer Lite)

`src/screens/ServerChoiceScreen.tsx` (route `ServerChoice`; the picker's "Use as server" row now opens it).

- **First open** (never chosen a type AND no admin password — `isFirstOpen`): two big keys, **SET UP HERE** and
  **REMOTE SETUP**. REMOTE SETUP is drawn only while the server has no admin password (`remoteSetupOffered`) — once one
  exists it is not drawn at all, and the server refuses to issue a code anyway.
- **SET UP HERE** → the list: **VibeServer Simple · VibeServer Full · RTL-TCP** (`choicesFor`: Lite = Simple + Full —
  RTL-TCP was removed from Lite on purpose 2026-09-18; an Icom = no RTL-TCP, said in one line).
- **Remembered** (`vs_serverchoice`): after that the entry opens the last type straight away. Each type has
  **‹ Server type** back to the list (`{ choose: true }`).
- A server already running wins: RTL-TCP running → its screen; a VibeServer → Simple or Full as last chosen (Full by
  default), which adopt it (`keepOnExit: true`).
- Switching between a VibeServer and RTL-TCP while one runs asks "Stop the running server?" first.
- Full is always opened `vibeServerOnly` now — RTL-TCP is its own entry, so Full's old protocol picker is not drawn
  (the same param Lite always used; no code change in Full for it).

Lite: `lite/app/index.js` now opens on the same entry with a tiny stack (navigate / replace / goBack, hardware back);
`InstancePicker` still minimises.

## VibeServer Simple — local network only

`src/screens/SimpleServerScreen.tsx` (route `SimpleServer`). **START first**, then SERVER NAME, ADDRESS ON THIS NETWORK
(`.local`, '' = made from the name — `vs_mdnsname`), CAPTURE WIDTH (the radio's own list — Full's `RATE_OPTIONS_*` by
driver, exported; none for an Icom) and WATERFALL (the fps tiers). Running: one status block — serving, the address to
open, `name.local`, listeners — and the remote-settings line. Keys: STOP · Keep serving (Lite: minimise) · Full settings
· ‹ Server type; then Change admin password and Reset at the bottom.

**How it is enforced (server side, not the screen):**

- Simple sends only its four plus `simple: true` (`simpleStartConfig`, through `startVibeServer` directly — not
  `nativeServerConfig`, which would fill every other key with defaults).
- `VibeServerOps.startVibeServer` **merges** them into the one settings store (`VibeServerBoot.simpleMerge` over
  `VibeRemoteSetup.settings`) — so **every Full setting stays stored**.
- `VibeServerBoot.applyAndStart` runs `effective(cfg)`: for a `simple` config, the **Simple view** — Simple's four, the
  radio's own front-end settings (`RADIO_KEYS`: bias-T, ppm, converter, gain rules, SDRplay card, Icom, antenna ports,
  admin password …), the engine's defaults for everything else, one listener, no PIN, and **LAN_ONLY_OFF**: community
  map, PSK Reporter, WSPRnet, DMR ID lookups, ADS-B feed, background decoding, DX scanner all OFF. Every start path goes
  through it (the screen, the crash restore, a restart for the /setup page).
- No public listing: `VibeTunnel.restoreIfWanted` is skipped for a Simple config in **VibeServerOps and
  VibeServerRestore**, a tunnel still up is closed (`pauseForServerStop` — not a delist: Full's standing instruction is
  kept), and the /setup page refuses to list a Simple server (`configSetPatch`).
- The three lists exist twice (TS `src/utils/serverChoice.ts`, Kotlin `VibeServerBoot`); `test_server_choice.ts` reads
  the Kotlin and fails if they drift.

**Simple ↔ Full:** one server, one store. Simple → Full: Full shows the name / address / width / rate (Full got an
ADDRESS ON THIS NETWORK field under LOCAL NAME and sends `mdnsName`); a running Simple server is handed over
(`keepOnExit`) and stays local-network-only until started from Full. Full → Simple: Full's settings stay stored; Simple
says so ("Settings you made in VibeServer Full are kept …"); a server started from Full and adopted by Simple says it is
not limited yet and offers **Restart as Simple**. Full's Start writes its config whole, without the mark.

**mDNS name:** `VibeServerBoot.mdnsName(cfg)` = `mdnsName` or the name; `hostSlug` makes the label on both start paths
(Start and the /setup restart). `vs_mdnsname` is DEVICE_ONLY in the mirror (`serverSettingsMirror.ts`, doc'd in
ANDROID-SERVER-PROCESS.md).

## RTL-TCP

Its own short screen first (`RtlTcpServerScreen`, opened with `setup: true` from the entry and nothing running): LOCAL
NAME, ▶ START RTL-TCP, ‹ Server type. Any other way in starts at once, as before. The start was moved into `beginRef` so
both roads run the same code.

## START / STOP first on Full; device-only password / PIN; reset

`src/components/ServerTopBar.tsx` — in b96-setupux's `serverChoiceTop` hook (setup view: above the banners and every section) and as the first line of the running view (START, or STOP when running, takes
the TV remote's first focus) plus the remote-settings line. Its START runs `pressStart` — the bottom START's own checks
(no admin password → red key, Icom CI-V) — so the two cannot drift.

**Remote-settings line** (`remoteSettingsLine`): with remote access ON and a password, "Change settings from another
device: http://<ip>:<port>/setup or <name>.local:<port>/setup — sign in with your admin password"; otherwise "Remote
settings: off (turn on here to change settings from another device)".

`src/components/DeviceCredentials.tsx` — **Change admin password / PIN**, no old password (the device is the proof).
In Full's setup view it is the `devicePassword` section (b96-setupux's slot, before "Admin password and PIN"); in the running view and on Simple it sits at the bottom. Typed twice, 8–128 characters (`passwordProblem`, the shim's rule too). Saves to the screen's copy, the server's store
(`settings.put`) and the running engine (`setVibeServerAdminSecret`, and a new live `setVibeServerPin`). The PIN half is
Full only. The remote /setup page still needs the current password and can never change it.

`src/components/DeviceReset.tsx` — **RESET ALL SETTINGS AND START AGAIN**, at the very bottom of Simple and Full (after every section):
the admin password typed (or the word RESET where none was ever set), then "Are you sure?" listing what goes.
`VibeRemoteSetup.resetAll` (binder only — no HTTP route; a /setup save naming `factoryReset` / `resetAll` is refused):

- **Wiped:** the server settings store, the remote-access flag, the restore's stored config and stamps, the public
  listing (delisted), the live admin password and PIN; in the app every `vs_` key (all server settings, the chosen
  type → first run again, `vs_mdnsname`, the PIN, the admin password copy …), `lsv_server_location`,
  `vsdr_rtltcp_server_name`, `vsdr_rtltcp_server_persist`.
- **Kept unless ticked** ("Also delete the gallery and stored data", off by default — **my choice**: they are the
  owner's catches, not settings): the gallery folder, `dxlog.json`, the learned bookmarks (`vibe_bookmarks.json`,
  `vs_learned_bookmarks`).
- **Never touched:** the app's own listening things (favourites, directory, receiver settings, pad profile …).

## Remote setup — "Set up on another device", first-time only

- **TV**: `src/screens/SetupElsewhereScreen.tsx` (route `SetupElsewhere`). Starts the server on this network (as Simple,
  so /setup is served and nothing leaves the LAN), asks for a code, shows it big (ABCD-EFGH), the address
  (`http://ip:port/setup` and `name.local:port/setup`), a countdown; "Setup in progress on <ip>" once used; "Too many
  wrong codes", "ran out" + New code; big ‹ BACK first (cancels the code and the window; stops a server it started
  only for this if no password was set).
- **Engine**: `android/app/src/main/cpp/vibe_setup_code.h` (header-only) + the shim's `POST /vibeserver/setup-code`,
  `/setup-code/password`, `/setup-code/finish`. 8 symbols from a 31-symbol no-look-alike alphabet, crypto-random
  (`vsRandom64`, rejection sampling), 5 minutes, single use, 5 wrong tries kill it, constant-time compare, never logged,
  LAN only (`isPrivateIp(socketPeerAddress)` and not through the tunnel — the RTL-TCP door's rule). **Refused at issue
  and at use while an admin password exists.** The code opens a session whose one power is setting the FIRST admin
  password (typed twice on the page); then the page signs in the ordinary way with it (nonce + HMAC). The session token
  is never in the state the TV reads.
- **Finish** (an admin by then): closes the window; the store loses the `simple` mark (the owner set every setting —
  it is a Full server now) and the engine restarts on it; the TV hands over to Full's status screen. **Remote access is
  switched on only if the owner ticked** "Allow changing settings from other devices later" — never as a side effect.
- **Kotlin**: `VibeRemoteSetup.setupCodeIssue / Cancel / State` (binder ops `setupCode.*`), `setupPasswordSet` (JNI
  upcall: stores it, puts it in force, emits `adminPassword` so the app's own copy follows), `setupFinish`. /setup is
  served while the window is open (`setupWindow` in `applySwitch`), and goes back to the owner's switch on cancel.
- A start never wipes a stored admin password with an empty one (the phone's screen may not know a password set from
  the laptop).

**The risk, plainly:** the local network has no HTTPS, so the code and then the new password cross the Wi-Fi
unencrypted. That is why it is first-time only, single-use, five minutes, five tries, LAN-only, and why the TV says
"use it on your own Wi-Fi". Someone on the same network who sees the TV's screen in those five minutes could set the
password first — the TV would show "Setup in progress on <their address>" and the owner can back out (which ends the
window) and reset on the device.

## Lock-out guard

"Remote access to server settings" is the device's switch: the page shows it read-only, and a save naming
`remoteAccess` / `remote` / `remoteSetup` is refused with "can only be changed on the device itself"
(`vibe_android_setup.h settingsPatch`). Only the device's own screen turns it on or off.

## Tests (run, results)

- `vibeserver/test-setup-code.cpp` — **42/42** (ASan + UBSan, compiled alone): shape and alphabet, refused with a
  password (at issue and at use), 5-minute expiry, cancel, single use, forgiving typing / near-miss refused, LAN-only
  refusal not counted, 5 wrong tries kill it, session renew / idle end / never in state, window after the password,
  Finish, uniformity over 32 000 symbols, rejection of the top of the range, the lock-out guard and reset refused from
  /setup. In CMake (`add_test setup-code`) and picked up by run-tests' C++ loop.
- `vibeserver/test-android-setup.cpp` — still all passed after the guard.
- `scripts/test_server_choice.ts` — **70/70**: the pure rules (choices, first open, remote setup offered, the label,
  Simple's config, merge keeps Full, view is LAN-only, the status line, password rule, reset keys), the Kotlin twins,
  the native wiring (tunnel gates on both start paths, no permanent remote access from first-time setup, code refused
  with a password, LAN-only, never logged, reset's password check and no HTTP route, the page's code → password →
  sign-in → Finish), and the screens (no bare touchables, pad list + first focus, START/STOP first in Full, credentials
  + reset at the bottom, Simple saves its four and remembers, Simple → Full hand-over, ‹ Server type, the picker row,
  Lite's entry). Added to run-tests.sh.
- `scripts/test_nav_coverage.ts` — passes; the seven new files are on its "every touchable registered" list.
- `scripts/check-remote-keys.mjs` — 503 passed, 2 FAILED — **both pre-existing on b96-integrate** (`vs_bgmode`, from
  b96-bgsplit); `vs_mdnsname` is accounted for.
- `tsc` (whole project, `--max-old-space-size=4096`): **no new errors** — the 45 left are identical to b96-integrate's.
- The setup page's script parses (`new Function` over the extracted `<script>`).

Not tested: anything on a device; the HTTP routes end to end against a running engine (the header is tested; the routes
are reviewed and compiled — see Builds); the Lite bundle under RN 0.73.

## Builds

- `cmake --build … --target vibeserver_core test-setup-code` (via the lock) — **built**: the shim with the new routes
  compiles; `test-setup-code` 42/42 from the CMake build too.
- `./gradlew :app:assembleDebug -PreactNativeArchitectures=arm64-v8a` (via the lock) — **built** (Kotlin, the JNI
  file, the arm64 native library, the JS bundle). Built before the last merge of b96-integrate (b96-setupux), which
  touched only ServerModeScreen's TypeScript here; tsc is clean for it.
- **Lite**: no Gradle run (its prebuilt native kit is not in this worktree). Its JS was bundled with Metro under its own
  RN 0.73 (`react-native bundle`, Lite's metro config with the real node_modules path) — **bundles**, and contains the
  new screens with no Reanimated / Skia.
- The tunnel / Kotlin shared files Lite compiles (`VibeServerBoot`, `VibeServerOps`, `VibeServerRestore`,
  `VibeRemoteSetup`, `VibeLocalSDR`) are the ones the app build compiled.

## Open decisions for Stuart

- Simple has **no admin-password field** (your four controls) — a fresh Simple server runs with none, on the LAN only.
  It can be set with "Change admin password" on the same screen. OK, or should Simple insist like Full?
- Simple runs with **no PIN** (local network only). OK?
- Full still has its internal **Simple / Advanced** toggle from August — two different "Simple"s now. Rename Full's to
  "One listener / Advanced"?
- LAN-only is enforced by **features** (no tunnel, no listing, no feeds); the engine does not refuse a non-private
  peer for a Simple server if an owner port-forwards it by hand. Add the RTL-TCP door's peer refusal to Simple too?
- A remote setup leaves the server as **Full** after Finish (the owner set every setting). OK?
- Reset keeps the gallery / DX log / learned bookmarks unless ticked.
