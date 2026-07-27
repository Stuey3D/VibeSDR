# BRIEF: Covering a whole band with several radios — SIDE BY SIDE, not stitched

Stuart's idea, 2026-07-27/28. Several radios on one antenna, their ranges overlapping at the
edges, covering a band no single device can hold — the FM broadcast band (87.5–108 MHz, 20.5 MHz)
needs three RSPs, since only ~8–9 MHz of a nominal 10 MHz survives the edge roll-off.

It unlocks the configuration [[competitor_spyserver]] shows is an empty niche: only 2 of 240
public SpyServers lock their tuning range for many users, and both belong to SpyServer's own
author. A locked FM-band, many-listener receiver with our RDS analyser has no equivalent.

## ★★★ DECIDED: PRESENT THEM SIDE BY SIDE. DO NOT BLEND.
The first version of this brief proposed STITCHING the radios into one seamless spectrum. Stuart
rejected it, and he is right, for a reason that outranks the tidiness:

**The overlap is not redundancy — it is a genuine CHOICE, and blending destroys it.**

His own evidence, on his RSP1A: **HFM on 102.3 is receivable on the FM Middle profile but
LISTENABLE WITH RDS on FM Upper**, because it sits near Middle's roll-off and comfortably inside
Upper's. A crossfade would have produced something BETWEEN the two — worse than the best
available. Cosmetic on a waterfall; actively harmful on the audio and RDS path, and RDS is
precisely where it bites, because RDS is SIGNAL-LIMITED
([[rds_signal_limited_not_decoder]]) — a few dB decides whether you get a station name or nothing.

★★ Consistent with how this project treats every other measurement: show what the receiver
actually hears, do not average it into something nobody is receiving.

### What that saves
No time alignment, no amplitude normalisation, no ppm trim between references, no seam artefacts,
no compositor process — and **no singleton refactor**. Each radio keeps its own shim and its own
listeners, which is exactly the one-process-per-radio shape [[vibeserver_multiradio]] already
concluded. This is buildable on what exists; stitching was not.

## ★★ THE MEASURED LAYOUT (Stuart's RSP1A, OpenWebRX, 2026-07-28)
Three 9 MHz windows with deliberate overlap:

| profile | range | overlap with neighbour |
|---|---|---|
| FM Broadcast Band – Lower  | 87–96 MHz  | — |
| FM Broadcast Band – Middle | 94–103 MHz | 2 MHz with Lower |
| FM Broadcast Band – Upper  | 100–109 MHz | 3 MHz with Middle |

★★★ **HFM 102.3 IS THE WHOLE ARGUMENT IN ONE NUMBER**: 0.7 MHz from Middle's top edge, 2.3 MHz
inside Upper's bottom. Receivable on Middle, listenable WITH RDS on Upper.
- ★ Implies **the outer ~1 MHz of a 9 MHz RSP window is compromised** — about 10% per edge, so
  ~7 MHz of every 9 is genuinely good. Tiling 87.5–108 (20.5 MHz) with 7 MHz good regions needs
  exactly THREE windows, which is why three RSPs is the real answer and not a round-up.
- ★ He uses the same pattern beyond FM — Airband 117/124/131, 70cm 425/430/435 — and on the
  RTL-SDR v4, 2.4 MHz windows with a 0.4 MHz overlap (~17%). **Overlap at least ~20%** so every
  frequency lands inside somebody's good region.

## ★★ THE ONE ADDITION: SIBLING AWARENESS
Side-by-side alone has a discovery hole. Stuart knows about HFM because it is his antenna and his
band; a visitor would simply never learn the station is better next door.

So the server should know its profiles OVERLAP and tell the client:
- Tuned inside an overlap (or near an edge) → "also covered by **FM Upper**", one tap, SAME
  frequency, different radio.
- ★★★ Which makes possible a feature nobody else has: **A/B the same station on two radios**,
  side by side, with our RDS panel on each. Signal, scatter, error rate, whether RDS locks at
  all. That is a DX tool, and it exists ONLY because we did not blend.
- ★ Say WHY the alternative might be better — "nearer the middle of that receiver's range" — or
  it looks like an arbitrary suggestion.

### ★ The rule is trivial to compute, and needs no measurement
For a frequency `f`, prefer the profile maximising **fractional distance from its nearest edge**:
`min(f - lo, hi - f) / (hi - lo)`. HFM: Middle scores 0.7/9 = 0.08, Upper 2.3/9 = 0.26 — Upper
wins, which is what Stuart hears. ★ Pure arithmetic on numbers the operator already typed in;
no signal measurement, no calibration, nothing to drift. Only suggest when the gap is worth a
switch (say the alternative scores at least twice as well) or it will nag over trivia.

## Still to decide
- Does one operator's set of radios appear as one server with N profiles, or N servers? Profiles
  is closer to what Stuart already runs under OpenWebRX and to how people think about a band.
- Occupancy interacts with this: three radios = three slots. The IN USE badge
  ([[vibeserver_sharing_limits]]) should show them per-radio, and "busy here, free next door" is
  a much better answer than a flat IN USE.
- Cost is now per-radio and independent, so a Pi can plausibly run ONE of them — unlike the
  stitching design, which needed a desktop for the composite.

## Rejected: seamless stitching (kept for the reasoning)
Compose N radios into one continuous spectrum, invisible to the client. Rejected above. If it is
ever revisited, the traps were: amplitude steps at the seams (align on the OVERLAP, never on
absolute calibration), roll-off dips (crossfade weighted by distance from each segment's own
edge), ppm offset (100 Hz per ppm at 100 MHz — sub-bin, but a signal ON a seam appears twice),
unsynchronised FFT frames (timestamp and compose on a common cadence, or the seams shimmer),
audio handoff across a seam, and ~120 MB/s of USB plus three wide FFTs.
