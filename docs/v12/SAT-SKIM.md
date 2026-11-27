# The satellite digital-mode skimmer (beta97, branch b97-sat-skim)

Stuart, 2026-11-27: he decoded FT4 through JO-97's transponder on the XCover with only the satellite Doppler lock's
correction (v1, b97-xpdr-tune: "decode here", spots tagged via the bird). v2 is his decision: **skim the whole
transponder passband, live.**

## Where it sits

| Part | File |
|---|---|
| The skimmer: windows, slot buffers, the catch-up queue, workers, dedupe | `android/app/src/main/cpp/decoders/sat_skim.h/.cpp` |
| The stream it reads: the finder's ONE Doppler mixer (`Opts::turned`) | `vibe_xpdr_run.h` |
| Wiring, tags, the SKIM switch, keep-the-IQ | `vibe_decoder_host.h` `buildSkimLocked_`, `xpdrSkim`, `emitSpot(…, tag)` |
| Keep the corrected IQ (WAV in the gallery) | `vibe_sat_skim_iq.h`, `vibe_gallery.h` `Store::patch` |
| The transponder passband in the pick's params (`plo`/`phi`), unattended passes | `vibe_sat_engine.h` `decoderOf`, `passKeepableWith`, `passDecoderOf` |
| Settings (`satPasses.skim / skimMode / skimIq`) | `vibe_bgdecode_config.h`, Setup page `#spSkim…`, app `BackgroundDecodingSetup` / `bgDecode.ts` |
| The pass itself | `local_sdr_shim.cpp` `satPassTick` (consider), `satPassStartLocked` |
| Clients (SKIM FT8/FT4, its line, the heard list) | `src/services/xpdr.ts`, `web/client/src/satellites.ts`, `src/components/SatellitePanel.tsx` |

## How it works

* **One Doppler correction for the whole passband.** The finder already cuts an IQ slice fixed on the span's
  satellite-frame centre and turns it by the model's Doppler at that centre. The skimmer reads that turned stream (only
  while the model's Doppler is in it). The differential across a passband — 2.7e-5 × the passband width: ≤ 0.6 Hz for
  JO-97, ≤ 2.7 Hz for FO-29's 100 kHz at 435 MHz — is inside FT8/FT4's tolerance.
* **Windows.** USB dials every 2.5 kHz; each window is searched 200–3000 Hz above its dial (neighbours overlap by
  300 Hz, more than an FT4 signal); the span + 1.4 kHz each side; ≤ 64 windows. JO-97 9 · RS-44 25 · FO-29 41.
  Each window's 12 kHz audio is cut by Band mode's `DigiTuner`. An LSB downlink (`"sb":"lsb"`) is mirrored first and
  its spots mirrored back. Every listed bird comes down USB (an inverting transponder turns an LSB uplink into a USB
  downlink); `inverting` changes nothing for decoding.
* **Slots.** One 15 s int16 buffer per window per FT8 slot (360 KB) serves FT8 and FT4's two 7.5 s slots. Tasks: FT4's
  first half at +7.5 s, FT8 and FT4's second half at +15 s.
* **Workers.** Live: up to 4 (cores − 1); catch-up: 1. Decoder priority (`vibeDecoderLagThread`), background class for
  an unattended pass. Each worker owns one FT8 and one FT4 slot decoder (≈ 4 MB), not one per window. Options are
  `Ft8Decoder::liveOptions` — the live decoder's own rules, pulled out of `runDecode`; extra passes only inside 40 % of
  the task's share of the slot, never when the server is loaded.
* **The catch-up queue.** A task not decoded in time waits. Bounded: a 24th of RAM, 8–64 MB (~10 MB on a 256 MB box);
  past it the oldest slot (every window's tasks) is dropped and logged. Spots decoded ≥ 30 s after their slot are
  marked `late` ("caught up").
* **The load ladder** (`digi_load_ladder.h`): behind = (live) a task still waiting a slot later, (catch-up) the queue
  over half its cap → depth steps down; calm brings it back.
* **Duplicates** from the overlaps: one spot per mode + slot + message.
* **Tags.** Each slot buffer takes the tag (bird, NORAD, pass AOS) when it starts, so a slot decoded after LOS keeps it.
  `emitSpot` adds `"via"`, `"norad"` and `"pass"`, files it as ORIGIN satellite and holds it back from PSK Reporter.
  Spots go through a sink that outlives the host (`SkimSink`), so a queue worked off after the finder has gone never
  touches a dead host. CW across the transponder was already the finder's own skimmer.
* **Default.** The owner's `skimMode`: `auto` (the benchmark's Band budget covers every window light, 1.2 FT8 units a
  window → live, SKIM starts on; else catch-up, SKIM starts off), `live`, `catchup`. A listener can always switch SKIM
  on; an unattended pass forces it on (`"skim":1`).

## CPU (measured on the M5, `tools/ft8-bench/sat_skim_bench`; others scaled)

One FT8 or FT4 station in every window at −12 dB (a busy transponder), full depth, one worker:

| windows | slice | decode thread (tuners) | workers per 15 s slot |
|---|---|---|---|
| 9 (JO-97) | 48 kS/s | 2.2 ms per second (0.2 %) | 68 ms (0.5 % of a core) |
| 25 (RS-44) | 96 kS/s | 7.1 ms per second (0.7 %) | 185 ms (1.2 %) |
| 41 (FO-29) | 128 kS/s | 13.2 ms per second (1.3 %) | 301 ms (2.0 %) |

≈ 2.5 ms per task. Scaled by the FT8 ratios already measured in this repo (FTX-SENSITIVITY.md: Pi 2 ≈ 50× the M5, Pi 3 /
A53 ≈ 25–30×, Pi 4 ≈ 10×, Pi 500 ≈ 3.5×; the tuners by DIGITAL-SPOTS.md's 20–30×) — **estimates, not measured on the
boards**:

| box | JO-97 (9) | RS-44 (25) | FO-29 (41) | expected default |
|---|---|---|---|---|
| Pi 500 / x86 laptop | ~2 % of a core | ~4 % | ~7 % | live |
| Pi 4 | ~5 % | ~13 % | ~21 % | live |
| XCover 4S (A53 1.6 GHz, ~21×) / Pi 3 (~28×) | ~10–13 % + tuners ~5 % | ~26–35 % + ~16 % | ~42–56 % + ~30 % | JO-97 live; RS-44 live on the benchmark's word; FO-29 catch-up |
| Pi 2 (A7 900 MHz, ~50×) | ~23 % + tuners ~5 % | ~62 % + ~18 % | ~100 % + ~33 % | JO-97 live; RS-44 / FO-29 catch-up |

Quiet windows cost less than the busy ones measured here; light depth ≈ 0.6×. The armhf build of the bench
(`tools/ft8-bench/qemu/xbuild-satskim.sh`) builds; its instruction count needs the qemu plugin image
(`tools/band-bench/qemu.sh`), not present on the Mac it was built on.

## Tests

`test-sat-skim` (the core behind the finder's mixer: windows; Stuart's JO-97 clip under a simulated pass Doppler —
both CQs; synthetic RS-44 / AO-7 Mode A / LSB-downlink passbands — every station once at its frequency; the queue
bounded, oldest dropped and logged), `test-sat-skim-host` (the clip through the decoder host: tags, nothing to PSK
Reporter, store origin satellite, SKIM off/on, the unattended pass's settings and attach, the tag kept after LOS, the
kept WAV), `test_xpdr_app`, `test_bg_decode_model`, `test-setup-satpass`, `test-web-xpdr` (SKIM in the real page).

## Open

* A real pass on air (JO-97 / RS-44), live and catch-up, and the board timings (Pi 2, XCover) behind the estimates.
* The amateur / spots map's satellite layer is on the card system (b97-maps-port): the spots carry `via` / `norad` /
  `pass` and the station store's satellite origin; the layer hook belongs to that merge.
* A station transmitting the wrong sideband through an inverting bird comes down LSB and is not decoded (both sidebands
  would double the cost).
* On a SATELLITES radio's hybrid pass the shared pipeline is not fed, so the unattended pass does not skim there.
