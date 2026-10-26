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
This is the button there always was, and the **cheap** one. The listener's 48 kHz audio is box-averaged by four to
12 kHz, the same decimation as before. It goes only into the registered modes (`single: true`) whose published dial is
where the listener is tuned; Band is the button that runs every mode. Spot RF is the listener's dial plus the audio
offset.

**How the modes are chosen** (`Spotter::singleModesAt`, one rule in `Spotter::atOwnDial`, 2026-10-26):
- A narrow mode on a dial of its own (`ModeDef::singleDialTolHz` > 0, WSPR's 150 Hz) runs when the listener's dial is
  within that of one of its dials.
- Any other mode: the listener's window is the USB passband dial + 100 … dial + 3000 Hz. The mode runs when at least
  half of that window lies inside its own audio window above one of its dials (dial + `audioLoHz` … `audioHiHz`).
- On 20 m that gives: 14.074 → FT8, 14.078 → JS8, 14.080 → FT4, 14.084 → FT2, 14.0956 → WSPR. A shared dial runs both
  (3.578 → FT2 and JS8). A few hundred hertz off a dial still counts (7.0743 → FT8).
- **Off the table** (no dial there, or no dial known) it runs the modes marked `ModeDef::singleFallback`: **FT8 +
  FT4**, which is what Single always ran, so it never goes silent.
- **On a retune** the set is chosen again on the next audio block. Engines whose mode is still wanted are kept, so a
  fine retune does not lose a slot in progress. The others are stopped (their slot counts are kept for the health
  pill) and new ones made.
- **Cost:** one engine per listener at a mode's dial, not five. Running FT8, FT4, FT2, JS8 and WSPR on every
  listener's audio would cost about 9× FT8 per listener.

`test-digi-spots` checks the set chosen per dial, the retune swaps, the kept engine and the off-table fallback.

### Spot times
Every spot is stamped with the **start of the slot** it was heard in (`digi::Spot::slotUtc` → the frame's `timestamp`):
WSJT-X's time column, as UberSDR stamps its spots too. Clients show it as **HH:MM:SS UTC** (`src/utils/spotTime.ts`,
one helper for the web list, the CW/PSK lists, the web map's popups and the app's list, CW view and map card), so the
seconds name the transmission block: FT8 :00/:15/:30/:45, FT4 every 7.5 s, FT2 every 3.75 s, JS8 by its submode,
WSPR on the even minute. M7EIW's request via Stuart, 2026-10-26. Checked by `test_spot_time.ts`, `test-digi-spots`
(each Band spot's slot), `test-server-spots` and `test-web-spots`.

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

**Status, 2026-10-27.** WSPR is wired in: `decoders/digimodes/wspr.h` wraps our own engine
(`decoders/digimodes/wspr_decoder.*`, branch `v12-wspr`) — 120 s slots, 12 kHz, audio 1350–1650 Hz (it searches
1380–1620 and measures its WSPRnet-scale SNR across ±150 Hz of 1500, so Band's tuner is asked for all of that flat),
`costVsFt8` 5 (busy 40 m: ≈ 480 ms per two minutes on a Mac against FT8's ≈ 84 ms). In Single mode it is fed only while
the listener's dial is within 150 Hz of a WSPR dial (`ModeDef::singleDialTolHz` — now the narrow case of Single's one
rule, `Spotter::atOwnDial`, above): on the FT8 dial its 1400–1600 Hz is FT8 traffic. Spots carry `snr` on the WSPRnet scale. With it registered, 20 m's Band centre is 14.0857 MHz and
2200 m / 630 m appear as WSPR-only bands. JS8 and FT2 are wrapped the same way (`digimodes/js8.h`, `ft2.h`;
docs/v12/JS8-FT2.md).

To check an integration, these must all hold:
1. `ModeDef::name` is exactly `"WSPR"` / `"JS8"` / `"FT2"`. The published dials in `digi_bands.h` and the client
   styles in `digiModes.ts` are keyed on that name.
2. `audioLoHz` / `audioHiHz` cover what the engine searches (WSPR 1400–1600). Band mode cuts exactly this, plus a guard.
3. `process()` takes **float** audio at `sampleRate`, which is 12 kHz in Band mode today. It is stamped with the capture
   UTC of the last sample, and slots are cut by that stamp.
4. `onSpot` carries the call and the sender's own grid, with `audioHz` above the dial.
5. `slotsSkipped()` counts the slots dropped while the previous one was still decoding.
6. Run `test-digi-spots` and `test-server-spots`. With a new mode registered, spans grow by themselves:
   20 m becomes 23.7 kHz and 40 m 42.1 kHz (the table above).

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
    double singleDialTolHz = 0;  // Single runs it only AT its dial: > 0 = within this many Hz of one (WSPR: 150); 0 = half the
                                 //   listener's 100–3000 Hz window inside this mode's window above a dial (FT8, JS8 …)
    bool   singleFallback = false;  // Single, tuned to no mode's dial: run anyway (FT8 and FT4 — what Single always ran)
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

**Real off-air 20 m** (Stuart's UberSDR, `~/VibeSDR-test-audio/spots/`, 2026-10-26; `test-digi-spots real … live`, fed in
real time so the FT8 pass gate behaves as it would live):
- **A/B: Band's tuner against a real USB receiver.** Two recordings made at the same moment, 150 s each:
  `ab_20m_14078800_iq48.wav` (48 kHz I/Q) went through **Band**, and `ab_ft8_20m_14074000_usb.wav` (the receiver's own
  USB demodulator at 14.074, 12 kHz) went through **Single**. Both runs use the same engines. FT8 found **75 unique calls
  either way**, 72 of them in common; the three found by only one path were weak ones each way. The tuner costs no
  sensitivity. Band also decoded **14 FT4 stations** on 14.080, which Single cannot hear from the FT8 dial.
- **200 s at 08:20 UTC** (`uber_20m_14078800_iq48.wav`): 269 FT8 and 65 FT4 spots, 83 + 11 unique calls, 37 slots
  decoded, 0 skipped, 0.34 % of an M5 core. UberSDR's own decoder (jt9-class, with AP decoding) logged 39 calls with
  grids on 20 m FT8 in the same window. Band decoded 22 of them; the 17 missed were mostly −13 to −25 dB. That gap is
  the FT8 engine's sensitivity, not Band's: the A/B above shows Band equals Single.

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


## JTTY, PSK and the Band governor (2026-10-27, branch `v12-band-all`)

Stuart, 2026-10-27: *"we want to make the band digital spots mode pull everything it can down, but we also have to be
mindful of CPU running that many decoders at once."*

### Two more modes in the registry
| mode | file | dials (USB) | spots |
|---|---|---|---|
| JTTY | `decoders/digimodes/jtty.h` (our `jtty::Decoder`, slot-less) | 7.090 · 14.090 · 21.090 (PSKReporter: all 128 JTTY reports in the day to 2026-10-26 17:35 were on these, 88 on 20 m) · 28.090 · 50.160 · 144.160 (announced, clear of other modes) | mode `JTTY`, `how` CQ / QSO, the caller's grid when a GRID4 frame follows its CQ in the same message |
| PSK | `decoders/digimodes/psk.h` (our `psk::Skimmer`, spots only) | 3.580 · 7.040 · 10.142 · 14.070 (PSKReporter: every PSK31 report 14.0700–14.0718) · 18.100 · 21.070 · 24.920 · 28.120 | mode `PSK31` / `PSK63`, `how` CQ / DE / TEST, SNR converted from Es/N0 to 2500 Hz |

JTTY's announced 1.838, 3.575, 10.140, 18.100 and 24.920 are LEFT OUT: each sits in an FT8 or FT4 window (3.575 and 10.140
are FT4's dials, 18.100 FT8's), nobody was heard there, and the costliest engine would be held open by FT8 energy all
day. 21.070 not 21.080 for PSK (the PSK31 segment's own). Only 14.070 (PSK) and 7/14/21 MHz (JTTY) are confirmed on air;
the rest are the published lists. Every spot frame now also carries `country` (our prefix table) and `how`.

Map: JTTY white with a black **T**, PSK Paul Tol's purple `#AA3377` with a white **P** — the eight Okabe–Ito fills were
taken; both stay ≥ ΔE 10 from every mode heard on the same band under protan/deutan/tritan simulation
(`test_digi_modes.ts`, the "core" set is now FT8 FT4 FT2 JS8 WSPR JTTY PSK). PSK31/PSK63 are drawn and filtered as their
family PSK (`digiModeFamily`). PSK spots carry no locator (PSK has none), so they are in the list, not on the map; a JTTY
caller lands on the map when it sends its grid.

### Spans (spans now grow in PRIORITY order, so a new mode never pushes an established one out)
| band | span | width | in | outside |
|---|---|---|---|---|
| 80 m | 3.5697–3.5839 | 14.2 kHz | FT8 FT4 FT2 JS8 WSPR PSK | |
| 40 m | 7.0396–7.0816 | 41.9 kHz | FT8 FT4 FT2 JS8 WSPR PSK | JTTY (7.090 would make 53 kHz) |
| 30 m | 10.1298–10.1478 | 17.9 kHz | FT8 FT4 FT2 JS8 WSPR PSK | |
| 20 m | 14.0699–14.0975 | 27.6 kHz (centre 14.0837) | all seven | |
| 17 m | 18.0998–18.1118 | 11.9 kHz | FT8 FT4 FT2 JS8 WSPR PSK | |
| 15 m | 21.0699–21.0965 | 26.6 kHz | FT8 JS8 WSPR JTTY PSK | FT4 FT2 |
| 12 m | 24.9149–24.9267 | 11.9 kHz | FT8 FT4 FT2 JS8 WSPR PSK | |
| 10 m | 28.0739–28.0938 | 19.9 kHz | FT8 JS8 JTTY | FT4 FT2 WSPR PSK |
| 6 m | 50.3128–50.3218 | 8.9 kHz | FT8 FT4 JS8 | JTTY |
| 2 m | 144.1599–144.1778 | 17.9 kHz | FT8 FT4 JTTY | WSPR |
(160 m, 60 m, 4 m, 70 cm, 2200 m, 630 m unchanged.) "Outside" is outside the span; a mode outside it still decodes when
the radio's slice happens to reach it (a 2.4 MS/s radio cuts 75 kHz for 40 m, which holds 7.090).

### The governor (`decoders/digi_activity.h`, `digi_governor.h`, `digi_spotter.cpp`)
1. **Activity gate — free.** Each IQ block of the slice arrives with its bins' power, read off the forward FFT it was cut
   from (`slicePower`: Hann in frequency, one multiply-add a bin; the shim's channeliser path and `vibe_wide_tap.h` both
   send it). The slice's floor is the median of half-second bin means. Per gate period (the mode's slot; JS8 30 s;
   JTTY/PSK 4 s) each window's strongest bin mean is tested against the floor with a Wilson–Hilferty threshold for an
   N-block mean, z = 5, looked at every 8th block. Undecided, the period's audio is HELD (int16 ×4096); the moment the
   window lights it is flushed to the engine and the rest goes live; a period that never lights is DROPPED — its decode
   never happens. A busy window starts its next period live (free-running: the next three).
2. **Depth before mode.** Before a window goes live the governor is asked. Every decoding mode runs LIGHT first (engine
   told `loaded`: one pass, no OSD/AP/coherent stage — `ModeDef::lightCostVsFt8`), then full depth is given back in
   priority order, greedily, while it fits.
3. **Shedding.** When even light does not fit, the lowest priority is shed — greedily, so a cheap mode below a shed
   expensive one still runs (PSK 0.6 after JTTY 12). A free-running mode is stopped at once when a higher one needs the
   room; a slotted one finishes the slot it has and is shed at its next. A dial whose engines are all off/shed skips its
   tuner too.
4. **Priority** FT8 10 · FT4 20 · WSPR 30 · JS8 40 · FT2 50 · JTTY 60 · PSK 70 · anything new 100 (Q65/FST4/FST4W/MSK144
   take 100 until measured). FT8 and FT4 are most of the traffic (PSKReporter's active receivers 2026-10-26: FT8 6666,
   FT4 597, JS8 198, JTTY 180); WSPR is costly but is the propagation reference and idle when empty; JTTY is the costliest
   engine (≈ 12 FT8s on the Mac, ≈ 20 on the Pi 2 build) and heard on few dials; PSK is spots only.
5. **Budget** in FT8 units: the benchmark's Band row (`dec_ft8band`, now 20 m with all seven modes, one pass) →
   `70 % ÷ (row % ÷ its light units)`; then live — a 15 s check in which any engine skipped a slot cuts it to 75 % of
   what was decoding, calm checks give 10 % back up to the benchmark's figure; no benchmark = unlimited until a skip.
   The server's `loaded` makes everything light.
6. **The owner** (Setup → Modes and decoders, under "Digital spots: Band"): per mode **Auto / Always / Off**, stored in
   the radio's blocked-modes list as `spotsband+<mode>` (Always: no gate, never shed — it takes its turn in priority
   order, so forcing JTTY on never starves FT8) and `spotsband-<mode>` (Off). The benchmark only sets the budget.
7. **Visible.** `{"type":"digital_spots_status","data":{"scope":"band","modes":[{mode,state,dials,idle,shed,live}],"budget"}}`
   when a state changes (≤ 1/s) and every 15 s → the spots box header's chips (ACTIVE · LIGHT · IDLE · SHED · OFF, each
   in its map colour and letter). Shed periods go to the health pill as "Digital spots: Band (modes shed)" (BehindWatch);
   idle periods are the gate working and are not a fault. The admin line reads "FT8 14.0740 active, WSPR 14.0956 idle …".

**Blind spot, stated:** a station too weak to lift a ~150 Hz bin over a whole period ALONE in an otherwise empty window
is not decoded. Measured (`test-digi-spots`, one FT8 station alone, 20 trials): −14 and −17 dB open 20/20, −19 dB 16/20,
−21 dB 2/20. Pure noise: 0 of 1800 windows opened in 10 minutes. On a live band FT8/FT4 windows are never empty; for a
dead band where the last −22 dB station matters the owner sets Always.

**RAM:** held audio per engine at most one gate period + 1 s lead-in, int16: FT8 ≈ 0.4 MB, FT4 0.2, FT2 0.1, JS8 0.75,
WSPR 2.9, JTTY/PSK 0.15 each — ≈ 4.6 MB for a band with all seven, only while windows are undecided.

### Measured
Real recordings off Stuart's UberSDR (`~/VibeSDR-test-audio/bandall/`, 96 kHz IQ, 5 min each, 2026-10-26 17:38 and
18:15 UTC), through every registered mode, fed in real time (`test-digi-spots real … live [gate]`); Mac = M5 Pro, process
CPU; Pi 2 = the armhf build (-O3 -marm -mfpu=neon-vfpv4) under qemu-arm with the instruction counter
(`tools/band-bench/qemu.sh`, 230 s, every engine light, every slot decoded), test harness excluded.

| recording | Mac, no gate | Mac, gated | spots (both) | Pi 2 instr/s, no gate → gated | Pi 2 % of a core (est.) |
|---|---|---|---|---|---|
| 20 m busy (17:38) | 1.58 % | 1.58 % | FT8 199 (52 calls), FT4 82, FT2 3, WSPR 3 — identical | 309 M → 302 M | ≈ 42–56 % |
| 40 m busy (17:38) | 3.75 % | 3.90 % | FT8 681 (161), FT4 165, JS8 13, WSPR 8 — identical | — | — |
| 10 m dead (18:15) | 2.58 % | 1.37 % | none | 244 M → 105 M | 34–45 % → 15–19 % |

Busy bands: every window really is occupied (on 20 m and 40 m at this hour even JTTY's and PSK's windows hold other
signals), so the gate saves ~nothing and loses nothing. Dead band: half the CPU on the Mac, 57 % of the instructions on
the Pi 2 build — JTTY alone 32.7 G → 2.0 G. What keeps the dead 10 m's JS8 and PSK windows open is steady carriers in
them (a gate refinement owed: ignore a bin that is steady for many periods).

**Where a Pi 2's Band CPU goes (20 m, 230 s, 71 G instructions):** JTTY 51 % (list Viterbi 27 %, its candidate decode
21 %), the seven tuners 25 %, FT8+FT4+FT2 ≈ 15 %, JS8 5 %, WSPR 1 %, PSK < 1 %, the gate < 1 %. Instructions → % of a
core assumes 0.6–0.8 instructions per cycle at 900 MHz (ESTIMATED; an A7 dual-issues simple ops but takes two cycles per
128-bit NEON op): **a Pi 2 can run all seven modes on 20 m at light depth (≈ 42–56 % of one of its four cores — inside
the 70 % green budget)**, with some full depth given back where the benchmark's budget allows; on a quiet band ≈ 15–20 %.
The next saving is the tuners (one per dial; a shared channeliser for neighbouring dials) and JTTY's list Viterbi.

**The channeliser's taper (found here):** `Channelizer::extract` rolls off the outer quarter of the slice on each side
(raised cosine), while the shim sizes the slice assuming only 4 % roll-off (`w / 0.92`). At 2.4 MS/s the 20 m slice is
75 kHz (flat ≈ 37 kHz, span 27.6 — fine), but 40 m's 41.9 kHz span has its ends (WSPR, JS8) in the taper, and on a
960 kS/s radio the 20 m slice is only 30 kHz. Signal and noise are attenuated alike, but the PSK skimmer's floor is
band-relative, so a PSK signal deep in the taper can be missed (the server test's PSK station was moved 1.2 kHz inward).
Owed: size Band's slice for the flat half.

### Tests
`test-digi-spots` (139 checks): JTTY/PSK registry and dials, no JTTY dial in another mode's window, priority order,
Single at 14.090/14.070, spans; the governor (no budget, loaded, 20/8/3 units, idle, Always/Off, no priority inversion);
the live budget; the gate (an empty window is never fed, a lit one is fed whole from its slot start, no bins = no gate);
noise and threshold; the owner's word and shedding + depth through the Spotter; JTTY and PSK31 end to end through Band
with real engines. `test-server-spots` (46): JTTY and PSK in the clip, both radio layouts, the chips, how + country, the
owner's `spotsband-psk,spotsband+wspr`. `test-web-spots` (31): six modes in the list, chips in colour and letter, WSPR
idle, legend with T and P, 390 px, chips gone in Single. `test_digi_modes.ts`: styles, CVD distances, families.

## Not done / owed
- **Band governor (2026-10-27):** the app (React Native) now has the Band key and the chips (2026-10-26 parity pass) but no per-mode owner setting in
  its server screen (the setup page has it); a steady carrier holds a window open; Band's slice is not sized for the
  channeliser's flat half (above); the costVsFt8 figures are the Mac's (JTTY is relatively heavier on the Pi 2 build);
  the benchmark's Band row now includes JTTY (0.77 % on the Mac vs 0.12 % before) — a Pi 2 measuring it amber would
  start with Band off by the existing rule, although the governor keeps it in budget (rule left as it was; owner can tick).
- **The app's own Band mode** — BUILT 2026-10-26 (APP-PARITY-2026-10-26.md): SPOTS: BAND beside SPOTS: SINGLE in the
  Mode sheet (absent / dimmed with why), band stepping on every tune gesture, the band's span drawn, the title naming the
  band, the governor's chips, the MODE filter and the map legend from `spotModes` (`src/services/spotsBand.ts`,
  scripts/test_spots_band_app.ts). Still owed in the app: the frequency readout naming the band (the web's cardFreqText),
  and the owner's per-mode Band setting in the app's own server screen.
- **Long press.** "Long press = exact frequency": the frequency box tunes exactly in Band. Holding an arrow repeats the
  band step (holdSweep). There is no separate long-press gesture.
- **Map themes.** Light and dark are checked by contrast arithmetic. The spots window always renders the GPU map at night
  (`setNight(true)`).
- **Real off-air test.** Band mode was not run against a real band recording. The decode engines are the same ones Single
  uses, and a real recording would test the tuner on a busy band with strong neighbours.
- **WSPR on 6 m and 70 cm**, and region-aware dials in general.
- **FT2's dials** are taken from observation. They need confirming against a published plan.
