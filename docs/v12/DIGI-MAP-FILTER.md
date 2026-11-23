# DIGI MODES MAP — the false-decode filter (V12 beta92)

## RESUME HERE

- **Built and wired (server-side, branch `b92-digifilter`):**
  - `android/app/src/main/cpp/vibe_callsign_entity.h` — the whole ITU call-sign series table (Radio Regulations
    Appendix 42, plus the four series amateurs use outside it: 1A SMOM, 1S Spratly, S0 Western Sahara, Z6 Kosovo), a
    generous region per administration (home + every overseas territory and DXpedition island, antimeridian boxes),
    an Antarctic station list, and `judge(call, grid)` → `kOk` / `kNoEntity` / `kImplausible`.
  - The station store (`vibe_station_store.h`): `spot()` judges every reception. No entity → never kept, counted
    (`falseToday`). Implausible grid → the station is kept, the grid is dropped, counted (`unlocatedToday`). Both
    counts live in the day file's header (`Hdr.falseSpots`, `Hdr.unlocSpots`, 0 in older files) so every radio's
    process adds to one number. Rows written by beta90/91 (record `ver` 1) are judged on read and on their next write;
    rows written since (`ver` 2) are not re-judged (no per-read cost on new files).
  - PSK Reporter (`vibe_spot_report.h note()`): a no-entity call is never reported; an impossible grid is not sent.
  - The live `digital_spot` (`vibe_decoder_host.h`): a no-entity call is not broadcast, not tapped (summary), not
    reported — only counted by the store; an implausible grid is cleared before anything sees it (so a listener's
    own spot list and live map layer never place it either).
  - Background summary cards (`vibe_bgdecode.h Ft8SummaryTap`): a false call is not a spot; a false grid is never
    "furthest".
  - `/vibeserver/stations.json` adds `falseToday` and `unlocatedToday`; a country the app's display table lacks
    (3Y, 9N, ZD7, V3 …) is named from the ITU table.
  - Web digi map TOTALS: "Set aside as false decodes" (only when > 0). "Heard without a locator" already counted the
    unplaced ones.
  - `src/components/AboutOverlay.tsx`: beta92 release note.
- **Not done / for the coordinator:** `vibe_web_page.h` was NOT regenerated in the branch (it was built once to check
  `main.ts` bundles, then reverted, to avoid a 6.7 MB conflict with the other beta92 branches) — run
  `scripts/build-web.mjs` after the merge. The app (iOS/Android) map reads the server's JSON and needs no change; it
  does not show `falseToday` (no TOTALS panel there). R36S: server-side, nothing to do.
- **Open decisions for Stuart:** (1) keep or drop a real-shaped call whose grid is impossible — built as the brief
  says (kept, unplaced); dropping it entirely would remove the last few false decodes that pass the prefix test but
  also a sailor sending an ocean grid. (2) 1S (Spratly) is accepted as an entity, the CW skimmer's own shape check
  rejects it — harmless, but the two could be made one.

## Why

Stuart's Pi 500, 2026-11-22: the 24 h map placed stations in the Southern Ocean — "132XQA" PC46 and "1H7IJH" EB72
(no country), "7Q8FAZ" (Malawi) at OB45, -74.5°, "YE9HMP" (Indonesia) at PC24, -65.5°, "N6LCO" (USA) at OC08, -61.5°
in open sea, "FQ2HMC" PD93 south of Tasmania. PSK Reporter shows no such points.

They are FT8 **false decodes**: the deep/OSD passes try many candidate codewords, and now and then a random one
passes the 14-bit CRC. Its 77 bits then unpack into a call and a grid that are only *shaped* like real ones. A busy
all-band server (many bands, many slots, dig-deeper on) makes enough tries for a few a day to get through.

**FT8's own guards are not weakened.** Nothing here touches the decoder (CRC, LDPC, OSD depth, the AP rules or the
hash table). The filter acts only on what a decoder has already accepted, and only on the call/grid's *meaning*.

## The rule

1. **A real call's prefix is allocated.** Every amateur call starts with a series the ITU allocated to an
   administration. The prefix designator is the call itself, or — for a portable call — the shorter part that is a
   prefix (`EA8/G4ABC`, `G4ABC/EA8`, `VK0/K1ABC`, `MM/W1AW` = in Scotland). Trailing `/P /M /QRP /A /R /B /LH` and a
   district digit (`W1AW/7`) change nothing. Longest series wins (3 characters, then 2, then 1: `3DA` Eswatini vs
   `3D2` Fiji; `SSA–SSM` Egypt vs `SSN–STZ` Sudan). Q, 0, 1 (except 1A, 1S) belong to nobody.
   *Prior art:* the CW skimmer's `ituSeriesOk` (decoders/cw_skimmer.cpp) already rejects calls by the series' shape;
   this is the full table.
2. **A real grid is where that administration can be.** Each administration has boxes covering its home territory
   and every territory, island and DXpedition entity under its series — the USA's take in Alaska (both sides of 180°),
   Hawaii and the NW Hawaiian islands, PR/USVI, Navassa, Guantanamo, Guam/NMI, American Samoa, Wake, Johnston,
   Palmyra, Baker/Howland, Jarvis; France's: Polynesia, New Caledonia, Wallis, the Antilles, Guiana, St Pierre,
   Réunion/Mayotte/the Éparses, Kerguelen, Crozet, Amsterdam, Clipperton; the UK's: Gibraltar, the SBAs, Bermuda,
   the Caribbean territories, Falklands, South Georgia, South Sandwich, St Helena, Ascension, Tristan, Chagos,
   Pitcairn; and so on (Norway: Svalbard, Jan Mayen, Bouvet; Australia: Lord Howe, Norfolk, Willis/Mellish, Christmas,
   Cocos, Heard, Macquarie; NZ: Chatham, Kermadec, Auckland/Campbell, Bounty/Antipodes, Tokelau; Chile: Easter,
   Juan Fernández, San Félix, Sala y Gómez; Japan: Ogasawara, Minami-Torishima, Okinotori; Brazil: Noronha, St Peter &
   Paul, Trindade; Ecuador: Galápagos; Colombia: San Andrés, Malpelo; Costa Rica: Cocos; Venezuela: Aves; South
   Africa: Marion; Mauritius: Agalega, St Brandon, Rodrigues; Kiribati: all three groups and Banaba; Fiji: Rotuma,
   Conway Reef). Each box is padded by 2° of latitude and 3° of longitude, and the grid's centre must fall inside.
   - **Antarctica (south of -60°)** is plausible only for an administration with an Antarctic programme (USA, Russia,
     UK, Argentina, Chile, Australia, NZ, France, Italy, Norway, Japan, China, Korea, Germany, India, Brazil, Uruguay,
     Peru, Ecuador, Poland, Ukraine, Bulgaria, Spain, Belgium, South Africa, Finland, Sweden, Netherlands, Czechia,
     Turkey, Belarus, Pakistan) **and** within 350 km of a station in the list (42 entries: McMurdo, the Pole, the
     King George Island cluster, Rothera, Mawson, Davis, Casey, Dumont d'Urville, Concordia, Syowa, Neumayer, Troll,
     Peter I …). Open sea is never a base: N6LCO at OC08 is ~600 km from Mirny and Casey.
   - `/MM` and `/AM` may be anywhere; so may 4U (UN), 4Y (ICAO) and C7 (WMO).
3. **What happens:** no entity → *not a station* (not kept, not shown, not reported; counted). Implausible grid → the
   station is kept *without* the grid ("heard without a locator"), never placed at a guessed spot.

### The trade-off (said, not hidden)

A maritime station sending its ocean grid with a plain home call (FT8 cannot carry `/MM` and a grid in one standard
message) loses its locator here. It is still a station on the list and still reported, just unplaced.

### What it catches

On 200,000 random FT8-shaped call + grid pairs (`test-callsign-entity`): 19.4 % have no entity, 75.4 % an impossible
grid, **5.2 % pass both** — so about 1 in 20 false decodes can still look real (a random US call with a grid in the
USA). The map's worst cases — a point in an ocean or on the wrong continent — are the 95 %.

## Bounds and cost

Two static tables parsed once (~330 series rows, ~330 boxes, 42 stations; ~20 KB). A judgement is a few hundred
compares: **0.6 µs** on the M5 Pro (`test-callsign-entity`); a store `spot()` with it **0.96 µs** (200,000 spots in
193 ms). Old-file rows are judged on read only while they are `ver` 1; the days before today are cached by the store
as before, so a live map poll re-judges nothing new.

## How it was tested

- `vibeserver/test-callsign-entity.cpp` — **234 / 0.** 166 real call/grid pairs pass (every continent; G/M/2E/GM/GI/GW/
  GU/GD; EA8/EA6/CU/CT3; Russia at Moscow, Kaliningrad, Novosibirsk, Vladivostok, Anadyr, Provideniya (W of 180°),
  Franz Josef Land; W1AW and US states, KH6, KL7 at Anchorage/Adak/Attu (both sides of 180°), KP4, KH2, KH8, KG4, KC4
  at McMurdo, a home call at McMurdo and the Pole; VE/VY0/VO; JA/JD1; VK/VK9N/VK0 Heard, Macquarie, Davis; ZL/ZL7/
  ZL9; 3D2 both sides of 180°; T30/T32; FO Tahiti + Marquesas; FK; LU at Esperanza; CE0Y/CE0Z; PY0F; VP8 Falklands,
  South Georgia, Signy; 3Y0J Bouvet, 3Y Peter I; ZD7/ZD8/ZD9; VQ9; VP6; FT Kerguelen, Crozet, Dumont d'Urville,
  Amsterdam; ZS8; /MM mid-Atlantic, /AM, EA8/ /EA8 /P /7 KH6/ MM/, 4U1UN, 4U1ITU, GB/TM special events, 8J1RL Syowa,
  RI1ANC Mirny, DP0GVN Neumayer, 1A0KM). Stuart's six: the two no-entity, the four implausible; nine more wrong-place
  cases; 39 series names; 8 unallocated series; the allocated-pair count (1054 of 1296); random pairs; cost.
- `vibeserver/test-station-store.cpp` — **116 / 0** (+30): the six from the Pi 500 into two radios' stores; the false
  ones never on the map, the four kept unplaced with their countries; Heard, McMurdo, Bouvet, a /MM placed; the
  PSK Reporter hook hears only the kept, without the bad grid; `falseToday` / `unlocatedToday` unflushed, then on disk
  from both radios; `decodesToday` excludes the false ones; a **synthetic beta91 day file** (ver-1 rows incl. the six)
  read clean, the 48 h map clean, and the next write of an old row drops its grid and makes it ver 2.
- `vibeserver/test-spot-report.cpp` — **76 / 0** (132XQA / 1H7IJH never pending).
- `vibeserver/test-bgdecode.cpp` — all passed (expectations updated: `Q0QQQ`/`Q1ABC` are now false decodes, not
  spots; a 7Q8FAZ at OB45 is a spot but never the furthest). `test-bg-band` 58/0, `test-decoder-hosts`,
  `test-digi-spots`, `test-cw` all passed.
- Every call/grid pair in the other C++ tests was put through `judge()`: only the MSK144 / FST4 protocol vectors
  (`N8BS DH90`, `VK8FDS GL18`, `G4ABC KP20`) are implausible, and those tests check decoded text, not spots.
- **Synthetic only.** No live capture was used; the six examples are Stuart's from the Pi 500's map.
