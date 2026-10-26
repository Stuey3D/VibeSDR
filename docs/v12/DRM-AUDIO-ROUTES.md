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
| VibeServer: Linux deb (x86_64, i386, arm64, armhf, ARMv6) | Yes | `linux/build-deps.sh`: FAAD2 2.11.3 from the pinned GitHub tag (sha256 `860ab620…`), static `libfaad_drm.a`, float. The NeAACDecInitDRM check is in it. `COPYING.faad2` goes to `/usr/share/doc/vibeserver` and `build-deb.sh` checks it is there. |
| VibeServer Mac app | Yes | `mac/build-deps.sh` (the same pin and checks). `COPYING.faad2` goes into `Resources/`. |
| VibeServer Windows | — | For the windows-m1 agent: add the same pinned build to `vibeserver/windows/build-deps.sh`. That branch was not edited. |
| Android app (Play + GitHub APK) and VibeServer Lite: the on-device server | Yes, once its prebuilt is made | `sdr-kit/build-faad2.sh` (the same pin, both ABIs); the CMake links it when it is there. **This puts GPL-2.0+ code in the Android app's APK on Play and GitHub, which is GPL itself — Stuart to confirm.** Lite has its own `sdr-kit` root (`lite/native`), so it needs the prebuilt copied there too. |
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

# DAB+ through FAAD2 — measured; NOT switched (the gate fails on two counts)

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
