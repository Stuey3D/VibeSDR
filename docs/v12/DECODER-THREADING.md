# Decoders split by channel across threads (V12, branch `v12-decoder-split`, 2026-11-01)

Stuart: *"any other decoders than can benefit from being split up into multiple smaller decoder threads split by
channels should be done if they are heavy on a pi2 core, if its 90% on one core and we can split it over 2 then that
is 45% per core in theory."*

This follows VDL2's design (branch `v12-aviation`, docs/v12/AVIATION.md §Threads): one shared front end, N workers
chosen from the core count, a bounded queue per worker that drops the oldest and counts it, workers below the network,
audio and spectrum threads, and output identical to the single-thread version. A thread is added only when it lowers
the hottest one (each thread costs stack and wakes on a 1 GB Pi 2).

## 1. Audit — the hottest thread of every multi-channel or multi-hypothesis decoder (Pi 2, % of one core)

Sources: the real-Pi-2 per-thread tables in DECODER-SIMD-AUDIT.md (§ISM final) and PI2-REAL-CPU.md; LORA-MESH.md's
benchmark rows; DIGITAL-SPOTS.md's qemu counts (IPC 0.4 used here, so those are estimates).

| Decoder | Channels / hypotheses | Hottest thread today | Over ~50 %? | Candidate split | Done? |
|---|---|---|---|---|---|
| **LoRa "lora+all"** (868, every plan channel) | 13 of 16 channels, 1–6 SFs each | **vibe-lora3 82.8 %** (vibe-lora2 77.2, vibe-lora 68.7); benchmark 86.7 / 85.3 % **red** | **yes** | stages 2 + 3 by channel into lanes; the per-channel slice into the lanes | **yes — below** |
| LoRa default (868) | the default channels | vibe-lora 53.1 (lora2 34.7, lora3 37.6) | just | its hottest is the capture FFT, which does not split by channel | no (1 lane = unchanged) |
| LoRa default (433), 433 every channel | | vibe-lora 47.5 / 60.9 (lora2 53.6) | 433-all only, and it is the FFT thread | as above | no |
| ISM front end (vibe-ism / ism2 / ism3) | burst channels | ≤ 25.1 % (hog-heavy ism3) | no | — | skip |
| AIS A + B | 2 channels | ≈ 53 % of a core for the whole mode (PI2-REAL-CPU.md); the shared front FIR at the capture rate dominates, the two demods are a small part | front only | a split by channel takes the demods off, not the front | skip (note: AIS's front is the target, not its channels) |
| Paging AUTO (POCSAG ×3 + FLEX) | 4 hypotheses | 1.5 % | no | — | skip |
| MFSK / OLIVIA / CONTESTIA / THOR AUTO | 19 hypotheses | 11.9 % | no | — | skip |
| Q65 one dial (15 + 30 + 60 s) | 3 periods | 21.6 % | no | — | skip |
| CW skimmer (192 kHz) | many | 27.5 % | no | — | skip |
| Band "digital spots" (20 m busy) | 7 modes, 7 tuners | **≈ 86 % of a core for the whole Band run** (309 M instr/s, qemu, IPC 0.4 — estimated); JTTY alone ≈ 44 %, the tuners ≈ 21 % | **if it is one thread** | by MODE engine (JTTY / tuners / FT8 family) — its per-thread layout was not measured here | **open** — see RESUME HERE |
| DAB station radio-text scan | one mux | the live server already splits DAB (DSP thread + worker); dab-offline's 130 % is a one-thread replay (PI2-REAL-CPU.md correction) | n/a | — | skip |
| VDL2 (12 channels) | 12 | 95 % → **21 %** (branch `v12-aviation`) | was | channels across workers | yes (other branch) |
| ACARS (3 channels) | 3 | ≈ 30 %, 90 % of it the front end | no | — | skip |

## 2. LoRa lanes (done)

`vibe_lora_service.h` (Engine) and `vibe_lora.h` (Channeliser):

- **Stages 2 and 3 run per LANE.** A lane owns a share of the channels and every detector on them, and has its own
  two threads (`vibe-lora2[.l]` deliver, `vibe-lora3[.l]` the detectors) and its own bounded queues (drop the oldest,
  counted). One lane is exactly the old vibe-lora2 + vibe-lora3, so the default set is unchanged.
- **Lanes from the channels and the cores:** one per 7 channels, at most half the cores (a lane is two threads), at
  most 2; 1 on ≤ 3 cores. lora+all on a Pi 2 → 2 lanes; the default sets → 1. `VIBE_LORA_LANES` forces it (benches).
- **The per-channel slice × filter** (bins × H, the inverse FFT's input) moves out of the FFT thread into the lane
  that owns the channel (with ≥ 2 lanes): `vibe-lora` keeps the transform and one copy of the bins.
- **Balanced, not round-robin:** channels go heaviest first to the lightest lane, weight = bandwidth × (1 + its
  detectors). Round-robin left one lane at 2.8 and the other at 1.6 % (Mac); detector-count weights flipped it.
- **Each channel keeps its own next-block count**, so a lane that drops or lags never puts another lane's NCO wrong.
- **Output identical:** test-lora "threads = inline", 11 frames, field for field, with 1 lane and with 2.

**Measured (Mac, `tools/lora-bench/lora_rows`, built with -DVIBE_DAB_PFFFT; % of one core):**

| lora+all (868) | vibe-lora | lane 1: lora2 / lora3 | lane 2: lora2 / lora3 | hottest |
|---|---|---|---|---|
| 1 lane (before) | 2.7 | 3.9 / 4.6 | — | **4.6** |
| 2 lanes, round-robin | 2.4 | 2.3 / 2.8 | 1.6 / 1.6 | 2.8 |
| 2 lanes, balanced + slices in the lanes | 2.0 | 2.3 / 2.3 | 2.8 / 2.3 | **2.8 (−39 %)** |

**Pi 2, projected** from the real Pi 2 per-thread figures (68.7 / 77.2 / 82.8) by these ratios: vibe-lora ≈ 51 %,
the heavier lane's deliver ≈ 55 %, its detectors ≈ 41 % — **hottest ≈ 55 %: green** (the benchmark's grade is green
below 70, amber to 85). ★ A PROJECTION: the optional real Pi 2 run of `lora_rows` was not done.

The benchmark needs no new rule: `dec_lora_all` runs the real Engine and is scored by its hottest thread, and the Engine
now chooses 2 lanes on a quad core by itself.

## Dead ends

- Round-robin lanes (unbalanced, above); detector-count weights (flipped the imbalance).
- Merging deliver and the detectors into one thread per lane: on the default 868 set it would put 34.7 + 37.6 % on one
  thread — worse than today; lanes keep the two-thread pipeline.

## RESUME HERE

1. **Real Pi 2 run** of `lora_rows` (0 listeners, never :48000, restore everything, agcLock back to 1) to replace the
   projection.
2. **Band digital spots**: measure its per-thread layout (`vibe_decoder_host.h` Band host); if JTTY and the tuners share
   one thread, give the JTTY engine its own (by mode, not by channel).
3. **AIS**: its cost is the shared full-rate front FIR, not the channels — a half-band cascade (as `vibe_avi_front.h`)
   is the fix there, not threads.
