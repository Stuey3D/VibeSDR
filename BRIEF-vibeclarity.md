# The IF filter, the AGC, and what VibeClarity taught us

★ Renamed in spirit: VibeClarity as an automatic *decision-making* suite is **abandoned**. What
survives is a band-aware AGC and an IF filter tied to the zoom. This brief is kept because the
reasons matter more than the code did.

## Why the automatic version could not be made to work
Every part measured well alone — framing found **+15 dB** on 105.4 by trial where the arithmetic
said the opposite; focus took **10.8 dB** off the converter. Together they were worse than nothing.

★★★ **THE LEVERS INTERACT, AND EVERY VERIFICATION IS CONFOUNDED BY THE LEVER IT MEASURES.**
- Narrowing the IF cools the converter → the AGC reads headroom and climbs → straight back into the
  overload the filter just cured. *"because of the 350KHz width its overcooking the AGC."*
- So a filter trial judged on the converter measures the **AGC**, not the filter: *"IF 450 kHz did
  not cool the converter (−2.7 → −1.7 dBFS)"* while it was doing its job.
- Moving the RF centre off a blowtorch pushes the **listener** into that same filter's skirt,
  because the filter is centred on the **tuner**: *"I'm now in the filter part."*
- Separation — which everything was judged on — is blind to **cross-modulation** and to
  **compression**: both terms of the ratio rise together. See [[cross_modulation_not_intermod]].

★★ **And one part actively broke HF**: the width test compared against 2× the demodulator
bandwidth — 18 kHz on AM, narrower than any real signal — so it fired permanently and walked the
gain to zero. *"trying it on HF and its set the gain to 0."*

Stuart's verdict: *"annoyingly on some signals it works well and others it ruins them ... I think
there is a reason this hasnt been done before."*

## The three modes (Stuart's spec, 2026-08-25)
| mode | IF filter | view | RF centre |
|---|---|---|---|
| **Single user, unlocked** | zoom-tied, full range | free pan + zoom | follows VFO + small offset (DC spike) |
| **Shared VFO, unlocked** | zoom-tied, full range | **shared zoom, NO panning** — pinned to the VFO | follows VFO |
| **Locked frequency, independent VFOs** | **none** — untouched | per-listener as today | pinned by the owner |

★★★ Mode 3 gets no filtering **by design**: *"Cant have a 2.4MHz sample rate and promise a
3-5.4MHz spectrum only to then IF filter half or 3/4 of it. Server owner if they need higher
selectivity will simply select a lower sample rate."*

## ✅ Built (vibeserver 4.1.0-34)
- **Auto — follows zoom** in the IF FILTER dropdown. Never narrower than the view, so it cannot
  hide a signal and needs **no overlay** (the overlay is gone). Verified: 2.4 MHz view → wide;
  1.2 MHz → 1458 kHz; 614 kHz → 844 kHz; 154 kHz → 384 kHz; view panned off the VFO → wide again.
- **The tuner follows the VFO** in Auto, at the usual +15 kHz DC-spike offset. Without it the
  centre gets left behind — *"the RF centre has stayed down at 663 and I'm up at 1467 ... I am in
  the cutoff of the filter."*
- **Locked-frequency receivers are excluded** entirely.
- **Shared radios take the widest listener's view**, so nobody gets dead band from someone else's
  zoom. (Superseded once mode 2 shares the zoom.)
- The width is on the status chip beside the gain, marked `auto`, so it is visible it is working.

## ▶ NOT built — next, and specified
1. **Mode 2: shared zoom + no panning.** The view is pinned to the VFO and the zoom is shared, so
   shared VFO becomes "our version of FM-DX where everything is shared". ★★ The safe shape is
   SERVER-SIDE ONLY: refuse a pan (force the view centre to the VFO) and apply a zoom to every
   client — exactly how the dial already works, with no client pushing state at another client.
   ★★★ **CAUTION:** the per-client view code carries the comment *"the hardware centre still never
   moves — it is locked"* — it is mode 3's path. Mode 3 must stay untouched.
2. **TEF6686-style AUTO BANDWIDTH for the DEMODULATOR.** FM only. A button beside IMS/CEQ/NR and a
   readout in the Advanced RDS window. Stuart measured the TEF doing 200 kHz on Heart, 184 on Flex,
   **134 on weak Smooth** — it narrows *below* the channel for weak signals, trading audio
   bandwidth for noise rejection. ★★ This is **DSP**, not the tuner: our IF floor is ~350 kHz, so
   134 kHz is unreachable in analogue. It is per-listener, so it cannot fight framing or affect
   anyone else. ★ Different failure from the IF filter: bandwidth helps a WEAK signal in a clean
   front end; the IF filter helps a clean signal behind a strong neighbour.

## The tools are the durable part
`scripts/agc-sweep` (hold the station, move the gain) · `agc-band-scan` (hold the gain, walk a
LABELLED band — this is what caught the false positives) · `agc-width` · `agc-iffilter`
(differential, the only way to see the filter's real shape) · `agc-framing` · `agc-settle`.
