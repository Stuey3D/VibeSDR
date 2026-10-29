# DRM's AAC: free routes to audio (2026-10-26, time-boxed investigation)

**The question:** how can VibeSDR get sound from DRM's older audio codec without buying ISO/IEC 14496-3? Most stations
still use it, including Radio Romania International (RRI).

The codec is DRM's AAC: ER AAC, error-resilient, with these differences from plain AAC:
- the HCR and VCB11 error-resilience tools are mandatory;
- 960-sample frames;
- an 8-bit CRC per frame, carried in the audio super frame;
- SBR data stored backwards at the end of each frame.

**Test material:** the real RRI 9570 kHz frames (`~/VibeSDR-test-audio/drm/uber_rri_9570000_iq48.wav`):
- 1732 AAC frames from 362 super frames;
- mono, 12 kHz, no SBR, about 11.6 kbit/s;
- extracted by our receiver.

**What was not used:** the leaked ISO copy in `~/Downloads` was not opened. GPL sources were read only for how the
format behaves, never copied.

## Result per route

| Route | Licence | Result | Evidence |
|---|---|---|---|
| **FAAD2, its DRM build** (`libfaad_drm`, already in Homebrew) | GPL-2.0-or-later. Its `COPYING` adds: *"Any non-GPL usage of this software or parts of this software is strictly forbidden. Commercial non-GPL licensing of this software is possible."* | **WORKS** | `NeAACDecInitDRM(12000, DRMCH_MONO)` decoded 1527 of 1732 frames. The 205 failures were "CRC error in DRM data", the frames before lock. The output is 122 s of audio: the envelope swings 24 dB (p10 to p90), spectral flatness is about 0, and 97 % of the energy is below 3 kHz, so it is programme, not noise. A 15 s clip is kept for listening: `~/VibeSDR-test-audio/drm/rri9570_faad2_reference_15s.wav`. **This proves our receiver delivers bit-correct audio frames.** |
| **FAAD2, normal build**, configured as plain ER-AAC-LC (object type 17) | GPL | **Does not work** | "Bitstream value not allowed by specification" on 1730 of 1732 frames. Inserting the 4-bit element tag did not help (17 of 1732). Object type 20 is refused at init. DRM's AAC is not plain ER-AAC-LC. |
| **Apple AudioToolbox** (macOS server, iOS app) | System decoder | **Does not work** | The first attempt failed because of our config, not only the codec: a raw ASC is refused with `'!dat'`, and the cookie must be an ESDS. With a proper ESDS, plain AAC-LC with 1024-sample frames is accepted. Object types 17 and 20 are refused `'!dat'` for every frame-length and frames-per-packet combination tried, and AAC-LC with the 960-sample flag is refused too. |
| **FFmpeg native** (any version, as a process on the server) | LGPL/GPL | **Does not work** | FFmpeg 9.0.1 refuses with "AAC data resilience (flags 5) is not implemented". The source (`libavcodec/aac/aacdec.c`) marks the error-resilience tools as not implemented, and no build option changes that. |
| **FFmpeg + libfdk-aac** (`--enable-libfdk-aac`) | "nonfree": distributions do not ship it | **Unknown** | Its wrapper opens FDK with `TT_MP4_RAW` plus the extradata ASC (`libavcodec/libfdk-aacdec.c:336`), so the object-type-20 route below would apply. A server owner would have to build FFmpeg themselves. |
| **Fraunhofer FDK-AAC, standalone** | FDK licence (below) | **Has DRM support. Untested here:** building it was blocked by the auto-mode permission check ("Code from External"). | Its public headers define `TT_DRM = 12` ("Digital Radio Mondial (DRM30/DRM+) bitstream format"), `AOT_DRM_AAC = 143` ("Virtual AOT for DRM (ER-AAC-SCAL without SBR)"), 144/145 with SBR and PS, `TC_RAW_SDC` (config = the DRM SDC), `DrmRawSdcAudioConfig_Parse`, and `CAPF_AAC_DRM_BSFORMAT`. So FDK decodes DRM's AAC itself when opened with `TT_DRM` and given the SDC audio config. |
| **Android MediaCodec** (FDK inside) | OS decoder, App-Store-safe | **Unknown — the most promising free route; one device test decides it** | Android's `C2SoftAacDec` opens FDK with `TT_MP4_ADTS` only, and for raw packaging passes csd-0 to `aacDecoder_ConfigRaw` as an ASC. `TT_DRM` cannot be reached. But FDK accepts object type 20 (ER AAC Scalable, DRM's base type) through an ordinary ASC: `aacdecoder.cpp` has the AOT 20 case and sets `AC_SCALABLE`. Its DRM-only branches (`AC_DRM`) concern the trailing bits, SBR and the CRC. So **non-SBR** stations like RRI may decode with csd-0 = an object-type-20 ASC (960 flag, section and spectral resilience on) and each frame given without its CRC byte. SBR stations probably will not: without `AC_DRM` the backward SBR bits would be parsed as extension payloads and the frame marked bad. |
| **libxaac** (Ittiam; Android's xHE-AAC decoder) | **Apache-2.0** (permissive) | **Unknown** | It decodes ER-AAC-LC with HCR (`ixheaacd_hcr.h`) and 960-sample frames, and its header parser knows object type 20. No DRM-specific framing (CRC, backward SBR) was found in its sources. |
| **Others** (Helix and so on) | — | No | No ER/HCR support found. |

### The FDK licence, quoted from its `NOTICE`

- **Binary redistribution:**
  - *"You must retain the complete text of this software license in the documentation and/or other materials provided with redistributions of the FDK AAC Codec or your modifications thereto in binary form."*
  - *"You must make available free of charge copies of the complete source code of the FDK AAC Codec and your modifications thereto to recipients of copies in binary form."*
- **Fees:** *"You may not charge copyright license fees for anyone to use, copy or distribute the FDK AAC Codec software or your modifications thereto."*
- **Patents:** *"NO EXPRESS OR IMPLIED LICENSES TO ANY PATENT CLAIMS, including without limitation the patents of Fraunhofer, ARE GRANTED BY THIS SOFTWARE LICENSE. … You may use this FDK AAC Codec software or modifications thereto only for purposes that are authorized by appropriate patent licenses."*
- **Android:** *"most manufacturers of Android devices already license these patent claims through Via Licensing or directly from the patent owners, and therefore FDK AAC Codec software may already be covered under those patent licenses when it is used for those licensed purposes only."*

So bundling FDK in the server or the iOS app is possible as far as copyright goes. It is not GPL, so there is no App Store clash, as long as the source is offered. **But it grants no patent licence, and it means shipping a codec**, which is against the DAB+ / DRM position. Android's own copy is the exception: the device maker's patent licence already covers it.

## What DRM's AAC is, as a format (learned by reading FAAD2 and FDK — facts only, nothing copied)

**A DRM AAC frame is one fixed element (ER AAC Scalable's core layer), not the element loop of plain AAC.** Compared
with an ER-AAC-LC single-channel element:
- there is no syntax-element id and no 4-bit element instance tag;
- the header has a reserved bit, the window sequence and shape, and `max_sfb` (with grouping for short windows), with no predictor bit;
- in stereo, one shared window set with an M/S mask and flags;
- per channel: a TNS-present bit, an LTP bit, then global gain, section data and scale factors;
- **the TNS data of both channels comes after both channels' side information**, not inside each channel;
- there are no pulse or gain-control fields;
- then the HCR-reordered spectral data per channel.

**The CRC:**
- The super frame's per-frame 8-bit CRC covers the side information. A decoder is given it as the byte before the frame.
- With SBR, the SBR part carries its own 8-bit CRC at its start (which is the end of the frame).

**SBR:**
- SBR is read **backwards from the end of the frame**: bit order reversed in each byte, bytes from last to first.
- Its length is the frame length minus what the AAC core used.
- The SDC's SBR flag and audio mode say whether it is there. The frame never signals it.
- The SBR decoder runs at twice the core rate.

**HCR (spectral data):**
- Two lengths are sent in the side information: the reordered spectral data's total length and the longest codeword's length.
- The data is cut into fixed-width segments; the width is the smaller of the longest codeword and the codebook's maximum codeword length.
- Each segment's first codeword is a priority codeword, decoded at once. Codewords are taken in codebook-priority passes, VCB11's virtual books before the others.
- The rest fill the segments in sets, rotating segment assignment and reading alternately forwards and backwards.

**VCB11** = codebooks 16–31. They are codebook 11 with a declared maximum escape magnitude per book.

## Our own route: a DRM-element → ER-AAC-LC "re-wrap" (not a full HCR undo)

**Key point:** the target decoders (FDK, libxaac, normal FAAD2) all accept ER-AAC-LC *with* HCR and VCB11. So the
spectral data does **not** need un-reordering — HCR's lengths and payload can be carried over unchanged. A re-wrap only
re-orders the side information:
1. insert the instance tag;
2. move `ics_info` after global gain and add a zero predictor bit;
3. add zero pulse and gain-control flags;
4. move each channel's TNS data back into its own channel;
5. for SBR, carry the backward SBR bits forward as an SBR extension;
6. drop the CRC byte.

**The blocker:** finding where the scale factors end needs the **scale-factor Huffman table**. That is only in ISO
14496-3 (paid) and in GPL/LGPL code, which the rules forbid copying.

**Estimate:**
- Mono without SBR: about 2–3 days once that table problem is solved.
- Stereo, SBR and PS: about 4–6 more days, and it needs the SBR header syntax too.
- On top of that, the re-wrapped stream still needs a decoder that does HCR. On Apple there is none, which rules the re-wrap out there anyway.

## Recommendation

1. **First, test Android MediaCodec on a device** — the Xcover, with the RRI frames. Feed csd-0 = an object-type-20 ASC (960 flag, section and spectral resilience on, `epConfig` 0) and each frame without its CRC byte. If non-SBR DRM AAC plays, the Android app and Lite can decode it for free with the OS decoder. It is App-Store-safe and patent-covered by the handset maker. About 2 hours of work.
2. **Server (Linux/macOS):** no free OS decoder exists. The honest choices for Stuart are:
   - (a) keep "no codec": AAC stations stay silent with the explanation, as built;
   - (b) allow an external helper: FAAD2's DRM build is GPL, as the server is, so a small GPL helper process could link it where the system has `libfaad_drm` (Homebrew does; Debian ships only the normal FAAD2). It is proven to work on the real frames, but it is shipping or using a codec in all but name;
   - (c) FDK linked in: copyright-fine, no patent licence.
3. **iOS / Apple TV / Mac app:** AudioToolbox cannot decode it. The only route is bundling FDK or libxaac, which means a shipped codec with no patent grant, so not recommended without Stuart's explicit decision.
4. **Do not start the re-wrap** while the scale-factor table has no clean source.

---

# Stuart's decision (2026-10-26): FAAD2's DRM build INSIDE the server — built

**Where it goes:**
- `vibe_drm_audiodec.h`: `DrmFaad` decodes DRM's AAC. It is fed the CRC byte, and is told about SBR and PS by the SDC.
- Concealment: the last good frame plays again, fading out, and the next good frame fades in.
- Output is upsampled to 48 kHz stereo.

**The licensing boundary:**

| Build | FAAD2? | How |
|---|---|---|
| VibeServer: Linux deb (x86_64, i386, arm64, armhf, ARMv6) | Yes | `linux/build-deps.sh`: FAAD2 2.11.3 from the pinned GitHub tag (sha256 `860ab620…`) with VibeSDR's patch series (since 2026-10-29, see "DRM's FAAD2, optimised" below), static `libfaad_drm.a`, float. The NeAACDecInitDRM check is in it. `COPYING.faad2` goes to `/usr/share/doc/vibeserver` and `build-deb.sh` checks it is there. |
| VibeServer Mac app | Yes | `mac/build-deps.sh` (the same pin, patches and checks). `COPYING.faad2` goes into `Resources/`. |
| VibeServer Windows | — | For the windows-m1 agent: add the same pinned build to `vibeserver/windows/build-deps.sh`. That branch was not edited. |
| Android app (Play + GitHub APK) and VibeServer Lite: the on-device server | **Yes, since 2026-10-29** (it was missing: DRM AAC was silent on Android servers) | `sdr-kit/build-faad2.sh` (the same pin and patches): `sdr-kit/{arm64-v8a,armeabi-v7a}/lib/libfaad_drm.a` at android-24 for the app, `lite/native/kit/armeabi-v7a/lib/libfaad_drm.a` at android-21 for Lite (checked free of the API-23 `stderr` symbol). The CMake links whichever its `SDR_KIT_ROOT` holds — the block used to sit inside `if (NOT VIBE_LITE_APP)`, so Lite could never have linked it. **This puts GPL-2.0+ code in the Android app's APK on Play and GitHub, which is GPL itself — Stuart to confirm.** |
| iOS / iPadOS / Mac Catalyst app (App Store) | **Never** | `build_ios.sh` never defines `VIBE_HAVE_FAAD2`, and **fails if any FAAD2 symbol is in the archive.** |

**Building it was not possible here:** the auto-mode permission check blocks building external code. Everything was
measured against Homebrew's FAAD2 2.11.3 (the same version as the pin), linked as a dev library. The `build-deps`
additions are syntax-checked but have not run.

**Results:**
- **`test-drm-faad`** (real RRI, 150 s):
  - 1526 frames decoded;
  - the output is programme (envelope p10/p50/p90 −60/−33/−25 dBFS);
  - with one frame in seven corrupted, 420 frames are concealed, with no larger step (0.0514 against 0.0525 clean) and no gap;
  - **0.63 % of one Mac core per DRM listener with audio.** Pi 2 estimated at about 15–25 % of one of its four cores.
- **`test-server-drm`** (40/40): RRI plays through the real server as PCM and as Opus (200 packets in 4 s, up to 369 bytes), on both radio kinds.
- **`test-web-drm`** (16/16): the box shows "Audio … via FAAD2 2.11.3".

# DAB+ through FAAD2 — the owner's choice since 2026-10-26; THE LINUX DEFAULT since 2026-10-29

**2026-10-29 (Stuart: "use FAAD2 for DRM and DAB+ by default and FFmpeg where needed"):** on Linux servers FAAD2 is
now the DAB+ default and FFmpeg the owner's alternative; the packages no longer Recommend the distribution's FFmpeg — it is
used only if installed — and xHE-AAC is libxaac, built in ([LIBXAAC.md](LIBXAAC.md); the FFmpeg bundle of
[BUNDLED-FFMPEG.md](BUNDLED-FFMPEG.md) was replaced the same day). The Mac keeps the platform's decoder as its default; Android
keeps AMediaCodec (FAAD2 there only for DRM's AAC). The setup page's switch is now a choice, "DAB+ audio decoder"
(config `dabAacDecoder`: 0 default, 1 FAAD2, 2 FFmpeg). The history below is kept as it was written.

**First version below (bench multiplex, synthetic damage). Superseded by the off-air measurements in the next section.**


`test-dab-faad` (a CMake target) runs the benchmark multiplex through the real `DabService` and takes the very ADTS
units the decoder gets. It compares them with today's FFmpeg path (the server's own arguments) after the server's
pacing (FFmpeg decodes each 960-sample unit as 1024; the service plays it at the measured rate).

**First finding:** the server's FAAD2 is the DRM build, and **that build refuses DAB+**: "No standard extension payload
allowed in DRM" on 254 of 264 units. DAB+ would need the **plain** FAAD2 build beside it. The two export the same
symbols, so one would have to be built with prefixed symbols. The figures below are from the plain build (Homebrew's).

| Measure (bench multiplex; 48 kHz AAC-LC stereo; 24 kHz HE-AAC mono) | FAAD2 (plain) | FFmpeg (today) | Verdict |
|---|---|---|---|
| Tones, true 220 / 275 / 330 Hz | **219.98 / 274.95 / 330.03** | 220.44 / 275.47 / 330.60 | FAAD2 is exact (a real 960 transform). FFmpeg is +0.2 % (its 1024 decode, re-paced). |
| Tones, true 257 / 321 / 385 Hz | **257.01 / 320.96 / 385.03** | 257.93 / 322.24 / 386.49 | Same |
| PCM against each other (gate 1: "within float rounding") | — | — | **FAILS by construction.** SNR between them is 0 dB gain-matched and max diff about 0.1. They are different transforms, so the waveforms decorrelate. |
| Level | −36.16 dB | −36.73 dB | **+0.57 dB louder with FAAD2** (gate 5: a change) |
| Stereo image (L/R correlation; side/mid) | 1.000; −326 dB | 1.000; −326 dB | Tie. **The bench has no real stereo or PS content** — gate 2 is not covered. |
| 1 unit in 10 CORRUPTED (largest step; 10 ms dips) | 0.047–0.109; 0–29 dips | 0.013–0.021; 0 dips | **FFmpeg better.** FFmpeg drops the bad unit (a 1 s gap that the service's pacing fills); FAAD2 decodes the garbage before its concealment can act. In the live path DAB+'s per-unit CRC removes such units first, so this case is harsher than reality. |
| Start-up (first PCM after the first unit) | **0.1–0.4 ms** | about 1.2 s (live pipe) | FAAD2 |
| CPU for 10.5 s | **0.07–0.13 %** | 0.33–0.37 % (child process) | FAAD2 |
| Memory | about 0.2 MB in process | about 16 MB child RSS | FAAD2 |

**Not done:** a real off-air DAB+ recording was not available. `weak-5/6.vbu8` and `dab-bench.vbu8` (the impaired clip)
were not run, because the clip-mode test links the DRM build, which refuses DAB+.

**Per the gate, FFmpeg stays the DAB+ default, and nothing in the DAB path changed** (one null test hook in
`DabService`). FAAD2 is more accurate, faster, smaller and quicker to start. But it is not sample-identical, it is
0.57 dB louder, and its handling of corrupted units is worse unless it is fed only CRC-good units, as the live path
does. **Stuart decides.** If he wants it, the next steps are:
1. build the plain FAAD2 with prefixed symbols;
2. feed it only CRC-checked units, with gaps concealed rather than decoded;
3. level-match it;
4. re-run on real stereo/PS DAB+ and on the weak clips.

The deb's FFmpeg `Depends` is therefore unchanged.

**Side findings in today's FFmpeg path** (`vibe_dab_aacdec.h`, `AacDecoderFfmpeg`) — reported, not changed:
- `drain()` keeps `r / 4` floats from each `read()` and drops the remainder, so a read that splits a float would
  misalign everything after it;
- `decode()` treats `EAGAIN` on the non-blocking pipe as success and loses that unit.

The test harness lost about 9 % of units this way when it wrote faster than real time. The live service writes at
programme pace, which is why it has not shown up.

## Off air, 2026-10-26 — the second round

**Inputs.** Raw IQ (2.4 MS/s) from Stuart's Pi 500 RTL: 12B BBC National (strong, MER 22 dB class), 11D Digital One
(strong), 10D Herts Beds Bucks (weak: undecodable for 37 s, then clean), and the 12B recording damaged in software
(threshold noise; 1 Hz and 2 Hz Rayleigh fading) so the receiver's own Reed-Solomon and unit CRC decide what is lost.
9D was not receivable on that aerial. `dab-unitlog` runs the real `DabService` over each and logs every unit slot:
good, failed its CRC, or in a lost super frame. The recordings and logs live only in `~/VibeSDR-test-audio/dab/`.
All services found were HE-AAC v2 (16 kHz core, SBR + parametric stereo).

**The reference.** FFmpeg's own decoder, fed LOAS/LATM whose StreamMuxConfig carries the AudioSpecificConfig with
frameLengthFlag = 1, decodes DAB+'s true 960-sample frames (2880 samples at 48 kHz per unit, exactly). It is used
ONLY as a yardstick (`test-dab-faad`), never as a server path.

| Measure | FAAD2 | FFmpeg today | Reading |
|---|---|---|---|
| Level vs the 960 reference | within 0.02 dB | −0.56 dB | Today's path loses 0.56 dB (the 1024 mis-framing). FAAD2 is the correct level; matching today would mean making it wrong. |
| Long-term spectrum vs reference (worst third octave, 63 Hz–16 kHz) | 0.2–1.1 dB | 2.0–3.8 dB | FAAD2 closer |
| Short-time spectral distance vs reference (median) | 0.25–0.85 dB | 1.1–1.3 dB | FAAD2 closer (waveforms differ by design: SBR noise, PS synthesis) |
| Stereo image (side/mid vs reference) | within 0.1–0.6 dB | within 0.1–0.3 dB | Same image |
| Hard edges into/out of silence (service output, 38 s, 3 % / 20 % / 40 % units lost) | 0 / 0 / 0 | 16 / 351 / 204, largest step 0.33 FS | FAAD2 conceals; FFmpeg splices, then cuts to silence once 240 ms behind |
| Silence in the same runs | 0.12 / 4.5 / 10.2 s | 2.8 / 9.7 / 16.9 s | — |
| CPU (Mac M-series, in the service) | 0.10–0.11 % of a core | 0.16–0.19 % (child) | FAAD2 now includes the 32→48 kHz polyphase resampler |
| Memory | ≈0.5 MB in process | ≈17 MB child RSS | — |
| First audio | at once | 1.2 s live pipe (≈3 s in the harness incl. priming) | — |

**Concealment** (`AacDecoderFaad::conceal`, driven by `DabService`'s unit clock): each missing unit becomes exactly one
unit of audio — the last good unit decoded again, fading out over ~120 ms (raised cosine, so a one-unit hole is a
−6 dB dip), then silence; the next good unit fades in. A CRC hole is concealed in place, an all-bad super frame at once,
a lost stretch by `holdTimeline` (concealed units instead of zeros). On air the unit CRC removes corrupted units, so
FAAD2 is never asked to decode one; a unit that passes the CRC and still fails in FAAD2 (or comes out above +12 dBFS)
is concealed, never played.

**Packaging.** `build-deps.sh` (Linux and Mac) build FAAD2 2.11.3's plain library twice: once unprefixed to read its
global symbols, once with a forced include renaming every one to `vibedab_` — `libfaad_dab.a`, checked to define
nothing unprefixed. Measured in the build images: armhf, ARMv6 (Tag_CPU_arch v6) and arm64; an armhf binary links
`libfaad_drm.a` and `libfaad_dab.a` together. CMake `VIBE_DAB_FAAD2` (on) requires it in a release deps prefix.

**The switch** (as first built — now "DAB+ audio decoder", `dabAacDecoder`, see the top of this section). Setup page, per radio: "Decode DAB+ audio with FAAD2, inside VibeServer" (`dabFaad2`), drawn only where
`hwinfo.dabFaad2Avail` (FAAD2 built and FFmpeg working). Live: the next super frame restarts the decoder as a service
change would. `VIBE_DAB_AAC=faad2|ffmpeg` overrides it. The DAB panel's "AAC decoder" line shows the decoder, its CPU
(`aacCpu`, % of a core: FFmpeg's child or FAAD2's time) and units concealed.

**Gate (Stuart's).** No regression found on these recordings: level is the correct one (+0.56 dB vs today), the
spectrum is closer to a true 960 decode than today's, the stereo image matches, damage is concealed without a hard
edge where today cuts, latency and CPU and memory are lower. Not sample-identical to today — today is not a 960 decode.
Not covered: AAC-LC or HE-AAC v1 stereo services (none on the muxes received), and a real "slowly fading" weak mux
(10D was a cliff). **Stuart listens (Desktop/DAB-FAAD2-vs-FFmpeg) and decides; FFmpeg stays the default until then.**

## DAB+ on its own thread, and a lighter FAAD2 (2026-10-26/27, Stuart's Pi 2)

**Why.** With FAAD2 on, the Pi 2 (Cortex-A7, the minimum spec) broke up: FAAD2 and its 32→48 kHz resampler ran
inline on `vibe-dab`, the thread that already carries the OFDM front end, FIC and Viterbi (~90 % of a core on its own),
and it read "FAAD2 · 11.9 % of a core" against FFmpeg's 6–10 %.

**The thread.** `AacDecoderFaadAsync` (vibe_dab_faad.h): `vibe-faad2`, nice −17 (one rung below the −19 chain that
feeds it, far above spectrum and decoders). DabService's decode/conceal/reset calls become jobs on a 32-slot SPSC ring;
the PCM comes back through a sink into `pushPcm48Stereo`. Drop-never-wait: a full ring (or > 500 ms queued) makes the
unit a missing one, concealed in its place when the next job fits — counted (`aacDropped`: the DAB panel's "too late",
the health pill's "DAB+ audio decoder: TOO HEAVY"). Resets are generations: stale jobs never run, stale PCM is dropped
under `pm_`. `holdTimeline` counts audio in flight as pushed. `VIBE_DAB_FAAD_INLINE=1` restores the inline layout.
Proof (armhf, qemu-arm per-thread instruction counts, 12B C229, full DabService, nothing dropped): inline → vibe-dab
33.41 G; threaded → vibe-dab 29.57 G + vibe-faad2 3.83 G. The Pi 2 A/B (faad4, 9A) confirmed vibe-dab ~90–100 % with
either decoder — the demodulator's own load.

**Where FAAD2's armhf cost was, and what was done** (armhf guest instructions, qemu-arm + a block-counting plugin; a
proxy, not cycles — NEON q-ops count once):

| Step | HE-AAC v2 12B, 59 s | AAC-LC 48 kHz stereo, 60 s | vs stock FAAD2 |
|---|---|---|---|
| stock 2.11.3, its own flags | 3209 M | 1519 M | — |
| `-funsafe-math-optimizations` (GCC puts no float loop on ARMv7 NEON without it) | 2455 M | 1360 M | 132.5 dB |
| + patch 1: no `-ffloat-store` (FAAD2's CMake adds it for every GCC: every float temporary through the stack) | 1241 M | 770 M | 132.5 dB |
| + patches 2–7 (SBR: DCT-IV kernel, QMF analysis/synthesis, HF generation/adjustment; PS: decorrelator, energy, transients, mixing) | 843 M | 770 M | 132.3 dB |
| + patch 8 (AAC core: radix-4 FFT pass, iMDCT twiddles, long-window overlap, no per-call 8 KB zeroing) | 816 M | 537 M | 131.7 / 140.1 dB |

"vs stock": SNR of the stock output against the difference; max |diff| ≤ 2.4e-6; level, every third octave 63 Hz–16 kHz
and side/mid identical to 0.0001 dB. Mac (21 real logs, 1909 s): 134.1 dB NEON path, 134.3 dB scalar path.
**Our resampler** was the other half: 64-bit division per output (a library call on 32-bit ARM) — 1.3 G of the
decoder thread's 2.15 G. Now 32 taps a branch, no division, the 2:3 case twelve outputs at a time: 0.18 G.
**The decoder thread end to end** (12B, armhf): 3.83 G (faad3/faad4) → 1.10 G. Pi 2 estimate by proportion (an
estimate): HE-AAC v2 ≈ 3–4 % of a core, 48 kHz AAC-LC ≈ 5 % (faad4 measured 11.9 % there; FFmpeg 8.7 %).

**The patches** live in `vibeserver/linux/faad2-patches/` (series order, each documented in its header), applied by
both deps builders to the plain (DAB+) build only; the DRM build stays stock (until 2026-10-29: see below). GPL-2.0-or-later,
published with FAAD2.
Not measured on: real AAC-LC or HE-AAC v1 DAB+ recordings (none received here — the AAC-LC row is a 1024-sample
ffmpeg stream), the 34-band PS configuration.

**Setup page — the switch would not stay off.** The page saves through the front door; the radio's own process kept
the old `dabFaad2` in g_runtimeConfig, and its next self-persist (the DAB block, as the last listener left) folded every
per-radio field from that copy back into config.json. The live-persist handler now folds its patch into the radio's
entry as it is on disk (vibeserver/main.cpp). scripts/test-setup-dab-faad2.mjs (headless muted Edge) reproduces it.

# DRM's FAAD2, optimised (2026-10-29)

**Stuart:** DRM AAC "fully optimised like DAB+". The DRM build (`libfaad_drm.a`) was still stock FAAD2: `-ffloat-store`
on every GCC build and none of the DAB+ series.

**What changed.** The patch series (`vibeserver/linux/faad2-patches/series`) is now applied ONCE, before both builds, by
`linux/build-deps.sh`, `mac/build-deps.sh` and `sdr-kit/build-faad2.sh`. Patches 1–8 apply unchanged with DRM_SUPPORT
(the 960 frame length, ER, the DRM SBR all build and run through them). Patch 9 is new and is DRM's:

- **0009 HCR** (`hcr.c`, `reordered_spectral_data`). DRM's AAC is error-resilient AAC with Huffman codeword reordering;
  on the Pi 2 that one function was 60 % of the stock decoder on an AAC mono service (84 % with its division helper once
  1–8 had made the rest cheaper). Step 1 scanned every section for every 4 lines of every sfb in all 22 pre-sort passes;
  step 2 visited every codeword in every trial and took a `%` per visit — `__aeabi_idivmod`, a library call on ARMv7.
  Now a table names each sfb's section, passes and sfbs that cannot match are skipped, each trial visits only the
  undecoded codewords with a running index, and the set ends once nothing is left to decode. Integer only, the same
  order: **bit-identical** to stock, clean and with frames corrupted on purpose (1 in 2/3/5/7), under ASan + UBSan.
- **No `-funsafe-math-optimizations` for DRM** (DAB+ keeps it). Measured on the Pi 2 it bought DRM nothing
  (0.365 vs 0.360 % of a core) and cost output margin (KTWR 132.0 → 131.7 dB on armhf, 130.99 dB on the Mac).

**Test material.** Real DRM AAC only, dumped from the receiver to `.drmau` files (`vibeserver/faad2-drm-bench/
drm-au-dump.cpp`) and decoded by a standalone bench (`faad2-drm-bench.c`, one static binary):
- RRI 9570 kHz, AAC mono 12 kHz (`uber_rri_9570000_iq48.wav`, 1825 frames, 142 s);
- KTWR Japanese, **HE-AAC (SBR) mono** — the KiwiSDR project's published sample recording `DRM.KTWR.slideshow.au`
  (12 kHz IQ, resampled to 48 kHz; 4840 frames, 387 s). It is test data, not in the repo.
- Recorded today off Stuart's UberSDR: TDF 13695 kHz ("TDF DRM", AAC mono) — too weak, every frame refused by stock
  FAAD2 too; 13650 kHz never decoded. **No parametric-stereo or stereo DRM station was found**, so DRM PS
  (`drm_dec.c`) is not exercised by any test here (forcing DRMCH_SBR_PS_STEREO on KTWR's mono stream gives
  byte-identical output: with no PS data in the stream the PS path does nothing) and was not optimised.

**Output against stock FAAD2 built for the same target** (SNR of stock against the difference; level identical to
0.00001 dB everywhere):

| Target (compiler) | RRI AAC mono | KTWR HE-AAC mono |
|---|---|---|
| amd64 (GCC 12) | identical | 137.9 dB |
| i386, SSE2 floor (GCC 12 `-m32`) | identical | 137.9 dB |
| arm64 (GCC 12) | 143.0 dB | 132.4 dB |
| armhf, NEON, ARM mode (GCC 12; qemu Cortex-A7) | 141.0 dB | 132.0 dB |
| ARMv6 (clang, Raspbian sysroot; qemu ARM1176) | identical | 137.9 dB |
| macOS arm64 (Apple clang) | 141.0 dB | 132.9 dB |
| Android arm64-v8a (NDK 27, emulator API 28) | 141.0 dB | 132.7 dB |

Patch 1 alone is bit-identical on armhf. Android armeabi-v7a was built and linked (Lite) but not run (no 32-bit ARM
emulator on this Mac); it is the same source and NEON path as the Linux armhf row.

**CPU, Raspberry Pi 2** (Cortex-A7, armhf as `build-deps.sh` builds it; the decoder alone, `nice -n 19`, median of 7
passes, % of one core):

| Build | RRI AAC mono | KTWR HE-AAC mono |
|---|---|---|
| stock FAAD2 2.11.3 (its own flags) | 1.663 % | 4.871 % |
| + patch 1 (no `-ffloat-store`) | 1.403 % | 2.920 % |
| + patches 2–8 (the DAB+ kernels) | 1.363 % | 2.543 % |
| **+ patch 9 (HCR) = shipped** | **0.365 %** | **1.318 %** |

4.6× and 3.7×. What is left on HE-AAC: HCR 16 %, the SBR DCT-IV 17 %, QMF synthesis 12 % / analysis 8 %, HF assembly
7 % (gprof on the Pi). On plain AAC: HCR ~40 %, the radix-4 FFT pass 15 %, Huffman 9 %. The 960-point transform's
radix-3/5/2 passes are scalar and small. Mac and Android arm64: 0.031 → 0.012 % and 0.054 → 0.025 %.

**Builds checked:** `linux/build-deps.sh armhf` in the release image (its `libfaad_drm.a` decodes byte-identically to the
library measured above); `mac/build-deps.sh` (DRM + DAB+ rebuilt with the series; test-drm 134/134, test-drm-signal
109/109, test-drm-faad 6/6 against it); `sdr-kit/build-faad2.sh` (three libraries); VibeServer Lite's
`libvibelocalsdr.so` (armeabi-v7a, android-21) linked with FAAD2; test-drm-faad built with the NDK for arm64 and run on
an API 28 emulator against the Android prebuilt — 1773 frames decoded, programme envelope, concealment, 6/6. amd64,
i386, arm64 and ARMv6: the FAAD2 build with each profile's flags, not the whole `build-deps.sh` (that runs on the
Lenovo at release).

