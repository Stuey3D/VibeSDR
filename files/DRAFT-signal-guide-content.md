# DRAFT — "Which Demodulator?" Signal ID guide (content only)

Status: **NOT in the app.** Pulled out 2026-07-15 to keep the shipping app clean.
This is the copy + structure, kept as an editable document. The implementation
(component `SignalGuide.tsx`, cropped screenshot assets, and the ModeSelector link)
was removed from the tree — re-add later if/when we want to ship it.

Design intent: offline, all-own-content (own-drawn SVG diagram for the AM teaching
picture + real captures from Stuart's own receivers for the per-mode "what it looks
like"). No third-party assets → no licensing/App-Store risk. Real captures live in
`~/Downloads` (the 18:14–18:27 screenshots); crop recipe was `scratchpad/crop.swift`,
rect `x0 y470 w1320 h1080 → 680w`.

Captured so far: AM, LSB, USB, WFM, RTTY. Still to capture: **CW, NFM, WEFAX, FT8**.

---

## Intro (Radio 101)

Every signal sits at a frequency — the **carrier wave**, the number in your frequency
box. What matters is how that carrier has been changed to carry sound or data: match
the demodulator to it and you get clear audio; get it wrong and it's just noise.

*(Diagram: AM signal — LSB · carrier · USB)*

An **AM** signal is the easiest to picture: the carrier in the middle, with a
mirror-image copy of the audio on each side — a **lower sideband** and an **upper
sideband**. Both sidebands carry the full audio, which is why AM sounds good enough
for music and broadcast. The trade-off is space — each signal is wide.

The amateur bands are crowded, so to squeeze in as many contacts as possible hams
throw away the carrier and one sideband and send just a single one — **SSB** (USB or
LSB). The audio is thinner and no good for music, but it's more than clear enough for
voice and fits far more signals into the same space.

At the other extreme, a **carrier-only** signal is simply the carrier switched on and
off — that's **Morse (CW)**.

---

## Per-mode cards

**AM — Amplitude Modulation**
Full carrier with a matching copy of the audio on BOTH sides (lower + upper sideband).
Sounds full and clean, so it's used for broadcast and airband. Wide and symmetric on
the waterfall.

**LSB — Lower Sideband**
The carrier and upper sideband are removed, leaving just the lower sideband — thinner
audio, but clear voice in far less space. By convention LSB is used on the ham bands
below 10 MHz.

**USB — Upper Sideband**
The same idea as LSB but the upper sideband is kept. Used on the ham bands above
10 MHz, for most VHF/UHF SSB, and a lot of utility voice.

**CW — Morse (Carrier Wave)**
The simplest signal there is: just the carrier, switched on and off. The short/long
pattern is Morse code. It looks like a single thin line that blinks. Use a narrow
bandwidth to pull it out of the noise.

**NFM — Narrowband FM**
Two-way radios, PMR446, and ham repeaters. A narrow, constant-width signal that
doesn't change shape with the audio (FM varies frequency, not amplitude).

**WFM — Wideband FM**
Broadcast FM radio (88–108 MHz). Very wide compared to everything else, often in
stereo and carrying RDS station text.

**RTTY — Radioteletype**
Text sent as two alternating tones (mark and space). Shows as two close parallel lines
flicking between each other. Set the correct sideband before decoding.

**WEFAX — Weather Fax**
Slow-scan weather charts sent line by line. A broad, steady band — decode it and the
chart draws in over a couple of minutes.

**FT8 — Weak-signal digital**
A hugely popular mode for making contacts under the noise. Eight tones in ~50 Hz, sent
in 15-second bursts, so you'll see a cluster of short parallel lines that all appear
and vanish together.

*Tip: the bandwidth sliders under the demodulator set how wide a slice you listen to —
narrow for CW and SSB, wider for AM and FM.*

Further reading: Signal Identification Wiki — https://www.sigidwiki.com/wiki/HF
