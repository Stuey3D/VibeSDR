# Message SNR — a colour-coded SNR on every decoded message (V12 beta96, 2026-11-26)

Stuart, 11-26: *"and for all modes if possible a colour coded snr readout per message"*. The reason, from his previous
message: *"it will allow me to check antenna performance quickly"*. Then: *"adsb should drop its 3 bar signal meter for
the new colour coded snr"*, and *"the snr colours need to be accurate to the mode so if a mode only needs a weak signal to
work great no point listing all as red"*.

Branch `b96-msgsnr` (from `b96-integrate` fe9bae62a + the ACARS/VDL2 msg/min commit 80c530552). Nothing is pushed or merged.

## RESUME HERE

- **Built:** one shared scale (`src/utils/snrColour.ts`), one app chip (`src/components/SnrChip.tsx`), one web chip
  (`snrChipHtml` + `.snrChip` in `web/client/index.html`). The modes shown in the table below are wired server → app → web.
  ADS-B's 3-bar meter is removed in the app, on the web page and in the shared map layer (`adsblayer.js`; its copies were
  regenerated with `scripts/gen-adsblayer-source.mjs`). The aircraft card gains an SNR row.
- **Server fixes made on the way:**
  - **AIS.** A ship's SNR used to read the floor AFTER the frame: about 3 dB whatever the ship. It is now the frame's own
    power (`vibe_ais.h` `burstSnr_`).
  - **Radiosondes.** The SNR was the demodulator's eye figure, which flattens out at about 5 dB. It is now S/N in the
    scan's box (`vibe_rs41_rx.h` `Chan::snr`).
  - **Older servers.** Both reports now carry `"snrv":2`. Clients draw no chip from an older server's figure.
- **New measurements:**
  - **ADS-B.** The reply's SNR is measured per reply (`Frame::snr`) and smoothed per aircraft. It travels in `"snr"` and
    in a new ADS-B delta column `cSnr`. That column is a beta96-only protocol change: flight, category and Comm-B move up
    one bit. Delta is new in beta96, so no released client or server is affected.
  - **The delta column is coarse** (Stuart 11-26: "use coarser to save resources"). It carries 3 dB BANDS on ADS-B's
    colour edges: k = floor((snr − 10) / 3), and the client shows 11.5 + 3k. So rounding never changes a colour
    (test-adsb-delta checks 701 figures), and the column goes out only when an aircraft's band changes.
  - **Delta bytes a tick, 172-aircraft stream (test_adsb_delta, `VIBE_ADSB_SNR_STEP=0|10|30`):** no SNR column
    **2.52 KB** · whole dB 2.80 KB (+11 %) · **3 dB bands 2.61 KB (+3.6 %)**, as shipped. The full report (and so the
    card on a non-delta client) keeps 0.1 dB.
  - **APRS.** The IQ paths measure each frame's SNR (`vibe_aprs_run.h` `frameSnr_`).
  - **Datalink and LoRa.** Datalink rows carry the last message's SNR and its scale. LoRa messages carry SNR + SF.
- **Next / not done:** see "Not done" at the bottom.
- **Needs a live capture (coordinator):**
  - ADS-B, AIS, VDL2/ACARS and an ORBCOMM pass, on two aerials, to confirm on air that the colours split real traffic
    sensibly (strong local = green, the far ones amber / red).
  - Of these, only the benches have been run. Nothing was verified on air.

## The rule

- **One number, one colour.** The number is the real dB the decoder measured, rounded. It is always shown: never colour
  alone.
- **Per-mode colour.** The colour is the MARGIN over that mode's decode threshold `thr`, on the scale that mode's
  decoder REPORTS:

  | Colour | Margin over `thr` | Meaning |
  |---|---|---|
  | red | under 3 dB | at or just above where the decoder stops: nearly lost |
  | amber | 3 to 6 dB | decoding, but a fade takes it |
  | green | 6 dB or more | solid |

- **Comparable within a mode only.** The number compares this antenna with that one, or today with yesterday, on the
  same mode. It does not compare across modes: each mode measures its own way.
- **Where `thr` comes from.**
  - A published figure, only where our decoder reports on that figure's scale (WSJT-X modes: SNR in 2500 Hz).
  - Otherwise our own bench: the SNR the decoder REPORTED for the messages it decoded at about the 50 % point. Every
    bench now prints that column.
- **The chip.**
  - It is filled with the band colour, with dark `#111` text. Contrast is ≥ 4.5:1 on all three colours, checked by the
    test.
  - It carries its own background, so it reads the same on the default, silver and black faceplates, with transparency on
    or off.
  - It uses the standard decoder font (Hyperlegible), never the display font: decoder boxes are small text.
- **Tooltip / accessibility label.** It says the margin, what is measured, and the mode's bands. For example: *"SNR 17
  dB, 3 dB over where this mode stops decoding (amber) — burst power over the channel noise floor. Decodes from about
  14 dB: red under 17, green from 20."*
- **No number, no chip.** Nothing is drawn when the server sends no number, or no scale.

## Per-mode table

`thr` is on the reported scale. Bench figures are synthetic signals through our real decoder, run on this Mac.

**Cost** is the extra work per sample or per message. CPU was compared before and after with the old header built from a
scratch copy of the tree; the "Cost" column says where.

### Aviation and maritime

| Mode (key) | Source (server) | What the number is | `thr` (dB) | Where `thr` came from | Cost |
|---|---|---|---|---|---|
| ADS-B (`adsb`) | `vibe_adsb.h` `Frame::snr`, aircraft `snr` (×0.25 a reply, like `rssi`) | Preamble pulse power over the demodulator's floor (`noiseP_`). Reads ~2 dB under the transmitted pulse SNR. | 10 | test-adsb: the preamble gate is 9 dB; at the 50 % point (2.4 MS/s, 12 dB transmitted) decoded replies report ~10 | One log per DECODED reply. Delta: 3 dB bands, 2.52 → 2.61 KB a tick at 172 aircraft (+3.6 %; whole dB would be 2.80) |
| AIS (`ais`) | `vibe_ais.h` `burstSnr_` (NEW: it replaces the floor-after-frame reading) | Frame's mean power over the channel floor, S/N | 9 | test-ais: 50 % at Eb/N0 ≈ 8.5; decoded frames report 8.6 / 10.0 / 11.6 / 13.3 / 16.5 at 8 / 10 / 12 / 14 / 18 | One add a sample into a per-bit ring (8 KB a channel); ≤ ~1,100 adds a frame. test-ais CPU row 0.7 % of a core before and after (0.017 → 0.017–0.018 s per 2.6 s) |
| ACARS (`acars`) | `vibe_acars.h` `Block.snrDb` | AM carrier over the channel floor. Reads C/N0 − 38.5 clear of the threshold; low near it (the floor follower). | 2 | test-acars: 27–67 % at 45 dB-Hz, those blocks report 1.6–2.6 dB | None new: it was measured and dropped; now sent |
| VDL2 (`vdl2`) | `vibe_vdl2.h` `Frame.snrDb` | Burst power over the channel floor, (S+N)/N. Within ~1 dB of the transmitted SNR. | 14 | test-vdl2: 1 of 24 at 14 dB, 17 of 24 at 16; decoded bursts report 12.6–14.9 | None new |
| HFDL (`hfdl300/600/1200/1800`) | `vibe_hfdl.h` `Message.snrDb` | The equaliser's own SNR. Reads ~+5 dB over the symbol-bandwidth SNR, compressed (−6 → 2, 0 → 6, +10 → 15). | 2 / 2.5 / 4 / 7 | test-hfdl `--sens 0…4` per bit rate (the table prints the reported SNR) | None new |
| ORBCOMM (`orbcomm`) | `vibe_orbcomm.h` `snrDb` (M2M4) | Es/N0 of the carrier. Reads the transmitted Es/N0 to 0.1 dB from 6 to 25 dB. | 5.5 | test-orbcomm: 553 of 800 packets at 6 dB (reported 6.0), 745 at 8; ORBCOMM.md: ~50 % at 6 | None new (already sent; now shown) |
| Radiosonde RS41 (`rs41`) | `vibe_rs41_rx.h` `Chan::snr` (NEW: it replaces the eye figure) | Sonde power over the floor in the scan's 10 kHz box, S/N. Reads the bench's "SNR in 10 kHz" − 2.1 dB, dB for dB from 7 to 30. | 4.5 | test-rs41: 2/12 at 6 dB (reported 3.9), 11/12 at 7 (4.9) | One log per candidate per scan (twice a second). Nothing per sample |
| DFM (`dfm`) | same | same, 10 kHz box | 5 | test-sondes-types: 0 at 4 dB, 12 of 15 at 6 (reported 6.0) | same |
| M10 (`m10`) | same | same, 20 kHz box (reads ~10 dB under the bench's 10 kHz SNR) | 1.5 | test-sondes-types: 0 at 10 dB, 14 of 19 at 12 (2.2) | same |
| M20 (`m20`) | same | same, 20 kHz box | 0 | test-sondes-types: 1 at 10 dB (−1.4), 16 of 19 at 12 (0.6) | same |
| NAVTEX (`navtex`) | `fsk_decoder` bit-centre slices (beta96) | SNR in the 100 Hz bit bandwidth. Bands: red < 9, green ≥ 12; `vibe_gallery.h` `kSnrRed` agrees. | 6 | tools/rtty-bench: CER 28 % at a meter reading of ~6 dB, 0.6 % at ~9 (synth label −6 / −3; meter = label + 11.8) | Unchanged |

### APRS, ISM and LoRa

| Mode (key) | Source (server) | What the number is | `thr` (dB) | Where `thr` came from | Cost |
|---|---|---|---|---|---|
| APRS 1200 Bd (`aprs`) | `vibe_aprs_run.h` `frameSnr_` (NEW, IQ paths only) | Carrier over the floor in the widest IQ channel filter. Reads CNR(12.5 kHz) + ~1.5 from 6 to 14 dB. | 6 | test-aprs-onair `--curve` (flat ±3 kHz): 50 % at CNR 3.6, where packets report ~6 | One add a sample on the widest filter into a per-bit ring (16 KB); ≤ ~2,800 adds a frame. test-aprs-onair `--curve` user time 3.11–3.13 s before, 3.13–3.16 s after (noise) |
| ISM bursts (`ism`) | `vibe_ism.h` `Burst` level − noise | Burst 95th-percentile envelope over the band median before it | 6.5 | `kMinBurstSnrDb`: the decoder slices nothing under 6.5 (noise alone reads ~4.5) — the limit by construction | None new |
| LoRa SF7…SF12 (`lora7`…`lora12`) | `vibe_lora.h` `Frame.snrDb` → node rows (existing) and messages (NEW) | LoRa's in-band SNR from the preamble. Within 0.5 dB of the transmitted at every SF. | −9, −11.5, −14, −16.5, −19, −21.5 | test-lora sensitivity table (now prints the reported SNR): 50 % points ~1.5 dB under Semtech's −7.5 … −20 | Two fields per message |

### Digital spots

| Mode (key) | Source (server) | What the number is | `thr` (dB) | Where `thr` came from | Cost |
|---|---|---|---|---|---|
| FT8 / FT4 / FT2 (`ft8` `ft4` `ft2`) | digital_spot `snr` | WSJT-X convention, SNR in 2500 Hz | −21 / −16 / −13 | FTX-SENSITIVITY.md 50 % points −20.8 / −16.3 / −13.3 (jt9 −20.7 / −16.9) | None |
| JS8 (`js8`) | same | same | −20 | FTX-SENSITIVITY.md: Normal 8/8 at −20 | None |
| WSPR (`wspr`) | same | same | −28 | **PUBLISHED** (WSJT-X); not benched on our decoder yet | None |
| JTTY (`jtty`) | same | same | −15 | JTTY.md: 50 % at about −15.3 for a single frame | None |

## Where it shows

### Aviation and maritime

| Mode | App | Web |
|---|---|---|
| ADS-B | `AdsbLiveList` (chip where the bars were); the aircraft card in `adsblayer.js` (an SNR row with the chip, "last few replies"; tooltip says the smoothing) | `adsb.ts` `adsbListHtml` (the same) |
| Datalink MESSAGES tab | `SDRScreen` msgRows → `TrackLiveList` | `main.ts` asRender (the `.sp` column) |
| AIS ships | `aisListRows` → `TrackLiveList` | `tracks.ts` `aisRows` (the range column) |
| Sondes | `sondeTrackRows` → `TrackLiveList`; `SondesPanel` rows | `sondes.ts` `sondeRows` |
| ORBCOMM packets | `SatellitePanel` | `satellites.ts` |

### APRS, ISM and LoRa

| Mode | App | Web |
|---|---|---|
| APRS stations | `aprsListRows` → `TrackLiveList`; `AprsView` box rows | `tracks.ts` `aprsRows`; `main.ts` APRS box |
| ISM sensors, meters, unidentified; LoRa nodes and messages | `IsmPanel` (replaces beta92's fixed ≥ 20 / 10–19 / < 10 colours; a LoRa node's below-zero SNR is now shown — the old `> 0` test hid it) | `ism.ts` |

### Digital spots

| Mode | App | Web |
|---|---|---|
| Digital spots (FT8 family, WSPR, JTTY) | `DecoderPanel` SNR column (a number-only chip) | `main.ts` `renderSpotsRowsInto` |

NAVTEX keeps its beta96 strip, timeline and line (now on this scale).

## The ADS-B list at every width (Stuart 11-26: "the signal hides the aircraft details")
A beta95 server in Safari, the list panel ~232 px on a ~1190 px window: every row showed the meter and a clipped "[", no
callsign. The fixed altitude / speed / range columns took the row, the thing beside the name never shrinks, and the only
narrow rule was a viewport `@media` — never true for a narrow PANEL on a wide window. The chip is wider than the bars, so
beta96 was worse. Fixed by width of the LIST, in one order on both readers:
- web: a container query on `#adsbList` (web/client/index.html) — < 420 px speed goes; < 360 the chip (and a MIL /
  emergency tag) moves under the callsign; < 320 altitude over range in one end column, "tracked" goes; < 210 the bearing
  word goes. The chip beside the name keeps a 5 px gap (TrackView's `.trkRow .snrChip` margin had removed it).
- app: `adsbRowLayout(listW)` (src/services/adsbList.ts), fed TrackLiveList's `adsbListWidth` — speed, then chip under, then
  stacked; checked at every width 160–300 pt in scripts/test_adsb_list.ts (layout arithmetic only: RN text is not measured).
- scripts/test-web-adsb-list-width.mjs: the REAL page on a real vibeserver + fake rtl_tcp ADS-B clip, panel forced to
  232 / 300 / 400 / 600 px and 390 / 320 phones — failed 86/18 before (0 px of callsign at 232 and 300), 122/0 after.
- Not changed: the other aircraft rows (`.trkRow`, auto columns) and NDB's `#ndbList` still narrow by the viewport only.

## Stored / logged

Decoder logs are one per server across radios.

Stuart 11-26: *"yes save snr history"*. Every store that keeps decoded messages and has room now keeps the SNR. Each
change is version-safe for older readers and writers, and the history views draw the same chip.

| Store | How the SNR is kept | Older readers / writers | History view | Test |
|---|---|---|---|---|
| Ship store `ais-ships.dat` (`vibe_ais_store.h`) | The 512 B record's `spare` HIGH byte = SNR + 64 (1–255 → −63…191 dB), 0 = none. The answer says `"snrv":2`; a ship with none has no `"snr"` (never a fake 0). | Older readers only read bits 0–1. Older writers leave 0, and the merge keeps the stored value. | Stored ship rows (AIR & SEA) | test-ais-store +5 (round trip, older record, merge, bounds), test_shipping_map +3 |
| Sondes day card (`vibe_posgal.h`) | A block of its own BELOW the 10-field table: `kSnrHead` then `id · dB` lines (two fields). | An older `galPosParse` / restore skips any line that is not 10 fields: checked ("an older reader still sees exactly the 2 rows"). | Stored sonde rows, per type | test-posgal +3 (block, older reader, restart), test_stored_layers +4 (read, rows, merge, types) |
| AIS and APRS day logs (`vibe_station_log.h`) | Each line ends `" · SNR 18 dB"` (`vibe_station_sink.h` `snrToken`: whole dB, U+2212 minus, −99…199). | Plain text: an older viewer shows it as words. The server-wide dedupe compares lines WITHOUT it (`vibe_gallery_shared.h` `withoutSnr`), so one message two radios heard is still one line, carrying the first radio's SNR. | The gallery text view: web `snrLogHtml`, app `SnrLogText`. Chips for line-end tokens only; plain over 200 000 characters, a translation or NAVTEX. | test-shared-day +2, test_snr_colour +6 |
| ISM day log (`vibe_ism_gallery.h`) | The same token on sensor lines. A reading with no measured SNR (0) writes none. | as above | as above | test-ism-gallery +2 |
| ORBCOMM packet lines (the decoder box and the gallery log) | The same token, before a message's raw bytes. | as above | as above | test-orbcomm-run +1 |

Already kept before this change:
- The station store (`snr`, `lastSnr`, `bestSnr`: FT8 and the skimmers).
- The NAVTEX gallery (beta96).
- The LoRa mesh `NodeFix.snr` (the ISM nodes card).
- The APRS in-memory frame log (`aprs_frame`, this change).
- ORBCOMM's sky plot (the best SNR per bucket).

**Not kept, and why:**
- **Datalink (ACARS / VDL2 / HFDL):** no store keeps its messages. Bodies are never in a public gallery, and
  `aircraft-live.dat` is a live share of positions with no per-message room. The live MESSAGES rows have the SNR.
- **LoRa text messages:** they do not go to the ISM day log. The nodes card keeps each node's SNR.
- **ISM meters:** their lines already carry their own signal words, so no token is added.

## Tests (all green on this Mac, 11-26)

### Scale and clients

| Suite | Result | What it covers |
|---|---|---|
| `scripts/test_snr_colour.ts` (new, in run-tests.sh) | 194/0 | Every mode: a typical good decode green, a just-decoded one red, a little margin amber. Also: the bands at +3 / +6, the text, the tooltip, no chip without a number or with an unknown scale, LoRa per SF, ISM by kind, spots by mode, contrast ≥ 4.5:1, NAVTEX's bands and the server's `kSnrRed`, `index.html`'s `.snrChip` colours, `adsblayer.js`'s copy of the ADS-B scale, no ADS-B bars |
| `scripts/test-web-msg-snr.mjs` (new, headless Edge, `--mute-audio`, in run-tests.sh) | 21/0 | ADS-B / AIS / LoRa rows through the real builders and CSS at 900 and 390 px on a dark and a light page: chip colour per mode, dark text, inside its row, no sideways push, no chip from an older server, no `.sig` bars |
| `test_adsb_list` | 35/0 | Bars gone; SNR rides on the row |
| `test_live_maps` | 65/0 | A message keeps its SNR; `avSnrByKey` |
| `test_aviation_app` | all passed (+3) | Parse `snr` + `sm` |
| `test_ais_aprs_app` | 195/0 | AIS snrv gating; app = web chips; APRS SNR parse / rows / box |
| `test_sondes_app` | all passed (+4) | — |
| `test_ism_app` | 260/0 | Message SNR + SF, app = web, the web rows' chips |
| `test_adsb_report` / `test_adsb_feed` / `test_adsb_layer` | 75/0, 19/0, 239/0 | — |
| `test_navtex` | 234/0 | — |
| `test-web-navtex-snr` | 11/0 | — |

### Server

| Suite | Result | What it covers |
|---|---|---|
| `test-adsb` | 4850/0 | Reported SNR within 3 dB of the transmitted pulse SNR at ≥ 2.4 MS/s |
| `test_adsb_delta` | 18/0 | `cSnr` round-trips; the stream test now varies `snr` |
| `test-ais` | 111/0 | Reported SNR within 2.5 dB of Eb/N0 − 1 at 10–18 dB |
| `test-acars` | 10040/0 | C/N0 − 38.5 ± 2.5 at ≥ 51 dB-Hz |
| `test-vdl2` | 48/0 | ±6 |
| `test-hfdl` | 58/0 | Reported − transmitted within 3–8 dB at 0–10 dB |
| `test-avi-service` | 41/0 + 3 SNR checks, exit 0 | — |
| `test-orbcomm` | 47/0 | ±3 dB |
| `test-rs41` | 132/0 | ±2.5 dB, 7–30 dB |
| `test-sondes-types` | 79/0 | Moves dB for dB, 15 → 30 |
| `test-lora` | 302/0 | Its sensitivity table now prints the reported SNR; held within 3 dB of Semtech's SNR at every SF |
| `test-lora-ham` | 43/0 | A message carries SNR + SF |
| `test-lora-nodes` | 51/0 | — |
| `test-aprs-onair` | 27/0 | IQ packets at CNR 6 / 12 report CNR + 1.5 ± 2; audio-only claims none |
| `test-aprs` | 73/0 | — |

Not run here:

- `test-navtex-quality`: it needs the ft8 group's objects. Its slices (2 / 3 dB red, 14–20 green) hold under the new 9 / 12
  bands by construction.
- The full `run-tests.sh`: the coordinator runs it.

Typecheck: `tsc` was run on the changed files with a scratch tsconfig. That covered the services, the web modules, the
components, `DecoderPanel`, `SatellitePanel` and web `main.ts`; all clean. `SDRScreen.tsx` was not typechecked alone
(memory cap).

## Not done (honest list)

- **CW, PSK and RTTY skimmer spots, and CW / RTTY signal rows.** Their figures are a 47 Hz bin SNR, Es/N0 and tone SNR,
  and no threshold has been sourced on those scales. They keep their plain number and old colours.
  - *Next:* run test-cw / test-psk / test-rtty-skimmer and print the reported SNR at the 50 % point.
- **WSPR.** `thr` is the published −28, not benched on our decoder.
- **POCSAG / FLEX, DSC, DMR / dPMR / D-STAR / M17, FUNcube `sat_tlm`, SSTV / WEFAX, SARSAT, balloons.** No per-message
  level is emitted yet.
  - Cheap spots exist: DSC `hiOn_` / `loOn_` vs `noise_` (`dsc_decoder.cpp`); FUNcube `rx_.snrDb()` per `sat_tlm`
    (`vibe_bpsk_rx.h`); the dvsig 4-FSK sync fit.
  - Each needs its own bench threshold. Not built in this pass.
- **The watch** (`ios/VibeSDRWatch`, `spike/WristSDR`) still shows an ADS-B aircraft's `rssi` dB as grey text (not bars).
  It was not changed.
- **R36S native UI:** not touched.
- **Message stores:** datalink has no message store, and LoRa text messages are not in the ISM day log (see "Stored / logged").
- **The NDB** keeps its own 3-bar meter. Stuart's request was ADS-B's.
