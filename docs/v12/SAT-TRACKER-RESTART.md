# The satellite tracker stops with the server (beta96)

On 11-25 and 11-26 the phone servers crashed while SATELLITE was in use, mostly with ORBCOMM picked. Every crash was in
the thread `vibe-sat-track`:

- XCover (app server): SIGABRT "FORTIFY: pthread_mutex_lock called on a destroyed mutex", at 11-26 21:15 and 23:26.
- Sony (Lite): SIGSEGV, at 11-25 20:14 and 11-26 23:27.

## Cause
`satEnsureTracker` started a detached `std::thread` that held the Impl. On Linux each radio is its own process, so
stopping the radio ended the process and the thread with it. On Android the server runs inside the app. Any stop or
restart deletes the Impl and makes a new one in the same process: a Setup save, the watchdog, the app's own stop, or the
stale-shim teardown in every `start*()`. The old tracker went on ticking against the freed Impl. Satellite picks always
start the tracker, ORBCOMM included. Nothing found here makes ORBCOMM restart the server itself, so the crash needed SATELLITE
in use plus any in-process restart. There is no rate change or self-restart in the shim on an ORBCOMM pick. All
`stopLocked()` callers come from outside the shim (JNI).

## Fix (c8b31b5db + the review commit)
- The tracker is a member thread (`satTrackerThread_`). It quits when `stopping` or `satTrackerStop_` is set.
- `satEnsureTracker` checks and starts the thread under `satTrackerMtx_`. `satTrackerHalt()` sets the flag and takes
  the thread in one locked step. This closes a race where a lock arriving during a stop could start a tracker after
  the join.
- `stopLocked` halts the tracker and joins it before anything the tracker reads is freed. It joins outside the mutex
  and detaches only if the join fails. `SatTrackerGuard`, the last member of Impl, does the same on any other
  destruction path.
- `stopLocked` then drops every lock in the process-wide `vibe::sat::engine()`. Orbits and the observer stay. Before
  this, the old Impl's locks (the room's pass, sessions that no longer existed) lived on into the new Impl.
- Deadlock check: the tracker never takes `g_lifecycle`, which the stop holds. Every socket the tracker writes to is
  closed before the join. The join therefore waits at most one 200 ms nap or one 40 ms tick.

## Tested how
`scripts/test-sattrack-restart.sh` runs `vibeserver/test-sattrack-restart.cpp`: the real shim in one process against
fake-rtl-tcp, with the ORBCOMM fixture's epochs moved to an hour ago. Each cycle is start → ORBCOMM pick with Doppler
(the orbcomm decoder attached, the tracker running) → stop with the socket still open → start again, with the stop
landing at varied moments. It runs 12 cycles on the shared dial and 12 with a listener's own lock (locked centre).
After every stop no `vibe-sat-track` thread may remain, and the stop must finish in under 20 s.
`SAN=address` or `SAN=thread` builds its own sanitised tree with the whole shim instrumented.

| | Release | ASan |
|---|---|---|
| fe9bae62a (before) | FAIL at stop 1 (1 tracker left). Abort on start 2: "recursive_mutex lock failed: Invalid argument", the macOS form of the XCover abort | heap-use-after-free WRITE in the tracker lambda (`satSharedShift_.store`) of memory freed by `stopLocked`'s `delete impl` |
| HEAD | 49/49 | 49/49, 0 reports |

TSan, HEAD, 10 + 10 cycles: 40/41. The tracker never outlived a stop. The one FAIL was the time bound: the first stop took 39.7 s under TSan's slowdown, and the tracker was already gone. TSan reported 19 ordinary data races and no destroyed-mutex or lifecycle report. Among them, `RxPipeline::setTune` races the demod thread's read; the tracker's `satApplyShared` makes that same call, as any retune does. The others are in `clientRetune`, `net::Socket::close`/`Listener::stop` and `currentGainTenthDb`. All were there before this change and are out of scope here.

NDK r27 arm64 `-fsyntax-only` of `local_sdr_shim.cpp`, with the app defines and with `VIBE_LITE_APP`: clean.

## Other detached threads (not the tracker)
None of the satellite decoders (orbcomm, lrpt, xpdr, fctlm, sarsat, VFO B) starts a thread of its own. They run on
`DecoderHost` threads, which the Impl owns. The shim's remaining `.detach()` calls that capture `this`:
- the DAB band scan and the FM scan (`dabBandScanRun`, `fmScanRun`): `stopLocked` waits for them for at most 3 s.
- recording playback: `recPlayStop` waits for at most 0.5 s.
- the battery low-power thread: it captures the LocalSdrShim, not the Impl, and reads `p` without `g_lifecycle`.

These are bounded waits, not joins. They were left as they are because none is satellite work.

## RESUME HERE
- Coordinator: run `scripts/test-sattrack-restart.sh` (it is in run-tests.sh beside test-keep-restart). Gradle was not run.
- Live check after the release, on the XCover and the Sony: pick ORBCOMM with Doppler, then save Setup (a restart)
  several times. There should be no crash, and logcat should show no "vibe-sat-track" after the stop.
- Some stops took about 4.5 s, in both phases and in Release and ASan. The tracker join is bounded far below that, so
  some other stage of the stop is slow. Not investigated.
