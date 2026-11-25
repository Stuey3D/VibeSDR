# The transponder signal finder (V12 beta94, 2026-11-25)

Stuart, 2026-11-24, RS-44 with CW and SSB contacts all over 435.61–435.67 MHz:

> *"with satellites like rs44 which can host multiple signals we need a voice skimmer, open the satellite in iq mode and as
> soon as a carrier is detected it gives you a button to listen to it"* · *"we wont need a separate vfo for the cw beacon,
> just open a wide iq like we do with orbcomm and any cw gets decoded, any voice gets a button to listen to it"* ·
> *"present it the same way as orbcomm a large wide vfo covering the whole receive range then the vfo a that snaps to the
> chosen voice signal, CW simply decodes"* · *"the little arrows you added for orbcom"* · *"if a user sees a signal that we
> havent detected they can tune to it manually by tapping the waterfall or using the keys/drum as normal"* · *"can we
> compensate for reverse doppler with vfo a so the main doppler tracks the satellite as we do now vfo a tracks the signal?"*

## RESUME HERE

Branch `transponder-finder` (off v12-experimental 2b1de58b7, beta93 shipped). Server, web page and app, tests. **Not merged,
not released. Synthetic only — never tried on a real pass.** Commits: c95ee85e0 (engine + detectors), cb13e3c4e (server
wiring + e2e), ebb7b56d9 (clients), then the docs.

Proven (all synthetic): two SSB voices' suppressed carriers within ~45 Hz beside a steady carrier and hard-keyed CW (neither
mistaken for a voice); an FLL from 40 Hz off a keyed tone to 0.1 Hz; the real server with a synthetic RS-44 clip
(beacon, G4ABC's CQ, a voice): rows, the beacon's CW row, G4ABC decoded, the slice turned by the lock's real Doppler
(−4804 vs −4807 Hz), the voice in the satellite frame within 10 Hz, LISTEN → VFO A follows (measured from the voice), a
manual tune ends it; the web page in headless Edge (rows, LISTEN, the arrows, 390 px).

**Owed on air:** a real RS-44 / FO-29 / AO-7 pass. Watch: does a real SSB voice confirm (≈ 1 s of speech above 6 dB)?
is the carrier estimate right on real speech (the low edge − 300 Hz)? does the follow hold a real station between overs?
does the CW skimmer read the beacon and contacts at their real speeds? CPU on a Pi (the skimmer + one 4096-point FFT
~23 times a second on a ~99 kHz slice).

## What it does

**One pick, one slice.** A linear transponder and its bird's CW beacon (`linkXpdr`, vibe_sat_engine.h — same band, within
30 kHz, the same catalogue mode name: AO-7's Mode B beacon goes with Mode B) are one satellite-frame **span**. Picking the
transponder — or the beacon, which now opens the transponder — attaches the **"xpdr"** decoder with a slice of the span +
the whole Doppler swing + 5 kHz each side (RS-44: ~99 kHz), cut **fixed** on the span's centre (the host's `iqFixed`, as
ORBCOMM's band). The finder turns the lock's Doppler out itself (a phase-continuous mix), so every signal keeps one
satellite-frame place all pass. The beacon is **never** offered in VFO B for these birds and is refused in words if asked.

**Every CW signal** in the span ±3 kHz goes through the CW skimmer we have (decoders/cw_skimmer.h): text, speed, callsign.
The beacon is the ONE CW row nearest the listed figure within 4 kHz (listed figures are approximate).

**Every voice**: `VoiceFinder` (vibe_xpdr.h) — 25 Hz bins, 0.25 s frames, the median bin as the floor, runs ≥ 6 dB over it
400 Hz–4 kHz wide that are spread like speech (a run whose strongest 100 Hz holds over half its energy is a carrier / CW,
never speech), ≈ 1 s of speech to confirm. The **suppressed carrier** = the speech's low edge (3 % of its energy) − 300 Hz
(USB; LSB the mirror). That is where LISTEN tunes the dial.

**Status** `{"type":"xpdr_status","data":{slice,span,beacon,dop,rows:[{id,kind,hz,dial,snr,ago,on,beacon,call,text,wpm}],follow}}`
once a second; rows bounded at 40.

## VFO A follows the person

The slice corrects OUR downlink Doppler from the orbit. A station's own uplink drift is theirs: it depends where they are
and whether they correct. So after LISTEN:

1. **The tune** — the client tunes the dial to the row's `dial` (an ordinary tune: on a linear transponder the passband
   rule re-bases the lock there) and sends `{"type":"xpdr_follow","id":"v3"}` on the decoder socket. The follow WAITS until
   the lock's nominal is within 600 Hz of the row (the tune arrives on another socket), then starts.
2. **First guess** — most stations heard are near us, so their uncorrected uplink sees our range rate:
   `uplinkDriftHz` = −(f_up / f_down) × our downlink Doppler on an inverting transponder (2 m up, 70 cm down: a third of our
   drift, the other way); + on a non-inverting one ("non-inverting" in a note).
3. **Then measured** — a voice: its carrier estimate each frame while they speak; a CW row: the `ToneTracker` FLL.
   Between overs: the last measurement + the guessed drift since. Source shown: wait / guess / voice / cw / held.
4. **Applied** — `Engine::setFollow`: an offset from the nominal (±6 kHz, slewed 150 Hz/s) that is PART of the lock's
   shift, like the trim — so the channel, the skimmer aim, the needle and the sat message's `offset` all move together;
   `follow` appears in the sat message (Hz, part of `offset`).
6. **The digits follow** (DECIDED 11-25, Stuart: "yes") — while `follow` is in the sat message (TRACK / COAST), the
   frequency READOUT (web and app, every readout style) shows where the followed station is in the satellite frame:
   round(`nominal` + `follow`), not the LISTEN point. Both clients run one rule, `SatFollowDial` in
   `src/services/satellites.ts` (web `spectrum.ts` satFollowApply, app `VibeServerWsClient.satFollowApply`):
   - the value is set LOCALLY — readout, the step / drum / bookmark base — and NEVER sent: a tune would go through the
     nudge and end the follow, and a joiner on a shared dial never transmits what it was told. In the app the audio
     socket forwards any frequency that is not the server's own dial (`audioTuneForward.ts`), so `serverDial` carries
     the followed value in the same snapshot;
   - a `config` whose `vfo` is the followed lock's nominal (the server's dial never includes the follow) is not adopted
     over it (`holds`);
   - updated at the sat message's rate, to the hertz, only on a change;
   - the marker is drawn at dial + (`offset` − `follow`) — never counted twice — and the dashed nominal line sits on the
     (following) dial; the Doppler words beside the digits say `offset` − `follow` (the Doppler, not the person's drift);
   - a manual step from the followed position lands relative to where the station is (the step base IS the following
     dial), and ends the follow as before;
   - the follow ends WITHOUT our tune (the finder stopped, `clearFollow` easing the channel back, the lock gone): the
     digits ease back with it and finish on the `nominal` — only if they still show the follow's value;
   - within 1.5 s of THIS client's own tune nothing is touched (a sat message in flight with the old follow).
5. **A manual nudge always wins** — any tune (waterfall tap, steps, drum) goes through `Engine::nudge`, which ends the
   follow and bumps its `seq`; the finder sees the seq move and stops ("the listener tuned").

## Clients

`src/services/xpdr.ts` — one copy for both: the parse (bounded, whitelisted), row order (on air first, voices first),
words, fade after 60 s, LISTEN (voices only — "CW simply decodes"), and the waterfall overlay as an `OrbOverlay` with a
`title` ("TRANSPONDER · every signal found here") drawn by ORBCOMM's own code (web `_drawOrb`, app `WaterfallView`).
Web: `decoders.ts` / `main.ts` / `satellites.ts` (the finder's panel). App: `DecoderClient` / `SDRScreen` /
`SatellitePanel`.

## Tests

- `vibeserver/test-xpdr.cpp` (run-tests.sh) — the catalogue's spans, the voices and their carriers, CW/carrier rejection,
  the FLL, the guess, the Run's rows and the follow (wait → follow → drift → manual tune ends it), the engine's follow on
  RS-44's real elements. `test-xpdr --write-clip FILE RATE CENTRE SEC` writes the synthetic RS-44 clip.
- `scripts/test-server-xpdr.mjs` — the real server with the clip, with and without the lock.
- `scripts/test_xpdr_app.ts` — the clients' module.
- `scripts/test-web-xpdr.mjs` — the web page in headless, muted Edge.
- Changed on purpose: test-sat-engine / test-server-sat (the beacon is the finder's, not VFO B's or the 24 kHz skimmer's).

## Open questions for Stuart

- **PSK Reporter**: CW callsigns heard through the satellite are NOT reported (nor put on the digi map). Should they be, at
  the downlink frequency?
- ~~The pass's gain hold~~ DECIDED 11-25 (Stuart: "gain do what you feel is best"): a transponder pick's finder counts as a
  pass decoder, so the hold measures headroom, sets the gain once and cuts only on a clip (RTL, Airspy R2/Mini, RSP; the HF+,
  USRP, HackRF and relayed streams stay as their owner set them, as for every pass). test-server-xpdr 26/0.
- ~~PSK Reporter~~ DECIDED 11-25: satellite CW stays OFF PSK Reporter (no "via satellite" field; it would read as a direct
  70 cm path). Our own map with "via <bird>" is proposed, not built.
- ~~The dial readout~~ DECIDED 11-25 (Stuart: "yes"): the digits follow — nominal + follow while VFO A follows a
  station (step 6 above). test_xpdr_app (the rule, the audio socket's echo rule) and test-web-xpdr (the real page: digits
  follow, no tune sent, the marker on the channel, a step from the followed position ends it, a stop without a tune
  puts the digits back on the nominal).
- ~~"inverting"~~ DECIDED 11-25 (Stuart: yes, assume inverting): every linear bird we list is inverting today; no
  change — a mirror entry would need "non-inverting" in its note to say otherwise.
