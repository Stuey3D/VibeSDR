# SATELLITE — the best it can be (plan, 2026-11-21, for Stuart's review — not built)

Starts from SATELLITE-MODE.md (spec + contract), -SERVER.md and -CLIENT.md as built: 13 curated birds; a pick tunes,
Doppler-locks and attaches lrpt | sstv | aprs; CW beacons and the BPSK telemetry downlinks are "heard, not decoded".

## Rules every item keeps
- **Published specs only.** GPL/AGPL decoders may be READ for ideas, never copied; each item names its spec.
- **Receive only.** No uplink or transmit, ever. No VARA. Nothing proprietary or encrypted (Starlink's LoRa frames are encrypted, so they are out).
- **Cheap hardware first** (Pi 3A+ 512 MB, Pi 2): NEON + SSE2 hot loops with a scalar fallback, decoders off the audio
  thread, bounded memory, heavy multi-channel work split across cores. Costs below are estimates and have NOT been measured.
- **The server does the work**; web + app only draw it. No SATELLITE on the R36S.
- **Every decoder has a toggle**: the pick attaches it AUTO, with manual controls underneath as a fallback.
- **Orbits and catalogue come only from our mirror** (/api/sats), never CelesTrak / SatNOGS from a server or the app.
- **No dead controls**: a downlink only gets a decoder key if we actually decode it.

## What is on the air (checked 2026-11-21; ★ = in our catalogue)
| Downlink | Status evidence | Notes |
|---|---|---|
| ★ ISS 145.800 voice/SSTV, 145.825 APRS, 437.800 rpt | AMSAT status reports FM, digi, SSTV, crew in the last week [1] | SSTV runs as events. Series 32 and the 2026-10 event used **437.550 Robot 36** [2] (one bulletin also gives 145.800 [2]) |
| ★ SO-50, ★ AO-123 FM | Both reported [1]. AO-123 also shows **[SSDV]** reports [1] | We do not know AO-123's SSDV format yet (uncertain) |
| ★ PO-101 FM | No reports this week [1] | Runs on a schedule |
| ★ RS-44, ★ FO-29, ★ JO-97, ★ AO-73, ★ MO-122 linear | AMSAT live linear page (2026-02-21) [3]. RS-44, FO-29, JO-97 and AO-73 reported this week [1] | JO-97's telemetry is inoperative [3]. MO-122's beacon is **FoxTelem-format BPSK 1200**, NOT FUNcube [3] |
| ★ AO-7 Mode A (V/a) 29.400–29.500 USB, beacon 29.502 CW | AMSAT [3][4]. Both modes reported this week [1] | Works in sunlight only and swaps A/B on a 24 h timer [3][5] |
| ★ AO-7 Mode B (U/v) 145.925–145.975 | AMSAT [3] | Mode B CW beacon **145.9775** per AMSAT [4]; SatNOGS gives ~145.977 nominal / 145.973 drifted [6]. **Not in our catalogue** |
| TO-108 (CAS-6) U/v 145.915–145.935 | AMSAT [3] | Intermittent (about 2 s on, 5 s off). CW 145.910 is a bare carrier; 4k8 GMSK telemetry on 145.890 [3]. **Not catalogued** |
| ★ AO-73 BPSK 1200 at 145.935 | SatNOGS observations 2026-11-12 and 11-17 [7] | The agreed phase-1 decoder |
| ★ SONATE-2 437.025 9k6 G3RUH AX.25; APRS 145.825 | University page [8]. VHF digi reported [1] | The 9k6 telemetry is **only on over German ground stations** [8]. **SSTV 145.880 Martin M1** is not catalogued [8] |
| AO-91 (Fox-1B) FM + DUV 200 bps | AMSAT status shows [FM] reports [1] | Whether DUV is still sent is uncertain. FoxTelem decodes it (GPL) |
| ★ METEOR-M2-3 / M2-4 LRPT 137.9 | WMO OSCAR and SatNOGS list both as operational; M2-4 had a TX glitch on 2026-05-16 [9][10] | Mode (72k/80k) and frequency (137.1/137.9) change from time to time. We already decode both |
| NOAA-15/18/19 APT | **Decommissioned** June–Aug 2025 [11] | Correctly dropped |
| ISS-band digipeaters (IO-86, NO-44 on 145.825; KNACKSAT-2 9k6) and 9k6 G3RUH cubesats (CroCube, KOSTKA, LASARsat …) | AMSAT live digipeater page [12] | AX.25 at 1200 / 9600, decoders we already have |
| LoRa cubesats (437.x MHz, 125 kHz, SF8 …) | TinyGS newsletters, 2026 [13] | The set changes month to month. Uplink-capable LoRa sats are mostly off [13] |
| GreenCube IO-117 | **Passivated 2024-02-05** (S5Lab) | Out |
| QO-100 (GEO, 10489 MHz) | Active [3] | Needs a dish + LNB, which is not "simple antenna". Out of scope |

## PHASE 1 — next beta (agreed, plus Stuart's transponder asks)

### 1.1 CW beacon auto-decode on a pick (S)
- **User gets:** picking a CW beacon shows its decoded text in the compact box (callsign, CW telemetry), live.
- **Covers:** RS-44 435.605, FO-29 435.795, AO-7 29.502 (Mode A), AO-7 145.9775 (Mode B, once catalogued — 1.5).
  TO-108's beacon is a bare carrier [3], so it gets no key.
- **Spec:** ITU-R M.1677-1 (International Morse code). Engine = our own `decoders/cw_skimmer` via `vibe_cw_run.h`.
- **Server:** for a `mode:"cw"` downlink, `decoderOf` returns `cw` (listener run, centre = the CW pitch). Its `use` becomes
  `"packet"` (text lines). The pick's `decoder:"cw"`. The DecoderHost join rule is unchanged.
  **Client:** the compact box shows the primary text line (as APRS lines do), and the CW toggle sits under it with the
  existing WPM AUTO/manual controls.
- **Pi cost:** one listener CW run, already shipped on every server (single-digit % of a Pi 2 core, est.). RAM < 1 MB.
- **Test:** SatNOGS observation .ogg audio of RS-44 / FO-29 beacons (demodulated audio, decoded through `feedAudio`).
  Synthetic: our CW keyer + a Doppler slope. Plus an on-air RS-44 pass.

### 1.2 BPSK 1200 telemetry decoder — FUNcube format (M)
- **User gets:** AO-73's telemetry decoded every 5 s — frames received, a good/failed count, the satellite's mode, and the
  values (1.3).
- **Covers:** AO-73 (FUNcube-1) at 145.935 [3][7]. Other FUNcube-format birds may be added later if heard. JO-97's
  telemetry is dead [3]. **MO-122 is NOT covered** — it sends Fox-format BPSK [3] (see 3.2). The catalogue's
  `bpsk-telemetry` consumer on MO-122 must be renamed so it is not offered a FUNcube decoder.
- **Spec:** FUNcube-1 "Downlink Data" issue 1.0 as flown, AMSAT-UK, "unlimited public release" [14]. FEC = AO-40
  telemetry coding, P. Karn [15]:
  - Reed–Solomon: two interleaved RS(160,128) words, shortened from CCSDS (255,223); conventional basis; field
    x⁸+x⁷+x²+x+1; roots α^(11j), j = 112…143;
  - scrambler x⁸+x⁷+x⁵+x³+1, seeded all ones;
  - K=7 r½ convolutional code (171/133, G2 inverted), six tail bits;
  - an 80×65 block interleaver with a 65-bit sync vector in row 0 (x⁷+x³+1);
  - 5200 symbols per 256-byte frame.
  ★ These are the same code, field, roots and scrambler as LRPT, so `vibe_lrpt_fec.h` (Viterbi, RS, PN) is reused. The
  only new FEC work is the shortening and the 80×65 de-interleaver + sync correlator.
- **Demod (new, `vibe_bpsk_rx.h`):** from the listener's channel IQ:
  - an NCO / AFC, then an RRC-ish matched filter at 1200 bd;
  - a Costas loop plus Gardner timing;
  - soft symbols into the de-interleaver;
  - polarity / differential handled as LRPT does ("try the ambiguities, keep the one the sync matches").
  Whether FUNcube is differentially encoded must be checked in the spec — **uncertain**.
  NEON/SSE2 on the filter.
  Manual fallback: centre audio Hz, AFC on/off, invert.
- **Server:** consumer `fc-bpsk` → `use:"packet"`, `decoder:"fctlm"`. Frames go out as JSON on the decoder socket
  (whitelisted ASCII / numbers only).
  **Client:** a compact "TELEMETRY" block in the pick box (1.3), behind the `fctlm` toggle.
- **Pi cost:** 1200 bd at ~9.6 kS/s plus one Viterbi over 5132 symbols every 5 s → well under 1 % of a Pi 2 core
  (est.). RAM ~ 100 kB.
- **Test:** an encoder of our own written from [14][15] (as `vibe_lrpt_tx.h` is for LRPT) with noise, Doppler and the
  ambiguities: BER / frame-rate curves. SatNOGS AO-73 observation .ogg (USB audio) for the real signal — test input only,
  not committed (licence to be confirmed). Plus an on-air AO-73 sunlit pass.

### 1.3 Telemetry turned into values (M)
- **User gets:** named values with units — battery V, currents, temperatures, antenna deployed, reboot count, sequence
  number — not hex. Raw is still available behind a "raw" switch.
- **How:** per-mission **layout tables as data** (bit offset, width, name, unit, published calibration).
  - Built into the server for now. Later they can come from the mirror's sats.json, so adding a bird is data, not code.
  - A channel with no published calibration is shown RAW, labelled "raw" — never a guessed conversion.
- **Covers:**
  - AO-73 real-time telemetry (440 bits: EPS, BOB, RF, PA, ANTS, SW — the field list is in [14]). Whether the
    calibration equations are published (FUNcube handbook?) is **uncertain**; otherwise raw counts.
  - AX.25 telemetry on SONATE-2 9k6 — the university publishes its frame format document [8].
  - APRS (ISS, digipeaters): APRS 1.0.1 position / status / telemetry (`T#`) fields, decoded in the box.
- **Spec:** the mission's own published document, per table. satnogs-decoders' Kaitai structs are AGPL/GPL, so they
  may be read to find a doc but never copied (licence **uncertain**; treated as read-only either way).
- **Size:** M (framework + AO-73 + SONATE-2 + APRS). Pi cost negligible (a few frames a minute).
- **Test:** golden frames from our own encoder (1.2); SONATE-2 frames from its published examples, if any (uncertain).

### 1.4 Transponders: labels, AO-7 Mode A on HF radios, and how the pick tunes (S)
- **Label rows by their mode**, as operators know them, e.g. "Mode B · U/v", "Mode A · V/a", "V/u", "U/v".
  - AMSAT notation [3]: uplink band UPPER case, downlink band lower case (A = 10 m, V = 2 m, U = 70 cm, L, S, X).
  - The server derives the letters from `uplink` / `downlink` and sends `tx.band:"U/v"`. Catalogue-only names
    ("Mode A", "Mode B") go in an optional `modeName`.
  - We only receive; the label is how people find the bird.
- **AO-7 Mode A** is already catalogued (29.400–29.500 USB, beacon 29.502) and matches AMSAT's published figures [3][4].
  - Reach works generically (`vsSatReachWhy` → `vsTunableRanges`), so any radio that tunes 29 MHz gets it, e.g.
    Airspy HF+ and RSP.
  - **Check (uncertain):** whether `driverCoverage("rtl")` includes HF direct sampling. Whether a radio that reaches ONLY
    Mode A is offered the SATELLITE key at all (hwinfo `doppler` ← `vsSatAnyReach`: should be yes).
  - Icom HF rigs are refused today ("no channel for the Doppler to move") — an open question for Stuart.
- **HF Doppler is small but not zero:** the worst case at 29.45 MHz is about ±0.6 kHz (AO-7, ~1450 km), against about
  ±3.3 kHz at 145.9 and ±10 kHz at 435.
  - ±0.6 kHz is enough to make SSB unreadable, so AUTO DOPPLER stays ON there too. It costs the same tracker tick.
  - Row note: "Mode A or B — 24 h timer, sunlight only"; we cannot know which mode is on until 2.4 (activity).
- **How a transponder pick should tune (recommendation):**
  - Today: the pick tunes the passband CENTRE. The lock then holds the satellite-frame frequency under the dial
    (`L.nominal`, clamped to the passband). A move < 15 kHz is a "nudge" that re-bases the lock; a bigger one ENDS it
    (`kEndTuneHz`).
  - Recommendation: keep the centre landing, plus show the whole passband (view zoomed to lo–hi + margin) with its edges
    marked. Then the listener **tunes freely inside the passband** while the lock keeps whatever they are on steady.
  - The 15 kHz rule is narrower than the passbands (FO-29 100 kHz, RS-44 60 kHz), so a click across the passband would
    drop the lock. Change it: **any tune that lands inside the Doppler-shifted passband re-bases the lock; only a tune
    outside it ends the lock.**
  - Keep the offset when the listener re-picks the same bird.
  - Whether the waterfall itself slides with Doppler while the dial is held is **uncertain**. It probably does on a
    shared capture — see 2.3.
- **Test:** test-sat-engine cases for the passband re-base rule, band labels, and Mode A reach on an HF-only coverage set.

### 1.5 Catalogue fixes (S, data + engine copy kept in step — test-sat-engine §9)
- **AO-7:** add the Mode B CW beacon (145.9775 per AMSAT [4]; the drift noted). Name the two transponders "Mode A" /
  "Mode B".
- **SONATE-2:** add SSTV 145.880 Martin M1, on its published schedule [8]. `sstv:true` auto-attaches our SSTV decoder,
  which already decodes Martin M1.
- **ISS:** add 437.550 "SSTV events" (Robot 36 is in our decoder) [2]. Uncertain whether this becomes the usual SSTV
  downlink — keep 145.800 too.
- **TO-108 (CAS-6):** add the U/v transponder 145.915–145.935 [3] (voice). Telemetry 4k8 GMSK waits for 2.2.
- **MO-122:** rename its telemetry consumer to `fox-bpsk` (not decoded). **AO-73:** rename it to `fc-bpsk` (decoded,
  1.2).
- Note on SONATE-2 9k6: "Germany passes only".

### 1.6 REMIND on a pass (Stuart 2026-11-21) (M — mostly client, one small contract field)
- **User gets:** a REMIND key on each pass row of the timetable. Shortly before AOS a pop-up asks "ISS · Voice / SSTV
  rises in 2:00 — LISTEN?". Tapping it enters SATELLITE and picks that downlink: tune + Doppler + its decoder, exactly the
  ordinary `op:"pick"`.
  - Lead time: 2 min by default; the choices are 1 / 2 / 5 / 10 min, kept per device.
  - It works only while the user is still connected to that server.
- **Where it is offered.** Only where a pick cannot disturb anyone:
  - a single-user radio;
  - a locked-range radio, where each listener has their own channel inside the range.
  - On a shared dial the key is **hidden, not greyed** (a control that can never act is not drawn).
  - The client must not guess this, so `sat_list` gains `source.pickPrivate: true|false` (the server knows: max users, a
    locked centre, the one-pipeline mode).
  - The key is also hidden on rows that are greyed for reach.
- **Where the reminder lives: the CLIENT only (DECIDED, Stuart 2026-11-21) — no server state for reminders.** A timer in
  the web page / the app's SATELLITE service, in memory, keyed `server + norad + tx + aos`.
  - The reason: if the user leaves, or their soft session limit ends the session, nothing fires, and the next user of
    that radio is never disturbed.
  - **A reminder whose connection is gone never fires.**
  - It is not written to storage, so a page reload or app restart forgets it.
  - A reconnect within the same page / app session may keep it held. When it fires it acts only through a normal pick,
    under the usual rules (single-user / locked-range only, ask-before-tuning).
  - An entry whose AOS has passed is dropped. Max 20 entries, so it stays bounded.
  - The timer works from the pass's `aos` as sent. On every `sat_list` refresh it re-reads the AOS for the same
    norad/tx, so a new element set moves the reminder rather than leaving a stale time.
- **Web:** an in-page toast (decoder-box styles, LISTEN / DISMISS).
  - If the user has granted permission, also a browser `Notification`. Ask for permission on the first REMIND press,
    never on load.
  - Clicking the notification focuses the tab and shows the toast; it never picks by itself.
- **App:** an in-app banner while in the foreground.
  - While backgrounded but STILL CONNECTED, an OS **local** notification is reasonable: it is the user's own request,
    and no push server is needed. It is never scheduled ahead with the OS, because that would fire after the
    connection or session had gone.
  - BUT the app has no notification module today. Adding one (Expo's local-notification package, a permission prompt)
    is a dependency decision for Stuart (**uncertain** whether a backgrounded socket stays connected long enough on
    iOS — likely not, in which case the banner shows on return if still before LOS).
  - Tapping it opens SDRScreen and shows the banner; the pick still needs the tap.
- **Several at once:** each reminder is its own toast/banner, stacked (newest on top, max 3 visible, "+2 more").
  - Two passes that overlap: picking one leaves the other's toast up with "picking this ends ISS" — one receiver, one
    pick.
- **A pass that becomes unreceivable:** at fire time the client re-checks the row.
  - `tx.reach` false, or `pickPrivate` now false (the owner changed the range or the max users, the radio is in another
    whole-radio mode, a listener now shares it).
  - The toast then says why, in the server's `tx.why` words or "This radio is now shared — a pass cannot be picked
    for everyone", and offers no LISTEN.
  - A refusal from the pick itself is shown in the same words.
- **Cancelling:** press REMIND again on the row (it shows "REMINDING · 2 min"), or DISMISS on the toast.
  - Leaving the server, or the session ending, cancels every reminder for that server.
  - A socket drop holds them only until the same page / app session reconnects. At fire time with no connection they
    are dropped silently.
- **Shared-dial contract kept:** the LISTEN tap sends the ordinary pick through ask-before-tuning. The server gates it
  exactly as today, so a reminder can never steer a dial the user could not steer by hand.
- **Test:** `test_satellite_mode.ts` (timer maths across an element-set change, cap, expiry, the hidden-key rule on
  `pickPrivate`). A web headless test with a fake `sat_list` and a fake clock (toast, re-check → why, LISTEN → pick
  message). test-sat-engine for `pickPrivate` on single / locked / shared.

### 1.7 The pass's orbit on the half-globe, dotted (Stuart 2026-11-21) (S)
**What the user gets:** a dotted line on the half-globe along the satellite's path for the current pass (or the next
one, while it is below the horizon): from ~10 min before AOS to ~10 min after LOS, drawn at the same exaggerated height
as the satellite, so how close it comes to the receiver's dot reads at a glance (an overhead pass runs over the dot,
a 10° pass skims the edge). The highest point (TCA) gets a small tick. Front side only (the far side lifts the pen, as
the coastlines do).
**Server:** the existing `{"type":"sat_list","op":"track"}` reply gains `geo:[[lat,lon,altKm],…]` beside `pts` — the
sub-satellite point `look()` already computes (vibe_sat_orbit.h, Bowring). Same bound: one pass, ≤ 120 points; the
window widened by 10 min each side. Free (asks nothing of the radio), like sat_list.
**Client:** satellites.ts `globeScene` takes an optional path and returns `orbit` (a path string, dashed by the
renderer — canvas `setLineDash`, react-native-svg `strokeDasharray`) and `tca`. Asked once per pick and again when the
next pass begins; an older server without `geo` simply draws no line.
**Cost:** nothing measurable (≤ 120 SGP4 steps once per pick). **Test:** test_satellite_mode (an overhead pass crosses
the receiver; a low pass stays near the edge; the far side is not drawn), test-web-satellite-mode (dashes inked),
test-sat-engine (`geo` present, bounded, agrees with `pts`).

### 1.8 A proper satellite on the half-globe (Stuart 2026-11-21) (S)
Our own drawing (not a copied stock icon): a TUBE body, a DISH on its Earth-facing side pointing DOWN at the
sub-point (towards the globe's centre), and TWO solar panels on a boom, the boom lying ALONG the direction of travel so
the bird leans the way its orbit goes (the heading from 1.7's path: the screen direction between the current point and
the next one; until a path arrives, panels level as today). Drawn as paths in globeScene (one geometry for the web canvas
and react-native-svg), the beam cone still from the dish, ~2× today's size so the shape reads at SMALL. Panel cells
ruled, faceplate inks only (amber on default). Test: the panels' axis follows the path's heading; the dish faces the
globe centre at every position on the dome.
**The receiver's dot in a contrasting colour (Stuart 11-21: "can you make the receiver marker green or a different
contrasting colour"):** green #5BE36B (the health pill's / the TRACK dot's), r 5 with a 1.5 px dark ring, on every
faceplate — never the ink colour, which is the dome's and the coastlines'. Same in the web canvas and react-native-svg.
**Labels (Stuart 11-21: "it took me a second or so to realise what the dots and lines were when i first saw them"):** the
receiver's dot gets a small label (the server's name, else RECEIVER) and the satellite its name; the 30° grid is drawn
fainter than the coastlines so land reads first; BIG adds a one-line key under the globe ("dotted: this pass's orbit ·
shaded: where it can be heard"). Labels avoid each other and the globe's edge (bird near the dot → label above).

### 1.9 MAP for a packet satellite's stations (Stuart 2026-11-21) (S)
A packet downlink (ISS 145.825 / SONATE-2 145.825 APRS digipeaters) gets a **MAP** key in the SATELLITE box's header, as the
APRS decoder box has — it opens the ONE DIGI MODES map (Stuart 11-21: the APRS map is combined with the digital spots in
the three VIEWS maps: DIGI MODES · AVIATION · SHIPPING), on its APRS layer: every station heard through the satellite with a position,
the pass's dotted orbit and its footprint on the same map, so it is clear the stations were within the bird's reach;
each station labelled "via ISS" / "via SONATE-2" (the digipeater in the path, vibe_sat_calls.h names it). Drawn only once a
station with a position has been heard (no dead key). The stations also go to the server's APRS store, so the DIGI
MODES map shows them afterwards. Web + app.

## PHASE 2 — make it a satellite station (after the beta lands)

### 2.1 Bigger catalogue from SatNOGS DB (M — directory + server)
- **User gets:** every listed downlink that one of our decoders actually handles (FM voice, AX.25 1200/9600/4800, CW,
  FUNcube BPSK, LoRa, SSTV), not just 13 birds. Curated birds stay first and keep their hand-checked words.
- **Source:** SatNOGS DB transmitters API — **CC BY-SA 4.0** [16]. Credit line in the window beside the CelesTrak one,
  plus share-alike on what we redistribute in sats.json.
  - Fetched only by the directory mirror; servers never contact SatNOGS.
  - Filter: alive + status active + recent observations + a mode we decode (no dead controls). Cap ~150 birds
    (bounded sats.json, est. < 200 kB).
- **Pi cost:** passes for 150 birds over 24 h computed lazily (on `sat_list`, cached per bird for an hour) — est. tens of
  ms on a Pi 2 per refresh. Measure first.
- **Test:** mirror unit test on a saved API reply. test-sat-engine with a 150-bird file: RAM and list-time bounds.

### 2.2 More packet speeds + generic cubesat AX.25 (S–M)
- **4800 GMSK / G3RUH** (TO-108 telemetry, ARICA-2 [12]): `vibe_g3ruh.h` with a rate parameter.
- **Spec:** G3RUH 1988 paper + AX.25 v2.2 (already the sources of our 9600). CCSDS / other framings only where a mission
  publishes them.
- **Test:** our own transmitter at 4k8; SatNOGS .ogg of TO-108 (FM-discriminator audio → `feedAudio`).

### 2.3 Doppler-corrected waterfall + passband marks (M)
- **User gets:** while locked, the waterfall window around the pick is drawn in the satellite's frame. The passband and
  beacons stand still and the S-curve goes away — the single thing that makes linear transponders easy for newbies.
- **Server:** the shift is already known (`dopplerHz`), sent with the spectrum frames as a per-frame display offset (no
  re-FFT, no extra CPU). Clients offset the row.
  The waterfall rules apply: no freeze, no refresh hold, tunes eased.
- **Test:** web headless test with a synthetic Doppler slope (silent), then an RS-44 pass.

### 2.4 "Heard now" activity per downlink (S–M)
- **User gets:** a row says "heard 2 min ago · 14 dB" when the server (any listener) has measured the downlink during a
  pass. This settles AO-7 A vs B and PO-101's schedule.
- **Server:** the lock's own measurement (already used for TRACK/COAST) is stored per tx with a timestamp, in memory and
  bounded.

### 2.5 Unattended passes: schedule, decode, record (L) — ★ DECIDED: a separate feature that pauses background decoding for the pass and resumes it after (see Decided 3)
- **User gets (owner switch, default OFF):** "decode every METEOR / ISS SSTV / AO-73 pass above N°". The server tunes when
  the radio is free, decodes into the gallery, and optionally records narrow baseband. Gallery entries are tagged with the
  pass (time, max el).
- **Rules:**
  - It never takes a dial a listener is on; it yields exactly like background decoding.
  - A shared tuner obeys the shared-dial contract.
  - A locked range limits it to the downlinks that range reaches.
- **Recording:** int8 IQ at the decoder's rate, e.g. LRPT 150 kS/s ≈ 300 kB/s ≈ 180 MB per 10-minute pass. Owner-set cap;
  oldest deleted first; refused on a card with < 1 GB free.
  On a Pi 3A+ decode-only is the default — no recording.
- **Pi cost:** one LRPT decode (measured already in LRPT.md; RAM peaks ~38 MB on the M5 test, Pi figure to be measured).

### 2.6 LoRa satellites on a pick (S–M)
- Our LoRa decoder (ISM mode) is attached for a pick with the lock steering it. SF/BW/CR come from the catalogue
  (SatNOGS DB lists LoRa transmitters).
- **Doppler:** a 125 kHz LoRa signal tolerates the residual after the lock. Frame times should be checked against the
  Doppler rate at SF ≥ 10 (**uncertain**).
- Frames are shown as text/hex plus published per-mission layouts (1.3).
- TinyGS's own list is not used unless its data licence allows (**unknown**).

## PHASE 3 — only if the spec / status checks pass
- **3.1 SSDV images from satellites** — the published SSDV format (P. Heron). Our `balloon_ssdv.h` already decodes it.
  Covers AO-123 if its [SSDV] downlink [1] is standard SSDV (**uncertain**; needs its framing document).
- **3.2 Fox-format telemetry** — AO-91 DUV 200 bps FSK under FM [17]; MO-122 LTM-1 BPSK 1200 [3].
  - 8b10b is the published Widmer–Franaszek code; DUV was designed by P. Karn [17].
  - Build only if AMSAT's documentation [18] gives the bit-level framing and RS parameters (**uncertain**).
  - FoxTelem is GPL — read only.
- **3.3 CW telemetry decoding** (FO-29, AO-7 beacon channels into values) — only from published JARL / AMSAT beacon
  formats (**uncertain** they are still accurate).
- **3.4 Sky-plot extras** — pass track drawn on the half-globe and the next 3 passes as arcs (client only, 2D canvas).

## Not doing (and why)
- **NOAA APT:** retired [11].
- **METEOR HRPT, GOES / Himawari, QO-100:** a dish is needed.
- **Starlink 137 MHz LoRa:** encrypted [13].
- **Any uplink or transmit.**
- **Proprietary links:** VARA, D-STAR DV on birds (AMBE), anything licensed.

## Test sources (public)
- **SatNOGS Network observations:** per-observation **demodulated audio (.ogg)**, waterfall PNG and decoded frames;
  **no IQ** is uploaded [19].
  - Good for CW, FUNcube BPSK (USB audio), AFSK 1200, and 9k6 / 4k8 (FM-discriminator audio at 48 kHz → `feedAudio`).
  - A community bulk mirror (~1 TB, json + png + ogg) exists [19].
  - Data licence for observations: **unconfirmed** — test input only, never committed.
- **IQ:** some station owners publish SigMF archives (search per bird). The sigidwiki METEOR-M N2 LRPT IQ (2018) is
  already in ~/VibeSDR-test-audio/lrpt.
- **Our own transmitters** (as `vibe_lrpt_tx.h`): FUNcube/AO-40 encoder, CW keyer, 4k8 G3RUH — BER curves with noise,
  Doppler slope, clock error.
- **On air:** Stuart's servers — an RS-44 pass (CW + linear), an AO-73 sunlit pass, an AO-7 Mode A pass on an HF radio,
  a METEOR pass through `pick`.

## Decided (Stuart 2026-11-21)
1. **Icom: yes, by reach.** Satellites are shown only to a receiver that can hear them, so an Icom HF rig shows AO-7's
   Mode A (and any HF downlink) and picks it — the lock steps the Icom's VFO, receive only. No Icom-specific refusal.
2. **REMIND in the app: an in-app banner** (no OS notification module).
3. **Unattended satellite passes = a SEPARATE feature that interrupts background decoding** (Stuart: "a user can specify
   that they want to receive meteor passes and when a pass is happening the current background decode is paused it goes
   to the meteor pass runs it then switches back to normal background decoding afterwards"). The owner picks which
   satellites / downlinks to catch (e.g. METEOR-M2-3 and -4 LRPT). When one of those passes begins while the radio is in
   background decoding (nobody listening), the server PAUSES the current background decode, picks the pass (tune +
   Doppler + decoder, results to the GALLERY), and at LOS RESUMES exactly the background decode it paused. A listener
   arriving mid-pass gets the radio as now (the pass is given up, as any background decode is). Its own setting, beside
   Background Decoding (setup page + app + Android remote setup); off until the owner picks a satellite. Optional
   capped recording stays an owner switch, off by default on a Pi.

4. **Idle timeout during a pass: yes, as WEFAX** — a listener on a picked pass counts as active until LOS, so an owner's
   IDLE timeout does not drop them. The hard and soft time limits still apply unchanged; this only touches idle timeouts,
   where an owner has set one.

## Still open (my recommendation in brackets)
1. 1.4 passband rule: any tune inside the passband keeps the lock (yes — otherwise FO-29's 100 kHz cannot be tuned).
2. 1.6 REMIND ships in the phase-1 beta (yes).

## Sources
[1] https://status.amsat.org/ (reports 2026-11-16 → 21) · [2] AMSAT ANS-109/ANS-130 bulletins https://www.amsat.org/ans-130-amsat-news-service-weekly-bulletins/ ,
https://community.libre.space/t/iss-sstv-event-from-2026-04-10-to-2026-04-14/14542 · [3] https://www.amsat.org/live-linear-satellites-2/ (2026-02-21) ·
[4] https://www.amsat.org/?p=1031 , https://www.amsat.org/?p=5033 · [5] https://www.arrl.org/news/venerable-ao-7-satellite-approaching-a-return-to-full-solar-illumination ·
[6] https://db.satnogs.org/satellite/7530 · [7] https://db.satnogs.org/satellite/39444/ · [8] https://informatik.uni-wuerzburg.de/en/aerospaceinfo/staff/kayal/research-activities/sonate-2/information-for-radio-amateurs ·
[9] https://space.oscar.wmo.int/satellites/view/meteor_m_n2_4 , https://db.satnogs.org/satellite/57166/ · [10] https://community.libre.space/t/observation-14082475-meteor-m2-4-59051-lrpt-tx-glitch/14784 ·
[11] https://www.nesdis.noaa.gov/news/legacy-orbit-noaa-decommissions-the-poes-satellite-constellation · [12] https://www.amsat.org/live-digipeater-satellites/ ·
[13] https://tinygs.com/newsletters/2026-07-12-weekly-newsletter , https://tinygs.com/newsletters/2026-02-15-weekly-newsletter ·
[14] https://funcube.org.uk/wp-content/uploads/2010/11/funcube-1-downlink-data-spec_ver1-as_flown.pdf · [15] http://www.ka9q.net/papers/ao40tlm.html ·
[16] https://db.satnogs.org/about , https://docs.satnogs.org/projects/satnogs-db/en/latest/api.html · [17] https://amsat.org/?p=3177 ·
[18] https://www.amsat.org/wordpress/wp-content/uploads/2018/06/AMSAT-Fox-Documentation.pdf · [19] https://wiki.satnogs.org/Artifacts , https://community.libre.space/t/database-download/5516/6

## Contract additions (beta90) — server branch `b90-satsrv`, written FIRST for the client agent (`b90-satcli`)
Everything below is ADDITIVE: an older client ignores the new fields; a new client facing an older server sees them
absent and draws what it drew before. All strings that came off the air are whitelisted to printable ASCII on the server.

### `sat_list` (reply) — new fields
```
source.pickPrivate   true | false — a pick here disturbs nobody: a single-user radio, or a locked-range radio where
                     each listener has their own channel (1.6 REMIND is drawn ONLY when true; hidden, not greyed).
                     ABSENT on a server with no satellites at all.
tx.band              "U/v" | "V/u" | "V/a" | "U/u" … — AMSAT notation: uplink band UPPER case, downlink lower case
                     (A 10 m · V 2 m · U 70 cm · L 23 cm · S 13 cm · X 3 cm). Only when the downlink has an uplink
                     (a transponder / repeater); absent on a beacon, telemetry or a weather downlink.
tx.modeName          "Mode A" | "Mode B" … — the catalogue's own name for that transponder, when it has one.
                     ★ Label a row: modeName && band → "Mode B · U/v"; band alone → "U/v"; neither → nothing.
tx.decoder           the decoder a pick attaches: "lrpt" | "sstv" | "aprs" | "cw" | "fctlm" | "" (none: heard only).
                     The same answer as `picked.decoder` will give (barring a refusal).
tx.use               unchanged words; a CW beacon is now "packet" (text lines), FUNcube telemetry "packet".
```

### `sat_list` `op:"track"` (reply `{"type":"sat_track",…}`) — new fields (1.7)
```
geo        [[lat, lon, altKm], …]  sub-satellite points (deg, deg, km), ≤ 120 points, evenly spaced in time from
           geoT0 to geoT1. Present only when the server knows the bird's orbit and has a location.
geoT0      unix s — AOS − 600 (the dotted path starts 10 min before AOS)
geoT1      unix s — LOS + 600
geoStep    s between consecutive geo points (geo[i] is at geoT0 + i·geoStep)
tca        unix s of the highest point of that pass (the TCA tick) — the pass's own tca, from the server's passes
```
`pts` (az/el AOS→LOS) is unchanged.

### `{"type":"sat","state":"picked",…}` — new fields
```
decoder    may now also be "cw" (CW beacons, 1.1) or "fctlm" (FUNcube BPSK 1200 telemetry, 1.2)
lo, hi     the downlink's passband edges (Hz, satellite frame) — only on a transponder pick; the client may zoom the
           view to lo–hi + margin and mark the edges. ★ PASSBAND RULE (1.4): while locked, ANY tune that lands inside
           lo − |Doppler| … hi + |Doppler| re-bases the lock on the new frequency (the lock continues); only a tune
           OUTSIDE it ends the lock. Single channels keep the old 15 kHz nudge rule.
band, modeName   as in sat_list (for the box's header)
```

### Decoder socket — new JSON text frames (the pick's decoder host; a decoder socket open for the session receives them)
**FUNcube telemetry (`fctlm`, 1.2)** — status, at most once a second while it runs:
```
{"type":"fctlm_status","data":{"sync":true|false,     // a sync vector found in the last 10 s
   "frames":N,          // frame periods with a sync seen
   "good":N,            // frames whose Reed–Solomon decode succeeded
   "failed":N,          // sync seen, RS could not correct
   "afcHz":-123.4,      // the carrier offset the demodulator is holding (audio Hz from its centre)
   "snrDb":7.5,         // the demodulator's estimate, symbol-rate SNR (approx)
   "centreHz":1200,     // the audio centre it listens on (manual fallback: "centre" on attach)
   "auto":true}}        // AFC on (manual fallback: "afc":false on attach)
```
Attach parameters (the manual fallback under the AUTO toggle): `{"type":"audio_extension_attach","extension_name":
"fctlm"[,"centre":<audio Hz, 300–2700, default 1200>][,"afc":true|false][,"invert":true|false]}`.
The pick attaches `fctlm` with NO parameters (AUTO).

**Telemetry values (1.3)** — one per good frame, from any decoder that has a layout for it (`fctlm` for AO-73, `aprs`
for APRS `T#` and SONATE-2 9k6 frames):
```
{"type":"sat_tlm","data":{
   "sat":"AO-73",                 // the satellite's name (the pick's, else from the callsign table)
   "format":"funcube-rt" | "aprs-t" | "sonate2-ax25",
   "t":1763700000,                // server's unix s when decoded
   "frame":"RT1" | …,             // the frame's own type / sequence word where the spec has one
   "seq":12345,                   // the spacecraft's sequence number where the spec has one, else absent
   "mode":"Safe" | …,             // the spacecraft's mode where the spec has one, else absent
   "values":[ {"k":"eps.vbat","name":"Battery voltage","v":8.13,"unit":"V"},
              {"k":"eps.tbat","name":"Battery temperature","v":143,"unit":"","raw":true}, … ],
   "hex":"…"}}                    // the decoded payload bytes, hex (≤ 512 chars), for the "raw" switch
```
`raw:true` = no published calibration: `v` is the raw count and the client must label it "raw" (no unit).
A value with a published conversion carries its unit and no `raw`. Order of `values` = display order. ≤ 64 values.
Clients show the last frame's values; the server keeps no history of them (the gallery keeps the day log).

**CW beacon (`cw`, 1.1)** — the existing CW frames unchanged (0x01 text of the primary signal, 0x03 state,
`cw_signals`). The pick attaches `{"extension_name":"cw"}` with the listener's existing WPM AUTO.

### APRS via a satellite (1.9 server half)
`aprs_list.data.stations[i].sat` / `aprs_packet.data.sat` = "ISS" | "SONATE-2" | … — the satellite that carried it
(the digipeater in the path, vibe_sat_calls.h, or the pick's satellite). The gallery's APRS line gains " · via ISS".

### Unattended satellite passes (Decided 3) — no listener wire; owner config only
`vibeserver.json` / Android config: `"satPasses":{"on":false,"minEl":20,"birds":[{"norad":57166,"tx":0},…],
"record":false,"recordCapMb":0}`. Gallery entries made by an unattended pass carry `"pass":{"norad":N,"name":"…",
"aos":…,"los":…,"maxEl":…}` in their metadata. Admin status `"satPass":{"state":"idle|waiting|running","name":…,
"los":…}` in the setup page's status JSON.
