# BRIEF: SpyServer Backend Adapter (remote IQ into VibeDSP)

**Project:** VibeSDR (React Native / Expo)
**Author:** Stuart Carr (Stuey3D)
**Status:** Draft — depends on the rtl_tcp / local-IQ pipeline (shipped) and the NWConnection networking layer. Exploratory; not yet scheduled.

**Licensing (read first):** SpyServer is Airspy's closed-source freeware *server*; the wire protocol is unofficial but publicly documented across multiple independent open-source clients (SDR++, SDRangel, standalone clients). This brief is a **protocol specification** — struct layouts, enum values and message semantics, which are uncopyrightable functional facts. Implementation rules:

1. **Do not copy, port, or paraphrase any SDR++ (or other GPL) client source.** VibeSDR cannot grant the APPSTORE-EXCEPTION §7 permission over third-party copyright — same reason SDR++ Brown was stripped in v5. Implement fresh Swift/Kotlin/C++ from this spec only, as with VibeDSP.
2. **Never bundle or redistribute the SpyServer binary** (Airspy EULA). We are a client interoperating over the network, nothing more.
3. Keep this brief in-repo as the provenance record of the clean-room process.

---

## 1. Goal

Add a `spyserver` backend: raw TCP client streaming **decimated IQ** into the existing VibeDSP demod chain (same consumer as rtl_tcp/USB), plus SpyServer's **server-computed FFT stream** feeding the Skia waterfall directly. Net effect: remote Airspy One / Airspy HF+ / RTL-SDR servers worldwide become VibeSDR backends, with *lower* client CPU cost than rtl_tcp (no wideband FFT on-device).

Architecture position: this is a **transport + control layer in the local-IQ family**, not a demod-server adapter. Demodulation, filtering, RDS: all VibeDSP, unchanged.

## 2. Transport

- Plain TCP (default port **5555**), binary, **little-endian** throughout. No TLS in the protocol; document that in the connect UI.
- iOS: `NWConnection` (TCP, not WebSocket). Android: Kotlin `Socket`/`SocketChannel` on a dedicated thread, mirroring the rtl_tcp client.
- Messages can be large (body up to 1 MiB); read loop must handle partial reads and reassemble by header `BodySize`.

## 3. Message framing

### 3.1 Client → server: commands

```
struct CommandHeader {         // 8 bytes
  uint32 CommandType;
  uint32 BodySize;             // bytes following
}
```

| CommandType | Name | Body |
|---|---|---|
| 0 | HELLO | `uint32 ProtocolVersion` followed immediately by an ASCII app-name string (not NUL-terminated; length implied by BodySize). Protocol version constant: `(2 << 24) | (0 << 16) | 1700` (i.e. "2.0.1700"). App name: `"VibeSDR"` |
| 2 | SET_SETTING | `uint32 Setting; uint32 Value;` |
| 3 | PING | empty |

### 3.2 Server → client: messages

```
struct MessageHeader {         // 20 bytes
  uint32 ProtocolID;           // server's protocol version
  uint32 MessageType;          // low 16 bits = type; high 16 bits carry flags/arg on some servers — mask with 0xFFFF when dispatching
  uint32 StreamType;           // 0 status, 1 IQ, 2 AF, 4 FFT
  uint32 SequenceNumber;       // per-stream, detect drops
  uint32 BodySize;
}
```

| MessageType | Meaning | Body |
|---|---|---|
| 0 | DEVICE_INFO | struct §3.3 |
| 1 | CLIENT_SYNC | struct §3.4 |
| 2 | PONG | — |
| 3 | READ_SETTING | — (unused by us) |
| 100/101/102/103 | IQ data: uint8 / int16 / int24 / float32 | interleaved I,Q samples |
| 200–203 | AF data (uint8…float) | **never implemented by real servers — do not build a path for it; demod is always client-side** |
| 300 | FFT, packed 4-bit ("dint4") | see §6 |
| 301 | FFT, uint8 | one byte per bin |

### 3.3 DEVICE_INFO (sent after HELLO)

```
uint32 DeviceType;             // 0 invalid, 1 Airspy One, 2 Airspy HF+, 3 RTL-SDR
uint32 DeviceSerial;
uint32 MaximumSampleRate;      // Hz, at decimation stage 0
uint32 MaximumBandwidth;
uint32 DecimationStageCount;   // stages available; rate at stage n = MaximumSampleRate >> n
uint32 GainStageCount;
uint32 MaximumGainIndex;       // device gain is an index 0..this
uint32 MinimumFrequency;       // Hz
uint32 MaximumFrequency;       // Hz
uint32 Resolution;             // ADC bits (informational)
uint32 MinimumIQDecimation;    // **guest restriction**: smallest decimation you may request
uint32 ForcedIQFormat;         // 0 = free choice; else server forces this StreamFormat
```

### 3.4 CLIENT_SYNC (sent after DEVICE_INFO and on state changes)

```
uint32 CanControl;             // 0 = spectator: you cannot retune/regain the hardware
uint32 Gain;
uint32 DeviceCenterFrequency;  // Hz — where the hardware actually sits
uint32 IQCenterFrequency;      // Hz — centre of YOUR decimated IQ slice
uint32 FFTCenterFrequency;
uint32 Minimum/MaximumIQCenterFrequency;    // allowed range for IQ retune requests
uint32 Minimum/MaximumFFTCenterFrequency;
```

`CanControl == 0` maps to VibeSDR Spectator mode, but note the nuance: even without control you can usually still *choose your IQ slice within the hardware's captured band* (the min/max IQ centre bounds tell you the window). This maps beautifully onto the v5.1 second-VFO model — `DeviceCenterFrequency` **is** the dashed RF-CENTRE marker, and the min/max IQ bounds are the pan walls.

## 4. Settings (SET_SETTING ids)

| Id | Name | Value |
|---|---|---|
| 0 | STREAMING_MODE | bitmask: 1 = IQ, 4 = FFT, 5 = FFT+IQ (**use 5**) |
| 1 | STREAMING_ENABLED | 0/1 — send *after* all other settings |
| 2 | GAIN | device gain index (only honoured when CanControl) |
| 100 | IQ_FORMAT | StreamFormat: 1 uint8, 2 int16, 3 int24, 4 float32, 5 dint4. Respect `ForcedIQFormat`; otherwise prefer **int16** |
| 101 | IQ_FREQUENCY | Hz, centre of the IQ slice (clamp to CLIENT_SYNC bounds) |
| 102 | IQ_DECIMATION | stage index; **must be ≥ MinimumIQDecimation**. Output rate = MaximumSampleRate >> stage |
| 103 | IQ_DIGITAL_GAIN | see §5 |
| 200 | FFT_FORMAT | 1 uint8 (recommended) or 5 dint4 |
| 201 | FFT_FREQUENCY | Hz, FFT view centre |
| 202 | FFT_DECIMATION | stage index → FFT span = MaximumSampleRate >> stage |
| 203 | FFT_DB_OFFSET | 0…100 |
| 204 | FFT_DB_RANGE | 10…150 (dB mapped across the sample range) |
| 205 | FFT_DISPLAY_PIXELS | bins per frame, 100…32768 — set to waterfall widget pixel width (device-pixel), re-send on rotation/zoom |

**Connect sequence:** TCP connect → HELLO → await DEVICE_INFO + CLIENT_SYNC → send STREAMING_MODE=5, IQ_FORMAT, IQ_DECIMATION, IQ_FREQUENCY, IQ_DIGITAL_GAIN, FFT_FORMAT, FFT_DECIMATION, FFT_FREQUENCY, FFT_DB_OFFSET/RANGE, FFT_DISPLAY_PIXELS → STREAMING_ENABLED=1. Send PING every ~5 s as keepalive; treat missing PONG ×3 as dead link.

## 5. Digital gain (dynamic-range compensation)

When the server decimates, each halving of bandwidth lowers the noise floor and shifts signal energy relative to full scale, costing headroom in narrow integer formats. `IQ_DIGITAL_GAIN` pre-scales samples server-side to recover it. Required behaviour (derive independently in VibeDSP terms — do not transcribe another client's code):

- The compensation is **~3 dB per decimation stage** (each stage halves bandwidth → 10·log₁₀(2) ≈ 3.01 dB of processing gain to reclaim).
- On Airspy One, additionally compensate for reduced front-end gain: add `(MaximumGainIndex − currentGainIndex)` dB.
- On Airspy HF+ and RTL-SDR: decimation term only.
- Recompute and re-send whenever gain or decimation changes. Getting this wrong shows up as quantisation hiss at high decimation in uint8/int16 modes — add it to the test plan.

## 6. FFT stream → waterfall

- Each FFT message carries `FFT_DISPLAY_PIXELS` bins for the span `MaximumSampleRate >> FFT_DECIMATION` centred on `FFT_FREQUENCY`.
- **uint8 format:** one byte per bin, 0…255 mapping linearly across `FFT_DB_RANGE` dB starting at `FFT_DB_OFFSET`. Convert: `dB = offset + (value / 255) * range` — then feed the existing Skia auto-range path (this is philosophically the Kiwi quantised-waterfall model; reuse that colour pipeline, *not* the `uint8 − 256` UberSDR decode).
- **dint4:** two bins per byte, low nibble first, same scale at 4-bit resolution. Support optional (halves FFT bandwidth on slow links); uint8 first.
- The FFT view centre is independent of the IQ centre — which is exactly the unlocked-VFO/panning model from v5.1: pan moves FFT_FREQUENCY, tuning moves IQ_FREQUENCY, walls come from CLIENT_SYNC bounds and the hardware span.

## 7. IQ stream → VibeDSP

- Interleaved I,Q at the decimated rate. Normalise to float in the native layer: uint8 → `(x − 128)/128`, int16 → `x/32768`, int24 → 3-byte LE sign-extend `/2²³`, float32 passthrough. NEON-vectorise like the rtl_tcp ingest (uint8 path can share code).
- Feed the same ring buffer the rtl_tcp source uses; downstream (channel filter, demod, RDS, squelch, recording) is untouched.
- Choose IQ_DECIMATION so the slice comfortably covers the widest demod (WFM ≈ 200 kHz + RDS): smallest rate ≥ 250 kHz is a good default; drop to ≥ 48 kHz slices for SSB/CW on HF+ servers to save bandwidth. Expose as "stream quality" advanced setting.
- Retune policy: for small VFO moves keep IQ_FREQUENCY put and shift in DSP (mixer offset) to avoid stream glitches; re-centre the slice when the VFO approaches the slice edge (~80%), mirroring the dongle-follow logic from the second-VFO work.

## 8. UI / integration

- `BackendKind` + deep-link grammar gain `'spyserver'`; connect screen takes `host:port` (and later, entries from the Airspy public directory — **action item:** confirm the airspy.com/directory list format before building a browser; treat as Phase 2).
- Device banner from DEVICE_INFO: "Airspy HF+ · 0–31 MHz · via SpyServer".
- Spectator mode on `CanControl == 0`, with pan-within-capture still enabled per §3.4.
- Format/decimation restrictions (`ForcedIQFormat`, `MinimumIQDecimation`) surface as a quiet "server-limited quality" note, not an error.

## 9. Out of scope (v1)

- dint4 IQ, int24 IQ (accept if forced, but don't optimise), AF stream, server directory browser, gain control UI beyond a single slider when CanControl, multi-client awareness.

## 10. Acceptance checklist

- [ ] Connects to a public Airspy HF+ server and an RTL-SDR SpyServer; waterfall live within 1 s (FFT stream), audio after tune (IQ → VibeDSP).
- [ ] SSB/CW/AM/WFM(+RDS) all demodulate from the IQ slice; RDS parity with rtl_tcp on the same station.
- [ ] Digital gain correct: no added hiss stepping through decimation stages at fixed input.
- [ ] Spectator server: tuning within bounds works where permitted; out-of-bounds requests clamp without desync; RF-CENTRE marker + walls track CLIENT_SYNC.
- [ ] Sequence-number gap triggers a soft resync (flush DSP ring), not an audible pop.
- [ ] PING/PONG keepalive; dead-link detection ≤ 20 s; clean reconnect.
- [ ] Rotation/zoom re-sends FFT_DISPLAY_PIXELS; waterfall stays pixel-crisp.
- [ ] iOS 27: runs entirely on NWConnection; background audio unaffected.
- [ ] Licence audit: no SDR++/GPL third-party code in the diff; brief referenced in commit message as clean-room source.

---
73!
