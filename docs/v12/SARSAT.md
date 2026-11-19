# COSPAS-SARSAT 406 MHz distress beacons (V12)

Branch `v12-vorsar`, 2026-11-19. A listener tunes 406.0–406.1 MHz and presses **406 BEACONS**: VibeServer finds each burst of
a first-generation beacon (EPIRB, ELT, PLB), corrects it with its own BCH codes and says what it is. ⚖ **Everyone** on the
decoder sees THAT a beacon was heard — its kind, country, protocol, and SELF-TEST / TEST where it is one; **only the owner's
signed-in sockets** see what identifies it (15 Hex ID, MMSI / call sign / registration / serial / aircraft address) and the
**encoded position**. Every view says: *real alerts are received by the satellites and handled by the rescue authorities —
VibeSDR reports nothing to anyone.*

**Nothing has been decoded off air by us.** Everything is checked against the specification's own worked examples and our own
transmitter (`decoders/sarsat_tx.h`).

## RESUME HERE

1. **A real burst.** Real 406 MHz bursts near the ground are rare: an activated beacon is an emergency, and self-tests are
   single short bursts. Realistic ways, best first:
   - **A PLB / EPIRB owner's SELF-TEST**, by arrangement (club member, a chandler): the self-test is designed to be radiated
     (frame sync 011010000 — we label it SELF-TEST; most beacons radiate one burst, some at reduced power). Tune **406.033
     MHz NFM** (one dial covers 406.022–406.044 — every channel in use) on the Pi 500's VHF/UHF RTL within a few hundred metres
     and record 60 s of IQ around the press. The owner's line gives the Hex ID to compare with the label on the beacon.
   - **Leave it running** on 406.033 near a marina/airfield for days (background decoding is not built for it — a listener's
     decoder only) — expect a few self-tests a week at best.
   - ✗ Never transmit a test beacon ourselves (a licensed beacon test needs the authority's permission and a dummy load).
2. Not built: the R36S view; a gallery copy (deliberately none); second-generation beacons (C/S T.018, spread spectrum).

## Where it is offered

- **A listener's decoder** (`extension_name: "sarsat"`), no parameters. **Reach** 406.0–406.1 MHz; an **Icom greyed** (no
  IQ); not on VFO B. **Owner switch** "406 MHz distress beacons (COSPAS-SARSAT)". **Benchmark row `dec_sarsat`** (a long
  standard-location self-test every 5 s, 48 kHz): **0.10 %** of an M5 Pro core (0.23 % in `test-sarsat`'s cost case after the
  search was widened to ±10.5 kHz), decodes.
- **Fed** the listener's slice (per-listener radios) or the one pipeline's channel — the same path as VOR (docs/v12/VOR-ILS.md
  "How it is fed"); ★ on the one pipeline the channel is the listener's NFM width (12.5 kHz → ±6 kHz: 406.028–406.037 from
  406.033) unless they widen it.
- **Clients:** **406 BEACONS** under **Maritime on the UHF tab**, the hint (with the owner-only rule and the authorities
  note); the web box shows the note and the log; status "3 bursts heard (1 test) · last: EPIRB · United Kingdom (232)".

## The receiver (`decoders/sarsat_rx.h`)

From **C/S T.001 "Specification for Cospas-Sarsat 406 MHz Distress Beacons", Issue 4 Rev. 7 (March 2021)** — §2.2–2.3 (160 ms
of carrier, then 400 bit/s ±1 % biphase-L, phase modulation ±1.1 ± 0.1 rad, 150 µs edges), §3 and Annexes A and B (read in
full from a public copy; the official library is cospas-sarsat.int).

1. Decimate to ≈ 24 kHz (SIMD dot products, scalar twin bit for bit); a 3 s ring.
2. **Find a burst:** a 2048-point Hann FFT every ~21 ms over ±10.5 kHz; a carrier LINE ≥ 11 dB over the bins' 75th
   percentile (a median among the empty outer bins of a narrower channel made every noise bin a line — found end to end) at a
   frequency with no line before. A line lasting > 1.2 s is a steady carrier and never decoded.
3. **Work the burst as a block** (0.85 s later): the FFT frequency, refined by the slope of the carrier's phase across the
   burst. ★ **No loop:** biphase PM keeps cos(1.1) of the carrier in every bit (the two halves cancel the ±sin part), so a
   one-bit (then, frequency refined, eight-bit) moving average of the signal is the phase reference through the whole message.
   y = Im(z · conj(ref)) / |ref|.
4. **Timing:** soft bits (first half − second half, prefix sums) — the 24 sync bits (15 ones + 000101111 / the self-test's
   011010000, either sense) by **cosine similarity ≥ 0.8** at every sample of the block (a window that only grazes the
   modulation scores √(k/24)); then the bit rate 396–404 in 0.5 steps and ±2 samples by the message's soft energy.
5. **BCH** (`sarsat_msg.h`): BCH-1 (82,61) t = 3, g = 1001101101100111100011; BCH-2 (38,26) t = 2, g = 1010100111001
   (Annex B) — decoded exactly by syndrome search over the positions (no Galois-field tables); a Chase retry on the 4 weakest
   bits, accepted only when every decoding combination agrees.
6. **Accepted only when:** ≤ 1 sync error; BCH-1 ≤ 3 corrected (never 3 on a short message — the radius-3 sphere is 4 % of
   all words); a long message's BCH-2 also corrects (or BCH-1 needed ≤ 1); the country is an ITU MID (201–775); and the
   carrier has gone 120–300 ms after the message (noise-floor-corrected power < ¼ of the message's).

**The protocols** (`parse`, Annex A): user (maritime MMSI / call sign, radio call sign, aviation registration, serial — ELT /
float-free / non-float-free EPIRB / PLB / 24-bit address / operator designator, type-approval number, test, orbitography,
national) and user-location (4 min); standard location (EPIRB-MMSI, ELT 24-bit address, serial ELT / operator / EPIRB / PLB,
ship security, standard test — ¼° + 4 s offsets); national location (2 min + 4 s); RLS location (incl. its MMSI form and test
coding); ELT(DT) (activation, altitude band, the 3LD rotating field). Default (no-fix) position patterns are recognised;
offsets applied in the coarse value's direction (A3.3.1). The 15 Hex ID = bits 26–85 with the position defaulted. Emergency
codes (Table A4 / A5) and activation from the short message's non-protected bits.

## Tests

| Suite | What | Result |
|---|---|---|
| `test-sarsat` (vibeserver/, `-O2`, header-only) | **T.001 Annex B**: B1's BCH-1 001011001010101001001, its Hex ID ADCD0 08004 40401, bits 25–112 56E68 04002 20200 96552 50, serial-user float-free EPIRB with homing; B2's BCH-2 0001 0101 0001 and its 43°32′ N 001°28′ E; BCH-1 1/2/3 errors 1000/1000 each, 4 errors never back to the sent word; BCH-2 1–2 errors 2000/2000, 3 never; 5 protocols round trip with positions; 36 bursts (25 / 48 / 64 kHz, −9…+7.5 kHz off the dial, 396 / 404 bit/s, ±1.0 / 1.2 rad, either sense) all exact; 40 dB-Hz 20/20, 37 dB-Hz 20/20, 34 dB-Hz 18/20 (0 wrong); a self-test labelled; three bursts 5 s apart; **an hour of noise: 0**; a steady, a keyed and unmodulated 520 ms carriers: 0; **200 beacon-shaped bursts of random bits: 0**; a channel cut to ±6.25 kHz (the one pipeline's NFM) 4/4, no false steady line; SIMD = scalar; cost | **28 / 0** |
| `test-server-vorsar.mjs` | shared dial: the attach note; the EPIRB, the ELT and the self-test; **the owner: Hex ID, MMSI 232123456, position 50.71° N 1.34° W, G-ABCD; a listener's mirror: none of it, checked against everything it was sent**; the status JSON carries no identity; per-listener, nobody signed in: public line only | (in the 25 / 0) |
| `test_navaids_app.ts` | the status, the note and owner-only rule in the hint, placement, both clients' keys, both setup screens | (in the 42 / 0) |

## Decisions for Stuart

1. **Identity owner-only, not just position.** The brief said positions; I also kept the Hex ID and the MMSI / registration /
   serial for the owner (a beacon's message is not a broadcast — WTA s.48). Listeners see kind + country + test. OK?
2. **Placement:** UHF **Maritime** (most are EPIRBs; ELTs and PLBs too). Or a UHF "Distress" group?
3. **Default when the benchmark is below green:** off with the figure (the listener-decoder rule). It costs 0.1–0.2 % of an
   M5 core.

## Sources

C/S T.001 Issue 4 Rev. 7 (March 2021) — read from the public copy the Thai SAR authority hosts
(sar.mot.go.th/document/THMCC/…); its Annex B worked examples are the test vectors. Country names: our own MID table
(`vibe_maritime_mid.h`). No other decoder's source was read.
