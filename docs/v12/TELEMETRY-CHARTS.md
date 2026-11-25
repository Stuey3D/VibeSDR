# TELEMETRY CHARTS — FUNcube telemetry saved and charted (V12 beta91)

Stuart, 2026-11-22: "how will the funcube telemetry display? will it be saved as a text file to the gallery?" → "that could
be good to have" → "especially for those who background decode it every pass".

★★ **Built and tested on SYNTHETIC frames only.** Nothing here has been checked against a real AO-73 pass. The fixtures
(`scripts/fixtures/fctlm/`) are the server's own writer fed the transmitter's sample frame with its analogue values swung.

## ★★★ beta94 — UNITS, JY1SAT'S OWN LAYOUT, DATA-VALID FLAGS (2026-11-25)

Stuart on a JO-97 pass (Pi 500, 3 good frames): *"charts are useless what is the voltage not raw?"* — every value said
`raw`, and JY1Sat read as FUNcube-1 gave "battery 0" with panel "temperatures" swinging 520 counts in two minutes.

**Sources** (AMSAT-UK's public documents, read for facts; no one's code read or copied):
- FUNcube-1 telemetry equations, `telemetry_equations_b1.xls` (Sept 2013) — linked from
  https://funcube.org.uk/working-documents/project-documents/ :
  https://funcube.org.uk/wp-content/uploads/2010/11/funcube-1_-telemetry_equations_b1.xls
- JY1Sat "data budget with tlm equations 2F" (Nov 2018) — linked from https://funcube.org.uk/2018/11/15/jy1sat/ :
  https://funcube.org.uk/wp-content/uploads/2018/11/jy1sat_data_budget-with-tlm-equations-2f.xls
- ESEO Downlink Data 1.2 §3, the extended satellite id: https://funcube.org.uk/wp-content/uploads/2018/11/eseo_downlink_data_1_21a.pdf
- FUNcube-1 Downlink Data (as flown) — field widths only: https://funcube.org.uk/wp-content/uploads/2010/11/funcube-1-downlink-data-spec_ver1-as_flown.pdf

**Units now** (vibe_fctlm.h `parseFc1` / `parseJy1`): EPS "real values are transmitted" — photo / battery voltages V
(mV ÷ 1000), currents mA, temperatures °C (signed byte); solar panel temps °C (one line per face); bus voltages V (the count
is mV ÷ 4 / ÷ 6 — checked on air: 819 → 3.276 V, 826 → 4.956 V); 3.3 V bus current mA; RF temperature °C (FUNcube-1
−0.857 n + 193.672, JY1Sat −0.98 n + 234.58); receive / transmit currents mA (0.636 / 1.272 a count); PA forward and reverse
power mW (0.005 n^2.0629); PA temperature °C (the sheet's table); PA current mA (0.5496 n + 2.5435); antenna temps °C (the
AntS curve — ★ the count taken as 0–3.3 V, which the sheets do not state; said in the channel's note); sun sensors lux (the
log-lux table; 984 = the sensor's limit); reset cause, PPT mode and iMTQ mode in words; iMTQ uptime h. **Raw**, each with
its own note: receiver Doppler and RSSI (both sheets: "use raw values" / "no conversion required"), JY1Sat's iMTQ error code
("not translated").

**JY1Sat**: satellite id 3 = extended — 16 header bits (byte 1 = S0–S5 F0 F1; JY1Sat's id 0x13 from its 0x10), then its own
432-bit block. Its software block (seq, flags) lands at bit 400 like FUNcube-1's. Confirmed on the 11-25 values (rebuilt in
test-fctlm §12 from the printout): sun sensors where the "panel temps" were, iMTQ MCU temp = 85 (the sheet's "invalid"), EPS
and iMTQ flagged not valid. An unknown extended id shows no values (on the JO-97 pick it is read as JY1Sat).

**Data-valid flags**: a board whose flag is off → its values `!valid`: left out of `sat_tlm` with `"absent":["EPS"]` (the box
says "No EPS data in this frame (its data-valid flag is off)"), BLANK in the pass CSV (the columns stay — no new file), and the
charts get no reading (a gap; a channel with no reading in the pass gets no chart). Charts lead with battery, currents,
temperatures, solar, then the radio; raw channels last and named in the note; each unit has a minimum axis span (V 0.1, °C 2,
mA / mW 10, lux 100) so one count of noise is not a cliff.

**Contract additions** (`sat_tlm`, format `funcube-rt`): `layout` ("FUNcube-1" / "JY1Sat"), `absent` [board names], per value
`unit` (UTF-8 "°C"), `text` (an enumerated value's words), `note` (why raw / how converted). Old clients ignore them.

Tests: test-fctlm 97/0 (§10 the real AO-73 frames in their units; §11 FUNcube-1 known counts → the sheet's values, EPS/PA/ANTS
flags; §12 JY1Sat synthetic + the 11-25 pass rebuilt), test-fctlm-gallery 54/0 (§7 blank cells, "no EPS data", JY1Sat's
columns), test_tlm_charts 64/0, test-web-satellite-mode 145/0, test-web-fctlm-gallery 18/0. The fixtures are now AO-73's real
frame with its readings swung (pass 3 frames 5–7 have the EPS flag off).

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

**The day** — ONE `.txt` per satellite per UTC day on the SERVER, whichever radio or listener decoded (Stuart 11-22: "all
radios on same server should feed the same log for the satellite, it is the same satellite after all"):
`Store::adoptSlot(…, anyWriter)` finds it by the slot `day-YYYYMMDD-<sat>` alone — never the radio, profile, origin or dial.
`14:02:16  AO-73  WO1 · seq 123456 · Sunlight · battery 1234 raw · el 23° · RS fixed 0`. Its title's count is the
file's lines after each append; a restart adopts and counts on.

**★★★ One pass, one day, whoever decodes it** (Stuart 11-22: "only one chart needed regardless of background or user
listened"). A background VFO and a listener's SATELLITE pick may decode the same pass at once (two decoder hosts in one
process). They share ONE `PassFile` (found by satellite + AOS within 2 min; with no AOS on one side, a frame within 20 min
of the pass's last) and ONE `DayFile` (satellite + UTC day) through `fctlmlog::Registry`: one `.csv`, one log, each
frame ONCE — keyed by the frame's sequence number (the satellite's own counter), else its time. Whichever tap flushes
writes; the last tap to leave writes the rest and closes the file. Appends are whole lines in one `O_APPEND` write under
the gallery's exclusive flock, so nothing interleaves; finding-or-creating a day item is serialised in the process.
**Across radio processes** (every radio is its own `vibeserver@<serial>` on one gallery directory): every flush runs
under an exclusive `flock` on `<gallery>/.fctlm.lock` (`FcLock`; the kernel releases it if a process dies). Inside it:
find the item by its slot alone (passes: `pass-<sat>-<AOS minute>`, `-t<start minute>` with no AOS, `-pN` for a later
part; failing that, a pass of the satellite on disk whose last row is under 20 min old — one radio knowing the AOS and
another not — unless both know the AOS and it differs), read the frames the file already holds (the CSV's seq column, the
log's `seq N`), drop ours that are there, append the rest as whole lines in one write, and take the title's count from
the file. The gallery's own index writes are already under its machine-wide `.lock` flock and append-only, so two
processes never lose an index line; since find-or-create is serialised by `.fctlm.lock`, they never make two items.
Cost: reading a ≤ 256 KB pass file and the day log once a minute while a pass is decoded.

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

- `vibeserver/test-fctlm-gallery.cpp` 49/0 — the CSV shape and values, no write before a minute, pass splitting (AOS,
  gap, satellite), the day log across taps / restart / midnight, the 256 KB part, text/csv; §4b a background profile and
  a listener (other origin, profile, dial) alive at once → one day log, no malformed lines, count right, restart finds
  it; §4c the SAME pass from both (one with AOS, one without, each missing frames) → one CSV and one log, each frame
  once; §4d TWO RADIO PROCESSES (fork, each its own Store and radio label on the shared gallery, one with AOS and one
  without, flushing interleaved in real time) → one CSV, one day log, one index item each, the union of frames once, no
  torn lines (49/0, 10 runs in a row clean). Built through run-tests.sh's
  decoder-host group (it needs the host's sources; not a CMake target, like test-lrpt-host). `--write-fixture <dir>`
  regenerates `scripts/fixtures/fctlm/`.
- `scripts/test_tlm_charts.ts` 47/0 (background + listener passes = one history, one series) — parsing, order, live history, geometry, across, gallery items, the loader.
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
- ~~All analogue channels are **raw**~~ — beta94: AMSAT-UK's published equation sheets give units (above).
- ANSWERED 11-22: values list stays the default (CHARTS one tap away); the first eight are fine; ONE day log per
  satellite per day and ONE pass file / chart whoever decoded — every radio on the server included (built as above).
