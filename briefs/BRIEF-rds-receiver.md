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

---

## ★★ ADVANCED RDS PANEL (FM-DX) — scoped 2026-07-26

Stuart's plan: a **Standard / Enhanced RDS** toggle, Enhanced aimed squarely at the FM-DX
community. The benchmark is an FM-DX Webserver (io95.fmtuner.org), whose display we studied.

### What that display actually contains — and the split that matters
- **From the air:** PS, PI, PTY, TP/TA, stereo, MS, AF list, RadioText.
- **From a DATABASE, keyed on PI:** the logo, "Pride Radio", "Newcastle upon Tyne", 0.05 kW,
  11 km, 122°. Distance and bearing are computed from the receiver's own position.

★ So most of the visible richness is a PI LOOKUP, not harder decoding. And PI is the most
robust field in RDS (block A of every group, ~11/sec, confirmable by repetition), so the
database half is reachable BEFORE the decoder improves — the opposite of the intuitive order.

★ Their advantage is not a cleverer algorithm: a TEF6686 decodes RDS in SILICON and hands the
host finished blocks, which librdsparser merely assembles. We do the whole chain in DSP.

### Contents of the panel
1. **PI code** — its own field. SHIPPED 2026-07-26 (decoder → pipeline → shim → clients).
2. **Block error rate** — errors before correction, last 12 groups. SHIPPED.
3. **Subcarrier level vs pilot, dB** — SHIPPED. Separates "not arriving" from "arriving and
   wasted".
4. **★ CONSTELLATION PLOT** — Stuart's request after seeing SDR++ Brown's. The DSP side is
   DONE (`RdsDemod::constellation()`, 64 recent symbol points, from the winning hypothesis,
   normalised by the running RMS so scale is stable and only SHAPE varies). NOT yet wired to
   any client — it belongs here, not bolted onto the station bar.
   ★ Two tight clusters = healthy; a diffuse cloud = subcarrier buried in noise. That single
   picture says at a glance what took three instruments and a whole evening to establish.
5. **Correction strength** — `RdsDecoder::setMaxBurstBits()`, 1..5, DEFAULT 2. The standard
   permits 5; see the warning below before offering it casually.
6. Later: PTY, TP/TA, MS, AF list, and the PI-keyed database lookup (logo/name/site/ERP/
   distance/bearing), which also feeds the FM-DX dial and learned stations.

### ★★ MEASURED, AND DO NOT REPEAT
- **5-bit burst correction REGRESSED it on air.** BBC Radio 4 in solid stereo produced NO RDS
  at all. A false correction on block A yields a wrong PI; the parser compares consecutive
  PIs, they disagree, `piLast_` is overwritten with each invention, so PI never confirms —
  and with PI gating repaired groups, nothing gets through. Strong stations were unaffected,
  which is exactly the pattern seen. The old 2-bit cap was a correct engineering judgement.
- **A symbol timing loop cannot help. Twice failed, and now understood.** The bit clock is
  the PILOT DIVIDED BY 16, so its frequency is exact and the only unknown is the divider's
  starting state — sixteen discrete possibilities, which is precisely what the hypothesis
  bank enumerates. One hypothesis is EXACTLY right, not approximately. There is no better
  sampling instant to find.
- **Widening the RDS baseband filter did nothing** (2400/2400 → 3200/1600), and cost groups
  at moderate noise. Reverted.
- **Relaxing the parser's gates bought ~1 group.** They were never the bottleneck.

### ★★★ THE ACTUAL LIMIT IS THE SIGNAL, NOT THE DECODER (proven three ways)
1. Our decoder makes **0% block errors** on a clean synthetic signal — no error floor.
2. **SDR++ Brown, same dongle, same antenna, same stations, struggles identically** — and on
   BBC R2 88.6 it showed PI `0xC202` and NOTHING ELSE, where we showed PI *and* resolved
   "BBC R2". Two unrelated implementations failing the same way means the input is at fault.
3. Its **constellation was a diffuse cloud**, not two clusters.

★ Leading explanation: the **8-bit dongle**. RDS is injected ~30 dB below peak deviation and
an 8-bit ADC gives ~48 dB of range, so a strong FM carrier leaves the subcarrier near the
quantisation floor — audio uses the top 30 dB and sounds perfect while RDS drowns. Fits the
evidence that RDS flips between decoding and nothing over ~2 dB of gain, and that MORE gain
helped until intermod took over. ★ NEXT TEST: build SDRplay (14-bit) support into VibeServer
and repeat. That is now a well-posed hardware question, not a software mystery.

★ Consequence: **auto-gain (`BRIEF-auto-gain.md`) is an RDS fix**, not a nice-to-have — one
control, two opposite failures. And BER gives it a real objective to hill-climb on instead of
a spectral heuristic.

### ★★ THE ADVANCED RDS BOX — field inventory (Stuart, 2026-07-26)

"Make an advanced RDS decoder box that has those cluster lines, the PI code and all the
other RDS data we discard." Today the parser handles group types 0 (PS), 2 (RadioText) and
1 (ECC) and drops everything else.

**★ TIER 1 — FREE. Already decoded, already error-checked, then thrown away.**
Block B is REQUIRED for every group we parse (it carries the group type and address), so its
other bits cost nothing at all — we read four of sixteen and discard the rest:
- **PTY** (bits 9-5) — programme type, on EVERY group.
- **TP** (bit 10) — traffic programme flag, on EVERY group.
- **TA** (bit 4), **MS** music/speech (bit 3), **DI** (bit 2) — on type 0 groups.
★ Same shape as the PI bug: recovered, validated, then binned at the door. Do these first —
they are a parser change with no DSP work and no extra bandwidth.

**TIER 2 — cheap, new group types**
- **AF list** (0A, block C) — alternative frequencies. The FM-DX display scores these.
- **CT** (4A) — clock time and date; also a free sanity check on decoder health.
- **PIN** (1A, alongside the ECC we already read).

**TIER 3 — worth it for the FM-DX audience**
- **RT+** (3A/11A ODA) — artist/title tagging inside RadioText. This is what turns a scrolling
  message into now-playing metadata, and it feeds the media card we already publish.
- **EON** (14A) — other networks, a DXer favourite.
- **Group-type histogram** — which groups a station actually transmits, and how often. Cheap
  (a 32-entry counter), and genuinely diagnostic: it identifies a transmitter's configuration
  and shows at a glance whether a weak signal is delivering a representative mix.

**TIER 4 — the receiver's own health (all SHIPPED except the plot)**
- **PI code** — SHIPPED. **BER** — SHIPPED. **Subcarrier level vs pilot** — SHIPPED.
- **★ CONSTELLATION PLOT** — DSP side DONE (`RdsDemod::constellation()`); needs a client.
  ★ Add **EVM** alongside it: cluster tightness vs scatter. The level figure alone cannot
  separate signal from noise — BBC R2 measured a perfectly healthy -10 dB while running 66%
  block errors, because that RMS includes in-band noise. EVM is the number that closes that
  gap, and the points are already captured.
- **Correction strength** (`setMaxBurstBits`, default 2) — with the on-air warning above.

★ PROVEN ON AIR 2026-07-26: on BBC R2 88.6 we decoded PI + name where **SDR++ Brown failed
entirely**, and on a station where it managed only a PI we managed PI *and* name. We are ahead
of the reference software implementation on identical hardware; the remaining gap is against
dedicated tuner silicon, not other SDR software.

### ★★ WHERE IT LIVES: with the other decoders (Stuart, 2026-07-26)

Advanced RDS is a DECODER, so it belongs in the decoder box beside FT8, WEFAX, SSTV, RTTY
and NAVTEX — same selection, same panel, same real estate.

★★ That settles the CPU question by itself. The earlier plan was a Standard/Enhanced setting,
possibly a server-owner option, because the extra decoding could burden a weak host. As a
decoder there is nothing to configure: **selecting it IS the toggle**, the cost is paid only
while someone is looking, and it stops when they close the box — exactly like the others.
No setting to explain, no server option to document, and no way to leave it running by
accident. ★ Prefer a structure that makes a setting unnecessary over a setting.

Consequences:
- The base RDS path (PS, RadioText, PI, stereo) stays ALWAYS ON and unchanged — it feeds the
  station bar, the media card, learned stations and the FM-DX dial, none of which can depend
  on a panel being open.
- Everything in Tiers 1-4 above is computed only while the decoder is selected, EXCEPT the
  Tier 1 fields, which are free (they fall out of a block B we already parse) and can simply
  always be available.
- ★ **AF is explicitly wanted** (Stuart) — the alternative-frequency list, as the FM-DX
  display shows with its score. It rides in 0A block C, which we already receive and discard
  today; it also has real utility beyond display, since AF is a ready-made list of where else
  the same PI can be found — directly useful to the FM-DX dial and to learned stations.
- The decoder box is a natural home for the constellation + EVM, the BER and the subcarrier
  level: diagnostics belong where someone is deliberately looking at signal quality, not on
  the station bar where they would be noise to an ordinary listener.

### ★★ VTS BAR vs DECODER BOX — no duplication (Stuart, 2026-07-26)

**The station bar identifies; the decoder box diagnoses.**

- **VTS bar keeps: the PI code beside the name**, and nothing else new. PI is
  IDENTIFICATION — it is what the station is — so it belongs where the name is, and it is the
  one field that survives when the name cannot be assembled. SHIPPED.
- **VTS bar loses: BER and subcarrier level.** They were shown there while they were being
  used to diagnose the decoder, and they do not belong: a DXer wants them, an ordinary
  listener reads them as clutter beside a station name. ★ Kept in the chip's TOOLTIP, so the
  measurement is a hover away — it cost an entire evening to be able to see these at all, and
  they should never be more than a hover from anyone debugging a station.
- **★★ WHEN THE ADVANCED RDS DECODER IS OPEN, HIDE THE VTS BAR ENTIRELY** and show everything
  in the decoder box — name, PI, RadioText, PTY/TP/TA/MS, AF, plus the diagnostics. No point
  having the same data twice, and the bar is the smaller, more compromised presentation of it.
  ★ It also gives the decoder box the screen space back: it is the surface the user has
  deliberately chosen, so it should be the complete one, not a supplement to a strip that is
  already saying half the same thing.
- The bar's other duties (bookmark/EiBi fallback names, band label, the media card) are
  unaffected and continue while the decoder is closed. ★ Note the media card reads the VTS
  name — hiding the bar must not stop PUBLISHING it, only stop DRAWING it. Same trap as the
  cleared-textContent bug: hiding an element is not the same as having no data.
