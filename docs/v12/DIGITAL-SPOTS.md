# Digital spots (V12): Single mode, Band, and the mode registry

Stuart, 2026-10-25/26: two buttons. **"Digital spots: Single mode"** is today's behaviour: the mode(s) at your dial,
cheap, on any server. **"Digital spots: Band"** reads a whole band's cluster of digital-mode frequencies at once, with
every mode's decoder reading its own slice. And the map is coloured *by mode*, because *"most of our users will be using
one SDR at a time so only able to cover one band at a time"*.

Branch `v12-spots` (worktree `~/VibeSDR-spots`).

| Part | Where |
|---|---|
| The interface a mode implements | `android/app/src/main/cpp/decoders/digi_mode.h` |
| The registry (the list of modes) | `decoders/digi_modes.cpp` |
| FT8 + FT4 as modes (the pattern to copy) | `decoders/digimodes/ft8.h` |
| Bands, published dials, spans, band stepping | `decoders/digi_bands.h` |
| Band mode's per-dial receiver | `decoders/digi_tuner.{h,cpp}` |
| One listener's run (Single or Band) | `decoders/digi_spotter.{h,cpp}` |
| The host (slot, IQ/audio, spot frames, gallery tap) | `vibe_decoder_host.h` (`startSpots(scope)`) |
| The server (subscribe, hwinfo, health, owner switch) | `local_sdr_shim.cpp` (`vsSpotSpans`, `vsSpotsBandWhyNot`) |
| Benchmark row and default | `vibe_benchmark_decoders.h` (`dec_ft8band`), `vibe_setup_page.h` `benchApplyDefaults`, `ServerModeScreen.tsx` `applyBench` |
| Gallery summary per mode | `vibe_bgdecode.h` (`Ft8Tally::byMode`, `Ft8SummaryTap::labelFor`) |
| Client styles + band stepping | `src/utils/digiModes.ts` (web and app) |
| Web client | `web/client/src/main.ts` (search "DIGITAL SPOTS: SINGLE MODE AND BAND"), `decoders.ts`, `index.html` |
| App | `src/components/MapOverlay.tsx` (map by mode), `DecoderPanel.tsx` (Mode column colour) |

## Architecture

### Single mode
This is the button there always was. The listener's 48 kHz audio is box-averaged by four to 12 kHz, the same decimation
as before, and goes into every registered mode with `single: true`. Today that is FT8 and FT4 on one passband. Spot RF is
the listener's dial plus the audio offset.

### Band mode
The cheapest correct route is **one IQ slice per band, then one tuner per mode dial on the decoder thread**. A wide audio
channel would not work:

- A real audio channel is one-sided. A 43 kHz band needs 86 kHz or more of audio rate, which no listener channel has.
- Every mode would still need its own shift and filter, and they would all be cut from audio that a demodulator's AGC had
  already pumped on the strongest station in 43 kHz.

The slice uses the CW skimmer's path, which already exists:
- **Range-locked radio:** one more `extract` of the forward FFT that every listener's channel already comes from
  (`feedOneClient`, `clientCentreBin`).
- **One-pipeline radio:** `vibe_wide_tap.h`.

Either way the DSP thread pays one small inverse FFT per block. The slice is centred on the listener's dial. Its width is
`DecoderHost::bandWidthFor(dial)`: the registered modes' span for the band the dial is on, wide enough on both sides that
a dial typed off-centre still covers the whole span. If the CW skimmer runs too, the two share the slice at the wider
width, and each decodes only its own part.

On the host's decode thread, `digi::Spotter` plans what can run in each block. A plan covers every registered mode and
every one of its dials whose passband fits inside the slice, leaving out the outer 4 % on each side, which is the slice's
own roll-off. Each distinct dial gets a `DigiTuner`. FT2 and JS8 both use 3.578 MHz, so they share one tuner. Each mode on
the dial gets an `Engine`.

When the slice moves, the plan is made again:
- Dials still inside keep their tuner and engine, so a fine retune does not lose a slot.
- Dials no longer inside are dropped.

The tuner (`digi_tuner.cpp`) is written from textbook multirate DSP. For each output sample at 12 kHz it does five things:
1. **Mix at the slice rate.** The wanted passband's centre is moved to 0 Hz using vibedsp `rotateBlockIlv`, which is a
   four-lane rotator (NEON / SSE2 / scalar).
2. **Decimate to 12 kHz.** A polyphase Kaiser windowed sinc (70 dB) computes only the outputs, choosing the nearest of 256
   phases for each. Its stopband starts at 12 kHz minus the passband, the first frequency that would alias into the
   passband, so the filter is short: 40 taps at 75 kHz.
3. **Shape at 12 kHz.** A complex low-pass at ± half the passband with a 300 Hz transition (60 dB, 147 taps), run on both
   rails with `dotReal` (NEON / SSE2). This filter removes the lower sideband. Without it, RF just below the dial would
   fold onto the audio when the real part is taken. Measured rejection: 90 dB.
4. **Up-mix and take the real part.** The passband centre goes back up, giving USB audio with the dial at 0 Hz. The
   oscillator is a recursion, set exactly from the phase once per block.
5. **Slow level control** (τ ≈ 8 s) to RMS 0.05. The slice is raw IQ at whatever level the radio's gain set, and ft8_lib's
   waterfall maps power over a fixed 127 dB range.

### Bands, dials, spans
All of these are in `digi_bands.h`, typed in by hand.

**Bands.** The step order is 2200 m … 10 m, then 6 m, 4 m, 2 m, 70 cm. The band edges are the widest of the three IARU
regions.

**Published dials.** Several modes are listed here before they exist, so that their engines need no dials of their own.

| Mode | Source | How checked |
|---|---|---|
| FT8 | WSJT-X defaults | HF: lowest spot on each dial in STUEY3D's UberSDR decodes, 2026-10-25. FT8 on 60/40/30/20/17/15/12/10 m confirmed from a 12:00–12:30 sample. |
| FT4 | The FT4 protocol's published dials | HF: as FT8 |
| WSPR, FST4W | WSJT-X | HF: as FT8 |
| JS8 | JS8Call defaults | HF: as FT8 |
| FT2 | Observed on UberSDR (no published plan found) | — |

- **VHF/UHF.** I checked these against the IARU Region 1 VHF plan (Novi Sad, December 2020) and its UHF plan. Every
  FT8/FT4 dial sits in an MGM segment: 50.300–50.400, 70.100–70.250, 144.150–144.400 and 432.100–432.400.
- **4 m WSPR.** 70.091 is the plan's own "personal WSPR beacons" frequency.
- **2 m WSPR.** The dial is 144.4905, which puts the RF at 144.492. That sits in the plan's 144.491–144.493 segment for
  personal weak-signal MGM beacons, and matches the RSGB's 144.492.
- **WSPR on 6 m and 70 cm is left out.** The usual dials sit outside Region 1's MGM/beacon segments; a region-aware table
  is a later job.
- **60 m WSPR** uses 5.3647, which is inside the WRC-15 allocation. The older 5.2872 would stretch the 60 m passband to
  73 kHz.

**Span.** A band's span is worked out from the registered modes only. It is the cluster around the FT8 dial, at most
48 kHz wide, plus a 250 Hz guard on each side. Dials further away are listed in `Span::outside`:
- 15 m: FT4 at 21.140 is 66 kHz from FT8.
- 10 m: FT4 at 28.180.
- 2 m: WSPR at 144.4905.

A wider span would mean a 100–300 kHz slice that is mostly empty.

Passbands per band with FT8, FT4, FT2, JS8 and WSPR registered (from `test-digi-spots`):

| band | span (MHz) | width | modes |
|---|---|---|---|
| 2200 m | 0.1371–0.1379 | 0.7 kHz | WSPR |
| 630 m | 0.4753–0.4760 | 0.7 kHz | WSPR |
| 160 m | 1.8377–1.8469 | 9.1 kHz | FT8 WSPR JS8 FT2 |
| 80 m | 3.5697–3.5819 | 12.1 kHz | FT8 FT4 WSPR JS8 FT2 |
| 60 m | 5.3568–5.3666 | 9.7 kHz | FT8 WSPR |
| 40 m | 7.0397–7.0819 | 42.1 kHz | FT8 FT4 WSPR JS8 FT2 |
| 30 m | 10.1298–10.1478 | 17.9 kHz | FT8 FT4 WSPR JS8 FT2 |
| 20 m | 14.0739–14.0976 | 23.7 kHz | FT8 FT4 WSPR JS8 FT2 |
| 17 m | 18.0998–18.1118 | 11.9 kHz | FT8 FT4 WSPR JS8 FT2 |
| 15 m | 21.0739–21.0966 | 22.7 kHz | FT8 WSPR JS8 (FT4, FT2 outside) |
| 12 m | 24.9149–24.9265 | 11.7 kHz | FT8 FT4 WSPR JS8 |
| 10 m | 28.0739–28.0817 | 7.9 kHz | FT8 JS8 (FT4, WSPR, FT2 outside) |
| 6 m | 50.3128–50.3218 | 8.9 kHz | FT8 FT4 JS8 |
| 4 m | 70.1539–70.1577 | 3.9 kHz | FT8 (WSPR outside) |
| 2 m | 144.1698–144.1778 | 7.9 kHz | FT8 FT4 (WSPR outside) |
| 70 cm | 432.1739–432.1778 | 3.9 kHz | FT8 |

With only FT8 and FT4 registered (today), 20 m is 14.0739–14.0838 (9.9 kHz, centre 14.0788).

### The tune controls in Band
Every tune control in the web client goes through `nudge()`: arrows, wheel, keys, the D-pad, a game controller and
media keys. In Band mode `nudge()` calls `bandSpotsStep`. That steps with `stepSpotBand` in `src/utils/digiModes.ts`,
which is the same rule as the server's `stepSpan`; both tests run the same cases. Stepping is clamped and never wraps.
- **Readout.** The frequency card's channel tag shows "20 m BAND".
- **Drawn VFO.** The waterfall's sidebands show the band's span. It is drawn only, never sent, as with the CW skimmer.
- **Typed frequency.** A frequency typed in the box tunes exactly. The span stays where the modes are, so it may be
  drawn off-centre.

### Server contract
- **vibeserver.json:**
  - `spotModes`: the registered modes, in order. Clients take their mode filter and legend from it.
  - `spotsBand`: true, or false with `spotsBandWhy`.
  - `spotBands`: `[{id, name, hz (the dial: span centre), lo, hi (span), bandLo, bandHi, modes}]`, only for bands this
    radio tunes in full.

  If no band is reachable, the Band fields are absent and the client draws no Band button (AGENTS.md). That covers an
  Icom, which gives audio and not IQ, and an RTL without direct sampling below 24 MHz.
- **Subscribe:** `{"type":"subscribe_digital_spots","scope":"band"}`. Without `scope` the server runs Single, so older
  clients get what they always got. In Band the server refuses, in words (`decoder_refused`, `what:"spots"`), in two
  cases: the owner blocked `spotsband`, or no band is reachable.
- **Spot frame:** `{"type":"digital_spot","data":{"mode":"<any registered>",...}}`. `band` now covers 2200 m to 70 cm.
- **Admin:** each running host reports `spotsScope` (`single` / `band`) and `spotsRun` ("FT8 14.0740, FT4 14.0800"). The
  session's decoder shows as `ft8band`, which the admin page draws as "BAND SPOTS".
- **Health pill:** `BehindWatch` notes each host's `spotSlotsSkipped()` as "Digital spots" or "Digital spots: Band". This
  is separate from the decode queue's dropped audio and IQ, which it still watches as well.

### The default and the owner's switch
The owner's switch is **`spotsband`** in blocked modes. It appears in Setup → Modes and decoders as "Digital spots: Band",
and in the app's server screen.

The benchmark adds a `dec_ft8band` row: the busiest band in the registry, 15 stations on each dial, one pass, measured
through the real tuners and engines. On a fresh setup, `benchApplyDefaults` and the app's `applyBench` (one rule, two
readers) switch `spotsband` off when that row is not green, and write the reason beside the switch. The owner can always
tick it back on. Listeners then see the Band button greyed with the reason; it is never hidden.

The Band row is **not** used as the "heaviest decoder" when the benchmark sets the decoder limit. Band is one slot, and
where it is too heavy it is already off.

### Gallery
The summary cards count each mode separately (`Ft8Tally::byMode`). Card titles name the modes heard, most first:
"FT8+FT4 18:05–19:00 UTC · …", "WSPR+FT8 last 10 min · …". A Band run's card begins "Digital spots summary · Band 20 m".

### Map: coloured by mode
The style table is `src/utils/digiModes.ts`. It uses Okabe–Ito fills with a dark rim (`#1A1A1A`). Each mode has its own
letter, and the letter's ink is chosen for at least 4.5:1 contrast on its fill.

| mode | fill | letter |
|---|---|---|
| FT8 | #E69F00 orange | 8 |
| FT4 | #56B4E9 sky blue | 4 |
| FT2 | #F0E442 yellow | 2 |
| JS8 | #009E73 bluish green | J |
| WSPR | #CC79A7 reddish purple | W |
| Q65 | #0072B2 blue | Q |
| FST4 / FST4W | #D55E00 vermillion | F / w |
| MSK144 | #BBBBBB grey | M |

Where the letters are drawn:
- **GPU map:** `vibemapgl-compat.js`. A circleMarker styled `{label, labelColor}` puts its letter on one symbol layer,
  `lc-circle-labels`. The asset copies in `assets/mapgl` and `directory/public/mapgl` were refreshed. They are byte copies,
  and `check-map-overlay` passes.
- **Leaflet fallback (no WebGL 2):** the newest 200 markers get letter markers. The cap is there because each one is a DOM
  node on the radio's main thread.
- **Legend and BY MODE bars:** listed by mode. Bands stay in the stats.

`test_digi_modes.ts` checks:
- Every letter is unique.
- Each letter's ink contrast is at least 4.5:1.
- Each fill stands off both map themes.
- Under simulated protan, deutan and tritan vision, FT8/FT4/FT2/JS8/WSPR stay at least ΔE 10 apart on colour alone.

## THE MODE INTERFACE: how WSPR, JS8, FT2, Q65, FST4, FST4W and MSK144 plug in

A mode is a header `decoders/digimodes/<name>.h` that defines `inline vibe::digi::ModeDef vibeDigiMode_<name>()`. The
engine's `.cpp` files go beside it in `decoders/digimodes/`. All three builds glob that folder: `vibeserver/CMakeLists.txt`,
the Android `CMakeLists.txt` and `modules/vibe-local-sdr/build_ios.sh`.

The registry (`digi_modes.cpp`) already `__has_include`s `wspr.h`, `js8.h`, `ft2.h`, `q65.h`, `fst4.h`, `fst4w.h` and
`msk144.h`, so **for those seven modes the header is the whole change**. Any other mode adds one `ADD(...)` line there.
The function name must match the file: `wspr.h` → `vibeDigiMode_wspr()`.

```cpp
// decoders/digi_mode.h (abridged)
namespace vibe::digi {
struct Spot {
    std::string mode;     // "WSPR" (or a submode "Q65-60A"); empty = the ModeDef's name
    std::string call;     // the SENDER as decoded — never a hash placeholder "<...>"
    std::string grid;     // its OWN 4/6-char locator, or "" (never a report "-12" / "RR73")
    std::string text;     // the decoded message (optional)
    int   snr = 0;        // dB in 2500 Hz (WSJT-X's convention)
    float audioHz = 0;    // audio frequency above the DIAL (USB): RF = dial + audioHz
    float dt = 0;         // s from the slot's nominal start (0 if free-running)
};
class Engine {
public:
    virtual ~Engine() = default;
    // n samples of REAL USB audio at ModeDef::sampleRate, ±1.0 full scale, the dial at 0 Hz.
    // captureUtc = corrected UTC (vibeUtcNow) of the LAST sample: cut slots by it, never by the decode thread's clock.
    // Called on the host's decode thread, in order. Return quickly: decode a slot on your OWN worker thread; if the
    // previous slot is still decoding when the next is complete, SKIP it and count it (never wait).
    virtual void process(const float* audio, int n, double captureUtc) = 0;
    virtual uint64_t slotsSkipped() const { return 0; }   // the health pill's "TOO HEAVY"
    virtual uint64_t slotsDecoded() const { return 0; }
    std::function<void(const Spot&)> onSpot;   // set by the host; may be called from your thread
    std::function<bool()> loaded;              // true = server loaded: skip OPTIONAL work (extra passes, OSD)
};
struct ModeDef {
    std::string name;            // "WSPR" — the wire, the list's Mode column, the map legend, the gallery card
    double slotSec = 15.0;       // T/R period; 0 = free-running (MSK144)
    int    sampleRate = 12000;   // the audio rate process() gets (Band's tuners make 12 kHz today)
    float  audioLoHz = 200, audioHiHz = 3000;   // the audio passband you search, above the dial — Band cuts exactly this
    bool   single = true;        // also run in Single mode on the listener's audio (false = Band only)
    double costVsFt8 = 1.0;      // relative CPU of one engine (docs, benchmark)
    std::vector<std::pair<std::string, double>> dials;   // {"20m", 14095600} … EMPTY = digi_bands.h's published table
    std::function<std::unique_ptr<Engine>()> make;
};
}
```

A worked example, for a WSPR engine (`decoders/digimodes/wspr.h`):

```cpp
#pragma once
#include "../digi_mode.h"
#include "wspr_engine.h"            // the WSPR decoder, built by its own author (decoders/digimodes/wspr_engine.cpp)
namespace vibe { namespace digi {
class WsprEngine : public Engine {
public:
    WsprEngine() { e_.onDecode = [this](const WsprDecode& d) {
        if (!onSpot) return;
        Spot s; s.mode = "WSPR"; s.call = d.call; s.grid = d.grid; s.snr = d.snr; s.audioHz = d.audioHz; s.dt = d.dt;
        onSpot(s); }; }
    void process(const float* a, int n, double utc) override { e_.feed(a, n, utc); }   // slots on its own thread
    uint64_t slotsSkipped() const override { return e_.skipped(); }
    uint64_t slotsDecoded() const override { return e_.decoded(); }
private:
    WsprDecoder e_;
};
inline ModeDef vibeDigiMode_wspr() {
    ModeDef m; m.name = "WSPR"; m.slotSec = 120; m.sampleRate = 12000;
    m.audioLoHz = 1400; m.audioHiHz = 1600;     // WSPR's 200 Hz sub-band, 1500 Hz above the dial
    m.costVsFt8 = 1.0;
    m.make = [] { return std::unique_ptr<Engine>(new WsprEngine()); };
    return m;   // dials: from digi_bands.h ("WSPR" rows), so none listed here
}
}}
```

What a mode gets without doing anything more:
- It runs in **Single mode** if `single` is true.
- In **Band mode** it runs on every band where one of its dials fits the cluster.
- `vibeserver.json` lists it in `spotModes`, and band spans grow to include its dials.
- Its spots reach the list, the map and the gallery card with their mode name.
- The health pill reads its skipped slots.
- The map draws it with its own colour and letter. Add its row to `DIGI_MODE_STYLE` in `src/utils/digiModes.ts` if it is
  not one of the nine already there; otherwise it draws grey with its first letter.

What a mode must do:
- **UTC slots.** Cut slots by `captureUtc`.
- **Threading.** Never block `process()`, and skip a slot rather than wait for one.
- **Untrusted output.** Report only decoded calls and their own grids. The host whitelists what it sends again, but a
  hashed call or a report is not a callsign or a locator.
- **Efficiency.** Hot loops need NEON (ARMv7 and aarch64), SSE2 as the x86 floor, and a scalar twin (the decoder rules
  file).
- **Bench row.** Add a benchmark row if the mode is heavier than FT8. The Band row already times whatever is registered,
  but only FT8 and FT4 have synthetic stations in it (`busyBandSlice`). A new engine's author should add its signal there.

## Measured

`vibeserver/test-digi-spots.cpp` (registered; `test-digi-spots cpu` adds the CPU figures).

**Tuner.** A 75 kHz slice in gives 12 kHz audio out (4 s → 48 001 samples). A tone 1000 Hz below the dial (the lower
sideband) is rejected by 90 dB. A tone 5 kHz above the dial is rejected by more than 50 dB.

**A synthetic 20 m band, end to end.** The slice is 75 kHz, which is what a 2.4 MS/s radio cuts for 20 m. It carries four
FT8 stations across two slots and three FT4 stations across three slots on their own dials, with noise. Result: **7/7
spotted**, each with the right mode and an RF within 2 Hz. A station 1200 Hz **below** the FT8 dial was **not** decoded.

**Pure noise.** 31 s on 20 m, 6 slots decoded: **0 spots**.

**Through the real server** (`scripts/test-server-spots.mjs`, 28/28). A fake rtl_tcp replays a UTC-anchored 20 m clip
(`fake-rtl-tcp --hf-utc`). Band spotted all five stations, each with its mode and RF, on both a one-pipeline radio
(`vibe_wide_tap.h`) and a range-locked one (per-listener FFT slice). The lower-sideband station was never spotted and
there were no false spots. Single on the same radio still works. The owner's switch greys Band with its reason and
refuses a subscribe in words.

**Web client** (`scripts/test-web-spots.mjs`, headless Edge with `--mute-audio`, 26/26):
- the two buttons, Band's title and readout;
- the Mode column in each mode's colour and letter;
- ▲ to 17 m's centre and ▼ back;
- map markers and legend by mode, with letters on the Leaflet fallback (headless Edge has no WebGL 2 here);
- nothing scrolls sideways at 390 px;
- back to Single, where the arrows tune by the step again.

**CPU, Apple M5 Pro, measured** (process CPU, every thread). The input is a busy 20 m: 20 FT8 stations in two slots and
10 FT4.

| run | % of one core | spots |
|---|---|---|
| Single (FT8+FT4 engines on the 14.074 passband) | 0.094 % | 20 |
| Band 20 m (FT8 on 14.074, FT4 on 14.080, from a 75 kHz slice) | 0.172 % | 30 |
| of which the two tuners | 0.057 % (0.029 % per dial) | |
| benchmark `dec_ft8band` (80 m, 15 stations on each dial) | 0.117 % | 25 of 30 |

**RAM.** Each tuner holds about 40 KB of polyphase table at 75 kHz (256 phases × 40 taps), or about 100 KB at 192 kHz, and
a few KB of history. Each engine is one Ft8Decoder: about 1.5 MB of scratch, the same as Single.

**Pi 2 (Cortex-A7, 900 MHz): an estimate, not measured on the hardware.** Using the CW skimmer's 20–30× factor from M5 to
A7 for float work:
- Single ≈ 2–3 % of one A7 core.
- Band on 20 m with FT8+FT4 ≈ 3.5–5 %.
- Each tuner ≈ 0.6–0.9 %.

With WSPR, JS8 and FT2 registered, a 40 m Band run would have five engines and five tuners, roughly 10–15 % of a core
spread over five worker threads. The `dec_ft8band` row measures this on the machine itself and sets the default.

## Not done / owed
- **The app's own Band mode.** The app (React Native) has the map coloured by mode and the Mode column, but no Band
  button and no band stepping yet. On a VibeServer it still subscribes to Single spots, as before. The web client is first,
  as the brief says.
- **Long press.** "Long press = exact frequency": the frequency box tunes exactly in Band. Holding an arrow repeats the
  band step (holdSweep). There is no separate long-press gesture.
- **Map themes.** Light and dark are checked by contrast arithmetic. The spots window always renders the GPU map at night
  (`setNight(true)`).
- **Real off-air test.** Band mode was not run against a real band recording. The decode engines are the same ones Single
  uses, and a real recording would test the tuner on a busy band with strong neighbours.
- **WSPR on 6 m and 70 cm**, and region-aware dials in general.
- **FT2's dials** are taken from observation. They need confirming against a published plan.
