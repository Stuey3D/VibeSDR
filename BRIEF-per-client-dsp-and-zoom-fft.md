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

UberSDR can do that because **every client already has its own DDC** — it must, since listeners
tune independently. Once a per-client decimated narrow-band IQ stream exists for the
demodulator, a zoom FFT on that same stream is nearly free: the expensive part is already paid.
**Per-user zoom falls out of per-user tuning.**

Our shim is the opposite shape — a deliberate singleton, *"one radio, one pipeline, one server
per process"*, with ONE `audioFreq`, ONE `viewCenter`, ONE `zoomFactor`. Every listener shares
one VFO and one view.

★★ So building a zoom FFT against the shared pipeline would be **the wrong layer**: it would be
rebuilt per-client the moment multi-user landed. Build per-client DSP once and get both.

## 4. Shape of the work

- **The DDC already exists**: `vibedsp` has `NCO` (mix to DC) and `FirDecimator`. No new DSP maths.
- **Per-client channel** = NCO + decimator + demod + a small FFT. One per connected listener.
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
([[vibeserver_pi500_headless_tui]]). A per-client channel is a decimator plus a small FFT —
cheap next to the wide 32k FFT already running. The thing to watch for the demo is the **uplink**:
several listeners each receiving their own spectrum stream, on an 8 MHz span.

## 6. Deliberately NOT doing

- **Raising `fftSize`** — see §1, it does not scale.
- **Touching the waterfall pan** — Stuart: *"took us ages to get it to the state its in, its
  responsive enough and works and is accurate."* Its lag is round-trip latency, not resolution.
