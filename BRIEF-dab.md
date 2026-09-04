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

## The chipmunks, named — and the two other things OWRX does not do

Stuart, 2026-09-04: *"I'm not going to half arse it like OWRX and call it a day. OWRX has
mismatched sample rates hence the chipmunks and only shows the station name, not even the now
playing text."*

Three separate failures, and all three are now designed against rather than hoped about.

### 1. The chipmunks are two bits wide (TS 102 563, table 2)
`dac_rate` and `sbr_flag` in the super frame header give four cases — and the AAC CORE rate is not
the playback rate:

| dac_rate | sbr_flag | AUs | core | **output** |
|---|---|---|---|---|
| 0 | 1 | 2 | 16 kHz | **32 kHz** |
| 1 | 1 | 3 | 24 kHz | **48 kHz** |
| 0 | 0 | 4 | 32 kHz | **32 kHz** |
| 1 | 0 | 6 | 48 kHz | **48 kHz** |

Two ways to get it wrong, both audible as chipmunks and neither failing loudly:
- assume 48 kHz when the service is 32 kHz → **1.5x fast**;
- take the CORE rate as the output rate when SBR is on → **2x fast**.

★★ AND IT IS OUR PROBLEM EVEN THOUGH WE DO NOT DECODE DAB+. We hand the bitstream to the browser,
but WE write the ADTS/ASC header it reads, and that header must carry the **core** rate — the
decoder does the SBR doubling itself. Put the output rate there and the browser makes the
chipmunks on our behalf. `vibe_dab_superframe.h` encodes exactly this, and the test pins all four
cases plus both wrong-ratio traps by name.

★ It also matters WITHIN a mux: services at 32 kHz and 48 kHz sit side by side, so switching
  service must renegotiate the rate. A pipeline that decides its rate once, at connect, is the
  same bug deferred.

### 2. "Only shows the station name" — we want the now-playing text
That is **DLS (Dynamic Label Segment)**, carried in X-PAD, and it is the RadioText equivalent. It
is not optional here: a receiver that shows a service label and nothing else is showing the least
interesting field DAB carries. DLS, plus the full inventory above.

### 3. And the firecode is for CORRECTING, not just checking
TS 102 563 on `header_firecode`: *"capable of detecting and correcting most single error burst of
up to 6 bits"*. The standard says correcting; DAB-Radio's TODO says it does not. That is the
difference between a marginal mux staying audible and stuttering, and it is a stated goal here.

## The decoder box: ONE PANE AT A TIME, stations first

Stuart, 2026-09-04, after considering a left/right split: *"stations/signal info toggle is the
better split actually, make that the default."* Agreed, and it is better than the split I had
argued for — with a toggle **each pane gets the whole box instead of half**, which answers the
scrolling problem outright rather than mitigating it. Side by side, both columns would have been
narrow AND still scrolling; this way neither is.

★ It also matches the instrument that already exists. On FM the display carries PS and RadioText
  inline and **ADVANCED RDS** is a button to the deep panel. DAB gets the same shape, so the two
  modes read as one radio rather than two designs.

**Left pane / default view — the station list**, and it carries the everyday fields itself:
- service label, and the **now-playing text (DLS)** — the thing OWRX omits and the reason Stuart
  named it. It belongs in the LIST, not behind the button.
- logo (off-air via MOT SlideShow where the mux sends one, else our own database)
- enough to choose a station: bit rate, codec badge, whether it is currently decodable.

**Behind the button — ADVANCED SIGNAL INFO**, the full block laid out exactly like the Advanced RDS
panel: codec detail and sample rate, error and error-correction statistics, protection profile and
code rate, sub-channel position, the physical layer (frequency offset in Hz and ppm, per-carrier
SNR, DQPSK MER, the SFN channel impulse response) and TII.

### The button lives in the decoder box HEADER
Stuart, 2026-09-04: *"the advanced signal info button must live in the header of the decoder box."*

★ The header is outside the scrolling pane, so the toggle cannot be scrolled away from — the same
  reason the frequency card's ✕ went in its header tonight rather than into the content.

★★ IN DAB MODE THE HEADER IS NEARLY EMPTY, so this fits any screen. Stuart, 2026-09-04: *"the
   decoder box will only have that button in it next to the minimise big/small and close buttons."*
   Four controls, no overflow — the crowded header that caused today's trouble is the FT8 case, not
   this one. ▶ The rule that keeps it true: **the DAB header stays minimal.** If controls are added
   later and it starts to overflow, the toggle is the one that must not scroll off, for the reason
   below.

★ (Kept for whoever adds that fifth control.) NOT MERELY PLACED THERE, IF IT EVER OVERFLOWS. That header SCROLLS HORIZONTALLY when its
    controls overflow the box — which is the exact fault Stuart reported earlier today: *"I had the
    FT8 box open and couldnt work out how to make it bigger or smaller or minimise it because the
    controls were off the edge of the box."* We fixed the AFFORDANCE (edge fades and chevrons) but
    the controls still scroll. If the STATIONS/SIGNAL toggle is allowed to scroll off on a narrow
    box, the entire advanced panel becomes unreachable and the feature looks missing rather than
    hidden. So it is fixed at one end of the header, outside the scrolling group, with the
    decoder's own controls scrolling beside it.

★★ AND IT COULD NOT LIVE ANYWHERE ELSE. Stuart, 2026-09-04: *"since the decoder box will be open
   for DAB anyway and moving to the demodulator menu will close the DAB box down."* A switch in the
   demodulator/decoders menu would be unreachable by construction — opening that menu dismisses the
   very box it controls. The header is not a preference here, it is the only surface that is up at
   the same time as the thing it switches.

★★★ THE COROLLARY: "always open in DAB mode" HAS TO SURVIVE THE MENU. If the menu closes the box,
    the box must come back when the menu does — either the menu does not dismiss it in DAB mode, or
    it is restored on dismissal. Otherwise a trip to the demodulator menu silently costs you the
    station list and the now-playing text, with no control anywhere to bring them back, and the
    receiver looks like it has dropped out of DAB when it has not. ▶ Decide which of the two on the
    way in and write it down; do not leave it to whichever happens.

★ It shows STATE, not just an action: which pane you are on has to be readable without pressing it.
  A toggle that does not say which side it is on is the same half-built indicator as a background
  job with no light.

### The one state rule that matters
The view returns to the station list **when the ENSEMBLE changes** — first open, or tuning to a
different multiplex — because a new mux means a new list and the old signal figures are about a
receiver you have left.

★★ It does NOT reset when you change SERVICE inside the same mux. That is the same lesson as the
   bookmark search earlier tonight: do not throw away the view somebody is working in. Someone
   comparing error rates across the services of one mux is doing exactly what this panel is for,
   and bouncing them back to the list on every selection would make it unusable for the one job it
   is best at.

## ★★★ AUDIO OFF AIR — 2026-09-04, 07:45

**BBC Radio 2, 3 and 4 decoded to listenable audio from the Pi's RTL-SDR V4.** Stuart, listening
to the WAVs: *"audio sounds good"*. The break-up in them is the capture (the dongle was run at a
fixed 30 dB and overloaded) and appears **at the same instant on all three services**, which is
exactly what a reception fault looks like and what a decoding fault does not.

| service | rate | protection | codec |
|---|---|---|---|
| BBC Radio 2 | 112 kbit/s | UEP L3 | Layer II |
| BBC Radio 3 | **160 kbit/s** | UEP L3 | Layer II |
| BBC Radio 4 | 112 kbit/s | UEP L3 | Layer II |

★ Radio 3 at 160 while the others are 112 is the BBC's real allocation, read off the air — a
  reality check that no synthetic test could have given.

### Two faults found by real audio that every synthetic test had passed
1. **UEP, not EEP.** Every BBC Layer II service uses the SHORT FORM of FIG 0/1 — a 6-bit table
   index, no explicit size — so `sizeCu` arrives as 0 and everything comes from table 8. A guard
   rejecting `sizeCu <= 0` silently refused every one of them while the station list looked
   perfect. Tables 8 and 15 are now extracted and all 64 profiles verified: coded bits == capacity.
2. **JOINT STEREO.** ★★★ The decoder matched ffmpeg to 3.7e-07 and was demonstrably correct — on a
   file I had encoded myself, which happened to be plain stereo. The BBC transmits mode 1, where
   sub-bands above the bound share ONE allocation and ONE set of samples between the channels.
   Reading two desynchronises the bit reader and the output is full-scale noise, with perfectly
   valid frame headers throughout. **A test built from convenient input agrees with the bug.**
   Four real BBC frames are now embedded in `test-dab-mp2` so it cannot come back.

★ Diagnosis: dumping our extracted logical frames and letting **ffmpeg** decode them separated
  "the bits are wrong" from "our decoder is wrong" in one step — ffmpeg got crest 7.2 from the same
  bytes we turned into noise, which pointed straight at the codec rather than the demodulator.

## ★★★ DECODED OFF AIR — 2026-09-04, 07:15

**Real BBC National DAB, 12B (225.648 MHz), RTL-SDR V4 on the Pi, 2.048 MSPS.**

```
frame  8: LOCK null 22.4 dB  off -111 Hz (-0.50 ppm)  shift +0  FIBs 12/12

=== ENSEMBLE ===  'BBC National DAB'  EId 0xCE15   15 services, 15 sub-channels
  SId 0xC221 'BBC Radio1'        [subch 1  MP2 ]
  SId 0xC222 'BBC Radio2'        [subch 2  MP2 ]
  SId 0xC223 'BBC Radio3'        [subch 3  MP2 ]
  SId 0xC224 'BBC Radio4'        [subch 4  MP2 ]
  SId 0xC225 'BBC Radio5Live'    [subch 5  MP2 ]
  SId 0xC229 'BBC Radio1Dance'   [subch 6  DAB+]
  SId 0xC22A 'BBC Radio1Xtra'    [subch 10 MP2 ]
  SId 0xC22B 'BBC Radio6Music'   [subch 11 MP2 ]
  SId 0xC22C 'BBC Radio4Extra'   [subch 12 MP2 ]
  SId 0xC22D 'BBC Radio1Anthms'  [subch 13 DAB+]
  SId 0xC22E 'BBC Radio3Unwind'  [subch 14 DAB+]
  SId 0xC236 'BBC AsianNetwork'  [subch 7  MP2 ]
  SId 0xC238 'BBC WorldService'  [subch 9  MP2 ]
```

12 of 12 FIBs on every frame, offset tracked at −0.5 ppm, phase reference correlating **0.922**
against a real transmitter. The MP2/DAB+ split is read from the air and matches reality: the
classic networks are Layer II, the newer ones DAB+ — which is exactly why the browser-refuses-MP2
measurement mattered.

★★ **THE BUG THAT COST THE SESSION, AND WHAT IT LOOKED LIKE.** Everything above the bit ordering
   was right from the first live run: 22.3 dB null lock, offset to 0.5 ppm, phase reference at
   0.92 — and 0 of 12 FIBs. Two faults, both in how bits reach the Viterbi:
   1. clause 14.5 — the 2K bits per symbol are TWO HALVES (real parts then imaginary), not
      alternating pairs;
   2. clause 14.4.1.1 — the four FIC codewords are **concatenated** (i' = 2304·mod(r,4) + i), not
      round-robin interleaved.
   ★ The flattened PDF renders that second equation as literal garbage ("3042 and 3032"). It only
     became readable by pulling the single page. **When a spec extraction looks like noise, read
     the page — do not infer the shape of the rule.**

## STATUS — 2026-09-04, 01:15

Built and tested overnight. Every header is `vibe_dab_*.h` in `android/app/src/main/cpp/`, each
with a matching `vibeserver/test-dab-*.cpp`.

| stage | what | state |
|---|---|---|
| 1 | channel plan (41 blocks), capability gate on EFFECTIVE limits | ✅ tested |
| 2 | transmission modes, in samples — verified against ETSI table 22 | ✅ tested |
| 3 | DAB+ superframe rate bits (the chipmunk bug) | ✅ tested |
| 4 | null-symbol acquisition + frame tracking with fade tolerance | ✅ tested |
| 5 | OFDM: fractional offset from the CP, de-rotation, soft DQPSK, carrier↔bin | ✅ tested |
| 6 | FEC: rate-1/4 soft Viterbi, energy dispersal, FIB CRC | ✅ tested |
| 7 | frequency interleaving — bijection **proven**, not sampled | ✅ tested |
| 8 | FIC/FIG parser → ensemble, services, labels, sub-channels | ✅ tested |
| 9 | phase reference symbol (tables machine-extracted from the PDF) | ✅ tested |
| 10 | puncturing vectors + depuncturing (machine-extracted) | ✅ tested |
| 11 | **complete FIC decode: soft bits → ensemble + station list** | ✅ tested |
| 12 | MSC: CIF geometry, 16-deep time deinterleaving, EEP profiles | ✅ tested |
| 13 | **MPEG-1/2 Layer II decoder — verified against ffmpeg to 3.7e-07** | ✅ tested |
| 14 | DAB+: RS(120,110), virtual interleaver, super frames, ADTS reframing | ✅ tested |
| 15 | **DabReceiver — the whole chain, IQ in, ensemble out** | ✅ tested |
| 16 | radix-2 FFT (was a deliberate O(N²) DFT while unproven) | ✅ |
| 17 | MSC per-service audio: CU extraction → deinterleave → FEC → frames | ✅ **off air** |
| 18 | UEP tables 8 + 15, all 64 profiles verified | ✅ tested |
| 19 | **joint stereo** — the fault only real broadcast exposed | ✅ tested |

★★★ **STAGE 11 IS THE MILESTONE, AND IT PASSES.** `test-dab-ficdec` builds a small multiplex,
transmits it through the real encode/puncture/multiplex chain, corrupts 5% of the symbols, and
recovers "BBC Radio 4" with its codec and sub-channel. That is the entire path below the MSC
proven end to end in one test — scrambler, rate-1/4 code, PI 16/15 puncturing, the tail vector,
the four-codeword round robin, the Viterbi, the CRC and the FIG parser. ★ It also proves the demux
PHASE, because decoding the wrong slot must yield nothing, and the test asserts that too.

**Still to build, honestly:**
- **The MSC audio path through the receiver** — the FIC half is wired and locks on a synthesised
  transmission; the per-service MSC extraction (CU selection, time deinterleave, EEP depuncture,
  Viterbi, then MP2 or DAB+) is built and tested stage by stage but not yet joined to DabReceiver.
- **Speed**: the receiver uses a plain O(N²) DFT — deliberately, so the first integration bug
  cannot be hiding in a transform. `vibedsp::ComplexFFT` is the one call to swap in, and it must
  be swapped before a Pi sees this.
- **UEP profiles** (table 15) for the older services — EEP is done, UEP extracts the same way.
- **Firecode CORRECTION** on the DAB+ header (detection is in; correction is the stated goal that
  DAB-Radio's TODO says it lacks).
- **Integration** into the shim (a DAB source path, per-service sample rates) and the **web client
  UI** (mux tuning, station list, signal panel, the pinned toggle).
- **Deployment** to the Pi.

★★ THE HONEST SUMMARY: the physical layer and the whole FIC path are built and tested; the audio
   path and the integration are not. Nothing here has yet seen a real signal — every test is
   synthetic or spec-derived, and AGENTS.md's rule stands: measure on the live radio before
   believing any of it. The first real test will be the station list, because it needs no codec.

★ Three spec tables (23/24 phase reference, 13 puncturing) were MACHINE-EXTRACTED from the PDF
  rather than typed, and each has an independent consistency check in its test — the puncturing
  one verifies that every vector keeps exactly 8+PI of 32 bits, which is its printed code rate.

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
