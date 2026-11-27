# METEOR LRPT: the live preview's CHANNEL PICKER, and NIGHT passes (beta96, 2026-11-26)

Branch `b96-lrptchan` (off `b96-integrate` fe9bae62a).

Stuart (11-26, a screenshot of NickB's Airspy HF+ server, Meteor-M2-3, "channels 1, 2, 4 · 682 frames"): *"can we have the
option to change the meteor image being viewed in the preview as one of the channels is this grain but the other channels
have images"*. The coordinator found why: it was a NIGHT pass (19:43 / 20:00 UTC, SNR 16 dB, 1254 frames — a clean decode).
MSU-MR channels 1 and 2 see reflected sunlight, so at night they hold only the sensor's noise, and the preview's composite
(R = G = ch 2, B = ch 1, each stretched 2nd…98th percentile) blew that noise up into blue/yellow speckle. Channel 4 (MWIR,
infrared) of the same pass, in the gallery, was an excellent picture (Italy, Sardinia, Corsica, the Adriatic; outline on).

## What was built

| Where | What |
|---|---|
| `vibe_lrpt_run.h` `kChannelRowOp` (0x09) | Beside each 0x01 preview row: `0x09 \| u32 y \| u32 w \| u8 rApid \| u8 bApid \| u8 n \| n × (u8 apid \| w grey)`. rApid / bApid say what the 0x01 row carries (R = G, B; equal for a one-channel grey row); the n channels are the live ones it does NOT carry, stretched by their own levels (IR inverted, as the gallery), missing blocks hatched / a gap's label lit exactly as the 0x01 row. Sent right after its own 0x01. |
| `vibe_lrpt_run.h` preview | Each live channel's levels taken ONCE per preview (it already took the composite's two every second); they also give `dark_`. |
| `vibe_lrpt.h` `kDarkHi`, `channelDark` | A visible channel (1–3) is DARK when its 98th percentile raw value is under 32. By day the brightest 2 % of a swath (cloud, land, glint) is far above it; at night it is the noise floor. No orbit, no clock: it measures what makes the speckle. A pass with > 2 % of its blocks lit is not dark (its composite shows the lit part). |
| `lrpt_status` | `"apids":[64,65,67]` and `"dark":[64,65]` (only the live visible ones). |
| gallery (`renderPictures`) | Unchanged products: every channel + the composite. A night composite is NAMED "RGB 221 false colour · night (visible channels dark)". Order unchanged: the composite is filed first, the channels after, so in the gallery's newest-first list channel 4 (filed last) is the TOP item — there is no separate "headline"/thumbnail choice in the gallery (each item is its own row with its own thumbnail; checked web main.ts gallery, app GalleryOverlay, SATELLITE watch view = GALLERY opens the same list). |
| `src/utils/lrptChannels.ts` (app + web) | 0x09 parse (bounded), `LrptRows` (rows kept per channel: the 0x01's R and B planes + the extras), `resolveLrptView` (AUTO rule), `lrptChoices`, `nextLrptPick`, labels/hints, stored pick (`vibe.lrptChannel`: auto / comp / ch1…ch6). |
| web | `decoders.ts` 0x09 → `onLrptChanRow`; `main.ts`: rows kept per channel, the live canvas holds the channel SHOWN (so SAVE, the squashed gap bands, north-up and the MAP OUTLINE all follow it); a change repaints the whole pass from the kept rows (never via drawDecLine — a line going back there is a new picture). Key `#decChan` in `#decOutlineBar` over the picture, beside MAP OUTLINE; localStorage in try/catch. |
| app | `DecoderClient` (the primary client — the one with the outline store) keeps the rows and feeds the picture the channel shown; `onImageRepaint` → `DecoderImageCanvas.repaintRgb` (same picture, PREV untouched). Key state + the pick (AsyncStorage) in `lrptOutlineLive.ts`; the key is a `DecoderKey` in the SATELLITE box's outline row and an `HBtn` in the decoder box header — both pad-reachable (test_nav_coverage unchanged). |

### The picker
AUTO (default) · COMPOSITE (only when ch 1 and ch 2 are both received) · each received channel. The key cycles in that order
(one key: pad- and keyboard-friendly, like MAP OUTLINE). It appears only when this server sends channels (0x09) and the
pass offers a choice (≥ 2 views besides AUTO) — no dead key on an older server or a one-channel pass.
- **AUTO = composite by day, infrared at night** (ch 4, else 5, 6 — the first lit IR). Without a composite on the pass: the
  infrared at night, else the first channel. The key says which: `AUTO · COMPOSITE`, `AUTO · CH 4 IR (NIGHT)`.
- **Why AUTO and not "composite" as the default:** with AUTO the night case — the one that made Stuart ask — is right
  with no tap, and by day nothing changes. A manual pick is obeyed even at night (CH 2 on a night pass shows its noise).
- A remembered pick this pass cannot show (CH 5 on a 1-2-4 pass): AUTO for now, said in the key, the pick kept.

### Not changed
The server's decode, its 0x01 rows (an older app sees exactly beta95/96's composite), the gallery's files, the outline
geometry. VFO B's LRPT picture (the SATELLITE box's second picture) stays the composite (it has no outline store either).

## Cost (measured, this Mac — M5 Pro)
- **Bandwidth:** on a 1-2-4 pass only ch 4 is extra: 405 bytes beside each 1185-byte 0x01 row = **+34 %** (test-lrpt and
  the headless web test both measure it). The preview runs ≈ 1.5 rows/s → ≈ +0.6 kB/s per viewer. The joiner's replay
  copy (≤ 4 MB cap) holds a full 1600-row pass with the extras (≈ 2.6 MB).
- **CPU:** one more `levels()` per preview second (the composite's two were already taken): 0.31 ms for a 1000-line channel
  on this Mac (bench of `Channel::levels`); the extra row bytes are 392 lookups a row. On a Pi 2 expect ~10–15× that: a few
  ms a second, on the decode thread.
- **Client memory:** ≈ 392 bytes per channel per row kept: ≈ 0.3 MB for a typical 250-row pass, ≈ 1.9 MB for a full 1600-row
  three-channel one.

## Tests (this Mac, silent)
- `test-lrpt --quick` 104/0 (single-file c++ -O2, nice 10): `testChannelPicker` — a Meteor-M2-3-shaped pass (1, 2, 4) through
  Run by DAY and NIGHT (ch 1 + 2 sent as dark noise, `TxConfig::dark`): every 0x01 has its 0x09 right after it, shaped
  65/64 + ch 4; ch 4 carries a picture both times; status apids; dark `[]` by day, `[64,65]` at night; every product filed;
  the composite night-named only at night; ch 4 filed last; extras ≤ 40 % of the preview bytes (34 %). The live-pass gap case:
  the extra channel's gap rows are hatched/labelled too. The dark rule: visible noise dark, thermal never, 6 % cloud lit.
- `npx tsx scripts/test_lrpt_app.ts` 77 ok (+21): parse/junk, rows per channel, composite = as sent, CH 1 = B, CH 2 = R,
  CH 4 = extra, a row whose 0x09 has not come is composite-only, hatch for a missing channel, AUTO day → composite / night →
  CH 4 IR, 1-2-5 night → CH 5, no composite, manual picks at night, a pick the pass lacks, cycle order, stored pick, status.
- `scripts/test-web-lrpt-channels.mjs` (headless Edge, --mute-audio, a real vibeserver, fake rtl_tcp replaying a synthetic
  NIGHT clip from `test-lrpt --write-clip … 960000 night 48`) **19/0**: the wire (every 0x01 followed by its 0x09, 65/64 + ch 4,
  +34 % bytes); the key appears and reads `AUTO · CH 4 IR (NIGHT)`; the picture is grey with a picture in it (ch 4); the key
  cycles COMPOSITE (70 % coloured speckle) → CH 1 → CH 2 → CH 4 IR; the pick in localStorage survives a reload; back round to
  AUTO; 390 px no sideways scroll; no exception. LRPT has had no DECODERS key since SATELLITE took it over, so the test
  re-adds one IN THE TEST PAGE ONLY (the same 'lrpt' AUTO attach a SATELLITE pick sends). It found one real fault, fixed:
  while the picture was only a few rows tall the wrapper clipped the keys over it (MAP OUTLINE too) — `#decImgWrap` now has a
  44 px floor while that bar shows. Run: `VIBESERVER_BIN=… VIBE_LRPT_CLIP=…/lrpt-night.cu8 node scripts/test-web-lrpt-channels.mjs`
  (not in run-tests.sh yet — it needs the clip; the coordinator may add it as test-web-hell is).
- `test_nav_coverage` ok; `tsc --noEmit` clean. The app's key and repaint were NOT run on a device or simulator (type-checked and
  unit-tested only).

## RESUME HERE
- On a real NIGHT pass (NickB's HF+ evenings, or any pass after dark): the key should read `AUTO · CH 4 IR (NIGHT)` and show
  channel 4; the gallery's composite named "night (visible channels dark)". On a DAY pass: `AUTO · COMPOSITE`, nothing changed.
- The kDarkHi = 32 threshold is from MSU-MR's behaviour (night visible counts ≈ 0–6) and the synthetic test, NOT from a raw
  night capture — check a real night pass's ch 1/2 98th percentile (server log idea below) and a dawn/dusk pass.
- Ideas, not built: (1) the composite could DROP a dark channel automatically (e.g. ch 2 lit, ch 1 dark at dusk → a grey ch 2
  instead of a half-speckled composite) — AUTO's "any composite channel dark → IR" already covers night; a per-channel mix
  rule needs real twilight passes to tune. (2) Skip saving the night composite (it is incompressible noise, ~4 MB a pass) —
  kept for now (every product saved; Stuart's call). (3) Log each channel's 98th percentile in the pass summary to tune
  kDarkHi on real passes.
