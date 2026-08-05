# VibeServer multi-listener probes

Two small WebSocket clients that join a running VibeServer as real spectrum listeners. They exist
because **multi-listener was "built but unproven" for weeks and was in fact plainly broken** — a
second listener deadlocked the whole server. Nothing had ever joined as listener 2.

See `BUG-vibeserver-broadcast-blocks.md` for what they caught.

## `two-listeners.mjs` — the stalled-listener case

A reads normally. B connects, then **stops reading entirely** (`socket.pause()`, so its TCP window
fills). Asserts A keeps streaming.

```sh
node two-listeners.mjs <port>
HOST=vibeserver.local RUN_MS=70000 node two-listeners.mjs 48000
```

★★★ **RUN IT AT 4096 BINS AND A HIGH FRAME RATE, OR IT PROVES NOTHING.** At 1024 bins / 15 fps /
20 s it **passes on code that still has the blocking-send bug** — the kernel socket buffers absorb
the stalled listener for the whole run. Start the server with `--fps 40` and give it 70 s:

```sh
vibeserver --tcp 127.0.0.1:1234 --lock-freq 100000000 --rate 1200000 --users 4 --fps 40
HOST=… RUN_MS=70000 node two-listeners.mjs <port>
```

## `many.mjs` — N healthy listeners

Joins N listeners 300 ms apart and reports frames each received. Every one should be within a few
frames of the others (the spread is just their staggered starts).

```sh
node many.mjs <port> [N]
HOST=vibeserver.local node many.mjs 48000 18
```

## Notes

- Multi-user needs a locked centre: the server refuses `--users > 1` without `--lock-freq`.
- The probes answer server pings, so they are not dropped by the 20 s liveness timeout.
- **A stale probe holds a slot.** If a run is interrupted, listeners can linger until the liveness
  timeout expires — a later run then sees a "full" server. Wait 20 s or restart the service.
- ★ `--fps` and bins are the two levers that decide whether this test is meaningful. If you change
  them, re-run against a deliberately broken build and confirm it still FAILS there.

## `binclash.mjs` — one listener must not resize another's waterfall

A joins at 4096 bins, B then joins at 128 (as the watch does). Asserts A stays at 4096 and B gets
128. Before the per-client-width fix, A was cut to 128 and never recovered.

```sh
HOST=vibeserver.local node binclash.mjs 48000
```

## `zoommixed.mjs` — the zoom path with mixed widths

Same two listeners, then A zooms in hard so the DSP zoom FFT takes over. The zoom row is produced
at the WIDEST listener's width and peak-held down for narrower ones; both must keep streaming.

★★ **Check the server log says `zoom spectrum ENGAGED`.** The first version of this test passed
without ever engaging zoom — it was sending the wrong message type, so it proved nothing. The
control message is `{"type":"zoom","frequency":…,"binBandwidth":…}`.

## `defbins.mjs` / `bandwidth.mjs` / `bw2.mjs`

`defbins` checks a client that sends no `bins` param gets the 1024 default. `bandwidth`/`bw2`
measure bytes/sec per listener (spectrum + Opus audio) — the numbers behind the listener cap.

## `config-api.mjs` — the config endpoint and its auth gate

Exercises `GET`/`POST /vibeserver/config`: no credentials and wrong password must both 401, the
right password returns the config, a valid POST persists and answers `{"ok":true,"restart":true}`,
and a contradictory one (multi-user with no locked centre) must 400.

```sh
node config-api.mjs 48000        # expects admin password "secret"
```

★★ The 401 cases are the ones that matter. **The config contains the PIN**, so an unauthenticated
GET leaking it would be worse than any setting being wrong.


## `independenttune.mjs` — OWRX-style per-client tuning

Two listeners tune to two different synthetic stations on one radio and must recover two
different audio tones (fake-rtl-tcp puts AM carriers at ±120 kHz with distinct pitches).

```sh
node independenttune.mjs 48200      # server needs --lock-freq and --users > 1
```

★★★ **OFFSET TUNING WILL FOOL YOU.** The radio sits `HW_OFFSET_HZ` (15 kHz) ABOVE the logical
centre so the DC spike misses the channel, so a station at capture-DC + X is at
`centre + 15 kHz + X`. Tuning without that puts both listeners 15 kHz off-station, and two
listeners hearing noise looks *identical* to two listeners sharing one VFO — which is the very
thing under test. Check the recovered tone matches the EXPECTED pitch, never just that the two
differ.

## `singleuseraudio.mjs` — the regression guard that matters most

One listener on an UNLOCKED server must still demodulate through the shared pipeline. This file
compiles into the phone and Mac apps, where there is exactly one listener and none of the
per-client machinery is constructed; breaking that would be far worse than any server bug.

```sh
node singleuseraudio.mjs 48210      # server with no --lock-freq
```

## `perclientzoom.mjs` — one listener zooming must not move anyone else's view

A zooms hard, B touches nothing, then C joins fresh. B must be unchanged and C must arrive at the
FULL span.

★★ **C is the decisive one.** An earlier version of this test passed while the bug was still
present: A's zoom went through the OLD GLOBAL handler and rescaled everyone, but B only *looked*
unaffected because nothing re-sent B its config — its waterfall was quietly rescaled underneath it
while it was still told the old span. A fresh joiner reads current server state, so it cannot be
fooled that way.

★ The cause was ordering: the shared handlers return as soon as they match, so a per-client block
placed after them is dead code for every message they claim.

## `spectrogram.mjs` — the 24-hour band record

Fetches `GET /vibeserver/spectrogram` and renders it as ASCII: proves the binary format parses,
the orientation is right (oldest at top) and carriers land at the right frequencies.

```sh
HOST=vibeserver.local node spectrogram.mjs 48000
```

Wire format — binary, because 1440 rows x 512 bins as JSON numbers would be megabytes of text for
a picture:

```
"VSPG" | u8 ver | u16 bins | u16 rows | f64 centreHz | f64 spanHz
per row: i64 epoch-ms, then `bins` bytes of dB
```

## `zoomnoducking.mjs` — zooming must not touch the audio

Measures the audio before a zoom and the WORST 100 ms block straight after it.

★★★ **THIS TEST FAILED TO CATCH THE BUG TWICE BEFORE IT WORKED.**
1. It zoomed to 10 kHz — the same channel width 10 kHz of AM audio already needs, so nothing was
   rebuilt and it passed on broken code. The span has to CROSS A CHANNEL-WIDTH BOUNDARY.
2. It averaged the whole window four seconds later. A rebuilt chain restarts its AGC, so the level
   dips and *recovers* — by then the dip had gone. The transient IS the symptom.
With both fixed it reads 0.60 on the broken build and 1.00 on the fixed one.

★ The bug: the channel was sized by `max(audio, view)`, so changing the ZOOM changed the channel
WIDTH, which rebuilt the pipeline. Tuning never did, because tuning keeps the same width — which
is exactly why "tune is fine though". Same shape as `memory/tuning_attenuates_agc_reset.md`.

## `zoomsteps.mjs` — the zoom must be continuous

Walks one listener down a ladder of spans (2 MHz → 8 kHz) and checks the server reports each one.

★ The bug: the shared wide row was cropped to the GLOBAL zoom and grouped only by bin width, so a
listener's own zoom did nothing until it was narrow enough to earn a private channel — the
waterfall jumped from the full span straight to 20 kHz with one step in between.

## `zoomnogap.mjs` — the spectrum must never stop while zooming

Walks a zoom ladder and measures the worst gap between frames after each step.

★★ The stall was NOT a pipeline rebuild, which is where I looked first. A zoom that changes
decimation empties the zoom FFT's accumulator, and it cannot emit until it refills — `fftN_`
samples of a now-much-narrower stream, about a SECOND at a 3 kHz span. Deeper zoom, longer wait,
which is exactly why it only showed at the deepest levels. The old samples are at the wrong rate
so they cannot be kept; the fix is to keep sending the shared wide row until the private view is
ready, so the picture keeps moving and merely becomes sharper a moment later.

★ 451 ms → 108 ms at 3 kHz. Verified to FAIL with the priming cover removed.

## `load.py` + `victim.mjs` + `audioprobe.py` — load testing that means something

- **`load.py` runs ON THE SERVER.** Running the load on a laptop measures the laptop's Wi-Fi as
  much as the server: an early run looked like a server fault and was the load generator and the
  browser fighting over one radio link.
- **It answers pings.** The first version did not, so every client was dropped after 20 s and the
  measurements were of an empty server that had looked busy briefly.
- **`victim.mjs`** is one listener over the real network while the load runs locally — what a human
  actually experiences. It reports GAPS, not frame counts: a probe that counts bytes cannot tell
  "smooth" from "stuttered then caught up".
- **`audioprobe.py`** judges the demodulated waveform, not its arrival: clipping, and energy at the
  channelizer's BLOCK RATE, which is the signature a phase bug leaves.

## `phasesweep.mjs` — sweep the dial, never sample it

The channelizer's phase correction is exactly zero when `centreBin` is a multiple of `OVERLAP_DIV`,
so a bug in it is invisible at one frequency in four. This steps by a quarter of a bin so every
residue is covered.

★★ That is not hypothetical: a stale phase reference reached the air as "tune to 7074 and it is
broken, 7073.5 or 7074.5 is fine". The channelizer's own comments already said "sweep, do not
sample" — from the last time it happened.
