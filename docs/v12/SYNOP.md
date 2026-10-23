# RTTY weather reports in plain English: SYNOP and SHIP (V12, 2026-10-23)

Stuart approved this on 2026-10-22: "if you can do it that would be great". DWD's weather RTTY (DDK2 4583 kHz, DDH7 7646,
DDK9 10100.8; 50 Bd, 450 Hz) is mostly WMO coded reports in five-figure groups. The decoder reads them correctly, but a
listener sees only digits. VibeServer now translates each report and shows the result **beside** the raw groups. It
never replaces them.

```
10384 41/96 82508 10118 20072 30115 40206 57012 8807/ 333 10173 20065=
  ▸ 10384 · Tempelhof, Germany · 06:00 UTC · wind 250° 8 kt (15 km/h) · 11.8 °C · 1020.6 hPa
```

## Where the translation runs

The server does the translating. Stuart's rule: "a much wider range of devices can listen than can host".

- `android/app/src/main/cpp/vibe_synop.h` holds the parser (header only). It is a stream reader: decoded RTTY text goes
  in, finished reports come out. The decoder host builds one reader for every RTTY decoder (`FskDecoder` with ITA2 or
  ASCII, and `RttyAuto`), and runs it on the decode thread under `decMtx_`. NAVTEX never gets a reader.
- `vibe_decoder_host.h` sends each report as frame `0x08`, **in its place** in the text. That means the 0x01 text up
  to the end of the report's line goes first, then the 0x08, then the rest of the text. The reader holds a finished
  report until its line ends: the next newline after the `=`, or the next printing character, or 3 s with no text.
- The gallery (`vibe_bgdecode.h`, `GalleryTap::report`) writes the line and its details indented under the raw report
  in the RTTY run's .txt. This covers background decoding and listeners' decoders alike.
- The clients only display what arrives: `src/utils/weatherReport.ts` checks each field, the web client places the
  report with `web/client/src/weatherBox.ts`, and the app with `src/components/WeatherReportText.tsx`. The line shows
  collapsed, and a tap opens the details and the raw groups.

## Wire

```
0x08 | u32 BE length | UTF-8 JSON          (decoder socket, RTTY only)
{"type":"weather_report","kind":"synop"|"ship","raw":"10384 41/96 …","line":"10384 · Tempelhof, Germany · …",
 "unreadable":0,"details":["Visibility 4 km", …]}
```

Old clients ignore it: their RTTY branch only acts on 0x01, 0x03 and 0x06 (the app's `DecoderClient.ts`, and the web's
`decoders.ts`), so they keep the raw text exactly as before. Old servers never send it. `docs/PROTOCOL.md` does not
list decoder frames, so the frame is documented in the header comments of both client files and in `weatherReport.ts`.

## What is read

- **FM 12 SYNOP.** After `AAXX YYGGiw`, each report is `iiiii iRiXhVV Nddff [00fff] 1sTTT 2sTdTdTd 3PPPP 4PPPP 5appp
  6RRRt 7wwW1W2 8NhCLCMCH 9GGgg`. Section 2 (`222Dsvs`): sea temperature, measured and wind waves, and `70HwaHwaHwa`.
  Section 3 (`333`): highest and lowest temperature, snow depth, `55SSS` sunshine, 6RRRt, the 24-hour precipitation,
  and the `910ff`/`911ff` gusts. Sections 4 and 5 and the other groups are kept raw only.
- **FM 13 SHIP.** After `BBXX`, each report is `D…D YYGGiw 99LaLaLa QcLoLoLoLo` and then the same sections. A
  five-figure identifier (a buoy or platform) is looked up in the station list.
- **NIL** reports read as "no report (NIL)".
- **Units:** °C, hPa, kt and km/h (or m/s with kt and km/h when `iw` says m/s), visibility in km, cloud in octas.
- **Present and past weather** use short words from the WMO tables: 4677/4561 for manned stations (and iX 4), and
  4680/4531 for automatic stations (iX 7). Codes 00–03 describe cloud development, not weather, so they appear in the
  details but not on the compact line.

## A value is never taken from a malformed group

- A group that is not exactly five characters of figures or `/`, or that breaks its own code table, is **unreadable**.
  Examples: VV 51–55, dd over 36, an iX that rules out the 7-group, a reserved 4680 code. A value outside physical
  limits is also unreadable: air temperature beyond −80…60 °C, sea temperature beyond −3…36 °C, sea-level pressure
  beyond 870…1090 hPa.
- Only that group is skipped. It is counted in the line ("1 group unreadable") and listed in the details as received.
  The rest of the report still reads.
- The whole group is checked before any part of it is used. A malformed visibility also discards the cloud base from
  the same group, because one misread figure could be any of the five.
- **Slots are never guessed.** iRiXhVV and Nddff are positional. If a damaged token occupies the iRiXhVV slot, the next
  group is read as Nddff and nothing else, even when it happens to look like an iRiXhVV.
- **Order.** Section 1's indicator groups come in rising order (1…9). In section 3, only 8 and 9 may repeat. A group
  out of order is unreadable. So if `333` is lost, the section-3 groups after it stay unreadable and are never read as
  section-1 groups.
- **Shift damage is not text.** A lost FIGS shift turns digits into the top-row letters (`RPPYU` = 40067). Inside a
  bulletin, a word made only of `QWERTYUIOP` and `XZVM` therefore counts as a damaged group. It does not end the
  bulletin. It is never converted back to digits.
- **No header heard** (the listener tuned in mid-bulletin): a report is still read if it plainly is one. It must start
  a line or follow an `=`, be five figures followed by a valid iRiXhVV and Nddff, and be ended by `=`. Its line then
  says "time not received" and "46 (unit not received)". A header never carries past NNNN, ZCZC or a new AAXX/BBXX.
- `;` ends a report as well as `=`. DWD sends `=` as the figures-shift V, and this server's ITA2 table prints that as
  `;`.
- A report from which nothing at all could be read is not sent.

## Station names: public domain only

The names come from NOAA NCEI's Integrated Surface Database station history (`isd-history.csv`, `country-list.txt`,
https://www.ncei.noaa.gov/pub/data/noaa/). It is a US Government work and in the public domain. It was fetched on
2026-10-23.

`scripts/gen-synop-stations.mjs --from` filters it into `scripts/synop-stations/wmo-stations.tsv`:

- USAF ids of the form `NNNNN0` (the ISD's spelling of WMO × 10), dropping the `99` and `00` prefixes;
- records ending 2010 or later;
- the latest record for each number;
- names title-cased (only the case changes, never the spelling).

The plain run of the script writes `vibe_synop_stations.h`. `--check` is the stale check in `run-tests.sh`. A number
that is not in the list is shown as the number only.

★ **The source mixes two country alphabets.** Most rows use FIPS 10-4 codes, but some recent rows use ISO 3166.
Heathrow is coded `GB`, which in country-list.txt (FIPS) means Gabon. A code is only trusted in its home WMO block
(the block where most of its rows are), or where it accounts for at least 10 % as many rows as that block's commonest
code. Anything else gives **no country** rather than a wrong one: 189 stations, Heathrow among them ("London
Heathrow"). Codes country-list.txt does not know (such as ISO `DE`) also give none.

**Size (the Pi 2 is the target).** 11,937 stations in 240 countries. That is 278 KB of `__const` in the binary, measured
on one translation unit, and 328 KB as the TSV in the repo. The table is compiled in as read-only data and **the whole
world is kept**. The kernel only pages it in when a lookup touches it: a binary search touches about 14 ids and one
name, so it costs no RAM until a SYNOP is heard, and nothing at all on a server that never decodes one. A file loaded
lazily from disk would add a code path, a file to ship and a way to fail, and still save no RAM. A region filter would
drop stations a listener can really hear (DX reports, and SHIP reports from anywhere). No client carries the table: the
server sends the name.

## Where a WMO table was ambiguous, and how it was settled

- **6RRRt, RRR = 000.** Code table 3590 is clear that 990 means "trace" and 991–999 mean 0.1–0.9 mm. I could not
  confirm from memory whether 000 is a measured nil or unused. It is shown as "0 mm", the reading used by the decoders I
  know. **Check this against the PDF.**
- **tR = 0 or `/`.** Code table 4019 gives periods for tR 1–9. For 0 and `/` no period is stated rather than one being
  guessed.
- **55SSS against 553SS/554SS.** These are told apart by value: SSS runs 000–240 (tenths of an hour), so 553xx and
  554xx are never sunshine for the day.
- **4PPPP against 4a3hhh.** A leading figure of 0 or 9 is a sea-level pressure. Otherwise a3 (1, 2, 5, 7, 8) names a
  standard level and the group is its height. That is shown as such and never as a pressure.
- **VV 90–99** (the sea scale) is accepted from land stations too, because the code allows both.
- **The 4680 gaps.** 06–09, 13–17, 19, 36–39, 49, 59, 69, 79, 88 and 97–98 are reserved, so a report using one is
  unreadable rather than guessed.
- ★ **The code-table words were transcribed from WMO-No. 306 Vol. I.1 (tables 4677, 4680, 4561, 4531, 0509, 0513,
  0515, 4377, 1600, 3850, 4451) by hand, shortened to a few words each. They have not yet been compared line by line
  with the current PDF.** That comparison is worth doing before release.

## Tests (`scripts/run-tests.sh`)

- `vibeserver/test-synop.cpp`: the parser. Covers a WMO-shaped example, a DWD-style bulletin with the station names,
  damage of every kind, SHIP in every quadrant, the no-header rules, text that must never become a report (DWD's
  call-up, RYRY, the frequency lines), stream placement and chunking, bounded memory, and JSON escaping. It also holds
  **the real DDK2 recording's decoded text, pasted verbatim**.
- `vibeserver/test-synop-host.cpp`: DWD-shaped RTTY audio fed into the real `DecoderHost` (FskDecoder and RTTY AUTO).
  Checks that the 0x08 frames arrive, in their places, with the raw text untouched, that the gallery .txt has them
  inline, and that NAVTEX gets none.
- `scripts/test-server-synop.mjs`: **end to end on a real vibeserver.** `fake-rtl-tcp.mjs --rtty` transmits the
  bulletin, a listener runs RTTY AUTO over the decoder socket, and the 0x08 and the gallery file are checked.
- `scripts/test-synop-real.sh`: `tools/rtty-bench/data/ddk-4583khz-2026-10-22.m4a` (ALAC: AAC changed three characters
  of the decode) is run through `rtty_wav`. It checks that the decoder still prints the text the C++ test pastes, and
  that nothing is claimed from it.
- `scripts/test_weather_report.ts`: the client-side checks and the app's markers.
- `scripts/test-web-weather-box.mjs`: the web box in jsdom (placement, collapsed state, text only, the cap).
- `node scripts/gen-synop-stations.mjs --check`: the stale check on the generated station table.

`SYNOP=1 rtty_wav …` prints what the server would translate from any recording.
