# NDB (V12): every aeronautical beacon on the LF band, decoded at once, idents by consensus

Stuart, 2026-10-26: an NDB skimmer — every carrier in a wide LF slice, its tone AM-detected, the ident CW-decoded and
VOTED across repeats ("never show noise letters as an ident"), matched against the published beacons for name, place,
flag, distance and bearing; harmonics flagged; shared frequencies ranked by ident and distance. Web first, with a map,
the gallery and background-ready decoding.

Branch `v12-ndb`. Engine: `android/app/src/main/cpp/decoders/ndb_skimmer.{h,cpp}`. Published list:
`scripts/gen-ndb-navaids.mjs` → `vibe_ndb_navaids.h` (generated), matching `vibe_ndb_db.h`. Service:
`vibe_ndb_service.h`. Gallery: `vibe_ndb_gallery.h`. Server: `local_sdr_shim.cpp` (NDB MODE beside ISM). Web:
`web/client/src/ndb.ts`, wiring in `main.ts`, `mobile.ts`, `spectrum.ts`. Tests: `vibeserver/test-ndb.cpp`,
`scripts/test-server-ndb.mjs`, `scripts/test-web-ndb.mjs`; bench `tools/ndb-bench`, `scripts/test-ndb.sh`.

Built from ICAO Annex 10 (the NDB, its ident: ≈ 7 WPM, 400 ± 25 or 1020 ± 50 Hz, at least three times in 30 s) and ITU-R
M.1677 (the Morse code — the CW skimmer's own table, `cw::morseText`). No other decoder's source was read.

## Architecture

**One filterbank, every carrier** (the CW skimmer's design, sized for NDBs): a Hann-windowed PFFFT per hop over the
whole slice, ≈ 23 Hz bins, hop = half a window (21 ms; a 7 WPM dot is eight hops). Power and its 1.5 s average in one
SIMD pass (NEON ARMv7-safe / SSE2 / scalar via `VIBE_FORCE_SCALAR`). Nothing is queued: sixty beacons cost the same
transform as one.

- **Carriers**: a local peak of the long average ≥ 3 dB over the floor (30th percentile per 64-bin block, calibrated to
  the mean: 1.00 on noise), seen two seconds running; not a window sidelobe of a stronger carrier, not a data signal's
  hump (DGPS MSK at 283.5–325 kHz — a carrier must be a LINE), not the radio's DC or its IQ image. A peak shaped like a
  stronger carrier's mirrored sidebands must stay above the threshold 15 s without swinging 6 dB (a keyed sideband
  falls in every pause; a real carrier on the 0.5 kHz raster does not).
- **The tone**: the sideband pair in the ICAO windows whose weaker side stands highest over ≈ 10 s (never chosen in the
  first eight seconds: noise reaches 1.15 somewhere on one look). **AM detection in the filterbank, coherently**:
  U·C̃* + L*·C̃ (both sidebands turned by the carrier's own phase from the same frame) — incoherently when another beacon's
  keyed sideband lands on this carrier's bin. A second keyed tone in the other window is a second beacon on the same
  frequency ("four on freq"): a child with its own vote. A1A (keyed carrier) is read from the carrier.
- **The decode** (once a second, on 48 s of envelope): a dot-long moving average (the matched filter); keyed only if
  the marks stand five noise spreads clear of the KNOWN noise level (the floor — a two-means split of noise always finds
  two clusters); threshold half-way in power; runs debounced at half a dot; the dot from the marks' two clusters (4–16
  WPM, learnt only from clear signals); one word per repeat; a word is a ballot only if every element is within 50 % of
  machine timing.
- **The consensus** (`IdentVote`): three identical clean readings (more for readings that say little — "EE" needs six;
  one letter, all-one-letter or all-single-element readings never), or a 70 % letter-by-letter majority of four that
  was itself read whole twice — **or the FOLD**: the repeat period found by autocorrelation and pinned by epoch folding,
  96 s of envelope averaged in step and read in **disjoint groups** (three groups of ≥ 3 repeats, or two) that must
  agree, or two disjoint whole-history windows that agree. A clean fold VETOES a vote of single repeats that disagrees
  (one repeat at a time a weak beacon is misread the SAME way — "OX" as "OTA"); three agreeing groups correct an ident.
- **The list** (`vibe_ndb_db.h`): ✓ match (ident published within ±600 Hz — the nearest if published twice), ≈ one
  letter off, italic candidate (not read yet: the published beacons on the frequency, nearest first — a guess, labelled),
  unlisted (read, not published: shown alone). **Harmonic**: a carrier at twice another's frequency (± 30 Hz).
- **RAM ↔ sensitivity, stated**: 96 s × 2 floats a hop per carrier ≈ 36 KB; a hundred carriers ≈ 3.6 MB. 48 s would halve
  it and lose the fold's deep path (−13 dB 400 Hz drops from 5-6/8 to 2/8). The published list is ≈ 250 KB of .rodata,
  paged in only when NDB runs.

## The mode (server contract)

`{"type":"ndb","on":1|0}` (gated by ask-before-tuning). The server saves the listener's place, puts the hardware centre
on the band (`vibendb::plan`: as much of 190–535 kHz as the radio reaches and its rate holds, **in one slice**; when it
cannot hold it all, where the listed beacons near the server are (`nearbyCentre`: the slice holding the most within
2000 km, 4000 km if under 10 — Stuart, 2026-10-29), centred on Europe's 255–526.5 without a position; x.25/x.75 kHz, between raster beacons; its DC and IQ image are never
carriers) and gives the radio back exactly as found.

**A whole-radio, map-first mode in ADS-B's pattern** (Stuart, 2026-10-27: "NDB should be a map and list view like ADS-B
… that way we can throttle back the waterfall to save server CPU and skim the entire NDB band at once"): no audio (the
chain runs spectrum-only), the waterfall at **1 fps** behind the clients' map (`recomputeEngineRate`, as ADS-B), tune /
mode / passband / zoom / rate / IF filter ignored with a log line. One whole-radio mode at a time. No rate change, ever.

**What one slice covers, per radio** (`ndbLo`/`ndbHi` in `vibeserver.json` say it for the rate running now):

| radio | capture | NDB band in the slice |
|---|---|---|
| Airspy HF+ | 384 / 456 / 768 / 912 kS/s | all of 190–535 kHz (384 kS/s holds 345.6 kHz — just) |
| Airspy HF+ | 192 / 256 kS/s | 172.8 / 230 kHz where the beacons near the server are (192 kS/s: UK ≈ 282–454, Kansas ≈ 242–414 kHz); without a position centred on 255–526.5 (≈ 304–477 / 275–505 kHz) |
| SDRplay RSP | ≥ 2 MS/s | all of it |
| RTL-SDR, direct sampling on or automatic | 2.4 MS/s (any ≥ 384 kS/s) | all of it |
| RTL-SDR without direct sampling, HackRF, Airspy R2/Mini, Icom | — | none: no NDB control at all |

- `vibeserver.json`: `ndb` true / false + `ndbWhy` / **absent** where the radio reaches no LF; `ndbLo`/`ndbHi`; `"ndb"`
  in `modes`; the owner's block list (Setup → Modes and decoders: "NDB beacons").
- `{"type":"ndb",…}` once a second and at once to a joiner: centre, lo/hi, carriers, identified, published/heardIn
  (coverage), far, beacons (hz, snr, tone, a1a, wpm, ident, votes, read, db {k,ident,name,cc,lat,lon,km,brg}, alt, also,
  harm, copy), log (the heard log), copy (the live readings, sequence-numbered, each with its confidence `q`: high = it
  reads the confirmed ident, medium = clean timing, low = timing off, poor = a non-Morse character). `ndb_off`,
  `ndb_error {why}`.
- `{"type":"ndb_find","q","limit"}` → `ndb_found` (published beacons nearest first; in or out of the mode).
  `{"type":"ndb_stats"}` → per hour (heard, furthest), each beacon's SNR every 5 min (in memory, 7 days).
- Health pill: "NDB skimmer" drops. Clean shutdown writes the gallery's day.

**Gallery**: per UTC day, the heard log (a card rewritten at most once a minute, read back after a restart) and a MAP
(PNG, azimuthal equidistant about the receiver, range rings, the map pack's country outlines when installed, the idents).
★ 2026-10-27: and the FT8 summary's two cards — a standing **last 10 minutes** card and one card per **UTC hour** (never
rewritten; partial with its minutes where the run began or ended inside it): beacons with idents, minutes heard, best
SNR, the furthest. Fed by the same `heard()` as the day's log (docs/v12/BACKGROUND-DECODING.md, "NDB and Band digital
spots").
Gallery chip NDB.

**Background decoding** (2026-10-27, Stuart's second pass): the **NDB skimmer is a VFO decoder** — "NDB skimmer
(summary)" (`"dec":"ndb-skim"`) in a background VFO row's decoder list, running BESIDE the profile's other VFOs on the same
capture (his Lenovo RSP at 1200 kHz / 3 MS/s: NAVTEX 489.5 + 517.5, 160 m Band and every NDB at once). Its frequency and
demodulator are greyed out — it covers as much of 190–535 kHz as the RF centre holds (`ndbSegment`; under 100 kHz is
refused in words at the save). The background runner cuts ONE slice off its own FFT and feeds this service in its
fed-slice mode (`Service::startFed` / `feedSlice` / `aimFed` — no tap of its own, a two-second queue that drops the oldest,
a decode thread at decoder priority), with the same gallery cards labelled background. One decoder slot; paused while a
listener's own NDB mode has the band. Offered only where the radio reaches LF (hwinfo `ndbHw`; at the save, by driver — an
RTL only with direct sampling). The first pass's whole-radio profile kind ("Decodes: NDB beacons", `"ndb":true`) is gone;
a saved one is converted on load into a VFO profile centred 362.75 kHz with one NDB skimmer VFO. Admin line: "NDB skimmer
190–535 kHz · running · 12 beacons identified of 41 carriers". Details: docs/v12/BACKGROUND-DECODING.md.

## The web view (map-first, ADS-B's pattern)

`#ndbView` (index.html, styled by #adsbView's own rules extended to it) — `web/client/src/ndb.ts` `NdbView` / `NdbMap`:

- **The top bar**: EXIT NDB (never scrolls away), the title, the identified count, the band and carriers, RINGS, UNHEARD,
  FIT. The page beneath is inert and its waterfall not drawn (as `adsbViewUp`).
- **The map**: our MapLibre map ('aero' profile — the airports and runways the beacons stand at; Leaflet without WebGL 2),
  the NDB layer `web/mapkit/ndblayer.js` (generated copy `web/client/src/generated/ndbLayerSource.ts` by
  `scripts/gen-ndblayer-source.mjs`; never edit the copy): every beacon **at its published position** with a **3-bar
  signal meter** (< 10 dB one red, < 20 dB two amber, else three green — ADS-B's list bars; dimmed when not heard in the
  last minute or not yet identified, the candidate's ident with "?"), the published beacons not heard faint (UNHEARD),
  the receiver's house, a selected beacon's line to the receiver, and autofit to the beacons heard until the map is moved.
  **The range rings are ADS-B's own** — `adsblayer.js` now shares `ringsFor` / `addRingLayers` / `setRingData` /
  `leafletRings` (VibeAdsbLayer.shared), so the ladder, the NM labels on all four sides and the look are one code.
- **The live list** (bottom-left, folds to a pill): its header holds the tiles (CARRIERS · IDENTIFIED · FURTHEST ·
  COVERAGE) and HEARD LOG · FIND NDB · STATS. Rows **lowest frequency first** (the CW skimmer's order), updated in place
  so the list stays where it was scrolled: kHz · ident (✓ match / ≈ one letter off / plain unlisted / italic? candidate)
  · flag + station · **NM + bearing** · the 3-bar meter, and the readings streaming in beneath, each word coloured by its
  confidence in the health pill's four colours. A row tapped selects the beacon on the map and opens its card.
- **The card** (top left, beside the zoom): ident, station, kHz, tone, WPM, NM and bearing, SNR now and its sparkline
  (kept by the page from the reports), the last readings, the other beacons published on the frequency, a harmonic.

## Measured (Apple M5 Pro)

**One beacon, 4 min, 8 draws a cell**, SNR = carrier / noise in 2.5 kHz (`ndb_bench accuracy`) — confirmed right /
wrong, mean time to confirm:

| SNR | 1020 Hz A2A | 400 Hz A2A | A1A |
|---|---|---|---|
| +10 dB | 8/8, 0, 41 s | 8/8, 0, 40 s | 8/8, 0, 40 s |
| 0 dB | 8/8, 0, 40 s | 8/8, 0, 40 s | 8/8, 0, 41 s |
| −5 dB | 8/8, 0, 43 s | 8/8, 0, 42 s | 8/8, 0, 46 s |
| −10 dB | 6/8, 0, 100 s | 8/8, 0, 83 s | 8/8, 0, 49 s |
| −13 dB | 2/8, 0, 211 s | 6/8, 0, 144 s | 8/8, 0, 65 s |
| −16 dB | 0/8 | 0/8 | 1/8, 0 |

5–13 WPM at −5 dB: 7–8/8, no wrong ident. **Noise: 30 min → 0 carriers, 0 idents.** **Forty beacons in 192 kHz**
(−12…+28 dB, 0.5 kHz neighbours, a shared frequency, both tones, A1A, double idents): 37–38 identified, 2–3 missed, 1
wrong — LBA read correctly but ALSO on its own sideband, where the test places another beacon's keyed sideband on
LBA's carrier (1 kHz raster + 1035 Hz tone); a known limit.

**Real**: Stuart's UberSDR, 256–448 kHz I/Q at 192 kHz, 10 min, 2026-10-26 07:29 UTC (daylight). Kept in
`~/VibeSDR-test-audio/ndb/` as its **first 330 s** (253 MB — every one of the eleven idents lands by 303 s), which
test-ndb replays when `VIBE_TEST_AUDIO` is set: the same eleven, nothing else, the first at 17 s. **11 idents, the first six in 17–36 s**: HEN 433.5 (17 s), EME 353.5 (24 s), EMW 393.0
(26 s), WOD 352.0 (28 s), BHX 406.0 (33 s), OX 367.5 (36 s), CHT 277.0 (45 s), EPM 316.0 (130 s), CAM 332.5 (150 s),
SND 362.5 (281 s), MRV 404.0 (303 s). The UberSDR NDB addon's list for the same window held four (WOD, EME, EMW, BHX),
re-identifying WOD at 29 s, EMW at 519 s, EME at 601 s and BHX not at all. Our eleven: nine are published UK beacons at
their frequencies (EME, EMW and SND are real UK beacons no source we may bundle publishes — shown "unlisted"); **MRV 404.0 is
unverified** (in no list we have; the only candidate for a false ident). DGPS humps at 284–291 kHz: no ident since the
narrowness test. 43 carriers.

**CPU**, one M5 core: real 192 kHz recording 0.43 %; synthetic busy slices (`ndb_bench cpu`) 192 / 384 / 600 kHz:
0.12 / 0.26 / 0.42 % NEON, 0.19 / 0.41 / 0.71 % scalar (≈ 1.6× from the SIMD loops + PFFFT SIMD). Plus the WideTap
channeliser on the raw capture (16384-point FFT per ¾ block — at 2.4 MS/s the CW skimmer tap's documented price).
**Pi 2 (Cortex-A7) is an ESTIMATE, not measured**: 20–30× an M5 core → ≈ 8–13 % of one core for a 384 kHz HF+ slice,
plus the channeliser at 2.4 MS/s on an RTL. ARMv7 NEON was not cross-compiled here (no armhf sysroot on this Mac);
x86-64 SSE2 syntax-checks clean.

## Beacon list sources

Stuart, 2026-10-29: *"any public sources please add them with the credits but any hobbyist sources that need me to write
off and ask leave out please."* So the list holds ONLY data we may bundle without asking: public domain, an open licence
(ODbL, CC BY, CC0, Open Government Licence, Licence Ouverte) or official data published for free reuse — each credited.
Non-commercial-only (CC BY-NC, BY-ND: the app goes through app stores), no stated licence, "contact us" licences and
hobbyist beacon databases are left out. `scripts/gen-ndb-navaids.mjs` merges the sources used into
`vibe_ndb_navaids.h`; every record carries its source (`NavaidRec::src`, `Navaid::source()`).

**Used** (licences checked at the source, 2026-10-29):

| Source | Licence | Adds |
|---|---|---|
| OurAirports `navaids.csv` — https://ourairports.com/data/ | Public domain (dedicated by David Megginson) | the base: 6 691 NDBs; 6 362 positions kept, 5 965 in no other source |
| FAA NASR 28-day subscription, `NAV_BASE.csv` (cycle 2026-10-29) — https://www.faa.gov/air_traffic/flight_info/aeronav/aero_data/NASR_Subscription/ | US Government work, public domain (17 U.S.C. §105) | 379 current US/US-administered NDBs (SHUTDOWN dropped): the position for the ≈ 330 OurAirports also has, and 49 OurAirports lacks |
| OpenStreetMap, Overpass `airmark=beacon` + `beacon:type=ndb` — https://www.openstreetmap.org/copyright | ODbL 1.0, © OpenStreetMap contributors — https://opendatacommons.org/licenses/odbl/1-0/ | 561 usable NDBs: 141 in no other source (Russia 38, Sweden 32 — SG, MF —, Brazil 10, France 10, Germany 8, Finland 8, UK 3 incl. NWI Norwich…), 420 merged; 9 frequencies refined (OurAirports gives whole kHz only: MIQ 427 → 426.5, LBA 403 → 402.5, …) |
| Natural Earth admin-0 countries (the map pack's cache) | Public domain | the country of an OSM beacon (point in polygon) |

★★ **ODbL obligations.** The merged table is a *derivative database* of OSM, so it is offered under ODbL 1.0 (the
generated header says so; it is in the public repository, regenerated by the script, which is the "method" ODbL also
accepts). Anything that reads it (servers, app) is a *produced work*: any licence, with the credit "© OpenStreetMap
contributors". Public-domain records may sit in an ODbL database. ★ An OSM element whose own tags cite a hobbyist
beacon list (64 cite one Italian list, `jdg.it`) is left out — the rule above applied to what a mapper copied in; 9
disused/decommissioned elements are left out too.

**The merge**: the same ident, frequencies within 0.5 kHz and positions within 30 km are one beacon (5 km kept two
mispositioned copies: UJ 5.0 km, ZZD 5.9 km apart). Position precedence FAA (current cycle) > OurAirports > OSM; the
name OurAirports' when it has one (mixed case); a finer (half-kHz) frequency beats OurAirports' whole kHz.
**Result: 6 691 → 6 882 beacons.** `NavaidRec` keeps its 28 bytes (`src` sits in the padding); `.rodata` 241 542 → 247 806 bytes
(+6.1 KB, +2.6 %: table 187 348 → 192 696, names 54 194 → 55 110).

**Credit** (`kNavaidCredit`, sent as the NDB report's `source`, printed in the gallery's heard log): *OurAirports (public
domain); FAA NASR (US Government, public domain); © OpenStreetMap contributors (ODbL)*. Also in the app's About
(`AboutOverlay` — OurAirports, FAA NASR, OpenStreetMap entries), the Find NDB note (web `ndb.ts`, app
`NdbLiveList.tsx`), the web client's map-data credit, `README.md` and `website/index.html`.

**Coverage — G4IRX's heard log (UK, 2026-10-28)**: 31 confirmed idents OurAirports alone did not name. **Before 0 / 31,
after 4 / 31** — NWI 342.5 (Norwich, OSM), SG 409 and MF 421 (Sweden, OSM), MIQ 426 (Munich: 427 in OurAirports, 426.5
in OSM). The other 27 are in NO source we may bundle — mostly UK/Irish aerodrome locators: LRN 398, HB 420, CDF 388.5,
NRD 444, SN 430, EMT 419, PHG 424, MVC 327, AX 417, HAW 340, BV 391, NQY 347, BPL 318, EME 353.5, EMW 393, MTZ 354,
NPR 488, WSN 378, SND 362.5, NJ 385, VAT 367, OA 338, BLB 397, EGT 328.5, MRV 404. Likely misreads: **MTM 393** (EMW's
carrier) and **MRV 402.5** (LBA's carrier, 402.5 kHz); **MRV 404** is in no list at all. test-ndb holds the four.

**Not used** (and why):

| Source | Why not |
|---|---|
| UK AIP / NATS digital datasets (nats-uk.ead-it.com; the ICAO AIP dataset holds the UK NDBs) | © NATS Ltd; no open licence stated on the dataset page or its terms; the dataset specification marks data "Not for resale". Not under the OGL — needs NATS's permission. This is where the UK aerodrome locators are. |
| UK Military AIP (No 1 AIDU, aidu.mod.uk) | Crown copyright, free to read; no OGL statement for the AIP; MOD's terms require written permission for anything not released under the OGL. |
| Ireland — AirNav Ireland / IAA eAIP | No open-data release of navaids found; no open licence stated. |
| France — SIA "Données aéronautiques XML" (AIXM 4.5 + XML-SIA, each AIRAC) | ★ **Licence is fine**: SIA *Licence Ouverte* (https://www.sia.aviation-civile.gouv.fr/pub/media/news/file//l/i/licenceopendata-vf_2.pdf) — free reuse incl. commercial, credit "Service de l'Information Aéronautique" + update date. **Not fetched**: the 0 € download goes through the SIA store's checkout, which needs an account. Owed: Stuart downloads one AIRAC zip; an AIXM 4.5 `<Ndb>` reader in the generator would add it (France's NDBs are mostly in OurAirports already). |
| Eurocontrol EAD | Registered access, not an open licence. |
| Australia — Airservices | No open navaid dataset; Geoscience Australia's CC BY 4.0 aviation-facility layers hold buildings, not navaids. |
| Brazil — DECEA / NAV Brasil | No open navaid dataset found; gov.br site content is CC BY-ND 3.0 (no derivatives). (Brazil's NDBs reach us through OSM, where mappers marked them as from official sources.) |
| Canada — NAV CANADA; Germany — DFS; Netherlands — LVNL; New Zealand | Copyright publishers, no open navaid release found. |
| open flightmaps | Its own "OFMA General Users' License"; full text not published on the site ("contact us" for licence requests) — needs asking. |
| openAIP (openaip.net) | CC BY-NC 4.0. Its home page adds that the data may ship inside paid apps (not sold alone), but that is not in the licence itself — a non-commercial licence plus an informal promise is "ask first" by the rule. The likeliest worthwhile ask if Stuart wants one. |
| Wikidata (CC0) | Usable, but only 4 items are NDBs — nothing to add. |
| Hobbyist beacon databases (RNA/REU/RWW by classaxe, ndblist, jdg.it, DX lists) | Hobbyist, permission needed — left out by rule. |

## Tests

- `vibeserver/test-ndb.cpp` (run-tests): 68 checks — the vote's rules, one beacon on both tones and A1A at +10/−5 dB,
  −10 dB by the fold, 5/12 WPM, 5 min of noise, the floor's calibration, the published list (Woodley 93 km 181°, ≈,
  candidates, EME never mismatched, Find NDB; every record's source, the credit, and the G4IRX coverage check — see
  "Beacon list sources"), the service (report, both named, stats, Find marks heard, a harmonic), the
  gallery (card, map PNG, restart). `--write-clip` makes the e2e clip.
- `scripts/test-server-ndb.mjs`: 22 checks on the real server — an RTL without direct sampling offers no NDB; with it,
  the capability, the band, WOD and CAM identified and named (38 s), a tune ignored, the 1 fps waterfall, confidence on
  every reading, Find, stats, the receiver given back exactly.
- `scripts/test-web-ndb.mjs`: 27 checks in headless Edge (`--mute-audio`): the map view, the list (order, NM, bars,
  coloured readings, tiles), the map (markers, NM rings), the card, RINGS, HEARD LOG, Find, stats, the fold, 390 px.
- `scripts/test-setup-bg-ndb.mjs`: the setup page's real profile editor — the NDB skimmer VFO offered on an HF+, an RSP,
  an RTL in direct sampling (HW.ndbHw first), never a HackRF / R2 / Icom / blocked radio, once a profile; its frequency and
  demodulator greyed out; saved as `{"hz":0,"dec":"ndb-skim"}` and read back; an older `{"ndb":true}` profile converted.
- `vibeserver/test-bg-ndb.cpp`: the NDB skimmer VFO end to end — synthetic beacons through the background runner's slice
  into the fed service and the gallery's cards, beside a NAVTEX VFO; slots, a listener's NDB mode, the radio moving.
- `scripts/test_ndb_layer.mjs`: the layer's generated copy, the bars rule, and the rings shared with ADS-B (ladder, NM,
  four labels). `scripts/test_adsb_layer.mjs` (ADS-B's own, 151 checks) still passes on the shared ring code.
- test-ndb also: an older whole-radio background NDB profile converted on parse (written back in the new shape, valid / refused on a radio locked away from the band or one with no
  LF), and the real recording's eleven idents.

## Not done / owed

- **The app** (React Native) — BUILT 2026-10-26 (branch `v12-app-parity-2-ndb`): the capability from the radio's own
  vibeserver.json (`src/services/ndb.ts` parseNdbCapability), the NDB key beside ISM (absent / dimmed with why / live; none
  on Apple TV, which has no WebView), `{"type":"ndb"}` + Find + stats in `VibeServerClient` (ghost guard, heal held, every
  tune/zoom/pan gate holds), the map-first view (`MapOverlay` kind `'ndb'`: `src/generated/ndbLayerSource.ts` from
  `gen-ndblayer-source.mjs`, injected after the ADS-B layer; the layer is given the VibeMapGL instance or Leaflet, never
  the compat adapter), `NdbLiveList.tsx` (tiles, RINGS · UNHEARD · FIT · HEARD LOG · FIND NDB · STATS with SHARE CSV,
  rows lowest first with the readings coloured by confidence, the card with its sparkline), audio released (setQuiet),
  every row/key/dialog by pad and keyboard (a dialog takes Back first), one whole-radio mode at a time, the gallery chip,
  the About note and the Mode tour card. `scripts/test_ndb_app.ts` (174) holds the app's rules to the web's.
  ★ Owed: not yet run on a device against a real NDB server (the page's layer start, the card's placement on an SE and a
  foldable); the stats' per-hour CSVs (the web's other two downloads) — the app shares the beacons' CSV only.
- Weak beacons below −13 dB (2.5 kHz); a strong neighbour's keyed sideband sitting on a weak carrier.
- Stats are in memory only (reset on restart; the gallery keeps the days).
- Real Pi 2 timing; real RTL direct-sampling and RSP captures (only an UberSDR RX888 recording was available).
