# JTTY (V12): the WSJT-X 3.2 keyboard mode — every JTTY signal in the passband, no slots

Branch `v12-jtty` (worktree `~/VibeSDR-jtty`), 2026-10-26. Engine: `android/app/src/main/cpp/decoders/jtty_decoder.{h,cpp}`.
Server: `vibe_jtty_run.h` (one receiver inside a decoder host, ext `jtty`), `vibe_decoder_host.h`, `vibeserver.json`
`"jtty"`, the owner's block switch (`vibe_setup_page.h`). Web: `web/client/src/jtty.ts`, `decoders.ts`, `main.ts`,
`index.html`. Tests: `vibeserver/test-jtty.cpp` (run-tests), `scripts/test-web-jtty.mjs` (server + web, headless Edge),
bench `tools/jtty-bench` via `scripts/test-jtty.sh`. Test audio: `~/VibeSDR-test-audio/jtty/` (README there).

Stuart, 2026-10-26: *"look into JTTY as it is new, it is a hybrid of FT8 and RTTY, if we can get in early this would give
us a significant boost"*.

**Status: working on real signals.** Ten minutes of 40 m off Stuart's UberSDR at 16:54 UTC on a Monday held two JTTY
stations, both decoded: **F4ELU/QRP** ("CQ F4ELU/QRP CQ", twice, at 7.0913735 MHz — PSKReporter has F4ELU/QRP in JTTY at
7091370 in the same hour) and **DK7OG** (a 30-frame free-text CQ, "CQ DK7OG JO62 … PSE K"). On 20 m, **N4YSA** calling DL5AXI and a frame
of **RN4AQ**, each within a few hertz of its PSKReporter report (§3). The sample recording the
WSJT-X 3.2 user guide ships decodes whole. No false frame in ten hours of noise, an hour of real FT8/FT4 or ten minutes of
strong RTTY.

## 1. The protocol — what is CONFIRMED, and from where

There is no published paper. The only full specification is the protocol text that ships **inside the WSJT-X source**
(GPL-3): `lib/jtty/jtty_source_encoding.txt` ("the canonical specification for JTTY framing, source encoding, and source
interfaces"), `lib/jtty/jtty_design.md`, the user guide's `jtty.adoc` / `protocols.adoc`, and the constants in the code.
Read at tag **v3.2.0-rc1** (commit `567ad29ce`) and **v3.3.0-beta4** (`0f7d5928`) of github.com/WSJTX/wsjtx; the wire
format is identical in both (the spec's wording changed, its tables and vectors did not). We read them to understand the
mode and wrote our own code from the description; no code or table was copied — every table below was typed in from its
meaning and is checked against the spec's ten published 34-bit golden vectors in `test-jtty`.

Confidence: **C** confirmed (stated in the spec/user guide AND reproduced: the golden vectors, the WSJT-X sample and real
off-air transmissions decode with our independent code), **S** stated in a primary source but not independently testable
here, **P** preliminary/secondary (news articles).

| Fact | Value | Source | Conf. |
| --- | --- | --- | --- |
| Modulation | 4-GFSK, BT = 2.0, modulation index h = 1 (tone spacing = baud) | spec "Frame and waveform"; `gen_jttywave.f90`, `transmit.f90` (bt = 2.0, hmod = 1) | C |
| Keying rate / spacing | 12000/384 = **31.25 baud**, tones 31.25 Hz apart, tone 0 lowest | spec; user guide Table (31.25) | C |
| Bandwidth | 125 Hz nominal half-power, 127 Hz occupied (99 %) | spec; user guide "125" | S |
| Frame | **13 sync + 46 data symbols = 59 symbols = 1.888 s** | spec; user guide "Tx Duration 1.888" | C |
| Sync | tones **0 2 2 3 0 0 3 2 1 3 1 2 0** at the start of every frame; its ACF peak sidelobe 2/13 | `jtty_fec_mod.f90` (`is13`) | C |
| Transmission | **1–16 frames back to back**, no gap (the next frame's sync follows the last data symbol); starts at **any** moment — no UTC slots | spec; `genjtty.f90`; user guide | C |
| Ramps | raised-cosine on the first and last nsps/8 samples of a transmission | `gen_jttywave.f90` | S |
| Payload | **34 bits**: grammar bits 1–32, bit 33 **reserved, zero**, bit 34 **end of message** (set on the last atom only) | spec "Source bits and validity" | C |
| Outer check | **CRC-12**, x¹²+x¹¹+x³+x²+x+1 (0x80F, x¹² implicit), register 0, MSB first, no final XOR, over the 34 payload bits; appended MSB first → 46 information bits | `jtty_tbcc_code_profile.f90` (`outer_polynomial=z'80F'`), `tbcc.f90` (procedure) | C |
| Inner code | **tail-biting convolutional, rate ½, K = 10** (memory 9, 512 states), generators **1167 and 1545 octal** (maximum-free-distance pair, d_free 12 per the source's note) → 92 coded bits; user guide: "(92,46)" | spec; code profile; user guide Table | C |
| Encoder detail | shift register with the NEWEST bit at the LSB; generator g applied to the 10-bit register; tail-biting start state = the last 9 information bits; no interleaver | `tbcc.f90` | C |
| Bits → tones | per information bit one symbol: (g0 out, g1 out) **00→0, 01→1, 11→2, 10→3** (Gray) | `tbcc.f90` | C |
| Validity | a receiver shall reject a frame failing FEC, CRC, the reserved bit, the all-zero sentinel or ANY grammar rule — before display, message assembly or subtraction | spec | S (we do exactly this) |
| Throughput | ~30 WPM free text, up to ~60 WPM structured (5 characters or one compact atom per 1.888 s) | articles; follows from the frame | P / derived |

### The source grammar (34-bit atoms) — confirmed by the golden vectors

Bits 31–32 (`i2`) select the class:

| i2.n2 | Form | Fields |
| --- | --- | --- |
| 0.0–0.3, 1.0–1.1 | six **call actions**: `CQ K1ABC CQ`, `K1ABC`, `TU K1ABC CQ`, `K1ABC TU`, `K1ABC AGN?`, `TU NOW K1ABC` | the FT8-family **28-bit standard call** (bits 1–28; only calls that round-trip — no tokens, no hashes, no compound calls, none starting with Q) + n2 |
| 1.2, 1.3 | reserved — invalid | |
| 2 | **STRUCT30**: body27 + family3 | see below |
| 3 | **TEXT5**: five 6-bit characters of `0-9 A-Z space + - . / ? ! " # $ % , & * ( ) _ ' = [ ] { } < > \| : ;` | the last atom padded with spaces |

STRUCT30 families: **000 EXCH_NUM** (role1 · kind4 — serial, CQ zone, ITU zone, age, power, check, licence year, generic —
· value17 · zero5), **001 EXCH_LOC** (role1 · kind4 — state/province, ARRL section, country prefix, generic QTH, local
admin — · length1 · base-36 token16 · zero5), **010 EXCH_PAIR** (ZONE_LOC3 `599 05 NWT`; CLASS_SECTION `1D EMA` with the
86 ARRL/RAC sections in the FT8 77-bit grammar's order), **011 EXCH_NUM_TIME** (`599 156 1749`), **100 MISC** (CONTROL —
18 phrases `AGN?` `CALL?` `AGN CALL` `NR?` `AGN NR` `EXCH?` `STATE?` `SECTION?` `ZONE?` `GRID?` `RPRT?` `QSL TU` `TU`
`QRZ?` `QSO B4` `WAIT` `NIL?` `OK?`; GRID4 AA00–RR99), 101/110 reserved, 111 an invalid guard. role 1 renders a "599 "
prefix. Structured atoms render with one implicit separating space; TEXT5 verbatim.

**Automatic packing** (the sender's job — we implement it for our test transmitter): the fewest frames that reproduce the
upper-cased, space-normalised text exactly; e.g. `CQ K1ABC CQ` 1 frame, `WB9XYZ 599 123` 2, `599 BRUCE` 2, `1D EMA` 1. The
contest function keys (F1 `CQ %M CQ` … F8 `%E`) and an N1MM `[[JTTY:ACTION]]` tag compile to typed atoms.

### Calling frequencies — PRELIMINARY

Announced in news coverage of the release candidates (cq.sk, 2026-10-06; dxzone "JTTY Frequencies"), "may change", not
band-plan allocations — USB dial, MHz: 1.838 · 3.575 · **7.090** · 10.140 · **14.090** (the main meeting point) · 18.100 ·
21.090 · 24.920 · 28.090 · 50.160 · 144.160. **Observed:** PSKReporter's JTTY reports this afternoon were at 7.0914–7.0915
and 14.0915–14.0917 MHz — i.e. dials 7.090 / 14.090 with the audio around 1.4–1.7 kHz, consistent with that list.

### Not confirmed / not known

- No paper and no protocol number (no QEX article yet). The user guide's protocol section says "more to be added".
- The rc grammar "has no version discriminator" (families 101/110 are reserved for that); a future protocol revision could
  change what valid atoms are. Our grammar is the v3.2.0-rc1 = v3.3.0-beta4 one.
- How WSJT-X measures the SNR it reports (we use the WSJT convention, signal over noise in 2500 Hz, from the sync).
- The "60 WPM" figure is the articles' description, not something the spec states.

## 2. The receiver — our design

1. **One spectrogram for the whole passband**: a 768-point real FFT (PFFFT: NEON / SSE) of a one-symbol window every
   eighth of a symbol (48 samples at 12 kHz). Bins are half a tone (15.625 Hz), so at the right alignment every tone sits
   on a bin centre and leaks nothing into the others. Ten stations cost what an empty band costs.
2. **A sync score for every cell** (eighth-symbol × half-tone): 3·(power on the 13 sync tones) ÷ (power on the other
   three tones at the same instants). Self-normalising — noise ≈ 1.03 (measured), a carrier or RTTY ≈ 1–1.3, a JTTY frame
   ≈ 1 + Es/N0. Vector loops over frequency (NEON / SSE2 / scalar). A local maximum over ±½ symbol and ±1 tone above
   **2.6** is a candidate.
3. **Fine stage** per candidate: mixed to 0 Hz, a 97-tap Blackman low-pass, decimated to 750 Hz (SIMD dot products);
   searched on the sync every 1/24 symbol and 1.5 Hz, then on all 59 symbols (decision-directed) every 0.5 Hz; a best at
   the edge of the search is searched again centred there (a sync sidelobe can win a coarse cell — found on a −12 dB miss).
4. **Soft decoding**: each symbol's four tone correlations → ln I₀(2A|z|/N), the exact noncoherent likelihood →
   **tail-biting list Viterbi** (two circular passes, every end state traced back, the paths that close on themselves,
   best first) → the first 4 through CRC-12 + reserved bit + the WHOLE grammar.
5. **A coherent second pass** when none passes. h = 1 makes the transmitter phase-continuous: every symbol advances the
   phase by 2π(k − 1.5) ≡ π about the centre whatever the tone, so (−1)ʲ·z on the sent tone has ONE phase all frame long.
   The best path's tones (mostly right even when its CRC fails) give the carrier phase symbol by symbol (±8 symbols, the
   symbol itself left out) and the coherent likelihood 2A·Re(z·e^−jψ)/N is decoded again. Worth about +1 dB at the
   threshold (−16 dB: 4 → 11 of 40 when it was added; −17 dB: 0 → 3).
6. **THE FRAME MUST BE CONSISTENT WITH ITSELF.** A 12-bit CRC tried on ~25 candidates a second lets noise through every
   few minutes, whatever anyone's grammar check does — measured: 1 false frame in 10 min of noise with the CRC and grammar
   alone. So, measured gates (ten hours of noise, ~850 000 candidates):
   - **coherence** of the decoded tones — blocks of 8 symbols summed with their phases ÷ the same powers summed without.
     Noise (and a codeword that is not really there) ≈ 1 whatever the decoder chose; a frame ≈ (1 + 8·Es/N0)/(1 + Es/N0),
     5–8. CRC-passing noise reached 2.8 (noncoherent pass) and 4.1 (coherent pass, whose phases are partly self-made):
     gates **3.2** and **4.5**, plus ≥ 24 of 46 strongest-tone agreements on the coherent pass (noise ≤ 22, frames ≥ 26);
   - **a strong sync must be matched by its data**: real FT8 lit a JTTY sync at a ratio of 64 and a path passed the CRC
     with only 24 of 46 tones agreeing — a frame that strong would agree on ~46. Above Es/N0 = 6 the agreement must reach
     what half that Es/N0 predicts for 4-FSK (3σ, capped at 38).
7. **Continuation**: a frame says where the next frame of its transmission is (59 symbols on, same frequency) — that
   position is decoded directly, even if its sync never crossed the threshold (7 of the 20 real 40 m frames came this way).
8. **Messages and signals**: frames on one frequency in sequence are one message, "… " where frames were lost, closed by
   the EOM frame (or two lost frames' time). A signal row per station; call atoms are spots (CQ / TU…CQ → "CQ", else "QSO").

## 3. Measured (vibeserver/test-jtty.cpp; scripts/test-jtty.sh)

One frame at an arbitrary instant and frequency (300–2700 Hz) in white noise, 40 trials per SNR (2500 Hz):

| SNR | −8 | −12 | −14 | −15 | −16 | −17 | −18 | −19 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| decoded | 40 | 40 | 36 | 25 | 15 | 7 | 1 | 0 |
| wrong | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

50 % at about −15.3 dB for a single frame; multi-frame messages gain from continuation (a 7-frame message at −17 dB: 12 →
16 frames recovered of 70). Six stations at once (overlapping, multi-frame, two on one frequency in turn): 7/7 messages
whole, nothing else.

False frames: **0** in 10 h of white noise (855 250 candidates), 0 in 60 min (the suite), 0 in 10 min of strong RTTY
(20 559 candidates), 0 in 52 min of real FT8/FT4 from the UberSDR (90 803 candidates).

Real: the WSJT-X sample (`samples/JTTY/260807_134110.wav`) → "RAN ALL NIGHT ON BAND NOISE - NO FALSE DECODES!" (10/10
frames). Off air, 2 × 10 minutes on 40 m and 20 m (16:54 and 17:06 UTC, Monday), every decode checked against
PSKReporter's JTTY reports for the same window (`~/VibeSDR-test-audio/jtty/pskreporter_*.xml`):

| Window | Decoded here | PSKReporter senders in the window | Not decoded — why |
| --- | --- | --- | --- |
| 40 m 16:54 | DK7OG (30-frame CQ, 20 frames, 7 by continuation), F4ELU/QRP ×2 | DK7OG, F4ELU/QRP, M3KXZ | M3KXZ: no JTTY energy in our audio |
| 20 m 16:54 | RN4AQ — one TEXT5 frame "4AQ L" at 1088 Hz (RN4AQ reported at 14091088) | RN4AQ, S59V, M3KXZ, K8DLD | S59V: nothing at 2151 Hz here; M3KXZ, K8DLD: only heard in the US |
| 20 m 17:06 | N4YSA: "YSA FM08", "DL5AXI DE N4 …" (N4YSA reported at 14091014) | N4YSA, DL5AXI, F1ABL, G1DLC, K8DLD | DL5AXI −18 dB even in Scotland; F1ABL −10 dB in Ireland; the others not to the UK |
| 40 m 17:06 | — | HB9DBK (−12/−14 dB in Ireland/Scotland) | too weak here |
| 15 m 16:54 | — | — | — |

Every frame we showed was a real JTTY transmission (frequency within a few hertz of the sender's report) — no false
frame on the air either. N4YSA's frames were −12 to −16 dB here, so a frame or two of each over was lost; the "…" marks
where. One thing seen: a single loud impulse on one sync symbol gives a sync score of 11 — harmless (the CRC and the
gates throw it out) but it costs a candidate; a per-symbol cap on the score is an easy saving for impulsive sites.

**CPU** (12 kHz audio, M5 Pro, one core): a busy passband (six stations, −10 dB) **0.74 %** NEON / 1.22 % scalar; white
noise 0.47 % / 0.76 % (NEON 1.6× the scalar). Pi 2 (900 MHz Cortex-A7) **estimated, not measured**: ~15–25× slower per
core → roughly 8–18 % of one core. SSE2 builds and is syntax-checked for x86 (`-target x86_64`); not timed here.
**RAM** per decoder ≈ 1 MB: the spectrogram 512 rows × ~212 bins (≈ 430 KB — one frame and a little, which is what lets a
frame be found at any instant), the 5.5 s audio ring (256 KB), the fine stage's buffers (≈ 190 KB) and tone references
(50 KB). A smaller ring cannot hold a frame plus its search margin; fewer rows would mean a coarser time grid (more loss).

## 4. What a listener sees (web)

DECODERS → **JTTY** (where vibeserver.json says `"jtty": true`). The settings: band keys for the announced calling
frequencies (USB), only those the radio reaches, and how it works. The box: one row per station, **lowest frequency
first**, frequency · call · SNR · confidence · last heard, then its recent messages coloured by confidence (#5BE36B high,
#E8C547 medium, #FF8A3D low, #FF4B4B poor); **SPOTS** below (newest first, CQ / QSO, country). Tap a row or a spot: USB,
the dial 1500 Hz below the station. The view keeps its place while it updates (pinned to SPOTS, as CW's). A line per
finished message also goes out as decoder text (`14:32:07z  14.0915  CQ G4ABC CQ`), so a client that shows decoder text
shows JTTY without knowing it.

## 5. Owed

- **The app** (React Native) — BUILT 2026-10-26 (APP-PARITY-2026-10-26.md): JttyView in the decoder box, the band keys
  the radio reaches, tap = USB 1500 Hz below; `src/services/jtty.ts` held to the web's by scripts/test_psk_jtty_dsc_app.ts.
- **Background decoding and the gallery**: not wired (a JTTY row in `vibe_bgdecode_config.h` and a summary tap like CW's).
- **A JTTY skimmer** on IQ (a whole 24–96 kHz segment, as CW's): the engine takes any rate change of one line — its
  frequency search is already passband-wide; the host's IQ path would need a 12 kHz real-or-complex front end.
- **Subtraction** of decoded frames (to dig a weaker overlapping station out): not done; overlapping stations on one
  frequency are rare in practice and both decode when their syncs are apart.
- **Transmit**: none — VibeSDR is receive-only.
