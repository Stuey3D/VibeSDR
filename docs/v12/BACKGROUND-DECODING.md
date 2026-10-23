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
| `scripts/test-bg-decode.mjs` | 41 | A real server on fake-rtl-tcp: wait → move, profile boundary, arrival → stop + dial restored, a decoder socket on its own, gallery HTTP and path traversal, config API round-trip and refusals, the save applied live. |
| `scripts/test-bg-decode-frontdoor.mjs` | 12 | Front door + a radio process: a save through the door reaches the radio, no park/release once due, an unrelated save restarts nothing. |
| `scripts/test-setup-bgdecode.mjs` | 20 | The setup page's real editor against a DOM stand-in. |
| `scripts/test_navtex.ts` | 187 | The NAVTEX cutter (fades, gap markers) and the station rule — the Lenovo's letters, positions, DX, "or"; Mondolfo, a self-named station the table lacks, Niton's French T. (beta 4, 2026-10-23) |
| `scripts/test-bg-decode-admin.mjs` | 15 | The admin line end to end: every state, measured CPU per VFO and for the channels, FT8's live count, nothing without the admin password. (beta 4) |
| `scripts/test_admin_bg.ts` | 17 | The admin line's wording. (beta 4) |
| `scripts/test-web-gallery-pages.mjs` | 20 | Pages through the real server and the real built page in headless Edge: totals per filter, the old request unchanged, the pager, filter reset, BACK to the same page/scroll/row. (beta 4) |
| `scripts/test_gallery_pager.ts` | 13 | The pager's numbering (one rule for web and app). (beta 4) |
| `scripts/test_gallery_translate.ts` | 26 | TRANSLATE's rule: the lines sent and kept, the Mondolfo text as Italian, Niton's French, the station hint, no key in the target language, the layout put back. (2026-10-23) |

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
