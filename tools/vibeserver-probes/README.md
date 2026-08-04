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
