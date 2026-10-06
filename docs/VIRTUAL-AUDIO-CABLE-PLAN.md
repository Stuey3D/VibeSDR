# Virtual Audio Cable — research and plan

> **DECISIONS — Stuart, 2026-10-06 (these override the recommendations below where they differ):**
> - **Opus, not PCM.** The audio is the Opus stream every listener already gets — it carries data modes (FT8/JS8 on
>   SSB, APRS/packet on NFM) well enough for another app to decode, and it is what keeps low-end connections working.
>   Uncompressed stays exactly what it is today: an owner setting. No new PCM stream, no server change for Tier 1.
> - **Windows = Tier 1 only.** Once VB-CABLE is installed the browser does the whole job (the "Send audio to…"
>   picker, or Windows' own per-app output device), so VibeIQ on Windows would only add a second install. Not built.
> - **VibeIQ audio only where it removes an install:** Linux (creates the device itself, trivial) and possibly macOS
>   (our own BlackHole-derived device). Elsewhere, and until then, Tier 1 + a free cable is the answer.
> - Still open: one pairing code or two; whether VibeIQ alone keeps the session alive; the owner switch default.

*2026-10-06. Research only: no product code. Stuart's idea, the facts per platform as of October 2026, and a
ranked plan. Sources are linked inline and listed at the end. Claims marked **[unverified]** need a test before
anyone relies on them.*

## 0. The idea, and the verdict in one paragraph

Stuart (2026-10-06): *"user opens vibesdr in the webclient from one of our servers and next to RAW IQ out we have a
button to enable Virtual Audio cable. When pressed the client device could act like we've just plugged a usb
soundcard in … a modification to the VibeIQ app could mean that click virtual audio out enter 6 digit code and then
VibeIQ … could then act like the virtual soundcard."*

The goal: the receiver's **demodulated audio** shows up on the listener's computer as an **audio input device**, so
WSJT-X, JS8Call, fldigi, MultiPSK, a DAW or Discord can use it like a radio plugged into a sound card.

**Verdict:**

- **A web page cannot create an audio device.** No browser API can register an OS sound device or act as a USB
  device (§1.1).
- **A web page can choose which existing output device it plays to.** If the listener has already installed a free
  virtual cable (VB-CABLE on Windows, BlackHole on macOS, a PipeWire null sink on Linux), the web client can send
  the receiver audio into it, and the other end of that cable appears as a microphone to WSJT-X. This works today
  in Chrome/Edge on every desktop OS, in Firefox, and in Safari 18.4+. It needs a few days of web-client work,
  installs nothing of ours, and gives most of the value. **Do this first** (Tier 1).
- **VibeIQ can become the sound card**, but the cost is very different on each platform:
  - **Linux:** trivial. A PipeWire/Pulse virtual source is created from user space with no driver.
  - **macOS:** medium. A Core Audio HAL plug-in, i.e. a renamed fork of BlackHole. The GPL-3.0 licence is
    compatible. It needs an admin install, a notarised `.pkg` and a `coreaudiod` restart.
  - **Windows:** creating a device needs a **kernel driver**, and that needs an EV certificate plus Microsoft
    attestation signing. That is the route Stuart already declined for the Windows port. So on Windows, VibeIQ
    **plays into an installed VB-CABLE**, which may be bundled under its donationware terms. It does not ship a
    driver of its own.
- **iOS / Android:** no system-wide virtual input is possible for a third-party app (§2.4).

★ Small correction to the brief: the pairing code is **six characters**, not six digits (`iqPairing.ts`, the
VibeIQ 1.0.1 release notes). A virtual-audio pairing would reuse the same format.

---

## 1. In-browser only

### 1.1 Can a page create an input device or emulate a USB sound card? No.
- **WebUSB, WebHID and WebSerial are host-side APIs.** A page talks *to* a device that is plugged in. None of them
  can make the computer *appear to be* a device, and none of them can register a device with the OS audio stack
  ([WebUSB spec](https://wicg.github.io/webusb/): "an API for securely providing access to USB devices from web
  pages"). USB "gadget" mode, where a machine pretends to be a peripheral, exists only on hardware with a
  device-side controller (e.g. a Pi Zero's OTG port) and is driven by the kernel. No browser can reach it.
- `getUserMedia` only **consumes** inputs. There is no web API that adds a device to Core Audio, WASAPI or PipeWire.
- So the "browser fakes a USB soundcard" idea is not possible. The browser can only *play into* a device that
  already exists.

### 1.2 What a page *can* do: choose its output device
Support for each method, from MDN browser-compat-data 8.1.4 (2026-10-01):

| API | Chrome/Edge | Firefox | Safari (macOS + iOS) | Notes |
|---|---|---|---|---|
| `AudioContext.setSinkId()` | **110+** | no | no | experimental; secure context; Chrome Android 110 |
| `HTMLMediaElement.setSinkId()` | 49+ / 17+ | **116+** | **18.4+** | standard track |
| `MediaDevices.selectAudioOutput()` | no | **116+** | no | the browser's own speaker picker; needs a click |

How the permissions work ([MDN setSinkId](https://developer.mozilla.org/en-US/docs/Web/API/AudioContext/setSinkId),
[MDN selectAudioOutput](https://developer.mozilla.org/en-US/docs/Web/API/MediaDevices/selectAudioOutput),
[MDN HTMLMediaElement.setSinkId](https://developer.mozilla.org/en-US/docs/Web/API/HTMLMediaElement/setSinkId)):
- **All of these are secure-context only.** They work on the tunnel/directory pages (https). They do **not** work on
  a plain-http LAN page such as `http://vibeserver.local:48000`. That page already lacks an AudioWorklet too, see
  `web/client/src/audio.ts`.
- `speaker-selection` permissions policy. The server sends no such header today, so the policy default applies.
  Check that the default is not "none" in any embed we do.
- **Chrome/Edge:** `enumerateDevices()` only lists non-default outputs with labels once the page holds **microphone
  permission**. MDN's own example calls `getUserMedia({audio:true})` first. So in Chrome the picker costs one "Allow
  microphone?" prompt. That prompt is confusing on a receiver page, so the copy must say why it appears ("Chrome
  only shows your audio devices after this — VibeSDR does not record you"). Stop the stream immediately afterwards.
- **Firefox:** `selectAudioOutput()` opens the browser's own device chooser with no mic prompt. Firefox has no
  `AudioContext.setSinkId`, so the route is AudioContext → `MediaStreamAudioDestinationNode` → an `<audio>` element
  with `srcObject`, then `setSinkId()` on the element.
- **Safari 18.4+:** the element method only. Our Safari path already plays Opus through a **MediaSource on an
  `<audio>` element** (`audio.ts` "MEDIA PLAYOUT"), so `setSinkId` goes on that element. Safari, like Chrome,
  lists outputs only after a getUserMedia grant. **[unverified on 18.x/26.x — test]**.

### 1.3 The realistic browser-only tier (Tier 1)
An **"Send audio to…"** device picker in the AUDIO panel, next to RAW IQ OUT. Choosing a device routes the
receiver audio to it. The user's virtual cable then turns that into a microphone.

What it needs to get right:
- **Level, not volume.** A decoder wants a steady level. In cable mode the output should be taken **before** the
  user's volume slider (or the slider pinned at 100 % with a separate "cable level" control). If it isn't, turning
  the speakers down starves WSJT-X.
- **Mute must not stop the stream.** On the app side, muting closes the audio socket ([[app_tune_two_sockets]]).
  Whatever the web client does on mute, cable mode must keep audio flowing. The obvious mute button should mean
  "monitor off", not "audio off".
- **Optional local monitor:** the cable takes the audio away from the speakers. To hear it as well, use a second
  sink (a `MediaStreamDestination` feeding an `<audio>` on the default device). This is cheap and worth offering as
  a tick box.
- **The page must stay open**, and the tab must not be discarded. Audio-playing tabs are exempt from Chrome's
  intensive background throttling. Memory-saver tab discard can still kill a long-idle tab **[unverified for
  audio-playing tabs — test]**.
- **Quality:** the server sends 48 kHz Opus at 64 kb/s (stereo on WFM, mono otherwise; `opusBitrateFor`). WSJT-X
  samples at 48 kHz and immediately decimates to 12 kHz ([WSJT-X user guide](https://wsjtx.github.io/wsjtx/wsjtx-main-3.2_en.html)),
  so 48 kHz is the rate to give it. Opus at 64 kb/s is far beyond what FT8/JS8/RTTY/SSTV need. The one real
  artefact is packet-loss concealment during tunnel stalls, which a decoder sees as a short dropout.
- **Latency:** tunnel + jitter buffer + `latencyHint: 'playback'` comes to roughly 0.3–1 s **[measure]**. For FT8 this
  shows as a positive **DT**. WSJT-X decodes within about ±2.5 s, so it works, but the PC clock must still be
  NTP-synced.
- **The listener installs a cable once.** Guidance in the help card:

| OS | Free cable | The "microphone" WSJT-X picks |
|---|---|---|
| Windows | [VB-CABLE](https://vb-audio.com/Cable/) (donationware) | *CABLE Output* |
| macOS | [BlackHole 2ch](https://github.com/ExistentialAudio/BlackHole) (GPL-3.0; also on Homebrew) | *BlackHole 2ch* |
| Linux | none needed: `pactl load-module module-null-sink sink_name=vibesdr` | *Monitor of vibesdr* (or a remap-source, §2.3) |

- **No secure context on the LAN:** for http LAN pages, point people to the OS's own per-app routing instead.
  Windows 11 Settings → System → Sound → Volume mixer can set *the browser's* output to *CABLE Input*.
  Linux has `pavucontrol` → Playback → move the stream. macOS has no native per-app routing, so a Mac on a LAN page
  must open the tunnel address instead.

**UI flow (Tier 1):** AUDIO panel → **SEND AUDIO TO…** (beside RAW IQ OUT) → device list (or Firefox's chooser) →
pick *CABLE Input* / *BlackHole 2ch* → a status line ("Receiver audio → CABLE Input · monitor off") → in WSJT-X choose
*CABLE Output* as the input. No code, no VibeIQ. It is unaffected by RAW IQ's "never on a shared dial" rule: this is
the same audio the listener already hears, so the picker can appear on every radio.

**Cost:** about 2–4 days, including testing on 3 browsers × 3 OSes. It is a web-client change, which means **it is a
server release** ([[web_client_lives_inside_the_server]]). AGENTS.md's grep list applies: the tour (`sdrTour`), the
About release notes and `website/index.html` must describe where the control lives.

---

## 2. VibeIQ tier — VibeIQ *is* the sound card

The point of this tier is (a) no third-party cable to install, (b) **no browser left open**, and (c) a device named
"VibeSDR Receiver". Today VibeIQ is Go, **standard library only, CGO off**, one static binary per OS, with a Swift
`.app` shell on the Mac (`tools/vibeiq/main.go`, `build-all.sh`, `mac/VibeIQApp.swift`). That constraint decides a
lot below.

### 2.1 macOS — Core Audio HAL plug-in (AudioServerPlugIn)
- **Mechanism:** a user-space `.driver` bundle in `/Library/Audio/Plug-Ins/HAL/`, loaded by `coreaudiod`. This is how
  BlackHole, Loopback and Soundflower work. After install, `coreaudiod` must restart (`sudo killall -9 coreaudiod`,
  per BlackHole's README). That kills all audio for a second, so do it once, in the installer.
- **Not DriverKit:** Apple's DTS states that AudioDriverKit entitlements are **not granted for virtual devices**.
  "If a virtual audio driver … is all that is needed, the audio server plug-in driver model should continue to be
  used" ([Apple forums 682035](https://developer.apple.com/forums/thread/682035),
  [WWDC21 10190](https://developer.apple.com/videos/play/wwdc2021/10190/)).
- **Start from BlackHole:** it builds for Intel and Apple Silicon and supports macOS 10.10+. Name, bundle ID and
  channel count are compile-time constants (`kDriver_Name`, `kPlugIn_BundleID`, `kNumber_Of_Channels`). It ships a
  `create_installer.sh` that signs and notarises a `.pkg`
  ([BlackHole README](https://github.com/ExistentialAudio/BlackHole)).
- **Licence:** BlackHole is **GPL-3.0**, and "a license is required for all non-GPLv3 projects". VibeSDR is GPL-3.0
  (`LICENSE`), so a fork is compatible: ship the source (it is in this repo), keep the notices, and **change the
  bundle ID and name** so it never collides with a user's own BlackHole.
- **Design choices:**
  1. *Loopback fork (simplest):* the device has an output side and an input side. VibeIQ plays into the output side,
     and apps record from the input side. This is exactly BlackHole with our name on it. One downside: users see a
     "VibeSDR Receiver" **output** too, and a system sound accidentally routed there is mixed into the decoder's
     audio. Hiding the output half depends on the fork's flags **[check BlackHole's current constants]**.
  2. *Feed-only device (better, harder):* an input-only device that VibeIQ fills directly through shared memory, a
     ring buffer the plug-in reads in its IO callback. No output side and no cross-talk. This means writing HAL
     plug-in IO code rather than renaming a build, which adds roughly a week.
- **Signing:** a Developer ID **Installer** certificate for the `.pkg`, Developer ID Application for the bundle,
  notarise and staple. Stuart already notarises VibeIQ.app and the Mac VibeServer
  (`vibeserver/mac/notarise-and-release.sh`). This is the same account plus the Installer cert. Installing needs an
  admin password because it writes to `/Library`. Uninstalling = delete the bundle + restart `coreaudiod`; put that
  in VibeIQ's menu.
- **Who writes the audio:** the Go bridge cannot call Core Audio without CGO. The Swift shell already owns the
  bridge process, so the Swift side gets the PCM over a pipe or loopback socket and plays it with AVAudioEngine to the
  VibeSDR device (option 1), or writes the shared ring (option 2). A bare-binary/CLI user on a Mac gets the Tier-1
  style "play into an existing device" route instead.
- **Effort:** option 1, about 1–1.5 weeks including installer, notarisation and an uninstaller. Option 2, about 2–3
  weeks. Risk: `coreaudiod` restarts behave differently across macOS releases (some need a reboot)
  **[test on 15 and 26]**.

### 2.2 Windows — a device needs a kernel driver
- **No user-mode API in 2026 creates an audio endpoint.** APOs (audio processing objects) are user-mode but ride on a
  driver ([MS Learn: Windows 11 APO APIs](https://github.com/MicrosoftDocs/windows-driver-docs/blob/staging/windows-driver-docs-pr/audio/windows-11-apis-for-audio-processing-objects.md)).
  Every virtual cable (VB-CABLE, VAC, Thesycon's SDK, the MIT
  [VirtualDrivers/Virtual-Audio-Driver](https://github.com/VirtualDrivers/Virtual-Audio-Driver) based on the
  SysVAD sample) is a WDM/ACX **kernel** driver. The old user-mode tricks (a DirectShow source filter, a waveIn
  driver) are invisible to WASAPI apps, so they would not reach modern WSJT-X/Discord.
- **Signing a kernel driver:** Windows 10/11 loads only Microsoft-signed kernel drivers. Attestation signing goes
  through the Hardware Dev Center, and **the account must have an EV certificate**
  ([MS Learn: driver code signing requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-reqs)).
  Stuart **declined the EV certificate** (£250–400/yr) for the Windows port (docs/WINDOWS-PORT-PLAN.md).
  **SignPath signs user-mode artefacts only. It cannot sign a kernel driver.** Microsoft is also tightening kernel
  trust in 2026: the April 2026 update stopped trusting old cross-signed drivers by default
  ([TechPowerUp](https://www.techpowerup.com/347807/windows-11-will-no-longer-trust-old-drivers-by-default-under-new-kernel-policy)).
  The VirtualDrivers project ships **test-signed** only, and asking users for `bcdedit /set testsigning on` is not
  acceptable.
  → **Our own Windows driver is off the table** unless the EV decision changes. Even then, a kernel driver is a
  BSOD-class liability for a free project.
- **What works instead: VibeIQ plays into VB-CABLE.**
  - VB-Audio's terms **allow bundling VB-CABLE in free or commercial installers** under the donationware model,
    provided the user can see it is a VB-Audio product, is shown `www.vb-cable.com`, and can donate
    ([VB-Audio licensing](https://vb-audio.com/Services/licensing.htm); precedent:
    [pipemix PR #3](https://github.com/Garvit-Agrawal7/pipemix/pull/3) bundles the unmodified
    `VBCABLE_Driver_Pack` and names VB-Audio on the finish page). "Significant companies" are asked for a licence
    fee. That does not apply to a free GPL hobby app, but **email VB-Audio first** as a courtesy
    **[decide/ask]**.
  - The simplest plan is not to bundle at all: VibeIQ detects "CABLE Input". If it is missing, VibeIQ says "install
    VB-CABLE (free) from vb-cable.com" with a button. If it is present, VibeIQ plays into it.
  - **Playing audio from stdlib-only Go:** `winmm.dll` `waveOutOpen`/`waveOutWrite` through
    `syscall.NewLazyDLL` is about 200 lines with no CGO, and it can select a device by name (`waveOutGetDevCaps`).
    Latency of 100–200 ms does not matter to a decoder. WASAPI through hand-rolled COM vtables is possible but heavier.
- **Effort:** about 3–5 days (device discovery + waveOut + the "install VB-CABLE" flow). Signing comes from SignPath
  like the rest of the Windows work.

### 2.3 Linux — user space, no driver
- **PipeWire** (default on current Fedora/Ubuntu/Debian desktops) and **PulseAudio** both create virtual devices from
  an ordinary user process ([PipeWire module-loopback](https://docs.pipewire.org/page_module_loopback.html),
  [PulseAudio modules](https://www.freedesktop.org/wiki/Software/PulseAudio/Documentation/User/Modules/)):
  - `pactl load-module module-null-sink sink_name=vibesdr media.class=Audio/Source/Virtual channel_map=mono` on
    PipeWire gives a **source** (a microphone) named VibeSDR that VibeIQ writes into with
    `pw-cat --playback --target vibesdr` (or `pacat -d vibesdr`).
  - Portable Pulse form: `module-null-sink sink_name=vibesdr` + `module-remap-source master=vibesdr.monitor
    source_name=vibesdr_rx`, so apps see a real input rather than "Monitor of…".
  - `pactl unload-module <id>` on exit, so nothing is left behind.
- Go stdlib-only fits: `os/exec` for `pactl`/`pw-cat`, with PCM on stdin. No CGO and no new dependency.
- **Fallback:** ALSA `snd-aloop` needs root (`modprobe`), so it is a documented fallback for headless/JACK setups,
  not the default.
- **Effort:** about 2–3 days. ChromeOS: VibeIQ-linux inside the Crostini container should give the same virtual
  source to Linux apps (WSJT-X for Linux) in that container, but not to Android or browser apps on the host
  **[untested]**.

### 2.4 Mobile
- **iOS/iPadOS:** a third-party app cannot add a system input device. Inter-App Audio was deprecated (iOS 13) and is
  gone. The nearest thing is an **AUv3 generator extension**: a host app such as AUM could load a "VibeSDR Receiver"
  unit, but iOS digital-mode apps read the microphone, not AUv3. So it is not worth building for this goal.
- **Android:** no API lets a normal app present a microphone to other apps. `REMOTE_SUBMIX` capture needs
  `CAPTURE_AUDIO_OUTPUT`, a system-only permission
  ([Android MediaRecorder.AudioSource](https://developer.android.com/reference/android/media/MediaRecorder.AudioSource)).
  The Android 10+ playback-capture workaround needs the *receiving* app to opt in
  ([jitsi-meet #16701](https://github.com/jitsi/jitsi-meet/issues/16701)). So the answer is no without root.
- For both, the right answer is an **in-app decoder** (the decoder work in docs/DECODER-AUDIT-2026-10-04.md), not a
  cable.

---

## 3. Transport (VibeIQ tier)

**Pairing:** reuse the RAW IQ pattern exactly. The server issues a token to the session, the client registers
`code → slug + token (+ /r/<serial>/ path)` with the directory (`registerIqCode`, `directory/src/index.js` `iq_codes`),
and VibeIQ looks up the code and connects to `<slug>.vibeserver.vibesdr.net`. Two ways to extend it:
- (a) add a `kind` (`iq` | `audio`) to the same table and API; or
- (b) let one code carry both, so VibeIQ offers both "rtl_tcp" and "sound card" for the same session.
(b) is nicer for a user doing DSD + WSJT-X at once. (a) is simpler to reason about. **[decide]**.

**Stream:** the server already has `/ws/audio` (48 kHz; Opus with `?codec=opus`, `channels=1` folds to mono). The
catch is the **decoder**:
- VibeIQ has **no Opus decoder** (stdlib-only Go). A pure-Go decoder exists ([pion/opus](https://github.com/pion/opus)),
  but how complete its CELT path is (which a 48 kHz fullband encoder uses) is **[unverified]**. Do not build on it
  without a test against our encoder's packets.
- Raw PCM is behind an **owner switch that defaults OFF** (`VsUncompressedAudio`, ~187 KB/s for 48 k stereo s16). That
  was deliberate, to protect the owner's uplink.
- **Recommended:** a VibeIQ-only PCM format negotiated on the token-authenticated socket. Use **mono s16 at 24 kHz**
  (48 KB/s ≈ 384 kb/s). It carries 12 kHz of audio, which covers every digital mode and SSB/AM/NFM. VibeIQ upsamples to
  48 k for the device with the polyphase resampler it already has (`resample.go`). On WFM, offer 48 k mono. That is
  about 6× the Opus cost per listener, so it goes under an **owner switch** in the RAW IQ card style. Alternatives:
  vendor libopus with CGO, which breaks the one-static-binary promise; or let the Mac Swift shell decode Opus with
  AudioToolbox and Linux/Windows take PCM, which means two code paths.
  **[decide: PCM-24k everywhere vs Opus where the shell can decode]**.

**Clock drift and jitter:** the server's 48 kHz and the PC's device clock differ by tens of ppm. VibeIQ keeps a jitter
buffer (target ~200–300 ms through the tunnel) and steers it with a **slow fractional resample** (±0.05 %), or by
dropping or repeating one sample in quiet passages. Never let it run dry or overflow. **Network drop:** feed silence,
never stop the device (a vanishing input makes WSJT-X/fldigi error out), and reconnect with the backoff the IQ bridge
already uses. **CPU:** decode/resample of one mono 24 kHz stream is well under 1 % of a laptop core.

**It is a listener, and that is right:**
- An audio-only socket is already counted as a listener (`specListenerCountLocked` → `audioOnly`), and decoders are
  now treated "same as a listener" ([[decoder_socket_not_presence]]). VibeIQ's audio socket should carry the
  **session id** of the page that paired it, so browser + VibeIQ count as **one** listener, not two.
- **Session limits:** hard limit ends it on time; soft limit keeps it until the radio is full and someone is waiting.
  Same rules as decoders. An all-day FT8 skimmer on a quiet soft-limit radio is fine. On a busy one it yields.
- **Idle prompt:** exempt while VibeIQ is actively reading, as RAW IQ already is.
- **Lifetime:** RAW IQ dies with the page's session. For audio, the whole point is that the page can close. Milestone
  1 keeps RAW IQ's rule (simple, safe); then decide whether VibeIQ's socket alone keeps the session alive
  **[decide — Stuart]**.
- **Shared dial:** audio is fine on a shared dial (it is what the listener hears). **Tuning from VibeIQ is not**: see
  §4 Tier 3 and the [[shared_dial_contract]].
- **Owner control:** Tier 1 needs none (it is the listener's own playback). Tier 2: an owner setting "Audio to other
  apps (VibeIQ): Off / Local only / Local & public", like RAW IQ OUT, defaulting **on** for Opus and asking about PCM
  because of the uplink cost. **[decide default]**.

---

## 4. Recommendation

| Tier | What | Platforms | Effort | Install for the user | Value |
|---|---|---|---|---|---|
| **1** | Web client **SEND AUDIO TO…** picker + help card for free cables | Win/Mac/Linux, Chrome/Edge/Firefox/Safari 18.4+ (https pages) | **2–4 days** + a server release | a free cable, once (none on Linux) | **Most of it**, now |
| **2a** | VibeIQ "Sound card" on **Linux** (PipeWire/Pulse virtual source) | Linux (+ Crostini) | **2–3 days** + server token/PCM work (shared, below) | VibeIQ only | high for Pi/Linux hams |
| **2b** | VibeIQ on **Windows**, playing into VB-CABLE (detect, guide, optionally bundle with VB-Audio's blessing) | Windows 10/11 | **3–5 days** | VibeIQ + VB-CABLE | high, and no browser |
| **2c** | VibeIQ on **macOS** with our own HAL device (BlackHole fork, notarised `.pkg`) | macOS 10.15+ Intel/AS | **1–1.5 weeks** (loopback) / **2–3 weeks** (feed-only) | VibeIQ `.pkg`, admin password | medium–high |
| — | Server side for all of 2: token-auth audio socket on the session, `kind=audio` pairing, PCM-24k mono option, owner switch | VibeServer + directory | **3–5 days** + an apt/APK cut | — | enabler |
| **3** | **CAT for WSJT-X**: VibeIQ speaks Hamlib `rigctld` on :4532 (`F`/`f`/`M`/`m`), so WSJT-X's band buttons tune the receiver, as rtl_tcp `SET_FREQUENCY` already does for IQ | all VibeIQ | 3–5 days | none extra | turns a receiver into a "radio" for WSJT-X. **One-listener / locked-range radios only**, never a shared dial (same rule as RAW IQ) |
| ✗ | Own Windows kernel driver | — | weeks + EV cert + attestation each release | — | not for a free project |
| ✗ | iOS/Android virtual mic | — | not possible | — | build decoders in the app instead |

**UI flow in each tier:**
- **Tier 1:** AUDIO panel → **SEND AUDIO TO…** next to RAW IQ OUT → pick the cable → WSJT-X input = the cable's
  other end. The panel shows a one-line hint per OS with the cable link.
- **Tier 2 (Stuart's flow):** AUDIO panel → **VIRTUAL SOUND CARD** next to RAW IQ OUT → shows a **six-character code**
  (as RAW IQ does through the tunnel) and "Open VibeIQ and enter this code" → VibeIQ: *Code* → *Connected:
  "VibeSDR Receiver" is now a microphone on this computer* → WSJT-X: Input = *VibeSDR Receiver* (Linux/macOS) or
  *CABLE Output* (Windows). On the LAN the code still works (the token is the credential). No raw port is needed, so
  unlike RAW IQ there is no "address and port" variant.
- If VibeIQ is installed and the page is on https, the button can also offer Tier 1 directly ("or send it from this
  page"). Both live under one control, so there are not two buttons that do nearly the same thing.

**First milestone, and what it proves:** Tier 1 in **Chrome on Windows with VB-CABLE**, on a tunnel page of the Pi
500 (an HF radio), feeding **WSJT-X on 14.074 FT8 for one hour**. Measure:
- decode count against WSJT-X on a local sound card;
- the DT offset (that is our latency);
- whether the stream survives a tab in the background and a tunnel drop.
If that works, Tier 1 ships and the VibeIQ tiers become "no browser, no third-party cable" polish, prioritised by
demand. If decodes fall well short, the problem is in the audio path (Opus, AGC, concealment), and that would hit
VibeIQ equally. Better to learn it before building the VibeIQ tier.

**Risks:**
- Chrome's mic-permission prompt for a speaker picker confuses people (copy).
- Mute/volume semantics in cable mode (§1.3).
- Tab discard on long runs.
- macOS `coreaudiod` restart quirks.
- VB-CABLE bundling etiquette (ask VB-Audio).
- PCM uplink cost on owners' slow links (owner switch, mono 24 k).
- Session-limit lifetime once the page can close.
- AGENTS.md: wherever the new control lands, the tour/About/website copy must say where it is.

---

## Sources
- MDN browser-compat-data 8.1.4 (2026-10-01), via `unpkg.com/@mdn/browser-compat-data/data.json`: `api.AudioContext.setSinkId`,
  `api.HTMLMediaElement.setSinkId`, `api.MediaDevices.selectAudioOutput`.
- MDN: [AudioContext.setSinkId](https://developer.mozilla.org/en-US/docs/Web/API/AudioContext/setSinkId) ·
  [HTMLMediaElement.setSinkId](https://developer.mozilla.org/en-US/docs/Web/API/HTMLMediaElement/setSinkId) ·
  [MediaDevices.selectAudioOutput](https://developer.mozilla.org/en-US/docs/Web/API/MediaDevices/selectAudioOutput)
- [WebUSB API spec (WICG)](https://wicg.github.io/webusb/)
- [BlackHole — ExistentialAudio](https://github.com/ExistentialAudio/BlackHole) (GPL-3.0, HAL path, coreaudiod restart, build constants)
- Apple: [AudioDriverKit and virtual devices — forums 682035](https://developer.apple.com/forums/thread/682035) ·
  [Create audio drivers with DriverKit, WWDC21](https://developer.apple.com/videos/play/wwdc2021/10190/) ·
  [Entitlements for a virtual audio driver — forums 736357](https://developer.apple.com/forums/thread/736357)
- Microsoft: [Driver code signing requirements (EV for attestation)](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-reqs) ·
  [Driver signing policy](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-policy--windows-vista-and-later-) ·
  [Windows 11 APO APIs](https://github.com/MicrosoftDocs/windows-driver-docs/blob/staging/windows-driver-docs-pr/audio/windows-11-apis-for-audio-processing-objects.md) ·
  [TechPowerUp: April 2026 kernel trust change](https://www.techpowerup.com/347807/windows-11-will-no-longer-trust-old-drivers-by-default-under-new-kernel-policy)
- [VirtualDrivers/Virtual-Audio-Driver](https://github.com/VirtualDrivers/Virtual-Audio-Driver) (MIT, SysVAD-based, test-signed) ·
  [Thesycon virtual audio SDK](https://www.thesycon.de/eng/virtual_audiodriver.shtml)
- VB-Audio: [VB-CABLE](https://vb-audio.com/Cable/) · [Licensing / redistribution](https://vb-audio.com/Services/licensing.htm) ·
  [pipemix PR #3 bundling precedent](https://github.com/Garvit-Agrawal7/pipemix/pull/3)
- PipeWire/Pulse: [module-loopback](https://docs.pipewire.org/page_module_loopback.html) ·
  [PulseAudio modules](https://www.freedesktop.org/wiki/Software/PulseAudio/Documentation/User/Modules/)
- [WSJT-X 3.2 user guide](https://wsjtx.github.io/wsjtx/wsjtx-main-3.2_en.html) (48 kHz input, decimated to 12 kHz)
- Android: [MediaRecorder.AudioSource (REMOTE_SUBMIX)](https://developer.android.com/reference/android/media/MediaRecorder.AudioSource) ·
  [jitsi-meet #16701](https://github.com/jitsi/jitsi-meet/issues/16701)
- [pion/opus](https://github.com/pion/opus) (pure-Go Opus; completeness unverified)
- In-repo: `src/services/iqPairing.ts`, `directory/src/index.js` (`iq_codes`), `tools/vibeiq/*`, `web/client/src/main.ts`
  (RAW IQ OUT), `web/client/src/audio.ts`, `android/app/src/main/cpp/local_sdr_shim.cpp` (`/ws/audio`, `VsUncompressedAudio`,
  `specListenerCountLocked`), `docs/WINDOWS-PORT-PLAN.md` (EV declined, SignPath), GitHub release `vibeiq-v1.0.1`.
