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
