# RAW IQ OUT IS THE REAL IQ (V12, branch `iqout-wide`, 2026-11-24)

## The fault (measured live, Pi 500 RTL-SDR V4, server beta92)

Raw IQ out (`iqout` → an rtl_tcp-style stream at 48 / 96 / 192 / 250 kHz) was **not** raw IQ:

1. **Wrong content.** The tap was the listener's demodulator channel AFTER the cascade's last stage — the listener's
   channel filter — resampled up. A 250 kHz request on USB carried ±3 kHz; on NFM ±7 kHz. PSD of a 250 kHz capture:
   −3..3 kHz 50.7 dB, 10–60 kHz 20 dB, 60–125 kHz 13 dB (the floor). `setIqMinRate` raised the channel's RATE, never its
   passband.
2. **Lost samples, a pinned core.** Raising the channel rate to ≥ 250 kHz ran the listener's whole audio chain there:
   vibe-demod at 99.5 % of a Pi 500 core (nice −19); 11.3 s of 20 s delivered (USB), 15.5 s of 20 s (NFM); a 25-minute
   recording came out with 71 % of its samples. Stuart, 11-23: "audio keeps breaking up though with iqon" — the same cost.

Also found on the way: the old writer resampled with `RationalResampler(int, int)`, whose ratio is **approximated** past
L = 256 (a stream that drifts by ppm), with the audio filter design (−6 dB at 0.45 of the rate, flat only to ~0.35,
Hamming ~53 dB); and `iqStopDirect` reset `setIqMinRate(0)`, which also dropped a DRM listener's 24 kHz floor.

## The design: option (b) — a tap of its own, off the baseband

Option (a), a second consumer of the APRS decoder-tap mechanism (the cascade's last stage INPUT), was rejected: that input
runs at `chFs_ × lastD` — 24 kHz for a USB listener at 2.4 MS/s — so a 250 kHz stream would still need the channel raised
(the cost that broke the Pi), and widening the anti-alias stages to protect ±112 kHz would change the listener's own chain
(their output would no longer be bit-identical with IQ out on).

So raw IQ out has its own tap, in two halves (vibedsp.h, `IqOutDecimator` / `IqOutResampler`):

- **`IqOutDecimator`** (ddc.cpp) — on the thread that owns the samples. From the dial-at-DC baseband straight to an
  integer decimation `Fd = fs/D` in [R, 2R). Every stage only has to keep ±P (P = 0.45 R) clear of folds: passband ±P,
  stopband from `fout − P`, transition centred on `fout/2` — so a stage by two is exactly a half-band
  (`HalfbandDecimator`, NEON/SSE2), the odd factors short Blackman FIRs (`FirDecimator`, NEON/SSE2). D and the stage order
  are chosen by cost (stage MACs ×2 because they are on the DSP/listener thread, plus the writer filter's ≈ 55·Fd).
  Optional NCO shift (the per-listener residual).
- **`IqOutResampler`** (resampler.cpp) — on the stream's writer thread (vibe-iqout, network rung): `Fd` → the consumer's
  EXACT rate (`RationalResampler` ExactRate, a new band-design constructor: flat to ±0.45 R, Blackman from ±0.55 R).
  Skipped when `Fd == R` (the decimator's last stage already stops from 0.55 R).

Wiring:

| Path | Before | Now |
|---|---|---|
| Direct (one pipeline) | `rx.setIqMinRate(rate)`; the channel IQ (`cb.iq`) | `rx.setRawIqRate(rate)`; `Callbacks::rawIq` off `baseBuf_` before the cascade — no rebuild, the chain untouched |
| Per listener (locked) | `chanBinsFor(rate/2)` widened the listener's slice + pipeline; the slice tapped | `iqTapClient`: a slice of its OWN off the shared FFT (≥ 1.2 R, a sixteenth roll-off — flat to ±0.4375 of it), its own decimator; the listener's slice untouched |
| Full rate | the raw block | unchanged (passes straight through `IqOutResampler`) |
| DRM floor | `max(24 k, iqDirectRate())` | 24 k alone; IQ out never touches it |

Every refusal rule is unchanged (owner switch, LAN-only, shared dial, DAB, paging, slots, the tunnel's 48 kHz).

## Measurements

### Content (vibeserver/test-iqout-wide.cpp — the real tap, 20 s synthetic captures)

In-band tones at −0.30, −0.10, +0.40, +0.445 × R; out-of-band at +0.62, −0.75, +1.3, −0.58 × R (checked wherever they
alias to).

| Case | In-band worst | Out-of-band worst | Second 10 s of 20 s |
|---|---|---|---|
| direct 2.4 MS/s USB/NFM, 48/96/192/250 k | −0.004 dB | −83.8 dB | exactly R × 10 |
| direct 2.048 MS/s USB/NFM, 48/96/192/250 k | −0.004 dB | −84.9 dB (250 k: −130) | exactly R × 10 |
| direct 768 kS/s AM 192/250 k; 8 MS/s WFM 250 k | −0.012 dB | −83.8 dB | exactly R × 10 |
| per-listener 2.048 MS/s/16k FFT 48–250 k; 2.4/32k, 8/32k 250 k | −0.002 dB | −83.6 dB | rate × input, ≤ 0.11 ms delay |

The listener's audio is **bit-identical** with IQ out off, on, and switched on/off/between rates mid-stream (USB, NFM,
WFM). Before: the +0.40 R tone came out 161–176 dB down (i.e. not at all).

(The per-listener slice was first extracted with the listeners' quarter roll-off — flat only to ±0.25 of the slice — and
the test caught it: −2.6 to −5.6 dB at +0.445 R. Fixed with the sixteenth roll-off, `kIqTaperDiv`.)

### Cost — Mac (M5 Pro), thread CPU per second of signal, best of 3

| | IQ off | old (channel raised) | new | writer old / new |
|---|---|---|---|---|
| USB 250 k @ 2.4 MS/s, DSP thread | 0.34 % | **60.6 %** | 0.60 % | 0.24 / 0.25 % |
| NFM 250 k @ 2.4 MS/s | 0.41 % | 4.0 % | 0.65 % | 0.23 / 0.23 % |
| USB 48 k @ 2.4 MS/s | 0.33 % | 1.75 % | 0.57 % | 0.04 / 0.05 % |
| NFM 192 k @ 2.048 MS/s | 0.45 % | 2.28 % | 0.63 % | 0.17 / 0.18 % |
| per-listener 250 k @ 2.048 MS/s (extract + decimate) | — | — | 0.34 % | 0.20 % |

(The old USB figure is the SSB chain — Weaver + selectivity — run at 300 kHz; on the Pi 500 that was the 99.5 % core.)

### End to end (scripts/test-server-iqout-wide.mjs — a real vibeserver on the Mac, fake rtl_tcp at 2.4 MS/s, USB dial, a carrier 100 kHz off it)

| | carrier over floor | consumer rate | server CPU, IQ off → on |
|---|---|---|---|
| 11.0.16 (old code), one pipeline | −2.9 dB (absent) | 499.8 kB/s | 2.2 % → **63.3 %** |
| 11.0.16, per-listener | +2.4 dB (absent — the slice was flat only to ±75 kHz) | 499.4 kB/s | 2.7 % → 3.4 % |
| **new**, one pipeline | **78.5 dB** | 499.3 kB/s (fake source's pacing), 0 drops | 2.4 % → **3.6 %** |
| **new**, per-listener | **67.3 dB** | 499.3 kB/s, 0 drops | 2.5 % → 3.1 % |

### The plan per radio (complex MAC/s on the sample owner's thread; writer taps per output and rail)

| Capture → rate | Stages | Fd | DSP | Writer |
|---|---|---|---|---|
| 2.4 MS/s → 250 k | /2 hb4 · /2 hb5 · /2 hb12 | 300 k | 13.5 M | 67 |
| 2.4 MS/s → 48 k | five half-bands | 75 k | 12.0 M | 86 |
| 2.048 MS/s → 250 k | /2 hb4 · /2 hb6 · /2 hb23 | 256 k | 14.8 M | 57 |
| 768 kS/s → 250 k | /2 hb7 | 384 k | 3.1 M | 85 |
| 8 MS/s → 250 k | five half-bands | 250 k | 45.8 M | pass |
| 10 MS/s → 250 k | five half-bands | 312.5 k | 51.2 M | 69 |

### Pi estimates (not measured on a device — please confirm)

Scaled from the Mac figures and the MAC counts (M5 P-core vs Cortex-A76 ≈ 3–4× on these NEON loops, vs Cortex-A7 at
900 MHz ≈ 20–30×):

- **Pi 5 / Pi 500**, 2.4 MS/s → 250 k: DSP thread +~1 % of a core, writer ~1 % (was vibe-demod 99.5 % and 29–44 % lost).
- **Pi 2**, 2.4 MS/s → 250 k: DSP thread +~5–8 %, writer ~5–8 %; at 48 k about the same DSP share (the half-band chain
  costs the same at the input rate) and a third of the writer.

A quick device check: `iqout` at 250 kHz with `top -H` (vibe-dsp, vibe-demod, vibe-iqout) and the log line
`raw IQ out: tap … → … (… M MAC/s), writer … taps/output`.

## Open / for Stuart

- The iOS prebuilt library (`modules/vibe-local-sdr/build_ios.sh`) is not rebuilt on this branch (other agents are
  editing native code too) — it needs the usual rebuild before the next iOS build.
- Flatness is ±0.45 × rate and ≥ 60 dB (measured ≥ 83 dB) from ±0.55 × rate. A consumer that wants flatter to the band
  edge would cost the writer more taps (the transition is 0.1 R).
- The per-listener stream follows the listener's dial and any satellite Doppler shift (the same NCO residual the
  listener's own channel gets); the direct path follows the pipeline's tune, which includes it too.
