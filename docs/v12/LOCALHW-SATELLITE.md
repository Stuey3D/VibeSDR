# SATELLITE in Local Hardware (beta94, branch `localhw-sat`)

Tester Mac Ha (beta93, Android, OTG RTL-SDR = Local Hardware) had no SATELLITE key. Stuart: "can we have satellite mode
enabled with a position? local hardware mode has position data for the spots and aviation maps" — then: "allow it to be
selected if no location and give the user the ability to enable location or add their own … location, state satellites
need location to calculate doppler and will not work without your location. This doesn't need to be street level,
nearest town will work." and "in local hardware mode the location goes nowhere anyway".

## RESUME HERE

Built, unit-tested, Android release APK compiled. **Not yet run on a phone.** Owed: a real OTG pass on Android —

1. Fresh install, location permission NOT granted, RTL on OTG → Local Hardware → mode sheet: SATELLITE is drawn and
   not dimmed → tap → the box says satellites need your location, with ENABLE LOCATION and a town/locator field.
2. Type `IO92nh` (offline) → the timetable appears within ~3 s (or "NO SAT DATA" while the first orbit download runs,
   then the timetable); the line under it says "Position: IO92nh (entered)" with CHANGE / CLEAR.
3. Leave and come back: not asked again. CLEAR → the prompt again (or the phone's location if it is allowed).
4. ENABLE LOCATION with permission granted → "Position: this phone's location · <square>".
5. rtl_tcp from the phone (Local Hardware via rtl_tcp) — the same, and the orbit data arrives (VibeMapGL.start now runs
   for rtl_tcp / SpyServer too).
6. While the phone ALSO serves as a VibeServer with a location: the server's listeners see the server location only;
   nothing typed in Local Hardware shows on vibeserver.json, /location or the directory.

## Root cause (verified)

- The satellite engine's observer came only from `LocalSdrShim::setLocationJson` (the owner's published server
  location). Local Hardware with no server location → no observer → hwinfo `doppler:1` + `dopplerWhy` "no location".
- The app hid the key: `satAvail` required `satCap.ok || satOn`. That was DELIBERATE, not a slip — the mode menu's
  rule since Stuart 10-27 ("those dead buttons can be hidden": a refused whole-radio key is not drawn unless on), the
  web page does the same. So a real server with no location keeps that rule (the owner's to fix); only this app's own
  engine gets the new selectable-with-prompt path.
- Local Hardware runs in the UI process with its own engine copy (docs/v12/ANDROID-SERVER-PROCESS.md); its orbit
  supply is wired by `VibeMapGL.start` in `startSpectrumNow` (USB) — NOT for rtl_tcp / SpyServer, which would have
  said "no orbit data" for ever. Now wired there too.

## What changed

- `vibe_sat_observer.h` (new, pure): owner position wins; the device's fills in only while the owner has none; CLEAR
  clears only what the device gave. `Engine::clearObserver`.
- Shim: `LocalSdrShim::setSatObserver(have, lat, lon)` → the engine ONLY (not g_locJson, setReceiverPosition,
  setBandRegion, g_rxInUs); `setLocationJson` goes through the same source; hwinfo adds `dopplerNeed:"location"`.
- JNI `nativeSetSatObserver`, `VibeLocalSDR.setSatObserver`, `VibeLocalSdrModule.setSatObserver/clearSatObserver`
  (UI process only — never `tell()` to the server process).
- App: `src/services/satPlace.ts` (pure rules + words), `src/services/localPlace.ts` (storage `vibe.localPlace`,
  grid-square centre, native hand-over), SDRScreen + SatellitePanel prompt, AboutOverlay note.

## iOS — deliberately unchanged

iOS local mode (rtl_tcp/SpyServer through the prebuilt shim) has no orbit-data supply (`vsSatSupply`: no hooks), so a
position would only turn "no location" into "no orbit data" for ever — a dead control. `satOwnEngine` is Android only and
the iOS module gets no new method (it would also need `build_ios.sh` to relink the prebuilt library). iOS keeps the old
rule (hidden). Making it work needs an iOS orbit fetch (URLSession → satLoadSats) — a separate job.

## Tests

- `vibeserver/test-sat-observer.cpp` 17/0 (precedence; the ISS pass from the phone's position vs Skyfield; CLEAR;
  sat_list carries `located` only, no lat/lon). `test-sat-engine` still 192/0.
- `scripts/test-server-sat.mjs` 197/0 with the new 4a (real server: no location → `dopplerNeed:"location"`, /location has
  no position; located → none).
- `scripts/test_sat_place.ts` 39/0 (the key, the prompt, which position, the words, nothing-published source guards).
- tsc: no errors under src/ (the existing node-types noise in scripts/ and handheld-ui/ is unchanged).
- `./gradlew assembleRelease -x requireUploadKey` (both flavours, armeabi-v7a + arm64-v8a, R8 on): BUILD SUCCESSFUL;
  `Java_com_vibesdr_app_VibeLocalSDR_nativeSetSatObserver` exported from libvibelocalsdr.so. The upload-key gate was
  skipped because keystore.properties is a secret not in the worktree — the debug-signed APKs were deleted, never
  installed. Mac vibeserver (Release) built clean via the build lock.
