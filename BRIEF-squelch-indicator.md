# BRIEF: Squelch indicator + watch squelch controls (all surfaces)

**Origin:** Stuart got bitten on a live VibeServer demo — audio kept cutting out and he thought
the network was failing, until he realised **squelch was active with NO on-screen indication**.
Silent muting with no visible cause reads as "the app is broken." Fix: make squelch **visible and
obviously active** everywhere.

## The indicator (agreed design — Stuart 2026-07-22)
On EVERY surface — phone (main app), VibeSDR Jr, VibeSDR Buddy, and the web interface:
1. **A breathing threshold LINE on the SNR/signal bar** at the level squelch is set to. It
   **breathes** (opacity/glow pulse) when squelch is **CLOSED** (signal below threshold → audio
   muted right now), and **dims to steady** when **OPEN** (audio passing). Breathing = "I am muting
   you this second." Unit-agnostic: just position the line on that backend's own meter scale.
2. **A compact red "SQL" label** near the readout, shown only when squelch is engaged (level ≠ off).
   Small, red, unmistakable.

Together: you can see (a) squelch is on, (b) the level it's set to, (c) whether it's cutting audio
*right now*. FM-DX never shows it (no squelch there).

### ★ Display in the SHOWN meter unit; send in the backend's native unit (Stuart 2026-07-22)
The threshold line's POSITION must track the **currently-displayed** meter unit, not the wire unit.
E.g. Kiwi squelch is dBm/dBFS, but if the meter is set to **S-units**, draw the line at the S-unit
position that corresponds to that dBm — and still send Kiwi the dBm it wants. So: the user sets/reads
squelch in whatever unit the meter shows (S-units / SNR dB / dBFS / dBm per `signalMode`), and we
CONVERT to the backend's native unit for the wire. Same conversion the meter already uses to render
the signal figure — reuse it so the line and the live signal are on the identical scale (that's the
whole point: you judge the gate against the signal you can see). Stuart believes the phone already
does the value-vs-display split this way — verify and reuse it, don't reinvent.

## Squelch backend map (the reference — from the phone, `src/`)
Squelch is BACKEND-DEPENDENT — the indicator draws the line on whatever unit that backend uses:
- **UberSDR / VibeServer** (radiod; `UberSDRClient.ts`): sent over the AUDIO-WS via
  `VibePowerModule.sendAudioCommand` — `set_audio_gate { min_snr }` (SNR gate; +30 dB wire offset,
  `SDRScreen.tsx:3035-3038`) and `set_squelch { squelchOpen }` (FM, `:3041-3043`, re-asserted on
  fm/nfm entry `:3046-3061`). **This SNR gate is what bit the demo on VibeServer.**
- **OWRX** (`OwrxAdapter.ts:201`): native squelch, `squelchLevel` dB (−150 = off), via `dspcontrol
  squelch_level` on `sendDemod()`. Driven by `client.setSquelch(db)` (`SDRScreen.tsx:4691`). OWRX
  profiles can ship a PRESET `initial_squelch_level` that auto-applies on connect (`:713-717`,
  `OwrxAdapter.ts:523-526`) — another silent-mute trap the indicator will now surface.
- **Kiwi**: CLIENT-SIDE dBm gate — `evalKiwiSquelch(dbm)` → `VibePowerModule.setSquelchOpen(open)`
  (`SDRScreen.tsx:1004-1027, 2076`), threshold `kiwiSquelch` dBm (−130 = off). Cleared on unmount
  `:1486-1490`.
- **Local RTL hardware**: `hwSquelch` dBFS → `LocalHw.setSquelch(sql>-100, sql)` (`:394,619-620`).
- **FM-DX**: NONE (`TunerScreen.tsx:829`).
Squelch UI today lives in `MenuSheet.tsx:99` / `AudioSheet.tsx:249,294`. Open/closed state for the
indicator: Uber/OWRX = server-gated (need to derive open/closed from live SNR vs threshold, or a
server signal); Kiwi/local = the app already computes open/closed (`setSquelchOpen`).

## Meter render targets
- Phone: `SDRScreen.tsx` `meterText(signalMode,…)` (`:2250`); three-way unit toggle `signalMode:
  'snr'|'smeter'|'dbfs'` (`:1089`, cycled in `MenuSheet.tsx:138-139,960`; OWRX/Kiwi skip 'snr'
  `:1287`). `src/components/SignalMeter.tsx` EXISTS but is UNUSED — the live meter is inline in
  SDRScreen. `snrToLabel` (S9+xx) lives in both SignalMeter.tsx:12 and `SignalMeter.tsx` logic.
- Jr: readout pill in `spike/WristSDR/WristSDR/ContentView.swift:1885` (`link.meter`); string built
  `SpikeLink.swift:369` `"\(Int(signalDb))dB"`; bar fill `link.level`.
- Buddy: `ios/VibeSDRWatch/ContentView.swift:1890` (`link.meter`, mirrored from phone verbatim).
- Web: the served web client (website/served page) — find its meter.

## Per-surface plan (sequence: phone → Buddy → Jr → web)
1. **Phone (reference)** — squelch already exists; ADD the breathing line + red SQL label to the
   meter. Derive open/closed (Kiwi/local already have it; Uber/OWRX: compare live SNR vs threshold).
   Expose squelch level + open state so Buddy can mirror.
2. **Buddy** — RELAY a squelch level to the phone (phone actions it): new `case 'squelch'` in
   `watchProvider.ts:411-466` → existing `onFmSquelch`/`onSnrSquelch`/Kiwi handlers; watch send in
   `WatchLink.swift` (copy `stopsrv`). Mirror level + open/closed in the state relay; draw the same
   indicator. Buddy = no new DSP.
3. **Jr (standalone)** — LIFT the per-backend squelch (no phone to defer to): Uber/VibeServer send
   `set_audio_gate`/`set_squelch` over Jr's AudioSocket; OWRX `squelch_level` via dspcontrol; Kiwi
   local dBm gate driven by the smeter in the watch audio path; FM-DX none. Add a squelch CROWN
   control + tile. Draw the indicator.
4. **Web** — add the indicator to the served client.

## Related asks bundled with this (Stuart 2026-07-22)
- **Squelch control** on Jr + Buddy (above).
- **SNR meter UNIT toggle** on the watches — phone already has `signalMode` (snr/smeter/dbfs). Jr
  only formats `"…dB"` locally (`SpikeLink.swift:369`) with NO S-unit concept; Buddy mirrors the
  phone's string. Cleanest: Buddy sends a command to flip the phone's `signalMode` (keep mirroring
  the string); Jr needs local unit conversion (knows only `signalDb`, meaning varies by backend).
- **Menu parity** Jr↔Buddy — port Jr's Display menu to Buddy (minus Auto contrast: phone normalises
  before bins reach Buddy). Buddy's WaterfallBuffer ALREADY supports brightness/contrast/peakHold/
  setLUT (byte-identical to Jr) — it's a UI-wiring port. Align tile order/presence; genuine
  exceptions only: Jr-only Bookmarks/Wrist-down/Link-Management, Buddy DAB tile, platform bits.
  Buddy `CrownMode` lacks `.autoContrast` (correct). See [[watch_display_menu_and_airplay]].

## Follow-up found while building: phone S-meter is ZOOM-DEPENDENT
The phone's dBFS/S-meter mode reads the **spectrum passband peak** (`SDRScreen.tsx:2219-2222`), whose
per-bin dBFS scales with zoom/bin-bandwidth — so the S-meter reads lower zoomed out, higher zoomed in.
madpsy's ka9q_ubersdr WEB UI does NOT do this: its S-meter reads radiod's **channel baseband power**
(the same `basebandPower` that, minus `noiseDensity`, gives the SNR already forwarded as `VibeSignal
{snr}`), which is zoom-independent. FIX (separate improvement): forward `basebandPower` from native
(`VibePowerModule` → `VibeSignal`) and use it for the dBFS/S-meter reading instead of the spectrum
peak. That makes the S-meter match the web AND makes an absolute squelch line trivially stable.
WORKAROUND shipped in the squelch line (build 134): position the line RELATIVE to the live signal via
SNR — `sqlNorm = level − (snr − threshold)/90` — so the gate margin (snr−threshold) is correct on the
zoom-reactive bar without needing an absolute floor.

## Do NOT
- Show the indicator on FM-DX (no squelch).
- Let an OWRX profile PRESET squelch mute silently — the indicator must appear for presets too.
