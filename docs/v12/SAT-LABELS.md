# beta91 — SATELLITE LABELS ON THE WATERFALL

Stuart, 2026-11-22: every satellite in range whose downlink falls inside the current waterfall span gets a label at its
**Doppler-shifted** frequency naming the bird and the downlink ("Meteor-M2-4 LRPT", "ISS FM", "ORBCOMM FM114"),
following the drift, gone at LOS. **Not only in SATELLITE mode** — "i could simply be listening to 2m band and if it
appears we can label it". ORBCOMM (seen at 137.71 MHz beside Meteor-M2-4) is commercial messaging: **a label, nothing else**.

## RESUME HERE

Built and tested on branch `b91-satlabels` (server, web, app) and the directory branch `b91-orbcomm` (main repo).
Not done: the R36S handheld (`handheld-ui` known_labels.h does not read `sat_labels` yet); on-air verification (no live
pass was watched — every test is synthetic); per-listener filtering on the server (each client filters by its own view).

## Design (and why)

**The server computes, the clients draw.** The server already has the orbits, the location and a checked clock;
clients have neither the element sets nor (necessarily) the location. Cheapest bounded design:

- `vibe_sat_engine.h` `Engine::skyLabels(clock, lo, hi)` — every downlink of every bird **above the horizon** whose
  received signal (published frequency + Doppler) falls in `[lo, hi]`, highest first, ≤ 24. One look per bird (and one
  1 s ahead for the rate) every **4 s**, cached and shared by every caller — 7.9 µs a refresh for 22 birds on the M5
  (`test-sat-labels` §5); ~100 birds is ~200 SGP4 steps every 4 s, nothing on a Pi 2. Nothing without a location,
  with NO TIME, for expired elements, or for a downlink SatNOGS marks inactive.
- The shim (`local_sdr_shim.cpp`, spectrum frame loop) builds the message **once per radio** every 4 s for the radio's
  whole capture (`centre ± rate/2`) and sends it on the spectrum socket in its own outbox class (`Out::SatLabels`,
  newest-wins, `teleLast` slot 3): on every change (each 4 s while a bird is up — its Doppler moves), the empty set
  once. Gated by a location, someone listening and the owner's switch.
- Wire: `{"type":"sat_labels","v":1,"t":unix,"sats":[{"id","n","norad","tx","hz","lo"?,"hi"?,"r","el","m","b"?}]}` —
  `hz` received centre, `lo/hi` a passband's received edges, `r` Doppler rate Hz/s, `m` the listening mode, `b` label-only.
- Clients (`src/utils/knownSignals.ts`, shared by app and web): `parseSatLabels` → `satLabelSignals` → the known-signal
  list (`withSatLabels`), so the **existing layout, stacking, crowding and tap rules** apply unchanged. Text is ONE TEXT
  (`satLabelText`): short name + downlink word ("FM repeater" → "FM", a transponder → "linear", "SSTV events" → "SSTV").
  Priority `PRIO_SAT` (5: below the curated table, above every bookmark); label-only birds `PRIO_EIBI`. Colour
  `SAT_LABEL_COLOUR` `#a8d8ff` (sky blue) for text, line and span bar. Dropped 15 s after the last message. A tap tunes
  there (the shifted frequency, the listening mode) — never a decoder, never a Doppler lock.

**Switches:** the listener's Display settings → SIGNAL LABELS (the same switch as every signal label); the owner's
`satlabels` in Setup → Modes and decoders (setup page and the app's server screen) stops the server sending them.

## ORBCOMM (label-only)

> **Superseded 2026-11-24 (docs/v12/ORBCOMM.md):** ORBCOMM is now a REGULAR satellite — the server turns the mirror's
> label-only ORBCOMM birds into ordinary picks with the ORBCOMM decoder (housekeeping decoded, subscriber messages shown
> raw, never decoded). The mirror below is unchanged; a band label keeps the `b` flag only so clients still merge it.

- Directory mirror (`directory/src/sats.js`, `sats-curated.json` on branch **b91-orbcomm** in the main repo):
  `labelGroups: ["orbcomm"]` — asked AFTER amateur + weather in the same round (one more URL, the same
  stop-on-non-200 rules); `constellations` expands every ORBCOMM bird (name prefix, ≤ 60; ≤ 200 birds in the whole
  file, every server's limit) with one transmitter of mode `"label"` and `"labelOnly": true`.
- **Channels:** ORBCOMM satellites change channel orbit by orbit, and there is no authoritative per-satellite list.
  So a bird's own frequency is used only when **SatNOGS DB lists exactly one alive, active 137–138 MHz downlink** for
  it; every listing not alive → the bird is **left out** (no label beats a wrong one); otherwise the bird is named over
  the **137.2–137.8 MHz band** (several such birds up at once = ONE label, "ORBCOMM FM114 +2", over the band).
- Its fetch time goes under `labelOrbits`, so the orbit data's age shown to listeners stays the main groups'.
- Servers before beta91 skip mode `"label"` (an unknown mode → a bird with no usable downlink is not offered).
- The server keeps label-only birds in their own set (`Engine::labelDefs_`): never in `sat_list`, the reach gate, a
  Doppler lock, a decoder or unattended passes. Decision: **not pickable at all** in SATELLITE (listening to ORBCOMM is
  only noise, and the content is commercial messaging); a label tap only tunes there.

## Tests (all synthetic)

| test | what | count |
|---|---|---|
| `vibeserver/test-sat-labels.cpp` (CMake + run-tests.sh) | label birds parsed apart, never offered/locked; ISS Doppler = the lock's model; window, rate, cache, LOS, no location / NO TIME; ORBCOMM channel vs band; the message; cost | 53/0 |
| `vibeserver/test-sat-engine.cpp` | unchanged engine behaviour | 169/0 |
| `vibeserver/test-satcat.cpp` | + the validator takes a sats.json with ORBCOMM | 79/0 |
| `scripts/test_sat_labels.ts` (run-tests.sh) | parse, ONE TEXT, shift + rate, ORBCOMM merge, priority/colour/hint/tap, layout stacking | 29/0 |
| `scripts/test_known_signals.ts` | unchanged | 132/0 |
| `scripts/test-web-sat-labels.mjs` (run-tests.sh; VIBESERVER_BIN) | real server under the ISS on 2 m (not SATELLITE): sat_labels shifted, capture only, moving; the page draws "ISS Voice/SSTV" in sky blue (headless Edge, muted, screenshot); owner switch; no location | 17/0 |
| `directory/scripts/test-sats.mjs` (branch b91-orbcomm) | orbcomm asked last, label birds, SatNOGS channel/band/dead, labelOrbits, a 503 on orbcomm | 81/0 |

Fixtures: `vibeserver/testdata-sat/celestrak-orbcomm-SYNTHETIC-2026-11-22.csv` (plausible ORBCOMM orbits — **not
CelesTrak's data**) and `sats-orbcomm-SYNTHETIC-2026-11-22.json` built from it by the mirror's own code.
