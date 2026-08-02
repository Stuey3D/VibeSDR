# BRIEF — per-client DSP: independent tuning AND real zoom resolution

**Written 2026-08-02, from an evening with an RSP1B at 8 MSPS on the Pi 500.**
Not started. This supersedes "add a zoom FFT" as a standalone task — see §3 for why.

---

## 1. The symptom that started it

Zoomed to ~16 kHz around the Buzzer (4625 kHz), our spectrum is blocky. UberSDR at a
comparable zoom shows real structure — individual signals, diagonal streaks in the waterfall.

The numbers say why. `fftSizeForRate()` caps at **32768**, so at 8 MSPS a bin is **244 Hz**.
A 16 kHz view is therefore ~65 real bins stretched across 1024 output bins: about **16x
interpolation**. Cropping cannot add resolution, and the code already says so:

> "ZOOM IS CAPPED BY REAL RESOLUTION… Zooming past the point where the FFT has bins to show is
> magnifying nothing. ★ To zoom DEEPER honestly, raise fftSize; do not raise this."

★ Raising `fftSize` is not the answer: filling 1024 real bins at 500x zoom needs a **512k-point
FFT every frame**. The answer is to make the bins narrower, not more numerous.

★★ The data rates make the point on their own — UberSDR **11 kB/s at 10 fps** against our
**27 kB/s at 19 fps**. It sends LESS data and shows MORE detail, because its 1024 bins each
cover a narrow slice.

## 2. What a zoom FFT is

Digitally down-convert to the view centre, decimate to the view span, FFT that narrow stream.
1024–4096 points is plenty: a 16 kHz span at 4096 points is ~4 Hz/bin. **Cost is roughly
constant regardless of zoom depth**, because the FFT never grows.

Handover rule, already computable in `onSpectrum`: `step` is source-bins-per-output-bin, so
**`step < 1.0` IS "zoomed past real resolution"** — that is exactly when the narrow path should
take over, and when zoomed out the existing wide path is both correct and cheaper.

## 3. ★★★ WHY THIS IS THE MULTI-USER FEATURE, NOT A SPECTRUM FEATURE

Stuart: *"UberSDR can offer different zoom levels per person… that is the model we need for our
multiple users one profile mode."*

UberSDR can do that because **every client already has its own CHANNEL** — it must, since
listeners tune independently. Once a per-client narrow-band stream exists for the demodulator, a
zoom FFT on it is nearly free: the expensive part is already paid. **Per-user zoom falls out of
per-user tuning.** (How that channel is produced is §4 — fast convolution, not a private DDC.)

Our shim is the opposite shape — a deliberate singleton, *"one radio, one pipeline, one server
per process"*, with ONE `audioFreq`, ONE `viewCenter`, ONE `zoomFactor`. Every listener shares
one VFO and one view.

★★ So building a zoom FFT against the shared pipeline would be **the wrong layer**: it would be
rebuilt per-client the moment multi-user landed. Build per-client DSP once and get both.

## 4. ★★★ SHAPE OF THE WORK — FAST CONVOLUTION, NOT N DDCs

Stuart: *"copy UberSDR's homework, it may be something to do with ka9q."* Correct, and it changes
the design. **ka9q-radio is "Multichannel SDR based on fast convolution and IP multicasting"**
(github.com/ka9q/ka9q-radio). Its `radiod` runs ONE shared forward FFT (overlap-save) and each
channel is *"independently tunable with its own sample rate and filter response curve, requiring
only that the impulse response of the channel filters be shorter than the configurable overlap
interval in the forward FFT"*. Its docs note the forward FFT is *"by far the single most
cpu-intensive operation in the entire package"*.

★★ WE ALREADY RUN THAT FORWARD FFT — 32768 points at 8 MSPS, for the spectrum. So the expensive
part is paid for. A per-client channel becomes:
    select the bins covering the channel → multiply by its transfer function → INVERSE FFT
        → time domain at that channel's own rate → demod (+ a small FFT for its zoomed spectrum)
rather than a private NCO + decimating FIR chain per listener. That is the difference between
"a few listeners" and "a lot of them", and it is why ka9q can run dozens of channels.

★ The constraint to design around: **the channel filter's impulse response must be shorter than
the forward FFT's overlap interval**. That sets the narrowest usable channel filter for a given
FFT size and overlap, and it is the number to work out FIRST.

★ `vibedsp` already has `NCO` and `FirDecimator` if a simple per-client DDC is wanted as a
stepping stone — but it does not scale the way fast convolution does, so it is a prototype route
at best, not the destination.

## 4b. The rest of the work
- The **wide** FFT stays shared and zoom-independent: it is the overview everyone sees when
  zoomed out, and it feeds the station-presence/SNR maths that must not become per-client.
- Per-client state is the hard part, not the DSP: `audioFreq`, `viewCenter`, `zoomFactor`,
  `rxMode`, `rxBwHz` are all currently globals on `Impl` — see [[vibeserver_per_client_state_globals]].
- **Hardware retune stays global and shared** and is the real constraint: the radio has ONE
  centre frequency, so clients can only tune independently WITHIN the captured span. At 8 MSPS
  that is a genuinely useful window (2.5–10.5 MHz covers 80m→30m); at 912 kHz on the HF+ it is
  not. Whoever moves the hardware centre moves it for everybody — that policy needs deciding
  BEFORE the code (owner-only? first listener? locked while >1 client?).

## 5. Cost and the ceiling

The Pi 500 with NEON is ~13x the old 32-bit box and **DSP is no longer the limit, BANDWIDTH is**
([[vibeserver_pi500_headless_tui]]). With fast convolution a per-client channel is an INVERSE FFT
of a slice plus a demod — cheap next to the forward 32k FFT already running, which is the part
ka9q calls the most expensive thing it does. The thing to watch for the demo is therefore the
**uplink**, not the CPU: several listeners each receiving their own spectrum stream on an 8 MHz
span.

## 6. Deliberately NOT doing

- **Raising `fftSize`** — see §1, it does not scale.
- **Touching the waterfall pan** — Stuart: *"took us ages to get it to the state its in, its
  responsive enough and works and is accurate."* Its lag is round-trip latency, not resolution.

---

## 7. THE SHARED-RECEIVER MODEL — decided 2026-08-02, mostly NOT built

Settled in conversation while the Pi demo ran. The DSP half is done (§4, and the
channelizer is measured); this is the product half.

### 7.1 Two receiver modes, one flag
`lockedCentre > 0` IS "shared receiver", and every behaviour below branches on it. It is already
the gate for the shared channel method, so the paths cannot drift apart.

| | Unlocked (personal) | Locked (shared) |
|---|---|---|
| Hardware control | the listener's | the OPERATOR's, nobody else's |
| Admin arriving | **takes the session** (built) | **never evicts** — becomes slot N+1 |
| Idle park | yes (the dongle is ~80% of battery) | **NO** — keeps the AGC converged (built) |
| LOCKED/FREE VFO | moves the dongle | purely a view preference, as on Uber/Kiwi |

### 7.2 Admin is an EXEMPTION, not an eviction
Stuart: *"the admin simply becomes a hidden number 21 with elevated privileges."* On a
one-at-a-time receiver taking the session is right — someone must yield. On a shared one there is
nothing to yield, so admin is exempt from the CAP and the SESSION TIMER rather than entitled to
displace anyone. ★ Hidden from the public count (it answers "can I get in", and the operator is
not competing for a slot) but visible to the admin themselves.

### 7.3 `--users N` becomes the actual cap
It currently only picks the channel method. Stuart describes it as slots, so: N visitor slots,
admin is N+1, and N > 1 implies the shared method. One number an operator understands.
- ★★ **N IS A GUESS UNTIL THE UPLINK IS MEASURED.** The channelizer means DSP will not stop us
  (+0.02%/listener) but the Pi's ceiling is BANDWIDTH ([[vibeserver_pi500_headless_tui]]). At
  ~25-50 KB/s per listener, 20 listeners is 0.5-1 MB/s sustained UP. Measure 2-3 real clients
  first — the sharing features have never run with two ([[vibeserver_sharing_limits]]).
- ★ Count REMOTE listeners: an admin on loopback/LAN uses none of the uplink the cap protects.
- ★ Lowering the cap must NOT evict — it applies to new joins; occupancy drains.

### 7.4 The GUI moves into the clients; the TUI becomes bootstrap only
Stuart: *"rather than a pretty unintuitive TUI, use that to set up basic settings then the GUI for
the server could be in the client and app behind the admin password."* One admin UI instead of two
that drift, and it matches how Kiwi and OpenWebRX work.
- **TUI keeps only**: radio, centre + rate, port, admin password. ★★ AND A PASSWORD RESET — once
  the password is the only way in, losing it locks the operator out of their own receiver. The
  local console is the escape hatch.
- **Clients gain** a RADIO section (centre, rate, gain, filters — changes what every listener sees
  RIGHT NOW) and a SERVER section (slots, session limit, fps cap, uncompressed policy).
- ★★ A radio change RESTARTS the engine and moves the band under everyone. Say so before doing it
  — "this will move the band for 14 listeners". An unexplained transient reads as a fault.
- ★★★ **PERSISTENCE IS THE TRAP**: every setting is a startup flag today. An operator lowers a
  limit from their phone, the Pi reboots, and it silently reverts to what systemd passes. Either
  write back to the config the service reads, or SAY it lasts until restart.
- ✅ Safe to expose: admin unlock is already **HMAC(adminSecret, nonce)** — the same
  challenge-response as the PIN, so the secret never crosses the wire even on plain HTTP.
- ★★★ **SET AN ADMIN PASSWORD ON THE PI BEFORE ANY PUBLIC LINK.** It currently has none and says
  so at startup; bias-T, direct sampling and calibration are open to anyone who can reach it.

### 7.5 Landing screen
A live listener count (and capacity — "3 listening" means nothing without knowing if that is full).
The identity endpoint already carries `isBusy` / `occupantSecsLeft`; both need to become counts.

### 7.6 First-run: the TUI gets it ONLINE AND PROTECTED, the client configures it
Stuart, 2026-08-02: *"the TUI exists to get vibeserver online and protected, the admin GUI in the
client and the app exists to configure it fully."*

- **TUI (headless Linux only)**: radio detection, PORT, mDNS name, and the **admin password**.
  ★★★ The password stops being optional — "protected" is half the job. A headless box heading for
  a network with no admin secret is the exact hole the Pi has tonight (it warns and starts anyway).
  Require it, or generate one and show it ONCE. Keep a TUI **password reset**: once it is the only
  way in, losing it locks the operator out of their own receiver.
- **First connection** then demands the admin password and runs a **setup wizard** in the client —
  a headless server has no GUI of its own, so the first client to connect IS its configuration
  surface.
- ★★ **TWO DIFFERENT THINGS, DO NOT CONFLATE**: the **first-run wizard** is headless-only, but the
  **ongoing admin panel** is useful anywhere — administering a Mac-hosted server from a phone is
  the same problem. Mac and Android have full local GUIs and must never see the WIZARD, or there
  are two configuration paths that can disagree about one server.
- ★ **No platform check needed**: the server advertises **"unconfigured"**, and only a headless
  build that was never given settings is ever in that state. The client shows the wizard when
  TOLD to, rather than inferring it — the same reason lockedCentre is published rather than guessed.

### 7.7 ★★★ ANDROID'S SIMPLICITY IS A FEATURE — DO NOT SPEND IT
Stuart, 2026-08-02: *"multiple people amazed at how easy android was to get working. Download
VibeSDR — Plug in Radio — Listen Locally/Use as Server — Start server — connected."*

**Five steps, no configuration, no password, no wizard.** That is a competitive advantage over
every other SDR server (Kiwi, OWRX and UberSDR all want a config file and an install), and it is
the kind of thing that erodes one justified step at a time. Nothing in §7 may add a step to it.

✅ **VERIFIED 2026-08-02, and it must stay true**: `setVibeServerLockedCentre`,
`setVibeServerSharedChannels` and `setVibeServerZoomSpectrum` are called from **`vibeserver/main.cpp`
ONLY** — the headless Linux binary. Nothing in `src/`, the Android Java or the native module
touches them, so the phone flow is unchanged:
- `lockedCentre = 0` → not a shared receiver → admin still takes the session, LOCKED/FREE still
  moves the dongle, and ★ **IDLE PARK STILL RUNS** (the never-park rule is gated on the lock —
  a phone keeps its battery saving, and the dongle is ~80% of it).
- `sharedChannels = false` → **Direct**, which is genuinely the CHEAPEST option for one listener
  (11% of a Pi core vs 27%) — the right default for a phone, not a compromise.
- `zoomSpectrum = false` → the wide path, waterfall exactly as before.

★★ **THE RULE THAT MADE THIS FREE**: every shared-receiver behaviour branches on `lockedCentre`
rather than being switched on globally. Keep doing that. A feature that a phone user must
DECLINE has already cost them a step; one they never see costs nothing.
★ The admin password becomes mandatory for the HEADLESS build only (§7.6) — the phone must never
be asked for one to serve on its own LAN.

### 7.8 ★★★ POWER OUTRANKS THE AGC — the never-park rule must be ORDERED, not absolute
Caught 2026-08-02 the same evening it was written, from Stuart's use case: *"the old android up the
allotment with a solar panel."*

§7.1 says a shared receiver NEVER idle-parks, because parking makes the AGC re-converge and a
listener on a locked receiver has no gain controls to rescue it. That reasoning holds on a
MAINS-powered Pi and **inverts on solar**: the dongle is ~80% of the draw, and a flat battery costs
the whole receiver, not one listener's AGC.

★★ `--force-idle-saver` ALREADY EXISTS for exactly this — *"a solar/cellular host where power
outranks a listener's preference"*. So the rule is:
> Do not idle-park on a shared receiver **UNLESS the operator has said power comes first**, in
> which case parking wins and the AGC re-converges on arrival.

★ The current code overrides that switch for every locked receiver. **Fix before anyone runs a
shared receiver on battery.** The general shape: a new rule that quietly overrides an existing
OPERATOR CHOICE is a bug, however good its own reasoning.

### 7.9 What a remote/solar host needs from the admin panel
- **It must work over a BAD link.** An allotment host is on 4G or a long Wi-Fi bridge, and the
  panel is the one thing that has to stay usable while the spectrum stream is struggling — it is
  how you would FIX the struggling link. Do not build it on top of the spectrum socket's health.
- **It must REPORT, not just accept.** Battery, draw, whether it is parking, uptime. Managing a
  solar box blind is how you discover it died yesterday. Most of this already exists in
  diagnostics ([[vibeserver_battery_measured]], the power meter) — it needs surfacing remotely.
