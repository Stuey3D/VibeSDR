# TELEMETRY CHARTS — FUNcube telemetry saved and charted (V12 beta91)

Stuart, 2026-11-22: "how will the funcube telemetry display? will it be saved as a text file to the gallery?" → "that could
be good to have" → "especially for those who background decode it every pass".

★★ **Built and tested on SYNTHETIC frames only.** Nothing here has been checked against a real AO-73 pass. The fixtures
(`scripts/fixtures/fctlm/`) are the server's own writer fed the transmitter's sample frame with its analogue values swung.

## RESUME HERE

- Done: server saving (per-pass .csv + day log), shared reading/geometry, app (SATELLITE box + gallery), web (SATELLITE
  box + gallery), tests, release note.
- Next, on air: a real AO-73 pass (background or picked) on a server built from this branch — check the CSV's header and
  rows, the day log, the gallery's FUNCUBE filter, the charts in both clients. Stuart's Pi 500 is the obvious one.
- Not done: the R36S handheld UI (it has no gallery charts; its gallery folder is `FCTLM/` on the card — the CSV and
  the log land there like any item); the Android server's own screen (nothing to show); the app's charts were type-checked,
  not screenshotted on a device (the web charts were).
- Open questions for Stuart are at the bottom.

## What the server writes (android/app/src/main/cpp/vibe_fctlm_gallery.h)

`FcTlmGalleryTap` replaces the old RTTY-style run for `fctlm` (makeGalleryTap). The decoder (vibe_fctlm.h `Run`) hands
every good frame, parsed, to the tap through `DecoderTap::satTlm(Decoded, TlmCtx)`; the decoder host fills the context
from the satellite engine (`decoderView`: AOS, LOS, elevation).

**A pass** — one `.csv` item, kind `fctlm`:

```
# FUNcube telemetry · AO-73 · 145.9350 MHz USB
# Pass from 2026-11-22 14:02:16 UTC · AOS 14:01 · LOS 14:14 UTC
# [raw] = sent uncalibrated: the FUNcube-1 Downlink Data document publishes no calibration. Times are UTC.
utc,unix,frame,seq,mode,elevation [deg],rs fixed,Photo voltage 1 [raw],…,Reboot count,…
2026-11-22T14:02:16Z,1795442536,WO1,123456,Sunlight,23.4,0,512,…
```

- Headings: the channel's NAME, then `[raw]` (uncalibrated — no unit), `[unit]` (a calibrated one, none today), or
  nothing (a count / flag). Names hold no commas; it opens in a spreadsheet as-is.
- Title `AO-73 telemetry · pass 14:02 UTC · 23 frames · max el 54°` (the clients find a satellite's passes by the
  `<sat> telemetry · pass ` prefix); download name `AO-73 telemetry 2026-11-22 1402 UTC.csv` (index `dn`).
- A new pass: a new AOS from the satellite lock; with none (a hand-tuned listener) a gap over 20 min; another satellite;
  another channel set. Past 256 KB, `part 2` (a real pass is ~60 KB).

**The day** — one `.txt` per satellite per UTC day (`Store::adoptSlot`, slot `day-YYYYMMDD-<sat>`, any origin):
`14:02:16  AO-73  WO1 · seq 123456 · Sunlight · battery 1234 raw · el 23° · RS fixed 0`. A restart adopts and counts on.

**Never a write per frame**: both held in RAM, appended once a minute or 8 KB, at a pass's end, midnight and the
decoder's stop. The owner's gallery space bounds the whole (oldest first). `.csv` is served as `text/csv`.

## The clients

- `src/utils/tlmCharts.ts` — shared: CSV → pass, headings, which channels are charted (analogue only; the power system
  first), the live history (in `satTelemetryMerge`, 720 frames, a 20-min gap starts THIS PASS again), the spark geometry
  (broken where 3+ frames are lost), across passes (mean, min–max band), the gallery's pass items and a loader that asks
  for one file at a time.
- App: `src/components/TlmCharts.tsx` (react-native-svg). SATELLITE box: **CHARTS** beside RAW. Gallery: **FUNCUBE**
  filter; a pass .csv opens as its charts + the passes up to it. `galleryReader()` in GalleryOverlay.tsx.
- Web: `web/client/src/tlmChartsWeb.ts` (inline SVG), the same keys in `satellites.ts`, the gallery in `main.ts`
  (`#galCharts`), FUNCUBE in index.html.
- Look: small multiples, one hue (the faceplate's accent / the page's amber), names and values in text inks, a single
  series so no legend; each chart: name / line with the newest point / latest value + range (`1427–1457 raw`). 8 charts,
  `+ N MORE CHANNELS` for the other ~30. 3 columns ≥ 480 px, 2 at phone width.

## Tests (all synthetic)

- `vibeserver/test-fctlm-gallery.cpp` 29/0 — the CSV shape and values, no write before a minute, pass splitting (AOS,
  gap, satellite), the day log across taps / restart / midnight, the 256 KB part, text/csv. Built through run-tests.sh's
  decoder-host group (it needs the host's sources; not a CMake target, like test-lrpt-host). `--write-fixture <dir>`
  regenerates `scripts/fixtures/fctlm/`.
- `scripts/test_tlm_charts.ts` 45/0 — parsing, order, live history, geometry, across, gallery items, the loader.
- `scripts/test-web-satellite-mode.mjs` 109/0 (+13) — CHARTS in the web box, LAST 3 PASSES from a fake gallery,
  + MORE, a new frame, 390 px, dark, no gallery.
- `scripts/test-web-fctlm-gallery.mjs` 16/0 — a vibeserver built from this branch serving the fixture passes + the built
  page: text/csv and the download name, FUNCUBE, a pass as its charts with LAST 2 / LAST 3 PASSES, the day log as text,
  390 px. (`--write-fixture` output was diffed against the committed fixtures: identical.)

## Decisions for Stuart

- Charts open from a **CHARTS** key in the telemetry block (values stay the default view). Should CHARTS be the default
  once a pass has a few frames?
- 8 channels first (battery, battery temp, photo current, system current, photo voltages, a panel temp), the rest behind
  `+ N MORE`. Right eight?
- All analogue channels are **raw** (FUNcube publishes no calibration). If AMSAT-UK's Dashboard calibrations are
  wanted, they would need a source we may use.
- The day log is per satellite AND per background profile (a listener's pick and a background pass with a different
  profile name get separate day logs) — the adoptSlot key includes the profile.
