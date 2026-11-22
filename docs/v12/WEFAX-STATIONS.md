# WEFAX: background starts, the hourly line, WEFAX stations in one slot (V12, 2026-11-22)

Branch `b91-wefax`, which comes off v12-experimental at b9b0419db.

## RESUME HERE

- **Done and tested.** All five parts are built and committed on the branch:
  1. Whether background WEFAX misses chart starts: it does not, on the recordings so far (see below).
  2. A listener's noise strips are no longer kept in the gallery.
  3. Each background WEFAX VFO writes an hourly log line.
  4. A "WEFAX stations" VFO runs several stations in one decoder slot.
  5. A VFO that is idle gives way to a listener before one that is part-way through a chart.
- **Still open: a live check on a real server.** The coordinator needs to install a build on Stuart's Pi 500 and read the
  hourly `wefax:` lines from Export logs for VFO 8 (4608.1 USB). Those lines will say whether the Pi 500's RSP1B at 4.6 MHz
  hears Northwood at all. The decoder, and the whole background chain, catch real starts off Stuart's RX888.
- **The web admin page.** I changed `web/client/src/adminBg.ts` but did not regenerate `vibe_web_page.h`. That happens in
  the coordinator's `node scripts/build-web.mjs`.

## 1. Does background WEFAX miss real chart starts? Not on the recordings

**What happened on the Pi 500.** Background VFO 8 is 4608.1 kHz USB, which is Northwood GYA on 4610.0, on the RSP1B. In
about 8 hours it logged no `wefax:` line and saved no chart. Over the same days, Stuart's UberSDR (RX888) WEFAX addon
archived:

- only 4 Northwood 4610 chart starts in 2 days. On 20 to 22 November they were 11-20 08:12, 11-20 16:47, 11-21 11:24,
  11-21 14:00, 11-22 08:00 and 11-22 08:48;
- DDK 7880 every 10 to 25 minutes, in a daily pattern: 07:17, 07:43, 08:04, 08:17, (09:45), 10:29, 10:50, 11:11, 11:35,
  12:56 …

**How it was tested.** I recorded the addon's live audio previews. The files are 12 kHz s16, which I re-wrapped to 48 kHz
with ffmpeg. I replayed them two ways:

- `tools/wefax-bench/wefax_wav <wav> <out> 120 0 1900 bg` runs the decoder alone with the background options
  (`auto_start`/`auto_stop`, as `attachMsg` sends them). It prints the decoder's Stats.
- `tools/wefax-bench/wefax_bg_runner <wav> <galleryDir> <dialHz> [stations]` runs the **whole background path**. The audio
  is put back on the air as USB IQ (40 kHz below a 240 kS/s capture's centre). From there it goes through the BgRunner
  channeliser, the RxPipeline USB demodulator at 48 kHz, the DecoderHost WEFAX and the gallery tap. With `stations` it runs
  as a WEFAX stations VFO instead.

| Recording (UTC) | What was on air | Decoder alone, background options | Whole background path |
|---|---|---|---|
| DDK 7880, 09:26–10:00 (11-22) | Noise, then a chart start at 09:44:37 | **Start tone on 7 of 10 lines** at 1117.4 s; phasing 38/38 used; North Sea SST chart, 1448 lines | **Same start** at 1117.5 s, phasing 38/38, the chart saved (1809×1450, partial only because the recording ended). 0 IQ dropped |
| DDK 7880, 16:37 (10-05, kept FLAC) | Start tone, phasing, long tone, ice chart | Start at 133.9 s, phasing 38/38, 1532-line chart | — |
| Northwood 4610, 16:37 (10-05, kept FLAC) | Joined part-way through a chart, then its stop tone | Stop tone at 567.9 s. No start: the chart began before the recording. Correct | — |
| Northwood 4610, 09:26 → (11-22) | Dead air (noise) through 10:04, including the top of the hour | Best start tone 2/10, best phasing 0/20, 0 begun. Correct on noise | — |

**What the recordings show.** On real off-air audio from the RX888, the background options catch the start tone and the
phasing, through the decoder alone and through the whole background chain. Nothing in the server's background path loses
the start.

So the Pi 500's eight silent hours are one of these:

1. **No usable signal at the Pi 500's RSP1B on 4.6 MHz.** Northwood transmitted very little in those days: the addon's own
   archive has 6 starts in 3 days. A start needs ≥ 7 of 10 lines with the 300 Hz tone above 3.5.
2. **The VFO was paused for listeners when the starts came.** It is the lowest-ranked VFO, so it was the first one paused.
   Part 5 fixes that only when it is part-way through a chart; when it is idle it still gives way first.
3. Something on that box I could not reproduce here.

**The next step is the hourly line (part 3).** It now says, per hour: the audio level, lines read, the best start-tone run
and the strongest tone line, and the best toneless-phasing agreement. That tells (1) from (3) the next time it happens.

**Comparison with the UberSDR addon's archive for the same period:**

- DDK 7880: our start is at 09:44:37. The addon's listing showed no 09:4x DDK item yet when I checked at 10:04; it files a
  chart only when it is saved.
- Northwood 4610: there was no chart on the air at 10:00. The addon recorded noise from 08:48 to 09:31 as one 3600-line
  item. We recorded the same noise and started nothing, as the background rule intends.

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
| test-wefax-phasing | 27/0 | unchanged (the decoder's output did not change) |
| test-bgdecode | 240/0 | listener strips, the hourly line, WEFAX stations (two charts at once in one slot, retune) |
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
