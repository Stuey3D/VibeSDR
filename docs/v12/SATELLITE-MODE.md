# SATELLITE MODE — one mode for every satellite (Stuart, 2026-11-20)

> "the satellites need a special mode as right now i have no idea how to use the satellites as it stands."
> "this mode replaces all the existing satellite modes that were added as it makes satellite tracking much easier and
> usable for newbies too"

## What Stuart asked for (his words, ordered)
1. **A SATELLITE mode that opens a decoder window** with a **drop-down list of known satellites and their frequencies**.
2. **Click one → the waterfall shifts to it and the correct demodulator / decoder is active.**
3. **AUTO DOPPLER correction — a button, default ON.**
4. **Audio-only satellites** (e.g. the ISS voice channel): the decoder box **shrinks** to show simply
   - the current frequency, with the Doppler correction applied,
   - the orbit details, e.g. **"in range for 09:55" counting down** (or the time to the next pass),
   - optionally **a little half globe with the satellite's position above it and its radio signal drawn as a beam
     coming down from it**.
5. **Image satellites** (APT / LRPT / SSTV …): the same, in a **bigger decoder window showing the image being received**
   if possible; if not, **the final image after decoding**.
6. **The list shows name + frequency, ordered:**
   - **passing now** — the **shortest remaining pass** at the top,
   - then **next passes by time**, soonest first,
   - satellites **outside this SDR's range greyed out** (reach rule: vibe_decoder_reach.h — greyed in this list, as the
     list is a timetable; nothing else offers an unreachable control).
7. **It replaces all the existing satellite modes / keys.**
8. **Where its key lives (Stuart, 2026-11-20): "put it in the tunes the whole radio section too next to ism sondes etc"** —
   SATELLITE is a WHOLE-RADIO mode key, beside DAB / ADS-B / ISM / AIS / DATALINK / SONDES in the mode menu (web
   mobile.ts mode grid, the app's ModeSelector), not a decoder under DECODERS…. Like those, it takes the receiver: the
   pick tunes the radio and Doppler steers it, under the shared-dial contract (a joiner on a shared tuner does not steer).

## Rules it must keep (project rules)
- Orbits come from the directory's mirror (GET /api/sats, directory/src/sats.js) — never CelesTrak from a server/app.
- The app has no hardcoded settings vs a server: list, ranges and capabilities come from the server (hwinfo / sats.json).
- Shared dial contract: on a shared tuner, a listener's pick + Doppler steering obeys the server's dial rules.
- Every decoder off the audio thread; SIMD; potato phones (32-bit, 1 GB) — the globe is cheap (2D canvas, no WebGL).
- Display font only on main controls + VTS; the box uses the decoder box styles.
- Web client (main.ts / mobile.ts), the app (SDRScreen / decoder panels) and the R36S handheld all get it.
- Fix the tours/notes that point at the old satellite keys (AGENTS.md "when a control moves").

9. **Orbit data on demand (Stuart, 2026-11-20): "if the server doesnt have any orbit data it needs to pull from our data
   on open and android needs this too, afterwards every 24h is fine."** — opening SATELLITE on a server with NO orbits
   makes it fetch GET https://vibesdr.net/api/sats (our mirror — never CelesTrak) at once; then the usual daily sync.
   Linux (satcat.cpp) AND Android (app server + Lite). One fetch at a time, the satcat flock / its hold rules kept
   (a 503 from the mirror → retry in 15 min, the mirror's Retry-After).
10. **The box shows when the orbit data was last synced** (e.g. "Orbits synced 16:08 · 2 h ago"; "Fetching orbit data…"
    while the on-open fetch runs; "No orbit data — <why>" if it failed).

11. **NOT ON THE R36S (Stuart, 2026-11-20): "r36s cant have the satellite mode as no network connection to sync the
    orbit data".** The handheld gets no SATELLITE key, its old SAT page and METEOR LRPT key are removed, its engine
    never advertises doppler/satellites and never tries the orbit fetch.

## THE CONTRACT (server ↔ clients) — fixed before the build splits
- `{"type":"sat_list"}` (unchanged request) → the reply adds:
  `source.synced` (unix s of the last good /api/sats fetch, 0 = never), `source.fetching` (bool),
  per bird `inRangeFor` (s to LOS while up, else absent), per tx `use`: `"voice" | "image" | "packet"` and `sstv: true`
  on the ISS 145.800 downlink. `tx.reach` stays; clients GREY a tx/bird with reach false and show `tx.why`.
  An empty catalogue with `source.synced == 0` makes the server start the on-open fetch (item 9) and answer
  `source.fetching: true`; clients re-ask every 3 s while fetching.
- `{"type":"sat","op":"pick","norad":N,"tx":i,"doppler":true|false}` → tune to the tx, lock Doppler (if doppler), attach
  the decoder its `use` needs (image: lrpt / sstv; packet: aprs at its baud; voice: none, but sstv auto on `sstv:true`),
  ALL server-side in one step; gated exactly like `sat lock` (shared-dial contract, ask-before-tune).
  Reply/broadcast: `{"type":"sat","state":"picked","norad","tx","decoder":"lrpt|sstv|aprs|"}` or a refusal with why.
- `{"type":"sat","op":"doppler","on":bool}` toggles the lock on the current pick; `{"type":"sat","op":"drop"}` = unlock +
  detach what pick attached. Leaving the SATELLITE mode sends drop.
- The existing `sat lock/unlock` stays for old clients (one release), then goes.
- ★★★ **beta92 OPEN IN VFO B** (`picked.vfob`, `{"type":"sat_vfob"}`): another downlink of the picked bird on the
  listener's VFO B, decoded and Doppler-corrected from the same pass — `docs/v12/SAT-VFOB.md`.
- ★ **beta90 additions** (band / modeName / decoder / pickPrivate in `sat_list`, `geo` in `sat_track`, lo / hi on
  `picked`, the passband re-base rule, the `fctlm_status` and `sat_tlm` decoder frames, APRS `sat`, the unattended-passes
  setting): `docs/v12/SATELLITE-BEST.md` → "Contract additions (beta90)". Server as built: `SATELLITE-BEST-SERVER.md`.

## What exists today (inventory 2026-11-20, v12 3bc309995)
- **SAT key** (web mode menu + TUNES THE RADIO ITSELF; app ModeSelector wholeKeys; R36S sat_page.h): a picker with passes,
  a polar sky plot (web/app only), DOPPLER, TRACK per downlink. TRACK tunes + `{"type":"sat","op":"lock"}` 400 ms later —
  **it starts no decoder**. Unreachable transmitters are LEFT OUT, not greyed. Order: up now by elevation, then next AOS.
- **METEOR LRPT** decoder key (VHF decoders tab, all three clients): streams a 392 px colour preview (SSTV frames, ≤ 1/s)
  + lrpt_status; full PNGs to the gallery at pass end.
- **SSTV** decoder (generic, covers ISS 145.800): streams lines live; one PNG per picture to the gallery.
- **APRS / packet** (ax25-1200/9600): text; satellite names by callsign (vibe_sat_calls.h).
- **Server engine**: own SGP4 (vibe_sat_orbit.h), passes 24 h (vibe_sat_engine.h), 25 Hz Doppler tracker
  (local_sdr_shim.cpp vibe-sat-track) for a listener or the shared dial, under the shared-dial contract. `sat_list` gives
  per bird: tx[] with reach, az/el/rangeKm, **sub-satellite lat/lon**, passes[] (aos/tca/los/maxEl). hwinfo `doppler`.
- **Catalogue**: 13 birds (sats-curated.json): ISS (437.800 FM rpt, 145.800 voice/SSTV nfm, 145.825 APRS), SO-50, AO-123,
  PO-101 (FM), RS-44, FO-29, AO-7, JO-97 (USB linear + CW beacons), AO-73, MO-122 (USB + bpsk telemetry), SONATE-2
  (9600 + APRS), METEOR-M2-3/-4 (LRPT 137.900). VibeServer (Linux) syncs /api/sats daily; **Android app/Lite servers get
  no sats.json** (no fetch, no JNI to satLoadSats) → their SAT is greyed today.
- **Does not exist**: decoder auto-start on a pick, APT (NOAA 15/18/19 deliberately dropped — retired), a bpsk-telemetry
  decoder, greying, "in range for" without a lock, any globe/orthographic drawing, a satellite icon, a real on-air test.

## Build plan (proposed — for Stuart's OK)
1. **Engine (server)**: `sat_list` adds per tx a `use` = voice | image | packet (from mode/consumer: lrpt → image,
   ax25 → packet, 145.800 nfm "Voice / SSTV" → voice + `sstv:true`), and per bird `inRangeFor` (s to LOS) when up. A new
   `{"type":"sat","op":"pick","norad","tx"}` = tune + lock + attach the right decoder in ONE step (no 400 ms client dance),
   still gated by the shared-dial contract; `op:"drop"` = unlock + detach. Android servers: fetch /api/sats daily like
   satcat.cpp (Kotlin or native) + the missing satLoadSats bridge — else SATELLITE is greyed on every phone server.
2. **One SATELLITE whole-radio key** (web mode menu grid + TUNES THE RADIO ITSELF row, app ModeSelector — NOT the R36S, item 11),
   replacing SAT and METEOR LRPT (LRPT keeps running underneath; the gallery keeps METEOR / SSTV kinds).
3. **The window**: the list (name · frequency · "UP · 6:12 left" / "AOS 16:42 (in 0:38)"), ordered: up now by SHORTEST
   remaining first, then next AOS; unreachable greyed with the reason. AUTO DOPPLER key, default ON.
   - voice / transponder / packet: the compact box — corrected frequency, countdown, half-globe (2D canvas
     orthographic, centred on the receiver, the bird at its sub-point, a beam cone to the ground; ~1 kB of code, no map
     tiles), plus the APRS lines for packet birds;
   - image (Meteor LRPT; ISS SSTV when a picture starts — SSTV auto-sync runs on 145.800): the bigger box with the live
     preview, the full image when the pass ends.
4. **Copy**: AboutOverlay notes (SAT, METEOR LRPT), tutorial.ts mMode card, sdrTour, setup-page hint, decoder hints,
   R36S dec_state.h group — all to say SATELLITE.
5. **Tests**: engine pick/drop + list order + greying; web/app box sizes; a recorded LRPT pass + an ISS SSTV recording
   through the pick path; first real-pass check on Stuart's servers.
