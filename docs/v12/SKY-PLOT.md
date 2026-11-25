# THE SKY PLOT (V12, 2026-11-24)

Where in the sky this station's aerial actually hears satellites. A polar dome — outer ring the horizon, centre the
zenith, **north up, east right** (a map's view, as GPS sky plots are drawn), rings at 30° and 60° — with one dot per
decode at the satellite's azimuth / elevation at that moment, coloured by SNR. It **accumulates across passes**, so after
a day or two of ORBCOMM (up almost all the time) it shows the aerial's real coverage: a YouLoop's nulls, a vertical's
overhead hole, a QFH's even dome, a building in the way to the north-east. Grey dots = the picked bird was tracked and
heard earlier in the pass, but nothing decoded there — the blocked directions are the point.

## RESUME HERE

Branch `orbcomm`. Server built and tested (synthetic); the clients' views by the helper agent on the same branch.
ORBCOMM feeds it; the other satellite decoders are designed in (kinds) but not wired yet.

## One file per server, filtered by radio

The logs' rule (docs/v12/LOG-RADIO-FILTER.md): every radio process appends to **`<gallery>/skyplot.dat`** under flock
(`.skyplot.lock`); each dot carries its radio's id (`vibe_log_radio.h` — eight hex characters, never a serial). A sky
plot is about an AERIAL, so every view filters by radio (`?radio=<id>`, the same chips as the logs); "all radios" is
offered but only means something when they share an aerial. Untagged dots (no radio id: the Android app's own radio, a
test) pass every filter, as in the logs.

## The store (`android/app/src/main/cpp/vibe_skyplot.h`)

- A dot: 24 bytes — UTC seconds, az (0.1°), el (0.1°), SNR dB (int8; −128 = nothing decoded), kind, count (decodes in
  the bucket), catalogue number, radio id. File = `VSKY0001` + records.
- **Bounded**: at most 60 000 dots (1.4 MB); past that the oldest go (rewritten to the newest 50 000 under the lock).
- **Aggregated by the decoder**: one dot per satellite per 5 s (`kBucketSec`), the best SNR in it — never a dot a packet.
- Kinds: `orbcomm` (1, wired), `meteor` (2), `aprs` (3), `funcube` (4) — any satellite decoder can call
  `vibe::skyplot::shared().add(...)` with the bird's az/el from the engine (DecoderView has az/el already). Not wired
  for the others yet: METEOR's frames and FUNcube's telemetry would need a dot per 5 s of good frames; APRS a dot per
  digipeated packet heard during a pick.

## The views

| where | what |
|---|---|
| `GET /vibeserver/skyplot.json?kind=orbcomm&radio=<id>&days=N&norad=N` | `{"v":1,"kind","dots":[[az10,el10,snr\|null,t,kind,norad,count,radioIndex]…],"radioIds":[…],"radios":[{"id","name","n"}…],"count","truncated"}` — newest first, ≤ 20 000; the gallery's privacy applies |
| `GET /vibeserver/skyplot.png?…` | the server's own picture (520 × 584, the same filters) |
| `orbcomm_status.dots` | the live plot: dots added since the last status (the SATELLITE box appends them) |
| the gallery | at each ORBCOMM pass's end (the pick sets) or a stop: this radio's ORBCOMM plot as a PNG item (kind `orbcomm`) |
| SATELLITE box (ORBCOMM pick), app + web | the plot, loaded from skyplot.json for this radio, live dots appended |

Colours (server `snrColour`, clients the same four stops): 0 dB deep blue (50,60,200) · 6 dB teal (30,170,190) · 12 dB
green (60,200,80) · 18 dB+ yellow (250,225,40); grey (110,110,110) = nothing decoded, drawn underneath.

## Tests

`vibeserver/test-orbcomm-run.cpp` §2: dots one per 5 s within 1° of SGP4; the store's radio tags, radio/kind filters,
untagged dots, the bound and compaction; the JSON; the PNG (written to `$TMPDIR/orbcomm-skyplot-synthetic.png`).

## ★★★ beta94 — unattended ORBCOMM monitoring: the LIGHT sky plot

Stuart (11-25): "can we do background monitoring of orbcomm, i like seeing the antenna plot filled up" — and "if orbcomm is
drawing an antenna plot it only needs to log the position and snr". With nobody listening and the owner's **ORBCOMM** group
switched on (Unattended satellite passes, `satPasses.orbcomm`), the server watches the ORBCOMM band whenever an ORBCOMM bird
is above the horizon and adds dots to the same `skyplot.json`. It does NOT run the full decoder (`vibe_orbcomm_run.h`):

- **`vibe_orbcomm_sky.h` SkyWatch** — two FFT frames a second of the capture (≤ 80 Hz bins), averaged in fours: a look every
  2 s. Peaks in 137.19–137.81 MHz (the full decoder's narrow-line spur cut first; ±2 kHz round the tuner's DC refused).
- **Named by the Doppler curve, not a packet.** For each bird above the horizon, `f − Doppler(t)` must land on the 2.5 kHz
  channel raster (within 400 Hz) and stay there while the predicted Doppler swings ≥ 1 kHz: the residual is fitted against
  the Doppler, `e = a + b·d`; the bird's own carrier gives `|b| ≤ 0.15` with ≤ 150 Hz RMS about the line (≥ 15 looks). A spur
  gives `b = −1`, another bird's carrier another slope. Then a dot each look where its peak is within **300 Hz** of
  channel + learnt residual + Doppler. A carrier two learnt birds both claim: no dot. A peak already explained by a learnt
  bird never teaches another. Up to three channels a bird (FM116 sends on two). The looks that named it are drawn too.
- **Refused:** elements older than 7 days; dots under 3 dB (the full decoder's dots start at ~4 dB — it cannot decode
  below, so the two plots mean the same thing).
- **The dot:** az/el from SGP4 at that moment (exactly what the full decoder's dots carry — its position reports only name
  the carrier), SNR = the carrier's power over ±3.4 kHz above the floor ÷ the floor in 4.8 kHz, calibrated −1.3 dB to the
  full decoder's; one dot per bird per 5 s (on the 5 s grid), its best SNR and its look count.
- **The gallery:** this radio's ORBCOMM plot as a PNG when a stretch of monitoring of ≥ 10 min and ≥ 10 new dots ends, and
  once a day while it runs on (kind `orbcomm`, decoder `orbcomm-sky`) — never one per pass.

### How accurate (Stuart: "as long as the plot is still accurate enough")

`vibeserver/tool-orbcomm-sky-compare.cpp` runs the full decoder and the light watcher on the SAME recording and compares the
dots 5 s bucket by bucket. On the FM116 recording of 11-24 (Pi 500 V4L 00000004, 2.4 MS/s, 445 s, 14 CelesTrak birds):

| | full decoder | light |
|---|---|---|
| FM116 dots | 48 | 48 (47 in the same 5 s buckets) |
| wrong-bird dots | — | 0 (before the fit: 2, FM20 on FM116's carrier — fixed by the slope fit and "explained peaks teach nothing") |
| SNR, light − full | | −0.2 dB mean, 0.8 dB sd (47 buckets) |
| CPU (M5 Pro, this replay) | 2.4 % of a core | 0.18 % (13× less) |

The dot's place against the satellite's own GPS position report (58 ephemeris packets): 0.25° mean across the sky (0.34°
max), 0.10° in elevation (0.17° max) — the elements were a day old. The FM113 recording (a silent bird) gives no dots in
either mode. `test-orbcomm-sky` (synthetic, the FM116 elements): its own carrier named and drawn at its az/el to 0.000° with
the SNR it was sent at (±3 dB); a still carrier, one between channels, one on half the Doppler and noise: no dot; a twin
10° behind in the same plane: never drawn; two channels from one bird: one dot per 5 s; cost ~15× under the full decoder.

### Meteor and ORBCOMM together (`vibe_sat_pass_sched.h`)

Both live in 137.0–138.0 MHz. When a Meteor pass is due and ORBCOMM is on, the server asks whether ONE capture holds the
pass's downlink (+ its Doppler) and the ORBCOMM band, with the tuner's DC ≥ 10 kHz clear of the Meteor signal
(`fitCapture`, on `min(usableSpan, rate × kBandFlatFrac)`). If so both run: the pass as ever (tune, Doppler, gain hold,
LRPT) and the watcher beside it, the capture held where both fit, and the IF filter told to hold the ORBCOMM band too
(`vibesatif::orbcommSkyRegion` — the pass's region alone narrowed it to ~500 kHz round the Meteor downlink). Only where it
does not fit does the owner's priority decide (Meteor first: ORBCOMM pauses until LOS; ORBCOMM first: no pass is taken).
Never dropped for CPU — the watcher is a fraction of a percent.

| radio | Meteor + ORBCOMM |
|---|---|
| RTL-SDR at 2.4 MS/s (or anything ≥ 915 kS/s) | together — centre 137.0078 MHz (Meteor 137.1) / 137.9922 MHz (137.9), DC outside both |
| SDRplay RSP at 2 MS/s | together — centre inside the ORBCOMM band (DC blanked ±2 kHz by the watcher), clear of Meteor |
| Airspy R2 / Mini (2.5 MS/s +) | together |
| Airspy HF+ (912 kS/s → 768 kHz usable; 768 → 648) | the priority decides (791 kHz needed) |

ORBCOMM alone: the dial on 137.5 MHz NFM and the capture where it holds the band with its DC outside it (137.179 MHz on an
RTL); an HF+ watches it from the band's middle. Tests: `test-sat-pass-sched` (both ways, the hand-over at AOS / LOS, the
IF filter, the pages' rate = the server's rule), `scripts/test-server-sat.mjs` §9b (the real server under a synthetic
ORBCOMM bird).

**Unproven on a real Pi:** the held capture centre surviving the tracker and auto-IF for a whole pass (the tick re-asserts
it and logs once a minute if anything moved it); LRPT decoding with the capture off-centre from the dial; the watcher on a
live sky over hours (channel changes, the 120 s forget); the Pi's CPU figure.

## ★★★ beta94 — storage by SKY POSITION, and the HEATMAP

Stuart (11-25): "could keep it as tracks when less data then a heatmap when it gets more" · "when in user mode you still see
a track for the satellite being tracked, once the pass ends it goes to the heatmap". Continuous unattended ORBCOMM adds
~1,500–2,000 dots an hour; the dot log (60,000) would only ever show the last ~day.

**The cells** (`vibe_skyplot.h` `namespace cells`, `Store` cell file): every dot is also counted into a cell of an
equal-area-ish grid — 3° elevation bands, `round(120·cos(band middle))` cells a band (3° of azimuth at the horizon, 3 cells in
the top band): **2,292 cells**, every one below the top band within ±30 % of the mean solid angle. Per radio and kind, in
**3-day buckets kept 30 days** (old buckets expire whole). A cell: dots, grey dots (tracked, nothing decoded), SNR sum (→ mean),
max, last heard (to the minute). `<gallery>/skycells.dat`: a 136-byte header (16 radio ids) + **16-byte records**; each process
keeps its new counts in RAM and appends them every 10 min (or at 3,000 cells, or at a stop); over 25,000 records it is
compacted (merged, expired, then the oldest buckets) to ≤ 23,000 — **≤ 400 KB**. One radio's whole sky for the whole window is
22,920 records, so it fits; three radios each hearing the whole sky keep their newest buckets. Built from the dot log the
first time it is missing. The dot log is unchanged and now feeds only the TRACKS view.

**Colour** = the cell's MEAN SNR. Each dot is already the best of its 5 s, so the mean of those maxima is a high-percentile
figure of the raw signal that one freak burst cannot repaint (a cell's max would). Grey = dots but nothing decoded.

**AUTO** (`cells::heatmapDue`, the clients' `skyCellsHeatDue` with the figures the server sends): the HEATMAP once ≥ 30 % of
the sky's cells are heard AND they average ≥ 3 dots each; TRACKS before (under ~30 % the heatmap is mostly empty sky with
single-dot cells — the tracks say more). Continuous monitoring gets there in about a day. **The live pass**: while a pass is
live (its newest dot < 60 s old — the picked bird's in the SATELLITE box), it is drawn as a track ON TOP of the heatmap, and
its dots are counted into the client's copy of the cells as they come; at LOS the track goes and the heatmap (with the pass
in it) is all that shows. TRACKS / HEATMAP buttons override (app: SATELLITE box + gallery; web: the same two); the PNG
follows `?mode=`. The gallery PNG (unattended or a pass's end) is AUTO.

**The line**: "heatmap · 30 days · 2,100 cells heard · strongest east (az 70–110°) · 12,867 dots" — the strongest 40° of
azimuth (10° steps, n-weighted mean, ≥ 20 dots), as the server's PNG says it ("STRONGEST EAST AZ 70-110").

**JSON**: `skyplot.json` gains `"cells":{"v":1,"elStep":3,"az":[120,…,3],"total":2292,"days":30,"bucketDays":3,
"auto":"heatmap|tracks","heatCoverage":0.30,"heatDotsPerCell":3.0,"c":[[index,n,grey,mean×10|null,max|null,lastT]…]}` — the
clients never keep their own grid.

**Reset**: the Setup page's "Clear this radio's sky plot" (beside Unattended satellite passes) → `POST
/vibeserver/admin/skyplot-reset` (admin, answered by the radio): this radio's ORBCOMM dots and cells; other radios keep theirs.

**Also fixed**: the pass-end picture never fired at LOS — `vibe_orbcomm_run.h` waited for in-range 1 → 0, but the tracker ends
the lock on the tick it reaches LOS (1 → −1). Any end from 1 now counts (test-orbcomm-run). The web gallery showed no SKY PLOT
row while nothing else was in the gallery (unattended monitoring fills the plot before its first PNG) — it does now.

Pictures (SYNTHETIC data, `vibeserver/tool-skyplot-fixtures.cpp`): `skyplot-web-heatmap-auto.png`, `skyplot-web-tracks.png`,
`skyplot-web-few-auto.png`, `skyplot-server-heatmap.png`. Tests: `test-skyplot-cells` (grid, aggregation, expiry, bounds,
AUTO, the heatmap PNG's pixels, reset), `scripts/test_skyplot_heat.ts` (cells parsed, the grid the server's, AUTO, pick → live
track over the heatmap → LOS → heatmap only, overrides, paths), `scripts/shot-web-skyplot.mjs` (the real server + headless
Edge, both modes).
