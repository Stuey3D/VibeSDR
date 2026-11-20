# What this phone can run — the device benchmark in Local Hardware (Android)

Stuart, 2026-11-20: "we should offer the benchmark to the local hardware mode too so people on weaker devices know exactly
what they are getting".

## RESUME HERE
- Branch `v12-localbench` (from v12-experimental 7dca7a783). Built and unit-tested; **not yet run on a phone**.
- First phone check (an RTL and, if possible, an Airspy HF+ on an Android phone): cog → "<radio> Controls" → foot of the
  sheet, WHAT THIS PHONE CAN RUN → MEASURE THIS PHONE. Watch that (1) the waterfall stops and the progress bar moves,
  (2) the run is NOT refused with "1 listener is connected", (3) waterfall and audio come back by themselves afterwards,
  on the same frequency and gain, (4) the server screen shows the same result, (5) only lines this radio can do appear.
- Open decision for Stuart: the server screen's benchmark DEFAULTS (Advanced RDS off below green, decoders off, keep-sharp
  zoom, decoders at once …) are NOT applied in Local Hardware — see "Defaults" below. Nothing was added silently.

## Where it is
The foot of LocalHardwarePanel ("<radio> Controls", from the settings cog), after the radio's own settings and outside the
owner-lock shield. Not on the control deck. Drawn only when SDRScreen passes `bench`, which it does only for `pagingHere`
(Android, a radio plugged into THIS phone: no localHost, not rtl_tcp, not SpyServer) and a build with
`VibeLocalSDR.runBenchmarkOwnRadio`.

## How it works
- **Same benchmark, same file.** `VibeBenchmark.run(ctx, wantDab, ownRadio = true)` → `nativeRunBenchmark(…, ownRadio)` →
  `benchmark.json`. The server screen reads the same file (`lastBenchmark`), so a run from either shows in both. The card
  re-reads it every time the sheet opens.
- **The listener refusal.** Local Hardware's engine is bound to loopback and its one listener is this app. With
  `ownRadio` the JNI skips the "N listeners are connected" refusal — but only while `LocalSdrShim::serveOnLan()` is false.
  The hold is unchanged: idle work stopped, radio released (an RTL; reopened on the fd we keep) or its capture paused
  (HF+ / HackRF / Airspy opened from an Android descriptor are parked), measured, taken back, `benchUnhold`.
- **Pause / resume (app side).** `src/services/localBenchRun.ts` owns the run (the sheet is a Modal that unmounts when
  closed): `client.pauseSpectrum()` (the backgrounding path — stops the stale-frame watchdog too), run, poll
  `benchProgress` every 700 ms, then in `finally` `client.resumeSpectrum()` + `reviveLocalAudio()`. The audio socket stays
  open throughout.
- **Reading.** `src/services/localBench.ts` (pure): grade → "Runs comfortably" / "Runs, but uses a lot of this phone" /
  "Too heavy for this phone" / "Not measured on this phone"; "N% of one processor core" as small print (a group names its
  heaviest part). Lines only for what this radio can do here, from the engine's own per-radio answers
  (/vibeserver.json: dab, freedv sub-modes, m17, adsb, ais, ism bands, ndb, avdata, sondes, hfdl, skimmers, spots band;
  `tunable` for FM and HF below 24 MHz). No row → no line: **DRM has no benchmark row**, so it never appears.
- "The radio pauses for about N minutes" from the last run's `benchSec` (rounded up), else "a few minutes".
- `wantDab` = the radio can do DAB (an HF+ does not fetch the 18 MB DAB clip; its stored result then has no DAB rows).

## Defaults (not applied — for Stuart)
ServerModeScreen.applyBench turns things off from the benchmark at a Lite first setup (vs_* keys sent with
startVibeServer). Local Hardware's start (`startSpectrumNow`) passes none of them and SDRScreen reads none of the vs_* keys,
so Local Hardware applies no benchmark defaults (Advanced RDS stays available on a phone that measured it amber/red, etc.).
This card only reports; it changes nothing.

## Tested how
- `scripts/test-local-bench.mjs` 37/37 (in run-tests.sh): words, an RTL vs an Airspy HF+ (no dead rows), groups, rates,
  when / how long, and the wiring (gate, pause/resume in finally, the JNI exception, server screen untouched).
- `tsc --noEmit`: src/ clean. NDK 27 clang `-fsyntax-only` on vibe_localsdr_jni.cpp: clean. Kotlin NOT compiled (no gradle).
- test_nav_coverage, test_platformCopy (175), test_popup, test_release_label, test-bench-defaults: pass.

## Unverified (needs a phone)
- That the RTL reopens on its fd after the run in a LIVE local session (the path "release when idle" uses on servers).
- An SDRplay RSP through OpenRSP's service on Android: release/reacquire inside a local session.
- If the app is backgrounded and foregrounded mid-run, the spectrum socket reopens during the hold and gets the
  "the server is measuring itself" busy reason; whether that line clears by itself after the run.
- Leaving the SDR screen mid-run stops the engine under the benchmark.
