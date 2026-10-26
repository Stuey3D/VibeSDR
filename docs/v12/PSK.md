# PSK31 / PSK63 (V12): a listener's decoder and a skimmer, from one light engine

Branch `v12-psk` (worktree `~/VibeSDR-psk`), 2026-10-26. Engine: `android/app/src/main/cpp/decoders/psk_skimmer.{h,cpp}`.
Server: `vibe_psk_run.h` (one PSK run in a decoder host), `vibe_decoder_host.h` (ext `psk` and `pskskim`, sharing the CW
skimmer's IQ path, so `vibe_wide_tap.h` and the shared FFT's slice feed it unchanged), the shim (`vibeserver.json`
`"psk"`, the owner switch, the live gallery summary), background profiles `psk-skim-6` / `psk-skim-12`
(`vibe_bgdecode_config.h`, the setup page). Web: `web/client/src/decoders.ts`, `main.ts`, `index.html` — the PSK button,
ONE SIGNAL / SKIMMER, CW's list box shared. App: the gallery's PSK chip only (the panel is owed — see the end).

Built from the published description (G3PLX's PSK31 papers): 31.25 / 62.5 baud BPSK, a zero is a phase reversal and a
one no change, each reversal a cosine through zero (the pulse two symbols long), Varicode with "00" between characters,
idle = continuous reversals. No other decoder's source was read. QPSK31 is not built (see the end).

## Architecture

**One forward FFT per hop is both the carrier finder and every signal's channeliser.** N = 128·m (m 5-smooth, nearest
rate/375), so the bins are ≈ 2.9 Hz whatever the rate and every channel comes out at rate/m ≈ 375 Hz. The transform is
unwindowed (overlap-save, hop N/2); the carrier finder applies a Hann window to it in the frequency domain (½X[k] −
¼X[k±1]). A signal's channel is 128 bins around it, masked (flat to 0.30·fs, cosine to 0 at 0.45·fs) and inverted — one
128-point inverse per signal per hop, the middle half kept, its LO phase carried across hops so k0 can move. Nothing runs
at the input rate per signal. SIMD (NEON ARMv7-safe / SSE2 / scalar reference): the per-bin pass, the mask, the NCO, the
matched filter (vectorised across outputs).

**Finding carriers.** The fast average (≈ 1 s) summed over ±6 bins standing ≥ 3 dB over a floor (the 25th percentile of the
slow average in 94 Hz blocks; a block that is mostly signal takes its neighbour's value; straight lines between blocks so a
filter skirt is not a carrier). **A new channel starts a second in the past**: the last six transforms are kept and a new
channel is run over them first — a PSK63 CQ is over before the finder's average notices it.

**Squared-signal line search** per tentative channel (and every ≈ 4 s after): z² takes the data off BPSK, leaving a line at
twice the offset; 512 channel samples give it to ≈ 0.1 Hz over ±40 Hz. It puts the channel ON the carrier before the loops
start, and finds a second carrier near by. A line on the channel (±4 Hz) wins over a stronger one off it — two signals near
each other put lines between them (their cross product at the midpoint, each one's ±1/T side lines), and no channel is
spawned at the midpoint of two it already has.

**Per signal, two lanes** (PSK31 and PSK63 demodulators on the same channel samples): the matched filter (the transmitted
pulse, Hann of two symbols); its own ISI (≈ 1/6 of each neighbour — the eye a third closed, EVM capped near 10 dB) taken out
at the symbol rate; symbol timing from the envelope's baud-rate component in idle and the Gardner detector in text (after
the matched filter the envelope component all but vanishes in text: 0.03 of the mean, measured); a second-order Costas
loop (k2 large enough to follow 1 Hz/s) and an FLL that re-centres the channel; coherent decisions, differentially decoded;
Varicode. The lane with a clean constellation AND characters coming out leads (a PSK31 signal read at 62.5 Bd is half clean
but never makes a "00"); fixed once the signal is confirmed.

### Text you can trust — zero garbage on noise

Noise read as BPSK decodes to Varicode happily (short codes: e t o a and space), and so do carriers and FSK modes. The gate:

- **Per-symbol EVM** (amplitude and phase, against the Costas reference and the short amplitude average) → an Es/N0 for any
  stretch of symbols. A character is judged by its own symbols and the 12 after it (a signal that stops leaves its last
  noise characters uncommitted). A phase-only measure passed RTTY, CW and noise-like signals: a loop that follows phase makes
  any slow phase look clean.
- **Confirmation** (before anything is shown): 64-symbol Es/N0 ≥ 5 dB, the long average ≥ 3 dB, ≥ 6 characters at ≥ 3 dB
  with ≥ 3 letters, ≥ 85 % of the last 16 plausible text, a share of reversals between 25 % and 75 %, and —
- **runs of seven or more ones** under 6 % of the bits: only I J 3 ! BS hold such a run, so text keeps it to a few %, while
  an FSK signal read as BPSK (FT8: a tone held ten PSK63 symbols, then a jump) decodes to exactly those — "o,3IKII3i3e" from
  the 30 m FT8 segment before this test. A run of twelve marks the characters around it as not text.
- **Ghosts**: a channel whose decisions agree (or disagree) ≥ 90 % with a stronger neighbour's, instant by instant, is reading
  that neighbour — half a baud from a strong PSK63 signal every bit comes out inverted, clean and confirmable. Dropped.
- What was held while tentative is shown on confirmation from after the last noise-like stretch, at the confirmed bar.

**IMD** as G3PLX defines it — the idle's third-order products (±1.5·baud) over its two tones (±0.5·baud), on the channel
before the matched filter, 16 symbols of idle, shown only where the products stand above the noise (Es/N0 ≥ 15 dB).

**SNR** shown is Es/N0 — signal over noise in the baud rate's bandwidth (PSK31 = SNR in 2.5 kHz + 19 dB; PSK63 + 16).

## Measured (Apple M5 Pro, scripts/test-psk.sh — tools/psk-bench)

Synthetic per the description; SNR = the signal's mean power over the noise in 2.5 kHz; CER = edit distance over the whole
text (a late join counts), mean of three draws; signals start after the engine's 1.7 s warm-up.

| | +10 | 0 | −5 | −8 | −10 | −12 | −13 | −14 dB |
|---|---|---|---|---|---|---|---|---|
| PSK31 audio | 0.8 | 0.5 | 0.7 | 0.5 | 1.2 | 6.1 | 7.6 | 28 % |
| PSK31 IQ 48 k | 0.7 | 1.7 | 2.0 | 1.0 | 1.0 | 3.7 | 6.6 | 30 % |

| | +13 | +3 | −2 | −5 | −7 | −9 | −10 | −11 dB |
|---|---|---|---|---|---|---|---|---|
| PSK63 audio | 2.6 | 2.0 | 3.1 | 2.5 | 3.7 | 5.7 | 7.5 | 14 % |
| PSK63 IQ 48 k | 2.3 | 2.0 | 2.4 | 1.7 | 2.8 | 5.3 | 7.9 | 14 % |

(The ≈ 1–2 % floor is the first word of the first CQ, before the loops lock.) Drift: 1.4 % to 0.5 Hz/s, 2.5 % at 1 Hz/s,
lost at 2 Hz/s. Clock 200 ppm off: 1 %. Offsets across 48 kHz IQ (centre, edges, odd Hz) and 300–3300 Hz audio: ≤ 6 %.
IMD: −24.6 / −18.6 dB read on a transmitter whose own is −24.6 / −18.5. **Crowded passband** (12–14 signals in 3 kHz,
PSK31 and PSK63, one +16 dB above its neighbours, two PSK31 signals 31.25 Hz apart): every one decoded (the 31 Hz pair at
0.6 / 1.7 %), every CQ spotted at its own frequency, no call invented; the misses are late joins (a PSK63 CQ over before
the channel opened). **Noise**: 10 min at 12 kHz and 48 kHz → 0 characters, 0 spots; 5 min with the carrier finder forced
open on noise (1 500+ channels tried) → 0 characters. A strong carrier + FT8-like 8-FSK + RTTY-like FSK, 90 s → 0.

**CPU**, one core, a PSK31 signal every 150 Hz at −3 dB (all of them decoded):

| input | signals | SIMD (NEON) | scalar |
|---|---|---|---|
| listener 12 kHz audio | 21 | 0.028 % | 0.039 % |
| skimmer 24 kHz IQ | 144 | 0.18 % | 0.24 % |
| skimmer 48 kHz IQ | 288 | 0.38 % | 0.48 % |
| skimmer 96 kHz IQ | 400 (cap) | 0.56 % | 0.71 % |

Real clips at 48 kHz with no PSK in them: 0.13–0.20 %. The per-signal work dominates, so SIMD saves 25–30 % here.
**Pi 2 (Cortex-A7 900 MHz): an ESTIMATE, not measured** — ≈ 20–30× an M5 core on this float work: a 12 kHz skimmer on a
busy 20 m segment (≈ 15 signals) ≈ 0.5–1 % of one core, a listener's decoder ≈ 0.5 %.

**RAM**: ≈ 12 KB a signal; the engine's transforms and the six kept for new channels: ≈ 0.3 MB for a 12 kHz skimmer,
≈ 2.5 MB at 96 kHz (the history is 1.5 MB of that — the trade for a PSK63 CQ caught whole).

## Real signals

Recorded from Stuart's UberSDR (`~/VibeSDR-test-audio/psk/`). Monday morning (07:30–07:55 UTC) the PSK segments of 80, 40,
30, 20, 17, 15, 12 and 10 m held no PSK at all — RTTY at 7.040, CW, FT8 next door, carriers — and the engine decoded
nothing from any of it (no false text, after the run-of-ones and EVM gates went in; before them it read FT8 and carriers as
garbage, which is how those gates were found).

**Monday 16:05 UTC, 20 m** (`uber_20m_14071500_iq48_afternoon.wav`, 5 min): four PSK31 transmissions, all read and readable —
EA5JTI calling CQ and working PD8GB, OE5BAY, IW2NWK and DC6GU ("Report : 599 599 / Name : Michael John … qth Aguilas.
Murcia,Spain loc IM97el", "END OF QSO 26/10/2026 16:05:00 SK SK 73 73 … EA5JTI 73 Giancarlo and thanks for PSK31 QSO",
"DM7MA de EA3CFV, Gert thanks for this BPSK-31 QSO on 20m, good DX. I wish you and your family all the best, 73");
EA5JTI, OE5BAY and IW2NWK spotted, nothing false. Es/N0 9–14 dB as listed. **Same time, 40 m**: RTTY at 7.040 and a wide
data signal at 7.0495 — nothing shown. Before the last two gates (below) both made garbage rows ("t  t  t", "rr==rr",
"ere en e g o ee a -etye"):

- **texture** of the text that would be shown (spaces 4–32 %, ≥ 7–11 different characters, words 2–12 letters on average;
  live, only gross failure — over 45 % spaces or under six characters — so a CQ's two-letter words pass);
- **words a reader would know**: of the words held while tentative, ≥ 3 and ≥ 30 % must be QSO English, its abbreviations,
  numbers or callsigns (one-letter words other than K do not count);
- and each lane holds its own characters while tentative (a lead that changed lane left the text interleaved, and the
  EA3CFV over never confirmed).

## Tests

- `vibeserver/test-psk.cpp` (run-tests.sh): Varicode's shape, the floor, CER at 0/−8/−12 dB (PSK31) and −5 dB (PSK63),
  offsets, 1 Hz/s drift, 200 ppm clock, the crowded passband incl. the 31.25 Hz pair, noise plain and forced open, the
  carrier/FSK set, IMD, `psk` and `pskskim` through a DecoderHost, JSON escaping, the gallery's PSK card.
- `scripts/test-server-psk.mjs`: the real server on a fake rtl_tcp (five stations at 14.071 MHz): the skimmer on a
  one-pipeline radio and a range-locked one, a second listener's own segment, ONE SIGNAL's text and spot.
- `scripts/test-web-psk.mjs`: headless Edge (`--mute-audio`) at desktop and 390 px.
- `scripts/test-setup-bgdecode.mjs`, `scripts/test_admin_bg.ts`: the PSK skimmer profiles and admin line.

## Not done

- **The app's PSK panel.** The gallery chip is there; the panel is not. It should be CW's (`CwView`, `cwList.ts`) with the
  mode where the WPM is, `psk`/`pskskim` in `DecoderClient`, `"psk"` from vibeserver.json, a 'centre' sent with the
  attach, and a tap that tunes USB with the signal in the middle of the passband — the web's `cwTune`/`decParams`.
- **QPSK31** (G3PLX's QPSK with the rate-½ K=5 convolutional code and a Viterbi decoder): not built. It is rare on the air and
  the sideband/phase-mapping convention could not be checked against a real signal; it would be a third lane.
- **Soft-decision Varicode** (a trellis over the code's no-"00" constraint): not built; hard decisions with the EVM gate.
- ONE SIGNAL's cursor is the passband's middle at attach time; changing the filter afterwards does not move it.
- PSK at 2 Hz/s drift and below ≈ −13 dB (2.5 kHz).
- Real-air confirmation on busy evening bands (see Real signals).
