# SATELLITE MODE — the client half (web + app; the R36S has none)

The spec and THE CONTRACT are `docs/v12/SATELLITE-MODE.md` (unchanged here). This is what the clients do with it.

## RESUME HERE
- Branch `sat-client` (off v12-experimental 7a4240cce). Client only; the server half is a parallel branch to the same contract.
- Not yet run against a server that speaks the contract: `scripts/test-web-sat.mjs` (real server, `VIBESERVER_BIN`) has
  been moved to the SATELLITE window but can only pass once the server answers `op:"pick"` with `state:"picked"`.
- No app screenshots: the app was type-checked, not run (no simulator was used). The box is built from DecoderShell's
  tokens, so it takes each faceplate (default / silver / black, transparency on/off) as every decoder box does —
  **check it on a device on all three before shipping** (memory rule: no visible polish gaps).
- First real pass (ISS voice, a METEOR picture) is the coordinator's / Stuart's.

## What each client shows
- **Key**: SATELLITE under TUNES THE RADIO ITSELF beside SONDES (web decoder panel + mode menu grid, app ModeSelector via
  `WHOLE_RADIO_IDS` 'sat'), glyph `satellite` (src/utils/modeIcons.ts). Offered from hwinfo `doppler` exactly as SAT was
  (`satCapFrom`); a refused key is not drawn unless on. Entering leaves any other whole-radio mode; entering another leaves
  SATELLITE (drop sent). The SAT picker and the METEOR LRPT decoder key are gone (LRPT is no longer in any band tab).
- **Window** (`web/client/src/satellites.ts`, `src/components/SatellitePanel.tsx`): AUTO DOPPLER (default ON) · EXIT; the
  synced line (`satSyncedText`); the timetable (`satModeRows` — one row per downlink, up now SHORTEST remaining first, then
  next AOS, no pass last; reach false or a dead orbit GREYED with why, a press says why and sends nothing); the CelesTrak
  credit. The list folds under the pick ("ALL SATELLITES").
- **Pick box** (`satBoxSize`): voice/packet COMPACT — name · downlink, the corrected frequency (`satCorrectedHz`, nominal +
  readout offset), "In range for m:ss" / "Next pass in h:mm · AOS hh:mm", the lock/decoder line, APRS lines (packet), and
  the half-globe (`globeScene`: orthographic, tipped 50° so the receiver sits on the dome; 30° graticule; the bird at
  1 + exaggerated height over its sub-point, footprint + beam cone; no coastlines — no data). Image BIG — the web lends the
  decoder box's own `#decImgWrap` canvas into the window; the app mounts `DecoderImageCanvas` on `decoderImageRef` (the
  decoder box is given no activeDecoder while SATELLITE owns it). ISS 145.800 (`sstv:true`) opens compact; the canvas is
  mounted hidden from the pick and the box grows on the first picture.
- **Wire**: `satPickMsg` / `satDopplerMsg` / `SAT_DROP_MSG`; the pick goes through ask-before-tuning like a tune; the drop is
  sent on every way out (EXIT, the key, another whole-radio mode, the cap disappearing). `parseSatPicked` takes
  `{"type":"sat","state":"picked"}` before the readout parser; a refusal is OFF + why (shown in the window).

## ★ One thing the server agent must know (not a contract change)
The decoder's frames reach a client on its DECODER socket (`/ws/dxcluster`), which the client opens only while a decoder
is attached, and parses binary frames only for the mode it attached. So on `picked` with `decoder` lrpt / sstv / aprs the
client FOLLOWS by sending the same `audio_extension_attach` a decoder key sends (lrpt with its AUTO params; aprs with the
downlink's baud). The server should treat that attach as the decoder it already attached for the pick (a no-op / same
decoder), not as a second slot.

## Tests
- `scripts/test_satellite_mode.ts` (TZ=UTC): 60 — contract parsing, pick reply, order, greying, words, box size, globe.
- `scripts/test-web-satellite-mode.mjs`: 46 — the window in the BUILT page (headless Edge, --mute-audio) against a fake
  sat_list: fetching → re-ask 3 s, none + why, synced, order, greyed press, pick/doppler/drop messages, compact (globe
  inked), big (canvas lent), ISS SSTV growing, APRS lines, 390 px fit. Screenshots `sat-web-*.png` ($SAT_SHOTS).
- `scripts/test_decoder_bands.ts` (SATELLITE in WHOLE_RADIO_IDS, LRPT in no tab), `test_ais_aprs_app.ts` (tour card).
- R36S: `handheld-ui/test/run_offscreen_tests.sh` 8/8 suites; `main.cpp` compiles.
