# BUG: multi-listener freeze — a self-deadlock, and blocking sends on the DSP thread

**Found 2026-08-04, after it froze the live Pi demo.** Stuart: *"I tried to connect to it whilst
still being connected in the app and both froze."*
**✅ BOTH FIXED AND PROVEN ON THE PI, 2026-08-04.** 18/18 listeners on the RSP1B; a deliberately
stalled listener no longer touches anyone else.

---

## ★★★ THERE WERE TWO BUGS, AND THE ONE NAMED IN THE TITLE WAS THE SECOND ONE

The original diagnosis below (blocking sends under a global mutex) was **correct but incomplete**.
It is real, it is fixed, and it was NOT what fired first.

### Bug 1 — a SELF-DEADLOCK on `clientMtx`, and it could only ever fire in multi-user mode

`acceptWs` evaluates "is the server full?" **inside** its `clientMtx` scope:

```cpp
std::lock_guard<std::mutex> lk(clientMtx);          // held
...
&& (maxUsers <= 1 || specListenerCount() >= maxUsers);
                     ^^^^^^^^^^^^^^^^^^ takes clientMtx AGAIN
```

`specListenerCount()` locks `clientMtx`. `std::mutex` is **not recursive**, so the second listener
deadlocked **on itself, while holding the lock**. Every other thread that wants `clientMtx` then
piles up behind it forever — including the **DSP thread**, which takes it in `onAudio()` to read
`audioClient`. The whole server stops.

★★★ **`maxUsers <= 1` short-circuits before the call.** A single-user server never reaches it. So
every path anyone had actually exercised skipped the bug, and it fired the instant a second
listener arrived on a server with a cap above 1. **THIS is what "multi-listener is BUILT BUT
UNPROVEN" was really hiding** — not a subtle race, a plain unconditional deadlock nobody had run.

**Fix:** split the helper. `specListenerCountLocked()` assumes the lock is held and is what the
occupancy test calls; `specListenerCount()` keeps the lock and is for everyone else.
★ **The rule:** a helper that takes a lock must never be called from a scope that holds it. Keep
both forms, named so the next caller has to choose on purpose.

### Bug 2 — blocking writes under ONE GLOBAL mutex, on the DSP thread

`sendWs()` used to do:

```cpp
std::lock_guard<std::mutex> lk(sendMtx);   // ONE global lock, ALL clients
if (!sock->isOpen()) return;
sock->send(hdr.data(), hdr.size());        // BLOCKING write
if (len) sock->send(payload, len);         // BLOCKING write
```

Three facts that only bite together:
1. **One global `sendMtx`** serialises every send to every client.
2. **`send()` is a blocking TCP write** — it waits when the peer's window is full.
3. **Most of these calls happen on the DSP THREAD** (`onSpectrum`).

So one listener that stops reading — a slow link, a paused browser tab, a client mid-teardown —
blocks in its own `send()`, holds `sendMtx`, and stops **every other client's traffic and the DSP
thread**. ★ **`isOpen()` is not a defence:** a socket can be perfectly open and simply not draining.

**Fix: a per-client outbox.** Nothing that produces data ever writes to a socket. Producers append
to a per-client queue and return; each client has its own mutex and **its own writer thread**, so a
stalled peer backs up its own queue and nobody can tell. Drop policy per class:
- **Spectrum — newest wins.** A waterfall row is worthless once a newer one exists, so a slow
  listener degrades to a lower effective frame rate instead of accumulating a backlog.
- **Audio and control — never dropped.** A gap is audible; a lost control reply desynchronises.
- **512 KB backlog ceiling** ⇒ that listener is dropped, **alone**. The old code punished everybody
  for one slow peer; the cost now stays with the client that incurred it.
- ★ Header and payload are **one buffer, one queue entry**. Queued separately they could interleave,
  and a WebSocket header split from its body desynchronises the stream permanently.

### Bug 3 (found on the way) — every listener after the first was WRITE-ONLY

Extra listeners `return`ed before the read loop, so for listener 2..N:
1. **nothing was ever read** from the socket — control messages ignored;
2. **no liveness** — the ping/pong probe lives in that loop, so a vanished peer (suspended phone,
   link dropped with no FIN) was never detected and **held a slot forever**. On a server
   advertising a user cap, leaked slots eventually refuse everybody;
3. **no teardown** — the cleanup at the end of `acceptWs` never ran for them.

**Fix:** fall through into the same loop. An extra is a listener like any other; the only thing that
differs is which pointer holds it.

### Also restored: the telemetry cutback
The 2026-08-04 mitigation moved `sig` and `rspstat` to the primary socket only. That removed the
amplification but left **every listener after the first with a dead S-meter and a frozen gain
readout** — and a control that is visible and inert reads as *"the feature is broken"*, not *"you
are the second listener"*. Both are back on every peer now that the write path cannot block. The
~5 Hz rate is kept: that was right on its own merits, only the fan-out was the bug.

---

## ★★★ THE METHOD NOTE — THE FIRST REGRESSION TEST PASSED ON THE BROKEN CODE

`tools/vibeserver-probes/two-listeners.mjs` at 1024 bins / 15 fps / 20 s **passed with the deadlock
fix alone** — the kernel socket buffers absorbed the stalled listener for the whole run, so it never
demonstrated bug 2 at all. Raising it to **4096 bins / 40 fps / 70 s** made it bite.

| build | listener A while B is stalled |
|---|---|
| original | **FAIL** — starved at 15 frames |
| deadlock fix only | **FAIL** — starved at 367 frames (~12 s in) |
| deadlock fix + outbox | **PASS** — 2800 frames, still 40 fps at 68 s |

★★ This is the second time this trap has been recorded here (see
`memory/tuning_attenuates_agc_reset.md`). **A regression test must be run against the broken code
before it is believed** — and when it passes there, the test is wrong, not the diagnosis.

## Verified on the Pi (RSP1B, locked 6.5 MHz / 8 MSPS, `--users 20`)
- **18/18 listeners** streaming at the same rate; process at ~32% of ONE core of four, 11.5 MB RSS.
- Stalled-listener case: A held 15 fps for 68 s while B was completely stalled.
- ★ The **old packaged binary on the live demo reproduced the deadlock exactly** — two listeners,
  0 frames, whole server frozen. The demo had been silently broken for anyone who connected second.
