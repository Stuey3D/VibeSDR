# The benchmark holds the whole machine still (2026-11-20)

Stuart: *"everything should stop for benchmark runs otherwise they are invalid"*. The benchmark sets defaults and user
limits, so a figure measured beside live work is wrong.

## What went wrong
The Pi 2 has one RTL and two systemd units: `vibeserver.service` (the front door, which serves the setup page) and
`vibeserver@00000001` (the radio's own process). The QUICK benchmark is started from the setup page, so it ran **in the
front door**. Three things followed:

- `releaseRadio()` released nothing, because the door owns no radio.
- The listener check counted only the door's listeners.
- The radio process carried on with its idle DX scanner (a DAB band sweep, about two thirds of a core) through the run.

The locked-capture figures came out at 78.7 % on the hottest thread with 366 drops. So every locked-range profile read
"0 WFM users (the capture itself cannot keep up)", even though the same run measured a plain WFM listener at about 60 %
of a core.

## The design (`vibeserver/vibe_bench_quiet.h`, `main.cpp` vsBenchQuietMachine)
- **Coordinator.** The process that takes the run coordinates it. That is the door for a quick run, or a radio's own
  process for a wide one. It talks to every other enabled and configured radio process on `127.0.0.1:<portForRadio>`.
  Radio ports behind a door answer this machine only. Each request goes to the existing admin-gated
  `/vibeserver/benchmark` endpoint and carries `vs_admin_ticket`, a ticket signed with the machine's shared admin secret
  (`LocalSdrShim::machineAdminTicket`). There is no new open endpoint, and a request without proof gets a 401.
  - `GET  …?hold=check`: replies `{"hold":"check","listeners":n,"busy":bool,"held":bool}`. Nothing changes. An older
    build replies with its saved result (no `"hold"` key), and the run is refused rather than measured beside it.
  - `POST …?hold=on[&force=1]`: quiets that process for a lease of 90 s. It refuses if there are listeners (unless
    forced), if a benchmark of its own is running, or if it cannot be quieted.
  - `POST …?hold=renew`: the coordinator sends this every 20 s.
  - `POST …?hold=off`: takes the radio back (`vsBenchBringBack`, with its retries and restart fallback) and ends the hold.
- **Order.** The coordinator works in four steps:
  1. Check every peer. Listeners are summed across all radios, and a refusal names each radio that has listeners.
  2. Claim the run.
  3. Hold every peer. If any peer refuses, roll back the ones already held.
  4. Quiet its own radio.

  If anything fails, the run never starts and the sentence names the radio and the reason. "Run it anyway?" (force) is
  offered for a listener refusal only.
- **Wake-up.** Wake-up runs whatever happened in the run (try/catch around the measurement). The other radios are let go
  first, then this process's radio is taken back, then `benchUnhold`.
- **Lease.** The lease is the safety net. If a coordinator dies or is restarted mid-run, each held radio brings itself
  back when its lease runs out.
- **Missing radios.** A radio whose radio-status file says its process is not running (`notfound` / `openfailed`) costs
  the machine nothing, so it is skipped and the log says so. Any other silent radio stops the run.

## In each process (`LocalSdrShim::benchQuiesce` / `benchUnhold`, `g_benchHold`)
- The hold goes up first. The DX scanner (`dxStop`, with its dial given back) and background decoding (`bgStopLocked`)
  stop while the radio is still held. Then the radio is released.
- A radio that cannot be released has its capture paused instead. That covers an rtl_tcp source (its reader stops) and an
  Android descriptor radio (`releaseRadio` parks it).
- While the hold stands:
  - `dxWhyNot` returns "the benchmark is measuring this machine".
  - `bgTick` stops everything and returns.
  - `adsbKeepTick` returns.
  - `resumeCaptureIdle` never takes the radio back. This covers the idle work and an arriving listener; the listener is
    told "the server is measuring itself".
- One-process servers (Simple mode, the Mac) go through the same path with no peers.
- **Android** (`nativeRunBenchmark`): it used to measure beside a serving radio. Its comment said "the caller stops the
  radio", but no caller did. Now it quiets the radio, refuses with words while anyone is listening, and takes the radio
  back afterwards.

## The page and the app (`vibe_setup_page.h`, `ServerModeScreen.tsx`, one rule, two readers)
- A capture-bound profile now reads "WFM @ 2.4 MS/s: the shared front end (one capture for every listener) did not keep
  up at this rate here, so there is no listener figure". It never reads "0 WFM users".
- The quick block's heading is "Quick benchmark — locked range (one capture shared by every listener), CPU/RAM:". On a
  machine where no radio runs as a locked range, the page adds that these figures apply when one does.

## Tested
- `vibeserver/test-bench-quiet.cpp` (ctest `bench-quiet`): the lease, the parsing of a peer's answer, and the sentences.
  20/20 pass.
- `scripts/test-bench-quiet.mjs` (in run-tests.sh): a door and two radio processes on fake rtl_tcp. The RTL runs a busy
  FM DX scanner, and the measurement is stubbed to 8 s. 33/33 pass. It covers:
  - A radio that does not answer refuses the run by name, and nothing is touched.
  - A quick run from the door holds both radios, with no DX hop or sweep inside the window, then both come back and the
    scanner resumes.
  - A wide run in the RSP's process holds the RTL.
  - The lease runs out and the radio brings itself back.
  - A 401 without proof.
  - A listener on the RTL refuses a run started at the door. Forced, the run goes ahead.
  - A one-process server.
- Existing suites still pass: `test-server-bench-wide` 18/18, `test-bg-decode-frontdoor` 12/12, `test-setup-bench-wide`,
  `test-setup-bench-stale`, `test-bench-defaults`, `test-dx-scan` 57/57 (its DAB part not run: no 12D clip here). The real quick run end to end (`test-server-bench-dab-rate`, heavy) was not run.

## Not verified
- Real USB radios. Release and take-back on RTL, RSP and HF+ is the existing, proven `releaseRadio` / `vsBenchBringBack`
  path, but the hold around it has not been run on hardware. The Pi 2 rerun is the coordinator's job.
- Windows: `SO_RCVTIMEO` takes a DWORD there. The code compiles in shape but was not built.
- Android: the JNI change was not built (no NDK build here).

## RESUME HERE
Merge `v12-benchquiet` into v12-experimental. Then rerun the quick benchmark on the Pi 2 with the DX scanner on. The
radio log should show "benchmark hold: idle work stopped and the radio released", then "benchmark hold ended", with no
DX hop between them. The locked-range rows should no longer be capture-bound because of the scanner.
