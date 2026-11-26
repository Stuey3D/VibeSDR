# NAVTEX on the Lenovo collapsed with beta92 — cause, evidence, fix, test

Stuart 11-25: *"why has my navtex dropped in performance?"* Lenovo (192.168.86.13, amd64, RSP1A at 3 MS/s, background
decoding profile centre 1.2 MHz, VFOs 489.5 / 517.5 kHz USB NAVTEX).

## Cause (one paragraph)

beta92's `adf09a108` ("Background decoding takes the back seat") changed `BgRunner::feedIq` to **try-lock** its IQ
queue and **drop the chunk** whenever the background worker held the lock. On an RSP the DSP thread hands over
one small chunk at a time (about 1 ms), and the worker locks the queue once for every chunk it pops, so the two collide by
themselves all the time: the Lenovo logged **0.5–0.6 s of IQ dropped every minute** as hundreds of tiny holes. A NAVTEX
VFO is an `RxPipeline` (USB demodulator), not a Band VFO, so a dropped chunk is **never bridged**. The samples on either
side just join up, and every miss pulls the 100-baud bit clock early by the chunk's length. The FSK decoder keeps
re-locking onto a wrong bit alignment and files CCIR-476 words that pass the 4B/3Y check but are the wrong characters.
That is the "Start lost … garbage" files, and the decoder believes them because each word is valid. b94's `3962c77a4`
(made for the same fault on the Pi 500's FT8/JS8/WSPR) **holds** a chunk the DSP thread cannot hand over and passes it
in ahead of the next one, so nothing is lost. That fix shipped in apt **12.0.0~beta94-2**, and the Lenovo's NAVTEX
recovered when beta94-2 was installed.

## Evidence (Lenovo, read-only)

- `dpkg.log`: beta91-6 → **beta92-1 11-24 01:08:41** → beta93-1 11-25 02:09 → **beta94-2 11-25 22:11:25**.
- journal (`vibeserver@2235030199`, which goes back to 11-20): `background decoding: N s of IQ dropped in the last minute`
  - **0 lines** from 11-20 to the beta92 install;
  - from 11-24 01:32 **every minute** until 11-25 22:11:18 (1 541× 0.6 s, 782× 0.5 s, 73× 0.7 s, …);
  - **0 lines** after the beta94-2 restart.
- Gallery, NAVTEX files with a ZCZC header, per hour:
  - 11-23 20–23 h (beta91-6): 30/35, 28/29, 21/32, 36/42;
  - 11-24 00 h (beta91-6): 14/16; **01 h onwards (beta92) 0–4 an hour**, all day 11-24 and 11-25 (11-25 18–20 h: 0/8, 0/15, 0/13);
  - **11-25 22:11 beta94-2 →** 22 h 20/22, 23 h 19/25. Almost every file from 22:20 has its header.
- beta93 did not help: it does not contain `3962c77a4`. Checked with `git merge-base --is-ancestor`: beta92 commit
  `35fddcdd7` has `adf09a108` and not `3962c77a4`; tag `beta94-2` has both; beta91-7 has neither.

## Fix

Already on this branch: `3962c77a4` (b94, `vibe_bgdecode.h` `BgRunner::feedIq`, the held hand-over, capped at
`kHeldSec` = 0.25 s). The fix does not change for NAVTEX. This branch adds the NAVTEX regression test, so the fault cannot
come back unnoticed for the non-Band (RxPipeline) VFOs. The b94 tests only covered Band FT8.

## Test

`vibeserver/test-bgdecode.cpp` section `lenovoBusyQueue()`. It runs one NAVTEX message (`ZCZC GA41 … LIGHT BUOY
51-01.2N 001-31.8E UNLIT … NNNN`) on 518 kHz, through the real `BgRunner` at the Lenovo's 3 MS/s and centre 1.2 MHz, fed
in 1 ms chunks:

1. **Quiet queue.** No forced busy chunk. It must drop nothing and decode the message.
2. **Busy queue.** The queue lock is held for 1 chunk in 50. It must hold the chunks (not drop them) and decode the same
   message, with its header and no "Start lost".

| build | result |
|---|---|
| current `vibe_bgdecode.h` (b94 held hand-over) | **257 pass / 0 fail**, 3 runs out of 3 |
| A/B: the same header with feedIq's busy branch put back to beta92's drop | **253 pass / 4 fail**, 2 runs out of 2 |

★ The beta92 build fails **even with no forced busy chunk**. The worker and feeder collided by themselves 54–153 times in
about 10 s, and 162 000–459 000 samples were dropped. The message came out as `ZCZC GA41 / 251830 UTC NOV 2' / NITON
N_KXYCQ_OFJFYV0_ … [end lost]`. With the forced busy chunks it gave a `Start lost` file of garbage, or no file at all.
That is the Lenovo's symptom, reproduced.

How the A/B was built: a scratch copy of `vibe_bgdecode.h` with the busy branch replaced by beta92's three lines (count
`missed_`/`dropped_`/`busyDrops_`, return), put first on the include path. The test file is the same for both builds.
Everything is compiled with `g++ -std=c++17 -O2` against the same source set as `run-tests.sh` uses for `test-bgdecode`.

## Verified on real signals vs fixtures

- **Real:** the Lenovo's own gallery and journal, read-only. The drop lines and the collapse start at the beta92 install
  and stop at the beta94-2 install, both to the minute. NAVTEX recovered on air after beta94-2 (22–23 h on 11-25).
- **Synthetic:** the test above (generated FSK IQ with light noise). No new recording was needed or made.

## Open (for Stuart)

- A NAVTEX/RTTY/etc. (RxPipeline) VFO still does not **bridge** a hole that is genuinely dropped: the queue's 0.5 s cap
  or the 0.25 s hold cap when the worker really falls behind. A Band VFO bridges it with zeros. Bridging with zeros
  would keep the bit clock, like a fade does, but it costs a full-rate pass over the zeros at exactly the moment the
  worker is behind. Not done; it is a trade-off, not a regression.
- There is no gallery-side warning. The "0% of characters lost" on garbage is the decoder trusting valid-looking
  CCIR-476 words. That deserves a separate look (a dictionary or plausibility score), but it was not the cause here.

## RESUME HERE

Nothing is owed for the regression: it is fixed in beta94-2 and confirmed live on the Lenovo. This branch carries only
the regression test and this note, ready for the coordinator to merge. The coordinator runs the full suite
(`run-tests.sh`); `test-bgdecode` passes alone, 257/0.
