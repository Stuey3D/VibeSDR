# The listener count and the ask lock when someone leaves (beta96, 2026-11-26)

## RESUME HERE
Done on branch `b96-listenercount` (off `b96-integrate`), not merged. Server (the shared `local_sdr_shim.cpp`, so the
Linux vibeserver and the Android server alike), app (`src/`), web page (`web/client/`, `vibe_web_page.h` rebuilt).
Not done: Jr / Buddy (watch) still show `listeners` as they always did (see "Open").

## The report
Another agent's headless run (a real vibeserver, fake rtl_tcp, a shared dial, 4 places, ask before tuning on): after the
second listener closed its sockets, `listeners` (vibeserver.json, `users`, the dial) stayed at 2/4 and the bar read
"ASK TO TUNE · 2/4", but the ask LOCK had lifted.

## What was really happening: two disagreements, both by design halves that did not meet
1. **A dropped connection is HELD** (THE HELD SESSION, since beta63): sockets gone with no goodbye (no `bye`/`leave`,
   no clean 1001 close from a foreground app) keep their place for 180 s, still counted (Stuart: "their listener slot
   stays counted"). But the ask lock leaves a held session out (`askInForceLocked`: nobody can ask a suspended phone).
   So the count said 2 and the lock said "alone" — and the banners derive FREE/ASK from the count: "ASK TO TUNE · 2/4".
   A Node `WebSocket.close()` (1000/1005) or a dropped socket is exactly this case — the reported run.
   A real browser tab closing sends `bye` on pagehide and closes with 1001, and is released at once (that path was fine).
2. **The opposite, found by the new test**: a FINAL departure (`bye`, or a clean 1001 close) still went into the
   socket-swap grace (`askGone_`, 8 s, for DAB's socket swap) — the count dropped to 1 at once, but the lock stayed on
   for 8 s + up to a 2 s tick: the one listener present told "1 listening" and kept locked.

## The fix
- `companyCountLocked()` — the people in the room (listeners − held + swap grace): ONE count for the lock and for what
  the banners are told.
- `away` = listeners − company (held places past the swap grace), sent beside `listeners` in `dial` AND `users`, only
  when > 0 (older clients read what they always read). The held place stays counted (capacity, vibeserver.json).
- Clients: `dialAlone` = `listeners − away ≤ 1` (blindTuneGate.ts, one rule for the app's bar, the media-skip gate and
  the web banner); `away` parsed in both socket readers; the web banner's tooltip says "N away — their place is kept".
- `askTick` re-tells the room (dial + users) when `away` moves without anyone arriving or leaving (the grace running out).
- A departure for which `noHoldPendingLocked` is true (final) is not put in `askGone_`.

## Tests (how verified)
- `scripts/test-server-leave-count.mjs` — real server, fake rtl_tcp, shared dial, 4 places, ask on, hold 12 s, swap
  grace 6 s. Listener A stays; B (spectrum + audio + /ws/dxcluster, presence foreground — the web client's shape) leaves:
  bye+1001, 1001 only, audio first then spectrum, and a drop (held). Checks the dial, `users`, /vibeserver.json and A's
  askFirst each time, and the INVARIANT over every dial A got: `askFirst === (listeners − away > 1)`.
  This branch 16/0. b96-integrate binary 10/6; b95-integrate binary (≈ v12-experimental / beta95) 10/6 — the same
  failures: **not a beta95 regression** (holds since beta63 d93ad592d, the swap grace since 11-15 3f3bf3506).
- Unchanged and passing: test-server-dial-leave 4/0, test-server-presence 22/0, test-ask-before-tuning 59/0,
  test_ask_before_tuning.ts 34/0, test_shared_banner.ts, test_blindTuneGate.ts 598/0 (+5 `away` cases).
- Synthetic only (fake dongle, raw sockets); no browser drove the banner — the web banner's test is `dialAlone`'s.

## Open
- Jr / Buddy: `dialListeners` drives the watch's count glyph and "N listening"; they make no FREE/ASK claim from it, so
  left alone — if the glyph should show people present, read `away` in WatchLink.swift / UberClient.swift.
- The picker / directory read `listeners` from vibeserver.json for FULL — a held place is still a place, so unchanged.
