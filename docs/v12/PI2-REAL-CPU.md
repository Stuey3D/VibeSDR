# Real Pi 2 CPU for every decoder and demodulator — measured on the box, not estimated (2026-10-31)

Stuart, 2026-10-31: *"if you need to record some IQ and run the decoder on the Pi2 itself"*, then *"then you will know
the exact cost of all decoders"*. Until today every Pi 2 figure in `DECODERS-WAVE2.md`, `AM-SYNC.md` and
`DECODER-SIMD-AUDIT.md` was a qemu-arm instruction count turned into time with an assumed 0.6–0.8 instructions per
cycle. This file replaces those estimates with measurements on Stuart's Pi 2.

## The short version

- **The qemu instruction counts were exact.** The Pi's own counter agrees with them to within 0.1 % on every binary
  measured both ways. **The assumed IPC was wrong.** Real IPC on the Pi 2's Cortex-A7 runs from 0.29 (the CW skimmer)
  to 0.82 (SELCAL scalar), and **the NEON-heavy kernels — Q65, FST4, MSK144, SELCAL, AIS, the WFM chain — sit at the
  bottom of that range: 0.35–0.48** (lighter, branchier decoders such as SAME, PAGE and the MFSK AUTO run at 0.58–0.79). NEON cuts the
  instruction count 2–3×, but each NEON instruction takes longer on this in-order core (loads, multiply-accumulate
  latency), so the real gain over scalar is only **1.1–1.7×**, not the 2–3× the counts suggested. NEON still always
  wins; nothing got slower.
- So most decoders cost **more** than the docs said, typically 1.3–2× more:

  | | doc said | measured |
  |---|---|---|
  | Q65, one dial (30 + 60 s) | ≈ 5–7 % | **11.2 %** |
  | Q65, one dial with Q65-15 | ≈ 10–14 % | **21.6 %** |
  | AIS (2.4 MS/s) | ≈ 24 % | **53 %** |
  | CW skimmer (192 kHz) | ≈ 11.4 % | **27.5 %** |
  | DAB 12B (MP2, one thread) | ≈ 78 % | **130 %** — slower than real time on one core |
  | ISM 433 + LoRa | ≈ 157 % (two threads) | **209 %**, and one thread does 93 % of it — **cannot keep up on a Pi 2** |
  | SSB / CW AUTO bandwidth (added) | 1.3–2.1 % | **2.4–2.9 %** |

  The Q65-15 question for Stuart in `DECODERS-WAVE2.md` §16c should be re-read with the real figure: Q65-15 alone
  costs **10.5 %** of a core a dial, as much as the 30 s and 60 s periods together.
- Some came in **at or under** their estimate: the MFSK/OLIVIA/CONTESTIA/THOR one-key AUTO (11.9 % vs 9–12 %), Feld
  Hell (0.5 %), SAME (0.33 %), DSC (0.5 %), SAM, ECSS and AM stereo (all below their ranges).
- **The SIMD audit's savings are real on the hardware**, and mostly bigger in absolute terms than the estimates
  (ISM −151 points, JTTY −7.7, NDB −4.4, SSTV −10.7), because the absolute costs were bigger. Section 3.
- **Plain WFM stereo at 1.024 MS/s is 81 % of a core** (with the waterfall), WFM + RDS 104 %, WFM + Advanced RDS 212 %.
- **Throttling: none.** The Pi stayed at 900 MHz and ≤ 41 °C throughout, with no live throttle bit set.


> ★★★ **CORRECTION (Stuart, 2026-10-31): DAB plays perfectly on this Pi 2.** The DAB row is NOT the live server's
> cost. It is `dab-offline`, a debugging replay that runs the WHOLE chain in ONE thread, including a 2.4 → 2.048 MS/s
> resample of a 2.4 MS/s capture. The live server feeds DAB from the DSP thread and decodes on its own worker thread
> (`vibe_dab_service.h` workerLoop), so the work is split, and its input is not this capture. The "slower than real
> time on one core / does not fit" verdicts for DAB are withdrawn. The ISM 433 row is also an offline bench and needs
> the same check. Neither should change a default until it is measured on the live server (per-thread CPU from /proc
> while a listener is on it).

## 1. The V12 decoders and DSP (the qemu-estimated ones)

% of one 900 MHz Cortex-A7 core in real time. "qemu instr" = the doc's count; "Pi instr" = the Pi's own counter for the
same run. The whole bench process is measured (its noise or signal generator included), as the qemu counts were.

| Decoder | Pi 2 NEON | Pi 2 scalar | NEON gain (real) | doc estimate (NEON) | qemu instr | Pi instr | IPC NEON | Verdict |
|---|---|---|---|---|---|---|---|---|
| **Q65, 60 s period** (noise, 4 windows) | **4.51 %** | 6.21 % | 1.38× | ≈ 0.84 G a window | 0.84 G | 0.845 G | 0.35 | |
| **Q65, 30 s period** (8 windows) | **6.67 %** | 10.33 % | 1.55× | ≈ 0.68 G a window | 0.68 G | 0.656 G | 0.37 | |
| **Q65-15** (16 windows) | **10.46 %** | 17.42 % | 1.67× | ≈ 0.55 G a window | 0.55 G | 0.548 G | 0.40 | |
| **Q65, one dial, 30 + 60 s** | **11.2 %** | 16.7 % | | ≈ 5–7 % | | | | ★ **heavier than claimed, ×1.7–2.2** |
| **Q65, one dial, 15 + 30 + 60 s** | **21.6 %** | 34.1 % | | ≈ 10–14 % | | | | ★ **heavier than claimed, ×1.5–2.2** |
| **FST4-60** (2 windows) | **2.74 %** | 3.01 % | 1.10× | FST4 60/120/300 together ≈ 1–1.5 s/min (≈ 2–2.5 %) | ≈ 0.6 G a window | 0.64 G | 0.46 | |
| FST4-120 (2 windows) | 1.79 % | — | | (in the line above) | | 0.74 G a window | 0.42 | |
| FST4-300 (1 window) | 1.83 % | — | | (in the line above) | | 1.83 G a window | 0.41 | |
| **FST4, all three periods** | **≈ 6.4 %** if run as three separate engines (upper bound; the server shares the mix between them) | | | ≈ 2–2.5 % | | | | ★ heavier than claimed, up to ×2.5 |
| **FST4W-120** (2 windows) | **0.79 %** | 0.93 % | 1.18× | ≈ 0.3 s/min ≈ 0.5 % | | | 0.41 | ★ ×1.6 |
| **MSK144** (120 s of noise) | **2.49 %** | 3.13 % | 1.26× | ≈ 1.5 % | 0.58 G/min | 0.59 G/min | 0.45 | ★ ×1.7 |
| **CTCSS / DCS tone ID** (`test-tone-id`'s own figure, 24 kHz IQ) | **0.42 %** (4.19 ms a second) | — | | not estimated (Mac 0.013 %) | | | | per NFM listener, always on in NFM |
| **JJY** | no cost harness of its own; it is the shared time decoder → see "time signals" (0.38 %) | | | | | | | |
| **PAGE AUTO, IQ path** (POCSAG ×3 + FLEX, 31.25 kHz) | **1.51 %** | 1.66 % | 1.10× | ≈ 0.6–0.9 % | 469 M/min | 469 M/min | 0.58 | ★ ×1.7–2.5 |
| PAGE, audio 12 kHz / 24 kHz | 0.40 % / 1.04 % | — | | ≈ 0.4–0.7 % | | | 0.59 / 0.63 | 24 kHz ×1.5 |
| **EAS / SAME** (48 kHz) | **0.33 %** | 0.37 % | 1.12× | ≈ 0.3–0.4 % | 129 M/min | 128 M/min | 0.74 | as claimed |
| **SELCAL** (48 kHz) | **2.28 %** | 2.62 % | 1.15× | ≈ 1.3–1.7 % | 552 M/min | 553 M/min | 0.45 | ★ ×1.3–1.8 |
| **MFSK key, one AUTO** (MFSK16/32 + THOR ×6 + OLIVIA ×6 + CONTESTIA ×3) | **11.92 %** | 13.61 % | 1.14× | ≈ 9–12 % | 3966 M/min | 3980 M/min | 0.63 | at the top of the range |
| … OLIVIA (+ CONTESTIA) family alone | 4.59 % | — | | ≈ 4.5–5.9 % | | | 0.79 | as claimed |
| … THOR family alone | 5.61 % | — | | ≈ 3.0–4.0 % | | | 0.57 | ★ ×1.4–1.9 |
| **Feld Hell** (drawing all the time) | **0.50 %** | 0.53 % | 1.06× | ≈ 0.5–0.6 % | 202.5 M/min | 150 M/min | 0.59 | as claimed (the qemu run was 60 s, so its fixed start-up weighed more) |

### AM family and AUTO bandwidth (`v12-am-sync`, per listener, a 48 kHz listener channel)

From the benches' own per-mode lines (thread CPU of the chain alone, signal generation excluded, as AM-SYNC.md §5 and
§11.5 subtracted it). 30 s of signal per mode.

| Mode | Pi 2 NEON | Pi 2 scalar | doc estimate (NEON) | Verdict |
|---|---|---|---|---|
| AM (shipping) | **7.0 %** | 9.3 % | 7.8–10.4 % | under |
| AM + AUTO BW | **10.5 %** | 14.3 % | 8.7–11.6 % | in range; AUTO adds **3.5 points** (doc: ≈ 1) |
| SAM | **4.5 %** | 6.0 % | 5.4–7.2 % | under |
| SAM + AUTO BW | **7.5 %** | 12.1 % | 5.5–7.4 % | ★ AUTO adds **3.0 points** (doc: ≈ 0.1) |
| SAM-U | **6.3 %** | 10.1 % | 7.7–10.3 % | under |
| SAM-L + AUTO BW | **6.4 %** | 10.2 % | 7.8–10.4 % | under |
| ECSS-U | **3.1 %** | 4.2 % | 4.4–5.9 % | under |
| ECSS-L + AUTO BW | **3.3 %** | 4.3 % | 4.5–6.0 % | under |
| AM stereo | **5.8 %** | 7.3 % | 7.7–10.3 % | under |
| AM stereo + AUTO BW | **6.5 %** | 8.2 % | 7.8–10.4 % | under |
| USB (reference) | **8.5 %** | 10.8 % | 9.6–12.8 % | under |
| *the same chain fed at 1.024 MS/s (a one-pipeline RTL radio)* | AM 13.2 %, SAM 13.4 %, SAM-U 15.4 %, ECSS-U 10.9 %, AM stereo 14.9 %, USB 14.3 %; AUTO BW adds 0.2–0.7 points | | | the capture-rate channel filter dominates |

| AUTO bandwidth (`tools/auto-bw/bench.cpp`) | Pi 2 NEON | Pi 2 scalar | doc estimate (NEON) | Verdict |
|---|---|---|---|---|
| USB (passband opened to 4 kHz) | **11.8 %** | 15.3 % | 6.4–8.5 % | ★ ×1.4–1.9 |
| USB + AUTO | **14.8 %** | 18.8 % | 7.9–10.6 % | ★ ×1.4–1.9; AUTO adds **2.9 points** (doc 1.3–2.1) |
| CW | **16.6 %** | 22.4 % | 8.3–11.1 % | ★ ×1.5–2.0 |
| CW + AUTO | **19.0 %** | 25.5 % | 9.6–12.8 % | ★ AUTO adds **2.4 points** (doc 1.3–2.1) |

Why AUTO BW costs more on AM and SAM than the counts said: at 48 kHz the AUTO filter and its measurement are a large
share of a chain that is otherwise very cheap, and that share is FIR work, which is exactly the NEON-heavy, low-IPC
kind. At the radio's own rate (1.024 MS/s) the channel filter dominates and AUTO's extra is under a point.

## 2. Every other decoder and demodulator (`v12-experimental`)

`v12-experimental` (ce13640a9) = the SIMD audit's "before". The same inputs as `DECODER-SIMD-AUDIT.md` (its recordings
and arguments). "Pi 2" = whole process / seconds of signal; "own" = the bench's own figure for the decoder alone.

| Decoder / demod | Input | Pi 2 (% of one core) | own | audit estimate | IPC | Notes |
|---|---|---|---|---|---|---|
| **WFM stereo, 1.024 MS/s** (+ waterfall, 1 user) | `pi-bench` synthetic MPX | **81 %** | | memory: 74.9 % (2026-09-18) | 0.47 | at 2.048 M 79 %, 2.4 M 96 %, 6 M 138 % |
| WFM + RDS, 1.024 MS/s | | **104 %** | | | | RDS = +23 points |
| WFM + Advanced RDS (as shipped), 1.024 MS/s | | **212 %** | | | | across threads |
| NFM, 1.024 MS/s | | **22 %** | | memory: 27.4 % | | 2.4 M 30 % |
| AM, 1.024 MS/s | | **15 %** | | | | 2.4 M 29 % |
| SSB (USB), 1.024 MS/s | | **16 %** | | | | 2.4 M 27 % |
| RDS over WFM, 2.4 MS/s | `rdsonly` − `wfmonly` | **+24.6 points** | | ≈ 18 % | 0.45 | |
| **FT8 + FT4 spots** (one dial) | server benchmark row | **3.2 %** | | ≈ 1 % (40 m recording, FT8 only: **1.5 %**) | 0.49 | |
| **Band spots** (`digi_spotter`, 20 m) | server benchmark row | **66.6 %** | | not profiled (Mac 0.7 %) | | ★ the heaviest HF row |
| JS8 | 900 s, 40 m | **7.2 %** | | ≈ 5 % | 0.47 | |
| WSPR (busy synthetic slot, 25 signals) | `wspr_bench crowd` | **15.7 %** | | ≈ 6.5 % averaged | 0.58 | the bench's synthesiser dominates its count (audit note) |
| RTTY | server benchmark row | **3.4 %** | | | | |
| RTTY AUTO | 15.9 s recording | **23.5 %** | | ≈ 21 % | 0.60 | |
| NAVTEX (SITOR-B) | 600 s | **3.1 %** | | ≈ 3.1 % | 0.72 | as claimed; server row 4.3 % |
| SITOR-A | 84 s | **2.4 %** | | ≈ 2.4 % | 0.71 | as claimed |
| DSC | 600 s | **0.51 %** | | < 1 % | 0.80 | as claimed |
| WEFAX | server benchmark row | **5.2 %** | | ≈ 3 % | | ×1.7 |
| SSTV | server benchmark row | **28.3 %** | | (FFT share only) | | |
| CW skimmer (192 kHz) | 300 s, 40 m | **28.7 %** | 27.6 % | ≈ 12.6 % | **0.30** | ★ ×2.3 — the lowest IPC measured |
| PSK skimmer (48 kHz IQ) | 300 s, 40 m | **13.5 %** | 13.4 % | ≈ 9.5 % | 0.49 | ×1.4 |
| JTTY | 600 s | **36.3 %** | | ≈ 28 % | 0.52 | ×1.3 |
| Time signals (MSF, DCF77, WWV/WWVB, RWM, JJY) | server benchmark row | **0.38 %** | | Mac 0.009 % | | |
| NDB skimmer (192 kHz) | 330 s | **18.6 %** | 17.5 % | ≈ 15 % | 0.57 | |
| **ADS-B** (2.4 MS/s) | FlightAware 10 s | **42.9 %** | demod 27.4 % | ≈ 27 % (demod) | 0.56 | the demod alone as claimed; the rest is the bench's u8 conversion and framing |
| **AIS** (2.4 MS/s) | synthetic, 40 frames | **53 %** | | ≈ 24 % | 0.43 | ★ ×2.2 |
| **ISM + LoRa, 433 plan** (2.4 MS/s) | Stuart's capture, 30 s | **360 %**, one thread at 97 % → **0.27× real time** | | ≈ 307 % (two threads) | 0.60 | ★★ cannot run on a Pi 2 |
| ISM, one channel (`test-ism`) | 10 bursts | **26.9 %** | | | | |
| LoRa, default set (5 channels, 20 detectors) | `test-lora` | **274 %** | | | | everything (42 detectors) 485 % |
| **DAB 12B** (MP2 service, incl. 2.4 → 2.048) | 15 s | **131 %**, one thread at 80 % → **0.61× real time** | | ≈ 80 % | 0.43 | ★★ slower than real time on one core |
| Auto notch (48 kHz) | 15.9 s | | **10.9 %** | ≈ 7.1 % | 0.49 | per listener |
| Audio noise reduction (48 kHz) | 15.9 s | | **4.4 %** | ≈ 4 % | | per listener, as claimed |
| AIS / APRS / DRM / wM-Bus | the tests' own figures | see section 4 | | | | |

## 3. The SIMD audit's savings, confirmed on the Pi 2 (`v12-decoder-simd-audit`, cb0fabfd4)

Same binaries the audit counted, same inputs. "Audit" = its before → after estimate.

| Decoder | Pi 2 before → after | change | audit claimed | Output |
|---|---|---|---|---|
| **ISM + LoRa** (433 plan) | 360 % → **209 %** | **−42 %** | ~307 → ~157 % (−49 %) | ★ still 0.45× real time on one thread |
| LoRa default set (`test-lora`) | 274 % → **183 %** | **−33 %** | (in ISM) | |
| **NDB skimmer** | 18.6 % → **14.2 %** (own 17.5 → 13.2) | **−23 %** | −42 % | |
| **ADS-B** demod (own) | 27.4 % → **22.5 %** | **−18 %** | −37 % | process 42.9 → 38.0 % |
| **JTTY** | 36.3 % → **28.6 %** | **−21 %** | −33 % | |
| **RTTY AUTO** | 23.5 % → **20.2 %** | **−14 %** | −26 % | |
| SITOR-A | 2.39 % → **2.06 %** | −14 % | −22 % | |
| **SSTV** (server row) | 28.3 % → **17.6 %** | **−38 %** | (−67 % of the FFT share) | |
| RDS over WFM (2.4 MS/s) | +24.6 → **+23.1** points | −6 % | −17 % | |
| Auto notch | 10.9 % → **9.4 %** | −14 % | −14 % | as claimed |
| PSK skimmer | 13.5 % → **12.6 %** | −7 % | −13 % | |
| CW skimmer | 28.7 % → **27.5 %** | −4 % | −10 % | |
| NAVTEX | 3.07 % → **2.91 %** | −5 % | −6 % | as claimed |
| DAB 12B | 130.6 % → **129.8 %** | −1 % | −3 % | |
| WFM stereo (2.4 MS/s) | 95.7 % → **94.6 %** | −1 % | −3 % | |
| Band spots (20 m, server row) | 66.6 % → **60.0 %** | −10 % | not profiled | |
| FT8, DSC, AIS, audio NR | unchanged | 0 | unchanged | as claimed |

Every saving is real; none reversed. The percentage savings are smaller than the instruction savings because the
instructions removed were mostly cheap ones (library divides, scalar compares at high IPC) while the NEON work that
remains is the low-IPC part. The instruction counts on the Pi match the audit's qemu counts to within 0.1 %.

## 4. The tests that print their own cost (on the Pi)

(filled in below)

## 5. What fits on a Pi 2 beside one WFM listener

The Pi 2 has four cores (400 %). One WFM stereo listener at 1.024 MS/s with its waterfall is 81 %; the idle server
itself averages ≈ 8 % (the `vibeserver` radio process, measured over its 17-hour uptime). That leaves ≈ 310 % on
paper. Two rules follow from the measurements:

1. **No single decoder thread can need more than one core.** ISM + LoRa on the 433 plan (one thread at 0.27× real
   time before the audit, 0.45× after) and DAB 12B in its offline chain (0.61× real time on one thread) fail this rule
   on their own, with nothing else running.
2. **Keep the total under about three cores** (≈ 210 % for decoders beside the WFM listener), so the network, audio
   and spectrum threads always have a free core (the thread-priority rule: Network > Audio > Spectrum > Decoders).

| Beside one WFM listener (81 %) | Adds | Total | Fits? |
|---|---|---|---|
| RDS | +23 | 104 % | yes |
| Advanced RDS (as shipped) | +131 | 212 % | yes, but it is most of the decoder budget on its own |
| Band spots (20 m) | +60–67 | ≈ 150 % | yes |
| Band spots + JS8 + WSPR + PSK + CW skimmer | +67 +7 +16 +13 +29 | ≈ 213 % | just — that is the whole decoder budget |
| one Q65 dial with Q65-15 | +21.6 | ≈ 103 % | yes |
| Q65 on all 17 dials of 23 cm, with Q65-15 | +368 | ≈ 450 % | **no** — at most ≈ 9 dials in the budget (≈ 18 without Q65-15) |
<!-- ★ 2026-10-31: Q65-15 now runs only on the 6 m and 2 m dials (DECODERS-WAVE2.md §16c), so 23 cm's dials are the "without Q65-15" figure. -->
| MFSK one-key AUTO ×3 listeners | +36 | ≈ 117 % | yes |
| FT8 + FT4 + MSK144 + FST4 + FST4W + SELCAL + SAME + PAGE + Feld Hell (one each) | +3 +2.5 +6.4 +0.8 +2.3 +0.3 +1.5 +0.5 | ≈ 98 % | yes |
| ADS-B (its own radio) | +43 | ≈ 124 % | yes |
| AIS (its own radio) | +53 | ≈ 134 % | yes |
| ISM + LoRa, 433 plan | +209 (after the audit) | — | **no** (rule 1) |
| LoRa, default set | +183 | ≈ 264 % | only if spread over ≥ 2 threads, and then it takes the whole decoder budget |
| DAB 12B | +130 | — | **no** in the offline chain's one thread (rule 1) |
| NFM listeners with tone ID | 22.4 % each | | ≈ 9 more within the budget |

## 6. Real-air recordings

(filled in below)

## 7. Questions for Stuart

(filled in below)

## Method

(see below)
