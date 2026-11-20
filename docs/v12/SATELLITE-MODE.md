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

## Status
Spec recorded 2026-11-20 during beta87. Inventory of the existing satellite modes and the build plan: below (to fill).
