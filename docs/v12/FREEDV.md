# FreeDV digital voice as a mode (V12)

FreeDV is a mode in the same list as AM, FM, USB and LSB. It is not a decoder window. The server demodulates SSB as it
always does. That audio goes through the FreeDV modem and the Codec 2 vocoder, and the decoded speech replaces it. The
mode is silent until there is speech to play. The VTS line explains the silence:

| What the listener sees | When |
|---|---|
| `FreeDV · searching` | AUTO, nothing decoded yet |
| `FreeDV 1600 · searching` | a sub-mode chosen by hand, nothing decoded |
| `FreeDV LSB · searching`, `FreeDV 700E USB · searching` | a sideband (and a sub-mode) chosen by hand, nothing decoded |
| `FreeDV 700D · 6 dB · G4ABC` | in sync: the sub-mode on air, the modem's SNR, the station's text |

The lines are kept short because the R36S screen is small.

## Sub-modes, AUTO and the manual fallback

- **Sub-modes:** 700D, 700E and 1600. RADE is not decoded — see "What is on air today" below: it is now nearly all of the traffic.
- **AUTO:** runs all three demodulators while it searches.
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

**RADE, in facts** (for a later decision — not built in this task)

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

**Recommendation**

1. Keep 700D/700E/1600 as they are: they cost nothing when nobody sends them, and the hardware users (SM1000, ezDV) can
   only send 700D.
2. **RADE V1 is the mode worth adding**, and its licence allows it: link `rade_c` (BSD-2) — the whole receiver, weights
   and FARGAN — rather than reimplementing it. There is no published standard to write it from; the reference *is* the
   definition, as Codec 2 is for 700D. As with libcodec2, prefer loading a system or bundled library to vendoring
   source, and measure the Pi 2 first: FARGAN at ~300 MMAC/s per listener is far heavier than the 700D modem (0.6 % of an
   M5 core), so it may need to be one shared receiver, or benchmark-gated like the other heavy decoders.
3. Wait for RADE V2 to settle before adding it (its own README says so).
4. Until RADE is in, the mode's hint and the VTS line should say what it decodes. Today's hint already names "700D, 700E
   or 1600"; a listener on 14.236 will see "FreeDV · searching" for ever. ⚠ **Decision for Stuart:** whether to say
   "RADE is not decoded yet" in the hint.

## The protocol

- **hwinfo / vibeserver.json:** `modes` lists `"freedv"`, and `freedvSubs` lists `["700D","700E","1600"]` (whatever this
  library can open). Both appear only when libcodec2 loaded. Clients build their mode list from this, so there are no dead
  controls. The setup page's "FreeDV (digital voice)" switch appears on the same condition (`vsRadioCaps`). FreeDV is
  never offered on an Icom (its audio is the radio's own demodulator) or on VFO B.
- **Selecting it:** `{"type":"tune","frequency":…,"mode":"freedv"}`, like any demodulator.
- **Report** (on the spectrum socket, whenever it changes):
  `{"type":"freedv","sub":"auto|700D|700E|1600","sb":"usb|lsb","sbSel":"auto|usb|lsb","state":"search|sync","mode":"700D|","snr":N,"text":"…"}`
- **Choosing a sub-mode or a sideband:** `{"type":"freedv_sub","sub":"auto|700D|700E|1600","sb":"auto|usb|lsb"}` — either
  field, both or neither. Sent with neither, it only asks for the report, which comes back at once. A value that is none
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
| `vibeserver/test-freedv.cpp` (run-tests.sh; exit 3 when libcodec2 is missing; also passes in Debian bookworm and trixie arm64 containers) | codec2's own transmitter on a synthetic voice, noise measured in 3 kHz. Each sub-mode by hand at 10 dB, AUTO naming each one, 700D at 4 dB, 700E and 1600 at 5 dB, 30 s of noise in AUTO (no lock, no sound), the real `RxPipeline` USB chain from IQ 6 kHz off centre with a +25 Hz error, a live sub-mode switch, and `VIBE_FREEDV_DISABLE` | 45/45 on the M5, Homebrew codec2 1.2.0 |
| `test-freedv clip OUT.cu8 [rate sec snr usb\|lsb]` | writes the bench clip: 700D at +10 kHz, 700E at +20 kHz, 1600 at −10 kHz | tool |
| `test-freedv wav FILE [sub]` | replays off-air audio and prints every report | tool |
| `scripts/test-server-freedv.mjs` (run-tests.sh; silent) | the real server on a fake rtl_tcp replaying the clips. 20 m USB and 40 m LSB, each on a one-pipeline radio and a range-locked one: AUTO names each sub-mode, the report's sideband and SNR, the text, decoded speech on the audio socket, silence on a manual sub-mode that is not on air, AUTO back again, SSB audio after leaving, and no mode without the library | see the commit log |
| `scripts/test-server-freedv.mjs`, sideband override | the 20 m (USB) clip replayed at 7.21 MHz, on both radio kinds: AUTO is LSB there and never syncs; `sb usb` → the report says `sbSel usb`, `sb usb`, and AUTO syncs on 700D; a nudge keeps the choice; `sb auto` → LSB again; nonsense ignored | 2026-11-16 |
| `scripts/test-web-freedv.mjs`, sideband override | the same station in the web page: the FREEDV SIDEBAND row, USB lit by the report, sync, the passband above the dial, one line at 390 px, AUTO back | 2026-11-16 |
| `scripts/test_freedv.ts` | the client rule module, the override and 60 m | 27/27 |
| `scripts/test_freedv_app.ts` | the app: passband side with an override, the row under the sub-modes, gated and lit by the report | 23/23 |
| `handheld-ui/test/run_vts_golden.sh` | the R36S VTS lines and sideband rule (override, 60 m), generated from `freedv.ts` | 2427 checks, 0 failed |

## Android, Lite and iOS: not this round

- **Android (app with local hardware, and VibeServer Lite):** there is no system libcodec2. It would need codec2 built
  with the NDK for the four ABIs and shipped as a separate `.so` in the APK, loaded with `dlopen` by name. Shipping it as
  a separate shared object keeps the LGPL relinking route open. The same `vibe_freedv.h` would then load it unchanged
  once the `__ANDROID__` guard is lifted. Cost: about 1 MB of APK per ABI. Check memory on 1 GB phones: AUTO opens three
  modems.
- **iOS:** the App Store does not allow `dlopen` of a library outside the bundle. It would need codec2 built as an
  embedded dynamic framework, signed with the app. That is allowed, and keeps LGPL relinking possible through the
  framework. Static linking would make the LGPL obligations harder for an App Store binary, so it is not recommended.
- **Windows:** `libcodec2.dll` from an installer, loaded the same way (`windows/compat/dlfcn.h`). It is not wired yet.
