# beta95 server quick fixes (branch b95-srvfix, 2026-11-26)

Four items from memory `beta95_small_queue.md`, built overnight. All the work is in VibeServer (the shared shim
`android/app/src/main/cpp/local_sdr_shim.cpp` is the Linux daemon AND Android Lite). The Setup page, the web admin page
and the app's server screen were changed only where these items needed it.

## RESUME HERE

| Item | Commit | State |
|---|---|---|
| 1. Rate latch redesign | fb5428a31, 01f72b08d | built, unit + e2e tested (Linux binary; the Android path is the same shim) |
| 2. Satellites vs ADS-B kept running: warn, don't block | 0fe72acb6 | built; admin-line tests; **the tick has not been driven end to end** (it needs orbit data and a pass) |
| 3. One merged community feed at the door | 79f2ca1c5 | built, unit + e2e tested; the directory is **not** changed or deployed |
| 4. Health pill audit + the Sony wide-tap finding | (this commit) | built, unit tested; the wide-tap stall is **findings only** |

Not done / open:
- VDL2 SHARING one 137 MHz capture with passes (map_landing_toggles.md) is a bigger job and was not built. Item 2 only
  pauses and resumes. ADS-B is the only keep-running mode today.
- Gradle was not run for this branch. The JNI and Kotlin are unchanged; the shim compiled in the Linux build.
- The directory can now drop its per-radio `/r/<id>/vibeserver/community.json` reads once servers run beta95. Not done.

## 1. The rate latch (`vibe_rate_guard.h`)

Rules (Stuart 11-25): (a) an update or a deliberate stop is never a crash. The marker carries `build` (the version plus
the compile stamp), so a marker from another build, or one with no build (pre-beta95), counts as an update. (b) One stop
is noted; a second at the same rate within 24 h latches. (c) The latch is one rung down (`rateGuardRungBelow`): 2.56 goes
to 2.4, and a stop above 2.4 is never capped below 2.4. A stop AT 2.4 caps at 2.048, which leaves ADS-B with no lower
rung. (d) 24 h after the latch, the higher rate is tried once (probation). A stop during probation latches until the
owner clears it; 24 h clean forgets it. (e) `say()`/json carry `noted`, `latched`, `ceiling`, `probation`, `retryAt` and
`build`, and Clear forgets everything.

★★ The guard is an OVERRIDE (`vsGuardAllows`) and never writes the owner's `dabRateBoost`, `adsb256`, `ism256` or
`avdata+8m`. The Setup page and the app never grey the switch.

Pre-beta95 records: the old record becomes one noted stop. On Linux, the `dabRateBoost:false` (or `ism256:false`) that
the old latch had persisted is given back (`rateGuardGaveBack`, which main.cpp applies to the config it already read).
On Android the old persist never reached the stored config: the JNI handler keeps only the DAB memory, and the Setup
page changes `radio().dabRateBoost` only on a user's change event. So one restart after the upgrade brings ADS-B back.

Tests: `vibeserver/test-adsb-capture.cpp` (62/62, rules a–e), `scripts/test-rate-guard.mjs` (33/33 e2e: two stalls,
update / noted / latched crash markers, the Sony's Android shape, the Linux give-back),
`scripts/test_capture_and_rate_guard.ts` (17/17).

## 2. Unattended satellites vs ADS-B kept running

`satPassWhyNot` no longer refuses. In `satPassTick`, kept-running ADS-B (`adsbByKeep_`) is not waited for. When a PASS
is due, ADS-B leaves through its own door. `satWantsRadio_` holds `adsbKeepTick` off; the session starts on the next
tick, and at LOS `satWorkEndLocked` clears the hold so the keep tick puts ADS-B back. ORBCOMM monitoring (nearly always
due) runs only beside a pass on such a radio. ADS-B as the radio's only mode refuses the leave; the pass is then not
taken, and the admin page says why. The warning text "a satellite pass takes the radio: ADS-B pauses for the pass, about
10–15 min, and resumes after" appears in four places: `satPassStatusJson.keepWarn`, the web admin (`adminBg.ts`
satPassLines), the Setup page (`spKeepNote`, `adsbKeepNote`) and the app (BackgroundDecodingSetup, the keep hint).
Tests: `scripts/test_admin_bg.ts`.

## 3. The door's merged community feed

Cause: a multi-radio front door owns no radio and took the first radio's options, so `communityLayerOk` used that
radio's blocked modes alone. On the Pi 500 that radio is the RSP1B, which allows DSC only. The data were already the
machine's (gallery, ship store, `aircraft-live.dat`, `.community/live-*`). The fix has four parts:
- The door gets every enabled radio's blocked modes (`setCommunityDoorRadios`, at start and on every save through the
  door) and shares a layer when ANY radio may (`vibecommunity::layerAllowedAnyRadio`).
- Remembered rows are deduped (`rowsDedupe`: one row per key, the newest, at most 4096 looked at).
- A DSC call filed by two radios is one row (same title and position, within 2 min).
- The floor is the existing per-section TTL cache (30 s / 120 s, built at most once whoever asks) plus Cache-Control
  and ETag.

Tests: `vibeserver/test-community.cpp` (63/63), `scripts/test-community-door.mjs` (11/11; the pre-fix binary failed 5),
`scripts/test-community-server.mjs` (40/40).

## 4. The health pill audit

Only what a listener feels stays on the pill: CPU, RAM, temperature, USB, the background "Server CPU overloaded" warning,
and DRM or DAB+ audio decoders dropping input (`vsBehindListenerFeels`). Everything else moved to the admin page:
- Every other decoder losing input (digital spots, Band skimmer, ADS-B, ISM, NDB, AIS, datalink, sondes, VOR map, a
  listener's text decoders, and the wide-IQ slice tap) goes to `adminStatusJson.decodersBehind`, shown as "Decoders
  losing input · …" (`adminBg.ts behindLines`).
- "Background decoding: eased for listeners" was already on the admin page's background line.
- The battery %: the BATTERY card already exists, and listeners still get the VTS warnings before a suspend.
Tests: `vibeserver/test-health-snail.cpp` (behindSplit), `scripts/test_admin_bg.ts`.

### The Sony ORBCOMM wide-IQ tap (findings only, no fix)

The full findings, and the commands the coordinator can run on the Sony, are in the item-4 commit message and the
report. In short:
1. The tap's sink (`DecoderHost::feedIq`) only copies under a short lock, and the decoder thread decodes with that lock
   released. A sink stall is therefore unlikely to be the cause.
2. The tap's queue holds only 0.5 s (`cap = rate * 0.5`), while the host's own queue holds 2 s. Any descheduling longer
   than 0.5 s drops blocks, whatever the average CPU.
3. On ORBCOMM the slice (about 750 kHz) is too wide to decimate at 2.4/2.56 MS/s (`Pre::plan` gives D = 1). So every
   block is a forward FFT the size of the DISPLAY's `fftSize`, plus an inverse FFT of the slice's bins (tens of
   thousands of points at a large fftSize). The tap's cost follows the waterfall FFT setting, not ORBCOMM's needs.
4. The prime suspect is Android scheduling: the tap runs at spectrum priority, and the TV may keep the process on its
   little cores or in a background cpuset. A machine-wide "CPU to spare" says nothing about the core the tap is on.
