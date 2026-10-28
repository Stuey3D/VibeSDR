# Background decoding and the gallery (V12)

**Status:** built 2026-10-22 on `v12-bgdecode`. Phase 1 (NAVTEX + RTTY, the gallery, the web GALLERY) shipped in
12.0.0~beta2 the same night, so NickB (G4IRX) could decode NAVTEX overnight while Storm Austen came in. Phase 2 adds
WEFAX/SSTV pictures, FT8/FT4 run summaries, NFM for SSTV and the app's GALLERY.
**Related:** `DECODERS-GALLERY.md` (the satellite decoders and client-assisted decode that will also fill the gallery),
`RADIO-ROLES.md` (dedicated and hidden radios — the next trigger for the same engine).

## What the owner sees

Setup → Radio, below the power-save/hand-off settings (Stuart's wording):

> **Background Decoding:** When the last user leaves the server after [5 minutes] the SDR will move to your selected RF
> Centre. Set an RF centre here and choose 1 or multiple VFO's and decoders for background decoding.

- On/off per radio (default **off**) and the wait (default **5 minutes**).
- **Profiles.** Each one has an `RF Centre`, then VFO rows in the order a listener chooses: frequency → demodulator →
  decoder. A row's decoder list holds only the decoders that work on its demodulator. Each row has Remove, and there is
  `+ Add another VFO` (up to 8). Under the rows: "Background profile active between [00:00] - [00:00]". Times are UTC,
  with the server's local time shown beside them; 00:00–00:00 means all day.
- "Want to add another background decoding profile?" appears **only** under a profile whose times are not all day. Every
  profile can be deleted. Up to 4 profiles.
- **Refused at the save, with the reason given** (`vibe_bgdecode_config.h validate`, enforced by the daemon rather than
  the page):
  - overlapping profiles, naming both and the minute they clash;
  - a VFO the radio cannot see at its sample rate, giving the distance and the radio's ±window;
  - a decoder on a demodulator it does not work on.
- The frequency typed is the DIAL, as a listener would type it. (A per-VFO "Listening for … kHz" line was tried and
  removed — Stuart: "they arent needed, and would look odd to someone setting up wefax or rtty".)

Nick's case: centre 500 kHz; VFO 1 489.5 kHz USB NAVTEX; VFO 2 517.5 kHz USB NAVTEX; all day.

| Decoder | Demodulators | Audio tone (USB: signal = dial + tone) | Kept as |
|---|---|---|---|
| NAVTEX | USB, LSB | 500 Hz | one .txt per message, written when the message completes |
| RTTY auto / 45.45 Bd 170 Hz / 50 Bd 450 Hz | USB, LSB | 1000 Hz | text appended every minute or 8 KB; one file per UTC day (max 1 MB) |
| WEFAX 120 LPM | USB | 1900 Hz (the carrier) | one PNG per chart, start tone to stop tone |
| SSTV | USB, NFM | dial | one PNG per picture |
| FT8 / FT4 | USB | dial | a **summary**, not the spots: a standing "last 10 minutes" card + one card per UTC hour |
| DSC (MF/HF) · DSC (VHF ch 70) | USB, LSB · NFM | 1700 Hz · dial | the day's log per DSC frequency + one item per distress/urgency call, pinned at its position (docs/v12/DSC.md); **+ DSC watch** fills the VFOs |
| Digital spots: Band (summary) | USB (greyed — set by the decoder) | — (a band picker; the row stores the band's dial) | the FT8 summary's two cards for the whole band, every mode counted (2026-10-27, below) |
| NDB skimmer (summary) | USB (greyed — set by the decoder) | — (no frequency: the part of 190–535 kHz the capture holds; stored hz 0) | the NDB cards: the day's heard log and map, last 10 minutes, one per UTC hour (2026-10-27, below) |

★★ **LSB inverts mark and space.** NickB asked for LSB so he could dodge local interference. An FSK decoder on an LSB VFO
is started with `inverted` flipped, so the same signal decodes identically on either sideband. `test-bgdecode` proves this
for RTTY, and for NAVTEX with its own polarity search switched off. WEFAX and SSTV are not offered on LSB: mirrored
audio gives a negative chart and a scrambled picture, and neither decoder can be told to correct for it.

Setup → Server holds the gallery settings:
- **size** (100 MB by default; 0 keeps nothing), shown beside the free space on the disk;
- **Everyone may see the gallery** (on by default);
- **Keep listeners' decodes** (on by default).

## How it works

```
                housekeeping thread (2 s)                       connect path (spectrum / audio / decoder socket)
                ─────────────────────────                       ─────────────────────────────────────────────────
  bgTick: nobody here ≥ wait, profile due ──► bgStartLocked      resumeCaptureIdle → bgStopForListener
          boundary / settings change ──► switch                   → stop runner, put the radio back, re-arm the wait
          no profile now ──► stop + restore

  DSP thread ──copy──► BgRunner queue (≤ 0.5 s, oldest dropped) ──► vibe-bgdsp thread (decoder priority)
                         Channelizer (one forward FFT) ─► per VFO: extract ─► RxPipeline (USB/LSB/NFM) ─► DecoderHost
                                                                                               └─► DecoderTap ─► gallery
```

- **One trigger, not the design.** `BgRunner` knows nothing about listeners. The shim's `bgTick` decides when it runs.
  A dedicated or hidden radio (`RADIO-ROLES.md`) will start it and never stop it.
- **Each VFO gets the same kind of channel a listener gets.** It is cut from one forward FFT of the capture, then has
  its own demodulator and its own `DecoderHost`, which runs its own decode thread and holds its own slot.
- **The shared-dial contract.**
  - Before moving the radio, the tick saves the centre, the view and the dial it was on.
  - Any arrival stops everything on the connect path, before the landing or the config are sent. That includes a
    pocketed app's audio socket or a decoder socket on its own.
  - The radio is then put back on the saved dial, so the newcomer lands where they would have without background
    decoding, never on the NAVTEX centre.
  - A profile switch does not restore the dial in between, because nobody is there to give it to.
- **The idle park is held off** while a profile is due (`bgWanted_`): parking and waking a radio just to take it back
  five minutes later is waste, and parking is the riskiest thing the server does to a radio. A parked or released radio
  is taken back by `resumeCaptureIdle(fromBg)`.
- **Retuning** goes through `retune()` first, so the AGC's bookkeeping, the band's gain cap and the permitted ranges see
  an ordinary tune. The capture is then placed exactly at the profile centre (`bgPlace`). A pan move that was put off is
  cancelled. On a locked-centre radio the capture moves too, and goes back to the locked centre on arrival. Spectrogram
  rows are skipped while the capture is away from the locked centre.
- **The sample rate is not changed.** A VFO outside the configured rate's window is refused at the save. VFOs are
  checked again against the rate the radio really runs at, and any outside it are skipped with a log line.
- **Decoder slots** are box-wide (`FileSlots`), and background decoders take them like listeners do. If a listener's
  decoder is refused:
  1. background decoding on this radio stops;
  2. every other radio of the machine is asked to give its slots back by writing `<slot dir>/bg-yield` — each runner
     checks that file's mtime twice a second, lets its decoders go, and its tick stops it and starts the wait again;
  3. the listener's decoder is tried again, after waiting up to 3 s for a slot to free.
- **A save reaches the radio's own process.** On a multi-radio Linux box the setup page saves through the FRONT DOOR,
  which owns no radio (the 12.0.0~beta2 bug on the Lenovo: the door applied the profile to itself and the RSP's process
  idled and was released). Every radio process therefore watches `config.json` (its stamp, every 2 s) and applies its
  own `bgDecode` and the machine's gallery settings when they change; re-applying the same settings restarts nothing.
  The RSP keepalive's "probe done — releasing again" is held off while a profile is due or running.
  `scripts/test-bg-decode-frontdoor.mjs` runs that layout.
- **Not on an Icom** (one physical receiver, no IQ to cut channels from; the control is hidden there) and **not in DAB**.

## The gallery

`vibe_gallery.h`. One directory per machine, `<data dir>/gallery`, written by every radio process on the machine. The
index, `index.jsonl`, has one line per item and is changed only under `flock(<dir>/.lock)`.

- **Bounded.** The owner's size is enforced on every write by deleting the oldest items first. An RTTY run still being
  written is never deleted. The index is capped at 5000 lines and is rewritten (never grown) when items are evicted.
- **Never a write per character.** NAVTEX is written once per message. RTTY is written once a minute or every 8 KB. A
  picture is written once, when it is complete.
- **Partial items.** A message or picture cut off — by a listener arriving, a profile switch, a new start tone before
  the stop, or a lost lock — is saved once and marked `partial`.
- **Labels.** Each item records background or live, time, frequency, mode, decoder, profile and radio. Nothing in an
  item locates the server beyond what the station itself transmitted.
- **Listeners' decodes.** `decoderEnv` gives every listener's host a tap labelled `live`, unless the owner has switched
  that off. The tap follows the listener's dial: on a retune, a NAVTEX message in progress is dropped (as the client's
  box drops it) and an RTTY run starts a new file. A listener's FT8 session gets the same summary as a background run.
- **The NAVTEX cutter** is a port of `src/utils/navtex.ts`, so a saved file holds exactly the message the decoder box
  showed. Its body is the same as the box's SAVE file, with one extra line saying where the message was heard.
- **FT8 summary.** It lists:
  - spots heard (FT8/FT4) and the number of stations;
  - the number of countries;
  - the furthest station: call, country, grid, and km from this server's own position (taken from setup's lat/lon or
    locator — the same source as DAB transmitter distances);
  - the most frequent and the rarest country, and stations by country.

  Countries come from `vibe_callsign_country.h`, which is generated from the app's `src/services/callsignCountry.ts` by
  `scripts/gen-callsign-country.mjs`, so the app and the server name countries the same way. Nothing is written for a
  stretch that heard nothing. **Two kinds of card (2026-10-23, replacing beta 4's one card per run rewritten every 10
  min):**
  - **Last 10 minutes** — the instant look. ONE standing card per VFO ("FT8 last 10 min · 23 spots · 9 countries ·
    furthest Japan"), always the rolling last 10 minutes. It is rewritten at most once a minute, and only when the
    window changed (a spot arrived or one aged out), atomically (`Store::replace`: a temp file renamed over the item,
    and the index the same way, so a power cut leaves the previous card whole), and moved to the top of the list. An
    empty window says "nothing heard since 18:42". When the run ends it is written once more ("… to 20:20 UTC") and kept,
    so a listener whose arrival stopped the run sees what it was hearing. The index marks it `"s":"last10"`; the next run
    on the same radio, origin, profile and dial finds it again (`Store::adoptSlot`), so a restart or a crash never leaves
    a second one.
  - **Hourly** — the record, for judging an antenna over days. One card per UTC clock hour ("FT8 19:00–20:00 UTC · 412
    spots · 31 countries · furthest Australia"), written when the hour closes and never rewritten; old ones are evicted
    oldest first like everything else. A run that began or ended inside an hour writes the part it heard, marked
    partial, with its minutes ("40 of the hour's 60 minutes"); a run stopped and restarted within one hour (a listener
    came and went) leaves two partial cards for it. Midnight is just an hour boundary.
  - **The cost, stated:** each rewrite of the last-10 card rewrites `index.jsonl` whole (about 250 bytes an item —
    125 KB for 500 items, 1.2 MB at the 5000 cap), at most once a minute per FT8 VFO and not at all on a quiet band.
    The hourly cards are one small append each.
  The admin line's live count is still the run's ("142 spots, 23 countries so far").
- **Pictures** are written as PNG by `vibe_png.h`, using `vibe_gzip.h`'s deflate, so no new library is needed.
- **HTTP, read-only:**
  - `GET /vibeserver/gallery.json?kind=&limit=` lists the items newest first (the old request: up to 1000, default 300);
  - **pages (beta 4):** `?offset=&limit=&kind=` — at most 100 a page (default 25), with `total` (how many match the
    filter) and `offset` added. Served from an in-memory copy of the index, rebuilt only when `index.jsonl` changes
    (inode, size or mtime) or every 30 s, so a page costs one `stat()` per item shown. `kind` keeps digits (`ft8`).
  - `GET /vibeserver/gallery/file/<name>[?download=1]` serves one item.

  Both are gated by the PIN like every other read. If the owner made the gallery private, only the admin can read it. A
  requested name must pass three checks, any one of which refuses `../`:
  1. it uses only the safe alphabet;
  2. it is listed in the index;
  3. it opens with `O_NOFOLLOW`.
- **Clients.** The web client has GALLERY in the DECODERS menu; the app has GALLERY in Mode → CLIENT DECODERS (VibeServer
  only). Both list items newest first with filters by decoder, **25 a page** with the pager at the bottom ("‹ Newer ·
  1 … 6 7 8 … 14 · Older ›", "Page 7 of 14" — `src/utils/galleryPager.ts`, one rule for both); a filter change goes to
  page 1; BACK from an item returns to the same page, scroll and row (the row keeps the focus, so keyboard, D-pad and
  controller carry on from it). An older server without pages gets the old request and no pager. Text items open inline (shown as text, never HTML) and
  images open as images; both clients can download or save an item.
- **TRANSLATE (2026-10-23)** beside ‹ BACK on a NAVTEX message or RTTY text (never an FT8 summary — that is our own
  English). The app uses the phone's on-device translator, the one TRANSCRIBE's captions use: Apple's Translation on
  iOS 26+ (`VibeTranscriber.translateText`, its pack sheet; the language read by NaturalLanguage,
  `detectTextLanguage`, with the station's language as the prior), ML Kit on Android (`TextTranslate.kt`, packs on
  Wi-Fi only, any Android version; no detector is bundled, so the language comes from the NAVTEX stopwords and the
  station). The target is TRANSCRIBE's TRANSLATE TO, or the phone's language when that is off. The web client uses the
  browser's own `Translator` / `LanguageDetector` into the browser's language, and draws no key where the browser has
  none or cannot do the pair. One rule for both (`src/utils/galleryTranslate.ts`): line by line so the layout
  survives; our header, ZCZC/NNNN, figures and positions, `[… signal lost N s …]` markers and garbage lines stay as
  received; all-capitals lines go in lower case and come back in capitals. Shown in place, marked MACHINE
  TRANSLATION, ORIGINAL toggles back; no key when the text is already in the target language.

## ISM profiles and ISM in the gallery (2026-10-25)

A profile can decode **ISM sensors on a whole band** instead of an RF centre + VFOs (`"ism":"433"`), and every ISM
session — a listener's or background's — keeps the day's readings as a .txt and a summary card per band. See
docs/v12/ISM-DECODER.md, "The gallery". In short: ISM runs only with nobody here (a whole-capture mode has nothing "in
range" of a listener), through the listener's own ISM mode, and a listener arriving ends it through ISM's own door.

## NDB and Band digital spots (2026-10-27)

Stuart (2026-10-27): *"I'd like to add NDB to the background decoding as my SDRPlay on the Lenovo already decodes Navtex
and 160M FT8 in the background NDB would be within its range too. Also full band digital modes too."*

### The NDB skimmer is a VFO decoder (2026-10-27, second pass)

★★ Stuart (2026-10-27): the NDB skimmer must be a **per-VFO decoder**, running BESIDE the profile's other VFOs on the same
capture — like the CW/PSK skimmers and Digital spots: Band in the same list. His case: the Lenovo RSP1A at 3 MS/s, RF
centre 1200 kHz, NAVTEX 489.5 + 517.5 and 160 m — that capture already spans the whole 190–535 kHz band. He looked in the
VFO decoder list and NDB wasn't there (it was a whole-radio profile kind, which dropped every VFO). **The whole-radio
"Decodes: NDB beacons" option is gone**; NDB is **NDB skimmer (summary)** (`"dec":"ndb-skim"`) in a VFO row's list.

- **The VFO's frequency means nothing — the decoder sets it.** The skimmer covers as much of 190–535 kHz as the capture at
  the profile's RF centre holds, inside the same guard every background VFO keeps from the capture's edge
  (`vibe_bgdecode_config.h ndbSegment`: centre ± `halfWindowHz(rate)`, clipped to the band and to what the radio reaches —
  `vsNdbReach`). The row's frequency box and demodulator are **greyed out** (disabled) on the setup page with a line saying
  what it covers ("Covers 190–535 kHz of the NDB band at this RF centre …"), and the row stores `{"hz":0,"mode":"usb"}`.
  At 1200 kHz / 3 MS/s that is the whole band; at 700 kHz / 1.024 MS/s it is 241–535 kHz.
- **Fed like the CW skimmers** (`vibe_bgdecode.h`, `BgRunner::placeNdb`): ONE IQ slice off the runner's SAME forward FFT,
  centred on the covered segment, sized and tapered as the listener's NDB tap cuts one (segment + 2.6 kHz over 0.92 of the
  slice rate, a power of two of bins — `vibe_wide_tap.h`'s rule). The slice goes to the **listener's own NDB service** in a
  new fed-slice mode (`vibendb::Service::startFed` / `feedSlice` — a copy into a two-second queue, the oldest dropped and
  counted, a decode thread of its own at decoder priority): the same skimmer, beacon list, harmonic flags and **the same
  gallery sink** (`vibe_ndb_gallery.h DayLog`, labelled background and the profile) — the day's heard log and map, the
  standing last-10-minutes card, one card per UTC hour. The decoder is made through `Params::ndbMake`
  (`vibe_bgdecode_ndb.h makeNdbFeedFactory`), so the runner and its other tests do not carry the beacon list.
- **One decoder slot, like any VFO**: claimed when it starts, given back when it pauses; listeners still win it (the tier
  list's "lowest VFO pauses"), and a locked radio's channel budget counts it. Paused **with the reason** while a
  listener's own NDB mode has the band (their skimmer keeps the cards meanwhile — never two skimmers on the same beacons)
  or the owner has NDB switched off on the receiver. With "keep decoding in range", a listener's dial that still holds
  ≥ 100 kHz of the band keeps it running (re-aimed — `aimFed` — not rebuilt; the day's cards carry on); one that does not
  takes it out of the window, its slot back. When a profile with a Band VFO moves the centre to keep the DC off its modes
  (`bgBandCentre`), the move may not shrink what the NDB skimmer covers.
- **Refused at the save, in words** (`validate`): on a radio that reaches no LF ("this radio cannot reach the NDB band
  (190–535 kHz) — an RTL needs direct sampling, and a HackRF, an Airspy R2/Mini or an Icom cannot do it at all"); where the
  capture holds under 100 kHz of the band (`kNdbMinSpanHz`: "the NDB skimmer needs at least 100 kHz of the NDB band … at
  2400 kS/s around 7000.0 kHz this radio sees 5853.0–8147.0 kHz, none of the band. Move the RF centre nearer the band");
  and a second NDB skimmer in one profile. A locked radio may run it when its lock covers the band (nothing moves).
- **Not offered where it cannot run** (AGENTS.md): the setup page lists it only where the radio reaches LF (hwinfo
  `ndbHw`, else the driver: an HF+, an RSP, an RTL with direct sampling on or automatic) and NDB is not switched off — once
  a profile. The headline's NDB sentence shows only there too.
- **Older profiles** saved as the whole-radio kind (`"ndb":true`) are **converted on load**, by the server's `parse()` and
  the page's `bgLoad()` alike, into a VFO profile with RF centre 362.75 kHz (the band's middle, off the 0.5 kHz raster) and
  one NDB skimmer VFO, their hours kept — they decode what they did, and save back in the new shape. The whole-radio
  background path in the shim (`bgNdbStartLocked`) is removed.
- **The admin line**: "NDB skimmer 190–535 kHz · running · 12 beacons identified of 41 carriers · 3.2 %" (status JSON
  `ndbLo` / `ndbHi` / `ndb`).

### NDB keeps the FT8 summary's cards

Whoever runs the skimmer — a listener's NDB mode, or the background NDB skimmer VFO above — the gallery keeps what an FT8
VFO keeps (`vibe_ndb_gallery.h`, `DayLog`):

- **Last 10 minutes** — ONE standing card, "NDB last 10 min · 23 beacons · furthest KER 412 km · best WOD 26 dB", the
  rolling last ten minutes; rewritten at most once a minute and only when the window changed, moved to the top, adopted
  by the next run (`Store::adoptSlot`, slot `ndblast10`), written once more "to 20:20 UTC" when the run ends and kept.
- **Hourly** — one card per UTC clock hour, "NDB 19:00–20:00 UTC · 41 beacons · furthest KER 412 km · best WOD 31 dB",
  written when the hour closes and never rewritten; partial with its minutes where the run began or ended inside it.
- Both list every beacon heard, lowest frequency first: ident · kHz · minutes heard · best SNR · distance · bearing ·
  country · station (an unlisted beacon by its ident alone). Nothing is written for a stretch that heard nothing.
- **One store.** The cards are fed by the same `heard()` the day's log is (the service's confirmed beacons, at most once a
  minute each); there is no second record of NDB readings. A listener's NDB session gets the same cards, labelled live.
- The RSP reaches LF (the API tunes from 1 kHz; the band constant is `vibendb::kBandLoHz`/`kBandHiHz`, 190–535 kHz). The Lenovo's
  RSP is offered the NDB skimmer by its driver at the save and by hwinfo `ndbHw` on the page.

### A Band digital-spots VFO

A VFO row can now choose **Digital spots: Band (summary)** (`"dec":"spots-band"`, USB rows only): every digital mode on the
band the VFO's frequency is in — FT8, FT4, WSPR, JS8, FT2, JTTY and PSK on their own dials — through the listener's own Band
mode (`digi::Spotter`, Band scope) and its CPU governor (`digi_governor.h`), kept as ONE summary for the band.

- **The setup page picks the BAND** (Stuart, 2026-10-27: when the decoder sets the frequency, the row's frequency box and
  demodulator grey out). A Band row has a **band picker** in place of the frequency — from the radio's own list (hwinfo
  `spotBands`), only the bands whose digital modes the profile's capture holds (160 m alone at 1200 kHz / 3 MS/s); the
  demodulator is greyed out. The row stores the band's dial (its span's centre), so an older row's typed frequency inside
  the band loads into that band and saves back unchanged until another band is picked; a band the centre no longer holds
  stays listed, marked "(outside this window)", in red. Only where the radio's band list is unknown (its radio offline)
  does the row keep a typed frequency.
- On the server nothing changed: **the frequency names the band** (7074 = 40 m). The slice is cut on the band's span (`digi::spanOf` — the cluster round
  FT8, ≤ 48 kHz), not on the typed frequency, sized and tapered exactly as a listener's Band slice (`vibe_band_slice.h`),
  off the runner's SAME forward FFT; the band's extra slices (40 m's JTTY at 7.090 …) are cut where the capture holds them.
  Each block carries its CAPTURE time, stamped on the DSP thread (`BgRunner::feedIq`), so FT8/WSPR slots are cut by when
  the signal was heard, not when the runner got to it; a hole the queue dropped is bridged with zeros (`bridgeBand`).
- **The card** is the FT8 summary (`Ft8SummaryTap`, decoder `ft8band`, the band's centre as its frequency): "Digital
  spots summary · Band 40 m · background decoding (Profile 1)", spots per mode, stations, countries, the furthest; the
  standing last-10 card and one per UTC hour, as an FT8 VFO's. Under the gallery's FT8 filter in the web and the app.
- **The governor, as for a listener.** The owner's word per mode (Setup → Modes and decoders → Digital spots: Band), the
  activity gate (an empty mode window is never decoded), engines told to go light when the server is loaded, and the
  benchmark's Band budget (`vsBandBudgetUnits`, from the `dec_ft8band` row) — **shared** between the profile's Band VFOs,
  never each given the whole box. One decoder slot per Band VFO, like any VFO; listeners still win it.
- **Where Band is switched off on this radio** (the owner, or the benchmark's default on a box that measured it red —
  Stuart's rule: the benchmark sets the default, it never hides) the VFO is shown **paused, with the reason**, takes no
  slot and is never started behind the owner's back; the setup page draws its row in red saying so. On an Icom, or a
  radio that reaches no band (hwinfo `spotBands` empty — an RTL without direct sampling), the choice is not offered.
- **The DC.** A zero-IF radio's DC sits at centre + `hwOffsetHz()`. When a profile with a Band VFO moves the radio, the
  centre is moved the smallest step that keeps the DC out of the Band VFO's slices (else off its modes —
  `vibe::bandDcCentre`, the listener's own rule), but only if every VFO of the profile stays in the window; otherwise the
  owner's centre stands and the log says so once.
- **Refused at the save, in words** (`validate`, with the span from `vibe_bgdecode_band.h installBandSpan()` — the daemon's
  save path links the registry): a frequency on no band ("6500.0 kHz is not in a band the digital modes use — type a
  frequency inside the band, e.g. 7074 for 40 m"), and a band whose modes are not all inside the window ("the 40 m band's
  digital modes (7.0396–7.0816 MHz) are not all inside this radio's window … Move the RF centre nearer the band").
- **The admin line**: "7074.0 Digital spots: Band 40 m · running · 212 spots, 31 countries so far · 2.1 %", and the status
  JSON carries the band and what the governor runs now ("WSPR 7.0386 idle, … FT8 7.0740 active, … JTTY 7.0900 idle").
- **Priority unchanged**: a listener arriving stops background decoding and gets the radio (`bgStopForListener`); with
  "keep decoding in range" a Band VFO whose span is still inside the listener's window carries on, re-cut; the RSP
  keepalive is still held off while a profile is due or running.

**Measured CPU** (`test-bg-band`, M5 Pro, the whole process as % of one core): at 250 kS/s, 40 m with two FT8 stations
0.4–1.1 %; at 2.4 MS/s with the runner's 65536-point FFT and a quiet band, one FT8/FT4 VFO 2.7 %, one Band VFO 2.9 %, Band +
a NAVTEX VFO 3.4 % — the shared FFT is most of it; the Band slice adds one small inverse FFT a block per slice, and on a
quiet band the gate decodes nothing. A busy band's engines cost what a listener's Band does (docs/v12/DIGITAL-SPOTS.md
"The Band governor"), capped by the box's budget. Not measured on the Pi 2 or the Lenovo yet.

**Stuart's Lenovo RSP, step by step** (Setup → Radio → the RSP's tab → Background Decoding, on):
1. The existing profile: **RF Centre 1200** kHz (the RSP at 3 MS/s) → VFO 1 **489.5** USB NAVTEX → VFO 2 **517.5** USB
   NAVTEX → VFO 3 decoder **Digital spots: Band (summary)**, band **160 m** (the only band this window holds; frequency
   and demodulator greyed out) → **+ Add another VFO** → VFO 4 decoder **NDB skimmer (summary)** (frequency and
   demodulator greyed out; the line says "Covers 190–535 kHz of the NDB band at this RF centre"). Four VFOs, four
   decoder slots — all on the one capture.
2. Save. The save refuses overlapping hours, a band outside the window, an NDB skimmer whose capture misses the band, or
   NDB/Band where the radio cannot run them.

## Listeners first: priority, in range, locked radios (2026-10-22, beta3)

- **Priority is the VFO order.** The setup page shows a tier list (rank numbers, a drag handle, ▲/▼ for keyboard and pad)
  wherever the slots can run short: on several radios, while "keep decoding in range" is on, or on a locked radio. Only a
  single unlocked radio with that switch off has no contention; there a profile is capped at the box's decoder slots
  ("This server can run 8 decoders at once — that's the most VFOs a profile can have"), and the save refuses more.
- **Listeners always win a decoder slot.** A listener's decoder that finds none pauses the LOWEST-ranked running
  background VFO on the box, that one only. A tie on rank is broken by the radio running the most. The paused VFO's
  output is written first (partial where cut off). It resumes, the highest-ranked paused first, within ~3 s of a slot
  freeing. Across radio processes this is `BgRegistry`: `bg-<pid>.state` (ranks, running or paused, under flock) and
  `bg-<pid>.drop` (one byte per request) beside the slot files.
- **Keep decoding stations still in range while listeners are on** (per radio, default on). A listener arriving still
  gets the radio back on its own dial. Every VFO inside the window the radio now captures carries on: re-cut against
  the new centre, not rebuilt, so a message in progress is not split. The rest pause, and follow the window as the
  listener retunes. The radio's centre, dial and rate are never moved for a background VFO. When the last listener
  leaves, the normal start applies after the wait: the radio moves and is re-cut, nothing is rebuilt. Items decoded with
  a listener on carry `"wl":true` ("BACKGROUND · LISTENER ON").
- **Locked-range radios** never move.
  - A profile has no RF centre of its own; its VFOs must sit inside the locked window, and the save refuses one outside.
  - Background decoding runs all through its hours, listeners or not, and starts at once (the wait is for radios it
    would move).
  - It may use only the FREE listener places (max listeners − listeners), as well as free decoder slots. A real listener
    is never refused because of it: the lowest VFO gives up its channel.
  - Background VFOs are never counted as listeners anywhere. They are not listener channels at all.
- Tests: `test-bg-priority.cpp` (36), `test-bg-decode-slots.mjs` (two radio processes), `test-bg-decode-inrange.mjs`
  (648 kHz with Nick's profile, tune away and back, a decoder claim, switched off, a locked radio),
  `test-setup-bg-tiers.mjs`.

## The held session: a suspended phone keeps its place (2026-10-27, v12-session-hold)

Stuart: *"rather than a 60 second grace period as that doesnt leave the user much time to do what they are doing in
their messaging app or whatever, maybe a 3 minute grace period and when the OS suspends the app we send a little
notification … your session on the server is held for 3 minutes all decoders are still running for this period and no
data is missed."* And: *"obviously if the user hits a hard time limit within that grace period then that is the hard
rule and user gets disconnected as normal".*

iOS suspends VibeSDR a few seconds after it goes to the background **whenever nothing is playing**: ADS-B, NDB, ISM and
AIS send no audio, so the app releases the audio session there (`setQuiet`). The sockets go with the suspension. Before
this change the server treated that listener as gone. Now it **holds** them. Background decoding waits for the hold to
end before its own wait begins: a held listener is somebody here.

**What a hold keeps** (local_sdr_shim.cpp, "THE HELD SESSION"). It starts when the last spectrum or audio socket of a
session closes:
- the whole-radio mode (ADS-B, NDB, ISM, AIS, DAB) and the dial. The radio is not left, parked or released, and
  background decoding does not take it;
- their channel on a per-VFO radio (VFO, mode, filter, AGC, effects), kept running and adopted back on return;
- their decoders (text, image, CW/PSK skimmers, digital spots, Single or Band), still running and still in the gallery;
- VFO B (the watch channel) and its squelch;
- their place, counted. The listener count and "busy" include held sessions. "Ask before tuning" does not: nobody can
  ask a phone that is suspended;
- their turn clock. It keeps running through the hold, so a hold never buys extra time.

**How long.** `kSessionHoldS` = 180 s, capped by the owner's "Let go after" (idle grace): 30 s or 1 minute holds that
long, and *Immediately* (0) holds nothing, which is the old quick release. The hold is cut short:
- **at a hard limit**: at the session time limit (and never started for a session already at it), or at the idle limit
  on a per-VFO radio. When the limit ends a hold, the turn is spent and the address cools down, exactly as
  `enforceSessionLimit` does to a connected listener. Coming back is met by the same cooldown and borrowed-time rules;
- **for a session ended for a reason**: the time limit, a kick, a ban, an eviction, the idle check, or `{"type":"leave"}`
  (the app sends it when you leave a server; the web page sends it on `pagehide` when the tab really goes). These are
  never held;
- **when it would turn somebody away**: on a one-listener radio, anybody else arriving takes it at once. On a full shared
  radio the oldest held place is released for them. On a shared dial a newcomer simply has the dial; the held listener
  rejoins as a listener of wherever the radio then is, by the server's decision and never by a client tune.

**Coming back.** The same session id carries straight on. Nothing is rebuilt, nothing is landed ("back from its hold —
landing skipped"), and the map never closed. After the hold, the old path applies: the mode is left at once (recorded),
and `mode_resume` may still give it back to a listener who returns to an untouched radio. The app then says the session
ended.

**Nothing missed, mode by mode.** The modes keep running through the hold. What the returning app or page gets:

| Mode | Kept on the server | On return |
|---|---|---|
| ADS-B | the whole table, messages counted, trails per aircraft | full table in the next report; trails fetched on demand (unchanged) |
| NDB | heard list, stats, the 10-minute and hourly cards | full report; `ndb_stats` as before |
| ISM (incl. wM-Bus meters) | the full sensor and meter list with history | the full list (every report is whole) |
| AIS | ships and tracks | full report; `ais_trail` on demand |
| CW / NAVTEX / RTTY / PSK / JTTY / DSC / APRS text, CW and PSK skimmer lists | **the gap log** (new): the last ~384 KB / 4000 messages of output | replayed after the re-attach, paced, then live |
| Digital spots (Single and Band) | the gap log, spots kind | replayed after the re-subscribe |
| SSTV / WEFAX | the picture in progress (`replay_`, as before) | the picture so far. **A picture completed and a new one started during the hold is only in the gallery** (when the owner keeps listeners' decodes) |

The gap log (`DecoderHost` in vibe_decoder_host.h) stamps every text frame, decoder JSON and spot with a sequence number.
The shim notes the number when a session's decoder socket closes, and replays what came after it once that session
re-attaches the same decoder (`Joined`) or re-subscribes spots. A new run of a decoder clears its own entries. A
decoder socket closing while its decoder still runs is no longer an immediate stop. If the owner is held, or is still
connected (the three sockets of a suspension close in any order), it is kept, and the house tick stops it once the
session is gone without a hold, or after 30 s if the session stays but its decoder socket does not return.

**The notice (iOS app).** It is sent only in the quiet modes, because iOS does not suspend an app with live audio. Stuart:
*"if audio playing the app will happily stay all day in the background so nobody ever sees that notification"*. It is
posted 4 s after going to the background. Native code checks once more as it posts: a running engine that is not quiet,
or the silent keep-alive, means no note. Coming back cancels it, pending or shown.

> **VibeSDR paused**
> iOS pauses apps that aren't playing sound. Your place on *Pi 500* is held for 3 minutes and its decoders keep running,
> so nothing is missed. Come back by 14:03 to carry on.

If a hard limit comes first: *"Your session on Pi 500 reaches its time limit at 14:01, so it is held until then, with
its decoders still running. Come back before 14:01 to carry on."* The terms come from hwinfo: `holdS`, and `holdHardS`
(seconds to a hard limit, −1 = none; a soft limit or borrowed time is not one).

Permission is asked once, the first time it matters: a quiet mode on a server that holds sessions, after a line of our
own ("Know when your place is held"). It is never asked at launch. If it is refused, nothing else changes. Tapping the
note brings a suspended app back where it was. If iOS had ended the app, the tap opens that server (the note carries
its `vibesdr://` link).

**A closed squelch is not quiet.** Stuart: *"if a user has a signal in squelch mode waiting to hear an incoming we must
not allow the OS to kill the app in the background, that is the one time where silence is justified".* The server sends
a squelched listener silence packets in step with the stream (`silencePending_`), manual or AUTO, VFO A or B. The engine
plays them, so the session stays live. The only things that release the session for silence are the four map modes
(`audioQuietFor`). The test reads SDRScreen's rule and the iOS `setSquelchOpen` to keep it that way.

**Android** does not suspend the app in these modes. Its `mediaPlayback` foreground service (VibeStreamService) keeps the
process and its audio socket alive; foreground-service apps keep their network in Doze, and Android has no `setQuiet`.
So no notice is shown there. The server hold still covers a dropped link.

**The web page on a phone** cannot post a notice. A backgrounded tab's sockets go and the hold keeps everything. The same
page coming back (same session id) carries on, its decoders' gaps replayed. A discarded tab that reloads is a new
session.

Tests: `scripts/test-server-session-hold.mjs` (real server, about 8 minutes, `VIBESERVER_SLOW=1`):
- ADS-B held 180 s; back at 100 s, still in ADS-B, nothing re-entered, messages counted in the gap; the hold running out
  at 180 s, the mode left, then `mode_resume`;
- a full radio and a one-listener radio giving way; `leave` never held;
- the CW skimmer, dial and VFO B through a hold, with the gap replayed;
- a 1-minute limit cutting the hold, released at the limit, and refused as normal on return.

`scripts/test_sessionHoldNotice.ts` covers the notice decision, its words and the squelch rule.

## NAVTEX: which station sent it (2026-10-22, beta 4)

A NAVTEX header names its transmitter by one letter (`ZCZC BA10` is station B), and a dozen stations round the world
share each letter. The gallery, the web client's NAVTEX box and the app's now say **"B · Oostende, Belgium 🇧🇪 · Nav
warning · #10"** when the station can be named, and "Station B" as before when it cannot.

- **The table** is `scripts/navtex-stations/navtex-stations.json`, parsed by `scripts/navtex-stations/parse-pub117.py`
  from **NGA Pub. 117, Radio Navigational Aids, Chapter 3 (300C.1 / 300C.2 NAVTEX Stations)** — a US Government work,
  public domain; 2021 edition (PDF dated 2021-03-11), fetched 2026-10-22 from msi.nga.mil. The Admiralty List of Radio
  Signals is copyright and is not used. 159 stations, 220 transmitters; one entry (Hamburg's 518 kHz S) has no frequency
  in the source and is left out; Pub. 117 lists **no HF (4209.5 kHz) letters**. The parser also checks every letter
  against its broadcast-time slot and reports the source's own irregularities on stderr.
- **The supplementary list (2026-10-23)** — `SUPPLEMENT` in the generator, each entry with its evidence: stations,
  letters and frequencies missing from the 2021 edition. Started by **Mondolfo** (Italy, E on 490): a real Italian
  warning for platforms off Ravenna, heading itself "MONDOLFO RADIO", was labelled "E · Corsen, France" because
  Pub. 117 knows no other E on 490 near Europe. The rest come from Wikipedia's
  [List of Navtex stations](https://en.wikipedia.org/wiki/List_of_Navtex_stations) (revision 1371431375,
  2026-08-26 — facts only; its text is CC BY-SA and is not copied): Italy's newer network (Mondolfo U 518, La
  Maddalena R 518 / I 490, Sellia Marina V 518 / W 490, Piombino N 490), Corsen A 518, La Garde W 518, Hamburg S 518
  (Pub. 117 gave no frequency), **Jeløy M 518** (Norway — this is the Lenovo's "tonight's M"), Greece's 490 service
  (Kerkyra P, Irakleio Q, Limnos R), Chile's 490 service, and six **HF 4209.5** letters (Istanbul M, Ismailia X,
  Irakleio S, New Orleans G, Shanghai Q, Hai Phong W). Supplements are ADDED beside Pub. 117, never instead: where the
  two disagree (Vardø, Tallinn, Kerch, İzmir, Malta, Murmansk, Arkhangelsk, Talcahuano, two Korean 490 letters)
  Pub. 117 is kept and the disagreement is written in the generator. Two Pub. 117 positions are corrected there too
  (Saudanes was in the Norwegian Sea, Limnos south of Crete).
- **One rule, two readers:** `node scripts/gen-navtex-stations.mjs` writes `src/utils/navtexStations.generated.ts`
  (app + browser) and `android/app/src/main/cpp/vibe_navtex_stations.h` (gallery titles); `--check` in run-tests.sh.
- **The rule — a likelihood, not a cut-off** (owner-approved, so genuine DX gets named). Every station with the letter on
  the band (±3 kHz of the dial) is scored from the message: **positions in the text** (all the NAVTEX formats; a lost
  `_` or an impossible value is ignored) by the station's median distance to them — the strongest evidence; if no
  candidate is within 1000 km of its warning's positions, none of them sent it; **its name in the text** (same letter
  and band) — decisive; **language** by stopwords (×4 on 490, ×1.5 elsewhere); and **distance from this server** as a
  soft prior, 1/(1+(d/800 km)²). The best at ≥3× the next is named, with "· DX 6,800 km" past 1500 km (MF) / 2500 km
  (HF); two close → "W · Corsen 🇫🇷 or Valentia 🇮🇪?"; no evidence and nothing within 1500 / 2500 km → the letter only.
  The weights and word lists are written into both readers by the generator.
- **A station the table does not have (2026-10-23).** When a message names itself in its first lines ("MONDOLFO
  RADIO", "OOSTENDERADIO", "… NAVTEX") and that name is none of the candidates, AND the message rules every candidate
  out — positions that fit none of them, or, with no positions, a language none of them speaks — no table station is
  named. The label is the message's own name, with a country where one can soundly be said: a station of that name on
  another letter or band gives its own; else the country of the table station nearest the positions (≤ 400 km, and
  speaking the text's language if it has one); else a language spoken in only one NAVTEX country (Italian, German,
  Swedish, Polish); else none — "T · Ancona Radio, Italy 🇮🇹?", "T · Widget Radio?". Positions that fit a candidate
  always keep it, and language alone never overrides the table: **Niton's T on 490 sends French** ("ORIGINE
  METEO-FRANCE") and stays "T · Niton, United Kingdom". The gallery shows this label too (never the "or" pair).
  The position reader also takes an `A` (or `-`) between the two halves: in ITA2 a figures-shift `-` received in
  letters shift prints as `A` ("44 31.4N A 012 30.6E").
- **The Lenovo's evening, through the real texts** (IO92nh): 490 A Malin Head, B Oostende, L Hamburg, U Cullercoats,
  F letter only; 518 G Cullercoats, I Varberg, J Stockholm, L Rogaland, N Orlandet, O Portpatrick, P Den Helder,
  V Oostende, M **Jeløy** (a Swedish warning with Skagerrak positions — the station was newer than the 2021 edition;
  with no text it is "Jeløy or Casablanca?"), W "Corsen or Valentia?" unless the message has positions.
- **Pub. 117 disagrees with itself in places:** the 300C tables (used here) and the per-country sections differ for a
  few stations — Oostende's 518 letters are V and T in the table, M and T in the Belgium section; Corsen is 518 W in the
  table, A in the France section. The table is used throughout; a later edition may settle it. Pub. 117 lists no NAVTEX
  for Jamaica (its section has VHF Ch. 13 and 8291 kHz voice only); the Caribbean stations it has are Isabella (Puerto
  Rico, R 518), Miami (A 518), New Orleans (G 518) and Curaçao (H 518).

## Not wanted

PSKReporter (parked: "not really designed as a PSKReporting service").

## Tests

| Suite | Checks | What it covers |
|---|---|---|
| `vibeserver/test-bgdecode.cpp` | 200 | Config, validation and the clock; LSB inversion (RTTY and NAVTEX); the NAVTEX cutter; the store (eviction, path safety); a listener tap; Nick's profile end to end on synthetic IQ (USB **and** LSB VFOs file the same message); WEFAX/SSTV complete, partial and noise; PNG; the FT8 summary — the last-10 card (one card, a minute's cadence, the window rolling, moved to the top, adopted by the next run) and the hourly cards (partial and whole hours) on a test clock; the NAVTEX station rule. |
| `vibeserver/test-config-radios.cpp` | — | `bgDecode` and the gallery fields round-trip, and nothing leaks into the radio's own fields. |
| `scripts/test-bg-decode.mjs` | 43 | A real server on fake-rtl-tcp: wait → move, profile boundary, arrival → stop + dial restored, a decoder socket on its own, gallery HTTP and path traversal, config API round-trip and refusals (a Band VFO on no band, or outside the window — 2026-10-27), the save applied live. |
| `scripts/test-bg-decode-frontdoor.mjs` | 12 | Front door + a radio process: a save through the door reaches the radio, no park/release once due, an unrelated save restarts nothing. |
| `scripts/test-setup-bgdecode.mjs` | 20 | The setup page's real editor against a DOM stand-in. |
| `scripts/test_navtex.ts` | 187 | The NAVTEX cutter (fades, gap markers) and the station rule — the Lenovo's letters, positions, DX, "or"; Mondolfo, a self-named station the table lacks, Niton's French T. (beta 4, 2026-10-23) |
| `scripts/test-bg-decode-admin.mjs` | 15 | The admin line end to end: every state, measured CPU per VFO and for the channels, FT8's live count, nothing without the admin password. (beta 4) |
| `scripts/test_admin_bg.ts` | 17 | The admin line's wording. (beta 4) |
| `scripts/test-web-gallery-pages.mjs` | 20 | Pages through the real server and the real built page in headless Edge: totals per filter, the old request unchanged, the pager, filter reset, BACK to the same page/scroll/row. (beta 4) |
| `scripts/test_gallery_pager.ts` | 13 | The pager's numbering (one rule for web and app). (beta 4) |
| `vibeserver/test-ism-gallery.cpp` | 56 | ISM's day log and summary card, an ISM profile's config (2026-10-25). |
| `scripts/test-server-ism-gallery.mjs` | 33 | ISM in the gallery and an ISM profile through the real server (2026-10-25). |
| `scripts/test-setup-bg-ism.mjs` | 18 | The setup editor's ISM profile (2026-10-25). |
| `vibeserver/test-bg-band.cpp` | 28 | A Band digital-spots VFO: the config and the save's refusals, 40 m end to end on synthetic IQ through BgRunner (two FT8 stations → "Digital spots summary · Band 40 m", Japan furthest), the admin line, a quiet band all idle, switched off here (paused with why, no slot, NAVTEX beside it runs), the CPU at 2.4 MS/s. (2026-10-27) |
| `vibeserver/test-ndb.cpp` (summary cards) | +10 | NDB's last-10 card (once a minute, only on change, one standing card, adopted by the next run) and hourly cards (partial and whole). (2026-10-27) |
| `scripts/test-setup-bg-band.mjs` | 23 | The editor's Band VFO: offered only where it can run, a band picker (only the bands the capture holds — 160 m at 1200 kHz / 3 MS/s) in place of the frequency, the demodulator greyed out, an older row's frequency loading into its band, red where it cannot, saved and read back. (2026-10-27) |
| `scripts/test-setup-bg-ndb.mjs` | 31 | The editor's NDB skimmer VFO: offered only where the radio reaches LF and once a profile, frequency and demodulator greyed out, its line following the RF centre, Stuart's Lenovo profile (NAVTEX ×2 + 160 m Band + NDB) round-tripped, an older `"ndb":true` profile converted on load, no NDB in "Decodes". (2026-10-27) |
| `vibeserver/test-bg-ndb.cpp` | 37 | The NDB skimmer VFO: the config (what it covers, the save's refusals in words, the Lenovo profile, the old profile converted), end to end — synthetic WOD + CAM at 1.024 MS/s centred 700 kHz through BgRunner's slice → the NDB service → the gallery's cards, NAVTEX beside it, nothing dropped; one slot (NAVTEX first, NDB waits), a listener's NDB mode pausing it with the reason, the radio moved off the band and back. (2026-10-27) |
| `scripts/test_gallery_translate.ts` | 27 | TRANSLATE's rule: the lines sent and kept, the Mondolfo text as Italian, Niton's French, the station hint, no key in the target language, the layout put back. (2026-10-23) |

The live test uses two test-only environment variables:
- `VIBESERVER_BG_WAIT_SEC` overrides the owner's wait;
- `VIBESERVER_BG_TEST_CLOCK` names a file holding `HH:MM` that stands in for the UTC clock.

## Not done yet

- **The Android server** (VibeServerBoot / VibeServer Lite) does not pass `bgDecode` or a gallery directory yet, so on
  Android there is no background decoding and no gallery. The shim is ready for it: a JNI setter and the app's server
  screen are all that is missing.
- **SSTV on LSB** (the 80 m/40 m convention) needs the audio mirrored in frequency before the decoder sees it.
- **The FT8 summary**: a crash loses the hour in progress (its card is written when the hour closes or the run ends);
  the last-10 card is at most a minute behind.
- **The cross-radio slot yield** is covered by its code paths, but the live test runs a single process.
