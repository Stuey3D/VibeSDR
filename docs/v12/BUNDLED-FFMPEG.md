# The bundled FFmpeg, and FAAD2 as the Linux default (V12, 2026-10-29)

Stuart: *"bundle FFmpeg on the Linux servers as you described, use FAAD2 for DRM and DAB+ by default and FFmpeg where
needed."* And: *"Android server can use system hardware decoders."*

## What decodes what now

| Server | DAB+ (HE-AAC) | DRM's own AAC | DRM xHE-AAC (USAC) |
|---|---|---|---|
| **Linux .deb** (amd64, i386, arm64, armhf + ARMv6) | **FAAD2 in process — the default.** FFmpeg (bundled) if the owner picks it | FAAD2 (DRM build), as before | **bundled FFmpeg 8.1**, as a child process |
| macOS | unchanged: FFmpeg if Homebrew has one, else AudioToolbox; FAAD2 if the owner picks it | FAAD2 | AudioToolbox, FFmpeg as fallback |
| Android on-device server / Lite | unchanged: AMediaCodec | FAAD2 (DRM build) | MediaCodec |
| iOS / App Store app | no decoder at all (plays the server's audio) | — | — |

**The default**, `vibe_dab_aacdec.h`: `dabFaad2Default()` is true on Linux (not Android); `dabWantFaad2()` resolves
test override (`VIBE_DAB_AAC`) → owner's choice → default. The owner's choice is per radio, config **`dabAacDecoder`**:
0 = this server's default, 1 = FAAD2, 2 = FFmpeg (the platform's on a Mac). It replaces the `dabFaad2` bool, which both
writers always wrote, so `false` could not be told from "never chosen": `"dabFaad2": true` is read as 1 (explicit
FAAD2); `false` reads as 0 (the default — now FAAD2 on Linux). An owner who had deliberately picked FFmpeg picks it
again; it is stored as 2 from then on. Setup page: "DAB+ audio decoder" (Default — FAAD2 / FAAD2 / FFmpeg), drawn only
where the server has both (`hwinfo.dabFaad2Avail`); hwinfo also carries `dabAacDecoder` and `dabFaad2Default`. The
WebSocket `{"type":"dabAacDecoder","v":n}` sets it live; the older `{"type":"dabFaad2","on":0|1}` still works
(on = 1, off = 2).

**The Mac is unchanged** — it does not depend on a system FFmpeg (it prefers Homebrew's if present, otherwise
AudioToolbox), and its default stays the platform's. **Android is unchanged** (AMediaCodec / MediaCodec; FAAD2 only for
DRM's AAC, which no system decoder accepts).

## The FFmpeg we ship

- **FFmpeg 8.1.3**, `https://ffmpeg.org/releases/ffmpeg-8.1.3.tar.xz`, SHA-256
  `7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3`, signature checked against FFmpeg's release key
  `FCF986EA15E6E293A5644F10B4322F04D67658D8` (committed as `sdr-kit/ffmpeg-release-key.asc`). **Unmodified.**
- **Why 8.1 and not 7.1.x:** 7.1 was the first release with a USAC decoder, but 7.1.5 refuses every USAC stream using
  parametric stereo (`stereoConfigIndex > 0`, "AAC USAC Mps212") — common at low stereo rates, which is DRM's
  territory. 8.1 parses Mps212 (decodes those streams; the Mps212 upmix itself is not implemented) and carries a year
  of USAC bounds-check and correctness fixes.
- **Built by** `android/app/src/main/cpp/sdr-kit/build-ffmpeg-linux.sh`, on the Mac, in ONE native arm64 bookworm
  container (`vibeserver/linux/Dockerfile.ffmpeg`) that cross-compiles all five binaries (2 CPUs, nice 19, `make -j2`).
  No emulated container; qemu-user only runs the finished binaries for the smoke test. Output (gitignored, like
  cloudflared's): `tools/ffmpeg-linux/bin/`. `build-debs-remote.sh` calls it (a no-op when current), so the binaries
  travel to the Lenovo with the tree; the Lenovo never builds FFmpeg.
- **LGPL-2.1-or-later only**: no `--enable-gpl`, no `--enable-nonfree`, `--disable-autodetect` (no external library
  at all). FFmpeg's own libraries are linked statically into the one `ffmpeg` program; it links glibc only
  (`libc.so.6`, `libm.so.6`), GLIBC_2.35 at most — the same floor as vibeserver (`HAVE_ARC4RANDOM_BUF` is turned off
  after configure: arc4random_buf is glibc 2.36).
- **Only what VibeServer feeds it**: decoder `aac` (USAC is part of it), parser `aac`, demuxers `aac` (ADTS — DAB+)
  and `matroska` (DRM xHE-AAC, below), muxers/encoders `pcm_f32le`/`pcm_s16le`, protocols `pipe`/`file`, filters
  `aresample aformat anull atrim abuffer abuffersink`, swresample. No mp2/mp3 (DAB's MP2 is our own decoder), no
  swscale, no network, no ffprobe/ffplay. `--extra-version=vibeserver1` → `ffmpeg version 8.1.3-vibeserver1`.
- **The exact configure lines** are in `BUILDINFO` (installed to `/usr/share/doc/vibeserver/BUILDINFO`). Common part:
  ```
  --prefix=/nonexistent --enable-cross-compile --target-os=linux --extra-version=vibeserver1
  --disable-everything --disable-autodetect --disable-iconv --disable-network --disable-doc
  --disable-ffplay --disable-ffprobe --enable-ffmpeg --disable-avdevice --disable-swscale
  --enable-static --disable-shared --enable-runtime-cpudetect --enable-swresample --enable-avfilter
  --enable-decoder=aac --enable-parser=aac --enable-demuxer=aac,matroska
  --enable-muxer=pcm_f32le,pcm_s16le --enable-encoder=pcm_f32le,pcm_s16le --enable-protocol=pipe,file
  --enable-filter=aresample,aformat,anull,atrim,abuffer,abuffersink
  ```
  plus per architecture:

  | arch | adds | size | SIMD in the binary |
  |---|---|---|---|
  | amd64 | `--arch=x86_64 --x86asmexe=nasm` | 2 358 592 B | nasm SSE…AVX-512, runtime-selected; 203 SIMD functions |
  | i386 | `--arch=x86_32 --cpu=i686 --disable-pic -msse2 -mfpmath=sse` (non-PIE, so the 32-bit asm keeps its registers) | 2 381 820 B | SSE2 floor (C code), asm SSE…AVX runtime-selected; 182 |
  | arm64 | `--arch=aarch64 --enable-neon` | 2 100 680 B | NEON; 53 |
  | armhf | `--arch=arm --cpu=cortex-a7 --enable-neon --enable-vfp -marm -mfpu=neon-vfpv4 -mfloat-abi=hard` (the Pi 2; ARM mode, as vibeserver) | 1 809 760 B | NEON (Tag_Advanced_SIMD_arch: NEON); 40 `_neon` functions |
  | armv6 | clang, Raspbian sysroot, `--cpu=armv6 --disable-neon --enable-vfp --enable-armv6 --disable-armv6t2` (Pi Zero / Pi 1) | 1 686 424 B | VFP + ARMv6 media; no NEON (checked) |

  The script refuses a build whose SIMD objects/functions are missing, whose ELF is not the target machine, which
  links anything but glibc, or needs a glibc above 2.35.

## The package

- `/usr/lib/vibeserver/ffmpeg` (every arch), `/usr/lib/vibeserver/armv6/ffmpeg` (armhf only — the ARMv6 vibeserver
  binary knows at compile time to use its own; the NEON one would die "Illegal instruction" on a Pi Zero).
- `/usr/share/doc/vibeserver/COPYING.ffmpeg` (LGPL-2.1 text), `NOTICE.ffmpeg`, `BUILDINFO`.
- **Recommends: `ffmpeg` removed** (now just `vibeserver-mapdata-detail`). Depends unchanged (`adduser, curl, gzip,
  libc6 (>= …)`; the ffmpeg binary needs nothing new). `check-deb-depends.cmake` and `build-deb.sh` gates unchanged
  in spirit; `build-deb.sh` now also refuses a package without the binary, its licence/notice/BUILDINFO, our build
  (`-vibeserver1`), or (armhf) without the ARMv6 copy / with a non-NEON main copy.
- CMake (`vibeserver/CMakeLists.txt`, next to cloudflared): FATAL without `tools/ffmpeg-linux/bin/ffmpeg-linux-<arch>`
  unless `-DVIBESERVER_ALLOW_NO_FFMPEG=ON`.
- **Which binary runs** — `vibe_ffmpeg_path.h`, one answer for DAB+ and DRM: `VIBE_FFMPEG` → the package's own
  (relative to `/proc/self/exe`, then `/usr/lib/vibeserver/…`, `/usr/local/lib/vibeserver/…`) → `/usr/local/bin/ffmpeg`,
  `/usr/bin/ffmpeg` → `$PATH`. A system ffmpeg is used only if the bundled one is missing.
- The Portable image manifest still installs Debian's ffmpeg (`pocket-packages.txt`) — now redundant; marked to drop
  at the next image (left so existing cards and the converge tests are unchanged).

## Licence obligations (LGPL-2.1)

1. **Licence text and notice** in the package: `/usr/share/doc/vibeserver/COPYING.ffmpeg`, `NOTICE.ffmpeg`.
2. **Credit**: the app's About credits (`AboutOverlay.tsx`, "FFmpeg — the FFmpeg developers (LGPL-2.1-or-later)",
   beside a new FAAD2 entry, which was missing), the web client's About (`web/client/index.html`), README's Credits
   table. FFmpeg's trademark line is in the notice and the About entry.
3. **The corresponding source** — published beside the packages, at **`https://apt.vibesdr.net/source/ffmpeg/8.1.3/`**:
   `ffmpeg-8.1.3.tar.xz` (+ `.asc`), `build-ffmpeg-linux.sh`, `Dockerfile.ffmpeg`, `COPYING.LGPLv2.1`, `NOTICE.ffmpeg`,
   `BUILDINFO`, `manifest.sha256`. `publish-apt-remote.sh` (step 6b) copies them from `tools/ffmpeg-linux/bin/` into the
   apt repository on every publish and **refuses to publish without them** (git stores identical files once). The
   build script is also in VibeSDR's own source.
4. **Modifications**: none today. **Any patch we ever apply to FFmpeg (for example the SIMD work below) is a
   modification under the LGPL: it must be published with the source the same way** (add the patches to the build
   script's inputs and to `source/ffmpeg/<version>/`), and the notice changed from "unmodified".
5. **Relinking**: the LGPL lets a user replace the library. Ours is a separate program run as a child process, so the
   owner can replace `/usr/lib/vibeserver/ffmpeg` with their own build of the published source (the notice says so).

**Patents (flagged, not decided here):** docs/v12/DECODER-RESEARCH.md §4.4 noted that with the HOST's decoder, an
AAC/xHE-AAC patent licence was the distributor's concern, not ours. Bundling FAAD2 (since V12 DRM) and now FFmpeg's
USAC decoder makes VibeSDR the distributor of those decoders in the Linux packages. Via LA administers the AAC and
xHE-AAC pools. Stuart's call; nothing in this change depends on it.

## DRM xHE-AAC: LOAS replaced by Matroska (found while testing)

FFmpeg's LATM/LOAS reader parses the in-band config with no decoder context, and its USAC config reader refuses that
(`if (!ac) return AVERROR_PATCHWELCOME` in `ff_aac_usac_config_decode`) — in 7.1, 8.1 and 9.0.1 alike. So the DRM path
as built (LOAS to an FFmpeg child) could never have played an xHE-AAC station with FFmpeg: every unit came back
"Invalid data". The same units with the config as the stream's extradata decode exactly. `drm_audio.h` now has
`MkvStream` — an EBML header, an unknown-size Segment and Clusters, one SimpleBlock per access unit, the
AudioSpecificConfig as `CodecPrivate` — and `DrmFfmpegChild` runs `ffmpeg … -f matroska -i pipe:0 -f f32le -ar 48000 -ac
2 pipe:1`, writing the head to every (re)started child and never splitting a block (a partial pipe write is kept and
finished next time; 256 KB behind = restart). `loasFrame()` is kept for decoders that take LOAS.

## Tests

- **`scripts/test-bundled-ffmpeg.sh`** — ISO's USAC conformance streams as published in FFmpeg's test suite with their
  reference decodes (fate-suite.ffmpeg.org `aac/usac/Fd_1_c1_0x03, Fd_2_c1_0x03, Fd_2_c1_0x05, Fd_2_c1_Tns_0x04`,
  `aac/Fd_2_c1_Ms_0x01, _0x04`; fetched into `~/VibeSDR-test-audio/usac`) are rewritten by the server's own
  `MkvStream` (`vibeserver/drm-usac-stream.cpp`) and decoded by **each of the five bundled binaries** (qemu-user for
  x86, i386 as an Atom N270, Cortex-A7, ARM1176), with DrmFfmpegChild's exact argv. **30/30 pass** (6 streams × 5 architectures): against the
  reference, max |diff| 1 LSB on every stream and arch, SNR 64–106 dB (the C-only builds are the lower figures — float
  rounding, not error); the DRM argv gives the stream's full duration of 48 kHz stereo. The streams are frequency-domain
  USAC (mono/stereo, M/S, TNS; 32–88.2 kHz) **without SBR** — no published conformance stream with USAC SBR, Mps212,
  eSBR or LPD was available, so those remain untested until an xHE-AAC station is received.
- **LOAS refusal** reproduced with the Mac's FFmpeg 9.0.1 (the same units: 0 bytes out, "Invalid data").
- **DAB+ on Linux with FAAD2 the default** (arm64 bookworm build image, `VIBE_TESTS=… scripts/run-tests.sh`, bundled
  arm64 FFmpeg as the FFmpeg half): 28 DAB/DRM suites pass. `test-dab-faad-queue` 21/21 including the live switch
  FAAD2 → FFmpeg (bundled) → FAAD2. Pre-existing, not from this change: `test-dab-confirm` does not build on Linux
  (`vibe_dab_confirm.h` uses `size_t` without `<cstddef>`); `test-dab-faad` is a measurement harness whose 960-sample
  yardstick needs a full FFmpeg with the LOAS demuxer (Homebrew's on the Mac).
- **DRM AAC unchanged** (Linux): `test-drm` 134/134 incl. RRI 9720 kHz off air; `test-drm-faad` 6/6 (RRI 9570:
  1539 frames decoded, concealment, 0.96 % of an M-series core); `test-drm-signal` 106/106. (`run-tests.sh` now puts
  `-lfaad_drm` after the source — before, test-drm-faad could not link on Linux.)
- Setup page: `scripts/test-setup-dab-faad2.mjs` updated to the choice (`#dabDecoder` 1 / 2 → `dabAacDecoder`).

## CPU — where an xHE-AAC decode goes on armhf (the Pi 2)

qemu-arm (Cortex-A7) with a block-counting plugin, the unstripped armhf binary, DrmFfmpegChild's argv (Matroska in,
48 kHz stereo f32 out). Guest instructions — a proxy, not cycles (a NEON q-op counts once).

| Stream (conformance) | M instructions per second of audio |
|---|---|
| Fd_1_c1_0x03 — mono, 48 kHz (no resampling; upmixed to stereo) | **13.5** |
| Fd_2_c1_0x05 — stereo, 32 kHz → 48 kHz | **23.9** |
| Fd_2_c1_Ms_0x01 — stereo, 88.2 kHz → 48 kHz | 50.4 |

**Pi 2 estimate** (Cortex-A7 at 900 MHz, roughly 0.6–0.8 instructions a cycle for this code — an estimate, not a
measurement): **about 2–4 % of one core** for a DRM-like 24–48 kHz stream, perhaps 3–6 % once SBR is in play (its QMF
banks are already NEON) — i.e. around 1 % of the Pi 2's four cores, in a separate process. Comparable to FAAD2's DAB+
figure (3–5 %).

**Where it goes** — Fd_2_c1_0x05 (the middle case), share of all guest instructions, including libc/libm (mapped from
qemu's load addresses; libc's own names are the nearest exported symbol, so read those as "allocator / memcpy"):

| Share | Function | SIMD? |
|---|---|---|
| 30.5 % | swresample polyphase filter (`ff_resample_common_float_neon`, `…apply_filter_x8_float_neon`) — 32→48 kHz | **NEON already** |
| 24.7 % | USAC arithmetic spectral decoder: `ff_aac_ac_get_pk` 8.9, `ff_aac_ac_decode` 7.1, `decode_spectrum_ac` 6.4, `…get_context`/`update_context` 2.1 | **plain C** (USAC-specific; bit-serial by nature) |
| 12.1 % | **libm `cbrt`** (+ `frexp`, `modf`, `scalbn` inside it) — `aacdec_usac.c` dequantises every coefficient with double-precision `a * cbrt(a)` | **plain C** (glibc) |
| 12.6 % | lavu/tx inverse MDCT + FFT: `ff_tx_mdct_inv_float_c` 4.8, `ff_tx_fft_sr_combine_float_c` 4.0, `fft64/16/8/256…_ns_float_c` 3.8 | **plain C on armv7** — FFmpeg 8.1 has NEON tx only for aarch64 |
| ~7 % | libc memset + allocator (the CLI's per-frame buffers), timers | C |
| 1.8 % | swresample sample copy (`conv_…FLT_to_…FLT`) | C |
| 1.2 % | `__udivmoddi4` (64-bit division in timestamp rescaling) | C (libgcc) |
| 0.8 % | `ff_vector_fmul_window_neon` (windowing) | NEON |

Mono at 48 kHz (no resampling): arithmetic decoder 27 %, cbrt chain 13 %, tx 14 %, allocator/memset ~9 %.
On x86 and arm64 the tx FFT/MDCT is SIMD already (nasm / NEON); the arithmetic decoder and the cbrt are C everywhere.

**Optimisation candidates — NOT done (Stuart decides).** Estimates are shares of the 0x05 decode above:

1. **cbrt → the table FFmpeg already has** (`ff_cbrt_tab`, used by the AAC-LC path): a few lines in
   `aacdec_usac.c`. ≈ −11 % of instructions, every architecture. Cheapest and largest per line of change.
2. **ARMv7 NEON for lavu/tx** (split-radix combine, the small fixed FFTs, MDCT pre/post rotation): ≈ −7 to −9 % on
   armhf only (2–3× on 12.6 %). Real assembly work; aarch64's NEON tx is the model.
3. **Arithmetic decoder tuning** (`ff_aac_ac_get_pk` table search, the context update): scalar only; perhaps −5 to −8 %.
4. **The resampler** is NEON already; the cheaper lever is not to use it — ask FFmpeg for the stream's own rate and
   resample with VibeServer's (as the FAAD2 path does), or a shorter swr filter. Up to −15 % on streams that are not
   48 kHz. That is our code, not an FFmpeg patch.

1–3 together: roughly a quarter fewer instructions on armhf. **Any of 1–3 is a modification of FFmpeg under the LGPL:
the patches must be published with the source at `source/ffmpeg/<version>/` and the notice updated** (see above).

Reproduce: the plugin, scripts and logs are in `~/VibeSDR-test-audio/usac/prof/` (`tbcount.c` — a qemu-user TB counter;
`prof.sh`, `prof2.sh` for the libc/libm load addresses; `agg2.py`), run in `vibeserver-ffmpeg:bookworm` against
`tools/ffmpeg-linux/bin/.debug/ffmpeg_g-armhf` (the unstripped build).

## Limits of FFmpeg 8.1's USAC decoder (honest)

- **eSBR** (harmonic SBR, PVC, inter-TES): refused. `usacFromDrm` now records it (`UsacInfo::esbr`), and the listener is
  told "This station uses xHE-AAC with enhanced SBR, which this server's FFmpeg (…) cannot decode yet" instead of
  silence. AudioToolbox (Mac) decodes it.
- **LPD core** (ACELP/TCX — speech): parsed, not decoded. It is chosen frame by frame, so it cannot be warned about from
  the config; such frames come out wrong.
- **Mps212**: parsed (the stream decodes), the parametric-stereo upmix is not applied.
- Fraunhofer's xHE-AAC encoders use eSBR and the LPD core at low rates, so **a real DRM xHE-AAC station may well not
  play on Linux** even with the bundled FFmpeg. If that proves so on air, the complete open decoder is **libxaac**
  (Ittiam, Android's own xHE-AAC decoder, Apache-2.0 — already listed in DRM-AUDIO-ROUTES.md), linked in process like
  FAAD2. Not started.
