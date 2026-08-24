# VibeClarity — automatic front-end management

> Combines VibeAGC with automatic RF centre offsetting and IF filtering to remove strong signal
> interference from where you are wanting to listen. — Stuart, 2026-08-25

Three levers against one failure: a signal strong enough to make the receiver misbehave somewhere
you are not listening. All three are measured, on air, on the Pi's RTL-SDR Blog V4.

| stage | lever | what it does | measured |
|---|---|---|---|
| **Framing** | RF centre | moves the offender away from the centre of the capture | **7.2 dB** off the ADC peak at 105.4 |
| **Focus** | tuner IF filter | attenuates what is left, ahead of the mixer | **17.8 dB**, automatic → the 350 kHz floor |
| **Exposure** | VibeAGC | spends the recovered headroom on the wanted station | 0.9 → 25.4 dB of usable range |

★★★ **THEY COMPOSE, AND ON HF NEITHER IS SUFFICIENT ALONE.** On FM the beacon is 1.2 MHz away and
framing alone pushes it clean out of the window. On 40m, Radio Romania at 7250 is **150 kHz** from
a 7100 centre — *inside* the narrowest IF the hardware has (±175 kHz), so focus cannot touch it.
Move the centre to ~6.95 and it is 300 kHz out, comfortably rejected. A single-lever design would
have looked fine on FM and quietly failed on 40m.

## The measured facts it must be built on
- **The IF floor is ~350 kHz.** Commanding 300/250/200 kHz produces an identical ADC peak — the
  tuner or librtlsdr clamps. Useful range is 800 → 350 kHz; above 1 MHz nothing happens at FM
  spacings. ★ 350 kHz is WIDER than a 200 kHz WFM channel, so focus physically cannot eat a
  broadcast station.
- **`rtlsdr_set_tuner_bandwidth()` must be RE-ASSERTED** after every `set_center_freq` and every
  `set_sample_rate` — both silently undo it. See [[agc_if_filter_selectivity]].
- **The RTL has no anti-alias skirt worth cropping** at 2.4 MS/s — flat to both edges, measured
  (`scripts/agc-edge.mjs`). The HF+'s `edgeCutoffHz()` crop must NOT be extended to it.
- **The V4 reaches HF through a built-in upconverter**, so the R820T and its IF filter are in
  circuit on HF too — the whole mechanism transfers. (It would NOT with direct sampling.)

## ★★★ THE RULE THAT KEEPS IT HONEST: FOCUS MAY NEVER NARROW INSIDE THE VIEW
Stuart's concern, and it is the one that would sink this: *"when the locked VFO centre is disabled
and someone zooms out and pans about, we will need a display to show what parts are working."*

A listener panning across a region **we** are attenuating sees nothing there and concludes the BAND
is dead. That is the AGENTS.md fault in its purest form — the receiver misdescribing itself to the
one person who cannot tell. So:

### The first version of this rule was WRONG, and Stuart's own listening disproved it
It read: *"the view always wins — focus may only ever cut OUTSIDE what is currently on screen."*
Principled, and it would have thrown away the entire benefit. ★★★ What Stuart actually does by
hand is set **600 kHz while looking at a much wider span**, deliberately accepting attenuated view
edges because the audio is worth it — and he is right: *"the IF filter is great, really cleans up
105.4 and even allows 105.7 smooth to come in with stereo"*. Zoom-following focus would have
re-opened the filter to his view width and undone it.

**The rule is about the CHANNEL, not the view:**

> Focus may never narrow inside the channel being DEMODULATED. It may narrow inside the VIEW — but
> when it does, the spectrum must say so.

★★★ **THAT MAKES THE DISPLAY LOAD-BEARING, NOT DECORATIVE.** It is the thing that makes aggressive
filtering honest, and without it automatic focus is a receiver quietly lying about the band. Mark
where the IF passband ends; everything beyond it reads as "attenuated by the receiver" rather than
"dead band", and focus is then free to narrow as hard as it usefully can.

## The display
- **A state line, not a justification.** Zoom-driven focus is caused by the user, so it states a
  fact: `focus 450 kHz · following zoom`. On a shared radio: `focus 1.2 MHz · widest listener`.
- **Framing must be visible on the spectrum**, because it moves the capture underneath the view —
  mark where the RF centre is and where the capture ends, in the manner of the existing band bar.
- **Anything outside the capture must read as "not received", never as "empty band".** Same
  principle as the HF+ crop: stop where the radio does, leave the roll-off visible so it reads as
  a receiver rather than a cliff.

## Sharing
`c->viewSpanHz` / `c->viewCentreHz` are already per-client, so the IF is the **union of every
listener's view** — a single listener gets the full benefit, a shared radio opens up automatically,
and no policy switch is needed for the common case. Framing moves the capture for everyone, so on a
shared dial it needs the owner's consent (Stuart: locked RF centre keeps the span; single-user and
shared-VFO may auto-select; the owner can lock it on; a status must be shown).

## Build order
1. **The passband marker on the spectrum.** Small, no behaviour change, and it makes everything
   after it safe. Without it, automatic focus is a receiver quietly lying about the band.
2. **Automatic focus**, driven by the DEMODULATED CHANNEL plus a margin — NOT by the zoom (see the
   corrected rule above). That is what reproduces by itself what Stuart gets by hand.
3. **Framing** — automatic RF centre. Biggest blast radius: it moves the capture for every listener
   and interacts with the pan logic in `dongleForView()`. Wants the marker in place first, and
   tonight's 7250 test to prove the rule on a second band. Needs the interference detection (the
   spraying test already firing while the gain is low) and the same "prove it worked" verification
   everything else uses: the move must drop the interference while leaving the CHANNEL level alone,
   or it is handed back.
4. Exposure is already shipped (VibeAGC, band-aware since 4.1.0).

▶ **OPEN, and it needs an EAR not an instrument:** at 600 kHz, does a strong clean station (96.6,
104.2) sound duller than at automatic? The 350 kHz floor is wider than a 200 kHz WFM channel so it
should not be able to touch the audio — but that is a calculation and the skirts are real. If a
strong station is unchanged, automatic focus can be aggressive by default; if it dulls, it must
stay well back and the gain is smaller than today's numbers suggest.

★ Verify tonight: 7250 on 40m, which is a night-time path. FM alone would overfit the framing rule
to a 1.2 MHz spacing HF never sees.
