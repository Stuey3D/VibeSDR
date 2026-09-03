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
