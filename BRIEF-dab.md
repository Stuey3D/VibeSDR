# DAB on VibeServer — experimental

Agreed with Stuart 2026-08-21, scoped 2026-09-03/04. **Pi/Linux only and WEB CLIENT ONLY while
experimental.** Nothing ships to the app stores from this until it earns it.

## The audio route, and why it is split

Stuart: *"we bitstream out the audio to allow the OS inbuilt decoders to handle it… if the OS
doesnt have the codec then silence."* That is right for DAB+ and WRONG for the BBC, and the
difference is measured, not assumed.

**Measured 2026-09-04, Chrome 152 on Android 10 (a real browser, not a spec):**

| stream | MediaSource | decodes a real file? |
|---|---|---|
| DAB+ AAC-LC `mp4a.40.2` | YES | — |
| DAB+ HE-AAC `mp4a.40.5` | YES | — |
| DAB+ HE-AACv2 `mp4a.40.29` | YES | — |
| DRM xHE-AAC `mp4a.40.42` | YES | — |
| **DAB MPEG-1 Layer II** | `audio/mpeg` says YES | **NO — `EncodingError: Unable to decode audio data`** |
| Layer II in mp4 `mp4a.40.33` | NO | — |

★★★ `isTypeSupported('audio/mpeg')` ANSWERS ABOUT MP3. It returns YES and then refuses actual
Layer II bytes. A capability probe that asks the wrong question is worse than no probe: it would
have told us the BBC was fine right up until every BBC station was silent.

So:
- **DAB+ → reframe the bitstream (RS(120,110), firecode, LOAS/LATM → ADTS/mp4) and hand it over.**
  VibeServer links no AAC decoder; the browser's does the work. The licence argument survives
  intact, and it is worth stating in those words: we parse, error-correct and REFRAME, we never
  produce PCM.
- **Plain DAB (MP2) → we decode it ourselves.** MP2's patents expired; this is the one codec we are
  free to implement. It is also the BBC national mux (R1–6 Music, 128 kbps stereo / 80 kbps mono),
  so it is FIRST, not a fallback.

## Our own demodulator — not a wrapper

Stuart, 2026-09-04: *"we build our own DAB demodulator if possible as I dont want to run into that
half arsed DAB implementation that OWRX uses and we had to fix."*

★ The app's existing DAB code is an OWRX REMOTE CONTROL (OwrxAdapter) — a window onto their
  decoder. Nothing in it is reusable here; do not mistake it for a head start.

★★ **UK DAB uses more than one audio sample rate** (Stuart: *"we need to work around that so we are
   better than OWRX out of the box"*). DAB+ services appear at 48 kHz and 32 kHz, and HE-AAC's SBR
   means the CORE rate is half the output rate; MP2 services are 48 kHz, with 24 kHz permitted.
   The output path must resample per-service rather than assume one rate — and it must switch
   cleanly when you change service inside a mux, which is where a naive implementation clicks or
   plays at the wrong speed.

## Hardware gate
A DAB ensemble is 1.536 MHz wide and wants ~2.048 MSPS. **Gate on the radio's CAPTURE BANDWIDTH and
frequency range, never on the driver name** — so a future wideband radio picks it up for free, and
the rule survives the next radio. Consequences today:
- RTL-SDR V4 (2.4 MSPS, to 1.7 GHz) — **capable**.
- **Airspy HF+ CANNOT do DAB** (~912 kHz max, and no Band III). That is Stuart's own shared radio,
  so the obvious test receiver is the wrong one.
- Same rule as AGENTS.md: never offer a control whose every use is a no-op.

## The UX Stuart specified
- DAB appears in the **demodulator / decoders menu** only on a capable radio.
- Selecting it switches the tuning buttons to **multiplex tuning**: 11A, 11B, 11C, 11D… The
  frequency display reads **`222.064 (11D)`** — the frequency AND the channel name.
- Any ensemble found populates the **station list**, and services are saved to bookmarks exactly as
  learned RDS stations are.
  ★★★ A DAB station is NOT a frequency. Recall needs **(channel + service id)** or a bookmark lands
  on the mux and plays whatever comes first.
- The **decoder box is always open in DAB mode**, split: station list with logos on one side, the
  full multiplex / signal / RDS-equivalent detail on the other.

## Two things that must NOT happen to a mux

### ★★★ ZOOM IS BLOCKED IN DAB MODE
Stuart, 2026-09-04: *"in DAB mode the zoom buttons should be blocked especially on an RTL-SDR
running auto IF narrowing with zoom."*

This is not tidiness. On the RTL-SDR the tuner's IF bandwidth FOLLOWS the zoom (`tunerBwAuto`, the
"IF wide auto" indicator) — zooming in narrows the analogue filter to match the visible span. An
ensemble is 1.536 MHz wide and needs every kHz of it: narrow the IF and the mux is cut, the DQPSK
falls apart and the audio stops. The user zoomed the WATERFALL and lost the RADIO, with nothing on
screen connecting the two.

So in DAB mode:
- the zoom controls are **disabled**, not merely ignored — a control that does nothing when pressed
  is the fault AGENTS.md names;
- the IF filter is **pinned wide** and the auto-narrowing is suspended for the duration, because
  the zoom is not the only thing that can move it;
- the span is the mux. There is nothing to zoom INTO: a DAB spectrum is one 1.536 MHz block, not a
  band to explore.

★ Same reasoning as the existing whole-profile data modes (DAB/ADS-B/ISM in `isWholeProfileMode`),
  where the VFO is already suppressed because the only thing it can do is drag you off the block.

### ★★ VIBEAGC MATTERS MORE HERE, NOT LESS
Stuart: *"the VibeAGC is probably more important than ever for DAB."* Agreed, and it is worth
saying why so nobody "simplifies" it away with the zoom controls: DAB is DQPSK across 1536
carriers, and the constellation only survives a narrow window of drive. Overload from a strong
adjacent mux clips the ADC and the errors are spread across every carrier at once; underdrive
loses the weak ones into the noise. FM degrades gracefully under both; DAB does not — it works,
then it stops.

So VibeAGC stays live in DAB mode, and the gain UI stays available. ★ The relationship to watch is
that the ADC-peak headroom the loop already tracks is exactly the signal a DAB front end needs, so
this should need no new mechanism — only care that the DAB path does not accidentally bypass it.

## Reference implementations — read them, do not wrap them

Stuart, 2026-09-04: *"look at open source DAB decoders but build our own, hopefully better one."*

| project | licence | what it is good for |
|---|---|---|
| [williamyang98/DAB-Radio](https://github.com/williamyang98/DAB-Radio) | **MIT** | The closest match to what we are building: its own OFDM demod + DAB decode for RTL-SDR, split into `src/ofdm` and `src/dab`. The most readable reference for the acquisition chain. Its own TODO admits **no firecode error correction on the DAB+ superframe** and **no TII decoding** — two places we can be better on purpose. |
| [welle.io](https://github.com/AlbrechtL/welle.io) | GPL-2.0+ | The mature one; runs on a Pi. Good for behaviour to compare against, not for structure. |
| [dab-cmdline](https://github.com/openatv/dab-cmdline) (van Katwijk) | GPL-2.0 | Library-shaped, many input devices; the ancestor of much of the ecosystem. |
| [dablin](https://github.com/Opendigitalradio/dablin) + eti tools | GPL | ETI-side playback and service handling. This is the lineage **OWRX** draws on — the one Stuart has already had to work around. |
| [csdr-eti](https://github.com/jketterl/csdr-eti) | — | How OWRX plugs DAB in, i.e. exactly what we are choosing not to be. |

★★ **LICENCE POSITION.** VibeSDR is GPL-3.0, so the GPL-2.0+ projects are compatible to *read* and
even to borrow from — but we are writing our own, so the only thing that crosses is understanding.
DAB-Radio being MIT makes it the safest to study closely. Phil Karn's Viterbi/Reed-Solomon routines
are the common ancestor of all of them and are separately licensed; if we use them, say so.

### Where we intend to be better
1. **Sample rates handled per service, not assumed.** Stuart: *"UK dab stations are broadcast with
   multiple sample rates… so we are better than OWRX out of the box."* 48 kHz and 32 kHz services
   coexist in one mux, and HE-AAC's SBR means the CORE rate is half the output rate. The resampler
   must switch cleanly when you change service INSIDE a mux — the moment a naive implementation
   clicks, plays at the wrong speed, or drops to silence.
2. **DAB+ firecode error correction**, which DAB-Radio explicitly does not do: it is what keeps a
   marginal mux audible rather than stuttering.
3. **DX-grade reporting**: per-service protection level, CIF error rate, ensemble label, ECC/country
   — and **TII** (transmitter identification), which tells a DX-er WHICH transmitter they have.
   Nobody in the list above surfaces this well and it is exactly our audience.
4. **A station list that does not flap.** Services must persist across FIB CRC failures rather than
   appearing and vanishing while the list is being read.

## Reference data captured from the Rohde & Schwarz talk (Stuart, 2026-09-04)

**Transmission modes** are now in `vibe_dab_modes.h`, stored in SAMPLES at 2.048 MHz rather than in
the published microseconds — see the note there. Band III, and everything the UK transmits, is
**Mode I**: 1536 carriers at 1 kHz, 76 symbols per frame, 96 ms.

**Error protection** — needed for the Viterbi puncturing and for the per-service protection level
we want to report to DX-ers:

| FEC code | code rate | mux capacity (kbps) |
|---|---|---|
| 1A | 1/4 | 576 |
| 2A | 3/8 | 864 |
| 3A | 1/2 | 1152 |
| 3B | 2/3 | 1536 |
| 4A | 3/4 | 1728 |

★ Protection may be **equal (EEP)** or **unequal (UEP)**, where the more important bits get more
protection; and interleaving spreads errors in **time and frequency** so bursts are recoverable.
Higher protection means lower capacity but the same coverage at lower power — which is exactly the
trade a DX-er wants to see reported, and nobody in the survey above surfaces it well.

## What we surface — "on par with our advanced RDS decoder"

Stuart, 2026-09-04: *"i want to see the codec, the error rate and error correction stats —
everything a signal nerd would love, i want our DAB implementation to be the absolute best on the
market with information on par with our advanced RDS decoder."*

That is the bar, and it is achievable because DAB carries far MORE than RDS does. The inventory
below is taken from EN 300 401 (downloaded and read, not remembered) so nothing here is guesswork
about what is available.

### The RDS equivalents — service information (clause 8.1, carried in the FIC)
| DAB | clause | the RDS thing it answers to |
|---|---|---|
| Ensemble label | 8.1.13 | — (no RDS equivalent; the mux name) |
| Service label / component label | 8.1.14 | PS |
| **Dynamic Label (DLS)**, via X-PAD | 7.4.5 | RadioText |
| Programme Type | 8.1.5 | PTY |
| Announcements + switching | 8.1.6 | TA / TP |
| Frequency Information | 8.1.8 | AF |
| Service linking | 8.1.15 | EON |
| Other-ensemble services | 8.1.10 | EON (other network) |
| Date/time, country, LTO | 8.1.3 | CT / PI country nibble |
| Component language | 8.1.2 | — |
| User applications (SlideShow, SPI/EPG) | 6.3.6 | — |

★★ **STATION LOGOS COME OFF THE AIR.** MOT SlideShow / SPI carries station artwork, so the "station
list with logos" Stuart specified need not depend on our own logo database at all — the broadcaster
supplies it. Fall back to `stationLogo.ts` when a mux does not send one.

### The nerd panel — what the demodulator can measure at each stage
★ Grouped by where it comes from, because that is also the order it becomes available: the physical
layer reports long before any service is decoded, which makes the panel useful while you are still
hunting a mux.

**Physical / OFDM**
- carrier frequency offset (coarse + fine), in Hz **and ppm** — the number that tells you your
  dongle is drifting rather than the transmitter
- sample-rate error, null-symbol depth, time-sync confidence
- **channel impulse response**: the SFN echo profile — how many transmitters you are hearing, at
  what delay and what relative level. ★★ This is the single most DX-interesting readout DAB can
  give and almost nothing on the market shows it.
- per-carrier SNR across the 1536, and DQPSK **MER / constellation tightness**

**FIC (the fast information channel)**
- FIB CRC pass rate — 12 FIBs per 96 ms frame, so a real error figure updated 12.5x a second
- FIG coverage: which extensions are being received, and how stale each is

**MSC (the audio path)**
- Viterbi corrected-bit estimate — the pre-FEC error rate, i.e. how close to the cliff you are
- **DAB+ superframe: Reed-Solomon (120,110) corrections per superframe, and uncorrectable count**
- **firecode CRC pass rate** — ★ and we intend to CORRECT with it, which DAB-Radio's own TODO says
  it does not do; that is the difference between a marginal mux stuttering and staying audible
- audio-frame CRC failures, concealment events

**Service configuration**
- protection profile: EEP or UEP, level, **code rate** (1A 1/4 … 4A 3/4) and the resulting capacity
- sub-channel size in CUs and start CU — where in the mux this service physically sits
- bit rate

**Codec** (Stuart's first-named item)
- MP2 vs AAC-LC / HE-AAC / HE-AACv2, **sample rate**, channel mode, SBR and PS presence
- and whether WE decoded it (MP2) or handed it to the browser (DAB+) — the honest answer to "why is
  this silent", which is the licence position made visible rather than mysterious

**TII — transmitter identification (clause 14.8)**
- main id + sub id, resolved against the ensemble's transmitter list where one is available
- ★★★ combined with the echo delays above this identifies WHICH transmitter you are receiving and
  roughly how far away it is. For a DX-er this is the whole game, and it is the clearest way to be
  "the best on the market" rather than merely complete.

## Build order (riskiest first)
1. Channel table + capability gate + the UI shell — settled, small, and it makes the rest testable.
2. OFDM acquisition: null-symbol detect, coarse/fine frequency offset, phase reference correlation.
   Reuses the FFT already in the shim. **This is the risk.**
3. DQPSK demap, frequency + time deinterleave, punctured Viterbi, FIB/FIC parse → ensemble and
   service list. The station list appears before any audio does, which is the honest way to build it.
4. MSC → MP2 decode (ours) — the BBC talks first.
5. DAB+ superframe → ADTS reframe → browser. Codec support already proven.
6. DRM last: partial by nature (CELP/HVXC are unreachable; say what the station IS, not what we lack).

★ Do not skip 3's checkpoint. A station list from the FIC with no audio at all is a real,
  demonstrable milestone and it proves the whole chain above the MSC.
