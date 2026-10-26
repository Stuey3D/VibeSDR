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
| **NDB skimmer** (`ndb`/`ndbWhy`/`ndbLo`/`ndbHi`; `{"type":"ndb","on"}`) | NDB key beside ISM; map-first like ADS-B: beacons on the map with a 3-bar meter, NM rings (shared with ADS-B), live list streaming readings, Find/stats, waterfall throttled, gallery chip | **missing** (web only — NDB-SKIMMER.md "Not done") | _see Result_ |
| **AIS** (`ais`/`aisWhy`; whole-radio mode) | AIS key; full-screen map + live list (ships, tracks, cards) — `tracks.ts` | **missing** (`src/generated/trackLayerSource.ts` written for it, unused) | _see Result_ |
| **APRS** (`aprs`, `aprsHz`; a listener's decoder) | DECODERS → APRS; stations on the tracks map + list | **missing** | _see Result_ |
| **DSC** (`dsc`; decoder `dsc` {band, center_frequency}) | DECODERS → DSC: channel keys (only those the radio reaches), calls newest first coloured distress/urgency/safety, repeats ×n, MAP on a position, gallery chip | **missing** (gallery filter only) | _see Result_ |
| **PSK31/63** (`psk`; `psk` / `pskskim` {width}) | DECODERS → PSK: ONE SIGNAL / SKIMMER (6/12/24 kHz), CW's list box with the mode in place of WPM, tap = USB with the signal mid-passband | **missing** (gallery chip only) | _see Result_ |
| **JTTY** (`jtty`; decoder `jtty`) | DECODERS → JTTY: band keys (announced calling frequencies this radio reaches), one row per station lowest first coloured by confidence, SPOTS (CQ/QSO), tap = USB dial 1500 Hz below | **missing** | _see Result_ |
| **Digital spots — mode list from the server** (`spotModes`) | list filter, Mode column, map legend from `spotModes`; PSK31/63 filtered as PSK | **hardcoded** `SF_MODES = ALL FT8 FT4 WSPR JS8` (no FT2/JTTY/PSK; a server with fewer still offers them — dead filter keys) | _see Result_ |
| **Digital spots — Band mode** (`spotsBand`/`spotsBandWhy`/`spotBands`; `subscribe_digital_spots` {scope:"band"}) | BAND button (absent / greyed with why / lit); tuning steps bands; readout names the band; drawn VFO = band span; title "DIGITAL SPOTS · 20 M BAND" | **missing** (Single only) | _see Result_ |
| **Band governor chips** (`digital_spots_status`) | per-mode chip in its map colour + letter: ACTIVE / LIGHT / IDLE / SHED / OFF | **missing** | _see Result_ |
| Spot times HH:MM:SS (slot start) | done | done (`src/utils/spotTime.ts`, shared) | — |
| Map coloured by mode, letters | done | done (`src/utils/digiModes.ts`, shared) | — |
| **wM-Bus meters** in ISM 868 (METER chip, line, tap detail, the list aging between 5 s reports, wmGap) | done | done (f0d381150 built web + app together; `src/services/ism.ts` / `IsmPanel.tsx` match `web/client/src/ism.ts` rule for rule) | — (verified) |
| **DRM** (`modes` includes `"drm"`; `{"type":"drm"}` reports on the spectrum socket) | DRM in the mode list where the server offers it; the DRM box (lock, mode/occupancy, SNR, label, text, audio light, "via FAAD2"); station bar shows label/flag/text; no width slider in DRM | **missing** (no DRM mode) | _see Result_ |
| Scanner M1 | app-first (267fdd55) | done | _see Result_ (checked after the later merges) |
| VFO A/B | done (387ff7cd) | done (94bc4954) | _see Result_ (checked: DRM/NDB/AIS refusals) |

## Order of work (biggest user-visible first)

1. NDB skimmer (map-first) · 2. AIS + APRS · 3. DSC · 4. PSK · 5. JTTY · 6. Digital spots: server mode list, Band
mode, governor chips · 7. DRM · 8. Scanner / VFO A/B check. Tour and About copy fixed for each.

## Result

_(filled in at the end of the branch)_
