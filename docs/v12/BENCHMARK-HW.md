# The benchmark, tailored to the radios here (V12, 2026-11-03)

Branch `v12-bench-hw` (from `v12-experimental`). Stuart: *"on the benchmark we need to tailor it for the hardware
installed, no point testing DAB if the user only has an AirSpy HF for instance, and no super wide sample rates for
users with only an RTL."*

## What runs

`vibe_benchmark_hw.h` decides, per row id, from the radios in the config (`main.cpp vsBenchHw`: driver, converter,
direct sampling, locked range). A row runs if **at least one** radio here can use it. Coverage is the server's own
table (`vibebands::driverCoverage`), shifted and clipped by an up/down-converter.

| Row(s) | Runs only where a radio… |
|---|---|
| `wfm_2400` / `wfm_2048` / `wfm_1024` (rate rows) | offers exactly that rate |
| `rdsx_2048`, `zoom_2048`, `lk_wfm`, `auto_wfm` | reaches 88–108 MHz (the feature rows run at the offered rate nearest 2.048 MS/s — an HF+ at 912 kS/s; id kept, label says the rate) |
| `nfm_2048`, `am_2048`, `ssb_2048` | any radio (at the nearest offered rate) |
| DAB (`dab`, `dab_scan`) | reaches a Band III channel at ≥ 2.048 MS/s (`vibedab::radioCanDab`) — its clip is not even fetched otherwise |
| ADS-B (`adsb_*`) | reaches 1090 MHz at ≥ 2.4 MS/s — likewise |
| AIS | reaches 162 MHz and is not an HF+ (`vsAisReach`) |
| RTTY (+AUTO), WEFAX, CW/PSK skimmers, JTTY, SITOR-A, DSC (MF/HF), SELCAL, MFSK, Hell | reaches HF below 24 MHz |
| NAVTEX, NDB | reaches 518 kHz / 190–535 kHz — an RTL only with direct sampling (`vsNdbReach`) |
| time signals, SAME (162.4), APRS (144), paging (138–470), ISM / wM-Bus / LoRa (433/868/915), digital spots (HF, 10 m, 6 m, 2 m) | reaches that band |
| CW, PSK31, SSTV, tone ID, NR, notch, the AM-family listeners | always |
| **wide rows** (`wfm_<kS/s>` full radio, `lr_<kS/s>_<1|5|10>` locked range) | offers ≥ 8 MS/s, or is a USRP whose gated rates (20–32 MS/s) the run must measure — see docs/v12/USRP.md "Wide rates" |

* **No radio configured** (a fresh install) = every standard row, as before, and no wide rows.
* **Never silently dropped:** the JSON's `notRun` lists each skipped row with its reason ("not run — no radio here
  reaches Band III (174–240 MHz) at 2.048 MS/s"); the Setup page shows them dimmed under the table. What a skipped row
  would have set keeps its existing default (benchmark sets the default, never hides).
* `recommendRate` is 0 (none) when no standard WFM rate row ran — an HF+ box is no longer told 1.024 MS/s.
* Bounded on a Pi 2: a Pi 2 has no radio offering 8 MS/s or more except an RSP / HackRF / Airspy, and the wide rows
  stop at the first rate the machine cannot carry (one rate's rows, then "not run — … could not keep up at …").
  Measured: an HF+-only run takes 8.6 s at 2 s a row on the Mac.

## Also fixed

* **The Mac counted one thread.** `benchdetail::threadCpu` read `/proc` only, so on macOS every row was the feeding
  thread alone — pipeline workers, demod and Opus invisible. It now uses the decoder rows' Mach reader.

## Not changed

* Android / Lite (`vibe_localsdr_jni.cpp`) and the app's `ServerModeScreen` still run the full list: their local radio
  set is not passed in yet. Follow-up: pass the phone's attached radio the same way.

## Checking it

* `vibeserver/build/test-bench-hw` — 53 checks (HF+ only, RTL only, RTL + direct sampling, USRP USB 3 / 2 / unknown,
  HF+ + RTL, Airspy R2 ± converter, RSP, the rate gate, 82–108 MHz).
* `VIBE_BENCH_RADIOS="airspyhf" VIBE_BENCH_OFFLINE=1 vibeserver-bench 2 - - -` — the tailored run for a fake set.
* `VIBE_BENCH_RADIOS="usrp:3:locked" VIBE_BENCH_WIDE_ONLY=1 vibeserver-bench 4` — the wide rows alone
  (`VIBE_BENCH_WIDE_RATES=30720000` for one rate).
