# V12 app parity audit, second pass — 2026-10-26

The first pass (APP-PARITY-2026-10-25.md) brought CW, ISM and the ADS-B chips to the app. Since then the web client
(`web/client/src/`) gained NDB, AIS + APRS, DSC, PSK31/63, JTTY, digital spots' Band mode (with the CPU governor's
chips), wM-Bus meters and DRM; VFO A/B and Scanner M1 were merged alongside. Branch `v12-app-parity-2` (from
`v12-experimental` at 2256e52b). Method as before: each feature read in the web client, the server's wire
(`local_sdr_shim.cpp` vsTunableJson for the capability, the `vibe_*_run.h` for the frames) and its doc, then the app.

★ "App" = the iPhone/iPad/Mac app in `src/` (the Android app shares it). Capabilities are read from the radio's own
`vibeserver.json` (the door owns no radio), never assumed: a key is drawn only where the server names the feature.

## The table

| Feature | Web client | App before | App after (this branch) |
|---|---|---|---|
| **NDB skimmer** (`ndb`/`ndbWhy`/`ndbLo`/`ndbHi`; `{"type":"ndb","on"}`) | NDB key beside ISM; map-first like ADS-B: beacons on the map with a 3-bar meter, NM rings (shared with ADS-B), live list streaming readings, Find/stats, waterfall throttled, gallery chip | **missing** (web only — NDB-SKIMMER.md "Not done") | built (107a4112, 34c85a5d, 8fcb0187, 58f08fd9; merged be462ec7): the key beside ISM (absent / dimmed with why / live; never on Apple TV), `{type:ndb}` + Find/stats, MapOverlay 'ndb' with the shared NDB layer after ADS-B's (NM rings), NdbLiveList (tiles, RINGS · UNHEARD · FIT · HEARD LOG · FIND · STATS, rows lowest first, readings by confidence, card, dialogs — pad/keyboard), audio released, one whole-radio mode at a time, gallery chip, copy; test_ndb_app.ts (174) |
| **AIS** (`ais`/`aisWhy`; whole-radio mode) | AIS key; full-screen map + live list (ships, tracks, cards) — `tracks.ts` | **missing** (`src/generated/trackLayerSource.ts` written for it, unused) | built (2aa9d213 … 05b633aa; merged 8b6e7a39): the key beside ISM (three states; not on Apple TV — no map), `ais`/`ais_trail` with the exit guard, MapOverlay 'ais' on the shared track layer, TrackLiveList (nearest first, safety strip), cards + tracks, EXIT AIS, audio released, leaves/enters against DAB/ADS-B/ISM/NDB and a mode pick, gallery chip, copy; test_ais_aprs_app.ts (146) |
| **APRS** (`aprs`, `aprsHz`; a listener's decoder) | DECODERS → APRS; stations on the tracks map + list | **missing** | built (same commits): CLIENT DECODERS → APRS where `"aprs": true`, PACKET VHF 1200 / HF 300 + TUNE + hint, AprsView (stations, messages, packets, CLR), MAP / a row / Enter = the station map on that station, gallery chip |
| **DSC** (`dsc`; decoder `dsc` {band, center_frequency}) | DECODERS → DSC: channel keys (only those the radio reaches), calls newest first coloured distress/urgency/safety, repeats ×n, MAP on a position, gallery chip | **missing** (gallery filter only) | built (df0375ac, 42b8ace7): CLIENT DECODERS → DSC where `"dsc": true`; CHANNEL keys only where the radio reaches them (lit on the tuned one), a key re-attaches with the new tones; DscView newest first, ×n, corrected/unconfirmed, the web's colours (read from its CSS); MAP on the shared track layer (MapOverlay 'dsc': pins by level, the call's line as card, CALLS list) — not on Apple TV; test_psk_jtty_dsc_app.ts |
| **PSK31/63** (`psk`; `psk` / `pskskim` {width}) | DECODERS → PSK: ONE SIGNAL / SKIMMER (6/12/24 kHz), CW's list box with the mode in place of WPM, tap = USB with the signal mid-passband | **missing** (gallery chip only) | built (df0375ac): CLIENT DECODERS → PSK where `"psk": true`; VIEW + SEGMENT 6/12/24 kHz + the web's hint; CwView shared (PSK31/63 where CW has WPM, the same pin, colours, spots); attach `psk` {centre} / `pskskim` {width}; tap = USB (LSB in LSB) with the signal mid-passband; the skimmer's segment drawn as the VFO |
| **JTTY** (`jtty`; decoder `jtty`) | DECODERS → JTTY: band keys (announced calling frequencies this radio reaches), one row per station lowest first coloured by confidence, SPOTS (CQ/QSO), tap = USB dial 1500 Hz below | **missing** | built (df0375ac): CLIENT DECODERS → JTTY where `"jtty": true`; BAND keys the radio reaches (lit on the tuned one) + hint; JttyView (rows lowest first by confidence, SPOTS CQ/QSO, the pin); tap = USB 1500 Hz below |
| **Digital spots — mode list from the server** (`spotModes`) | list filter, Mode column, map legend from `spotModes`; PSK31/63 filtered as PSK | **hardcoded** `SF_MODES = ALL FT8 FT4 WSPR JS8` (no FT2/JTTY/PSK; a server with fewer still offers them — dead filter keys) | built (b60c6251, a09fd67f): the MODE filter is `spotModes` matched by family (UberSDR keeps its four); the spots map's filter and legend too, and a PSK31/63 spot is PSK's colour; test_spots_band_app.ts |
| **Digital spots — Band mode** (`spotsBand`/`spotsBandWhy`/`spotBands`; `subscribe_digital_spots` {scope:"band"}) | BAND button (absent / greyed with why / lit); tuning steps bands; readout names the band; drawn VFO = band span; title "DIGITAL SPOTS · 20 M BAND" | **missing** (Single only) | built (b60c6251): SPOTS: BAND beside SPOTS: SINGLE in the Mode sheet (absent / dimmed with why under it / lit), `subscribe_digital_spots {scope:band}`, landing band (dial's, last used, 20 m), every tune gesture steps bands (keys, drum, media keys, pad/keyboard through onVfoStep), the band's span drawn, the title "DIGITAL SPOTS · 20 M BAND". **Owed:** the frequency readout naming the band |
| **Band governor chips** (`digital_spots_status`) | per-mode chip in its map colour + letter: ACTIVE / LIGHT / IDLE / SHED / OFF | **missing** | built (b60c6251): a chip per mode in its map colour and letter + ACTIVE/LIGHT/IDLE/SHED/OFF (the web's colours), the state in words for VoiceOver |
| Spot times HH:MM:SS (slot start) | done | done (`src/utils/spotTime.ts`, shared) | — |
| Map coloured by mode, letters | done | done (`src/utils/digiModes.ts`, shared) | — |
| **wM-Bus meters** in ISM 868 (METER chip, line, tap detail, the list aging between 5 s reports, wmGap) | done | done (f0d381150 built web + app together; `src/services/ism.ts` / `IsmPanel.tsx` match `web/client/src/ism.ts` rule for rule) | — (verified) |
| **DRM** (`modes` includes `"drm"`; `{"type":"drm"}` reports on the spectrum socket) | DRM in the mode list where the server offers it; the DRM box (lock, mode/occupancy, SNR, label, text, audio light, "via FAAD2"); station bar shows label/flag/text; no width slider in DRM | **missing** (no DRM mode) | built (42b8ace7): DRM beside AM only where `modes` lists `drm` (OWRX's own drm mapped too); DrmPanel over the waterfall (label, SId, programme · language · flag, text, six lights, SNR by quality, signal line, Audio light + codec + "via FAAD2", the server's why, offset/clock/FAC; tap the title to fold); reports from the spectrum socket, ghosts dropped; no width row in DRM; test_drm_app.ts (27). **Owed:** the station bar (VTS) showing the DRM label |
| Scanner M1 | app-first (267fdd55) | done | checked, no change: openDecoder('scanner') stops the spots (so Band) and VFO B; entering NDB/AIS/ISM puts the box (and the scan) away; the scanner's own mode list has no DRM (as intended) |
| VFO A/B | done (387ff7cd) | done (94bc4954) | checked, no change: the toggle follows the server's `watch` (0 in DRM / NDB / AIS / ISM / ADS-B / DAB, as the web's `watchMax`); the app's extra gates (DAB, Icom, scanner) unchanged |

## Order of work (biggest user-visible first)

1. NDB skimmer (map-first) · 2. AIS + APRS · 3. DSC · 4. PSK · 5. JTTY · 6. Digital spots: server mode list, Band
mode, governor chips · 7. DRM · 8. Scanner / VFO A/B check. Tour and About copy fixed for each.

## Result

Branch `v12-app-parity-2` (from `v12-experimental` 2256e52b), not pushed. NDB and AIS/APRS were built in parallel on
`v12-app-parity-2-ndb` / `-ais` from the audit commit and merged in (be462ec7, 8b6e7a39); the rest on the branch itself.
Commits: 5dc2c5cf (this audit) · NDB 107a4112 34c85a5d 8fcb0187 58f08fd9 · AIS/APRS 2aa9d213 a6c2a66c 4c4e8a87 05b633aa
dfe6be62 · df0375ac (PSK, JTTY, DSC) · b60c6251 (spots: server modes, Band, governor chips) · 42b8ace7 (DRM, DSC MAP,
copy) · a09fd67f (spots map by the server's modes and family) · and this update.

**No web or native source changed** (`web/`, `android/`, `ios/`, `vibeserver/` untouched — `vibe_web_page.h` needs no
rebuild; no Swift/Kotlin/C++ to compile). Everything is TypeScript in `src/` plus tests and docs.

Verified: `npx tsc --noEmit -p .` clean. The 164 JS/TS suites registered in `scripts/run-tests.sh` — **156 pass**; the
8 that fail fail identically on `v12-experimental` (checked in ~/VibeSDR-v12): `test_release_label.ts`,
`test_mac_audio.ts` (known), `test_vfoAB.ts` (a module load error), `test_vibeSquelch.ts` (2), `test_ask_before_tuning.ts`
(2), `test_nav_coverage.ts` (DabPanel's two scan rows), and `test-web-no-desktop-bar.mjs` / `test_chunk_cache.ts` (need
`web/dist`, not built in a fresh worktree). New suites: `test_ndb_app.ts` 174 · `test_ais_aprs_app.ts` 146 ·
`test_psk_jtty_dsc_app.ts` 108 (runs the web's own jtty.ts / dsc.ts modules on the same inputs) · `test_spots_band_app.ts`
41 (the web's spotsReadCap / spotsTitle / status parse cut out of its source and run) · `test_drm_app.ts` 27 (the web's
drmRender on the same reports, its text compared word for word). `check-map-overlay.mjs`, `test_ndb_layer.mjs`,
`test_adsb_layer.mjs`, the ISM / ADS-B / CW app suites and the faceplate suites (modeBox's label list) still pass.

Copy: the Mode tour card (sdrTour) now names DRM beside AM, NDB and AIS with their EXITs, PSK / JTTY / DSC / APRS in
CLIENT DECODERS and SPOTS: BAND; the About V12 note's PSK, JTTY, DSC, DIGITAL SPOTS and DRM paragraphs say where each is
in the app (the "still to come" sentences are gone), with new NDB and AIS/APRS paragraphs. The decoder docs' "app owed"
lines (NDB-SKIMMER, AIS-APRS, DSC, PSK, JTTY, DIGITAL-SPOTS, DRM) say what was built.

Chrome: every new box and key uses the decoder tokens (DecoderShell / useDecoderTokens) or the popup keys (SegBtn,
MetalKey, srvKey), so it follows default / silver / black and transparency on / off as the existing CW and ISM boxes do;
rows wrap (governor chips, DRM lights, signal line) rather than clip at SE-in-Display-Zoom width. Pad / keyboard: the new
keys and settings are NavItems / SegBtns; JTTY's and DSC's rows are the decoder box's list (Enter tunes / opens MAP);
NDB's and AIS's live lists are pad-reachable. **Not seen on a device** — no simulator or device run was made.

### Still owed

- Band mode: the frequency readout naming the band (the web's cardFreqText); the owner's per-mode Band setting in the
  app's own (Android) server screen.
- DRM: the station bar (VTS) showing the DRM label / text / flag (the box shows them).
- AIS on Apple TV (no box — the map is its screen); the AIS GAIN key the web may draw.
- NDB: the web's two per-hour stats CSVs (the app shares the beacons' CSV only).
- A device pass over all of it (themes, transparency, phone + iPad + Mac window sizes, foldables).
- `website/index.html` does not mention the V12 decoders (left alone).
