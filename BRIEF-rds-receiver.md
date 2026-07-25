# BRIEF: The RDS receiver — what it is, why, and what is left

**Project:** VibeSDR / VibeDSP (`android/app/src/main/cpp/vibedsp/rds.cpp`, `stereo.cpp`)
**Author:** Stuart Carr (Stuey3D) with Claude, 2026-07-25 (overnight session)
**Status:** IMPLEMENTED and on air, except §6. Written down because the session that produced it
found three separate faults, reverted one "improvement", and the reasoning is not obvious from
the diff.

---

## 1. Where it started

Stuart: *"no RDS on stations a car radio reads easily"* — including one at **25 dB SNR in stereo**.
The assumption going in was that this was a weak-signal problem. It was mostly not.

## 2. ★★ The real fault: a phase null

Detection was **real-only** — `mpx * ref57`, mixing the MPX against a single in-phase 57 kHz
reference derived from the pilot. That scales the recovered data by **cos(theta)** against the
station's ACTUAL subcarrier phase, and takes it to **zero at 90 degrees**.

That phase is not ours to control. The standard ties RDS to the third harmonic of the pilot only
within a tolerance, receive filters add phase of their own, and multipath rotates it continuously.
So which stations produced RDS was close to luck — Heart at S9+30 landed well, a 25 dB stereo
station landed in the null.

MEASURED (`vibedsp_rds_snr_probe`, phase sweep at fixed noise): the old detector recovers nothing
at all at 80-90 degrees, where the current one is flat across every angle.

**The fix — complex and differential:**
- `StereoPLL` now emits `sin(3*phase)` alongside `cos(3*phase)`, both from the same sin/cos pair
  via triple-angle identities, so no extra trigonometry.
- The decision is `Re{A_k * conj(A_(k-1))}` — differential (DBPSK). **The carrier phase cancels
  algebraically**, so no phase estimate is needed at all. RDS is differentially encoded anyway, so
  this hands the decoder the data bit directly. Costs ~1 dB against coherent detection, and buys
  complete immunity to the thing that was actually breaking it.

## 3. ★ The second fault: the losing hypotheses were talking

The demod runs NPH=16 timing hypotheses. They shared one set of callbacks, so a **misaligned**
hypothesis that stumbled into block sync could emit — which is how a perfectly strong station
reported a two-character name. Burst correction (§4) made false sync easier and so made this
worse; the two had to be fixed together.

Now each hypothesis routes through a trampoline carrying its index, and only the **best-scoring
synced** hypothesis reaches the caller.

★ Note this also means **callback counts are not comparable across decoder counts** — with 16
decoders several aligned ones each fire, inflating any "groups decoded" metric. Compare "is the PS
exactly right", and compare against the theoretical maximum (~11.4 groups/sec).

## 4. Weak-signal handling

Following the approach of [redsea](https://github.com/windytan/redsea) (Oona Räisänen, MIT —
credited in `README.md`; no redsea code is used and VibeDSP shares none of its dependencies):

- **Burst-error correction** from a syndrome → error-vector table, 1- and 2-bit bursts at all 26
  offsets. ★ Deliberately NOT the 5-bit bursts the code technically allows: every extra pattern
  makes a FALSE correction likelier, and a wrongly "corrected" block is worse than a discarded one
  because it enters the parser looking valid.
- **Rhythm sync acquisition** — three boundaries agreeing on the 26-bit grid in cyclic A,B,C,D
  order, instead of latching on a single block-A syndrome that noise can fake.
- **Error-RATE sync drop** — 25 bad in 50, not 4 consecutive. A fade no longer costs the lock and
  everything in flight with it.
- ★ **Confirmation only where EARNED.** A block whose checkword matches exactly is already strongly
  verified and commits at once; only a REPAIRED block waits for a repeat. Charging every update
  made strong stations visibly slow to populate for no safety gain — Stuart spotted it as "the
  stronger stations are now taking a lot longer to populate".

## 5. ★★ REVERTED: the timing loop. Do not retry blind.

Replacing the 16 hypotheses with three early/late gates driving a timing loop, feeding ONE
decoder, was implemented and then **reverted**. It measured beautifully on synthetic signals —
perfect decode to a higher noise level, flat across phase — and was a **disaster on air**: a strong
station took **~30 seconds** to produce a name instead of being instant.

★ The brute force has a virtue worth more than the cycles it costs: **ZERO ACQUISITION TIME**. One
hypothesis is always already aligned, so there is no loop to pull in, no transient, and nothing to
re-acquire after a fade. A timing loop must find the phase before the first bit is right, and
every slip while it hunts breaks the differential chain and costs block sync.

Two traps that made this hard to see:
1. **The synthetic tests cannot measure it.** They report steady-state yield, not time to first
   name. Any future attempt MUST measure time-to-first-name on a real station.
2. ★ **Cached RDS bookmarks made a slow decode look instant.** The station name appeared
   immediately from a saved bookmark while the decoder was still hunting. Clear them, or tune away
   and back, before believing any acquisition-time measurement.

It also gave **no CPU win**: two decimating FIRs (I and Q) cost what 15 decoders saved.

## 6. What is left

1. **A two-stage filter cascade for the RDS baseband.** The complex path runs two decimating FIRs
   where the real path ran one, and the filter dominates. The same trick the WFM audio path uses —
   a cheap wide filter decimating hard, then the sharp narrow one at the low rate — is worth
   roughly 3x here. This is the only remaining CPU item and it is a clean, contained change.
2. **A matched filter for the transmitted pulse shape.** The integrator is a rectangular biphase
   correlator; RDS is transmitted with cosine data shaping, so a matched receive filter is worth
   1-2 dB. Small, but free once the cascade above is being touched.
3. ★ **An RDS quality readout** — block error rate and groups per second, live. It turns "no RDS"
   into a number, so a change to gain or antenna can be judged instead of guessed. Directly useful
   to the FM-DX crowd, who compare receive setups for sport, and it is the natural companion to
   `BRIEF-auto-gain.md`'s "which limit are you against" indicator.

## 7. What will NOT fix weak stations

The noise-limited threshold was **not** moved by any of this, and was not moved by the earlier
decimation work either (proved by running the same probe against both checkouts). RDS is injected
~21-27 dB below the stereo subcarrier and rides at 57 kHz where FM's noise triangle is worst,
offset by ~8 dB of processing gain from its narrower bandwidth — so **RDS is ~17-22 dB harder than
stereo**. Perfectly clean stereo with no RDS is normal, not a bug.

An RDS listener needs roughly **35-40 dB of channel SNR**. Measured on air: Heart at 42 dB decodes
instantly; 98.2 at 25 dB produced nothing in 20 seconds, and a gain sweep across the tuner's whole
range did not change that. **That is an RF problem** — see `BRIEF-auto-gain.md`.

## 8. Tools built for this, and worth keeping

- **`vibedsp_rds_snr_probe`** — sweeps IQ noise AND subcarrier phase error with a realistic 5% RDS
  injection and a stereo L-R subcarrier present (leaving that out makes RDS look easier than it
  is). Reports whether the exact PS was recovered. ★ The phase sweep is the one that found §2, and
  a clean-phase test cannot show it at all.
- **`vibedsp_iq_probe`** — runs the real `RxPipeline` over an `rtl_sdr` capture, reporting pilot
  lock, stereo blend, every RDS group and time-to-first-name. ★ Built because the app's own
  recorder saves DECODED AUDIO, which can say nothing about RDS: the subcarrier is at 57 kHz and
  the 15 kHz audio filter discarded it long before anything was written.

## 9. Licensing notes for anyone extending this

Three RDS-adjacent projects were examined; two are dead ends:
- **FM-DX Webserver** — GPL-3.0, AND it does not decode RDS in software at all (it reads it from
  TEF668x / XDR-F1HD hardware tuners). Nothing to copy even if we could.
- **ka9q-radio** — GPL-3.0.
- **redsea** — MIT, genuine software MPX→RDS. The one usable reference, and the one credited.

★ The constraint is not GPL-vs-GPL. VibeSDR IS GPLv3. It is that the **App Store exception**
(`APPSTORE-EXCEPTION.md`) is a §7 additional permission Stuart grants **as sole copyright holder** —
incorporate someone else's GPL code and he can no longer grant it over their portion, and the store
build breaks. Permissive licences (MIT/BSD/Apache) are fine with attribution, as KissFFT already is.
