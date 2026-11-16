# NCDXF / IARU beacon tracker (V12, 2026-11-16)

The International Beacon Project: 18 beacons on 14.100, 18.110, 21.150, 24.930 and 28.200 MHz. A listener's decoder
("BEACONS", under Amateur) that knows from the clock which beacon is due, checks that it is that one, measures its four
power steps, and builds a propagation grid (beacon × band).

| File | What |
|---|---|
| `android/app/src/main/cpp/decoders/ncdxf_tracker.h` | the beacons, the schedule, the keying, the per-slot judge, the grid |
| `vibe_ncdxf_run.h` | inside a decoder host: the band from the dial, the lines, the JSON |
| `vibe_decoder_host.h` | ext `ncdxf`; `decUtc_` = the block's capture UTC handed to the decoder |
| `local_sdr_shim.cpp`, `vibe_decoder_reach.h`, `vibe_setup_page.h` | `"ncdxf"` capability with reach + reason; the owner's switch |
| `src/services/ncdxf.ts` | the shared client half: sanitiser, grid rows, status, hint (web + app) |
| `web/client/index.html`, `main.ts`, `decoders.ts` | the BEACONS key, BAND keys, the grid |
| `src/components/NcdxfView.tsx`, `DecoderPanel.tsx`, `ModeSelector.tsx`, `SDRScreen.tsx`, `DecoderClient.ts` | the app |
| `vibeserver/test-ncdxf.cpp`, `scripts/test_ncdxf_app.ts` | tests |

## The schedule (published) and the timing (measured)

- Each beacon sends for 10 s, then moves up a band: beacon `(slot − band) mod 18` is on `band` in slot
  `(UTC s mod 180) / 10` — 4U1UN is on 14.100 at hh:00:00, hh:03:00 …, and on 18.110 ten seconds later. Order:
  4U1UN VE8AT W6WX KH6RS ZL6B VK6RBP JA2IGY RR9O VR2B 4S7B ZS6DN 5Z4B 4X6TU OH2B CS3B LU4AA OA4B YV5B.
- Callsign in CW at 22 WPM at 100 W, then four dashes at 100 W, 10 W, 1 W, 0.1 W.
- ★ **Measured on air** (2026-11-16, Stuart's UberSDR, 14.100, OH2B and CS3B at 15–20 dB): the callsign starts at the
  slot's start (±20 ms); the first dash a word space (7 units) after its last element; each dash ~0.80 s key-down,
  repeating every ~1.02 s — "one-second dashes" is their period. `ncdxf::Timing` holds these numbers. One OH2B slot
  was 1.0 s late (the other OH2B slot, and every CS3B one, on time) — the ±1.5 s lag search takes it.
- Beacon locations in the table are approximate (the grid does not use them yet).

## How a slot is judged (`Tracker::judge`, 2 s after the slot ends)

1. The listener's audio (48 kHz) → the beacon carrier's expected pitch mixed to 0 Hz (the host's audio→RF map
   inverted: USB / CW-U / LSB all work; the carrier must fall at 100–3000 Hz of audio), low-passed, 2 kHz complex, a
   16 s ring (256 KB).
2. 20 ms frames, 17 bins at −200…+200 Hz: the signal's bin is the strongest within ±75 Hz.
3. Every beacon's whole keying is slid ±1.5 s against that bin (normalised correlation). The scheduled beacon's best lag
   is where it is (`lag`: + = the slot arrived late — the server's clock fast, or the beacon late).
4. Noise = that bin's own key-up frames (median / ln 2 — exponential power). Never neighbouring bins: a narrow CW
   filter or the low-pass shapes them (the first version did and called noise "heard" in every slot).
5. Each dash's SNR is its middle (length − 0.2 s) against the noise. A step is HEARD at ≥ 2 dB (about 4 σ of a 1 s mean),
   counted from 100 W down **without a gap** (QSB that takes the 10 W dash and leaves the 1 W one is 1 step, not 3),
   and **only where the keying fits** (≥ 0.25; noise reads ≤ 0.17, a beacon 8 dB under the noise in 2.5 kHz ≥ 0.47).
6. The callsign: each beacon's CALLSIGN PART alone, where its own whole keying put it. The dashes are alike for every
   beacon, so they find the place; the callsign decides whose. ✓ when the scheduled one fits best by 0.05 (on air OH2B
   0.66 against VR2B 0.60 — both end "2B"); "another beacon" when another fits better by 0.15 — and then nothing is
   credited (a clock a slot out, a mistuned band).
7. The grid cell (18 × 5): last heard, its steps, the 100 W dash's SNR, ID, the best steps in the hour.

★ The SNR is per ~50 Hz bin: a dash sits 17 dB higher there than its SNR in a 2.5 kHz passband, so the tracker hears
the 0.1 W dash of a beacon whose 100 W dash is +15 dB in 2.5 kHz.

CPU: ~0.1 % of one M5 core (scalar: a mixer + three one-poles per sample, then once per 10 s a 17-bin DFT on ~700
frames and 18 × 151 correlations). Not a hot loop; no SIMD needed.

## FOLLOW (V12 2026-11-16, Stuart)

A **FOLLOW** key under the BAND keys (web and app): the listener's **own** dial hops with one beacon up the five bands —
20, 17, 15, 12, 10 m, ten seconds each — then waits on the first band it will return to while it is silent (130 s of the
three minutes). The tracker judges each slot as it always does, so that beacon's row of the grid fills in.

- **Whose dial:** only this listener's. FOLLOW is offered only where the server's `dial` mode is `exclusive` (a radio they
  have to themselves, or their own channel on a range-locked radio) and the web page's `tuneMode` is not `open`. **Never
  on a shared dial** — the shared-dial contract: the client has no vote, and a timer retuning everybody's receiver every
  ten seconds is not a listener's to start. A dial that turns shared stops it (the `dial` message re-checks).
- **Which beacon:** the one on the air now on the band the dial is on (20 m otherwise); ‹ › (app) or the list (web) picks
  another.
- **When:** a tick four times a second works out the slot from the **server's** clock (the report's `now` against the
  page's clock — the tracker's own time base) and tunes only when the band changes, 0.5 s before the slot starts so the
  callsign is heard from its first element. Bands this radio cannot reach are skipped.
- **Stopping:** the key again, a BAND key, a hand tune more than 1.5 kHz away, leaving BEACONS, or a dial that turns
  shared. The existing tracker is unchanged.
- The rule is one module (`src/services/ncdxf.ts` — `ncdxfFollowBand`, `ncdxfFollowAllowed`), tested in
  `scripts/test_ncdxf_app.ts` against the published schedule (`ncdxf_tracker.h beaconAt`'s order and calls).

## Clients

- **Web**: BEACONS under Amateur (hidden unless `"ncdxf"`; greyed with the reason where the radio reaches none of the
  five); BAND keys (only the frequencies this radio can tune, CW); the grid — beacon rows, band columns, the steps heard
  coloured green (4) → amber (1), "—" tried and not heard, blank not tried, the beacon on the air now outlined, a cell
  over an hour old faded; the status line (who is on now, the last slot's verdict, "late by 1.0 s"); the heard log.
- **App**: the same key, BAND row and grid (`NcdxfView`, the same `ncdxfRows`), the heard log under it.
- **R36S**: not built yet (its decoder views are per-decoder native code; this one needs a grid view).

## Tests

- `vibeserver/test-ncdxf.cpp` (40 checks, synthetic CW at the schedule): the schedule (every beacon on every band
  once a cycle, never two bands alike, 4U1UN/OH2B/CS3B slots, every 10 s fits); steps against SNR in 2.5 kHz
  (+30 → 4, +10 → 3, 0 → 2, −8 → 1, −25 → 0, never more than one above); 140 slots of noise on five bands: none heard,
  no ID; a slot holding the next beacon's keying named as such and never credited; a 0.8 s clock; 40 Hz off the pitch;
  QSB; the grid's JSON; the host's run (band from the dial, USB and LSB, the line, off-frequency); CPU.
  `test-ncdxf --wav <file> <start_utc> <dialHz> [lsb]` replays a recording.
- `scripts/test_ncdxf_app.ts` (28): the sanitiser (junk cells dropped, steps clamped), rows, status, colours, both
  clients' keys gated on the capability.

## On air — DONE 2026-11-16 (recordings, not a live server)

Recorded from Stuart's own UberSDR (RX888, standing permission) with `tools/ubersdr-rec/ubersdr-rec.mjs`, USB with the
dial 1 kHz below, 400 s each, kept in `~/VibeSDR-test-audio/ncdxf/`:

| Recording | start_utc | Heard (steps, ID) |
|---|---|---|
| `ncdxf-14100-usb-2026-11-16.wav` (≈ 08:03–08:10 UTC) | 1794816190.235 | OH2B 4 ✓ (twice, once 1.0 s late), CS3B 3 ✓ and 2 ✓, VE8AT 1 ✓ |
| `ncdxf-21150-usb-2026-11-16.wav` (≈ 08:09–08:16 UTC) | 1794816590.334 | 4X6TU 3 ✓ and 2 ✓, OH2B 4 ✓ (twice), CS3B 1 ✓ |

Every heard slot was the scheduled beacon, ID ✓. The steps were checked against the tone's envelope printed per 20 ms
(the heard slots show the callsign and dashes plainly); nobody listened to the recordings by ear.
Not yet: 18.110 / 24.930 / 28.200 on air, a night-time band, a listener's live session through a server (the host
path is tested in `testRun`, not on a running VibeServer), and the beacon status list (ncdxf.org: some beacons are off
at any time — "not heard" is all the grid can say).

## Left / ideas

- **Background / all five bands at once**: a wide HF capture (RX888, USRP) could run five channels at once on the
  channeliser and keep the grid with nobody listening — a background profile beside the NDB skimmer.
- **FOLLOW a beacon**: the client stepping the dial every 10 s to follow one beacon across the bands.
- R36S view; a map of the grid (great-circle lines from the receiver), using the beacon positions.
