# THE SKY PLOT (V12, 2026-11-24)

Where in the sky this station's aerial actually hears satellites. A polar dome — outer ring the horizon, centre the
zenith, **north up, east right** (a map's view, as GPS sky plots are drawn), rings at 30° and 60° — with one dot per
decode at the satellite's azimuth / elevation at that moment, coloured by SNR. It **accumulates across passes**, so after
a day or two of ORBCOMM (up almost all the time) it shows the aerial's real coverage: a YouLoop's nulls, a vertical's
overhead hole, a QFH's even dome, a building in the way to the north-east. Grey dots = the picked bird was tracked and
heard earlier in the pass, but nothing decoded there — the blocked directions are the point.

## RESUME HERE

Branch `orbcomm`. Server built and tested (synthetic); the clients' views by the helper agent on the same branch.
ORBCOMM feeds it; the other satellite decoders are designed in (kinds) but not wired yet.

## One file per server, filtered by radio

The logs' rule (docs/v12/LOG-RADIO-FILTER.md): every radio process appends to **`<gallery>/skyplot.dat`** under flock
(`.skyplot.lock`); each dot carries its radio's id (`vibe_log_radio.h` — eight hex characters, never a serial). A sky
plot is about an AERIAL, so every view filters by radio (`?radio=<id>`, the same chips as the logs); "all radios" is
offered but only means something when they share an aerial. Untagged dots (no radio id: the Android app's own radio, a
test) pass every filter, as in the logs.

## The store (`android/app/src/main/cpp/vibe_skyplot.h`)

- A dot: 24 bytes — UTC seconds, az (0.1°), el (0.1°), SNR dB (int8; −128 = nothing decoded), kind, count (decodes in
  the bucket), catalogue number, radio id. File = `VSKY0001` + records.
- **Bounded**: at most 60 000 dots (1.4 MB); past that the oldest go (rewritten to the newest 50 000 under the lock).
- **Aggregated by the decoder**: one dot per satellite per 5 s (`kBucketSec`), the best SNR in it — never a dot a packet.
- Kinds: `orbcomm` (1, wired), `meteor` (2), `aprs` (3), `funcube` (4) — any satellite decoder can call
  `vibe::skyplot::shared().add(...)` with the bird's az/el from the engine (DecoderView has az/el already). Not wired
  for the others yet: METEOR's frames and FUNcube's telemetry would need a dot per 5 s of good frames; APRS a dot per
  digipeated packet heard during a pick.

## The views

| where | what |
|---|---|
| `GET /vibeserver/skyplot.json?kind=orbcomm&radio=<id>&days=N&norad=N` | `{"v":1,"kind","dots":[[az10,el10,snr\|null,t,kind,norad,count,radioIndex]…],"radioIds":[…],"radios":[{"id","name","n"}…],"count","truncated"}` — newest first, ≤ 20 000; the gallery's privacy applies |
| `GET /vibeserver/skyplot.png?…` | the server's own picture (520 × 584, the same filters) |
| `orbcomm_status.dots` | the live plot: dots added since the last status (the SATELLITE box appends them) |
| the gallery | at each ORBCOMM pass's end (the pick sets) or a stop: this radio's ORBCOMM plot as a PNG item (kind `orbcomm`) |
| SATELLITE box (ORBCOMM pick), app + web | the plot, loaded from skyplot.json for this radio, live dots appended |

Colours (server `snrColour`, clients the same four stops): 0 dB deep blue (50,60,200) · 6 dB teal (30,170,190) · 12 dB
green (60,200,80) · 18 dB+ yellow (250,225,40); grey (110,110,110) = nothing decoded, drawn underneath.

## Tests

`vibeserver/test-orbcomm-run.cpp` §2: dots one per 5 s within 1° of SGP4; the store's radio tags, radio/kind filters,
untagged dots, the bound and compaction; the JSON; the PNG (written to `$TMPDIR/orbcomm-skyplot-synthetic.png`).
