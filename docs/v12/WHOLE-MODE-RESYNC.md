# Whole-radio modes: the client re-syncs to the server (2026-11-21, beta89 report)

## The report
Stuart, app 622 / iOS 577 and the Mac app: EXIT ADS-B, open SATELLITE and pick — audio fine, spectrum/waterfall blank
until one tune click. Also: after ADS-B, DAB 11A shows the right spectrum scale but the readout stays 1090000 kHz; later
216928 while the SATELLITE box is open; waterfall rows stacked at stale spans/centres.

## Proven cause (app, `src/services/VibeServerWsClient.ts`, hwinfo handler)
On a dial that is NOT shared (a one-listener radio), the "returning socket" re-assert ran on EVERY hwinfo after the first
(`hadSession` is true from the first one on), and the server sends hwinfo on every retune. So every time the SERVER moved
the radio (leaving ADS-B, a satellite pick, DAB, ISM, AIS) the client:
1. re-asserted its own old frequency with `tune()` → stamped `lastLocalTuneAt`;
2. skipped the config carrying the server's new dial (inside the 1.5 s echo window — and that config is sent ONCE);
3. treated the config as an unsolicited "session reset" and re-asserted its old VIEW → the server's view was dragged back
   to the old centre while the VFO sat on the satellite: rows at 96 MHz, audio at 437.8 MHz — "blank until a tune".
Second, smaller hole in BOTH clients: a person's tune followed within 1.5 s by a whole-radio request (or sat pick) had the
server's one config swallowed by the echo window → readout and view stuck (web too: view stayed on the tuned frequency).

## Fix
- App: the hwinfo re-assert runs only on the FIRST hwinfo of a returning socket (`hwinfoThisSocket`).
- App + web: `serverTakesDial()` — every whole-radio request (DAB, ADS-B, ISM, NDB, AIS, datalink, sondes) and a satellite
  pick / drop clears the echo window, so the server's next dial is adopted.
- App: while a whole-radio hold is on, an unsolicited view is adopted, not re-asserted.

## Tested how
`scripts/test_whole_mode_resync.ts` (registered in run-tests.sh): the app's real `VibeServerClient` (React Native stubbed,
JS WebSocket path as on iOS) against the real vibeserver on fake-rtl-tcp with the satellite fixture, shared dial and a
one-listener radio, through ADS-B ↔ normal, ADS-B → SATELLITE, SATELLITE → ADS-B, ADS-B → DAB, DAB → ADS-B, ADS-B → AIS,
AIS → DAB, tune-then-DAB race, DAB → normal, ISM → SATELLITE, SATELLITE → normal. Each step: readout = server's dial; every
row in the last second overlaps the emitted view at the same Hz/bin (WaterfallView's placement rule); view = server
capture; dial inside the view. Plus the web SpectrumClient's tune-then-mode race. 160/160; before the fix 118/134 (the
one-listener radio), and the race steps fail without `serverTakesDial` (app 1, web 6).

## RESUME HERE
- Not covered: the waterfall renderer itself (WaterfallView history re-map on a jump) — the harness checks what the
  client hands it. If Stuart still sees stacked stale blocks with the fix, look at the fan/re-map on a jump of more than
  the width (MOTION_MAX_FRAC) and whether ADS-B's crawl rate (recomputeEngineRate) lingers.
- Server oddity seen, not changed: DAB 11A reports vfo 217.088 MHz (block centre 216.928); DAB → normal after AIS returned
  to an older dial (437.8 MHz). Worth a look by whoever owns dabRestore.
- Web page needs `node scripts/build-web.mjs` (via the lock) and vibe_web_page.h committed before a server release.
