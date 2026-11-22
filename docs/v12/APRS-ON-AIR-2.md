# APRS on air, round 2 (2026-11-22) — branch `b91-aprsdbg`

## RESUME HERE

- **The symptom.** Pi 500, the RTL-SDR Blog V4 (serial 00000001: a YouLoop into a Nooelec LaNA on the V4's bias tee),
  144.800 NFM, beta90. APRS AUTO was attached ("aprs AUTO (1200 + 9600) FX.25 on IL2P on"), several packets a minute
  showed on the waterfall **a few dB over the floor**, and **none decoded**. Squelch was OFF (Stuart). Gain about 38.6 dB,
  VibeAGC stepping it. Another receiver on the same aerial decoded some.
- **What was found (synthetic only, no live signal yet):**
  1. **No regression since 11-10.** The AFSK demodulator from before the 11-19 packet work (`b8366147d~1`) and today's
     (plain, and with FX.25 + IL2P as AUTO runs it, and the whole `Run` in AUTO) found **exactly the same packets**:
     593 of 1200 over the same 30-row sweep (5–10 dB, flat / ±6 dB tilt, 30 and 12 flags of preamble). The NFM chain
     (`pipeline.cpp`) has no change since 11-08 apart from a reverted WIP. The beta90 squelch change does not reach the
     decoders: they are fed **before** the squelch, on the per-listener path (`onClientAudio`) and the shared one.
  2. **The server's chain costs nothing against a minimal one.** `test-aprs-onair` keys FM bursts in noise (FM hiss
     between packets, as off air) through the real `RxPipeline` NFM and compares a bare FIR + ideal discriminator on the
     same IQ: equal or better at every row.
  3. **Weak packets die at the FM THRESHOLD, and the listener's filter width decided which.** Below ~9 dB carrier to
     noise (in 12.5 kHz) the discriminator breaks into clicks; nothing after it can recover that (any decoder working
     from discriminator audio is bound by the same limit). At 8 dB the filter is everything: 48 packets, a pre-emphasised
     transmitter, the listener's audio alone — **9 kHz 41, 10 kHz 43, 12.5 kHz 31, 16 kHz 11**. At 6 dB: 0 for every
     chain. "A few dB over the floor" on a waterfall bin is about this range, so these packets were most likely at or
     under the threshold, where our listener-audio decoder (on the listener's 12.5 kHz or wider) lost most of them.
  4. **The modem itself is near theory.** Flat audio, half the packets at ≈ 6.7 dB in 3 kHz ⇒ Eb/N0 ≈ 10.7 dB; ideal
     non-coherent orthogonal FSK needs ≈ 11.3 dB for a ~1100-bit frame at 50 % — the one-bit repair and five slicers make
     up the rest. A different audio demodulator would not have found many more.
- **What changed (fix, proven):** 1200 Bd now has its **own narrow FM discriminator on the channel IQ** (±5 kHz FIR +
  ideal discriminator + the same `AfskDemod`), beside the listener's audio; a frame both find counts once. At 8 dB:
  listener on 12.5 kHz **31 → 43 / 48**, on 16 kHz **11 → 42 / 48**; every strong, over-deviating (±5 kHz), Voice /
  de-emphasised, gain-step, short-TXDELAY and off-tune row still full; 60 s of FM hiss: 0 frames. +0.13 % of an M5 core.
- **The developer capture is built** (`VIBE_DEC_AUDIO_DUMP`, below) and proven in the real server. **NEXT: the
  coordinator captures a live session on the Pi 500** with this build, then `tool-dec-replay` it here: the `.cf32`
  channel IQ gives the packets' real CNR, the `.txt` the listener's filter/gain each second. If the live packets turn
  out to be ≥ 10 dB and still lost, the cause is elsewhere and the capture will show it.

## Open decisions for Stuart

- The narrow path's width: ±5 kHz (10 kHz). ±4 kHz would gain a little more at the threshold; the listener-audio path
  keeps over-deviating trackers either way. Left at ±5 until a live capture says otherwise.
- The earlier proposal (11-10) — the decoders fed the NFM audio BEFORE Voice — is no longer needed for 1200: the narrow
  path does not depend on the listener's Voice / ALL / de-emphasis at all. The clients' NFM ALL while APRS is open can stay.

## The developer capture (never a user feature)

`android/app/src/main/cpp/vibe_dec_dump.h`, used by `vibe_decoder_host.h`. With `VIBE_DEC_AUDIO_DUMP=<dir>` every decoder
attach on any DecoderHost writes `<dir>/dec-<unix>-<ext>-<n>.wav` (the exact 48 kHz mono audio the host feeds its decoder,
32-bit float, header rewritten each second so a cut-off file opens), `.cf32` (the channel IQ, when the decoder reads it —
APRS 1200 / AUTO / 9600, paging, VOR …; float32 I,Q at the rate the `.txt` names) and `.txt`:

```
# ext aprs
# session <the listener's session id>
# params {"type":"audio_extension_attach","extension_name":"aprs","packet":"auto",…}
# channel IQ at 37500.000 S/s, float32 I,Q interleaved -> ….cf32
5.1 5.06 0.00 0 | dial 144800000 Hz mode nfm bw 12500 Hz chan 37500 S/s squelch off -100.0 dB level -71.3 dB nfm all deemph 50us gain 38.6 dB target 49.6 dB agc vibeagc ovl-steps 0
```
(per line: seconds since the attach, audio seconds fed, seconds queued, frames the host dropped, then the server's state —
the applied tuner gain each second shows every VibeAGC step.) Written on the decode thread, never the audio/DSP thread.
One cap for the process, every file together: `VIBE_DEC_AUDIO_DUMP_MB` (default 512; 48 kHz float audio is ~0.7 GB an
hour, the 37.5 kS/s IQ ~1.1 GB an hour). At the cap the files close and the `.txt` says so. Unset: not even a file handle.

### Enabling it on a server (coordinator)

The directory must be writable under the units' `ProtectSystem=strict` — `/var/lib/vibeserver` is (`ReadWritePaths`):

```
sudo install -d -o vibeserver -g vibeserver -m 0750 /var/lib/vibeserver/decdump
sudo mkdir -p /etc/systemd/system/vibeserver.service.d /etc/systemd/system/vibeserver@.service.d
printf '[Service]\nEnvironment=VIBE_DEC_AUDIO_DUMP=/var/lib/vibeserver/decdump\nEnvironment=VIBE_DEC_AUDIO_DUMP_MB=2048\n' | sudo tee /etc/systemd/system/vibeserver.service.d/decdump.conf
printf '[Service]\nEnvironment=VIBE_DEC_AUDIO_DUMP=/var/lib/vibeserver/decdump\nEnvironment=VIBE_DEC_AUDIO_DUMP_MB=2048\n' | sudo tee /etc/systemd/system/vibeserver@.service.d/decdump.conf
sudo systemctl daemon-reload && sudo systemctl restart vibeserver   # (and/or vibeserver@<serial> — restarts drop listeners)
```
Then open APRS on 144.800 as a listener, let packets come, detach (closes the files). Copy `dec-*-aprs-*.{wav,cf32,txt}`
back. **Turn it off after**: `sudo rm /etc/systemd/system/vibeserver{,@}.service.d/decdump.conf`, daemon-reload, restart,
delete the directory's files. The front door forks one process per radio: they inherit the variable; the cap is per process.

### Replaying

`vibeserver/tool-dec-replay.cpp` (built like test-decoder-hosts; `scripts/run-tests.sh` builds it as `$OUT/tool-dec-replay`):
```
tool-dec-replay /path/dec-1795360644-aprs-0              # .txt → ext + params; .wav; .cf32 if there
tool-dec-replay /path/dec-…-aprs-0 --params '{"packet":"1200"}'   # another attach on the same capture
tool-dec-replay --ext aprs --msg '{"packet":"auto"}' --wav any48k.wav [--iq f.cf32 --iq-rate 37500] [--quiet]
```
It runs a real DecoderHost, paced on its own queues (never past what it drops), and prints every text frame and each
APRS packet's TNC2 line.

## Files

| File | What |
|---|---|
| `android/app/src/main/cpp/vibe_dec_dump.h` | the capture (+ a WAV reader the tools share) |
| `android/app/src/main/cpp/vibe_decoder_host.h` | `Env::dumpState`; a capture per attach; audio/IQ written on the decode thread |
| `android/app/src/main/cpp/local_sdr_shim.cpp` | `decoderEnv`: the state line (only while the variable is set) |
| `android/app/src/main/cpp/vibe_aprs_run.h` | 1200 Bd's narrow path (`narrow1200_`, `offer1200_`); `wantsIq()` true for 1200 too |
| `vibeserver/dec_replay.h`, `vibeserver/tool-dec-replay.cpp` | the replay |
| `vibeserver/test-dec-dump.cpp` | the capture's tests |
| `vibeserver/test-aprs-onair.cpp` | APRS off air through the real chain (`--sweep` for the full table) |
| `scripts/test-server-aprs-capture.mjs` | the real server: range-locked radio, a listener's own VFO, AUTO, squelch closed, the capture, the replay |

## Tests (this Mac, M5 Pro)

| Test | Result |
|---|---|
| `test-dec-dump` | **14 / 0** — unset writes nothing; `.wav` and `.cf32` bit-exact with what the host was fed; `.txt` ext/params/IQ rate; replay = the same 12 packets in order; the cap |
| `test-aprs-onair` | **10 / 0** (9 s) — regression rows above, the threshold rows with their numbers, FM hiss 0 frames |
| `test-aprs-onair --sweep` | the table quoted above (44 s) |
| `test-server-aprs-capture.mjs` | **8 / 0** — the real server writes the capture (state line: mode nfm, squelch on, dial, 5 s fed); replay finds every live packet |
| `test-server-ais-aprs.mjs` | 36 / 0 (unchanged) |
| `test-aprs` 63 / 0 · `test-ax25hs` 74 / 0 · `test-decoder-hosts` all passed | unchanged |

CPU (M5 Pro, one core): APRS 1200 audio 0.09 %, the narrow path on 37.5 kS/s IQ +0.13 %, AUTO in all 0.48 %. The Pi 500
is perhaps ×5–8: ~3 % of a core for AUTO. Its discriminator uses scalar `atan2` (the FIR is `dotReal`, SIMD) — a SIMD
pass is owed by the house rule if it ever shows on a profile.

## Not done / honest limits

- Nothing here was verified on a real off-air signal: all synthetic (keyed FM, white noise, no RTL 8-bit quantisation,
  DC spur or tuner settling after a hardware gain step). The live capture is the next step.
- The Android app's server shares `vibe_decoder_host.h` / `vibe_aprs_run.h`, so it gets the narrow path where its host
  feeds channel IQ; not built or run on Android here. The R36S (handheld-ui) was not checked.
- No client copy changed (no control moved or appeared).
