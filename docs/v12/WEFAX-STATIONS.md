# WEFAX: background starts, the hourly line, WEFAX stations in one slot (V12, 2026-11-22)

Branch `b91-wefax`, which comes off v12-experimental at b9b0419db.

## RESUME HERE

- **Done and tested.** All five parts are built and committed on the branch:
  1. Whether background WEFAX misses chart starts: it did, for a chart that fades in after its start tone and phasing
     (Northwood 10:00 UTC on 11-22). Fixed in the decoder, with tests and the recording's numbers (see below).
  2. A listener's noise strips are no longer kept in the gallery.
  3. Each background WEFAX VFO writes an hourly log line.
  4. A "WEFAX stations" VFO runs several stations in one decoder slot.
  5. A VFO that is idle gives way to a listener before one that is part-way through a chart.
- **Still open: a live check on a real server.** The coordinator needs to install a build on Stuart's Pi 500 and read the
  hourly `wefax:` lines from Export logs for VFO 8 (4608.1 USB). Those lines will say whether the Pi 500's RSP1B at 4.6 MHz
  hears Northwood at all. The decoder, and the whole background chain, catch real starts off Stuart's RX888, and now a
  chart that fades in too.
- **The web admin page.** I changed `web/client/src/adminBg.ts` but did not regenerate `vibe_web_page.h`. That happens in
  the coordinator's `node scripts/build-web.mjs`.

## 1. Does background WEFAX miss real chart starts? YES — when a chart fades in; fixed

Stuart's Pi 500 (RSP1B, background VFO 8 = 4608.1 USB = Northwood GYA 4610.0): ~8 h, no `wefax:` line, no chart saved.
His UberSDR (RX888) WEFAX addon archived only 4 Northwood 4610 starts in 2 days but DDK 7880 every 10–25 min.

Recorded from the addon's live audio previews (12 kHz s16 → 48 kHz with ffmpeg) and replayed two ways:
`tools/wefax-bench/wefax_wav <wav> <out> 120 0 1900 bg` (the decoder alone, background options, prints its Stats) and
`tools/wefax-bench/wefax_bg_runner <wav> <galleryDir> <dialHz> [stations]` — the **whole background path**: the audio put
back on the air as USB IQ, a real BgRunner (channeliser → RxPipeline USB 48 kHz → DecoderHost WEFAX, auto_start/auto_stop
→ gallery tap).

| Recording (UTC, 2026-11-22) | On air | Before the fix (background options) | After |
|---|---|---|---|
| DDK 7880 09:26–10:00 | noise, start tone 09:44:37, North Sea SST chart | **caught**: start 7/10, phasing 38/38, 1448 lines (decoder alone and whole path) | same; as a WEFAX stations VFO too ("Pinneberg (DWD) 7880.0 kHz") |
| Northwood 4610 09:26–10:34 | dead air; **a chart that faded in ~2½ min after it began** (~10:03, no start tone, no phasing), stop tone 10:11:09; then a full chart with start tone 10:11:58 → stop 10:23:10 | the 10:00 chart: **missed** (best start 2/10, phasing 0/20, nothing drawn); the 10:12 chart caught | 10:00 chart saved **PARTIAL 1809×882**; 10:12 chart complete 1809×1291; two false starts (09:36, 09:54 — a few lines of vertical stripes) begun, ended after 3 min of noise, dropped by the gallery |
| DDK 7880 / Northwood 4610, 2026-10-05 FLACs | start tone + chart / joined part-way to a stop tone | DDK caught; Northwood joined part-way: nothing | DDK caught; Northwood: begun out of the noise at 2.9 s, saved to its stop tone (1122 lines, partial) |
| 5 h of other modes (2026-10-05 FLACs: NAVTEX 2 h, FT8 1 h, SSTV 1 h 45, MSF, DCF77) | no WEFAX | 0 begun | **0 begun** |

**The addon for the same period** (its own archive, read once): DDK 09:45:05→10:05:16 (we: start tone 09:44:37);
Northwood 10:12:07→10:23:18, 1261 lines (we: 10:11:58→10:23:10, 1291); the 10:00 Northwood chart is not in its archive
either (its channel read `decoding:false` at 10:11). So a chart whose start the receiver never heard was invisible to both.

**Why it was missed (two faults in `decoders/wefax_decoder.cpp`):** waiting in the background (`auto_start`, nothing begun)
the decoder never measured the chart/noise verdict (`corrAvg` was updated only on lines being drawn), so (1) a chart that
fades in after its start tone and phasing could never begin, and (2) the 2026-11-15 ★★★ "a chart after the stop tone is a
new chart" rule could never fire in background at all. **Fix:** an idle background decoder measures the verdict every line
(`resampleLine`/`updateCorr`, split out of `decodeImageLine`); chart-like lines out of the noise begin a chart (never on a
tone's own lines — a start tone's 300 Hz stripes read as chart lines a line before it fires); a chart begun on the verdict
alone (no tone, no phasing — out of the noise or after a stop tone) ends after `IDLE_END_LINES` = 360 lines (3 min) of
noise, so a false start never draws noise until a stop tone that never comes; the gallery keeps such a chart only at
600+ lines and marks it **PARTIAL** (its top is missing). Listener decoding (no `auto_start`) is untouched.

**What this means for the Pi 500:** at least part of its silence is this fault — Northwood fades in and out at 4.6 MHz,
and any chart whose start was faded was invisible. The rest may be signal (the RSP1B and its antenna vs the RX888) or the
VFO being paused for listeners; the new hourly line (part 3) tells them apart from Export logs. **Needs a live check.**

## 2. A listener's noise strips (`vibe_bgdecode.h` GalleryImageTap)

**The problem.** A listener's WEFAX free-runs, with no `auto_start`. Every retune with WEFAX open used to leave a
58–100-line strip of noise in the gallery.

**The rule now.** A listener's chart is kept only if one of these is true:

- a start tone or phasing was seen for it (the 0x02 start event, or a 0x04 phase report of 1 or 2);
- it ran **600 lines**, which is 5 minutes at 120 LPM and covers a chart joined part-way.

Anything else is dropped silently. A chart that rolls over the 3000-line cap keeps its mark. Background WEFAX is unchanged:
it only draws after a start anyway.

## 3. The hourly line (`WefaxDecoder::Stats`, `DecoderHost::Env::wefaxHourly`)

Every background WEFAX channel, single or stations, logs a line like this every hour:

```
wefax: the last hour on background VFO 8 (4608.1 kHz USB): audio -19.1 dBFS rms (peak -3.0); 7200 lines read; start tone at
best 3 of 10 lines (7 start a chart; strongest line 2.1, a tone line reads over 3.5), stop tone at best 0 of 10; phasing at
best 5 of 20 lines agreeing (14 start a chart without a tone); 0 chart(s) begun, 0 saved; phasing 0 used, 0 rejected
```

- A run stopped after five minutes or more (paused, out of the window, or the profile changed) says "the part-hour before
  it stopped … (N min)".
- The cost is one multiply-add per sample and a few adds per line.
- The decoder's output is unchanged: test-wefax-phasing is 27/0, as before.

## 4. WEFAX stations: several frequencies in one decoder slot

**The station list.** `android/app/src/main/cpp/vibe_wefax_stations.h` is the one list: 19 stations and 68 frequencies.

- **Source.** NWS, "Worldwide Marine Radiofacsimile Broadcast Schedules", 7 March 2025
  (https://weather.gov/marine/media/rfax.pdf).
- **Frequencies.** They are the assigned (centre) frequencies. The dial is 1.9 kHz below, on USB.
- **Converted where the schedule quotes the carrier.** Athens 4481/8105 → 4482.9/8106.9 (wefaxAlign.ts already had
  these); Bangkok 7395.0 → 7396.9.
- **Kodiak 12412.5.** The station table's figure. The schedule page's footer says 12410.6.
- **Murmansk** is listed as the schedule gives it. The schedule itself marks it as uncertain.
- **Left out:**
  - Wellington ZKLF: it stopped on 1 July 2023.
  - The Canadian Arctic stations: discontinued in 2019.
  - Pevek: LF, 90 LPM.
  - Kyodo: press, not weather.

**The saved setting.** The decoder is `wefax-stations`, and `Vfo::fax` is stored as `"fax":[Hz…]`. The save refuses each
of these, in words:

- none ticked;
- a frequency that is not in the list;
- the same frequency twice;
- more than **8** (`kMaxFaxFreqs`);
- none of the ticked frequencies inside the window;
- a mode other than USB.

**The engine (BgRunner).**

- **One slot for the VFO.** The VFO claims ONE decoder slot.
- **A channel per frequency.** Each ticked frequency gets its own slice from the shared FFT, an RxPipeline USB demodulator
  and a DecoderHost "wefax" with the background options.
- **Bounded memory.** Each channel has `noReplay`, so there is no 4 MB joiner copy, and its own hourly line.
- **Window changes.** Frequencies start and stop as the radio moves. The VFO is "in the window" when any of its frequencies
  is.
- **Gallery.** Each chart is its own item, titled "WEFAX · Boston NMF 4235.0 kHz · 1809×N" (`gallery::Meta::station`) and
  filed at that frequency's dial.
- **Admin page.** It lists each frequency as receiving a chart, standing by, paused or out of range.
- **The shim.** Its centre shift for a Band VFO (`bgBandCentre`) and `bgAnyInWindow` both know about the stations row.

**Memory.** A chart in progress is at most 3000 × 1809 bytes (5.4 MB) in its gallery copy. The worst case for 8 channels,
all mid-chart at once, is about 45 MB. That is bounded, and it would never happen in practice.

**CPU** (`tools/wefax-bench/wefax_cpu`, on this thread's own CPU clock, the M5 Pro). One channel costs **0.19 % of one
core** in real time: its slice cut, the RxPipeline USB demodulator and the WEFAX decoder. This was the same at 2 MS/s and at
250 kS/s. The shared channeliser costs 0.53 % at 2 MS/s and 0.05 % at 250 kS/s. The figure has not been measured on a
Pi 2.

**Clients.** None of them carries its own station list.

- The Setup page and the Android server screen draw the ticks from the server's `wefaxStations`. It comes from
  GET /vibeserver/hardware and from backgroundCapsJson, through one writer, `vsWefaxStationsJson`. It lists only
  frequencies the radio tunes: its range, the owner's allow and block lists, and a locked window.
- `decoderBands.ts WEFAX_KHZ` is now exactly the server's list.
- `scripts/test_wefax_stations.ts` reads the header and fails if any of these drift from it: WEFAX_KHZ, knownSignals
  DWD_FAX/NORTHWOOD_FAX, or wefaxAlign NORTHWOOD_KHZ/BORDER_KHZ.

## 5. Who gives way to a listener

**The rule.** Rows are in rank order, but a VFO part-way through a picture is passed over while an idle VFO is running.
This applies on one radio (`pausePick`) and across the box. The box-wide registry's state line gained an optional third
field, `<rank> <running> <busy>`. An older process writes two fields, which are read as idle. Only when every running VFO
is mid-picture does the lowest of them go.

**What "busy" means.** `DecoderHost::midPicture()` covers a WEFAX chart from its start to its stop, and SSTV from its VIS
to its end. It stops counting after 30 minutes.

## Tests (numbers on the branch)

| Suite | Result | New |
|---|---|---|
| test-wefax-phasing | 33/0 (+6) | background: a chart out of the noise (the Northwood numbers), after a stop tone, not on a start tone's stripes, a false start ended after 360 lines, 10 min of noise begins none |
| test-bgdecode | 243/0 | listener strips, the hourly line, WEFAX stations (two charts at once in one slot, retune) |
| test-bg-priority | 46/0 (+10) | the registry's busy field; a listener pauses the idle RTTY VFO and not the mid-chart WEFAX |
| test-setup-bg-wefax.mjs | 17/0 | new |
| test_bg_decode_model.ts | 64/0 (+11) | the app model |
| test_admin_bg.ts | 28/0 (+2) | the admin line |
| test_wefax_stations.ts | 7/0 | new |

**Unchanged and still passing:** test-setup-bgdecode, -bg-skim, -bg-ndb, -bg-band, -bg-tiers, -bg-ism, -bg-release,
check-setup-page, test_known_signals (132/0) and test_decoder_bands.

**Build.** The `vibeserver` target builds (CMake Release).

## Recordings

The recordings stay local because of the DDK and Northwood content licences. They live in the scratch directory and are
deleted when the work is done. The two FLACs from 2026-10-05 in `~/Documents/VibeSDR-recordings/2026-10-05/` were only
read.
