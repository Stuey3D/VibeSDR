# Kept-running ADS-B at 2.56 MS/s after a restart (b96, branch b96-adsbstart)

## The report (Stuart 11-26, the Sony Bravia, Android Lite 122, 12.0.0~beta95)
An RTL on ADS-B with "Keep ADS-B running when nobody is connected" and adsb256 (base 2.4 MS/s). "I change a setting and
restart VibeServer, the 2.56 MS/s mode complains and says 94 % dropped" (the ADS-B view's "capture 94 %"), and no
aircraft at all. FM, then back to ADS-B, and it works. Intermittent: a later restart worked.

## The cause (proven by reproduction)
The whole-radio mode flags (`g_adsbMode`, `g_ismMode`, `g_ndbMode`, `g_aisMode`, `g_avMode`, `g_sdMode`, `g_vorMode`,
`g_dabMode`) and the ADS-B service `g_adsb` are **process statics** in `local_sdr_shim.cpp`. `stopLocked()` stopped some
mode services (ISM, NDB, AIS, …) but never `g_adsb`, and cleared **none** of the flags.

- **Android** restarts the engine **in the same process** (Setup save → `VibeServerOps.restartFromSettings`: `stopEngine`
  then `VibeServerRestore.restore` → `applyAndStart`). So did the Mac app (`vs_stop` / `vs_start`).
- A restart taken while kept-running ADS-B had the radio left `g_adsbMode = true` and the old `g_adsb` running, its demod and
  `CaptureMeter` built for 2.56 MS/s.
- The new receiver opened at the owner's 2.4 MS/s on the landing frequency. `adsbKeepTick` returned at
  "already in ADS-B", so nothing tuned 1090 MHz or applied the 2.56 boost.
- The DSP loop still fed `g_adsb` (it feeds whenever `g_adsbMode` is set) with 2.4 MS/s of landing-band IQ. The result:
  capture = 2.4 / 2.56 = **93.75 % → "94 %"**, and no aircraft (the wrong band).
- FM → ADS-B ran the real `adsbLeave` and then `adsbEnter`, which is why the round trip cured it.
- **Intermittent** because it needs ADS-B to be ON at the moment of the restart. A save made while a listener had the
  radio in another mode (FM, say) left the flag false, and that restart came up correctly.

It was not the rate guard. A guard that holds 2.56 makes `adsbTargetRate` return 2.4, and the service is then started
at 2.4 (`captureSpanHz`), so the two always agree.

**Linux (the Pi 500 question):** every daemon restart is a new process (`execv` / systemd), so these statics start
clean and the fault cannot happen there. Measured: 10 of 10 fresh daemon starts, before and after the fix, entered
ADS-B at 2.56 MS/s on 1090 MHz with aircraft (`scripts/test-adsb-restart-linux.sh`). If the Pi 500 really did show it,
it is a different fault, and the journal around the restart is needed. Capture these lines:
`[ADS-B] keep running: …`, `[ADS-B] rate … -> … MS/s`, `[ADS-B] mode ON: …`, `rate guard` / `[rate]`, and `USB IQ:`.

## The fix
`endWholeRadioModesForStop()` is called from `stopLocked()` after the decoders stop. It:
- clears every whole-radio flag;
- stops `g_adsb`;
- zeroes the datalink's 8 MS/s from-rate;
- ends DAB through `dabModeEnd()`;
- logs `stop: the receiver was in ADS-B — ended with it, so the next start enters afresh`.

Nothing is "restored", because the receiver those saved states belong to is being destroyed. The next start is then a
clean one. Kept-running ADS-B comes in through the same internal entry a listener's button uses: open at the base rate,
let the stream run, then apply the boost to 2.56 in `adsbEnter`. That is Stuart's "start at 2.4 then change to 2.56"
shape, and it needs no special case.

## Tests
- `vibeserver/test-adsb-restart.cpp` + `scripts/test-adsb-restart.sh` (CMake target `test-adsb-restart`). It runs the
  real shim **in one process** against `fake-rtl-tcp` replaying a 2.56 MS/s clip with three aircraft
  (`test-adsb --write-clip out secs 2560000`, a new optional rate argument). The loop is start → kept ADS-B → stop →
  start …, with varied gaps. After every start it checks:
  - the radio's rate (`captureSpanHz` = 2 560 000);
  - fresh aircraft (`/vibeserver/aircraft.json?since=3` count > 0);
  - the fake's log, for a 1090 MHz tune and a 2.56 MS/s request made by **this** start.
  - **BEFORE the fix: 1 of 6 starts good.** Every in-process restart stayed at 2.4 MS/s, never tuned 1090 and heard 0 aircraft.
  - **AFTER: 30 of 30**, each start in ADS-B with aircraft within 2.5–3.5 s.
- `scripts/test-adsb-restart-linux.sh`: N fresh daemon processes with the same config. 10/10 before, 5/5 after.
- Android: an NDK r27 arm64 `-fsyntax-only` compile of `local_sdr_shim.cpp` with the Lite defines is clean. Gradle could
  not run in the worktree because it has no `node_modules`.

## RESUME HERE
- Coordinator: add `scripts/test-adsb-restart.sh` (needs `test-adsb-restart` built) and the Linux script to
  run-tests.sh if wanted. A Gradle build of Lite is still owed.
- Live check on the Sony after the release: restart from Setup while in kept ADS-B. ADS-B should come straight back
  with aircraft and no "capture 94 %". Logcat should read `stop: the receiver was in ADS-B — ended with it …` and then
  `[ADS-B] keep running: … the server has started` and `[ADS-B] rate 2.400 -> 2.560 MS/s`.
- The same stale-flag fault would have hit any kept-running or background whole-radio mode across an Android restart
  (ISM, AIS, VDL2). The fix covers them all, and b96-keeprun should know.
