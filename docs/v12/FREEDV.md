# FreeDV digital voice as a mode (V12)

FreeDV is a mode in the same list as AM, FM, USB and LSB. It is not a decoder window. The server demodulates SSB as it
always does. That audio goes through the FreeDV modem and the Codec 2 vocoder, and the decoded speech replaces it. The
mode is silent until there is speech to play. The VTS line explains the silence:

| What the listener sees | When |
|---|---|
| `FreeDV · searching` | AUTO, nothing decoded yet |
| `FreeDV 1600 · searching` | a sub-mode chosen by hand, nothing decoded |
| `FreeDV 700D · 6 dB · G4ABC` | in sync: the sub-mode on air, the modem's SNR, the station's text |

The lines are kept short because the R36S screen is small.

## Sub-modes, AUTO and the manual fallback

- **Sub-modes:** 700D, 700E and 1600. RADE is out of scope.
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

FreeDV keeps the SSB convention: LSB below 10 MHz, USB from 10 MHz up. This is from freedv.org, "What you need"; a
community proposal to use USB everywhere has not been adopted. A modem signal on the wrong sideband is inverted and never
syncs. So the server picks the sideband from the dial and reports it:

- **Per listener:** `clientRetune`, which runs on every tune.
- **Shared demodulator:** `rxModeDial()`, which every `rx.setTune` call now uses.
- **DC placement:** `mainDcZone` applies the same rule, so the tuner's DC is kept off the side actually heard. Without
  this, 700E on 40 m LSB failed on a one-pipeline radio.

Clients draw the passband on the side the report gives (`"sb"`). They never decide it themselves.

⚠ **Decision for Stuart:** the rule is fixed at 10 MHz. 60 m (5 MHz) is reported to be USB by some operators. A manual
sideband override is not built.

## The protocol

- **hwinfo / vibeserver.json:** `modes` lists `"freedv"`, and `freedvSubs` lists `["700D","700E","1600"]` (whatever this
  library can open). Both appear only when libcodec2 loaded. Clients build their mode list from this, so there are no dead
  controls. The setup page's "FreeDV (digital voice)" switch appears on the same condition (`vsRadioCaps`). FreeDV is
  never offered on an Icom (its audio is the radio's own demodulator) or on VFO B.
- **Selecting it:** `{"type":"tune","frequency":…,"mode":"freedv"}`, like any demodulator.
- **Report** (on the spectrum socket, whenever it changes):
  `{"type":"freedv","sub":"auto|700D|700E|1600","sb":"usb|lsb","state":"search|sync","mode":"700D|","snr":N,"text":"…"}`
- **Choosing a sub-mode:** `{"type":"freedv_sub","sub":"auto|700D|700E|1600"}`. Sent without `sub`, it only asks for the
  report, which comes back at once. On an ask-before-tuning shared dial this is gated with the mode (`askRefuses`).

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
| `scripts/test_freedv.ts` | the client rule module | 17/17 |
| `handheld-ui/test/run_vts_golden.sh` | the R36S VTS lines, generated from `freedv.ts` | 2323 checks, 0 failed |

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
