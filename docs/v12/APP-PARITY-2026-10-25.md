# V12 app parity audit — 2026-10-25

Stuart, 2026-10-25: *"trigger an xcode cloud build to bring my iPhone up to the same spec as the webclient for all the
new stuff we have added"*. Branch `v12-app-parity` (from `v12-experimental` at f00ef133). Read: `git log` since V12 began
(merge-base with main, 5aba7265 — 55 web-client commits, 134 app commits), `docs/v12/*.md`, AboutOverlay's V12 note.

★ "App" = the iPhone/iPad/Mac app in `src/` (the Android app shares it). The web client is `web/client/src/`.

## The table

| Feature | Web client | App before | App after (this branch) |
|---|---|---|---|
| **CW decoder — ONE SIGNAL** (`cw`, the listener's audio) | CW key in DECODERS (only where `vibeserver.json` says `"cw": true`); primary signal's text + the signals list + SPOTS | **missing** (the gallery's CW chip only; the old UberSDR `morse` decoder path exists but is not offered) | see below |
| **CW SKIMMER** (`cwskim`, 24/48/96 kHz segment centred on the dial) | ONE SIGNAL / SKIMMER + WIDTH; list lowest frequency first; each row freq · call · wpm · SNR · confidence · age · recent text; text coloured by confidence (poor #FF4B4B, low #FF8A3D, medium #E8C547, high #5BE36B); SPOTS accumulated under the list; tap a row/spot = tune (CW); the view stays where the listener scrolled; VFO DRAWN as the whole segment (±width/2, drawn only), tuning moves it | **missing** | see below |
| **ISM live mode** (433/868/315/915, device list) | ISM beside DAB/ADS-B; box over the lower waterfall: EXIT · ◀ band ▶ · status · kind filter · units · list newest first | **missing** ("App (not built)" in ISM-DECODER.md) | see below |
| ISM in the gallery | ISM chip | done (79b39e94) | — |
| CW in the gallery | CW chip | done (5dd74407) | — |
| **Audio lets go in ADS-B / ISM** (AirPods followed silence) | `AudioPlayer.setQuiet` while ADS-B or ISM is on (1f0b204a) | **missing** — the native engine keeps the output running in ADS-B | see below |
| ADS-B map as the mode's screen, live list, LABELS, rings, card ▲/▼, trails via radioUrl | done | done (shared `adsblayer.js`, 398cc920) | — |
| ADS-B flight lookup links (ADS-B Exchange · FlightAware · Flightradar24 · Planespotters) | done (7c1fb8fc) | done (same commit, shared layer) | — |
| ADS-B FURTHEST chip | list header | top bar (MapOverlay) | see below |
| **ADS-B NEWEST / LONGEST TRACKED chips** (only aircraft with a position; tap = show it on the map) | list header (14416d1c, 398cc920) | **missing** | see below |
| ADS-B capture warning (< 97 %) | done | done (adsbTypes `adsbCaptureWarning`) | — |
| ADS-B GAIN key + card (AUTO = the server's ramp, TRAINING) | done (19f2d603) | **missing** — no gain control while the map covers the screen | see below |
| ADS-B landing / keep running (owner) | setup page | done (002fc6d6) | — |
| ADS-B 2.56 MS/s owner switch (RTL) | setup page | done (57520a29) | — |
| Health pill "<decoder>: TOO HEAVY" | done (9b67fc96) | done (same commit, HealthPill.tsx) — verified | — |
| Record Buffer (Off/30/60, default 30) | done | done | — |
| Binaural tuning | n/a (app feature) | done | — |
| Transcribe / translate | browser translator in the gallery | done | — |
| Game controllers / one keyboard scheme | done | done | — |
| Gallery in pages, TRANSLATE, NAVTEX station names | done | done | — |
| Background decoding (owner setup page, admin line) | setup page / admin | n/a (server pages) | — |
| Chat in the listener's language | done | done | — |
| Icom receive-only client | done | done | — |
| RSP setup per model, bias-T | done | done | — |

## What this branch builds (biggest user-visible first)

1. **CW** — the whole web CW panel, in the app's decoder box.
2. **ADS-B NEWEST / LONGEST TRACKED** chips beside FURTHEST.
3. **Audio quiet in ADS-B / ISM** — the native output released while nothing is sent.
4. **ISM live mode**.
5. ADS-B GAIN key — if time allows (otherwise listed as owed).

The "App after" column is filled in at the end of the branch (below).
