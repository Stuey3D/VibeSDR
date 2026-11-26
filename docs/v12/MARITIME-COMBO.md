# MARITIME: AIS + DSC channel 70 from one wide capture (beta96, branch b96-maritime)

Stuart, 11-26: *"do this for HackRFs, SDRplay etc"* — the ACARS + VDL2 combination's idea for the sea. AIS (161.975 /
162.025 MHz) and VHF DSC channel 70 (156.525 MHz) are 5.5 MHz apart; a radio whose capture holds both decodes both at once.

## RESUME HERE

**★★★ b96-appserver2 (2026-11-26) THE RATE BOOST IS BUILT** (Stuart: an owner switch like the datalink's "avdata+8m"):
**`ais+8m`**, a "+" token IN the CSV when ticked, under AIS in Modes offered to listeners — "MARITIME: AIS + DSC together
(raise the capture to 8 MS/s)". Offered only where the radio can (vibe_radio_caps.h: 8 MS/s listed AND 156.51–162.04 MHz in
reach; dropped without AIS or DSC) — an RSP, an Airspy R2, a HackRF, a USRP; never an RTL, the HF+ or an Airspy Mini.
Server (local_sdr_shim.cpp): `vsAisDscBoostRate` (vibeavi::wideBoostRate: switch, not pinned, the rate guard allows it) →
MARITIME offered at any running rate (`vsAisDscFitsOrBoosts` in WhyNot, mapModes, caps); `aisApplyRate` raises to 8 MS/s on
entering AIS + DSC (the guard's marker first, "MARITIME"), gives the owner's rate back on AIS alone (retarget) and on leaving
(before the locked centre returns); `g_aisBoostFrom` (never persisted; cleared by endWholeRadioModesForStop); the AIS report's
`rateNote`. Web Setup + app: the row and its notice (`AIS_8M_NOTICE`, one text two readers), the keep / landing rule follows
it (web keepFacts; app keepAvailLocal `ais8mOk` from the radio's caps). Default OFF, no benchmark auto-tick (unlike
avdata+8m — a rate change moves every listener; say if you want dec_aisdsc to tick it like dec_avboth). Tests:
**test-server-maritime.mjs §5 (19 new, 50/50)**: RTL caps never; 2 MS/s switch off → not offered; on → offered, enters at
159.2625 MHz / 8 MS/s, rateNote, log, AIS alone → 2 MS/s back, back to 8, leave → 2 MS/s + 96.1 MHz; pinned → never.
**test-radio-caps 285/0**, **test_keep_run.ts 60/0**. Not run on a real RSP.

| Piece | Where | State |
|---|---|---|
| The combination: geometry, rule, front end | `android/app/src/main/cpp/vibe_maritime_combo.h` (`vibemar::`) | built, unit tested |
| AIS service's pre-front hook (type-erased; AIS-only builds carry no DSC) | `vibe_ais_service.h` `PreFront`, `start(…, pre)` | built, unit tested |
| AIS worker at DECODER priority (it had none) + queue capped at 4 M samples | `vibe_ais_service.h` | built |
| Shim: `{"type":"ais","on":1,"band":"ais+dsc"}`, enter / retarget / leave, `g_aisDsc` | `local_sdr_shim.cpp` `aisControl`, `aisEnter(dsc)`, `aisRetarget`, `aisMakeCombo` | built, **e2e tested** |
| Reach / rule | `vsAisDscTunes`, `vsAisDscWhyNot`, `vsAisDscOffered` | built, e2e tested |
| Capability `aisDsc` (+ `aisDscOn`), absent where not offered | vibeserver.json caps (beside `ais`) | built, e2e tested |
| Channel 70's calls → gallery (DSC log, distress / urgency / positioned pins) → SHIPPING / AIR & SEA DSC layer, community map | `aisMakeCombo` → `vibe::bg::makeGalleryTap(kind "dsc")` | built, e2e tested (gallery) |
| AIS report: `combo`, `hw`, `dsc:{hz,calls,syncs,recent[≤20]}` | `ComboFront::reportFields` | built, tested |
| Landing map / keep running: mode `aisdsc` ("AIS + DSC") | `vibe_keep_store.h` modes(), `mapModeOnNow`, `mapWhyNot`, `vsMapModesJson` (`hw`, `minRate`, `tunes`) | built, unit + e2e tested |
| Single mode `aisdsc` (stands on AIS **and** the DSC switch) | `vsEnforcedBlocked`, `singleBlockOf` | built, unit + e2e tested |
| In-process restart (endWholeRadioModesForStop covers it: it is the AIS mode) | `vibeserver/test-keep-restart.cpp` phase 3 | see Tests |
| Benchmark row `dec_aisdsc` ("AIS + DSC together", 8 MS/s, hottest thread; shown beside AIS, never primary) | `vibe_benchmark_more.h`, `_hw.h`, `vibe_benchmark.h`, setup page label | built; **not run in the setup benchmark here** |
| Web: MARITIME key (mode menu + Tunes the radio itself), AIS status "DSC ch 70: N calls" | `main.ts`, `mobile.ts`, `spectrum.ts`, `tracks.ts` | built (esbuild + build-web's type gate) |
| Setup page: "AIS + DSC (channel 70)" under MARITIME; single mode "MARITIME: AIS + DSC (channel 70)" | `vibe_setup_page.h` KEEP_MODES, keepFacts, singleOptions | built; test-setup-keep-run 73/73 (8 new) |
| App: MARITIME key, keep card + single mode (own engine) | `ModeSelector.tsx` marKey, `SDRScreen.tsx` onMar, `VibeServerClient.ais(on, band)`, `tracks.ts`, `utils/keepRun.ts` | built; esbuild syntax OK; **not type-checked**; `test_keep_run.ts` 42/42 |
| R36S handheld | — | **not done** (its radios are RTLs: the combination would never be offered there) |
| Copy | About beta96 note; KEEP-RUNNING.md | done; tours mention no AIS location (checked); website has no AIS copy |
| `vibe_web_page.h` | rebuilt via the lock (build-web's type gate clean) | regenerate again if integration changes web/ |
| Android | the shim, NDK 27 clang `-fsyntax-only` for arm64 with the gradle build's defines; the combination's test TU for arm64 + armv7 NEON | clean; **gradle not run** (no RN node_modules in the worktree) |

## How it works

- **Where the radio sits:** 159.2625 MHz — halfway between channel 70 and AIS's own centre (162.000), so the DC remnant is
  2.74 MHz from both and both sit well inside an RSP's 8 MHz IF filter.
- **The rule (`vibemar::fits`):** the running capture's USABLE span ≥ `minRateHz()` = 2 × max distance to channel 70 − 12.5 kHz /
  AIS 2 + 12.5 kHz, ÷ 0.9 (the datalink bands' 10 % edge rule) = **6.17 MS/s**. Offered: RSP at 8, Airspy R2 at 10, HackRF
  8 / 10 / 20, USRP 8+. Not: RTL (any rate), HF+, RSP at 2 / 6, Airspy Mini at 3 / 6, R2 at 2.5. No rate change: the mode runs
  at the rate it finds, as AIS does (an owner who wants it on an RSP sets 8 MS/s).
- **The front end is the datalink's** (`vibeavi::MultiChannel`, `vibe_avi_front.h`): half-band stages (NEON / SSE2 / scalar)
  only where the capture is wider than needed (20 MS/s → 10), then ONE forward FFT a block (PFFFT, 4096 points), and two
  slices of ~250–312 kS/s: AIS's (centred on 162.000, flat to ±62 kHz: both channels, their filters and the ±12 kHz AFC) fed
  to AIS's own `Front` — the same receiver, AFC and second-witness rule — and channel 70's, cut to ~41.7 kS/s by a
  `vibedsp::FirDecimator` (NEON / SSE2), FM-discriminated and linearly interpolated to exactly 48 kHz for the existing VHF DSC
  receiver (`vibe_dsc_run.h`) — what a listener's NFM decoder hands it. No new channeliser.
- **Threads:** all of it on the AIS service's one thread, now at decoder priority (`vibe-maritime`); it drops its OLDEST block
  past half a second (≤ 4 M samples, 32 MB) and counts it. The mode sends no audio. Network > Audio > Spectrum > Decoders.
- **Data:** ships into the same list, ship store and gallery as AIS alone. Channel 70's calls into the gallery's DSC log
  ("DSC 156.525 MHz · …") and items (distress / urgency, and since beta88 any call with a position, pinned) — which the
  SHIPPING / AIR & SEA map's DSC layer and the community map already read. A transmission's repeats are one call. The AIS
  report carries the count and the last 20 calls for the status line.
- **Switching:** the MARITIME key enters (or widens AIS in place: `aisRetarget`, ships kept); the AIS key while MARITIME runs
  goes back to AIS alone in place; pressing the lit key leaves. Background decoding's AIS profiles stay AIS alone.

## Tests (all silent)

| Test | Result | What |
|---|---|---|
| `vibeserver/test-maritime-combo.cpp` (run-tests.sh) | 50/50 | the rule per radio (16 cases); slices inside at 6.25 / 8 / 20 MS/s, the half-band only at 20; **4 ships on both channels + a DSC distress alert decoded at 8 and 20 MS/s** (synthetic, in noise); through `vibeais::Service` on its thread in real time (report `combo`/`dsc`, the gallery hook, tick, finish, no drops, restart as AIS alone); 20 s of noise → 0 AIS frames, 0 DSC calls; CPU |
| `vibeserver/test-keep-run.cpp` §9 | 118/118 overall | entry, CSV, a bad pair, the gate (RTL: not drawn; RSP 8: drawn, one at a time with AIS; DSC off: greyed), single-mode token / switch / enforcement |
| `scripts/test_keep_run.ts` | 42/42 | the app's local rule: RTL / RSP 6 / Mini 6 not drawn, RSP 8 / HackRF 20 / R2 10 drawn, DSC off greyed, single-mode list |
| `scripts/test-server-maritime.mjs` (run-tests.sh) | 31/31 | **e2e on the Mac binary** (fake rtl_tcp at 8 MS/s with an RSP's rate list — `VIBE_TEST_TCP_RATES`, tests only — replaying `test-maritime-combo --write-clip`): RTL not offered + refused; 8 MS/s offered, centre 159.2625, 4 ships + 1 call, gallery distress pin + day log, AIS alone in place, back, leave; kept at start; single mode |
| `vibeserver/test-keep-restart.cpp` phase 3 (`scripts/test-keep-restart.sh`) | 7/7 | kept AIS + DSC at 8 MS/s back after every IN-PROCESS restart (with kept VDL2 ×3, ISM ×2) |
| `scripts/test-setup-keep-run.mjs` | 73/73 | the page's real functions: RTL not drawn (card nor single list), RSP 8 drawn + one choice + keep label + single mode, DSC off greyed, the form raised to 8 MS/s draws it |
| `scripts/test_ais_aprs_app.ts` (web = app tracks) | 181/181 | unchanged behaviour with the report's new fields |
| `test_admin_bg.ts` 63/63, `check-setup-page` clean, `check-android-settings-parity` 211/1 | | the 1 = b96-newmodes' `newModesPolOn/Off`, pre-existing on b96-integrate |

## CPU

Measured on the Mac (Apple M5 Pro, one core, the decoder's own single thread, test-maritime-combo §5):
AIS alone @ 2.4 MS/s **0.7 %** · AIS + DSC @ 8 MS/s **2.3 %** · @ 10 MS/s **2.8 %** · @ 20 MS/s **3.9 %**.
**Estimates, not measured:** a Pi 4 (Cortex-A72 1.5–1.8 GHz) ~8–10× an M5 core on this float / FFT work → **~20–25 % of one
core at 8 MS/s**; a Pi 5 (A76 2.4 GHz) ~3–4× → **~8–10 %**. On top of that is the radio's own 8 MS/s ingest (USB, conversion,
spectrum), which any 8 MS/s use of the radio pays. The `dec_aisdsc` benchmark row measures it on the box (hottest thread).

## Live checks owed (the coordinator)

1. An RSP at 8 MS/s near the coast (or a HackRF): MARITIME on, ships as with AIS alone (compare an hour each), channel 70
   calls in the gallery (DSC 156.525 MHz log). VHF DSC has never been heard off air here (DSC.md) — the first real call is
   the check of the FM path's level and sense.
2. The `dec_aisdsc` benchmark row on a Pi 4 / Pi 5 with an 8 MS/s radio.

## Open decisions for Stuart

- ~~No rate boost~~ — built 11-26 (`ais+8m`, above). Should the benchmark's `dec_aisdsc` tick it on a green box, as `dec_avboth` does for avdata+8m?
- The landing card lists "AIS + DSC (channel 70)" as its own row beside AIS (one choice), not a tick of AIS plus a tick of DSC.
