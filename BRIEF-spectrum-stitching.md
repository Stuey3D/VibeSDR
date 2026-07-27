# BRIEF: Stitched spectrum — several radios presented as one unbroken band

Stuart's idea, 2026-07-27, late. Overlapping profiles on 2+ radios sharing one antenna, composed
server-side and served to clients as a SINGLE continuous spectrum with no seams.

## Why it exists
No single device we support covers the FM broadcast band (87.5–108 MHz, **20.5 MHz**) in one
window. An RSP does 10 MHz nominal but only ~8–9 MHz is usable once the roll-off at each edge is
discounted — so **three RSPs** on one antenna, overlapped, would cover it. That unlocks the
configuration [[competitor_spyserver]] shows is an empty niche: a LOCKED-RANGE, MANY-USER server
where every listener picks their own station — and on FM, gets our RDS analyser with it.

★ The reveal is the point: the client sees one spectrum and cannot tell it is three radios.

## ★★★ THE BLOCKER, before any DSP
`local_sdr_shim.cpp` is a SINGLETON with file-scope globals (`g_vs*`, `p`), which is why
[[vibeserver_multiradio]] already concluded ONE PROCESS PER RADIO. So stitching is not "open three
devices in a loop" — it is **a compositor that consumes N radio processes**. That shape is
actually the right one anyway: each radio keeps its own shim, and a stitcher merges their output.
Do not start by trying to make the shim multi-device.

## The real engineering problems, and the fix for each
1. **Amplitude steps at the seams.** Every radio has its own gain and AGC, so noise floors differ
   and a naive splice shows a visible ledge. ★ FIX: align on the OVERLAP, never on absolute
   calibration — measure mean power in the shared region and offset each segment to match its
   neighbour. Self-correcting as gains drift.
2. **Roll-off dips.** Each segment is attenuated toward its own edges, so a hard cut leaves a
   notch at every seam. ★ FIX: crossfade across the overlap weighted by DISTANCE FROM EACH
   SEGMENT'S OWN EDGE — the bin further from its edge wins.
3. **Frequency offset.** Independent references; at 100 MHz, 1 ppm = 100 Hz. Sub-bin for a
   waterfall, so cosmetically harmless — but it means a signal sitting ON a seam appears at two
   slightly different frequencies. Worth a per-radio ppm trim in the profile.
4. **Time misalignment.** Three FFT streams arrive independently; assembling a row from unaligned
   frames makes the seams shimmer or tear. ★ FIX: timestamp frames and compose on a common
   cadence, buffering to the slowest radio.
5. **Which radio serves the audio?** The one whose window contains the VFO, with a handoff when
   the user tunes across a seam. A brief mute is acceptable; a stutter on every seam is not.
6. **Cost.** 3 × 10 MSPS 16-bit I/Q is ~120 MB/s off USB plus three wide FFTs. A Mac or a proper
   desktop, ★ NOT a Pi. Size this before promising it.

## ★ Be honest at the seams
Invisible in the UI, but the server should still SAY it is a composite (and where the joins are)
somewhere a DXer can find. A signal near a seam can be an artefact of the blend, and someone
reporting a catch needs to be able to check that. Consistent with the RDS panel's whole approach.

## Smallest first step
TWO radios, one overlap, FM band only, overlap-normalised with a linear crossfade — enough to
prove the seam is invisible and to measure the CPU. Three is the same code with more of it.
