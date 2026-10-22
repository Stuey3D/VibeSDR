# Icom transceiver support: research and plan

*Written 2026-10-21 for Stuart. Saber is the tester.* **Status 2026-10-22: Phase 0 (capture kit), Phase 1
server side and Phase 2 (macOS build) are BUILT on branch `v12-icom` and tested against a fake radio — see
"Built 2026-10-22" at the end, the client contract ("hwinfo + wire contract for clients (built)"), and
"Open for Stuart". Nothing has touched a real Icom yet.*
*Labels used throughout: **[verified]** means checked against a source or our own code. **[unverified]**
means believed true but not yet tested. **[decide]** means Stuart has to choose. **[Saber]** means
only a capture from Saber's radio can settle it.*

---

## 0. Summary

- **What Saber has:** "Mk1, USB only; the MK2 has USB and LAN". That is almost certainly an **IC-7300**.
  The IC-7300MK2 (2025) adds Ethernet (RS-BA1 v2 remote), USB-C with **two** virtual COM ports plus
  audio, HDMI and RX-antenna SMA ports [verified, Icom America product page and WiMo comparison].
- **An IC-7300 is not an IQ radio.** It has no IQ output of any kind. What it gives a computer is:
  - **CI-V over a USB serial port**, used for tuning, mode, filter, AGC, ATT, preamp and RF gain.
  - **Scope waveform data** (CI-V `27 00`): 475 points per sweep, amplitude 0-160, at most ±500 kHz
    (a 1 MHz span) [verified, Hamlib `ic7300.c`].
  - **Demodulated audio**, already through the radio's own filter, AGC and NR, on a USB Audio Class
    codec at 48 kHz/16-bit.
- **That makes it a new kind of source for VibeServer.** It is a *shared hardware tuner*, the model
  the app already uses for FM-DX (`FmdxAdapter`: one tuner, global tuning, the server demodulates).
  On VibeServer it means **shared-dial only**, with the spectrum taken from the radio's scope, audio
  injected after the demodulator stage, and tuning sent as CI-V.
- **IQ only exists higher up the range.** The **IC-7610** sends IQ at 1.92 MS/s, 16-bit, on its
  rear USB 3 port (FTDI "SuperSpeed-FIFO Bridge", `0c26:0029`, through the binary-only libftd3xx;
  firmware 1.20 or later) [verified, Icom I/Q supplement and df7cb's write-up]. The **IC-R8600** has an
  `[I/Q OUT]` port too (firmware 1.30 or later). Those two could later be ordinary IQ drivers with
  every VibeServer feature (§7, Phase 6).
- **Safety is the main design constraint.** The radio can transmit, so the design must never key it.
  There are three ways it could, and only one of them is the obvious PTT command (§3):
  1. CI-V commands whose **read and write forms differ only by a trailing data byte**. `1C 00` reads
     the TX state; `1C 00 01` *transmits*.
  2. **Serial modem lines.** If the owner set *USB SEND* or *USB Keying* to DTR or RTS (normal for
     digital-mode users), the act of **opening the serial port** can key the transmitter. Linux raises
     DTR and RTS on open by default.
  3. **Audio into the radio's USB MOD input** with VOX on. This is a real risk on Android, which
     routes system sounds to a newly attached USB audio device.
- **Licensing: clean-room it.** VibeSDR is GPLv3, so wfview's GPLv3 code is licence-compatible *as
  source*. However, `APPSTORE-EXCEPTION.md` only works because Stuart is the **sole copyright holder**.
  wfview code in any App Store or Play binary would break that exception. So the plan writes our own
  small CI-V layer from Icom's published CI-V Reference Guide. For the LAN protocol, which Icom has
  never published, we either clean-room it from packet captures or ask the wfview authors for the same
  section-7 permission (§4).
- **Smallest useful first step:** IC-7300 over USB on Linux VibeServer, receive-only, with CI-V tune
  and mode, the scope as the waterfall, and USB audio, in one shared dial (§7, Phase 1). It is about
  3-4 weeks of work plus Saber's test turnarounds.

---

## 1. How the radios expose themselves

### 1.1 IC-7300 (Mk1): USB only

| Item | Detail | Status |
|---|---|---|
| USB serial | Silicon Labs CP2102 bridge (`10c4:ea60`). Shows up as `/dev/ttyUSB0` on Linux and `/dev/cu.SLAB_USBtoUART` on macOS. | [unverified] → **[Saber]** `lsusb` |
| CI-V address | `0x94` by default. Hamlib `IC7300_priv_caps`. | [verified] |
| Baud | 4800-115200, plus *Auto*. Scope streaming needs **115200**, and the menu needs *CI-V USB Port = Unlink from [REMOTE]* (DXLab and wfview users both say so). | [verified] |
| Echo | *CI-V USB Echo Back* setting. Hamlib notes "USB CI-V may not echo", so the parser must accept either. | [verified] |
| Transceive | *CI-V Transceive ON* makes the radio announce front-panel changes unasked (`00` freq, `01` mode). This is how we follow the owner at the knob. | [verified] |
| Audio | A TI PCM2900-family "USB Audio CODEC" (`08bb:29xx`), 48 kHz, 16-bit, stereo. RX audio is the same on both channels. Its level is set by *USB AF Output Level* (`1A 05 00 60`), separately from the front AF knob, so the owner's volume does not reach listeners. | [unverified] chip ID; [verified] the level command |
| AF/IF select | Menu `1A 05 00 59`. With *IF* selected, USB audio carries a **12 kHz IF about 15 kHz wide** instead of audio. This is a window our own demodulators could run in (Phase 7). | [verified] command; [unverified] IF figures |

**CI-V frame:** `FE FE <to> <from> <cmd> [<sub>] [<data…>] FD`. The controller address is
conventionally `E0`. Frequencies are 5 bytes of BCD, least significant first. Replies are `FB` (OK)
or `FA` (NG).

**Receive controls we need** (command numbers from Hamlib's `icom_defs.h`, cross-checked against the
Icom reference):

| Control | Read | Set | Notes |
|---|---|---|---|
| Frequency | `03` | `05` + 5 BCD | `25 00` / `25 01` for the selected or unselected VFO on newer firmware |
| Mode + filter | `04` | `06` mode fil | Modes: LSB 00, USB 01, AM 02, CW 03, RTTY 04, FM 05, CW-R 07, RTTY-R 08. Filter: FIL1/2/3. |
| Data mode | `1A 06` | `1A 06` d fil | USB-D for FT8 and similar |
| Filter width | `1A 03` | `1A 03` n | Continuous width index; it depends on mode |
| Attenuator | `11` | `11 00` / `11 20` | Off or 20 dB only (Hamlib `.attenuator = {20}`) |
| Preamp | `16 02` | `16 02 00/01/02` | Off, P.AMP1, P.AMP2 |
| AGC | `16 12` | `16 12 01/02/03` | FAST, MID, SLOW. The time constant is `1A 04`. |
| RF gain | `14 02` | `14 02` 0000-0255 (BCD) | |
| NB / NR / ANF | `16 22` / `16 40` / `16 41` | same, with 00/01 | The radio's own DSP |
| Squelch | `14 03` | `14 03` | |
| S-meter | `15 02` | — | 0-255. Hamlib `IC7300_STR_CAL` maps it to S-units. |
| TX state (read only) | `1C 00` | **never** | Used to blank audio while the owner transmits (§3.4) |
| Radio ID | `19 00` | — | Model check on connect |

### 1.2 The scope (CI-V `27`)

| Sub | Meaning |
|---|---|
| `27 00` | Waveform data (the radio *sends* it; the controller never writes it) |
| `27 10` | Scope on/off |
| `27 11` | **Scope data output to CI-V on/off.** Must be ON, or nothing is streamed |
| `27 14` | Centre / Fixed / Scroll-C / Scroll-F |
| `27 15` | Span (centre mode) |
| `27 16` | Fixed-edge set selection |
| `27 17` | Hold |
| `27 19` | Reference level, −20 to +20 dB in 0.5 dB steps |
| `27 1A` | Sweep speed: fast / mid / slow |
| `27 1C` | Centre type (filter centre / carrier point) |

**IC-7300 scope caps** [verified, Hamlib]: 475 points per line, amplitude 0-160 (Hamlib maps this to
−80…0 dB, so 0.5 dB steps). Spans in centre mode are **5, 10, 20, 50, 100, 200, 500 and 1000 kHz
total** (±2.5 … ±500 kHz). Fixed-mode edge sets run per band range. The radio's own rated sweep is
about 30 frames/s.

**Waveform framing over USB** [verified in outline from the IC-7610 reference; **[Saber]** for exact
7300 bytes]. One sweep arrives as several CI-V frames, numbered `seq/total` in BCD:

```
FE FE E0 94 27 00 00 <seq=01> <total=11> <mode> <freq A: 5 BCD> <freq B: 5 BCD> <out-of-range> FD
FE FE E0 94 27 00 00 <seq=02> <total=11> <50 amplitude bytes> FD
...                                       (divisions 2-10 carry 50 bytes, division 11 carries 25)
```

In centre mode, A is the centre frequency and B is the half-span. In fixed mode, A and B are the
lower and upper edges. Hamlib's `single_frame_data_length = 50` fits 9 × 50 + 25 = 475 exactly.
**Over LAN the whole sweep arrives in one division (`01/01`).**

**Why Saber sees low fps over USB.** One sweep is about 475 data bytes plus about 10 bytes of framing
per frame × 11 frames, plus the header: roughly **600 bytes, or 6000 bits on the wire at 10 bits a
byte**. At 115200 baud that sets a ceiling of about **19 sweeps/s** with nothing else on the port.
Each S-meter poll or tuning command takes a share of that. At 19200 baud, which is a common default,
the ceiling falls to **about 3 sweeps/s**. That would match "low fps".
**[Saber]**: what baud is wfweb set to, and does the radio show *Unlink from [REMOTE]*? The ceiling
is arithmetic and has not been measured. Phase 0 measures it.

### 1.3 What the MK2, IC-705, IC-9700 and IC-7610 add

| Radio | CI-V addr | Scope | Links | IQ |
|---|---|---|---|---|
| IC-7300 | `94` | 475 pts, 0-160 | USB (CP2102 + codec) | **none** [verified] |
| IC-7300MK2 | `B6` [verified, Hamlib] | 475 pts | USB-C (2 COM + audio) + **LAN** | none [unverified] |
| IC-705 | `A4` | 475 pts | USB + **Wi-Fi LAN** | none |
| IC-9700 | `A2` | 475 pts, main/sub | USB + **LAN** | none |
| IC-7610 | `98` | **689 pts, 0-200**, dual scope [verified, Hamlib] | USB + **LAN** + USB 3 IQ | **1.92 MS/s 16-bit** [verified] |
| IC-R8600 | `96` | yes | USB + LAN + USB 3 IQ | yes; rates **[unverified]** |

**The Icom LAN protocol** (RS-BA1 compatible, reverse-engineered by wfview and Spectrum Lab):
- **Three UDP streams:** control on **50001**, CI-V "serial" on **50002**, and audio on **50003**
  [verified, wavecom app note and Spectrum Lab].
- **Packet types:** a 0x10-byte control packet (are-you-there / I-am-here / are-you-ready /
  disconnect), pings every 500 ms, sequence numbers with retransmit requests, a 0x80-byte login, a
  token renewed every 60 s, a 0x90-byte connection-info packet, and a capabilities packet listing
  each radio's name, CI-V address and supported sample rates [verified, wfview `packettypes.h`
  sizes and fields].
- **The connection-info packet has explicit `rxenable` and `txenable` bytes.** We send
  `txenable = 0`, so the radio never opens a TX audio path for our session. This is a second,
  protocol-level lock, and USB has no equivalent [verified, field exists; **[unverified]** that every
  firmware honours it].
- **Login credentials** are the radio's *Network User* name and password, under a fixed substitution
  obfuscation. That is **not encryption**, so this protocol must never cross the internet in the
  clear. Keep it LAN-only and let VibeServer, which already has TLS and tunnels, be the internet face.
- **Audio** is negotiated as uLaw, PCM8 or PCM16, mono or stereo, at 8/16/24/48 kHz, with Opus on
  some firmwares **[unverified per model]**.
- **The scope over LAN** arrives one sweep per CI-V frame on the 50002 stream, at the radio's full
  rate (about 30 fps).
- **The radio allows only a limited number of remote sessions**, and the info packet carries a `busy`
  flag. While VibeServer holds a session, the owner's RS-BA1 or wfview may not be able to connect
  **[Saber, on the MK2]**.

---

## 2. How it maps onto VibeServer

### 2.1 What VibeServer is today [verified, our code]

- The engine is `android/app/src/main/cpp/local_sdr_shim.cpp` (about 34 k lines), shared by Linux,
  macOS and Android. Each radio is a `*_source.{h,cpp}` class (`AirspyHfSource`, `SdrplaySource`,
  `HackRfSource`, `AirspySource`, plus the built-in RTL path and the rtl_tcp / SpyServer readers)
  that pushes **IQ** into `enqueueIq` / `enqueueIqInt16` / `enqueueIqFloat`.
- Everything downstream assumes IQ: the FFT and SPEC frames, the demodulators, VibeAGC, the IF ladder,
  RDS and DAB, raw IQ out, and per-listener VFOs (`perClientDsp()`, which is locked centre plus more
  than one user).
- **`radioCapsJsonCore()` is the hwinfo contract.** Each driver publishes `"radio":{"driver":…}` with
  exactly the controls it has, and the client draws only those (`RadioCaps` in
  `src/services/sdrProtocol.ts`, plus `LocalHardwarePanel.tsx`, `ServerModeScreen.tsx` and
  `web/client/src/main.ts`).
  ★ That function's own comments record that a new driver **must get its own branch before the RTL
  fallthrough**, or it is announced as a dongle. That happened to the HF+ in July.
- `vibeserver/radios.cpp` has `detectRadios()`, one flat ordered list. New drivers are appended so
  that no existing `--radio N` moves.
- **Non-IQ precedent, server side:** the **decoder-only sidecar** (`startDecoderService` /
  `feedDecoderPcm`) already feeds PCM audio, with no IQ, through `vibe_decfeed_resampler.h` into the
  decoder host at 48 kHz. That is the pattern for Icom audio reaching FT8, RTTY and WEFAX.
- **Non-IQ precedent, client side:** `FmdxAdapter` (one shared hardware tuner, the server
  demodulates, no client DSP) and `KiwiAdapter` / `OwrxAdapter` (the server sends demodulated audio
  plus dB bins). The app already knows how to show a receiver that is not ours.
- **No serial port or audio-capture code exists anywhere in the server today** [verified, grep for
  `termios` / `snd_pcm` / `AudioUnit`]. Both are new.

### 2.2 The new source: `IcomSource` (driver `"icom"`)

```
             ┌──────────── CI-V (serial / UDP 50002) ────────────┐
radio ──────►│ CivLink: framer, allow-list, request/response,     │──► tune/mode/AGC/ATT/preamp/RF
             │ transceive listener (front-panel changes)          │◄── dial state → hwinfo + `dial`
             │ ScopeAssembler: 27 00 divisions → one sweep         │──► SPEC frame (475 bins + edges)
             └────────────────────────────────────────────────────┘
radio audio ─► AudioCapture (ALSA / CoreAudio / AudioRecord / UDP 50003) ─► 48 kHz PCM
                     └─► straight into the shared-dial audio fan-out (Opus encode, after the demod stage)
                     └─► decoder host (feedShared), exactly as the sidecar does
```

- **Always a shared dial.** `perClientDsp()` must return false for `icom` whatever the owner's
  locked-centre setting, because there is one physical receiver. That gives the
  shared-dial contract for free: the radio *is* the server state. The front panel is
  another "user", announced by CI-V transceive, and the server re-broadcasts the full `dial` state.
  The client re-adopts and never re-asserts.
- **Spectrum.** Each assembled sweep becomes a SPEC frame. Its edges come from the sweep header
  (centre ± half-span, or fixed edges), and it carries 475 bins (689 on a 7610) resampled to the wire
  bin count the client asked for (`?bins=`). The amplitude is mapped from 0-160 to the dB scale the
  wire uses, with the radio's REF level folded in.
  - **The view follows the radio.** In centre mode the window moves with every tune. In fixed mode the
    VFO marker moves inside a fixed window, which is the analogue of our locked centre.
  - **Frame rate is the radio's, not ours.** The spectrum governor and fps tiers do not apply. The
    server publishes the rate it actually measures.
- **Zoom becomes the span ladder.** The zoom drum steps through `27 15` spans (8 rungs, 5 kHz to
  1 MHz). No digital zoom past 475 points is offered: zooming the picture would invent resolution.
- **Audio** goes in *after* demodulation. The IQ path, demodulator, VibeAGC and IF filter are all
  bypassed. Our audio-domain post-processing (Opus encode, recording, the Record Buffer, client-side
  NR, L/R meters on mono) still works.
- **S-meter** comes from `15 02` polled at a low rate (about 4 Hz), so it does not starve the scope on
  USB, and is converted with the published calibration points.

### 2.3 What does **not** apply on an Icom (non-IQ)

| Feature | Why |
|---|---|
| Our demodulators (AM/SAM/ECSS/C-QUAM/NFM/WFM/SSB), VibeAGC, the IF filter ladder and Auto IF, the gain-step and AGC history machinery | The radio has already demodulated. Mode and filter go to the radio instead. |
| Multiple listeners on different frequencies, locked centre, multi-VFO | One receiver. The IC-7610's **main/sub** could later give two dials. |
| Waterfall wider than 1 MHz, or finer than 475 points | That is what the scope provides |
| Spectrum fps controls, the spectrum governor, the 4-slot queue | The rate is set by the radio and the link |
| RDS, Advanced RDS, MPX, DAB, stereo pilot | HF transceivers do not demodulate wideband FM to MPX. An R8600's WFM would be audio-only. |
| Raw IQ out, IQ recording, IQ pairing | No IQ |
| Sample-rate picker, direct sampling, bias-T, ppm/ppb, the dongle EEPROM | Not radio properties |
| Wideband or IQ-based decoders (HFDL, satellite and Doppler engine work) | They need IQ. **Audio decoders work** (FT8/FT4, RTTY, CW, WEFAX, SSTV) inside the one passband. |
| Band defaults applied on a person's tune | Partly. Mode yes, but the IF width maps to FIL1/2/3, not our ladder. |

### 2.4 The UI must branch on `radio.driver === 'icom'` (AGENTS.md)

The server publishes, for example:

```json
"radio":{"driver":"icom","model":"IC-7300","civ":"94","link":"usb","noIq":true,"sharedOnly":true,
 "modes":["lsb","usb","am","cw","cwr","rtty","rttyr","fm","usbd","lsbd"],
 "filters":[1,2,3],"att":[0,20],"preamp":[0,1,2],"agc":["fast","mid","slow"],
 "rfGain":[0,255],"nb":true,"nr":true,"anf":true,
 "spans":[5000,10000,20000,50000,100000,200000,500000,1000000],"scopePoints":475,
 "scopeMode":"centre","scopeFps":14.2,"ranges":[[30000,74800000]],
 "tx":false,"ownerLock":false}
```

- **Draw for `icom`:** a Preamp segment (OFF/P1/P2), ATT (OFF/20 dB), AGC (FAST/MID/SLOW), an RF gain
  slider, FIL1/2/3 (and width), NB/NR/ANF toggles, a span selector (the zoom drum), and a TX banner.
  Every list comes from hwinfo, never a constant (`feedback_app_no_hardcoded_settings`).
- **Hide for `icom`:** the sample rate, VibeAGC, the IF ladder, the gain-table slider, direct
  sampling, bias-T, ppm, raw IQ, Spatialise Stereo, the RDS panels, multi-VFO and fps tiers.
  ★ Every one of these must actually disappear on all three clients (app, web, watch). A control left
  visible and inert is the thing AGENTS.md forbids.
- **The `RadioCaps` comment in `sdrProtocol.ts` gets an Icom section.** The web `main.ts` and
  `LocalHardwarePanel` driver switches get an `icom` arm *before* any default. `radios.cpp` appends
  `icom` after `airspy`.
- **Copy:** when this ships, update the grep list in AGENTS.md (`sdrTour`, `pickerTour`,
  `AboutOverlay`, the watch `TutorialSheet`s, `website/index.html`) for "Icom". Where a control
  differs (zoom = span), say so in the tour.

---

## 3. Safety: receive only, provably

Saber keeps wfweb local-only *because it can transmit*. That bar is the one to meet: **VibeServer
must be incapable of keying the radio**, and no listener, admin or bug may change that.

### 3.1 No TX code at all

- There is **no `ptt()` function** and no TX audio path. Nothing ever opens the radio's USB MOD input
  for playback, and on LAN `txenable = 0` and no packet is sent on the TX audio stream. The CI-V
  layer has **no code path that builds `1C 00 01`, `17`, `28` or `1C 01 02`**.
- **Clients never send raw CI-V.** They send the same semantic JSON as today (`tune`, `mode`, `att`,
  and so on). The server alone builds frames, from a typed table.

### 3.2 An outgoing allow-list on whole frames, not command bytes

★★ **The read and write forms of the same command differ only by data length.** `1C 00` (no data)
reads the TX state; `1C 00 01` transmits. `18` is power (`18 00` is OFF). `1A 05 xx xx` reads a menu
item, and the same bytes with a trailing value *write* it. A filter on command and subcommand alone
would pass the transmit form of every read we use.

So the last function before the port write, `civ::mayTransmit(frame)`, checks the **complete frame**
against a static table of `(cmd, sub, exact data length, value range)`:

| Allowed to write | Data |
|---|---|
| `05` set freq | exactly 5 BCD bytes, inside the published RX `ranges` |
| `06` set mode | mode ∈ RX list, filter 1-3 |
| `1A 06` data mode, `1A 03` filter width | in range |
| `11` ATT | `00` or `20` |
| `16 02` preamp, `16 12` AGC, `16 22` NB, `16 40` NR, `16 41` ANF | in range |
| `14 02` RF gain, `14 03` squelch | 0000-0255 |
| `27 10`, `27 11` scope on and data out, `27 14` mode, `27 15` span, `27 17` hold, `27 19` REF, `27 1A` speed | in range |
| Reads (no data byte): `03`, `04`, `15 02`, `1C 00`, `19 00`, `1A 05 <item>` for the preflight items only | **zero data bytes, checked** |

Anything else is **dropped and logged as a fault**, never sent. That includes `1C xx` with data,
`17`, `28`, `18`, `0F` split, `0D` offset, `09`/`0B` memory write and clear, `1A 00`/`1A 02` memory
and band stack, **`1A 05` with data** (all menu writes: TX power, USB SEND, VOX settings), `14 0A`
RF power, `14 0B` mic, `16 46` VOX, `16 47` break-in, `1E` band edges, `0E` scan, `1A 01`, and any
`FE` preamble (power-on wake).
- **Unit-tested exhaustively.** The test enumerates every `cmd`/`sub` byte pair with 0-6 data bytes
  and asserts that only the table passes. It asserts the specific frames `1C 00 01`, `1C 01 02`,
  `17 …`, `28 …`, `18 00` and `1A 05 00 xx 01` are refused. It lives in `vibeserver/test-icom-civ.cpp`
  and joins the standard test run.
- **The allow-list also applies to the client's direct-LAN mode** (Phase 5). It is shared C++, so
  there is one rule with two readers.

### 3.3 Modem lines and preflight (the non-obvious TX path)

The IC-7300 menu *Set > Connectors* has **USB SEND** (OFF/DTR/RTS) and **USB Keying (CW)** /
**USB Keying (RTTY)** (OFF/DTR/RTS) **[unverified exact names, Saber to read off the radio]**. Digital
-mode users routinely set USB SEND = RTS. In that state, **asserting RTS keys the transmitter**.

- **Linux asserts DTR and RTS when a tty is opened.** The cp210x driver does it in `dtr_rts()` on
  open, before user space can intervene. Clearing them with `TIOCMBIC` right after `open()` still
  leaves a glitch.
- **Fix: do not use the tty.** VibeServer already opens SDRs with libusb, including on Android by fd.
  Driving the **CP2102 directly over libusb** (the vendor requests IFC_ENABLE, SET_BAUDRATE,
  SET_LINE_CTL and SET_MHS, plus bulk in/out) means **DTR and RTS are never asserted**. We send
  `SET_MHS` with both lines low explicitly, before anything else. On Linux this detaches `cp210x`
  from that one interface for the session. **[unverified]**: the CP2102 power-on line state, to be
  checked on Saber's radio into a **dummy load**.
- **Preflight on every connect,** read-only: `1A 05` reads of USB SEND, USB Keying ×2, VOX (`16 46`),
  and the DATA MOD source. If any keying line or VOX is enabled, the server **refuses to start the
  radio**. The admin page names the exact menu item to change. We never write the setting ourselves.
- **Android audio routing.** When a USB audio device appears, Android may route system and
  notification output to it, which goes into the radio's MOD input. With VOX off and no PTT this
  cannot transmit, but it is one reason the VOX preflight is a hard gate.

### 3.4 While the owner transmits

- Poll `1C 00` (read) at about 2 Hz, and use the scope header's TX flag where present. During TX:
  **mute the stream** (listeners must not hear the owner's monitor audio), freeze the waterfall, and
  show a "station transmitting" banner (`radio.tx`).
- **Owner lock [decide]:** an admin switch, "Listeners follow the radio". When it is on, remote
  listeners cannot tune at all; they hear whatever the owner is on. It defaults **ON**, because Saber
  operates the radio himself and a stranger retuning his radio mid-QSO is the failure he fears. When
  it is off, tune and mode are open to listeners as on any shared dial. RF settings (ATT, preamp, RF
  gain, AGC) are admin-only either way **[decide]**.

### 3.5 Legal note

Receive-only rebroadcast is the WebSDR / Kiwi norm. Nothing here operates the transmitter, so no
remote-operation licence conditions are engaged. Muting during TX also keeps the owner's own
transmission out of the public stream.

---

## 4. Licensing

| Option | Verdict |
|---|---|
| **Copy wfview or wfweb code** (GPLv3) | Compatible with VibeSDR's GPLv3 **as source**. But `APPSTORE-EXCEPTION.md` relies on Stuart being **sole copyright holder**, and he cannot grant a section-7 store exception for wfview's authors' code. So that code could never enter the iOS app, the Play app or Lite. ✗ for anything shared with the app (§5 Phase 5), and a trap even in the server because the shim is shared. |
| **Ask wfview's authors** (Elliott Liggett W6EL, Phil Taylor M0VSE and others) for the same store exception on their LAN code **[decide]** | It would legitimately save the most work on the LAN protocol. A polite email costs nothing. |
| **Hamlib** (LGPL-2.1) | Licence-compatible for the server. But it is a large dependency against the "no external deps" deb rule. It gives us no control over what it sends on open, which undermines §3.2. It does not do the Icom LAN audio protocol. Static linking LGPL into an App Store binary is awkward. ✗ as a runtime dependency. ✓ **as a reference for facts** (CI-V addresses, scope caps, S-meter calibration points), which are data, not expression. |
| **Clean-room from Icom's own *CI-V Reference Guide*** (published free per model) | ✓ **Recommended for CI-V and the scope.** About 20 commands and one reassembler, a few hundred lines. This is also what makes §3.2 auditable. |
| **LAN protocol, clean-room** | Icom has never published it. We would write it from our own packet captures of the radio talking to RS-BA1 (observing traffic is fine), with struct layouts written from those captures, not copied from `packettypes.h`. More work. Needs a MK2/705/9700/7610 owner. |

`tinyalsa` (BSD-3) can be vendored for Linux capture, so there is no `libasound2` dependency
(`feedback_linux_deb_no_external_deps`).

---

## 5. Platforms

| Platform | Serial | Audio | Verdict |
|---|---|---|---|
| **Linux / Pi VibeServer** | libusb straight to the CP2102 (§3.3). Fallback: termios on `/dev/ttyUSB*` with lines cleared, for CDC-ACM radios (705/9700/MK2 present native USB CDC; **[unverified]**). udev rule: add the radio's VID:PIDs to `99-vibeserver-sdr.rules`, **and `ENV{ID_MM_DEVICE_IGNORE}="1"`**, because ModemManager probes CP210x ports with AT commands. | Vendored tinyalsa on `/dev/snd/pcmC?D?c`, with the `vibeserver` user in `audio`. A headless Pi has no PulseAudio or PipeWire holding the device; a desktop might. | ✓ **Phase 1.** The Pi 2 can do it easily: no FFT and no demod, only Opus encoding. |
| **macOS VibeServer** | The CP210x is visible as `/dev/cu.SLAB_USBtoUART` with Apple's DriverKit driver (**[unverified]** on our macOS), or by libusb as above. | CoreAudio (AUHAL) input. That needs **`com.apple.security.device.audio-input`**, `NSMicrophoneUsageDescription` and a TCC microphone prompt. The app is signed and notarised as always. | ✓ Phase 2 |
| **Android (VibeServer Lite / app server)** | UsbManager fd → our libusb, as for SDRs. CP210x vendor requests in C, no kernel driver, so no stray DTR/RTS. CDC-ACM is equally simple. | The kernel's `snd-usb-audio` usually claims the codec, so libusb cannot. Use **AudioRecord with `setPreferredDevice(TYPE_USB_DEVICE)`**, which needs `RECORD_AUDIO` and a **microphone-type foreground service** (Android 14 rules: it must start while in the foreground). Support varies by phone and kernel. | ⚠ Phase 4, risky. Prove it on the XCover first. On potato phones it is a bonus, not a promise. |
| **Client direct to a LAN radio** (705/7610/9700/MK2) | UDP 50001/2/3 from a native module (iOS local-network permission, which we already have for mDNS) | Radio audio over UDP, PCM16 or Opus | ⚠ Phase 5. It needs the LAN protocol *in the app*, so licence-clean code only (§4). It is an owner-only "local hardware" mode, and the same §3.2 allow-list is compiled in. Internet listeners still go through VibeServer. |
| Windows | Follows the Windows port plan | WASAPI | Later |

**Exclusive port.** Only one program can hold the serial port. While VibeServer serves the radio,
Saber's wfweb cannot talk CI-V over USB. On the MK2 the second COM port or LAN could serve wfweb
alongside **[Saber]**. Say so plainly in setup.

---

## 6. Hardware notes for later models

- **IC-7610 IQ (Phase 6):** this is a *real* IQ driver, `icomiq`. IQ comes from the FTDI SuperSpeed
  FIFO (`0c26:0029`) at 1.92 MS/s cs16, enabled by CI-V `1A 0B 01` and disabled by `1A 0B 00`, with
  tuning over CI-V. libftd3xx is **binary-only**, so it would be dlopened at runtime like the SDRplay
  API, or replaced by our own libusb FT60x bulk reader if the protocol is simple enough (df7cb found
  libftdi1 does not work). With this, every VibeServer feature applies: our demods, multi-VFO,
  decoders and raw IQ.
- **IC-R8600 IQ:** the same family. Wider rates are **[unverified]**. It would be the best
  VibeServer Icom of all, but it needs a tester.
- **IC-7610 dual receiver:** main and sub could become **two shared dials** on one server.
- **IC-9700:** three bands and satellite mode. The v12 satellite engine needs IQ, so this would be
  audio and scope only.

---

## 7. Phased plan

Effort is in agent-working days and excludes waiting for Saber. Each phase ends with a release-
ready, auditable commit series. Every phase follows "one build with everything" and the CPU caps.

### Phase 0: capture kit (1-2 days)
- Add `tools/icom/civ-capture.py`: pyserial, **read-only plus scope-on**. It sends only `19 00`,
  `03`, `04`, `15 02`, `27 10 01`, `27 11 01`, and on exit `27 11 00`. It uses the same allow-list
  table as §3.2, with RTS and DTR forced low before the first byte. It writes a raw byte log.
- Saber runs it for 5 minutes with the radio **into a dummy load**, while tuning, changing span, and
  switching centre/fixed.
- **Saber reports:** `lsusb -v` for the radio, firmware version, CI-V baud / Unlink / Transceive /
  Echo settings, USB SEND / USB Keying / VOX values, AF/IF select, and the wfweb baud.
- Output: real 7300 frames committed under `test/fixtures/icom/` for the assembler tests, plus the
  **measured** sweeps/s at 115200.

### Phase 1: IC-7300 over USB on Linux VibeServer (about 3-4 weeks)
1. `civ_codec.h` (pure: framing, BCD, typed builders, `mayTransmit`) and
   `test-icom-civ.cpp` with the exhaustive allow-list test. *(4 days)*
2. `IcomSource`: the CP2102 over libusb with lines held low, the request queue (scope traffic and
   polls share one wire, and tuning comes first, per the thread priority rule), transceive listener,
   preflight gate, and the device-lost watchdog. *(5 days)*
3. `ScopeAssembler` (replayed against the Phase 0 fixtures) → SPEC frames with the real edges, the
   span ladder as zoom. *(3 days)*
4. Audio: vendored tinyalsa → 48 kHz → the shared-dial audio fan-out and the decoder host (the
   `feedDecoderPcm` pattern). Mute on TX. *(3 days)*
5. hwinfo `radio:{driver:"icom",…}`, `radios.cpp` detection, forced shared dial, owner lock, admin
   setup page entry, udev and ModemManager rules. *(3 days)*
6. Clients: an `icom` arm in `RadioCaps`, `LocalHardwarePanel`, `ServerModeScreen` and web `main.ts`.
   Hide what does not apply (§2.3). Tour and About copy. *(5 days)*

**What Saber tests:**
- (a) a dummy-load session; the preflight refuses with USB SEND = RTS and passes with OFF
- (b) tune, mode and filter from the web and app, with the radio's display following
- (c) the front-panel knob, with VibeServer following within one sweep
- (d) waterfall fps compared with the radio
- (e) audio quality and latency
- (f) FT8 decoding on 14.074 USB-D
- (g) keying the radio by hand: the stream mutes and the banner shows
- (h) an hour's soak with two listeners

### Phase 2: macOS VibeServer (about 1 week)
CoreAudio capture, the TCC microphone prompt, the CP210x via libusb, signed and notarised. Saber or
Stuart tests on a Mac.

### Phase 3: LAN in VibeServer for the MK2 / 705 / 9700 / 7610 (2-3 weeks)
The control/CI-V/audio UDP streams with `txenable = 0`. The scope arrives at full rate over LAN, which
is the fix for "low fps". Clean-room from captures, or the wfview exception (§4 **[decide]**). It
needs a LAN-radio owner, so ask on Discord.

### Phase 4: Android VibeServer Lite over USB (about 2 weeks, risky)
UsbManager fd for CI-V, AudioRecord on the USB device, a microphone foreground service. XCover first.

### Phase 5: app connects straight to a LAN Icom (2-3 weeks plus store review)
Owner-only local mode. It reuses the Phase 3 C++ (licence-clean), compiled into iOS and Android.
Batch it with the next store release (`feedback_no_micro_store_updates`).

### Phase 6: IQ Icoms (IC-7610, IC-R8600) as true IQ drivers (2-3 weeks, needs a tester)

### Phase 7: optional "IF mode" on the IC-7300 [decide]
If the owner selects AF/IF = IF, the 12 kHz IF window (about 15 kHz) could feed our own demodulators,
SAM and decoders, giving a narrow IQ-like path. It changes the owner's menu, so it is the owner's
choice, never ours.

---

## 8. Top risks

1. **Accidental transmission.** The read and write CI-V forms are identical apart from data length.
   DTR and RTS are raised on serial open when the owner has set USB SEND or Keying. VOX with Android
   routing system audio to the radio. All three are designed out (§3) and must be tested into a dummy
   load before any real-antenna test. **Nothing ships until the exhaustive allow-list test and Saber's
   preflight test pass.**
2. **Licensing and the LAN protocol.** wfview code would silently void the App Store exception. The
   LAN protocol has no public spec, so Phases 3 and 5 depend on either clean-room captures from a LAN
   radio we do not have, or the wfview authors' goodwill.
3. **A non-IQ source in an IQ-shaped engine, on one tester.** The 34 k-line shim assumes IQ almost
   everywhere: sample rate, view centre, zoom, AGC, `perClientDsp`, watchdog and fps. Each assumption
   is a place where an Icom could be mis-described, like the HF+ that was announced as a dongle. Scope
   fps over USB is capped by baud (about 19 sweeps/s at best). And the radio's serial port is
   exclusive, so Saber cannot run wfweb and VibeServer together on the Mk1.

---

## Sources

- Hamlib `rigs/icom/ic7300.c`, `ic7610.c`, `icom_defs.h` (master, read 2026-10-21): scope caps, spans,
  CI-V addresses, command numbers.
- wfview `include/packettypes.h` (GitLab eliggett/wfview, master): LAN packet sizes and the conninfo
  `rxenable`/`txenable` fields. Read for facts only.
- [Icom America IC-7300MK2](https://icomamerica.com/lineup/products/IC-7300MK2/) ·
  [WiMo: IC-7300MK2 vs IC-7300](https://www.wimo.com/en/blog/post/icom-ic-7300mk2-vs-ic-7300)
- [IC-7610 I/Q supplement](https://k0pir.us/wp-content/uploads/2018/08/IC-7610_ENG_I_Q_Supp_0.pdf) ·
  [df7cb: IC-7610 IQ via libftd3xx](https://www.df7cb.de/blog/2023/ic7610ftdi.html) ·
  [Icom USB I/Q package](https://icomamerica.com/support/firmware_driver/1956/)
- [wavecom IC-9100 remote note (ports 50001-3)](https://wavecom.ch/content/pdf/application_note_icom-ic-9100-remote-control.pdf) ·
  [libre.space: IC-9700 LAN](https://community.libre.space/t/satnogs-with-icom-ic-9700-audio-via-ethernet-udp/14679)
- [DXLab: Icom spectrum over USB at 115200](https://dxlabsuite.com/dxlabwiki/SpectrumWaterfallIcom) ·
  [IC-7610 CI-V info (scope 27 00 divisions)](https://icomfrance.com/uploads/files/produit/not-IC-7610_ENG_Info_V130_0a-en.pdf)
- [wfweb](https://github.com/adecarolis/wfweb): GPL-3.0, "headless wfview fork".

## Phase TX — owner transmit (decided by Stuart, 2026-10-21; after receive-only Phase 1 ships)

A TX button exists only when ALL of these hold. Every check is the SERVER's; the client has no vote.

1. **"Allow transmit" is switched on in setup.** Default OFF. Plugging an Icom in never produces a TX button.
2. **The admin password is entered** for this session.
3. **The connection is from the local network.** A direct TCP peer in a private range (RFC 1918 or IPv6 ULA/link-local)
   on a subnet of one of the box's own interfaces. NOT local, ever: loopback (a tunnel or reverse proxy on the box
   arrives as 127.0.0.1), the directory relay/tunnel, any request carrying X-Forwarded-For / Forwarded, and VPN
   ranges (Tailscale 100.64/10, WireGuard interfaces). Stuart: VPNs stay remote — "at least if they are on their local
   network we know they are in the vicinity of the transmitter".

Safeguards:
- **Hold to talk**, never a latch. A **3 minute** TX timeout unkeys regardless.
- **Dead-man**: socket drop or > ~500 ms without TX audio → unkey at once. A frozen tab must never leave it keyed.
- **One transmitter at a time.** Listeners see the TX banner; the RX stream mutes while keyed.
- The admin's live TX session is the ONLY thing that opens the TX frames in the CI-V allow-list. The DTR/RTS rule
  (libusb, lines held low) and the VOX / USB SEND / USB Keying preflight stay exactly as in Phase 1 — keying is CI-V
  `1C 00 01` / `1C 00 00` from the server only.
- **Microphone**: the native app can capture at once. The web client needs a secure context, i.e. HTTPS on the LAN —
  shared work with GitHub #52 (self-signed certificate trust).
- Tested into a dummy load with Saber before any antenna, like the RX phases.
- Licensing is the operator's responsibility; the setup switch says so in plain words. Local-only means the operator
  is physically near the station.

## Phase TX-remote — the owner's PAIRED app transmits from anywhere (Stuart, 2026-10-21; after Phase TX)

Stuart: "Pairing code with the app when setting up, only the server owner's app is paired."

- **Pairing at setup, on the LAN.** The setup/admin page (reached locally) shows a short code (6 digits, single use,
  expires in 5 minutes). The owner types it into the VibeSDR app while the phone is on the same network. The app and
  server swap a key pair; the private key lives in the iOS Keychain / Android Keystore, never leaves the phone.
- **Only the paired app can transmit remotely.** Not the web client, not any other phone, not a password alone.
  Remote TX = paired device key + admin session + "Allow remote transmit" switch ON (separate from local TX, default
  OFF). Before each TX session the app asks for Face ID / fingerprint, so a lost unlocked phone cannot key the rig.
- **Revoke and re-pair** from the admin page (lost phone, new phone). Re-pairing is only possible on the LAN.
- **Encrypted, pinned connection required** — the server is plain HTTP today. Shared work with GitHub #52.
- Phase TX safeguards all apply. **Instant unkey on loss of connection** (Stuart, 2026-10-21): socket drop or
  > ~500 ms without TX audio → the server sends `1C 00 00` at once, on every device type.
- Licensing is the operator's responsibility and matters more here (remote operation); ask the club before building.
- **Up to 5 paired devices**, apps only (Stuart, 2026-10-21) — e.g. iPhone app, Jr, iPad, Mac (Touch ID or the Mac's
  password stands in for Face ID), one spare. Also covers TWO OPERATORS on one rig (Stuart: POTA couples, each
  with their own callsign): each paired device carries its operator's CALLSIGN, the TX banner shows who is keyed
  ("TX: M0ABC"), and the server keeps a TX log (callsign, device, start/end, frequency, mode) for their station logs.
  One transmitter at a time still holds — the second operator waits for unkey. Listed by name + last used on the admin page.
- **Jr on the watch transmits too** ("like a Dick Tracy watch"). Jr is standalone, so it pairs itself: the same
  6-digit code typed on the watch, its own key in the watch Keychain, counting as one of the 5. Wrist detection +
  watch passcode stand in for Face ID. Hold-to-talk on a big button (Action button on Ultra). The watch mic streams
  through AVAudioEngine while Jr is frontmost; dropping the wrist or leaving the app unkeys at once (dead-man).
  Mind arm64_32 (32-bit Int below watchOS 27) in the audio and key code. Latency/battery over LTE to be measured.

---

## hwinfo + wire contract for clients (built)

*Built on branch `v12-icom`, 2026-10-22. This is what the SERVER sends and accepts for an Icom, frozen so
the app, the web client, Jr and the Android port can be built from it. Server code: `radioCapsJsonCore()`
(icom branch), `Impl::icomControl()`, `onIcomSweep()`, `onIcomState()`, `sendConfig()` in
`android/app/src/main/cpp/local_sdr_shim.cpp`; the radio itself in `icom_source.*`. Proven against the
fake radio by `scripts/test-server-icom.mjs`.*

### How a client knows it is talking to an Icom

`hwinfo.radio.driver === "icom"`. Branch on that **before any default** (AGENTS.md; "else means dongle").
`config.icom === true` says the same on every config. `/vibeserver.json` → `tunable` is the model's
receive range (`[[30000,74800000]]` on a 7300) and `dab` is `false`.

### `hwinfo.radio` (sent on join, on `{"type":"state"}`, and again whenever RF/scope/TX/link state changes)

```json
"radio":{"driver":"icom","model":"IC-7300","civ":"94","link":"usb","transport":"cp210x","linesHeldLow":true,
 "baud":115200,"noIq":true,"sharedOnly":true,"canTransmit":false,
 "modes":["lsb","usb","am","cw","rtty","fm","cwr","rttyr","usbd","lsbd"],"modeNow":"usbd",
 "filters":[1,2,3],"filNow":1,
 "filterWidths":{"ssb":[3000,2400,1800],"cw":[1200,500,250],"rtty":[2400,500,250],"am":[9000,6000,3000],"fm":[15000,10000,7000]},
 "att":[0,20],"attNow":0,"preamp":[0,1,2],"preampNow":1,"agc":["fast","mid","slow"],"agcNow":"mid",
 "rfGain":[0,255],"rfGainNow":255,"nb":true,"nbNow":false,"nr":true,"nrNow":false,"anf":true,"anfNow":false,
 "scope":{"points":475,"spans":[5000,10000,20000,50000,100000,200000,500000,1000000],"span":50000,
          "modes":["centre","fixed"],"mode":"centre","speeds":["fast","mid","slow"],"fps":19.9},
 "ranges":[[30000,74800000]],"tx":false,"linkUp":true,"ownerLock":true,"rfOpen":false,
 "audio":"ALSA hw:2,0 (CODEC)","audioProblem":"",
 "preflight":{"passed":true,"verified":false,"summary":"Safe to receive: …",
              "items":[{"name":"USB SEND","value":"OFF","ok":true,"gate":true,"where":"MENU > SET > Connectors > …"}, …]},
 "refusedFrames":0}
```

- **Every list comes from here, never a constant** (`feedback_app_no_hardcoded_settings`). `att` is in dB;
  `preamp` 0 = OFF, 1 = P.AMP1, 2 = P.AMP2; `scope.spans` are TOTAL widths in Hz.
- `filterWidths` are the radio's FACTORY widths for FIL1/2/3 per mode family (`ssb` covers usb/lsb/usbd/lsbd,
  `cw` covers cw/cwr, `rtty` covers rtty/rttyr). The owner can change them in the radio and CI-V does not
  report them, so draw the passband from these as approximate.
- `ownerLock` true (the default) = "Listeners follow the radio": only a **signed-in admin** (`admin_unlock`
  accepted on this socket) may tune, change mode/filter/span/scope mode. `rfOpen` false (the default) = ATT,
  preamp, AGC, RF gain, NB, NR, ANF and scope speed are admin-only. A refused control is answered with
  `{"type":"notice","why":"…"}` and a fresh `config`; the client should disable those controls for a
  non-admin rather than let them bounce.
- `tx` true = the OWNER is transmitting: show a "station transmitting" banner. The server has already muted
  the audio and frozen the waterfall; do not treat the silence as a fault.
- `linkUp` false = the radio stopped answering (unplugged, switched off). The server retries every 3 s.
- `preflight` is for the setup page and an admin; listeners never need it. `canTransmit:false` is
  unconditional in this phase.
- The **generic hwinfo fields outside `radio`** (`gains`, `gainNow`, `agc`, `digitalAgc`, `ds*`, `biasT`,
  `adcPeak`, `ifGrFloor` …) are still sent and **mean nothing on an Icom** — ignore them when
  `radio.driver === "icom"`.

### `config` (unchanged shape, Icom meanings)

- `vfo` = the radio's frequency. `centerFreq` = the centre of the radio's CURRENT sweep (centre mode: the
  VFO; fixed mode: the middle of the fixed edges). `binBandwidth × binCount` = the sweep's real span.
  The server re-sends `config` whenever the sweep's edges move, BEFORE the next SPEC row, so rescale on it.
- `mode` is in the client vocabulary (`usb lsb am cwu cwl nfm`); `radioMode` is the radio's own
  (`usbd`, `rtty`, `cwr`, …) — show `radioMode` (e.g. "USB-D", "RTTY") on the mode button.
  Mapping: usb/usbd/rttyr→usb, lsb/lsbd/rtty→lsb, cw→cwu, cwr→cwl, fm→nfm, am→am.
- `shared` is true whenever more than one listener is allowed; an Icom never gives anyone a private VFO.

### SPEC frames

Same 22-byte header and u8 bins as every radio (FFT order, DC first; byte = dB + 256). One frame per radio
sweep, at the radio's rate (`radio.scope.fps`: ~19/s at 115200 baud, ~3/s at 19200), resampled to the
client's `?bins=` by sample-and-hold up / peak-hold down — 475 real points, never interpolated. The
amplitude (0-160 on a 7300) is placed over 80 dB with the floor at −130 dB, i.e. relative to the radio's REF
level, **not** dBFS. The waterfall's colour auto-range works as for any radio; an S-meter derived from SPEC
bins is meaningless — use the `icom` status line.

### `{"type":"icom", …}` status line (new)

Sent on join (straight after hwinfo), on every change, and with the S-meter at most 4 times a second:
`{"type":"icom","freq":7074000,"mode":"usbd","fil":1,"sRaw":48,"sDbOverS9":-30.4,"tx":false,"link":true,"fps":19.9,"span":50000,"scopeMode":"centre"}`.
`sDbOverS9` uses Hamlib's published IC-7300 calibration (0 = S9, −54 = S0, +60 = S9+60); S-units =
9 + sDbOverS9/6 below S9. Draw the S-meter from this.

### What a client SENDS

| Message | On an Icom |
|---|---|
| `{"type":"tune","frequency":…,"mode":…}` | Tunes the RADIO (owner lock applies). A `mode` equal to the one in `config.mode` is ignored (so "usb" never flips USB-D to USB); another one is mapped (table above). `bandwidthLow/High` inside a tune are ignored. |
| `{"type":"mode","mode":…}` | Mode change, same mapping. `wfm`, `sam`-as-SAM and `dab` do not exist (sam maps to AM). |
| `{"type":"bandwidth","bandwidthLow","bandwidthHigh"}` | The NEAREST of FIL1/2/3 by `filterWidths` for the current mode. |
| `{"type":"zoom","binBandwidth":…}` | **The span ladder**: the nearest rung of `scope.spans` to binBandwidth × your bins. A pan (`frequency`) does nothing — the radio's window is centred on its VFO; the reply `config` says where it is. |
| `{"type":"icom_control", …}` (new) | Any of: `mode` (radio names, e.g. `"rtty"`, `"usbd"`), `fil` 1-3, `span` (total Hz, a rung), `scopeMode` `"centre"`/`"fixed"`, `scopeSpeed` `"fast"`/`"mid"`/`"slow"`, `att` (dB from the list), `preamp` 0-2, `agc` `"fast"`/`"mid"`/`"slow"`, `rfGain` 0-255, `nb`/`nr`/`anf` true/false. Several fields in one message are fine. |
| `nr`, `notch`, `say`/chat, decoders, `state`, `ping`, admin sign-in, recording | Unchanged — they work on the radio's audio. |
| `gain`, `agc` (VibeAGC), `tunerbw`, `rsp_control`, `ahf_control`, `airspy_control`, `hackrf_control`, `antenna`, `sampleRate`, `directSampling`, `autoDirectSampling`, `biasT`, `ppm`, `rawIq`, `iqout`, `iqtune`, `rdsx`, every `dab*`, `autobw`, `ims`, `ceq`, `nb`/`nbx` (OUR blanker), `stereo`, `deemph`, `nfmvoice`, `squelch`, `fftRate` | **Swallowed** (logged, no effect). Do not draw the controls that send them. |

Clients never send CI-V and never can: the server builds every frame from a typed table and checks the whole
frame against the allow-list (plan §3.2) before it is written.

### Client controls that do NOT apply on an Icom (hide them — plan §2.3, AGENTS.md)

Sample-rate picker · the gain slider / gain table · VibeAGC and its lock · the IF filter ladder and Auto IF ·
direct sampling (manual and auto) · bias-T · ppm/ppb · the dongle/RSP/HF+/Airspy/HackRF panels ·
squelch (ours) and our noise blanker · RDS, Advanced RDS, MPX, stereo pilot, de-emphasis, NFM voice ·
DAB and the DAB landing · raw IQ out, IQ recording, VibeIQ pairing · multi-VFO / locked-centre / per-listener
VFO UI · spectrum fps tiers and the 4-slot governor display · Spatialise Stereo · binaural · any digital
zoom past the span ladder · HFDL and other IQ decoders (audio decoders — FT8/FT4, RTTY, CW, NAVTEX, WEFAX,
SSTV — DO work, inside the radio's passband, at `config.vfo`).
**Draw instead:** mode buttons from `radio.modes` (label from `radioMode`), FIL1/2/3, a span selector on the
zoom drum (`scope.spans`), centre/fixed, ATT, preamp, AGC fast/mid/slow, RF gain 0-255, NB/NR/ANF (the
RADIO's), an S-meter from the `icom` line, the TX banner, and a "listeners follow the radio" notice when
`ownerLock` is on and the user is not admin. Copy: update the tour/About grep list in AGENTS.md for "Icom",
and say in the tour that zoom = the radio's span.


---

## Built 2026-10-22 (branch `v12-icom`)

### What exists

| Piece | Where | Tested by |
|---|---|---|
| CI-V codec + **whole-frame allow-list** (§3.2) | `android/app/src/main/cpp/civ_codec.h` | `vibeserver/test-icom-civ.cpp` (ctest `icom-civ`): every cmd/sub pair at 0-6 data bytes, ~7.8 M frames, against an independent copy of the table; the named transmit frames refused by name |
| Models table (address, scope caps, spans, ranges, modes, preflight menu items) | `icom_models.h` | via the source tests |
| Scope assembler + sweep → SPEC row | `icom_scope.h` | `test-icom-scope.cpp` (ctest `icom-scope`) + replay of every `test/fixtures/icom/*.civhex` |
| Transports: CP210x and CDC-ACM over libusb (DTR/RTS never raised), opt-in tty, memory | `icom_transport.{h,cpp}` | memory + pty paths in the tests; **libusb paths compile only — no radio here** |
| Receive-audio capture: raw ALSA ioctls (Linux), CoreAudio AUHAL (macOS), tone/WAV test sources | `icom_audio.{h,cpp}` | tone/WAV in the tests; ALSA compile-checked with the NDK's Linux headers; **not run on Linux** |
| `IcomSource`: identify, preflight gate, request queue (tuning first, coalesced), transceive + 1 s poll, TX mute/freeze, watchdog + reconnect (preflight again) | `icom_source.{h,cpp}` | `test-icom-source.cpp` (ctest `icom-source`, 50 checks, zero violations at the fake) |
| Shim integration (`startIcom`, scope → SPEC, audio → `onAudio`, `icomControl`, hwinfo, status line) | `local_sdr_shim.{h,cpp}` (guarded by `VIBE_HAVE_ICOM`, set only by the vibeserver CMake) | `scripts/test-server-icom.mjs` (41 checks, incl. real off-air RTTY decoded through the radio-audio path) |
| Detection (`detectRadios` appends Icoms by USB serial), `--icom*` flags, `icom*` config fields | `radios.cpp`, `main.cpp`, `vibeserver_config.*` | e2e (`--icom`), config round-trip by the existing setup tests |
| Setup page Icom card | `vibe_setup_page.h` | `scripts/test-setup-icom.mjs`, `check-setup-page.mjs` |
| udev (+ ModemManager ignore), systemd `DeviceAllow=char-alsa`, `audio` group | `vibeserver/linux/` | review only |
| macOS: audio-input entitlement, `NSMicrophoneUsageDescription`, `libvibe_icom.a` on the app's link line | `vibeserver/mac/` | the .app's swiftc link verified against this tree; **not signed** |
| Phase 0 capture kit + README for Saber | `tools/icom/civ-capture.py`, `tools/icom/README.md` | run against `tools/icom/fake-icom` (19.6 sweeps/s at 115200, zero violations); its output is a committed fixture |
| Fake IC-7300 on a pty (also a witness: two independent transmit checks) | `vibeserver/icom_fake_radio.h`, `tools/icom/fake-icom.cpp` | used by all of the above |

### Decisions made while building (Stuart was at work; each is easy to reverse)

1. **Audio on Linux = the ALSA kernel interface directly, not vendored tinyalsa.** Capture needs five ioctls
   (HW_PARAMS, SW_PARAMS, PREPARE, READI_FRAMES, DROP) — exactly what tinyalsa does underneath — in ~150
   lines, with nothing new in `vendor/`, no playback or mixer code that could ever be pointed at the radio's
   MOD input, and no `libasound2` (the no-external-deps rule). Needs `<sound/asound.h>` at build time, which
   every build image already has (linux-libc-dev via build-essential / crossbuild-essential / the armhf
   sysroot's libc6-dev).
2. **An Icom is recognised by its USB serial string** ("IC-7300 03001234" — the radio programs its model into
   the CP2102), read from sysfs on Linux with nothing opened and nothing sent. A CP2102 that does not name an
   Icom is never touched (GPS units, programming leads). An owner can still point `icomPort` at any port. The
   radio must then answer `19 00` with a model we know, or it is refused.
3. **The discovery chain never auto-picks an Icom.** With no radio configured, VibeServer still picks the
   SDRs it always did; a transceiver is served only when the owner chose it (setup's radio list, `--radio`,
   or `--icom`).
4. **libusb for BOTH chip kinds; the tty only behind `icomTtyOk`.** CDC-ACM radios (705/9700/MK2) are also
   driven over libusb with SET_CONTROL_LINE_STATE(0) first. The tty fallback exists for platforms where the
   OS driver owns the chip (likely macOS) and for the fake radio, and the setup page explains its risk.
5. **The preflight fails CLOSED**: a keying item that cannot be read counts as unsafe. The IC-7300 menu item
   numbers (USB SEND 00 89, USB Keying CW 00 90, RTTY 00 91, DATA MOD 00 67) are **[unverified]** —
   `menuVerified=false`, and the setup page says so. The libusb path never raises DTR/RTS, so the preflight is
   the second wall, not the first. VOX is read by its own command (16 46), not a menu number.
6. **Owner lock ("Listeners follow the radio") defaults ON and means a SIGNED-IN admin** (`admin_unlock` on
   that socket) — deliberately NOT adminGate's loopback shortcut, because a tunnel or reverse proxy on the box
   arrives as 127.0.0.1. With no admin password set anyone can sign in (the server's existing model), so the
   setup page says the lock only protects the radio once a password is set. **RF settings** (ATT, preamp,
   AGC, RF gain, NB/NR/ANF, scope speed) are admin-only unless `icomRfOpen` (plan §3.4 [decide] → this default).
7. **Server-side tunes are ignored on an Icom** (landing, resume, recovery): the radio is the dial and stays
   where its owner left it. `retune()`/`tuneHw()` are no-ops for it, and the landing card is hidden.
8. **Width → FIL only on a `bandwidth` message**, never from the passband a client tucks inside every `tune`
   (that would flip the radio's filter on each step). **Zoom → the nearest span rung** (log distance); a pan
   is answered with the config that says where the radio really is.
9. **Scope amplitude → wire dB**: 0..ampMax over 80 dB with the floor at −130 dB (relative to REF, not dBFS);
   rows are resampled sample-and-hold up / peak-hold down — never interpolated.
10. **TX**: audio muted while `1C 00` reads 1 and for 300 ms after; scope frozen; `radio.tx` + status line.
11. **Only the IC-7300 is "known"**; the other rows (MK2, 705, 9700, 7610, R8600) identify and are REFUSED
    with a plain message until someone with one tests them.
12. **On close** only the scope DATA OUTPUT we switched on is switched off (`27 11 00`); the radio's own scope
    display is the owner's and is left on.
13. **The capture kit's `--menu-scan` is opt-in and goes beyond the plan's seven frames**: it READS menu items
    0001-0110 (`1A 05 hh ll`, no value) so the preflight numbers can be pinned on Saber's radio. Reads only,
    still allow-list checked — but it is a change to the Phase 0 brief, so it is listed under Open for Stuart.
14. **Threads**: one CI-V thread (spectrum priority — it is the waterfall) and one audio-capture thread (audio
    priority); no DSP thread, no hotplug watch (IcomSource has its own watchdog). Per-sweep work allocates
    only what `onSpectrum` already does (`allSpecPeers`, `dueForFrame`); the SPEC buffer and PCM buffer grow
    once.

### Open for Stuart

- **Saber's Phase 0 run** (`tools/icom/README.md`): real 7300 frames into `test/fixtures/icom/`, the measured
  sweeps/s, and — with `--menu-scan` (approve it, decision 13) — the real menu numbers for USB SEND / USB
  Keying / DATA MOD. Until then the preflight's numbers are guesses that fail closed.
- **Defaults to confirm**: owner lock ON, RF settings admin-only (decision 6).
- **macOS CP210x**: if macOS's own driver owns the 7300's CP2102 (likely), libusb cannot claim it without
  root, and the only path is the tty with `icomTtyOk` (a DTR/RTS glitch on open). Options: accept that with
  the preflight as the guard; run the engine with the privilege to detach; or apply for
  `com.apple.vm.device-access`. Needs a Mac + 7300 to know. **[unverified]**
- **Signing/notarising the Mac app**: nothing new to do beyond the usual `vibeserver/mac/notarise-and-release.sh`
  run with NO argument (never on /Applications — memory rule). The entitlements file now carries
  `com.apple.security.device.audio-input`, which that script already applies to every executable in the
  bundle. Check on the notarised build that the microphone prompt names VibeServer when the engine first opens
  the radio's audio (TCC attribution to a child process is **[unverified]**), and that `codesign -d
  --entitlements -` on `Contents/MacOS/vibeserver-engine` shows it.
- **Linux**: built and unit-tested on macOS only; the ALSA and sysfs paths were compile-checked with the NDK's
  Linux headers, not run. The first apt build will be their first real compile on Linux.
- **Android (Phase 4), LAN (Phase 3), clients** are not started. The shim compiles without `VIBE_HAVE_ICOM`,
  so Android/iOS are unaffected. Client work starts from the contract section above.
- **Copy**: when the client ships, add "Icom" to AGENTS.md's grep list (tours, About, website).

### Not verified without a real radio

DTR/RTS staying low on a real CP2102 (dummy load!); the 7300's exact `27 00` division bytes and whether its
scope commands take the `00` selector byte; the preflight menu numbers; echo/transceive behaviour on Saber's
settings; the codec's ALSA/CoreAudio names; the sibling-card search on a real USB topology; real sweep rate at
115200; S-meter calibration on the radio; behaviour while wfweb holds the port; CDC-ACM radios entirely.

---

## Clients built 2026-10-22 (branch `v12-icom-ui`) — the app and the browser

*Built from the contract above while Stuart was at work. Nothing here has met a real radio; the web half was driven
against the fake IC-7300 (`scripts/test-web-icom.mjs`), the app half is type-checked and source-checked only (no
native build was allowed on this branch — another agent held the Android build).*

### One rule, two readers: `src/services/icomRadio.ts`

hwinfo.radio and the `{"type":"icom"}` line are parsed ONCE, in a file with no imports that the app and
`web/client` both use: the hide list (`ICOM_HIDDEN_CONTROLS`), owner lock / RF-admin (`icomMayTune`, `icomMayRf`),
labels (USB-D, CW-R, ±25k, P.AMP1), the span ladder (`nearestSpanIndex`, `spanStep`), the S-meter (`sMeter`, 6 dB
per S-unit from `sDbOverS9`), the banner words, and `icomControlMsg`, which only ever emits values the radio listed
and has no transmit field to build. Tested by `scripts/test_icom_radio.ts` (129 checks) and, for the app's gates,
`scripts/check-icom-app-gates.mjs` (38). Both are in `run-tests.sh`.

### What the APP does on an Icom

- **The radio's controls** are in the cog under the radio's name (the existing hardware sheet, `LocalHardwarePanel`):
  an Icom branch FIRST, then nothing of the IQ body (gain table, VibeAGC, IF ladder, rate, ppm, bias-T, DS, the
  RSP/HF+/Airspy/HackRF stages). Drawn: MODE from `radio.modes` (USB-D, RTTY …), FIL1/2/3 with the factory widths,
  SCOPE SPAN from `scope.spans`, CENTRE/FIXED, preamp, ATT, AGC, RF gain (sent on release), the radio's NB/NR/ANF,
  scope speed.
- **Hidden elsewhere**: our squelch (audio sheet + menu), de-emphasis, stereo, every FM DSP key (NR/IMS/auto BW/
  CEQ/NB/NBX), NFM voice, binaural, raw IQ out, link mode, VFO lock + centre-on-VFO, WFM in the mode list (the list
  is the client modes the radio's modes map to), and the spectrum smoothing / spatial smoothing / waterfall-speed
  controls (see below). DAB and Advanced RDS were already unreachable (`dab:false`, no WFM).
- **S-meter** from the status line (dBm = −73 + sDbOverS9, read like OWRX's S-meter), never from SPEC bins.
- **Banners** in the owner-notice furniture: "The radio is transmitting — receive paused", "The radio is not
  answering — VibeServer is retrying"; and, for a listener under the owner lock, "Everyone hears what the radio is
  tuned to — the owner tunes it." (dismissible).
- **The wire**: `icomControl()` sends `icom_control`; zoom lands on a span rung and is NOT capped by
  `config.maxBandwidth` (on an Icom that is the CURRENT span, so the ordinary cap would forbid zooming out); a resume
  never re-asserts our tune; under the owner lock a listener's tune/mode/width is refused locally with one notice
  per 4 s (the server would send one per drum detent); the generic hwinfo/config IF/gain/AGC fields are ignored.
- **No false alarms**: no frame-stale / no-first-frame reconnect and no red link bars while the radio says it is
  transmitting or off CI-V (the server freezes the scope on purpose).
- **Owner setup** (`IcomSetupCard`, inside the radio sheet, signed-in admin only): the facts from hwinfo (model,
  CI-V, baud with the 115200 ≈ 19/s vs 19200 ≈ 3/s note, transport and DTR/RTS, preflight with the exact menu item,
  audio device/problem, refused-frame count, "only one program can hold the radio's USB port"), and the settings
  (baud, CI-V, scope speed, owner lock, RF open, audio, port, serial-device opt-in) saved through the setup page's
  own endpoint: GET `/vibeserver/config` at the door with the admin ticket, change only this radio's `icom*`
  fields, POST the whole thing back (it restarts the server, as the setup page says). A phone server answers 501
  and the card shows the facts only. "OPEN THE SETUP PAGE" is there too.

### Decisions (each easy to reverse)

1. **A control a listener may not use is hidden and its STATE shown read-only** (e.g. "P.AMP1 · ATT OFF · AGC
   MID · RF 100%"), rather than greyed — the house rule (hidden, not greyed) and the contract (don't let refused
   controls bounce) both hold, and a listener still sees what the radio is doing.
2. **Scope speed shows no selected key**: the radio does not report its sweep speed, so lighting FAST would be a
   guess.
3. **No smoothing on an Icom**: spectrum smoothing 1, averaging 0, spatial smoothing off, waterfall SHARP (one row
   per sweep). A 3/s sweep under a 20 fps EMA lags by seconds, and interpolated rows invent data between real
   sweeps. The listener's own settings are kept for other radios; the controls are not offered while on an Icom.
4. **The app's own server screen (ServerModeScreen) is NOT changed here**: an Icom on the Android app server is
   Phase 4, being built on `v12-icom-android`, and the native config keys do not exist yet — a setup section there
   now would be a set of dead controls. That branch can reuse `icomSettingsFrom`, `ICOM_BAUDS`, `parseCivHex`,
   `preflightLines` and `IcomSetupCard`'s layout.
5. **The website is not changed**: its promise is "nothing ships that I haven't driven myself on real hardware",
   and no client has met a real Icom yet. The About V12 note and feature list say "experimental" and "receive only".
6. **The mode button on the main screen shows the client mode (USB for USB-D)**; the radio's own name is in the
   radio sheet. Showing `radioMode` on the main mode key is a follow-up if Saber wants it.

### Not done / not verified

- **The watch (Jr) and the iOS/Android watch faces** have no Icom branch: on an Icom server they would still draw
  their IQ controls. Swift could not be built on this branch.
- **The app UI has not been run**: no simulator/device build was allowed. Theme (default/silver/black,
  transparency on/off) and phone/wide layout of the new sheet section and banners are unverified on screen; they
  reuse the existing sheet's styles and the owner-notice banner, which already pass those checks.
- **The IcomSetupCard save** has not been exercised against a real VibeServer (the fake e2e has no admin config
  round-trip from the app). The round-trip logic (`withIcomSettings`) is unit-tested, including that the admin
  password survives.
