# FreeDV digital voice as a mode (V12)

FreeDV is a mode in the same list as AM, FM, USB and LSB. It is not a decoder window. The server demodulates SSB as it
always does. That audio goes through a FreeDV modem and vocoder — **RADE V1** (FreeDV's own receiver, compiled in; see
"RADE V1" below) or **700D / 700E / 1600** (the system's Codec 2) — and the decoded speech replaces it. The mode is silent
until there is speech to play. The VTS line explains the silence:

| What the listener sees | When |
|---|---|
| `FreeDV · searching` | AUTO, nothing decoded yet |
| `FreeDV 1600 · searching` | a sub-mode chosen by hand, nothing decoded |
| `FreeDV LSB · searching`, `FreeDV 700E USB · searching` | a sideband (and a sub-mode) chosen by hand, nothing decoded |
| `FreeDV 700D · 6 dB · G4ABC` | in sync: the sub-mode on air, the modem's SNR, the station's text |
| `FreeDV RADE · 4 dB` | in sync on RADE (its callsign comes only at the end of the over) |
| `FreeDV · last G4ABC` | a RADE over has ended: its end-of-over callsign, in place of "searching" |

The lines are kept short because the R36S screen is small.

## Sub-modes, AUTO and the manual fallback

- **Sub-modes:** RADE (V1), 700D, 700E and 1600 — RADE first in every list, because it is nearly all of the traffic (see
  "What is on air today" below). RADE runs without libcodec2; the other three need it. RADE V2 is not decoded.
- **AUTO:** runs every demodulator this server has (up to four) while it searches.
  - **A win:** a demodulator must hold sync for one second **and** have decoded more frames that pass the FEC than fail
    it (`freedv_get_rx_status`). Ties go to the cleaner, then the stronger. The other two are then closed, which gives
    back their memory and CPU.
  - **Giving up:** after 8 s with no clean frame, AUTO searches all three again. A winner that decodes no clean frame for
    1 s within 4 s of winning goes back to the search at once ("probation").
  - **A new station** (the dial moves more than 50 Hz) gets a new search at once.
  - **Why frames and not sync alone** (2026-11-16, an intermittent test-server-freedv failure, about 1 run in 4): an OFDM
    demodulator can hold "sync" on a signal that is not its own. 700E won on the 1600 station at about 1 dB with every
    frame failing its FEC, and closed the 1600 demodulator that was about to win. 700D held sync for 2.9 s on a 1600
    signal after its own station stopped. The false sync also flickered in and out, and each flicker restarted the old
    "8 s without sync" clock, so it never gave up. The listener heard silence. Loss is therefore measured as time since
    the last clean frame, which a flicker does not reset.
- **`nin == 0`** (found while chasing the same failure): the OFDM timing loop sometimes asks for no samples at all. The
  receiver used to take 0 as "nothing to do" and never called the modem again, so that demodulator was deaf for good.
  On 700E, a noise lead-in of 0.37 s or 2 s before the station wedged it every time. `freedv_rx` is now called with
  `nin` 0 (it consumes nothing and moves the state on), at most a few times per block.
- **Diagnostics:** `VIBE_FREEDV_LOG=1` prints AUTO's decisions to stderr: wins, losses, probation and re-searches, with
  the sync time, SNR and clean/failed frame counts.
- **Manual choice:** runs that one sub-mode only. It is the fallback for when AUTO cannot settle. This follows the house
  rule that auto decoders keep manual controls underneath them.
- **Held sync:** a sync is reported and played only after it has held for 0.5 s. When 1600 was chosen by hand on a 700D
  station, its demodulator flickered into sync for a frame or two. Without the hold, that showed a "sync" on the VTS for a
  signal it could not decode.
- **Text:** the varicode text channel (usually the callsign) is shown only once the same line has arrived twice (matched
  against the last four lines, so one garbled line in between does not hold it back). Plain varicode has no check, and
  noise reads as text. A line takes about 7 s, so the text shows 6–16 s after sync on a clean signal.

## Sideband

FreeDV keeps the SSB convention: LSB below 10 MHz, USB from 10 MHz up **and on 60 m** (5.25–5.45 MHz). That is the
FreeDV application's own rule (freedv-gui `isUsbFreq = f >= 10 MHz || is60MeterBand`) and freedv.org's getting-started
page. A modem signal on the wrong sideband is inverted and never syncs. So the server picks the sideband from the dial and
reports it:

- **Per listener:** `clientRetune`, which runs on every tune.
- **Shared demodulator:** `rxModeDial()`, which every `rx.setTune` call now uses.
- **DC placement:** `mainDcZone` applies the same rule, so the tuner's DC is kept off the side actually heard. Without
  this, 700E on 40 m LSB failed on a one-pipeline radio.

Clients draw the passband on the side the report gives (`"sb"`). They never decide it themselves.

### The sideband override (Stuart, 2026-11-16)

**FREEDV SIDEBAND — AUTO / USB / LSB**, under the sub-mode keys (FREEDV MODE) on the web page, in the app's mode sheet
and on the R36S. AUTO is the convention above; USB or LSB demodulates that side whatever the band, for a station or a net
that does not follow it (and for FreeDV 3.0, which moves to USB everywhere — see "What is on air" below).

- **Whose choice:** the same as the sub-mode's. On a range-locked radio it is the listener's own and is kept across
  retunes (`ClientDsp::fdvSb`; a change re-runs `clientRetune`, so the channel flips side at once). On a one-pipeline
  radio or a shared dial it is the room's (`fdvSharedSb`, read by `rxModeDial()` and `mainDcZone`), and asks before tuning
  like the sub-mode does (`freedv_sub` is in the gated list).
- **A new receiver** (the dial moved more than 50 Hz) is built with the choice already set, so its first report never
  says AUTO for a listener who chose USB.
- **The report** carries both: `"sbSel"` (what was chosen) and `"sb"` (what is demodulated). The keys are lit from
  `sbSel`, never from the tap. An older server sends no `sbSel`; clients read that as AUTO.

## What is on air today, and why Stuart has never decoded FreeDV off air (2026-11-16)

**Short answer: almost everybody on FreeDV now transmits RADE, which we do not decode.** 700D, 700E and 1600 — the three
modes this receiver runs — have become legacy modes that hardly anyone sends.

**Evidence**

- **FreeDV Reporter** (qso.freedv.org; it shows each station's mode). Two view-only snapshots on 2026-11-16:
  - First: 67 stations connected. Transmit mode was RADEV1 for 58, RADEV2 for 6, analogue LSB for 1. **None were on 700D,
    700E or 1600.**
  - Second, at 16:55 UTC: 73 stations. RADEV1 62, RADEV2 7, LSB 2. Again none on 700D, 700E or 1600. Receive reports:
    RADEV1 130, RADEV2 28.
  - Busiest frequencies: 14.236 (19 stations), 7.177, 7.065, 7.160, 7.115, 3.603 (RADEV2), 7.197.
  - Caveat: Reporter sees only software users who have reporting switched on. SM1000 and ezDV hardware users, who can
    only run 700D, may not appear.
- **The FreeDV application** (freedv-gui 2.4.x, its USER_MANUAL): RADEV1 is the default. 1600, 700D and 700E are
  "legacy" modes, hidden unless "Enable Legacy Modes" is ticked; 700C is deprecated. freedv.org keeps 700D "for
  compatibility with hardware such as EzDV and SM1000 which cannot run RADE".
- **FreeDV 3.0** (in development, branch `v3.0-dev`, tag `v3.0.0-dev1`): the June 2026 developer update says it removes
  700D/E and 1600 and moves the official convention to **USB on every band**. The sideband override above covers that
  for RADE-era stations that still use the legacy modes.
- No current net was found that runs 700D or 700E specifically. The UK nets listed on freedv.org (3.640 / 3.643 LSB,
  5.3685 USB, Sunday 0900 and daily 1600 local) do not name a mode.

**RADE, in facts** (as researched that afternoon, before it was built — see "RADE V1" below for what was built and measured)

| | |
|---|---|
| What it is | "Radio Autoencoder": a neural network maps speech features straight to OFDM symbols and back. Rowe and Valin, arXiv:2505.06671 |
| Licence | **BSD-2-Clause** — the Python reference (github.com/drowe67/radae) and the C port (github.com/freedv/rade_c), which the README says to use for deployment |
| Runtime | rade_c is pure C, no Python or PyTorch. It uses Opus's DNN `nnet` code and the **FARGAN** vocoder from Opus (BSD); its build fetches and builds Opus |
| Weights | compiled in as C arrays: about 1 M weights each for encoder and decoder (paper), ~4–5 MB each as float32 (estimated from the generated sources' size); V2 adds larger ones and an ML sync network |
| CPU | paper: 32 MMAC/s each for encoder and decoder, ~300 MMAC/s for FARGAN — "real time on a laptop CPU". On a Pi 2 this needs measuring before it is offered |
| Waveform (V1) | OFDM, 8 kHz modem rate, 30 carriers at 50 baud (20 ms + 4 ms cyclic prefix), 120 ms frames with pilots; ~1.5 kHz (paper) or ~2.1 kHz 99 % bandwidth (README) |
| V2 | 14 carriers, ~860 Hz, 40 ms frames, ML sync. The README says on-air use is not yet recommended and the waveform, weights and API may change — yet it is already on Reporter |
| Specification | **none published** beyond the V1 paper and the reference code; a V2 paper is "planned" |

**Off air, 2026-11-16 16:55–17:05 UTC** (Stuart's UberSDR, standing permission; ten minutes each, recorded at once with
`tools/ubersdr-rec`, kept in `~/VibeSDR-test-audio/freedv/` with a README): 14.236 USB, 7.177 LSB, 7.160 LSB, 7.065 LSB,
3.643 LSB and 5.3685 USB, while Reporter showed 69 of 73 stations on RADE and none on the legacy modes. Replayed through
our receiver (`test-freedv wav`, AUTO, `VIBE_FREEDV_LOG=1`):

- **No sync on any recording** — not one held sync, not one frame passed the FEC, no text, no speech.
- There **were** signals: on 7.065 (five RADEV1 stations on Reporter) the 0.7–2.2 kHz band stood 6 dB over its edges
  for 22 % of the recording, on 3.643 for 28 % (1.2–2.0 kHz), on 7.177 for 7 %. 20 m and 60 m were empty from the UK
  at that hour.
- So the honest reading: what was on air was (by Reporter's count) RADE, and our receiver correctly found nothing it
  knows. It is not proof our 700D path works off air — that still needs a 700D transmission (an SM1000 / ezDV user, or a
  scheduled test). It does say why Stuart has never heard FreeDV decode: there is almost no 700D/700E/1600 left to hear.

**Recommendation**

1. Keep 700D/700E/1600 as they are: they cost nothing when nobody sends them, and the hardware users (SM1000, ezDV) can
   only send 700D.
2. ✓ **Done the same evening — see "RADE V1" below.** RADE V1 is the mode worth adding, and its licence allows it: link `rade_c` (BSD-2) — the whole receiver, weights
   and FARGAN — rather than reimplementing it. There is no published standard to write it from; the reference *is* the
   definition, as Codec 2 is for 700D. As with libcodec2, prefer loading a system or bundled library to vendoring
   source, and measure the Pi 2 first: FARGAN at ~300 MMAC/s per listener is far heavier than the 700D modem (0.6 % of an
   M5 core), so it may need to be one shared receiver, or benchmark-gated like the other heavy decoders.
3. Wait for RADE V2 to settle before adding it (its own README says so).
4. ✓ Superseded: the hint now names RADE first ("AUTO finds RADE (what most stations send), 700D, 700E or 1600 —
   whichever this server runs").

## RADE V1 (built 2026-11-16, the evening after the findings above)

RADE V1 is a fourth sub-mode, first in every list. It is FreeDV's own receiver, compiled into the server. It needs no
libcodec2, so a server without that library still offers FreeDV, with RADE alone.

### Where the code comes from, and its licences

Nothing of upstream is vendored in this tree. Both `build-deps.sh` scripts (Linux and Mac) fetch pinned tarballs, check
their SHA-256, and build them with **`vibeserver/linux/build-rade.sh`**. Their licences ship beside the binary: the
deb's doc dir and the Mac app's Resources (`LICENSE.rade_c`, `COPYING.opus`, `LICENSE.freedv-backend`).

| Piece | Upstream, pin | Licence | Used for |
|---|---|---|---|
| **rade_c** | github.com/freedv/rade_c @ `77980d6eb02f16e3f59a60243b4d7578b364b67e` (main, 2026-11-16), archive sha256 `01f16e51…3dfaf` | **BSD-2-Clause** (Peter B Marks; `rade_api.h` © David Rowe, BSD) | The V1 modem, acquisition, neural decoder. Its weights are generated C arrays in the same repository, under the same licence (no separate licence for them; the radae training repository is BSD-2 too). |
| **Opus 1.6.1** | downloads.xiph.org, the tarball already pinned for libopus (sha256 `6ffcb593…411a1`) | **BSD-3-Clause** | DNN layer code (`nnet`, int8 GEMV), **FARGAN** vocoder and its weights (generated C in the release tarball, under Opus's COPYING); the tests' feature extractor |
| **freedv-backend** | github.com/freedv/freedv-backend @ `49006b029853061ca4e992f9632a51875aa3ecf1` (2026-11-12), archive sha256 `d8d8e158…0d7cc7` | **BSD-2-Clause** (Mooneer Salem) | The end-of-over callsign: `rade_text.cpp`, `ldpc_decode.cpp`, `ldpc_encode.cpp`, `HRA_56_56.h` (CRC-8, LDPC(112,56), interleaver) |

No GPL or LGPL code is involved, and there are no patents to declare beyond Opus's royalty-free grant.

**Our own files** are in `vibeserver/linux/rade/`, beside the builder, so the Linux build images can COPY them:

- `vibe_rade.h` / `.c`: a small C API for the receiver.
- `vibe_rade_tx.c`: the transmitter, for the tests and the benchmark.
- `vibe_rade_acq.c`: the vectorised pilot search (see "Cost" below).

**Why build it ourselves, and not use a system library:** no distribution packages rade_c, and freedv-gui builds its own
copy too.

**Why not rade_c's own Opus.** rade_c pins Opus commit `940d4e5a` and its 183 MB model tarball (`4ed9445b`). We use Opus
1.6.1, the one Opus the project already pins. On 5 minutes of off-air RADE (rade_c's own `FDV_offair.wav`), decoding
through 1.6.1's FARGAN is **bit-identical** to decoding through `940d4e5a` + `4ed9445b`. Measured 2026-11-16.

**The one change to Opus.** RADE V1's decoder has convolution layers with 1536 inputs. Opus 1.6.1 sizes
`compute_generic_conv1d`'s stack buffer at 1024 floats, so it overran. ASan caught it: memcpy-param-overlap, then an
abort. `build-rade.sh` compiles a copy of `nnet.c` with `MAX_CONV_INPUTS_ALL 2048`, the same change rade_c makes to the
Opus it pins. The script checks that the line changed and refuses to build if it did not.

### How it is built, and kept apart from the server's libopus

`build-rade.sh` makes two archives:

- **`librade_vibe.a`**, the receiver. The server links it.
- **`librade_vibe_tx.a`**, rade_c's transmitter. The tests use it, and the benchmark's dec_rade row uses it for its
  decoding measurement.

Each archive is a single partial-linked object, and its only global symbols are `vrade_*`. So its private copy of Opus's
`nnet` and FARGAN cannot collide with the `libopus.a` the server links for its own audio. The isolation is done
differently on each platform:

- **ELF:** `ld -r --force-group-allocation`, then `objcopy --keep-global-symbols`. GNU ld does this even for ARMv6,
  whose clang uses lld 14, which lacks the option. Dissolving the COMDAT groups matters. Otherwise the server's own
  `std::vector<float>` instantiations could make the linker throw ours away. The symbols that pointed at our copy would
  then point at a discarded section; that failure was seen once.
- **Mach-O:** `-exported_symbols_list`.

The script refuses an archive that defines any other global, or that leaves an Opus or RADE symbol undefined.

**Builds checked:** macOS arm64 (Apple clang); Linux arm64 (bookworm image, also linked against `libopus.a` with
`--whole-archive`, no clash); armhf `-marm -mfpu=neon-vfpv4`; ARMv6 (clang + Raspbian sysroot); x86_64; and i686
`-msse2`. The x86_64 and i686 builds were cross-built and run under qemu: both decoded the same signal, with the same
frames and callsign.

CMake defines `VIBE_HAVE_RADE` when it finds the archive:

- **Release builds:** the deps prefix. A missing archive is a configure error unless `-DVIBE_RADE=OFF`.
- **Dev builds:** `VIBE_RADE_PREFIX`, or the Mac deps prefix.
- **Windows:** not built yet.

### In the mode

- **AUTO** opens RADE beside the codec2 trio.
  - **Sync:** RADE's own receiver decides it. It needs four pilot detections in a row (~0.5 s), so the 0.5 s
    "held sync" rule for codec2's modes does not apply.
  - **Clean frames:** a RADE frame counts as clean when its unique word (the auxiliary bits the decoder returns) has no
    errors. RADE has no FEC result to report, and the unique word is what its own receiver uses to drop a false sync.
  - **Winning:** the rules are the same as for the other sub-modes (one second of sync, more clean frames than failed).
- **Speech** is FARGAN's 16 kHz output, resampled to 48 kHz. It goes into the same bounded queues on the same
  `vibe-freedv` thread.
- **The callsign** travels in RADE's **end-of-over** frame (FreeDV's reliable text: CRC-8 + LDPC). It therefore names
  the station that has **just stopped**:
  - The report carries it as `"last"`, and keeps it while the receiver is on that dial.
  - Clients show it only while searching, in place of "searching": `FreeDV · last G4ABC`. It is never shown beside a
    station that is talking, which may be the other side of the QSO.
- **The sideband** follows the same convention. freedv-gui 2.4.x, the current release, applies its SSB rule
  (`isUsbFreq`) to RADE too. The 40 m RADE stations decoded below were all on LSB, as the convention says. FreeDV 3.0
  (in development) moves to USB everywhere, and the sideband override already covers that.
- **The owner's switch:** "FreeDV: RADE V1", under FreeDV on the setup page (`"rade"` in the blocked-modes list).
  Off, RADE is not offered in `freedvSubs`, AUTO does not hunt it, and a manual RADE is not applied.
- **The benchmark row `dec_rade`** measures the dearer of RADE **searching** (band noise) and RADE **decoding** (a real
  over from rade_c's own transmitter, at 10 dB). Both are fed in real time.
  - When the row is not GREEN, `"rade"` starts OFF on a fresh setup. The codec2 sub-modes are untouched, and the owner
    can tick RADE back on.
  - `dec_freedv` now measures the codec2 trio alone (`withRade = false`).
  - **M5 Pro:** searching 2.6 %, decoding 2.0 %, 29 speech blocks decoded (test-bench-decoders, 2026-11-16).

### Cost

Instruction counts are per second of audio. The method is `PI2-REAL-CPU.md` / `AVIATION.md`'s: static builds, counted
under qemu with the TCG instruction-count plugin. "Searching" is 30 s of noise. "Decoding" is 52 s of real speech
(rade_c's `wav/all.wav`) at 10 dB, including 1.4 s of search before sync. "Decoder alone" is the neural decoder plus
FARGAN on synthetic latents (`vrade_bench_decode`).

| Build | Searching | Decoding | Decoder alone |
|---|---|---|---|
| armhf (Pi 2), rade_c's search | 1035 M/s | 395 M/s | 249 M/s |
| **armhf (Pi 2), ours** | **244 M/s** | **312 M/s** | 249 M/s |
| arm64, rade_c's search | 338 M/s | 286 M/s | 167 M/s |
| **arm64, ours** | **223 M/s** | **234 M/s** | 167 M/s |
| ARMv6 (scalar, rade_c's search) | 1859 M/s | 2158 M/s | 2040 M/s |

- **Where the time goes:** in sync, 63 % of the armhf count is Opus's int8 GEMV (`cgemv8x4`, FARGAN and the decoder),
  which is already NEON.
- **The search** is rade_c's brute-force pilot correlation: 960 timings × 40 frequencies × 160 samples, twice, every
  120 ms. That is ~100 M complex multiply-adds a second. It cost **more than decoding** on every platform.
- **`vibe_rade_acq.c` is our reorganisation of that search.** The arithmetic is the same; only the loop order changes,
  with eight frequency bins summed in registers. SSE2 is left to the compiler. NEON is written by hand: GCC vectorised
  neither 32-bit NEON (no IEEE floats there) nor AArch64's loop. On AArch64 it uses the fused `fmla`. The in-sync grid
  refresh (5 % a frame) uses the same kernel.
- **Effect of the search change:**
  - M5: 3.6 % → 1.3 % of a core.
  - armhf: 1035 → 244 M/s.
  - On the Mac, the 5-minute off-air decode is bit-identical before and after.

| Machine | Estimate for one RADE listener | Basis |
|---|---|---|
| Mac M5 Pro | **1–2.6 % of a core** | measured (rt harness 1.1–1.3 %; test-freedv 1.1 %; benchmark row 2.6 % with its thread) |
| Pi 500 (A76, 2.4 GHz) | ~4–7 % | 234 M/s at IPC 1.5–2.5 |
| Pi 3 (A53, 1.2 GHz) | ~20–33 % | 234 M/s at IPC 0.6–1.0 |
| R36S (A35, ~1 GHz) | **~30–47 %** — runs, with room | 234 M/s at IPC 0.5–0.8 (the A35's NEON is 64 bits wide); not run on the device |
| **Pi 2 (A7, 900 MHz)** | **~72–99 % of one core decoding, 56–77 % searching** | 312 / 244 M/s at the measured NEON IPC 0.35–0.48 (PI2-REAL-CPU.md) |
| Pi Zero / Pi 1 (ARMv6) | impossible (> 2 cores) | 2040 M/s for the decoder alone |

**So, on a Pi 2,** one RADE listener needs most of one core. That is real time on a quiet box, with nothing to spare. The
dec_rade row will not be GREEN there, and RADE will start off on a fresh setup; the owner can tick it on. ARMv6 boards
are hopeless. x86 machines were not counted; the M5 figure and SSE2 suggest a small fraction of a core on anything
since ~2010.

**Memory:**

- Each RADE receiver is **~0.8 MB** of heap. Most of it is the 960 × 40 acquisition grid.
- The decoder's and FARGAN's int8 weights and code page in **once, ~2.7 MB**, read-only and shared by every listener.
  Measured on the Mac: 10 receivers +8.9 MB, then +2.7 MB on first decode.
- AUTO's search adds the 0.8 MB to a listener's codec2 modems.
- This is well inside the 256 MB floor.

**Binary size** (macOS arm64 vibeserver): **+3.8 MB**, 22.1 → 25.9 MB.

- About 2.4 MB is the receiver: `__TEXT` +3.7 MB and `__DATA` +0.8 MB in total.
- About 1.4 MB is rade_c's transmitter, linked in only for the benchmark row's decoding measurement. ⚠ See the
  decisions below.
- The archives are the same size on Linux: receiver 2.2 MB, transmitter 1.4 MB.

### Off air (Stuart's UberSDR, standing permission; recordings in `~/VibeSDR-test-audio/freedv/`, never committed)

Every decoded over was put through **Whisper** (whisper.cpp, the `base` model, language auto-detected) as the
intelligibility check. That is the same objective measure the RADE authors use (WER).

- **The control run** first established that our chain gives intelligible speech:
  - rade_c's own 5-minute off-air sample (`FDV_offair.wav`, Australia) decoded through our receiver. It found callsigns
    VK5KVA and VK3TPM, and Whisper transcribes it as fluent English: "…I spoke too soon, Peter…", "…they get 5KVA…
    3TPM…".
  - A synthetic round trip at 6 dB transcribes word for word.
- **16:55 UTC recordings**, made in the afternoon for the findings above, replayed through our receiver:

  | Frequency | Result |
  |---|---|
  | 7.065 LSB | 25 RADE syncs in 10 minutes. End-of-over callsign **CT1DHR** (Portugal). On its 42 s over, Whisper transcribes **coherent Portuguese** ("Eu não tinha que falar… Eu tenho aqui… menu 17…"): **intelligible**. The other overs were weaker (RADE's SNR −1 to −4 dB, 19 % of frames with unique-word errors) and are not intelligible. |
  | 7.177 LSB | Callsign **DG4SDX** (Germany), decoded twice. Whisper hears German, partly garbled, ending in the phonetic "X-ray": **partly intelligible**. |
  | 3.643, 7.160, 14.236, 5.3685 | No RADE sync (14.236 and 60 m were closed to the UK at that hour). |

- **19:15 UTC, recorded live** at the frequencies FreeDV Reporter showed:
  - Brief RADE syncs at ~0 dB on **3.612** (DL1DX / S56SG) and **7.177**, a second or two of speech each, no callsign.
  - Nothing on 3.774 (SV1UH / SV1QVZ were in QSO there), 14.236 (the US net, closed to the UK), 5.3685 (G8SEZ) or 7.197.
- **19:26 UTC**, a second round, picked from a fresh Reporter snapshot (SV1QVZ transmitting on 3.774, CT7BHT on 3.680):
  - One RADE sync at −3 dB on 3.612, and nothing else.
  - Nothing on 3.774 on either sideband (so it was not a sideband mix-up), 3.680, 7.177 or 7.197.

**Honest summary:** RADE V1 decodes off air through our receiver. Two stations' callsigns were read from their
end-of-over frames. One over is intelligible speech, and a second is partly intelligible. Weak overs (≲ 0 dB) sync but
are not intelligible, which is RADE's own limit. 700D is still unproven off air: nobody was sending it.

### Tests

- **`test-freedv`: 82/82.** It runs both halves, codec2 and RADE. The RADE half needs `librade_vibe_tx.a` and reports
  NOT RUN without it. Without RADE built, the suite is 57/57. The RADE checks:
  - RADE by hand: sync, speech, the report naming RADE, SNR near the 10 dB sent, and the end-of-over callsign as
    `"last"`, never as `"text"`.
  - AUTO naming RADE.
  - "Never a wrong win", both ways: 16 starts per sub-mode with the loop wrap (64 runs), plus 8 through the real USB
    chain per sub-mode (32 runs). No RADE win on a codec2 signal, and none the other way.
  - RADE → 700D and 700D → RADE on one dial, the callsign kept.
  - RADE at 2 dB.
  - 60 s of noise with no false sync and no sound.
  - The RxPipeline USB chain.
  - Switching to RADE by hand.
  - `VIBE_RADE_DISABLE` (codec2 as before), `VIBE_CODEC2_DISABLE` (RADE alone) and `VIBE_FREEDV_DISABLE` (nothing).
- **`test-freedv clip`** adds a RADE station at +30 kHz: a 14 s over, then G4ABC's end of over, then noise to the loop.
  It is at the same level as the others. There RADE's estimate reads ~8 dB on the range-locked radio, right at the
  callsign's threshold (~9 dB: decoded every time at 8.7 dB in AWGN, never at 6.5).
- **`test-server-freedv`:**
  - The RADE station on both radio kinds and both sidebands: AUTO names it, speech on the audio socket, no text while
    in sync, `"last":"G4ABC"` after the over.
  - A server without libcodec2 offering `["RADE"]` and syncing on it.
  - The owner's `"rade"` switch off: not offered, not hunted, not accepted by hand.
  - Every listener pings, because the server drops a silent session after 30 s.
  - **146 passed, 0 failed** (2026-11-16, final run). Two earlier runs, with the RADE station louder in the clip, failed
    other checks instead: a louder RADE station cost the codec2 stations 3–5 dB of reported SNR through the server.
    The station therefore stays at the same level, and the "last" check waits for two overs' ends (45 s).
- **`test-web-freedv`: 59/59.** Headless Edge, `--mute-audio`.
  - The RADE key is first in the FREEDV row.
  - "FreeDV RADE · N dB", then "FreeDV · last G4ABC".
  - RADE by hand is lit by the report.
  - Five sub-mode keys stay in one row at 390 px.
  - Without libcodec2, the row is AUTO + RADE.
- **`test_freedv.ts`: 38/38.** The R36S golden: 2454 checks, 0 failed.
- **`test-bench-decoders`:** `dec_rade` measures (> 0 %), decodes 29 blocks, and has its defaults rule.

### Verified, and not

- ✓ Built and run on macOS arm64. The tests above were run there.
- ✓ `build-rade.sh` run inside the bookworm arm64 and armhf/ARMv6 cross images, and cross-built for x86_64 and i686.
- ✓ ARM builds counted under qemu, and x86 decodes checked under qemu.
- ✗ **Not run:**
  - The full `build-deps.sh` (Linux images or Mac), a .deb, or `build-app.sh`. The fetch-and-unpack lines are new and
    untested end to end.
  - A real Pi 2, R36S or Pi 500. The figures above are counts with an IPC range.
  - The app on a phone. Its logic is tested in TypeScript.
  - The R36S UI on the device (golden lines only).
  - The setup page's new switch in a browser.

### Android, Lite and iOS: RADE could be offered there (not built in this task)

RADE needs no libcodec2 and is all BSD C, so the LGPL questions that kept FreeDV off the phones do not apply. Static
linking is fine on iOS and on Android.

- **What it would take:** `build-rade.sh`'s source list built with the NDK (four ABIs) and for iOS, then lifting the
  `VIBE_FREEDV_NO_DLOPEN` gate for RADE alone. FreeDV would then be offered on phones with RADE only.
- **The cost to weigh:**
  - CPU: ~234–312 M instructions a second per listener. That is a few per cent of a modern arm64 phone core, but
    heavy on the 32-bit 1 GB phones the users include.
  - Battery.
  - APK size: ~2.5 MB per ABI.

### Decisions for Stuart

1. **The transmitter in the server (+1.4 MB):** it is there only so `dec_rade` can measure a receiver decoding a real
   over. Without it, the row would measure searching plus the decoder alone (`vrade_bench_decode`), which reads ~20 % low on
   armhf (249 against 312 M/s). Keep it, or drop it for the smaller binary?
2. **Pi 2:** one RADE listener ≈ 0.7–1.0 of a core, so expect RADE off by default there, with the owner able to tick it
   on. Should RADE also be capped to one listener at a time on such boxes? Not built.
3. **ARMv6:** RADE cannot run. Its switch stays offered and starts off (the rule is "never hide"). Agree?
4. **The callsign line:** `FreeDV · last G4ABC` replaces "searching" once a RADE over ends. It was "· searching · last",
   but that was 41 characters, past the R36S's 40.
5. **RADE V2** (7 of 73 stations on Reporter) is not decoded: its README says the waveform will change. Revisit when
   FreeDV announces it.
6. **The search**, our vectorised `vibe_rade_acq.c`, could go further with an FFT correlation (~10× fewer operations).
   It is not needed while decoding (FARGAN) costs as much as searching.

## The protocol

- **hwinfo / vibeserver.json:** `modes` lists `"freedv"`, and `freedvSubs` lists `["RADE","700D","700E","1600"]` —
  RADE where it is built in and the owner has not switched it off, the other three whatever this libcodec2 can open. Both
  appear only when at least one sub-mode runs (a server with RADE alone lists `["RADE"]`). Clients build their mode list from this, so there are no dead
  controls. The setup page's "FreeDV (digital voice)" switch appears on the same condition (`vsRadioCaps`). FreeDV is
  never offered on an Icom (its audio is the radio's own demodulator) or on VFO B.
- **Selecting it:** `{"type":"tune","frequency":…,"mode":"freedv"}`, like any demodulator.
- **Report** (on the spectrum socket, whenever it changes):
  `{"type":"freedv","sub":"auto|RADE|700D|700E|1600","sb":"usb|lsb","sbSel":"auto|usb|lsb","state":"search|sync","mode":"RADE|700D|","snr":N,"text":"…","last":"G4ABC"}`
  — `last` (RADE only): the callsign of the over that has just ENDED, from its end-of-over frame, kept until the receiver
  is rebuilt for a new dial. Clients show it only while searching, never beside a station that is talking now.
- **Choosing a sub-mode or a sideband:** `{"type":"freedv_sub","sub":"auto|RADE|700D|700E|1600","sb":"auto|usb|lsb"}` — either
  field, both or neither. A sub-mode this server does not run now (no libcodec2, or RADE switched off) is not applied. Sent with neither, it only asks for the report, which comes back at once. A value that is none
  of ours is ignored. On an ask-before-tuning shared dial a choice is gated with the mode (`askRefuses`).

## Threading and memory

The rule is Network > Audio > Spectrum > Decoders. Here the decoded speech *is* the audio, so the modem must never stall
the audio thread:

1. **Audio callback:** it only copies its block into a bounded queue. Past 2 s, the oldest is dropped and counted.
2. **Worker thread:** the modems and the vocoder run on their own `vibe-freedv` thread, at the audio-codec rung (as
   FAAD2 does for DAB+). That is one step below the chain that feeds it.
3. **Speech back:** the callback takes exactly the frames it sends from a PCM buffer.
   - It is silent until 0.3 s is primed, and silent on underrun.
   - Past 1.5 s, the oldest speech is dropped, so latency never grows.
   - A slow modem is heard as a gap, never as a stall.
4. **Modem input:** each modem's input buffer is compacted as it goes and never holds more than 4 s.

There is one FreeDV receiver per listener on a range-locked radio, and one shared receiver on a one-pipeline radio or a
shared dial.

**Benchmark row `dec_freedv`:** one listener's worst case, AUTO searching on band noise with all three modems open. After
that, unmeasured, the row proves it decodes a 700D signal from the library's own transmitter. Its rule is `"freedv"`,
primary and not a slot: on a box where it is not green, the mode is off by default on a fresh setup, and the owner can
always tick it back on.
- **M5 Pro:** 0.6 % of a core.
- **Pi:** not measured yet.

## The library, and why it is loaded at run time

**Codec 2** (David Rowe et al., **LGPL-2.1**) is the only definition of the vocoder, so it is used rather than rewritten.
No codec2 or FreeDV source is in this tree. `vibe_freedv.h` declares the few FreeDV API functions by their published C
signatures (`freedv_api.h`) and finds them with `dlsym` in the **system's** library:

- **Linux:** `libcodec2.so.1.2`, `.1.1`, `.1.0`, then `.so`, by soname through the system loader.
  - Checked 2026-11-16 in arm64 containers, test-freedv 45/45 on both: Debian 12 bookworm ships libcodec2-1.0 (1.0.5),
    and Debian 13 trixie ships libcodec2-1.2 (1.2.0). Both offer 700D, 700E and 1600.
  - Raspberry Pi OS follows its Debian base. The Ubuntu versions have not been checked.
- **macOS:** Homebrew `codec2` (`/opt/homebrew/opt/codec2/lib`, `/usr/local/opt/codec2/lib`) or MacPorts. A file is
  loaded only when its real path is owned by root or by the server's own user, and nobody else can write to it. The Mac
  app runs with disable-library-validation, so nothing else checks what `dlopen` loads.
- **What is offered:** each sub-mode is probed with `freedv_open` at load. A library that cannot open 700E simply does
  not offer it.
- **Switching it off:** `VIBE_FREEDV_DISABLE=1` makes the server behave as if the library were missing. The tests use it.

Why dynamic, and why not a hard dependency:

- **LGPL:** linking dynamically meets the LGPL's relinking requirement without any work on our side. The owner's
  distribution supplies and updates the library.
- **Packaging:** the .deb must install everywhere with nothing but libc6 in Depends (CMakeLists, "nothing from the system
  but glibc"). The package therefore **Recommends** `libcodec2-1.2 | libcodec2-1.1 | libcodec2-1.0`. apt installs the
  first alternative that exists, `--no-install-recommends` skips it, and a missing library costs only this mode.

## Tests

| Test | What it does | Result |
|---|---|---|
| `vibeserver/test-freedv.cpp` (run-tests.sh; exit 3 when libcodec2 is missing; also passes in Debian bookworm and trixie arm64 containers) | codec2's own transmitter on a synthetic voice, noise measured in 3 kHz. Each sub-mode by hand at 10 dB, AUTO naming each one, 700D at 4 dB, 700E and 1600 at 5 dB, 30 s of noise in AUTO (no lock, no sound), the real `RxPipeline` USB chain from IQ 6 kHz off centre with a +25 Hz error, a live sub-mode switch, and `VIBE_FREEDV_DISABLE` | 45/45 on the M5, Homebrew codec2 1.2.0; with RADE V1 (2026-11-16) 82/82, and 57/57 built without it |
| `test-freedv clip OUT.cu8 [rate sec snr usb\|lsb]` | writes the bench clip: 700D at +10 kHz, 700E at +20 kHz, 1600 at −10 kHz | tool |
| `test-freedv wav FILE [sub]` | replays off-air audio and prints every report | tool |
| `scripts/test-server-freedv.mjs` (run-tests.sh; silent) | the real server on a fake rtl_tcp replaying the clips. 20 m USB and 40 m LSB, each on a one-pipeline radio and a range-locked one: AUTO names each sub-mode, the report's sideband and SNR, the text, decoded speech on the audio socket, silence on a manual sub-mode that is not on air, AUTO back again, SSB audio after leaving, and no mode without the library | see the commit log |
| `scripts/test-server-freedv.mjs`, sideband override | the 20 m (USB) clip replayed at 7.21 MHz, on both radio kinds: AUTO is LSB there and never syncs; `sb usb` → the report says `sbSel usb`, `sb usb`, and AUTO syncs on 700D; a nudge keeps the choice; `sb auto` → LSB again; nonsense ignored | 2026-11-16 |
| `scripts/test-web-freedv.mjs`, sideband override | the same station in the web page: the FREEDV SIDEBAND row, USB lit by the report, sync, the passband above the dial, one line at 390 px, AUTO back | 2026-11-16 |
| `scripts/test_freedv.ts` | the client rule module, the override and 60 m | 27/27 |
| `scripts/test_freedv_app.ts` | the app: passband side with an override, the row under the sub-modes, gated and lit by the report | 23/23 |
| `handheld-ui/test/run_vts_golden.sh` | the R36S VTS lines and sideband rule (override, 60 m), generated from `freedv.ts` | 2427 checks, 0 failed |

## Android, Lite and iOS: not this round (codec2 — for RADE see "RADE V1" above)

- **Android (app with local hardware, and VibeServer Lite):** there is no system libcodec2. It would need codec2 built
  with the NDK for the four ABIs and shipped as a separate `.so` in the APK, loaded with `dlopen` by name. Shipping it as
  a separate shared object keeps the LGPL relinking route open. The same `vibe_freedv.h` would then load it unchanged
  once the `__ANDROID__` guard is lifted. Cost: about 1 MB of APK per ABI. Check memory on 1 GB phones: AUTO opens three
  modems.
- **iOS:** the App Store does not allow `dlopen` of a library outside the bundle. It would need codec2 built as an
  embedded dynamic framework, signed with the app. That is allowed, and keeps LGPL relinking possible through the
  framework. Static linking would make the LGPL obligations harder for an App Store binary, so it is not recommended.
- **Windows:** `libcodec2.dll` from an installer, loaded the same way (`windows/compat/dlfcn.h`). It is not wired yet.
