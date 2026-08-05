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

## `blockedmodes.mjs` — the block list must be ENFORCED, not just advertised

With `blocked: ["wfm","rdsx"]` saved, asserts the list is published on `hwinfo`, that a client
asking for `wfm` is refused with `mode_blocked`, and that an allowed mode still works.

```sh
node blockedmodes.mjs 48000
```

★★ It tests **both** routes. `{"type":"mode"}` is the obvious one; `{"type":"tune"}` also carries a
mode, and guarding only the first would leave the block bypassable by the message clients send most
often. A UI that hides a mode is not enforcement — a client can send anything.
