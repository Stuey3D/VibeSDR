# The 8-second shutdown hang (V12 beta92)

## RESUME HERE

- **Fixed on branch `b92-digifilter`:** after its orderly stop (`shim.stop()` → "local SDR stopped") a vibeserver process
  that started no radio processes now **ends** — `fflush(stdout/stderr)` + `_exit(0)` (`main.cpp
  endProcessAfterStop`) — instead of `return 0` from `main()` into `exit()`. Both exits use it: the plain stop
  (SIGTERM: systemd, the radio reconcile, a reboot) and the settings-save restart under a service manager. A front
  door that spawned its own radios (no service manager) still returns, so its `atexit(reapRadios)` runs.
- The station store's last flush moved from its static destructor into the stop (`stopLocked`, stage "the station
  store's last flush"), so no rows are lost by skipping destructors. Nothing else a radio needs was done in a static
  destructor (checked: PSK Reporter, the aircraft share, the DMR lookup and the ADS-B updater only join or nothing;
  the gallery taps and day logs are finished by the decoders' own stop).
- The watchdog (`armShutdownWatchdog`, 8 s) stays exactly as it was — the net, not the plan.
- **Not proven on the Lenovo itself** (no access by the rules): the Mac reproduces the *shape* exactly with a
  stand-in finaliser, and the Lenovo's thread list points at the class (below). After beta92 ships, the journal
  should show no more "shutdown did not complete" lines; if one appears, its stage will now say which step.

## The fault

Stuart's Lenovo, 2026-11-20 → 22, beta90 and beta91, 12 times in 3 days:

> VibeServer: shutdown did not complete in 8s — stuck at: (before or after the radio's stop) — exiting anyway so the
> service can restart.

followed by the "still running" threads (as reported in the brief, not copied here verbatim): the main thread in
`futex_wait_queue`, `vibe-ft8deep` and others in nanosleep, `libusb_event` in poll, `vibe-adsbdata` in futex.

One radio process was started 22:59:06 and told to stop 22:59:07 (the radio reconcile restarting it), logged
"local SDR stopped" at 22:59:08 — and the 8 s net fired at 22:59:15.

### What the main thread was waiting on

- "stuck at: (before or after the radio's stop)" means `g_vsStopStage` was null: the stop had **finished** (it clears
  the stage as its last act, after "local SDR stopped"). The main thread then did `return 0` from `main()`.
- `return` from `main()` is `exit()`: it runs every **static destructor** and every loaded library's **finaliser** on
  the main thread, *while the process's other threads keep running*. Ours join threads (the station store's flusher,
  PSK Reporter's, the aircraft share's, the DMR lookup's); the libraries' are not ours at all.
- **`libusb_event` is UHD's thread** — the `libusb_session_impl::libusb_event_handler_task` symbol and the
  "libusb_event" name are in the shipped linux binary (checked in a shipped linux arm64 build's strings); libusb itself names no threads.
  It is the event loop of UHD's global libusb session, which UHD joins in that session's destructor — at exit, if the
  session is still alive then, which it was (the thread was still polling 7 s after the stop). A futex wait on the
  main thread is exactly what a join (or a library's lock) looks like. SDRplay's API is the other known one: this
  code base already records it blocking on its system-wide lock during teardown.
- One second into a process's life is when threads are still starting and libraries still initialising, which is
  why the reconcile's quick restarts were the ones that hit it.
- **Detached sleeping threads** (`vibe-ft8deep`'s 60 s sleeps, `vibe-adsbdata`'s hour-long cv wait): they never delayed
  an exit by themselves — the process ends when `exit()` finishes, and the kernel takes every thread with it. They
  were listed because they were alive, not because anything waited on them. (They are a use-after-destroy hazard
  *during* static destruction — `vibe_adsb_data_update.h` records a SIGSEGV of that shape — which `_exit` also ends.)

### Why `_exit` is the cure, not a workaround

Every destructor that could block at exit belongs to something already finished: the radio is closed (or left to
the kernel — see `stopLocked`, 2026-10-05), every decoder joined, every gallery day written, the spectrogram saved
before the stop, the config on disk, and now the station rows flushed by the stop. The kernel closes every file and
USB handle at process end either way. What `exit()` adds after that is only risk: other people's finalisers,
running on threads that are still alive. The same file already ends the watchdog, the parent-death watch and the
restart helpers with `_exit` for the same reason.

## How it was tested

`scripts/test-server-quick-stop.mjs` (registered in `run-tests.sh`, needs `VIBESERVER_BIN`; silent; every wait has a
deadline; children killed in `finally`; the watchdog set to 30 s so a hang shows as a long exit instead of being cut
off at 8 s; on macOS a hang is sampled and the stacks saved beside the log):

1. SIGTERM 0.3, 0.6, 1.0, 1.5 and 2.5 s after the process starts, against a fake rtl_tcp;
2. SIGTERM after 4 s running with a listener (audio + spectrum sockets);
3. **the Lenovo's shape:** `VIBE_TEST_EXIT_WEDGE=1` makes a static whose destructor never returns (a stand-in for a
   library finaliser), SIGTERM at 1 s and at 4 s.

| build | cases 1 + 2 | case 3 (exit-time wedge) |
| --- | --- | --- |
| before (77da6b07c) | all clean, 0.5–1.4 s | **30 s, the watchdog fired**, "local SDR stopped" then "stuck at: (before or after the radio's stop)" — the journal line exactly; `sample`: main thread `exit → __cxa_finalize_ranges → ~ExitWedge` |
| after (97b5aedf9) | all clean, 0.6–1.4 s | **0.6–0.75 s, no watchdog** |

After: **42 / 0** over three rounds, and `scripts/test-server-stop-during-dab.mjs` **17 / 0** (its watchdog case —
a stop wedged *inside* `stopLocked` — still names its stage and is still cut off). Built and run on the M5 Pro
(macOS, no UHD in that build); the Lenovo is Linux with UHD — see "Not proven" above.

A SIGTERM in the first ~0.3 s, before `main()` installs its handler, ends the process by the default action — also a
prompt stop; the test accepts it when the log shows the server had not started.
