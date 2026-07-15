# VibeSDR — Changelog

VibeSDR is free software under the **GNU GPL v3**. Source: https://github.com/Stuey3D/VibeSDR

---

## v9.0.1 — SSB was throwing away half the voice (2026-07-14)

### Fixed — the bandwidth slider was lying to you

Set SSB to 2.7 kHz, and you got about **1.4 kHz** of audio. It still sounded like
speech — just muffled — which is why it went unnoticed for months. You only catch a
bug like this by listening to another receiver on the same signal at the same moment.

The channel filter was built around *half* the stated bandwidth. That is correct for
AM and FM, where the signal sits either side of the carrier. It is wrong for SSB and
CW, where the whole sideband lies on **one** side of it — so the filter was closing
at half-width and taking the consonants with it.

Measured against another receiver on the same 7.187 MHz signal, VibeSDR was **37 dB
down across 2.0–2.7 kHz** — precisely the band that carries intelligibility. Below
1 kHz the two matched almost exactly, which is why it sounded clear, but never quite
*as* clear.

It is fixed. Set 2.7 kHz and you get 2.7 kHz — and above the passband VibeSDR is now
cleaner than the receiver we compared it against.

### Faster — the radio uses 2–8× less CPU

Benchmarking the engine on a Raspberry Pi turned up three inefficiencies, and fixing
them makes VibeSDR dramatically lighter everywhere — most visibly on cheap phones,
which is exactly where it matters:

- **FM stereo now works on a 2019 budget phone that could never manage it before**,
  without breaking up, *while simultaneously streaming to a browser over VibeServer*.
  It was never a stereo bug. The DSP simply couldn't keep up, and now it can.
- **FM broadcast** costs less than half what it did; on a low-end phone the whole
  radio went from nearly three CPU cores' worth of work to one.
- **SSB** costs about a quarter of what it did.
- Turning the **sample rate down** used to make FM *more* expensive, not less. It
  doesn't any more.

Filters now run at the rate that suits them rather than the highest rate available —
same sound out, a fraction of the work.

### Fixed — VibeServer

- **Stations learned from RDS were never saved.** Sit on a station long enough for
  the receiver to learn its name and it appeared, then vanished on restart — it was
  the one thing in the bookmark list that never got written to disk.
- **…and the browser never noticed them anyway.** The web client asked the receiver
  for its bookmarks once, when the page loaded. Learning a station takes about twenty
  seconds, so the station you're actually listening to was *always* learned too late
  to show up. It now keeps asking.
- **Squelch no longer reports itself as a fault.** With squelch closed, the web client
  flashed a red *"NO SOUND — IS THE TAB MUTED?"* and hid the S-meter — the one thing
  you want to watch while waiting for a signal. Now it says, calmly, that the squelch
  is on, and leaves the meter alone.
- **The bandwidth slider** filled from the wrong end on the low side, showing the
  bandwidth you *weren't* using.

### Also

The waterfall's band edges now follow the receiver's ITU region rather than yours.

---

## v9.0.0 — The Apple Watch companion (2026-07-13)

### Added — VibeSDR on your wrist

A real Apple Watch app: not a remote control with a few buttons, but **the waterfall
itself**, live on the watch, drawn from the same data and the same palette as the phone.

- **Turn the Digital Crown to tune.** Tap the frequency to type one on a passcode-style
  pad. Press and hold the waterfall for the menu — **demodulator**, **tuning step**, zoom,
  brightness, contrast and your saved servers.
- **It works with the iPhone locked in your pocket**, which is the whole point — and it
  will start the phone for you. Open the watch app with VibeSDR closed and the phone wakes
  in the background, connects to your default receiver, and the waterfall arrives on your
  wrist **without the phone's screen ever coming on**.
- **Four screens, chosen by what the receiver actually is:** the spectrum waterfall; the
  FM-DX tuner (station, distance, RDS); the DAB service list (the Crown *selects* a
  service — DAB is a list, not a continuum); and the ADS-B aircraft table.
- **Switch receivers from your favourites** without touching the phone.
- **Control the iPhone's system volume and mute from the wrist.** It mirrors the phone's
  *real* volume — including changes you make on the phone itself — so the two can never
  disagree.
- **The band you're in, in words** ("20m Ham Band", "41m Broadcast Band"), from the ITU
  band plan for wherever the **receiver** is — with marks on the frequency ticker showing
  where that band **ends**.
- **Your watch's own battery**, next to the clock. A live waterfall costs the watch about
  a third of a CPU core, and this is an app you might leave running on a hilltop.
- **A first-run guide on each screen.** Everything the app does on a wrist is a gesture,
  and gestures are invisible.

### Fixed — the waterfall could freeze for good, and never come back

With the phone **locked in a pocket on mobile data**, the waterfall could stop and stay
stopped — while audio and tuning carried on working perfectly.

A mobile network can **silently invalidate a connection without ever closing it** (a
CGNAT rebind on a cell handover does exactly this — no FIN, no RST). The audio stream had
a watchdog that ran in the background and healed itself. The spectrum stream's only
recovery ran when the app came back to the *foreground* — which never happens when the
phone is in your pocket. So the socket sat there, open and dead, forever. That asymmetry
is why *tuning kept working while the waterfall was frozen*.

VibeSDR now **actively probes the spectrum link** and rebuilds it the moment it stops
answering, waits for the audio session to confirm itself first (so it can't reconnect into
a session the server has already reaped), and **reacts instantly when the phone changes
network** — Wi-Fi to mobile and back.

### Added — the watch tells you *which* link is rough

There are **two radio hops** in the chain — phone-to-server and watch-to-phone — and they
fail independently. Until now the watch could only shrug. It now shows a small diagram of
which hop is struggling, **over a waterfall that keeps drawing**; tuning goes on working
throughout, and the app says so rather than throwing up a blank screen.

### Fixed — VibeServer

- **The sample-rate box was lying.** On first use the web client showed **3.2 MS/s** while
  the receiver was actually running at 2.4 — it displayed a rate the radio was not using.
  It now defaults to **2.4 MS/s** (the fastest an RTL-SDR can reliably sustain over USB;
  above that the dongle silently drops samples), *tells the receiver* so the two agree, and
  marks the higher rates as liable to drop samples.
- **The web client collapsed on a smaller laptop.** At 1366×768 the control bar's three
  blocks overlapped by ~46 px — the REC button sat underneath BOOKMARKS — and the signal
  readout ran clean off the right-hand edge of the window. The bar now scales properly all
  the way down to 1280×720.
- **The server now shows its name as well as its address** (`vibesdr-yourphone.local`), so
  there's no IP to remember — and it survives the router handing the phone a different
  address tomorrow, which the IP does not.

### Documentation

- **README and About: a Limitations section**, explaining *why* there is no WebSDR support
  (its author does not sanction third-party clients, and VibeSDR only implements platforms
  that welcome them), why digital voice and DAB+/DRM/HD Radio are not decoded natively
  (patent-encumbered codecs — and how OpenWebRX's server-side decoding is the supported
  route), and why the skip buttons disappear on FM-DX (one tuner, many listeners).

---

## v8.0.1 — Favourites fix (2026-07-12)

### Fixed
- **Saved favourites could start connecting as a VibeServer** instead of the UberSDR
  receiver they actually are — and, worse, the wrong answer was then **written back onto
  the favourite**, so it stayed wrong.

  VibeSDR identifies a server by looking at its web page. It was treating the word
  *VibeSDR* as evidence of a **VibeServer** — but that is the *client's* name, not the
  server's, and UberSDR receivers carry an "open in VibeSDR" deep link of their own. So a
  perfectly ordinary UberSDR looked like one of ours. (Reaching the same receiver through
  the UberSDR directory still worked, because that route already knows the type and never
  needs to guess — which is why it only bit favourites.)

  VibeSDR now looks for a marker only a real VibeServer carries, and **repairs any
  favourites that were mislabelled**. Favourites whose type is cleared are simply
  re-identified the next time you connect.

### Changed
- The in-app **About** page listed the v7.1 changes under a "V8.0.0" heading and never
  mentioned VibeServer at all. It now has a proper V8.0.0 section, and V7.1.0 has its own
  again.

## v8.0.0 — Now introducing VibeServer (2026-07-12)

> **Your phone is now the receiver — and anyone can listen to it in a browser.**

### VibeServer
- **Turn an Android phone with an RTL-SDR into a receiver anyone on your network can
  use** — from a **web browser**, or from VibeSDR on another phone. Point a browser at
  the serving phone's address and the full client is there: no install, no app.
- The **serving phone does all the DSP** and sends compressed audio plus a ready-made
  waterfall, so it's roughly **25× lighter on the network than raw RTL-TCP** — it works
  comfortably over Wi-Fi, and over a phone hotspot.
- The web client is the real thing, not a cut-down view: waterfall and spectrum with the
  same palettes and colouring as the app, **click-to-tune, panning and cursor zoom**,
  audio with **recording**, the **decoders** (RTTY, NAVTEX, WEFAX, SSTV, and FT8 with its
  map), **station search**, **bookmarks** you can export, the band plan, and **OS media
  controls with artwork** on the lock screen.
- **Decoders run on the server**, as they do in OpenWebRX — a browser doesn't have to do
  any DSP, and a phone or tablet client stays cool.
- **PIN protected** by HMAC challenge-response, so the PIN itself never crosses the
  network. Or run it open on a trusted LAN.
- **Turn the web client off** and only the VibeSDR app can connect, so nobody can stumble
  into your receiver from a URL.
- **Bandwidth: client-controlled or pinned.** Leave clients free to pick their own span,
  or pin it — pinned, the client's picker disappears and tells them the server set it.
  Enforced on the server, not just hidden in the UI.
- **Receiver location, opt-in.** Granting location to sort a server list is not consent to
  broadcast your position, so publishing is off until you choose it: use the device's
  coarse position, or name a town, or give a Maidenhead locator (which needs no internet
  — the shed case). Clients then show the receiver's **name and place** on the spectrum,
  and — importantly — measure spot distances, map centring and the **regional band plan
  from the ANTENNA**, not from wherever the listener happens to be sitting.
- **Survives a crash.** If the app is killed while serving, the server rebuilds itself and
  carries on. (After a phone *reboot*, replug the dongle — Android doesn't detect one that
  was plugged in while the phone was off.)
- Warns you if the phone is **background-restricting** VibeSDR, which would otherwise
  starve the server with no visible explanation.

### Station bookmarks that learn themselves
- **The receiver names the stations it can hear.** When a station announces itself over
  RDS, VibeSDR remembers it against the frequency — so the search bar fills itself in
  with the stations this aerial *actually receives*, rather than a schedule of what
  merely exists. Works while listening on local hardware as well as while serving.
- It keeps itself honest. The **PI code** identifies the station (short, error-protected,
  and present in every RDS group), so if you move and a different broadcaster is on that
  frequency, the change is spotted immediately. If a frequency simply goes quiet, the
  bookmark **expires after 30 days unheard** — so a station you can no longer receive
  doesn't sit on top of static forever.
- The name is **reconstructed by majority vote** across repetitions. RDS corruption is
  random each time, so "H%art", "He%rt" and "H**r%" all vote for **Heart** — it recovers
  a name that no single transmission ever delivered cleanly, and it declines to guess at
  all when the signal is too poor to be sure.
- **Save to the receiver** (shared with everyone who connects) or **to this browser**
  (private to you) — marked with a server-rack and a monitor icon respectively. Import an
  existing list to either, including an UberSDR YAML export. The server's own list can be
  imported and reset from the phone.

### "vibesdr.local"
- Point a browser at **`http://vibesdr.local`** — no IP address to remember. The name
  follows the server's advertised name, and if two phones serve on the same network the
  second renames itself automatically.

### Station logos and flags
- Logos and country flags now actually appear — across **every backend**, and on AM and
  shortwave stations too, not only FM. They were previously so rare as to look broken: a
  station named "Heart" could never match "Heart FM" in the logo database, and the country
  needed to anchor that search is something most stations never transmit.
- Where the country genuinely cannot be known — a Spanish station arriving on sporadic-E,
  say — VibeSDR now **declines to show a flag** rather than showing your own country's.

### Custom server — one box reaches every backend
- The RTL-TCP box becomes **CUSTOM SERVER**. Type any address — `192.168.1.50:8073`, or
  `myserver.example.com:8073` — and VibeSDR **works out what's listening**: VibeServer,
  OpenWebRX, KiwiSDR, UberSDR, FM-DX Webserver, rtl_tcp or SpyServer.
- Give it a name, and it's saved as a favourite that reconnects straight to the right
  backend. (rtl_tcp and SpyServer speak raw TCP with nothing to identify them, so on a
  non-standard port you can still pick the type by hand.)
- **Local Hardware is now RTL-SDR**, with **Listen** and **Use as server** side by side.

### Fixes
- **Entering a frequency in another band now switches the demodulator and span to match.**
  Jumping from a medium-wave station to an FM one used to leave you in AM with a 5 kHz
  filter — audio broke up, and the span looked stuck zoomed in.
- **The waterfall no longer shows half a minute of stale history after a big jump.** It
  wasn't frozen; it was still scrolling out rows from the band you'd left.
- **The lower sample rates (0.96 and 1.2 MHz) no longer break up.** The USB buffer was
  sized in samples, not time, so a low rate meant a ~136 ms buffer.
- **rtl_tcp no longer plays chipmunks** on rates the tuner quantises.
- **Dragging the gain slider no longer breaks up the audio.** Each step was a USB control
  transfer competing with the sample stream; they're now coalesced.
- **Panning past the tuned station no longer drops audio or crawls.**
- **Auto-contrast now defaults to 5** — 10 was too dark.

---

## v5.1.0 — Unlocked VFO + waterfall panning, recordings player (2026-06-23)

### Waterfall panning & the VFO Lock
- **Waterfall panning is back, as an opt-in.** A new **VFO Lock** toggle in the
  menu (replacing the old centre button) controls it. **Locked is the default and
  is byte-for-byte the previous behaviour** — the VFO stays pinned to the centre.
  Unlock it and you can **drag the waterfall to pan around the band while staying
  tuned**; the VFO needle can sit off-centre or off-screen.
- A floating **CENTRE ON VFO** button appears (above the controls) whenever the
  VFO is unlocked and off-screen — one tap snaps the view back to the VFO without
  re-locking.
- **Gestures are now: tap = tune, drag = pan, pinch = zoom.** The old vertical
  drag-to-zoom is gone (it fought panning). Panning runs **on the UI thread**, so
  the grab/drag stays responsive even when the connection is laggy or busy with
  incoming data.
- Discrete jumps (frequency entry, bookmarks, VTS, Siri, lock-screen skip) always
  re-centre on the target, regardless of the lock.

### Second VFO for the RF (dongle) centre — Local Hardware / RTL-TCP
- On the on-device backends the **dongle centre is now a true second VFO**, drawn
  as a dashed **RF-CENTRE** marker with its own frequency readout.
- With the VFO unlocked you can **pan the view across the entire captured
  bandwidth at full resolution** while a station stays tuned. The dongle follows
  your pan until the tuned station nears the capture edge, then **locks** — and the
  view keeps scrolling, the RF-CENTRE marker sliding off to the side, up to the
  hard **walls** at the capture-band edges.
- The pan limits track the **actual sample-rate / bandwidth mode** the device
  reports (so 250 kHz, 1 MHz, 2.4 MHz etc. each get the right walls), keeping the
  tuned station clear of the anti-alias rolloff.

### Saved Recordings player
- New **Recordings** screen (from the menu) lists every audio recording you've
  made — **play them back in-app** with a scrub bar, **share**, or **delete**.
  Previously a recording you didn't immediately save from the share sheet was
  stranded in app storage with no way to reach it.
- Recordings are now kept in the app on **both platforms** (Android no longer
  pushes them straight to the Music library), so the browser is identical on iOS
  and Android. The live SDR is paused while a recording plays so the two don't
  fight over the audio session.

---

## v5.0.1 — CW fix on local hardware (2026-06-20)

### Fixed
- **CW now demodulates correctly on the Local Hardware (USB RTL-SDR) backend.**
  The beat-note offset and the actual filter width had drifted apart: the client
  applies a narrow CW passband, but the demodulator was placing the carrier
  outside it, so a signal tuned dead-on produced silence and you could only hear
  morse when tuned well off the signal. The CW filter and beat note are now kept
  in sync — tune straight onto a CW signal and you get a clear, audible ~600 Hz
  tone with readable morse.
- The mode pill now reads **CW** (matching the single CW button) instead of the
  internal sideband id.

---

## v5.0.0 — Native, GPL-free on-device DSP (2026-06-20)

**The headline: the on-device radio no longer uses any third-party GPL DSP.**

VibeSDR's Local Hardware (USB RTL-SDR on Android) and RTL-TCP (Android + iOS)
backends used to demodulate IQ with a bundled copy of the **SDR++ Brown** DSP core
(plus **FFTW** and **VOLK**). V4 was built on SDR++ Brown to get on-device SDR up
and running quickly — and we're grateful for it. **In V5 that has been removed
entirely** and replaced with **VibeDSP**, VibeSDR's own clean-room signal-processing
engine, written from scratch.

Why it matters: VibeSDR stays GPLv3 (its own choice — it fronts free/open SDR
servers and decoders, so FOSS is the right fit), but because **no SDR++ / FFTW /
VOLK code is bundled anymore**, the app can ship on the public App Store and Play
Store without the GPL-on-store concerns that bundled third-party GPL code can raise.

### New DSP engine (VibeDSP)
- Clean-room, from-scratch C++ engine — only permissively-licensed **KissFFT**
  (BSD-3) is vendored; everything else is original VibeSDR code.
- **Hand-optimised with ARM NEON SIMD** across every hot path (FFT power/dB,
  channel filters, FIR decimator + resampler, IQ conversion) plus trig-free
  recursive oscillators. Runs **noticeably cooler and lighter on the battery**,
  especially on low-end phones and tablets.

### DSP improvements over the old engine
- **True single-sideband SSB** with proper image rejection (Weaver method) — the
  unwanted sideband is silent instead of bleeding through (was effectively DSB).
- **Genuine FM stereo**: 19 kHz pilot PLL with a smooth stereo blend, a reliable
  stereo indicator, RDS station name / RadioText, and a **force-mono** switch.
- **Audio AGC** for AM / SSB / CW — steady level, no fading or crackle.
- **Working FM de-emphasis** (50 µs / 75 µs / off) — the control now takes effect.
- RDS no longer flickers, and clears immediately when you retune.

### Reliability / UX
- Fixed a USB-teardown crash and a session-switch race that could stop a new
  RTL-TCP session from connecting.
- Local-USB sample-rate selector hides sub-1 MHz rates (a dongle is sluggish
  there); RTL-TCP keeps the low rates for sources like UberSDR.
- About page and menus updated; honest credit retained for SDR++ Brown (bootstrap
  + colour palettes) and the other open projects VibeSDR builds on.

### Platforms
- Android: Moto G35, Galaxy Tab A9 and similar — smoother and cooler than V4.
- iOS: RTL-TCP retained; the IPA is now free of bundled GPL DSP.

---

## v4.1.0 and earlier

See the in-app **About → Changelog** for the full V1–V4 history (first-run tutorial,
bookmark overhaul, multi-backend support — UberSDR / OpenWebRX / KiwiSDR — Android
Auto, Siri voice control, local SDR hardware, and more).
