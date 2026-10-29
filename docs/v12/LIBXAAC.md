# libxaac for xHE-AAC — and why FAAD2 stays (V12, 2026-10-29)

Stuart: *"if we can reduce the amount of libraries we include the better, FAAD2 Libxaac"*. We wanted libxaac (Ittiam /
the Android Open Source Project, Apache-2.0, `github.com/ittiam-systems/libxaac`, also AOSP `external/libxaac`) to replace
both the bundled FFmpeg and FAAD2. These are the measurements.

> ★★ **UPDATE, 2026-10-29 (branch `v12-no-ffmpeg`):** Stuart chose the dual approach — FAAD2 for DRM AAC and DAB+,
> libxaac for xHE-AAC — and VibeServer on Linux and the Mac no longer uses FFmpeg at all. The owner's DAB+ decoder choice
> (`dabAacDecoder`) is gone; the Mac decodes DAB+ with FAAD2 like Linux. Android keeps MediaCodec. The sections below
> that describe an FFmpeg option or the Mac's old DAB+ path are history.

## The answer

| Decoder | Replaced by libxaac? | Why |
|---|---|---|
| **FFmpeg (bundled, xHE-AAC)** | **Yes. Done on `v12-libxaac`.** | FFmpeg 8.1 produced garbage on every USAC stream with SBR (see below). libxaac decodes all of them. |
| **FAAD2, DRM build (DRM's own AAC)** | **No. It cannot.** | libxaac has no DRM AAC. DRM's AAC is ER AAC *Scalable* (object type 20) with mandatory HCR + VCB11, DRM's own CRC, SBR stored backwards at the end of the frame, and DRM's own parametric stereo (not MPEG PS). libxaac decodes ER AAC LC/LD/ELD with HCR and RVLC, but it has no scalable-layer decoding, no DRM framing and no DRM PS. Neither does any other free decoder we know of. |
| **FAAD2, DAB+ build** | **Not yet. Stuart decides.** | libxaac decodes DAB+ correctly: 960-sample frames (`frameLengthFlag`), SBR with 15 time slots, and PS. It costs more CPU than our patched FAAD2, though, and each of its two SBR paths has a catch (see "DAB+"). |

**What we bundle on Linux after this branch:** FAAD2 (`libfaad_drm.a` + `libfaad_dab.a`, one source, two builds) and
libxaac (`libxaacdec.a`). FFmpeg is gone from the packages, and (since `v12-no-ffmpeg`) from the server altogether.

## Correctness

Everything here is reproducible from `~/VibeSDR-test-audio/xaac/` (bench, data, results). Test streams are in
`~/VibeSDR-test-audio/usac/` and `usac/xhe/` (see the README there).

### xHE-AAC (USAC)

- **ISO conformance** (the 6 FFmpeg fate-suite streams that the bundled-FFmpeg work used, × 5 architectures = its "30"):
  libxaac at the stream's own rate matches the reference decodes to **±1.2 LSB16, SNR 66–89 dB** (the lower figures
  are the quieter streams; the references are 16-bit). Start-up delay is 2220 samples (2323 at 88.2 kHz).
- **The tools FFmpeg could not decode.** The conformance set has no SBR, eSBR, LPD or MPS, so we generated test
  streams with libxaac's own encoder. It is pinned to 64/96 kbit/s for USAC; we removed that clamp locally, for testing
  only. Sources were 20 s of BBC Radio 1 and an amateur SSB QSO. We decoded each stream three ways: libxaac, the
  bundled FFmpeg 8.1.3, and **macOS AudioToolbox**, the independent reference. The table gives the median SNR per
  2048-sample block against AudioToolbox, and the arm64 build vs the portable C build.

  | Stream | Tools | libxaac (arm64 NEON / C) | FFmpeg 8.1.3 (bundled) |
  |---|---|---|---|
  | fd_st64_noesbr | FD, stereo, 64 k | 97 / 77 dB | correct (135 dB) |
  | fd_mono16_sbr | FD + USAC SBR | 96 / 75 dB | **garbage**: peak 2.9×10⁴ FS, 30 % of the length |
  | fd_st24_mps | FD + SBR + MPS 2-1-2 | 92 / 76 dB | **garbage**: "Error parsing OTT CLD data", −3 dB SNR |
  | sw_mono16_esbr | switched FD/LPD + SBR | 80 / 72 dB | **garbage**: peak 9×10⁵ FS, NaN |
  | sw_speech12 | switched, speech | 97 / 70 dB | **garbage**: peak 1.9×10⁴ FS |
  | td_speech12 | ACELP/TCX only + SBR | 52 / 51 dB ¹ | **garbage**: peak 1.7×10⁵ FS |
  | sw_st24_mps | switched + MPS | 79 / 73 dB | **garbage**: −20 dB, 1/3 of the length |
  | fd_st32_sbr | FD + SBR, stereo | 27 / 27 dB ² | **garbage**: peak 2.2×10⁴ FS |
  | sw_mono16_pvc / sw_st20_ites | eSBR PVC / inter-TES | decodes (AudioToolbox refuses both) | refused ("Not yet implemented") |

  ¹ ACELP's recursive synthesis filters carry float rounding forward. Two libxaac builds (NEON vs C) differ from each
  other by the same 46 dB. Band energies match AudioToolbox within 0.06 dB on average.
  ² The right channel differs from AudioToolbox by about 25 dB in every band, and the left channel matches to 96 dB.
  Both decoders are **equally close to the source** (5.40 vs 5.44 dB SNR). The cause is unresolved, so the test
  reports it without gating on it.

  **Measured against the original source**, libxaac and AudioToolbox are indistinguishable on every stream (band-energy
  distance to the source within 0.02 dB of each other). FFmpeg 8.1's USAC decoder is only fit for FD streams without
  SBR. DRM's xHE-AAC runs at 12–24 kbit/s, which means SBR, and often the speech core, every time. ★ **So the bundled
  FFmpeg would most likely have played garbage on a real xHE-AAC station rather than refusing it.** Only the eSBR
  refusal was caught (from the config). Plain USAC SBR passed straight through.
- **Real DRM signals:** no xHE-AAC station has been receivable. `uber_rri_9570000_iq48` is DRM AAC (mono, 12 kHz, no
  SBR). `uber_rri_9720000` and the 5970 kHz French recording never reach an audio configuration in our receiver. So
  **no eSBR or LPD frames from a real signal exist yet**. That remains the open test (DRM.md "next").
- **Across architectures** (`xaacbench` under qemu): amd64, i386 and ARMv6 (all the portable C path) are bit-identical
  to each other. armhf (NEON) is within 1 LSB of arm64. The C path differs from the assembly paths by 76 dB on SBR
  streams, from float rounding in the eSBR code.

### DAB+ (HE-AAC v2, 960) — libxaac can, but it is not a clear win

The real off-air unit logs were 11D ×4, 12B ×3, 10D ×2 and a simulated 8 dB SNR log, all 32 kbit/s HE-AAC v2 with PS.
libxaac decoded every unit with 0 errors and the correct length (1920 samples a unit at 32 kHz).

- PS + SBR use random noise and differ between implementations, so waveform SNR between *any* two decoders is
  low: libxaac↔FAAD2 15–33 dB, FFmpeg↔FAAD2 6–14 dB. (FFmpeg here means FFmpeg 9.0.1 fed the 960 ASC through
  Matroska. Our ADTS path cannot signal 960.)
- In a three-way comparison of third-octave band energies, **libxaac's eSBR path is the consensus**: it is 0.4–0.6 dB
  rms from FFmpeg, while FAAD2 is 1.0–1.2 dB rms from both and runs +0.18 dB hot below 4 kHz. The core (non-PS)
  output matches FAAD2 to 73 dB where the programme is near-mono.
- ★★ **Its catch: overload wraps.** When the programme is above 0 dBFS (FAAD2 shows peaks up to +1.14 FS on 11D-C1C0),
  the libxaac eSBR/PS path **wraps to the opposite full scale on every build except arm64**. That is 29 samples a minute
  on that station, each one a loud click. arm64 saturates correctly. UBSan finds no signed overflow or implicit
  truncation in the PS/QMF files, so the cause is not pinned down yet. This needs a patch before libxaac could decode
  DAB+.
- **The other path** (`ESBR=0`, libxaac's legacy low-power SBR) is bit-identical on armhf and arm64 and never wraps.
  But it runs a peak limiter (peaks 0.75 FS where FAAD2 gives 1.14), sits about 0.3 dB low, and is further from the
  consensus (0.6–1.4 dB).

## CPU — the Pi 2 (Cortex-A7, 900 MHz), measured

Standalone static binaries ran in a scratch directory under `nice -n 19`. vibeserver was not touched, and the
scratch directories have been removed. Figures are **% of one core** for decoding alone, at the stream's own rate,
user+sys. FFmpeg is the bundled CLI, with process start included.

| Stream | libxaac armhf (NEON asm) | libxaac armhf C only | libxaac ARMv6 build ³ | FAAD2 (ours, patched) | FAAD2 stock | FFmpeg 8.1.3 bundled |
|---|---|---|---|---|---|---|
| xHE Fd_1 mono 48 k (no SBR) | 3.5 | 4.5 | 4.6 | — | — | 3.2 (3.5 at 48 k st.) |
| xHE Fd_2 0x05 stereo 32 k | 4.5 | 5.7 | 6.1 | — | — | 3.6 (5.5 → 48 k) |
| xHE mono 16 k SBR + LPD | **5.8** | 8.2 | 8.5 | — | — | (garbage) |
| xHE ACELP 12 k + SBR | 6.0 | 8.4 | 8.8 | — | — | (garbage) |
| xHE stereo 32 k SBR | 8.9 | 13.3 | 14.2 | — | — | (garbage) |
| xHE stereo 24 k SBR + MPS | 14.2 | 16.3 | 17.3 | — | — | (garbage) |
| DAB+ HE-AACv2 (12B / 11D), eSBR path | 8.8 | 10.8 | 11.3 | **2.9 / 3.0** | 8.9 ⁴ | 6.6–6.9 (ADTS → 48 k) |
| DAB+ HE-AACv2, legacy SBR path | **3.9–4.0** | 8.5–8.7 | 9.3–9.4 | — | — | — |
| DRM AAC (RRI 9570, mono 12 k) | n/a | n/a | n/a | — | 1.46 (armhf) / 1.25 (armv6) | n/a |

³ The ARMv6 binary was timed on the Pi 2's Cortex-A7, not on an ARM1176. ⁴ The stock armhf FAAD2 is GCC with
`-ffloat-store`, which is what the shipping **DRM** build still uses. That flag alone makes it about 2× slower than
the same code built with clang on ARMv6 (4.3 %). Patch 0001 drops it for the DAB+ build only. Side finding: applying
just patch 0001 to the DRM build would roughly halve DRM AAC's CPU on armhf.

x86 was not timed (qemu only). On instruction counts the C path costs about 1.8× the armhf NEON path (i386 DAB eSBR
86 M instr/s against armhf 47).

## Profile — where libxaac's time goes (qemu Cortex-A7, guest instructions per function)

| Path (armhf) | M instr / s of audio | Top functions (share) |
|---|---|---|
| DAB+ eSBR path | 47.1 (FAAD2 patched: 13.3) | `esbr_ps_de_correlate` 10.9 % C · libm `pow` (shown as cbrtl/scalbnl/frexpl; SBR envelope dequantisation calls `pow()` per value, `env_dec.c`) ~12 % C · `RADIX4_OUTLOOP`/`LOOP1`/`S_WITH_R_L5` 10 % **ARM asm (integer, not NEON)** · `fft_32_points` 4 % C · `hbe_post_anal_prod3` 4 % C (the *harmonic transposer* running on a stream that does not use harmonic SBR) · `esbr_ps_apply_rotation` 3.7 % C · `sbr_env_calc` 3.5 % C |
| DAB+ legacy path | 26.5 | `fft_32_points` 7 % · `filterstep3` 5.6 % · `ld_dec_fft_15_opt` 5.3 % · armv7 asm loops (incl. NEON `DE_COR_FIL2`) ~20 % |
| xHE mono SBR + LPD | 29.3 | `arth_decoding_level2` 8 % C · asm radix-4 6 % · `bass_post_filter` 5.5 % C · `sbr_env_calc` 4.7 % · memcpy/memset 8 % · `residual_tool_float` 3.7 % |
| xHE stereo MPS | 70.7 | `mps_decor_apply` 9 % C · `mps_synt_out_calc_dec` 8.4 % C · arithmetic decoder 6 % · `mps_synth_calc_fft` 4.9 % |
| xHE FD, no SBR | 28.9 | `arth_decoding_level2` 19 % · **libgcc `__udivmoddi4` 14 %** (a 64-bit division per coefficient in the arithmetic decoder) · `ac_spectral_data` 13.6 % |
| i386, xHE SBR + LPD | 70.7 | `esbr_radix4bfly` 26 % C · `complex_fft_p2_dec` 17 % C — **no SIMD at all** |

**Hand-written SIMD in libxaac, and whether it runs on our builds:**

- **armhf:** `decoder/armv7` has 61 `.s` files: **28 NEON, 33 integer ARM-DSP**. Our build compiles them and they
  run (their labels are in the profile). They cut the Pi 2's CPU by 20–33 % against the C path, and by up to 55 % on
  DAB+ legacy. ★ They are UAL syntax: GNU as rejects them (`subsne`), so `build-libxaac.sh` assembles them with
  clang. They also lack `.note.GNU-stack`, so we pass `-Wa,--noexecstack`. Without it, the whole vibeserver would get
  an executable stack.
- **arm64:** `decoder/armv8` has 20 NEON `.s` files, built and running.
- **ARMv6, i386, amd64, Mac:** **none.** libxaac has no x86 SIMD, and the ARMv7 asm needs ARMv7/NEON. These run the
  C path. i386 gets only the compiler's SSE2 (our floor).
- ★ libxaac's `decoder/generic` function selector has drifted. Its function-pointer prototypes no longer match the
  functions (WORD16 outputs where the functions write WORD32): clang refuses it, while GCC 12 and clang 14 only warn.
  It also expects the armv7 asm whenever `__ARM_NEON__` is defined, which the Mac's compiler defines. We use the
  x86_64 selector (plain C) with `__ARM_NEON__` undefined for every C build, and `-Werror=incompatible-pointer-types`.
  This is worth reporting upstream.

**Headroom, armhf, if we patched it (estimates from the profile; not built):**
1. Replace the 64-bit division in the arithmetic decoder (`__udivmoddi4`): about −10 to −14 % on every xHE stream.
2. A table or `exp2f` for SBR envelope `pow()`: about −10 % (DAB+ eSBR), less on xHE.
3. Skip the harmonic transposer's analysis when the stream has no harmonic SBR: −4 to −8 % (DAB+ eSBR path).
4. NEON for `ps_de_correlate`, `ps_apply_rotation`, `fft_32_points`, `sbr_env_calc`: −15 to −20 % on PS streams.
5. Per-frame memset/memcpy of large buffers (8 % in xHE SBR): partly avoidable.

Together that is roughly **25–40 % fewer instructions for SBR streams**. For DAB+ even that leaves the eSBR path at
about 2× our patched FAAD2. The legacy path, at 1.3–1.4× on armhf, is the nearer one, but it needs its limiter
question settled.

**Would a local patch series hold up?** Well. The hot DSP files (`ixheaacd_ps_dec.c`, `esbr_polyphase.c`,
`sbr_dec.c`, `hbe_trans.c`, `arith_dec.c`, `env_calc.c`, `qmf_dec*.c`) have had **no upstream commits since
2024**. Upstream work is fuzz and parsing fixes (`api.c`, MPS, DRC). Releases come once or twice a year (0.1.12
2024-12, 0.1.13 2026-01, 0.1.14 2026-10). Patches would live in `vibeserver/linux/libxaac-patches/` with a `series`
file, like faad2-patches. **Apache-2.0 §4(b) requires every modified file to carry a prominent notice that we
changed it**, so each patch adds a "Modified by VibeSDR, <date>: <what>" line to the file's header. §4(c) requires us
to keep the existing notices, and §4(d) to ship NOTICE.

## Packaging (this branch)

- **Pinned:** libxaac **v0.1.14** = commit `2fbadd57bca46693b7a077bcf1513131106d960c` (2026-10-01). Tarball
  `https://github.com/ittiam-systems/libxaac/archive/refs/tags/v0.1.14.tar.gz`, **SHA-256
  `f467a6e5bcf3941ce5d1a4d811442835fba7eb3744ae986f3ea37e110584b611`**. Unmodified.
- **Built by** `vibeserver/linux/build-libxaac.sh`, called by `linux/build-deps.sh` (every profile) and
  `mac/build-deps.sh`. It builds the decoder only, statically and in process. The source list is read from libxaac's
  own cmake lists. Variants:
  - `armv7` for armhf: NEON asm, assembled by clang.
  - `armv8` for arm64.
  - `c` for amd64, i386, ARMv6 and the Mac.

  The checks: the entry point is present, every object carries `.note.GNU-stack`, and (from build-deps) every object
  is the right machine and ARMv6 where it must be. Verified on all five Linux targets in the bookworm cross image. The
  linked binary's stack is `RW`.
- **Size added to the vibeserver binary** (text+data+bss, static): armhf 1.84 MB, arm64 1.83 MB, armv6 1.64 MB,
  i386 2.15 MB, amd64 1.94 MB. The FFmpeg it replaces was a separate 1.7–2.4 MB program per arch, two of them in the
  armhf package. The libxaac archive itself is 2.1–2.8 MB.
- **Mac VibeServer:** it links libxaac too (the `c` path, arm64; the Mac build is arm64-only). LICENSE/NOTICE go into
  `Contents/Resources`. **xHE-AAC on the Mac is now libxaac**, and AudioToolbox / Homebrew FFmpeg remain only for
  builds without it. ★ Found while testing: `DrmAppleUsac` hands AudioToolbox the bare AudioSpecificConfig as its
  magic cookie, and in a standalone test making the same calls **AudioToolbox refuses it** (`'!dat'`). Wrapped in an `esds` ES_Descriptor it decodes fine. So
  the Mac's AudioToolbox xHE path never worked; it silently fell back to Homebrew FFmpeg, if present. With libxaac the
  path is no longer used, and fixing it is not needed now. **Mac DAB+ is FAAD2 since `v12-no-ffmpeg`**, as on
  Linux (vibe_dab_aacdec.h); AudioToolbox remains only for an Apple build without FAAD2 (the iOS app, a Mac dev build).
  Given a 960-sample ASC, AudioToolbox refuses it in the same test, which is why that path decodes DAB+ as 1024.
- **Android:** unchanged. libxaac does not fix the silent DRM AAC on Android: that needs `libfaad_drm.a` for the NDK,
  which is a separate job. xHE-AAC there is MediaCodec, which on Android 10+ *is* libxaac (`c2.android.xaac.decoder`).
  Linking libxaac into the Android server would only add xHE-AAC on Android 5–9.
- **iOS:** untouched. The app links no decoder.
- **Licence:** Apache-2.0 is compatible with GPL-3.0. We ship `LICENSE.libxaac` and `NOTICE.libxaac` (libxaac's
  `LICENSE` and `NOTICE`) in `/usr/share/doc/vibeserver` and the Mac app's Resources. Credits are in the app's About,
  the web client's About and the README. No source has to be published (that was FFmpeg's LGPL obligation), so
  `publish-apt-remote.sh` step 6b is gone.
- **Patents (flagged, unchanged):** shipping an xHE-AAC decoder makes VibeSDR its distributor. Via LA's pools cover
  AAC and xHE-AAC. This was already true of the bundled FFmpeg. Stuart's call.

## Keeping the pin current

libxaac's security fixes arrive in three places:
- Upstream commits ("Fix for … in ixheaacd_…", from OSS-Fuzz: 34 of 50 commits since 2025-01).
- Tags (yearly).
- The Android Security Bulletin (AOSP `external/libxaac`), which has carried many libxaac CVEs.

Suggested routine: check the ASB monthly and upstream `main` at every VibeServer release. If a fix lands on `main`
before a tag, pin the commit (GitHub archive tarball by SHA, the same `fetch` + SHA-256 as today) rather than wait.

## Config: how existing settings map

- `dabAacDecoder` (and the older `dabFaad2` bool) — **removed in `v12-no-ffmpeg`.** FAAD2 is the only DAB+ decoder on
  the Linux and Mac servers, so there is nothing to choose. A config that still carries either key loads cleanly; the
  key is ignored and dropped at the next save (`scripts/test-setup-dab-faad2.mjs`).
- No new key: xHE-AAC has one decoder on server builds.

## Tests

- **`vibeserver/test-drm-xaac.cpp`** (in run-tests.sh, which needs a deps prefix with `libxaacdec.a`) runs the
  conformance and `usac/xhe` comparisons above, the server's 48 kHz path (duration, finite output, ≤ +6 dBFS) and
  concealment (1 unit in 7 failing CRC). **85/85 on the Mac (C path) and on Linux arm64 (NEON path).**
- `test-drm-faad` now builds the DRM decoder door with FAAD2 **and** libxaac, as the server does. It passes, with
  RRI 9570 off air.
- Also passing: `test-drm` 134/134 (incl. real recordings), `test-drm-signal` 109/109, `test-dab-faad` 53/53,
  `test-dab-faad-queue` 21/21 (incl. the live switch), `test-ffmpeg-path`.
- Not exercised: a real DRM xHE-AAC signal end to end (none receivable). `DrmAudioDecoder` hands libxaac the same ASC
  (`usacFromDrm`) that FFmpeg's Matroska header carried.

## For Stuart to decide

1. **DAB+ on libxaac** (it would drop `libfaad_dab.a` and the FAAD2 patch series, but FAAD2 stays for DRM either
   way). You can have the eSBR path, which is the consensus quality but costs 3× our patched FAAD2 on the Pi 2 and
   must have its overload wrap fixed first. Or the legacy path, at 1.35× FAAD2 on armhf (3× on ARMv6 and x86), with
   its peak limiter. Or keep FAAD2 (recommended for now).
2. Whether to carry a **libxaac patch series** for the Pi 2: arithmetic-decoder division, `pow` tables, HBE skip,
   NEON for PS.
3. ~~Whether the owner's **FFmpeg DAB+ option** should stay at all on Linux~~ — decided 2026-10-29: removed, with every
   other FFmpeg use (`v12-no-ffmpeg`).
4. Patents (above).
