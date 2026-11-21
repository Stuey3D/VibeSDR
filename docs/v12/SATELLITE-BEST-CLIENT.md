# SATELLITE-BEST — the client half (beta90: web + app; no R36S)

The plan and its decisions are `docs/v12/SATELLITE-BEST.md` (items 1.3, 1.4, 1.6–1.9 here); the wire is that file's
"Contract additions (beta90)" section, written by the server branch `b90-satsrv` (8d894336b). This branch: `b90-satcli`.

## RESUME HERE
- **Merge notes for the coordinator**
  - **1.9 MAP opener — repoint at merge.** The web has a placeholder `openDigiModesMap(o)` in `web/client/src/main.ts` (next
    to `satLendImage`) that opens today's APRS TrackView. b90-maps has the real one (`web/client/src/digiMap.ts`
    `openDigiModesMap(DigiMapOpts)`). The object passed is already in its shape: `{ layer: 'aprs', overlay }`, where
    `overlay` is `satMapOverlay()` (label "ISS PASS", `lines` = ground track split at the antimeridian / gaps, `circles` =
    footprint, `points` = the bird). At merge: delete the placeholder and import theirs. The app has the same placeholder
    in `SDRScreen.tsx` (`const openDigiModesMap`, before the main return) → `setAprsMapOpen(true)`; b90-maps has no app
    DIGI MODES map yet, so that one stays until they build it.
  - `android/app/src/main/cpp/vibe_web_page.h` was rebuilt locally for the web test and then **reverted** (not committed);
    rebuild it at release as usual.
  - `fctlm` was added to the decoder types: `src/components/DecoderPanel.tsx` (DecoderType + label TELEMETRY),
    `src/constants/modeBox.ts` (CLIENT_DECODERS), `src/services/DecoderClient.ts`, `web/client/src/decoders.ts` and
    `main.ts` `activeDec`. If the server branch or another branch also adds it, keep one.
- **Not done (honest list)**
  - The app was **type-checked, not run** (no simulator / device). Check on a device: REMIND banner position under the
    status bar on every faceplate (default / silver / black, transparency on/off), the labels' size at SMALL, the RAW key.
  - 1.4's other client half — zooming the view to the passband (`picked.lo/hi`) and marking its edges — was not in this
    brief and is not built. Labels only.
  - The MAP count uses every positioned station on the packet pick; the server now tags `stations[i].sat` ("via ISS") —
    the DIGI MODES map draws that, this branch does not filter on it.
  - The web half-globe's labels are canvas text with a halo; the app's are two `SvgText`s (halo + ink) — no `paintOrder`
    in react-native-svg's types.
  - No live pass. First real check (an ISS / AO-73 / SONATE-2 pass with REMIND set) is the coordinator's / Stuart's.

## What each item does (web `web/client/src/satellites.ts`, app `src/components/SatellitePanel.tsx` + `SDRScreen.tsx`,
## shared words/geometry `src/services/satellites.ts`)
- **1.6 REMIND** (`SatReminders`, `satRemindOffered/Why/Text/Ends`): a REMIND key beside each pass row, drawn ONLY when
  `source.pickPrivate === true`, the row is hearable and its pass is still to come — hidden (not greyed) on a shared dial,
  on an older server (no field) and on greyed / up-now rows. Pressed: "REMINDING · 2 min"; again cancels. Lead 1/2/5/10
  min under the list (web localStorage `vibe.satRemindLead`, app AsyncStorage — per device). The book is in memory for
  the page / app session only, keyed `server|norad|tx|aos`, max 20, never stored, nothing on the server. Every `sat_list`
  moves a reminder to its pass's new AOS (nearest within 15 min). A fresh list is asked ~40 s before one is due (with the
  mode off nobody else asks). Due = AOS − lead, until LOS (a throttled background tab that wakes mid-pass still says "is up
  now"); past LOS dropped unfired; **no connection at fire time → dropped silently**; the session ending / a kick clears
  them (web `showSessionEnded` / `showKicked`, app `onSessionEnded` / `onKicked`); leaving the server = the page / screen
  goes. Re-checked as it fires AND at the tap: shared now → "This radio is now shared — a pass cannot be picked for
  everyone"; reach false → the server's `tx.why`; gone from the list → says so; then no LISTEN. LISTEN = into SATELLITE
  the way its key goes in (other whole-radio modes switched off) + the ORDINARY pick through ask-before-tuning (web
  `spec.satPick` → `askBlocked`, app `askGate`); a held ask keeps the pop-up. Several: newest on top, three shown, "+N
  more"; "picking this ends ISS" when another pick is on. Web: `#satRemind` toasts (decoder-box styles) + a browser
  `Notification` if granted — permission asked on the first REMIND press, never on load; clicking it only focuses the tab.
  App: `SatRemindBanner`, an in-app banner only (Stuart: no OS notification module).
- **1.7 dotted orbit**: the pick asks `{"type":"sat_list","op":"track"}` once for the pass in progress (else the next) and
  again when that changes; `geo` (null entries lift the pen) is drawn at the bird's exaggerated height, front side only,
  dashed (canvas `setLineDash`, svg `strokeDasharray`), the TCA tick at the server's `tca` (`geoT0`/`geoStep`), else at
  the point nearest the receiver. Older server: no `geo`, no line.
- **1.8 the drawing**: `globeBird` — a capsule body, a bowl dish opening towards the globe centre (the beam leaves its
  rim), two ruled panels on a boom along the screen heading from the path (level until a path), about twice the old
  size; receiver dot `GLOBE_RX_DOT` green #5BE36B r 5 with a 1.5 px dark ring (CSS px — scaled by the drawn width);
  grid alpha 0.12 under the coastlines' 0.55; labels (`globeLabels`: server name else RECEIVER below the dot; the bird's
  name above it, else right / left / below; kept off each other, off the bird's exact box, inside the picture; 10 CSS px);
  BIG adds `GLOBE_KEY_TEXT` under the globe (only the "shaded" half until an orbit is drawn).
- **1.4 labels**: `satTxModeLabel` — `modeName · band` ("Mode A · V/a"), the name left out where the row's label already
  says it; an older server's transponder worked out from `upHz` + `hz` (AMSAT letters). On every row's detail line and the
  box's name line.
- **1.3 telemetry**: `fctlm_status` (counts) and `sat_tlm` (a frame's values; also on an `aprs` pick for APRS T# /
  SONATE-2) merged (`satTelemetryMerge`) into a TELEMETRY block: "14 frames · 13 good · 1 failed · Safe" and the values
  with units; `raw:true` shows the count labelled "raw" with no unit; RAW shows the frame's hex. A `fctlm` pick follows on
  the decoder socket with `{"extension_name":"fctlm"}` (no params = AUTO), as lrpt/sstv/aprs do.
- **1.9 MAP**: in the box header for a packet downlink, drawn only once a station with a position has been heard; opens
  the DIGI MODES map's APRS layer with the pass overlay (see the merge note).

## Tests (all silent; headless Edge with --mute-audio)
- `TZ=UTC npx tsx scripts/test_satellite_mode.ts` — **157 passed** (71 before): mode labels; REMIND offer rule, the
  fake-clock book (fires at exactly AOS − lead, moves with a new element set, lead 10, no-connection drop, LOS expiry,
  cap 20, per-server clear, one per downlink), fire-time re-check words, "picking this ends"; telemetry parse / merge /
  raw / hex; geo parse (null gaps, tcaIdx, bound 200), the orbit (overhead TCA over the dot, a low pass far from it, far
  side not drawn, a gap lifts the pen, the server's tca places the tick); the bird (dish towards the centre and panels
  along the heading at five places on the dome; level with no path; ~2× size; beam from the dish); labels (no overlap,
  inside, above a bird that sits on the dot); the MAP overlay (antimeridian / gap split, footprint ~2250 km at 420 km).
- `node scripts/test-web-satellite-mode.mjs` — **90 passed** (55 before) in the built page: everything before, plus the
  track asked once per pick, the dotted orbit adds ink, the green dot is on the canvas, BIG's key line, no REMIND for an
  older server or a shared dial, REMIND with a fake clock (2:01 nothing, 2:00 the pop-up, LISTEN → enter + `op:"pick"`,
  shared-by-then → why without LISTEN, DISMISS, four held → three shown + "+1 more" newest first with "picking this ends
  SO-50", no connection → dropped, `remindClear`), the "Mode B · U/v" row, MAP hidden until a positioned station then the
  overlay handed over, the telemetry block (counts + values, raw without a unit, RAW hex), no empty packet lines under it.
  Two real bugs it caught are fixed (a stray quote dropped the greyed row's class; a telemetry pick showed "no packets
  yet"). Screenshots `sat-web-remind-list|remind-toast|remind-stack|telemetry|…png` in `$SAT_SHOTS`.
- `npx tsc --noEmit -p tsconfig.json`: no errors in `src/` or `web/client/` (the repo's `handheld-ui/test` node-types
  errors are older and outside both).
- Also run green: `test_satellites_app.ts`, `test_ais_aprs_app.ts` (168), `test_platformCopy.ts` (184),
  `test_release_label.ts`, `test_decoder_bands.ts`.

## Cost
Client only: one ≤ 120-point path per pick, a 1 s timer only while a reminder or pop-up exists, one extra `sat_list` a
reminder (free on the server). Nothing measured on a Pi — nothing runs there.
