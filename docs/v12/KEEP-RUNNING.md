# Keep-running modes beyond ADS-B: the landing map (branch b96-keeprun, 2026-11-26)

Stuart, on the Sony's Setup page (11-26): *"I thought I'd be able to set VDL2 as the Sony's background mode"*. beta95 had one
answer, "Use ADS-B as the landing page" + "Keep ADS-B running when nobody is connected". This branch generalises it, as
memory `map_landing_toggles.md` specifies.

## RESUME HERE

| Piece | Where | State |
|---|---|---|
| The model: modes, CSV, entry, migration | `vibe_keep_store.h` (no DSP; the config reader uses it) | built, unit tested |
| The rate gate, the shared 137 MHz fit, cost words | `vibe_keep_run.h` | built, unit tested |
| Config: `landingMap` / `landingModes` / `mapKeep` per radio | `vibeserver_config.*`, `main.cpp` | built, tested (both writers, the flatten, the merges) |
| Kept mode in the shim (VDL2, ACARS, ACARS + VDL2, AIS, VOR map) | `local_sdr_shim.cpp` `mapKeepTick`, `keepHolds()` | built, **e2e tested** on the Mac binary (fake rtl_tcp) |
| Landing a new listener in the map's mode | connect path, `landOnMap` | built, e2e tested |
| Shared 137 MHz capture between passes (ORBCOMM sky plot beside kept VDL2) | `keepVdl2Centre`, `keepOrbRiderTickLocked` | built; the CENTRE is e2e tested; **the sky-plot rider has not run** (needs orbit data + a bird up) |
| A Meteor / NOAA pass sharing kept VDL2's capture | `satPassTick` step 5, `avRiderSync`, `satIfTick` | built; **not driven end to end** (needs orbit data + a pass) |
| Kept VDL2's narrow IF filter on an RTL | `applyAutoIf` | built; **not measured on air** |
| hwinfo `mapModes` / `mapCombo` / `mapRate` | `vsMapModesJson` | built |
| Admin line `keepRun`, satellite `keepShared` / `orbRider` | `keepRunJson`, `adminBg.ts keepRunLines` | built, unit tested |
| Setup page card | `vibe_setup_page.h` `mapLandWrap` | built, tested against stand-in elements |
| Android: bridge + engine | `vibe_android_setup.h`; the engine reads the keys off the config JSON (`vsMapFromConfigJson`) — **no new JNI** | built (the shim compiled on the Mac), test-android-setup passes; **gradle not run** |
| App's server screen | `ServerModeScreen.tsx` + `src/utils/keepRun.ts` | built; esbuild syntax OK; **not type-checked** (tsc on the screen needs > 1 GB) |
| Copy | About beta96 note (+ the old ADS-B note pointed at the new control) | done; tours do not mention keep-running (checked) |
| `vibe_web_page.h` | not changed: the only web-client change is `admin.ts` / `adminBg.ts` | **regenerate at integration** (the admin page ships in it) |

## ★★ The AMATEUR map: ISM (coordinator / Stuart 11-26, added on this branch)

*"keep-running must also offer ISM (868 MHz and 433 MHz bands, incl. LoRa: MeshCore / Meshtastic / LoRaWAN) under the AMATEUR
landing map, so a radio can run 868 ISM for MeshCore repeaters continuously and feed the community map's MeshCore layer."*
Stuart's estate plan: Sony → VDL2; a Pi 500 RTL-SDR V4 → 868 ISM for MeshCore; ADS-B from a PiAware Beast feed.

- `landingMap` "amateur", `landingModes` "ism:<band>" — the band id from ISM's own plan (`vibeism::bands()`: 868, 433, 915 …),
  offered only where this radio reaches it (hwinfo `ismHwBands`); the page and the app pick 868, else 433, else the first.
- The kept mode enters ISM exactly as its key does (`{"type":"ism","on":1,"band":"868"}`), so LoRa coverage is ISM's own
  setting (the "every channel" tick under ISM in Modes offered). Nothing here records or offers IQ of ISM bursts — the
  standing policy (no IQ capture/download of ISM, ever) is untouched.
- One mode at a time (a radio has one landing map; Amateur has one mode). A listener's band change or custom frequency is
  somebody's use; the keep puts its band back when they leave. ISM blocked → greyed with why. Cost: `dec_ism`.
- A satellite pass pauses kept ISM and resumes it (beta95's rule).
- Tested: test-keep-run §7, test-setup-keep-run (the Amateur card), test_keep_run, and **e2e** (test-server-keep-run 7b: kept
  ISM 868 entered at start, a client gets ISM reports, the last listener leaving does not take it away).
- Live check owed: the Pi 500 V4 on 868, MeshCore repeaters on the community map with nobody connected.

## What the owner sees

Setup page, **Where new listeners start**: *"Start new listeners on a **map**?"*, then the map (Aviation & Maritime, or Amateur). On
Aviation & Maritime the radio's modes are listed under AVIATION (ADS-B, VDL2, ACARS, VOR / ILS map) and MARITIME (AIS), each with its setup-benchmark cost.
The facts come from the SERVER (`hwinfo.mapModes`): a mode this radio can never do is not drawn (an HF+ has no ADS-B or VOR
row); one it cannot do now is greyed with the server's reason (blocked, the rate). The form's own rate and blocks apply on
top, so the gate follows edits before they are saved.

- At an ordinary rate: *"2.4 MS/s detected: you can choose one of these modes."* Ticking one greys the others: *"2.4 MS/s: one
  mode at a time on this radio — VDL2 is ticked"*; ACARS says *"ACARS and VDL2 together need a capture of 6.1 MS/s or more"*.
- Where the capture holds the datalink's wide band (~6.083 MS/s, or the owner's 8 MS/s "avdata+8m" switch): *"8 MS/s
  detected: one mode at a time, or ACARS & VDL2 together"*.
- Beneath: *"Keep VDL2 running when nobody is connected"* (never ticked by default) and a note with the CPU cost and the
  satellites rule.
- ✗ **AIS + DSC is not offered.** DSC is a VFO decoder (background decoding's), not a whole-radio mode, so a combined tick
  could only half-work (AGENTS.md). The model takes a combination only where one whole-radio mode decodes both.
- The beta95 ADS-B controls are still in the page, hidden; their rule (`adsbLandOffer`) still decides the ADS-B row.

Picking VDL2 is the whole setup: the radio goes to `vibeavi::bands()` "vdl2", 136.8625 MHz, every 25 kHz channel
136.700–136.975. Nothing is typed.

## Migration (unchanged ADS-B)

`vibekeep::applyRead` / `migrate`: a radio with beta95's `landingAdsb` (and `adsbKeepRunning`) and no `landingMap` reads as
the map + "adsb" (+ keep). The writer writes all five; the ADS-B pair is mirrored only for the ADS-B case, so an older build
reading a VDL2 config sees ADS-B off and simply idles (it never lands anywhere it does not know). An older page or app saving
only the old words: "ADS-B off" clears an ADS-B landing but never a VDL2 one it cannot know about. Same rules in the Android
bridge, the Setup page (`keepMigrate`) and the app (`keepMigrate` in `utils/keepRun.ts`). The e2e test's §8 runs a beta95
config on the new binary: kept ADS-B as before.

## How it runs (server)

- `setVibeServerMap(map, modes, keep)` sets the map's `vibekeep::Entry` (ADS-B stays with `setVibeServerLandingAdsb`). Linux:
  `main.cpp` (start, reload, the door's re-read). Android: `VibeServerBoot` already passes the whole config JSON to
  `setVibeServerFmDefaults`; the engine reads the three keys there. Kotlin's private DAB scan and rtl_tcp configs PUT the keys
  empty so nothing stale outlives them.
- `mapKeepTick` (housekeeping, after `adsbKeepTick`) is kept ADS-B's pattern: nobody here → the owner's plain landing → the
  mode's own door (`entryFor(...).msg`, e.g. `{"type":"avdata","on":1,"band":"vdl2"}`). Retried once a minute if refused.
- `keepHolds()` = kept ADS-B or the map's kept mode — read by every idle decider (park, release, the RSP probe, background
  decoding, the DX scanner, an Icom's power-off).
- The last listener leaving does not take the kept mode away (`wholeModeLeavesWhenEmpty`); a different band or mode a listener
  chose leaves as ever and the keep puts VDL2 back. `mapByKeep_` marks "the keep's, nobody's use of it" (the resume and the
  passes read it).
- A new listener lands in the map's mode (ADS-B's landing rules: the first arrival after a start always, later ones unless
  admin / pre-tuned / a shared dial). Kept, the radio is already there: they join.

## The shared 137 MHz capture

Stuart 11-25: VDL2 136.7–136.975, ORBCOMM 137.2–137.8 and Meteor 137.1 / 137.9 fit one 2.4 / 2.56 MS/s capture;
*"meteor will need its filter widened in background mode"*; *"build the heatmap alongside vdl2 and meteor"*.

1. **Between passes.** With ORBCOMM monitoring ticked (Unattended satellites) kept VDL2 sits where VDL2 and the ORBCOMM band
   both fit (`vibekeep::shareFit`: 137.179 MHz at 2.4 MS/s, the DC 10 kHz clear of the VDL2 block) — still all 12 channels.
   The light ORBCOMM sky-plot watcher rides that capture (`keepOrbRiderTickLocked`): no session, the radio is not taken, fed
   from the datalink's own DSP branch, stopped when the capture no longer holds the band (a listener's band step).
2. **A pass.** A Meteor / NOAA pass due on a kept VDL2 radio whose capture holds the pass + VDL2: the datalink MODE steps
   aside (the pass needs the ordinary demodulator — its decoder is the pass's), but its **VDL2 service rides the session's
   capture** (`avRiderSync`, fed from the ordinary DSP path, restarted if the capture moves). The scheduler holds VDL2 in every
   fit (`satsched::In.hold`), with ORBCOMM too where all three fit, else ORBCOMM pauses. The pass's IF filter regions hold the
   VDL2 channels too (`satIfTick`), so on an RTL the filter OPENS over ~136.69–138.0 MHz for the pass and, when the session's
   regions go at LOS, closes again. Once a minute: *"shared capture check: VDL2 N messages, x % of samples near full scale
   [— OVERLOAD], gain, IF, blocks dropped"*. At LOS the keep tick re-enters the datalink mode. The cost: a few seconds of
   VDL2 at each hand-over.
3. **Where it cannot fit** (an HF+), VDL2 pauses for the pass and resumes after (beta95's warn, don't block).
4. **Kept VDL2's filter on an RTL** narrows to the VDL2 channels (500 kHz rung at 136.8625; 1.4 MHz with the ORBCOMM band),
   so the 2 m band and the airband's voice channels stay out of the 8-bit converter all day. Other radios keep the
   whole-capture width, as before.

## Tests (all silent)

| Test | Result | What |
|---|---|---|
| `vibeserver/test-keep-run.cpp` | 95/95 | table, CSV, entries; migration; the gate at 2.048 / 2.4 / 2.56 / 8 MS/s; multi-radio (the Pi 500 shape); the shared fit (Meteor 137.9 / 137.1 at 2.4 and 2.56, NOAA, 2.048, an HF+); the scheduler's hold; cost words |
| `vibeserver/test-sat-pass-sched.cpp` | 43/43 | unchanged behaviour with no hold |
| `vibeserver/test-config-radios.cpp` | 278 checks, 0 fail | the three fields over every hop; beta95 migration; old writer vs new |
| `vibeserver/test-android-setup.cpp` | all passed | the bridge's identity round trip with the new keys; the mirror; a beta95 phone config on the page |
| `scripts/test-server-keep-run.mjs` | 32/32 | **e2e on the Mac binary**: kept VDL2 at start, listener joins, EXIT to the plain landing, back after leaving, a band step then the hand-back, landing-only, negative, the shared centre, ACARS + VDL2 refused at 2.4 with the reason, AIS kept, a beta95 ADS-B config |
| `scripts/test-setup-keep-run.mjs` | 48/48 | the page's real functions on the C++ cases; the card for an RTL at 2.4, an RSP at 8, an HF+, an Icom |
| `scripts/test_keep_run.ts` | 28/28 | the app's rule, same cases |
| `scripts/test_admin_bg.ts` | 63/63 | the admin keep-run line, the shared / rider satellite rows |
| `check-android-settings-parity`, `check-setup-page`, `test-setup-adsb-landing` | clean | |

Not tested here: a real pass (orbit data), the ORBCOMM rider running, the narrow IF on air, Android gradle, the app's types.

## Live checks owed (the coordinator, on Stuart's say-so)

1. The Sony (Android, RTL at 2.56): Setup → Map → VDL2, keep running. Expect the admin line "Aviation & Maritime · VDL2 kept
   running · running", the datalink map populated for a newcomer, the community map's aircraft unchanged.
2. A Meteor pass on that radio with Meteor + ORBCOMM ticked: the log's "VDL2 rides the pass's capture …", the once-a-minute
   "shared capture check" (watch for OVERLOAD with the wider filter), VDL2 messages during the pass, the image as good as a
   pass without VDL2, and "VDL2 … riding the pass's capture — it resumes now" at LOS.
3. Between passes: "ORBCOMM monitoring rides kept VDL2's capture (137.1790 MHz …)" and the sky plot's dots growing.
4. Kept VDL2's 500 kHz / 1.4 MHz IF on an RTL: VDL2 message rate against the whole-capture filter (A/B an hour each).

## Open decisions for Stuart

- AIS + DSC "together": DSC is background decoding's VFO, not a whole-radio mode — not offered. Make a combined AIS + DSC
  whole-radio mode later, or leave DSC in background decoding (it cannot run while a mode is kept)?
- Kept VDL2's narrow IF filter applies on RTLs only, and only while KEPT (a listener-entered VDL2 keeps the whole capture).
- The Amateur map offers ISM only; MeshCore/LoRa follow ISM's own settings. Other Amateur modes (APRS, spots) are VFO decoders, not whole-radio modes — not offered.
- Landing without keep-running for VDL2 / AIS works like ADS-B's (a new listener lands in it); OK, or keep-only?
