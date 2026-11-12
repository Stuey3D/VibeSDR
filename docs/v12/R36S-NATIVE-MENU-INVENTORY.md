# R36S native UI — inventory of every control in the web handheld UI

Branch `v12-r36s`, written 2026-11-06 from the code. This is a reference to port FROM. Where `docs/PROTOCOL.md` and the
code disagree, the code wins (example: the passband fields are `bandwidthLow` / `bandwidthHigh`, not `low_cut` / `high_cut`).

**How it runs today.** The image starts Cog on `http://127.0.0.1:48000/?handheld=1`. The page asks `GET /handheld/session`
and switches the handheld profile on only when a handheld supervisor answers (`handheld.ts:144-183`). The audio socket is
`/ws/audio?…&proto=1&speaker=1` with no `codec=opus` (`main.ts:1417-1421`). With `speaker=1` the ENGINE plays the audio
on the device and the browser builds no audio chain. The START RADIO gate is skipped (`main.ts:2850-2853`).

**Who answers what.**
- `/handheld/*` HTTP is the supervisor, `vibeserver --handheld` (`vibeserver/handheld.cpp:1137-1179`). Section 7 lists every endpoint.
- Every other HTTP path, plus `/ws/spectrum` and `/ws/audio`, is the radio child, `vibeserver --handheld-radio`. It is
  `LocalSdrShim`: WS control is handled in `android/app/src/main/cpp/local_sdr_shim.cpp`, from the `type == "…"` dispatch
  at ~18325 and ~20383-21330. There is one radio and so no `/r/<id>/` prefix.
- The hardware service `vibe-hwd` is reached ONLY through the supervisor (`docs/v12/R36S-HANDHELD-IMAGE.md` §0.2).

**Conventions in this document.**
- `prefs().x` / `savePref('x', …)` means key `x` inside the localStorage JSON blob `vibesdr_web_prefs_v1`.
- On the handheld that blob is split in two (`handheld.ts:113`, `GLOBAL_KEY`). Keys matching
  `^(palette|ctl|freqUnit|vfoColor|volume|specRatio|specShow|wf(Scroll|Speed|Boost|Data|DataRate)$|lastHost|tapSnap|ptrFreq|font|look|theme|disp|padTuck|wheelTunes)`
  belong to the DEVICE.
- Every other key is that RADIO's memory. It is posted to `POST /handheld/memory` every 2 s when changed, and restored on
  plug-in (section 8).
- `vibesdr_handheld_v1` (localStorage) holds the device's own handheld settings: `uiFps`, `specRate`, `screenOffMin`,
  `perf`, `wf20`.
- **WS** means a JSON text frame on the spectrum socket (`SpectrumClient._send`, `spectrum.ts:1143`).
- **Speaker-mode flag** (✅ / ⚠️ / ❌) says whether a control's effect still happens when the engine, not the browser,
  plays the audio.

## Sections

The forks' sections are inlined:
1. Bottom deck (`#mcard` / `#mMain` / `#mStatus`)
2. Frequency entry panel (`#freqPanel`)
3. AUDIO sheet (`#audioPanel`)
4. MENU: 4a SDR HARDWARE · 4b DISPLAY, GAME CONTROLLER and the rest · 4c R36S HARDWARE and the four handheld DISPLAY rows
5. Bookmarks and Recordings
6. Handheld-only screens and overlays
7. `/handheld/*` endpoint reference
8. Per-radio memory

## Read first: speaker-mode flags and handheld gotchas (collected from every section)

**Speaker mode.** Every DSP control is a WS message that the engine acts on, so it works in speaker mode. This covers
squelch, NR, notch, NB/NBX, de-emphasis, stereo, wsp/ims/ceq/autobw/nb, nfmvoice, and VFO B squelch, mute and balance.
The engine's speaker path takes the audio after all of it (§3.0).

**Volume, mute and recording** are browser features that the page forwards:
- Volume and mute go as `{"type":"speaker","level":0..1}`.
- REC and REC BUFFER go as `{"type":"speaker","tap":1}`. The engine then also streams PCM to the page. The page builds the
  WAV and POSTs it to `/handheld/recordings`.
- A native UI must either keep that tap-and-upload flow or move recording into the engine.

**Browser-only, needs engine or native work:**
- ❌ Playback in the RECORDINGS list is an HTML `<audio>` element. It does not come out of the engine speaker (§5.2).
- ⚠️ AUTO SQUELCH is a tracker that runs in the page. It reads the spectrum floor and sends `squelch` (§3.2). It must be
  ported or moved into the engine.
- ⚠️ VFO A/B `monoOut` is meant to come from the browser's output channels. In speaker mode there is no AudioContext, so it
  never reflects the real speaker or headphones (§2.3, §3.12). Native should derive it from the hwd `output` event.
- ⚠️ There are TWO volumes in series:
  - the AUDIO sheet volume, the engine's linear speaker gain (`speaker.level`)
  - R36S HARDWARE → VOLUME, the ALSA master via hwd (`POST /handheld/device {"set":"volume"}`), which the hardware volume
    keys also move.

**Dead or unreachable on the R36S** (AGENTS.md: do not port dead controls):
- The deck's CHAT (hidden) and SHARE (a link to 127.0.0.1).
- The mode picker's external links: SIGNAL ID WIKI and QRZ.COM.
- The Bookmarks sheet's EXPORT, IMPORT HERE and IMPORT TO SERVER (no downloads, no file picker). The card's FILES row
  replaces them.
- DISPLAY → MOUSE WHEEL; FREQUENCY AT POINTER (no hover pointer); SAVE FOR ALL (vibesdr.net only).
- UNCOMPRESSED AUDIO (already hidden).
- RAW IQ OUT is not hidden but serves another device over the network.

**Bugs found while inventorying** (not fixed: this is a read-only pass):
- REC BUFFER default on a fresh handheld. `main.ts:16787` stores `recbuf` as the STRING `'0'`, but `segment()` (`main.ts:17586`)
  only honours a number. The panel lights the markup default (30 s) while the buffer is actually OFF.
- `handheld.ts:657` hides `#wfDataNote` and `#wfBoostNote`, but those ids do not exist (both are bare `.rowNote` divs).
  Two help texts stay on screen under hidden rows (4b).
- `GLOBAL_KEY` (`handheld.ts:113`) misses most waterfall-look keys, and lists `wheelTunes` where the real key is
  `wheelAction`. Those settings follow the RADIO, not the device, despite the comment (4b, §8).
- R36S HARDWARE → EXPORT BOOKMARKS exports only the browser-local list, never the receiver's list. Meanwhile IMPORT writes
  into the receiver's list. The two do not round-trip (§4c.2).

**Driver strings.** Branch on hwinfo `radio.driver`: `rtl`, `sdrplay`, `airspyhf`, `airspy`, `hackrf`, `usrp`, `icom`.
It is `rtl`, NOT `rtlsdr` (4a).

**Sent without a press, at page build or on a caps message** (4a, §3.0):
- the saved bias-T
- HF+ `att` / `lna` / `thresh` / `ppb` / `agc`
- the RSP's saved LNA, AGC, notches and bias-T
- HackRF LNA and VGA; Airspy mode
- NR (slider applies once at wiring), and every `toggle(…, pushOnInit)` row

Decide deliberately whether native re-asserts these or reads the radio's state.

**The handheld is the owner.** Loopback callers pass `adminGate` when no admin password is set or when not serving on the
LAN, so owner-only rows work: PPM/ppb, bias-T, `vagcmode`, HackRF amp, auto direct sampling, antenna.

## 1. Bottom deck — `#mcard` (index.html:4556-4716, wired in mobile.ts `initMobileControls` and main.ts:9882-9954)

The handheld uses the same single control card as every width.

**Layout, left to right.**
- `#mVfoDial`: ‹ [A/B key] glyph ›
- `#mStackL`: STEP / MENU
- `#mPill`: meter, frequency, mode, stereo, SNR
- `#mStackR`: AUDIO / CHAT, with DEVICE on the handheld
- `#mZoomDial`: − glyph +
- `#mStatus` underneath.

**Hidden on the handheld** (`handheld.ts:244-249` `injectStyle`, plus `.hhHide`):
- `#mShared`, `#linkBars`, `#chatPanel`
- `#mChat`: it gets `.hhHide` and `#mDevice` is inserted right after it (`main.ts:3176-3183`)
- `#srvHealth`, `#srvBattery`, `#secListeners`, `#rawAudioRow`, `#rawAudioNote`
- the admin rows (`#btnAdmin`, `#adminRow`, `#adminPwRow`, `#adminServerRow`) and `#splash`

**Tune and zoom keys tuck away.** While a pad or keyboard drives, `body.padTuck` fades the tune and zoom keys. This
happens when GAME CONTROLLER → TUCK AWAY is ON (default ON, pref `padTuck`, `main.ts:21020-21023`). See 4b.

### 1.1 Tune ‹ › (`#mVfoDown` / `#mVfoUp`, index.html:4561/4570)

| | |
|---|---|
| Type | Button. Tap = one step. Hold ≥ 350 ms = accelerating sweep, 3 → 15 steps/s over 3.5 s, paced on the server's `config` echo (timeout 700 ms) and on RTT (cap 400 ms) (`main.ts:18625-18733` `attachHoldSweep`) |
| Label | `‹` / `›` with the step text drawn ON the arrow (`#mStepCapD/U`, e.g. `1kHz`). In DAB / ISM / Band spots the caps name the previous and next block or band (`main.ts` `syncDialTips` ~11370-11405) |
| Tooltip | `Tune down 1kHz` / `Tune up 1kHz`. In ADS-B: `Locked in ADS-B — the receiver is held on 1090 MHz`. In Band spots: `Previous band — <name>` / `Next band — <name>` / `The lowest band this radio reaches` / `The highest band this radio reaches` |
| Action | `nudge(±step)` (`main.ts:19077-19101`) |

What `nudge` does, in order:
- **DAB:** `dabTune(±1)`, the next or previous multiplex block.
- **ISM:** `ismStep(±1)`, sends WS `{"type":"ism","on":1,"step":1|-1}`.
- **Band spots:** `bandSpotsStep(±1)`.
- **ADS-B / AIS:** nothing.
- **Otherwise:**
  - snap to the step grid via `airbandStepFrom` (the 8.33 kHz walk in the airband) and clamp with `clampTune` (§2.4)
  - send WS `{"type":"tune","frequency":<Hz>,"mode":"<mode>","bandwidthLow":<Hz>,"bandwidthHigh":<Hz>}`, paced by
    TunePacer: latest wins, gap set by server load and RTT (`spectrum.ts:1005-1008`, `2022-2060`)
  - if VFO B is selected: WS `{"type":"watch","id":1,"frequency":…,"bandwidthLow":…,"bandwidthHigh":…}`, at most one every
    40 ms (`spectrum.ts:936-945`)
  - the view follows the VFO (LOCKED) with WS `{"type":"zoom","frequency":<centre Hz>,"binBandwidth":<Hz/bin>[,"free":1]}`

Shim: `tune` at `local_sdr_shim.cpp:20546-20598`. It honours `mode` (refused if blocked) and `frequency`. It ignores
`bandwidthLow/High` when the same tune changed the mode.

### 1.2 VFO A/B key (`#mAbKey`, index.html:4567)

| | |
|---|---|
| Shown | Only while VFO A/B is ON (`spec.abOn`). Shown in place of the radio glyph |
| Label | `A` or `B` (the selected VFO). Tooltip `VFO A/B — which VFO the controls tune. Press to switch.` |
| Action | `spec.swapVfo()` → `selectVfo` (`spectrum.ts:871-882`). Client-side swap of which VFO the controls tune, plus a `zoom` when zoomed in (`viewToVfo`). No dedicated message |

The toggle that creates VFO B is in the frequency panel (§2.3).

### 1.3 Zoom − + (`#mZoomOut` / `#mZoomIn`, index.html:4637/4639)

| | |
|---|---|
| Type | Button with press-and-hold repeat: fires on press, then every 70 ms after 380 ms (`mobile.ts:119-147`) |
| Action | `spec.zoomBy(√2)` in, `spec.zoomBy(1/√2)` out (`mobile.ts:192-193`) → WS `{"type":"zoom","frequency":<view centre>,"binBandwidth":<span/binCount>[,"free":1]}` (`spectrum.ts:2533`) |
| Tooltips | `Zoom out — show more of the band` / `Zoom in — show less of the band, in more detail`. In ADS-B: the ADS-B lock text |
| Held | Ignored by the server in DAB / ADS-B / ISM / NDB / AIS (`local_sdr_shim.cpp:20250-20272`). An Icom quantises to `radio.scope.spans` |

### 1.4 Signal meter pill (`#mPill` > `#mSig`, `#mSqlLine`, `#mDabQ`)

- **`#mSig`:** a gradient bar, width = `sigSmooth` × 100 %. `sigSmooth` is the client's smoothed 0..1 level, painted on
  every spectrum frame (`mobile.ts:577-591`). NOT clickable; the pill itself has no handler.
- **`#mSqlLine`:** the squelch threshold marker. `left = (squelchDb − sqlScaleMin)/(sqlScaleMax − sqlScaleMin)`. Hidden
  when squelch is off (`vsSqlOn(squelchDb)` false) (`main.ts:9908-9914`).
- **`#mDabQ`, DAB only:** the reception meter replaces the gradient. Three bars plus `<label> · <advice> · <detail>`, from
  `src/utils/dabQuality.ts` (`mobile.ts:599-627`). `data-level` drives the bars.
- **`#mStereo`:** two-ring glyph. Lit (`.on`) when the RDS/FM stereo pilot is locked (`syncStereoLight`). Display only.
- **`#mAmSync`, AM-sync modes only:** lock and offset text (`amSyncReadout`, `main.ts:7234-7242`).
- **`#mChan`, airband only:** a small line, e.g. `CH 118.010 · 8.33` or `8.33 · 118.0083`. In ISM it reads `ISM <band>`.
  In Band spots it reads `<name> BAND` (`main.ts:19157-19185`).

### 1.5 Frequency box (`#mFreqBox`, index.html:4606-4608)

| | |
|---|---|
| Type | `span role=button tabindex=0`. Enter / Space click it (`mobile.ts:213-217`) |
| Content | `#mFreqLock` (padlock), `#mFreq` (main digits), `#mFreqFine` (dimmed 10 Hz digits), `#mUnit` (`Hz`/`kHz`/`MHz`), then `#mChan` and `#mAmSync` |
| Readout format | `cardFreqPlain`, `main.ts:19187-19202`, in the unit chosen in §2 (pref `freqUnit`, default `mhz`): |
| | • **MHz:** main `floor(hz/1e3)/1e3` to 3 dp, fine = `floor((hz mod 1000)/10)` as 2 digits |
| | • **kHz:** `(hz/1e3).toFixed(3)` |
| | • **Hz:** an integer |
| | • **Airband in MHz:** the channel NAME |
| | • **ISM:** the band centre in MHz |
| Updates | Repainted at 4 Hz by `refresh()` (`mobile.ts:538-566`) |
| Action | `openFreqEntry()` (`main.ts:19239-19249`) |

`openFreqEntry` cases:
- **DAB:** refused with `showTuneGapMsg('Frequency is locked in DAB mode — there is no dial inside a multiplex. Press EXIT DAB to tune.')`.
- **ADS-B:** `'Frequency is locked in ADS-B — the receiver is held on 1090 MHz. Press EXIT ADS-B to tune.'`
- **Otherwise:** opens `#freqPanel` (§2), fills `#freqInput` with the current frequency in the chosen unit, selects it.

### 1.6 Mode pill (`#mMode`) → mode picker (`mobile.ts:297-521`)

**Label.** `modeLegend(mode)` (`src/services/amSync.ts:63`). An AM-sync id shows its own label (`SAM-U` …); everything
else is upper-cased. While a whole-radio mode is on, the label is `DAB` / `ADS-B` / `ISM` / `AIS` / `NDB`. On an Icom it is
the radio's own name (`main.ts:9900`).

**Picker.** A popup `#mModeMenu`, anchored to `#mPill`, flipped above it if there is no room below. It is a pad navscope.
Back closes it. A tap outside closes it AND is swallowed, so it cannot retune.

Grid `.mModeGrid`, in this order:

| # | Entry | Condition | Selecting it sends |
|---|---|---|---|
| 1 | `WFM` `NFM` `AM` then `SAM` `SAM-U` `SAM-L` `ECSS-U` `ECSS-L` `AM-ST` (only those listed) then `USB` `LSB` `CWU` `CWL` then `DRM` | Fixed list `MODES = ['wfm','nfm','am','usb','lsb','cwu','cwl']` (`main.ts:216`). The AM-sync ids go right after AM, and only those in `vibeserver.json` `modes[]` (`amSyncOffered`, `main.ts:899`, `7227-7232`). `drm` is offered if `vibeserver.json modes` includes `'drm'` (`main.ts:897`). Then minus hwinfo `blocked[]` (`spectrum.ts:1710` → `blockedModes`; a blocked `cw` removes both cwu and cwl, `main.ts:226-231`). Current mode is marked `.on`. Tooltip on AM-sync entries: `AM_SYNC_NAME` text, e.g. `Synchronous AM, upper sideband`, `AM Stereo (C-QUAM compatible)` | `setMode(m)`, see "Sending a mode" below |
| 2 | `DAB` (with icon) | `vibeserver.json` `dab === true` (`main.ts:896`). Title `DAB (Experimental) — digital radio, Band III` | `toggleDabMode()` (`main.ts:13955`). It first leaves any other whole-radio mode. `dabSetMode(on)` (`main.ts:8516-8540`) sends WS `{"type":"dab","on":1,"channel":<block index>}`. If the radio is already on a multiplex it sends nothing and only joins. Off: `{"type":"dab","on":0}` |
| 3 | `ADS-B` | `vibeserver.json` `adsb === true` or `modes` includes `adsb`. If `adsb === false` the cap is `{ok:false, why: adsbWhy}`, and the entry is drawn only while ADS-B is ON (so it can be left), class `dis`. Title `ADS-B — aircraft on 1090 MHz, with a map` / `ADS-B is not available: <why>` (`main.ts:8622-8629`, `mobile.ts:360-369`) | `toggleAdsbMode` → WS `{"type":"adsb","on":1\|0}` (`spectrum.ts:2283-2291`). If not ok: a pill with the reason, nothing sent |
| 4 | `ISM` | `vibeserver.json` `ism` true/false AND `ismBands[]` non-empty (`main.ts:8890-8899`). Refused means not drawn unless on. Title `ISM — weather stations, sensors, doorbells, tyres (the arrows step bands)` | WS `{"type":"ism","on":1[,"band":"<id>"]}` / `{"type":"ism","on":0}` (`spectrum.ts:2318-2327`) |
| 5 | `NDB` | `vibeserver.json` `ndb` true/false (`main.ts:9001-9004`). Same rule. Title `NDB — every aeronautical beacon on 190–535 kHz decoded at once, with a map` | WS `{"type":"ndb","on":1\|0}` |
| 6 | `AIS` | `vibeserver.json` `ais` true/false (`main.ts:9450-9455`). Same rule. Title `AIS — ships on 161.975 and 162.025 MHz, with a map` | WS `{"type":"ais","on":1\|0}` |
| 7 | `DECODERS…` or the sole decoder | `anyDecoderLeft()` (`main.ts:13944-13951`): any of rtty, navtex, wefax, sstv, time, rds, ft8 or spots not in `blocked`, or a server-capped one (cw, psk, aprs, dsc, sitora, same, selcal, MFSK families, hell, jtty) present and not blocked. If `spots` is blocked and exactly one `#decodersPanel [data-dec]` key is left, THAT key is drawn here and toggles directly (`soleDecoder`, `main.ts:13978-13985`) | Opens `#decodersPanel` (out of scope for this inventory: a whole panel of its own) |

Under the grid:
- **`BANDWIDTH` heading and `.bwRow`**, moved in from `#bwHome` (index.html:4704-4715). Hidden in DRM (`syncBw`,
  `main.ts:18521-18549`). An Icom draws its `FIL1/2/3` row (`icomFilRow`) in its place via `modeExtra`.
  - **`#bwLo`, `#bwHi`:** sliders holding the edge MAGNITUDE. Per-edge max is `BW_EDGE_MAX` (`main.ts:18478-18486`):
    usb/lsb/ecss 6000, am/sam/sau/sal/amst 20000, cw 2000, fm/nfm 30000, wfm 250000. Slider step is 1000 if max > 50 k,
    100 if > 10 k, else 10. WFM's min per edge is 20000 (`WFM_MIN_BW_HZ/2`).
  - **Labels `#bwLoVal` / `#bwHiVal`:** `−5.0k` / `+5.0k` (`edgeLabel`).
  - **A drag** sends WS `{"type":"bandwidth","bandwidthLow":<signed Hz>,"bandwidthHigh":<Hz>}` (`spectrum.ts:2104-2119`,
    clamped by `clampPassband`). Shim at `local_sdr_shim.cpp:20617-20630`. A drag also switches AUTO off (below).
  - **`SYNC`** (`#bwSync`, tooltip `Mirror both edges`): a toggle, pref `bwSync`. Default ON for am/sam/sau/sal/amst/dsb/
    fm/nfm/wfm, else OFF. Turning it on mirrors at once to the wider edge (`main.ts:18571-18590`).
  - **`AUTO`** (`#bwAuto`): shown only if hwinfo `autoBwModes` lists the current mode. Sends WS `{"type":"autobw","on":true|false}`.
    While on, the sliders SHOW the server's chosen edges. Tooltip verbatim: `AUTO bandwidth — the receiver fits the passband
    to the signal by itself (SSB: to the voice that is there; CW: round the keyed tone; AM: away from a neighbour; FM:
    narrower for a weak station). The sliders show what it chose. Move a slider and AUTO switches off: your width wins.`
    The AUDIO sheet has a twin, AUTO BW (§3).
- **`SIGNAL ID WIKI ↗`:** a link to `SIGID_HOME`. **`QRZ.COM ↗`** and **`CW TERMS GUIDE`** (opens `cwGuide.ts`), then
  the `QRZ_SIGN_IN_NOTE` line. Links in a new tab are probably dead in Cog. Flag for native: drop them, or keep the CW guide only.
- **`WHAT IS THE DC SPIKE?`:** shown only while the red DC-spike label is in view (`main.ts:9939-9942`). It expands
  `DC_SPIKE_EXPLAINED` (`mobile.ts:26-30`, verbatim there).
- **`CLOSE`.**

**Sending a mode** (`main.ts:9925-9932` → `setMode(m, true)` `main.ts:18444-18467` → `spec.setMode`, `spectrum.ts:2085-2101`):
- If ADS-B / ISM / AIS is on, the pick is parked (`adsbPendingMode`) and the mode is left first. If DAB is on,
  `dabSetMode(false)` first.
- Then two WS frames, in this order, because the shim ignores bandwidth on a mode-changing tune:
  1. `{"type":"tune","frequency":<Hz>,"mode":"<m>","modeSet":1}`
  2. `{"type":"bandwidth","bandwidthLow":<MODE_BANDWIDTHS[m][0]>,"bandwidthHigh":<[1]>}`
- `MODE_BANDWIDTHS` (`spectrum.ts:47-56`): usb [50,2700], lsb [−2700,−50], am/sam [−5000,5000], cwu/cwl [−200,200],
  fm [−6000,6000], nfm [−5000,5000], wfm [−100000,100000], drm [−5000,5000], sam/sau/sal/amst [−5000,5000],
  ecssu [50,4500], ecssl [−4500,−50].
- With VFO B selected, it is a `watch` message instead.
- The airband default passband may follow (`airbandFollowPassband`).
- Mode is NOT a step change: steps follow the BAND, not the mode (§1.8).

### 1.7 SNR chip (`#mSnr`) → reading menu (`mobile.ts:228-276`, `577-591`)

| | |
|---|---|
| Shows | `SNR 23 dB` / `S7` (the `toSUnit(lastSigDb)` string) / `-87 dBFS` |
| Overrides | `SQL` (class `sql`) while the squelch gate is closed (`vsSqlOn(squelchDb) && lastSigDb < squelchDb`). In DAB: `MER 14 dB`, or the verdict's short word. On an Icom: the radio's own reading (`S9+20`) |
| Tooltip | `Signal reading: SNR / S-meter / dBFS` |
| Action | Popup `#mMeterMenu` with `SNR`, `S-meter`, `dBFS`; the current one is highlighted. Stored in localStorage key `vibe.mMeterMode` (`snr`, the default, / `smeter` / `dbfs`). NOT in the prefs blob, so it is device-wide and not per radio. Client-only; nothing is sent |

### 1.8 STEP (`#mStep`, index.html:4586) → step popup (`main.ts:18761-18827`)

| | |
|---|---|
| Label | `formatStep(step)`: `10Hz` … `1kHz` `6.25kHz` `12.5kHz` `8.33kHz` (`main.ts:18597-18605`). Tooltip `Tuning step (now 1kHz) — tap to change` |
| State | `step` (Hz), pref `step` (per radio). Default 1000 (`main.ts:235`). Restored by `buildVfo` (`main.ts:18734-18740`) |
| Ladder | `stepsForFreq(freq, ituRegion())` (`src/services/sdrTypes.ts:58-62`): |
| | • below 30 MHz `STEPS` = 10, 100, 500, 1000, 5000, 9000, 10000 |
| | • from 30 MHz `STEPS_VHF` = 100, 500, 1000, 5000, 6250, 12500, 25000, 50000, 100000 |
| | • airband COM band (`isAirbandCom`) `STEPS_AIRBAND` = 100, 500, 1000, 5000, 6250, 8333 (STEP_833), 12500, 25000, 50000, 100000 |
| | • ITU Region 2 in 87.5-108 MHz: `STEPS_VHF` + `STEP_FM_ODD` (200 kHz odd-tenths) |
| | The ladder can change under you: `syncStep` snaps the step to the nearest rung on a new ladder (`main.ts:18836-18845`) |
| Pick | `setStep(v)`: savePref `step`, the airband passband may follow, the step caps and tooltips are repainted (`main.ts:18742-18750`). Client-only; nothing is sent |
| Default per band | There is no per-mode default. On a JUMP (typed frequency, bookmark or search result, band pick, the first landing), `applyBandStep(hz)` takes `bandTuneDefaults(hz, region).step` from `BAND_PLAN` (`src/constants/bandPlan.ts:235-249`): ham band entries' `step`, e.g. FM broadcast 100 kHz, MW 9/10 kHz, QO-100 500 Hz (`main.ts:11407-11414`). The arrows never change the step |

**SNAP WATERFALL TUNING TO STEP RATE** (`#tapSnapSw`, a `role=switch` at the foot of the popup):
- Label: `SNAP WATERFALL TUNING TO STEP RATE · ON` / `… · OFF`.
- Help line under it, `#tapSnapHint`, same text as the title:
  - ON: `On: clicking or tapping the waterfall to tune snaps to the nearest step rate chosen above.`
  - OFF: `Off: the radio tunes to exactly where you click or tap on the waterfall.`
- Pref `tapSnap` (DEVICE-global per GLOBAL_KEY), default ON. Affects only waterfall click/tap tuning (`tapHz`, `main.ts:244`).
  Flicking it does not close the popup. Client-only.

### 1.9 MENU (`#mMenu`)

`togglePanel('menu')` opens `#menu`. On the handheld, `buildHandheldMenu` has rebuilt its top as three accordions (§4).
Tooltip `Menu`.

### 1.10 AUDIO (`#mAudio`)

A speaker glyph, or a muted glyph via `.mIcLive` / `.mIcMute`. Tooltip `Audio: squelch, noise reduction, notch`.
`togglePanel('audioPanel')` (§3).

### 1.11 DEVICE (`#mDevice`, handheld only — created at `main.ts:3176-3183`)

| | |
|---|---|
| Label | `DEVICE`. Tooltip `This device: volume, battery, clock, card, power` |
| Placement | Inserted right after `#mChat`; `#mChat` gets `.hhHide`, so DEVICE takes the CHAT cell in `#mStackR` |
| Action | `openDeviceMenu()` (`handheld.ts:782-786`): opens `#menu`, then opens the R36S HARDWARE accordion (§4c) |

### 1.12 CHAT (`#mChat`)

Hidden on the handheld (above). Not to be ported.

### 1.13 Status line (`#mStatus`, index.html:4645-4701)

| Element | Content | Source |
|---|---|---|
| `#mClock` | `HH:MM UTC · HH:MM <abbr>`. The local half is the RECEIVER's zone (hwinfo/config `srvTzOffsetMin`, `srvTzAbbr`), else the browser's. Every 10 s | `mobile.ts:635-652` |
| `#initChip` | `INITIALISING AGC`, class `set` while an RSP's AGC is still settling (`rspInit`). Tooltip in index.html:4659 | `main.ts:2354` |
| `#ovlChip` | `OVERLOAD`. On a dongle the text is replaced by which way the gain went (onOverload) | `main.ts:2167`, `2295`, `2426`, `2511`, `2622` |
| `#antChip` | `FM BAND-STOP FILTER FITTED` / `OUTSIDE ANTENNA RANGE` etc. (`antennaNoticeAt`). Clickable: re-announces the sentence | `antennaNotice.ts:66-80` |
| `#mRecTime` | Breathing dot plus `MM:SS` while recording (`audio.recordedSeconds`) | `main.ts:17357-17361` |
| `#dsChip` | `DIRECT SAMPLING (HF AND BELOW)`. Only while hwinfo `dsActive` and the dial is ABOVE the crossover | index.html:4683 |
| `#linkBars` | Hidden on the handheld | — |
| `#status` | Handheld: **`Spectrum rate <fps> fps`** + ` · IDLE` while idle-throttled. `<fps>` = measured spectrum frames/s, 1 dp, padded with U+2007. Server version: `KB/s · fps · ping · buf`. The tooltip still carries the link figures | `main.ts:6012-6013` |
| `#rspAgcTag` | SDRplay only: `· RF 3 · IF 42`, amber `warn` when stuck. Tooltip `SDRplay AGC — RF = LNA state (0 = most gain), IF = IF gain reduction, dB. …` | `main.ts:3478-3490` |
| `#dspTags` | Boxed `NR` `NB` `AN` when on. Read from the controls: `#nr` value > 0, `#nbxBtn` on, `#notch` on. Never in WFM broadcast | `main.ts:6019-6062` |

**Floating buttons over the waterfall:**
- **`#mCentreFloat`:** `‹ ⌖ CENTRE ON VFO` / `⌖ CENTRE ON VFO ›`. Only while the VFO is off-screen. Action
  `spec.pan(spec.frequency)` → WS `zoom` at the dial frequency (`main.ts:9800-9803`, `10138-10152`).
- **`#mGainStuckFloat`:** SDRplay only. Shown when rspstat `gainStuck === 1` or `agcReinit === 1`.
  - Label `SDRPLAY GAIN API FAILURE · TAP TO RESET (MAY CAUSE AN AUDIO DROP)`, or `PLEASE WAIT · SDRPLAY API RESTARTING`
    (disabled) while restarting.
  - Tap sends WS `{"type":"rsp_agc_restart"}` (`main.ts:2395-2410`, `9790-9798`).

## 2. Frequency entry panel — `#freqPanel` (index.html:5134-5184, `main.ts:19239-19331`)

**Header.** `FREQUENCY`. `CLOSE` (`#freqClose`) calls `closePanels()`.

**Share-pick bar.** `#sharePickBar` belongs to chat share-pick mode (`Pick a station to share — search, or open BOOKMARKS
for the ones this receiver has learnt.` + `CANCEL`). Unreachable on the handheld (no chat). Do not port.

### 2.1 Input (`#freqInput`)

- `type=text inputmode=decimal`. Filled with `(hz / unitDiv).toFixed(dp)`: Hz 0 dp, kHz 3 dp, MHz 3 dp. Refilled on every
  retune unless focused (`renderFreq`, `main.ts:19204-19226`).
- Both `,` and `.` are taken as the decimal point; with both present the comma is a thousands separator (`normaliseDecimal`).
- **While typing in 118-137 MHz,** `#freqMsg` says `Channel <name> · 8.33|25 kHz · <true MHz to 4 dp> MHz`.
- **Controller keyboard:** `kbField` (`padKeyboard.ts`) with title `FREQUENCY`. Its unit key switches the segment (2.2).
- **Enter** = TUNE.

### 2.2 UNITS segmented (`#freqUnitSeg`)

- `Hz` | `kHz` | `MHz`, default `MHz`.
- Pref `freqUnit` (DEVICE-global). The same unit drives the deck readout (§1.5).
- Switching converts the value in the box (100.7 MHz → 100700.000 kHz) rather than reinterpreting it.

### 2.3 VFO A/B (`#freqAbRow`, `#abToggle`)

| | |
|---|---|
| Shown | Only if hwinfo `watch` ≥ 1 (`spec.watchMax`, `spectrum.ts:1672-1673`; `abRowPaint`, `main.ts:20965-20974`) |
| Label | `VFO A/B`. Button `ON` / `OFF` |
| Tooltip (verbatim) | `VFO A/B: a second VFO on this radio, heard at the same time — the lower frequency in your left ear, the higher in your right. The A/B key between the tune buttons picks which one the controls tune; each VFO has its own squelch and MUTE under the speaker button, and a decoder or the spots run on the VFO selected when you start them (it says DEC). Not on FM broadcast.` |
| ON | WS `{"type":"watch","id":1,"on":true,"stereo":<bool>,"monoOut":<bool>[,"balance":<-1..1>]}` (`spectrum.ts:835-850`). `balance` only if hwinfo `watchBal === true` |
| OFF | WS `{"type":"watch","id":1,"off":true}` |
| Panel | Stays open |
| Speaker mode | ⚠️ `monoOut` is set from `audio.outputChannels < 2` every 2 s (`main.ts:20999`). In speaker mode the browser has no output device, so the value is meaningless. The native side should send `monoOut` from the device (true on the built-in speaker, false on headphones, per the `output` event) |

Related AUDIO-sheet rows (per-VFO squelch and MUTE, A⟷B balance, STEREO) are in §3.

### 2.4 TUNE (`#freqGo`) and the range refusal rules (`go()`, `main.ts:19286-19331`)

Steps, in order:
1. Parse. Empty, non-numeric or ≤ 0: `#freqMsg` = **`Enter a frequency`**; the panel stays open.
2. `typed = round(value × unit)`.
3. **Airband pilot entry** (`airbandEntry`, `src/utils/airband.ts`). In 118-137 MHz, a value on a 5 kHz boundary is a
   channel NAME (118.010 tunes 118.0083 MHz). A non-existent name (.x20/.x45/.x70/.x95) is refused:
   **`<name> is not an aviation channel — 8.33 kHz channel names never end in .x20, .x45, .x70 or .x95. Nearest: <lower> or <upper>.`**
   (or `Nearest: <lower>.` at 136.995). The panel stays open.
4. `got = clampTune(asked)` (`main.ts:18903-18966`):
   - a. **VFO B selected,** a watch window known and not `follow`: clamped to the window, with a pill
     `VFO B must stay inside the radio's window, <lo> to <hi> MHz`.
   - b. **Locked window** (hwinfo `lockedCentre` > 0 and the capture rate known): `[centre − h, centre + h]` with
     `h = Fs/2 − min(50 kHz, 0.15·Fs)` (`src/utils/tuneWindow.ts:29-40`). Clamped.
   - c. **Otherwise ranges** = hwinfo `radio.allowed` (the owner's limit) if present, else `radio.ranges` if it has more
     than one range (hardware gaps, e.g. HF+ 0.5 kHz-31 MHz + 60-260 MHz). Else [10 kHz, 1.8 GHz].
     - In a gap: the FIRST attempt parks on the edge with `showTuneGapMsg`:
       **`<edge MHz> MHz — <why>. Tune up|down again to jump to <other MHz> MHz`**, where `<why>` is
       `the server owner does not allow tuning here` (the hardware could reach it) or `this radio cannot receive here`.
     - A second attempt the same way jumps: **`Jumped to <MHz> MHz`**.
5. **A locked window that clamped by more than 1 Hz:** `#freqMsg` = **`This receiver covers <lo>–<hi> MHz`** (3 dp each);
   the panel stays open, nothing is sent.
6. **ISM on:** WS `{"type":"ism","on":1,"hz":<got>}`, close. **AIS on:** pill
   `AIS listens on its two channels — leave AIS to tune elsewhere`, close.
7. **In DAB / ADS-B / NDB** (`heldMode()`): leave that mode first and park the tune (`freqPending`) until the first config
   from outside it. Close.
8. **Otherwise:**
   - `jumpMode = bandJumpDefaults(from, got, mode, region).mode` (`bandPlan.ts:264-274`): the landing band's mode only when
     the band CHANGES and that mode differs from both the band left and the current mode. FM broadcast counts as WFM.
   - `spec.tune(got, jumpMode, {recenter, retarget})` → WS `tune` (plus `tune{modeSet:1}` + `bandwidth` if the mode changed)
     and a `zoom` to a "landing" span (`spectrum.ts:2022-2075`).
   - Then `applyBandStep(got)` (the step follows the band) and the airband passband. Close.

**`SHARE`** (`#freqShare`, tooltip `Copy a link to this frequency`):
- Builds `http://<host>/?freq=<Hz>&mode=<m>&bwl=<lo>&bwh=<hi>` (DAB: `?dab=<block>&sid=<sid>`) and copies it to the
  clipboard. Pill: `Link copied — paste it into a message to share`. On failure it shows a read-only box in `#freqMsg`
  (`main.ts:19343-19385`).
- On the handheld the link points at 127.0.0.1 and there is no one to paste to. **Dead on the R36S; do not port.**

### 2.5 BOOKMARKS and search

- **`★ BOOKMARKS`** (`#mBookmarks`, row `#mBmRow`, `.mOnly`): opens `#bookmarksPanel` with filter `all` and refreshes the
  server list at most every 10 s (`main.ts:11462-11470`). See §5.
- **Search** (`#search` in `#mSearchHost`, placeholder `Search stations, bands, frequency…`) with results `#searchResults`
  (`search.ts`). A result tap calls `tuneTo(r)` (`main.ts:11420-11438`):
  - DAB results go to `dabGoTo`.
  - Otherwise mode = the result's mode, or the band's (`bandTuneDefaults`), or the current one; then `tune` + mode.
  - The result's `bandwidthLow/High` (if any) is sent as `bandwidth`.
  - Then `applyBandStep`.

## 3. AUDIO sheet (`#audioPanel`)

Markup: `web/client/index.html:5208-5404`. Opened by the deck's speaker key `#mAudio` → `togglePanel('audioPanel')`
(`main.ts:9940`); closed by `#audioClose` "CLOSE" (`main.ts:17660`). Wiring: `main.ts:17760-17845` (in `buildMenu`),
volume `main.ts:9805-9812`, recorder `main.ts:16780-16833`, squelch `main.ts:5370-5830`, VFO A/B rows `main.ts:20900-20995`.
Setters: `web/client/src/spectrum.ts:2473-2498` (+ `:835-934` for `watch`). Engine: `android/app/src/main/cpp/local_sdr_shim.cpp:21107-21305`.

Row order on screen (top to bottom): SQUELCH (A) · SQUELCH (VFO B) · BALANCE · VOLUME · REC / RECORDINGS · REC BUFFER ·
UNCOMPRESSED AUDIO · RAW IQ OUT · NOISE REDUCTION · AUTO NOTCH · VFO A/B STEREO · NOISE BLANKER · NFM AUDIO ·
"BROADCAST FM" sub-heading · FM DE-EMPHASIS · PROCESSING (ST / NR / IMS / CEQ / AUTO BW / NB).

### 3.0 How the handheld's speaker mode changes this sheet

- The page opens `/ws/audio?...&speaker=1` (no `codec=opus`) on the handheld (`main.ts:1419-1421`). `AudioPlayer.speakerMode`
  (`audio.ts:867-883`) then builds **no AudioContext, worklet or decoder**; the Worker only keeps the socket open.
- The engine marks that socket a speaker socket (`local_sdr_shim.cpp:22911` → `speakerMark` `:6624`). In `sendWs`
  (`:18005-18024`) every raw-PCM audio frame for that socket is copied, scaled by `speakerLevel` (linear) and pushed to
  ALSA (`vibe_local_speaker.h`). **It goes on over the wire as well only while `speakerTap` is set.** "Every audio path
  (shared, per-listener, squelched zeros, VFO B mixes) ends in this call", so everything the ENGINE does to the audio
  (squelch, NR, notch, NB, de-emphasis, stereo, VFO A/B mix/balance/mute) is heard on the speaker.
- The page talks to the speaker with ONE message on the spectrum/control socket (`main.ts:18062-18067` `sendSpeaker`):
  `{"type":"speaker","level":<0..1, 3 dp>,"tap":0|1}` (either field may be absent). Engine: `local_sdr_shim.cpp:21117-21123`
  (Linux, `g_vsHandheld` only): `tap` → `speakerTap`, `level` → `speakerLevel` clamped 0..1. Re-sent on every (re)connect
  from `pushSettingsToServer` (`main.ts:18072`).
- **Conclusion: every row on this sheet except the recordings playback, the A/B mono-output detection and the
  (already hidden) UNCOMPRESSED AUDIO row is engine-side and works in speaker mode.** Nothing here is a Web Audio effect;
  the browser's only own audio processing was volume/mute (GainNode), which speaker mode forwards as `speaker.level`.

Helper semantics (they decide "state comes from"):
- `toggle(id, apply, prefKey, initial, pushOnInit=true)` (`main.ts:17505`): first paint = saved pref (if `pushOnInit`) else
  `initial`; with `pushOnInit` it SENDS the stored value at wiring time. Click flips, sends, saves pref.
- `setToggleTo` (`:17573`) paints a server report (hwinfo DSP fields), ignored for 4 s after a press of that control, and
  saves the pref. `segment` (`:17588`) / `setSegmentTo` (`:17608`) the same for segmented rows (note: `segment` only accepts a
  NUMBER pref; a string pref falls back to the button marked `on` in markup).
- `slider(id, valId, fmt, apply, prefKey)` (`:17399`) restores a numeric pref and **applies once at wiring** (so NR is sent
  on start).
- Server DSP report = fields of the `hwinfo` message, parsed `spectrum.ts:1711-1742` → `onDspState` (`main.ts:2008-2075`).
- Prefs = `localStorage['vibesdr_web_prefs_v1']` via `prefs()/savePref()` (`main.ts:305-313`). On the handheld only `volume`
  among these keys is DEVICE-wide (`handheld.ts:113` GLOBAL_KEY); all the others are per-radio memory.

---

### 3.1 SQUELCH (VFO A) — live meter bar with a draggable ball

| | |
|---|---|
| Label | `SQUELCH` (+ tag `VFO A` / `VFO A · DEC` and a `MUTE` key only while VFO B is on) — `index.html:5221-5222` |
| Bar tooltip | "Drag the ball to set the squelch; drag fully left for OFF" |
| Overlay text when auto | `AUTO SQUELCH ACTIVE` (`#sqlAutoTag`) |
| Help line `#sqlNote` (`main.ts:5409-5413`) | auto: "Auto — the red line tracks the measured noise floor and keeps the gate a few dB above it. Drag the bar to take over manually." · on: "Audio passes only above the ball. The level you set is where it stays — the bar underneath moves with the signal, the threshold does not." · off: "Off — audio always passes. Drag the ball up from the left to set a threshold." |
| Type | custom bar: fill = live signal, ball = threshold. Pointer drag (`setupSquelchBar` `main.ts:5714-5740`) |
| Scale | METER_MIN_DBFS **−121** … METER_MAX_DBFS **−13** dBFS (`main.ts:5839`, fed to `sqlScaleMin/Max` at `:5347`). Ball x → `round(min + f·(max−min))` dB, step 1 dB. Dragging past the left edge (`frac < −0.04`) = OFF (`VS_SQL_OFF = −999`). Ball parks at 0 % when off. Bar gets class `closed` (red) while raw signal < threshold. Fill = the main meter's smoothed level (`sqlSigNorm`). |
| Default | OFF |
| State from | prefs `squelch` (dB) + `squelchOn` (bool), restored by `vsSqlRestore` in `pushSettingsToServer` (`main.ts:18080-18081`) |
| Sends | `applySquelch` (`main.ts:5370`) → `spec.setSquelchVfo('A', db)` → with no VFO B (or A selected) `setSquelch` → `{"type":"squelch","db":<dB, or −100 when off>,"on":true|false}` (`src/services/vibeSquelch.ts:45-48`, `spectrum.ts:2473-2478`). If VFO B is the SELECTED VFO, A's threshold still goes as the plain `squelch` message (`spectrum.ts:864-868`). Engine: `local_sdr_shim.cpp:21125-21137` (per-session gate `sessionSquelch`). |
| Also | `audio.squelchDb = db` (`main.ts:5374`) — only feeds the "is the tab muted?" heuristic, no audio effect. The deck's `#mSqlLine` and `SQL` chip read the same `squelchDb`. A drag while AUTO is on turns AUTO off first. |
| Shown | always (hidden only on an Icom, `.icomHide`) |
| Speaker mode | **Works** — gate is the engine's; the speaker plays the squelched (zeroed) PCM. |
| Pad | the bar is a plain `<div>` with pointer handlers only — no keyboard/pad adjust path found. |

### 3.2 AUTO SQUELCH + ABOVE NOISE (VFO A)

| | |
|---|---|
| Label | `AUTO SQUELCH` (toggle button `#sqlAuto`, class `on` when active) — `index.html:5236` |
| Tooltip | "Track the channel's own noise automatically — the threshold follows it, with a hang through the gaps between words"; when the server sends no `sig`: disabled, "Unavailable — this server does not report a measured noise floor, so there is nothing for auto squelch to follow" (`main.ts:5421-5430`) |
| Type | toggle (client-side controller) |
| Default | off; pref `squelchAuto` (bool) restored in `pushSettingsToServer` (`main.ts:18084-18085`) |
| Logic | Client-side tracker `autoSquelchTick` (`main.ts:5574-5667`), run on every `sig` message (`{"type":"sig","chan":dB,"floor":dB,"chanB"?:dB}`, `spectrum.ts:1938-1943`): EWMA noise baseline (falls fast, rises only while gate shut), neighbour ceiling, margin, hysteresis 3 dB (`SQL_AUTO_HYST_DB`), hang 700 ms (`SQL_AUTO_HANG_MS`) during which it sends OFF. Sends the same `squelch` message as 3.1 on each change, `persist=false` (does not overwrite manual pref). Turning AUTO off restores the manual pref value (`setSquelchAuto` `:5670-5684`). |
| Margin row | `ABOVE NOISE` slider `#sqlAutoMargin` **4..20, step 1, default 12**, readout `+12 dB`; tooltip "How far above the channel's own noise the squelch opens. Lower hears weaker signals and lets more noise through; higher is quieter but can clip the start of a weak over. The red line on the bar moves with it." Pref `squelchAutoMargin`. **Shown only while AUTO is on** (`main.ts:5419`). Shared with VFO B's auto. No message of its own (re-runs the tick). |
| Speaker mode | **Works** — controller is in the page but its output is the engine `squelch` message. A native port must re-implement this tracker (or move it to the engine). |

### 3.3 SQUELCH (VFO B), MUTE A / MUTE B — only while VFO B is on

`#sqlBlockB` (`index.html:5252-5263`), shown by `sqlRowsPaint` when `spec.abOn` (`main.ts:5790-5800`).
- Label `SQUELCH` + tag `VFO B` (or `VFO B · DEC`); bar tooltip "VFO B's squelch: drag the ball to set it; drag fully left for OFF". Same scale/drag as 3.1 (`setupSquelchRowB` `main.ts:5802-5824`).
- Sends `{"type":"watch","id":1,"sql":<dB>,"sqlOn":true|false}` (`spectrum.ts:864-868`, or via `setSquelch` when B is selected `:2476`). Engine `handleWatch` (`local_sdr_shim.cpp:18677`, doc `:29027`). No pref (per session); AUTO off = squelch OFF.
- `AUTO SQUELCH` `#sqlAutoB` — tooltip "VFO B's own auto squelch — it tracks B's channel noise the same way VFO A's does, with the same ABOVE NOISE margin". Client tracker `SqlAutoTrack` (`src/utils/sqlAutoTrack.ts`) on `sig.chanB`.
- `MUTE` keys: A `#sqlMuteA` (hidden unless B on) tooltip "Mute VFO A — it keeps decoding and its meter keeps moving; VFO B comes to the middle, both ears. Press again to hear it." → `{"type":"watch","muteA":true|false}`; B `#sqlMuteB` tooltip "Mute VFO B — it keeps decoding and its meter keeps moving; VFO A comes to the middle, both ears. Press again to hear it." → `{"type":"watch","id":1,"mute":true|false}` (`spectrum.ts:854-861`). Label flips `MUTE` / `MUTED`.
- Speaker mode: **works** (engine mix).

### 3.4 BALANCE (A ⟷ B) — only while VFO B is on AND hwinfo `watchBal:true`

`#abBalBlock` (`index.html:5270-5281`), `abBalPaint` (`main.ts:20921-20936`).
- Label `BALANCE`; key `EVEN` (tooltip "Even — both VFOs at their own level") sets 0.
- Slider `#abBal` **−1..1, step 0.05, default 0**, flanked by `A` / `B`; tooltip "Slide towards the VFO you want to hear more of: the other is turned down — nothing is ever turned up. All the way over plays that VFO on its own, in both ears." Readout `#abBalVal`: `EVEN` / `A only` / `B only` / `A −n dB` / `B −n dB` (from `balanceGains`, `src/services/dualMix.ts`).
- Note: "Turn the louder VFO down — a broadcast station against an SSB contact. Works with VFO STEREO on or off, and on a mono output."
- Sends `{"type":"watch","balance":<−1..1, 2 dp>}` (`spectrum.ts:930-933`); pref `abBalance` (`main.ts:20800`). Also sent inside the `watch` on-message and on every hwinfo while B is on (`spectrum.ts:1669-1671`).
- Speaker mode: **works**.

### 3.5 VOLUME + mute

| | |
|---|---|
| Label | `VOLUME` (`index.html:5288`) |
| Controls | mute key `#mPanelMute` (speaker glyph / crossed glyph, tooltip "Mute") + slider `#mPanelVol` **0..100, step 1** |
| Default | pref `volume` (0..1) × 100, else **80** (`main.ts:9806`) |
| Sends | slider → `audio.volume = v/100` + `savePref('volume')` (`main.ts:9808-9811`); mute → `setMuted(!muted)` (`:9734`). In speaker mode the setters (`audio.ts:2875-2890`) send `{"type":"speaker","level":<volume>}` / `{"type":"speaker","level":0}` when muted, and `level:volume` on unmute. `setVolumePct(0)` = mute (`:9757`). Mute also toggles class `muted` on `#mAudio` and `#mPanelMute`. Mute state is NOT persisted. |
| Engine | `speakerLevel` multiplies the PCM linearly before ALSA (`local_sdr_shim.cpp:18015-18020`). This is a **software level on top of** the R36S HARDWARE `VOLUME` row (`/handheld/device {"set":"volume"}`, the codec) — two independent volumes on the handheld. |
| Shown | always (`.mOnly` = flex) |
| Speaker mode | **Works — forwarded** via `speaker.level`. Pref `volume` is device-wide on the handheld (`handheld.ts:113`). |
| Also | pointer-only hover/wheel popover on `#mAudio` (`main.ts:9827-9876`) — `(hover:hover) and (pointer:fine)` only, so absent on the R36S. |

### 3.6 REC / RECORDINGS

| | |
|---|---|
| Keys | `#mPanelRec` record glyph + `REC` (label `#mRecLbl` → `STOP` while recording), tooltip "Start / stop recording"; `#mPanelRecs` `RECORDINGS`, tooltip "Recordings" (`index.html:5292-5296`) |
| REC behaviour | `initRecorder` (`main.ts:16791-16824`): start → `audio.startRecording()` (prefix = record buffer contents); `showRecording(true)` (classes `rec` on `#mPanelRec` and `#mAudio`, deck timer `#mRecTime/#mRecVal`). Stop → `audio.stopRecording()` = 48 kHz **stereo 16-bit WAV** blob, then clears the record buffer. |
| Handheld save | `saveRecordingToCard(blob, spec.frequency, detail, startMs)` (`handheld.ts:412-419`): **`POST /handheld/recordings?hz=<Hz>&mode=<MODE or "DAB <block>">&start=<unix s>`**, body = the WAV; reply `{ok, name, error?}`; toast "Saved to the card" / "Recordings/<name>"; failure toast "Recording NOT saved". |
| Where the PCM comes from in speaker mode | Starting/stopping calls `_syncWorkerTap` (`audio.ts:2478-2481`) → `{"type":"speaker","tap":1|0}`. With `tap` set the engine ALSO sends the speaker socket's raw PCM over the WebSocket (`local_sdr_shim.cpp:18022`), the Worker forwards it (`audio.ts:1154`) and the page builds the WAV. So **yes, the browser receives PCM, but only while recording or while the record buffer is on**. The tapped PCM is the payload *before* `speakerLevel` is applied (the scaling is on a copy), so recordings are independent of the page volume, but include squelch zeros and all engine DSP. |
| RECORDINGS | `togglePanel('recordingsPanel')` + `renderRecordings()`; on the handheld `renderCardRecordings` (`main.ts:17314`, `:3193-3227`): `GET /handheld/recordings` → `{recordings:[{name,bytes,mtime}]}`; each row = name (minus .wav) · size, `✕` (press twice within 3 s, shows `DELETE?`) → `DELETE /handheld/recordings/<name>`, and an `<audio controls preload=none src="/handheld/recordings/<name>">` player. Empty: "No recordings on the card yet — press ● REC." |
| Speaker mode | REC **works** (engine tap → page encodes WAV → engine writes the card). **RECORDINGS playback is BROWSER-ONLY**: it is an HTML `<audio>` element, played by the browser (Cog/WPE/GStreamer), not the engine speaker. A native UI needs an engine "play file" path. |

### 3.7 REC BUFFER

| | |
|---|---|
| Label | `REC BUFFER` (`index.html:5298-5305`) |
| Type | segmented `#recBufSeg`: `OFF` (data-rb 0) · `30 s` (30, marked `on` in markup) · `60 s` (60) |
| Help | "Record Buffer: when enabled, VibeSDR holds a live buffer of up to 30 or 60 seconds of audio, which can allow for recording of easily missed signals. This buffer is cleared on retune, so the previous station's audio is not included." |
| Default | 30 s generally (`recordBuffer.ts RECORD_BUFFER_DEFAULT`); **on the handheld forced OFF**: `if (isHandheld() && prefs().recbuf === undefined) savePref('recbuf','0')` (`main.ts:16787`) |
| State / sends | pref `recbuf`; no WS message of its own — `audio.setRecordBuffer(sec)` (`audio.ts:2445`) keeps a client-side ring of PCM; it sets the speaker `tap` (so the engine streams PCM continuously while it is on). Cleared on a retune beyond the mode's allowance / mode / source change (`recBufCheck` `main.ts:16770-16777`, `RecordBufferAnchor`). |
| Speaker mode | Works, but the buffer itself lives in the browser and costs a continuous PCM stream (~45 % of an A35 core, per the comment at `:16784-16786`). A native port would hold this buffer in the engine/UI process. |
| ⚠ Display bug | the handheld default is saved as the STRING `'0'`, but `segment()` only restores a NUMBER pref (`main.ts:17592,17604`), so on a fresh handheld the segment shows **`30 s` lit while the buffer is actually OFF** (`recBufSec` = `parseRecordBufferSeconds('0')` = 0). Fixed itself only once the user presses a segment. |

### 3.8 UNCOMPRESSED AUDIO — hidden on the handheld

`#rawAudioRow` / `#rawAudioNote` (`index.html:5307-5315`). Label `UNCOMPRESSED AUDIO`, toggle `#rawAudio` OFF/ON. Note: "Off, audio is compressed to around 10 KB/s. On, you get the raw stream — about 187 KB/s, roughly twenty times more, out of the operator's connection. Worth it on a fast link if you can hear the difference; a courtesy to leave off if you can't." Shown only when `/vibeserver.json` `uncompressed:"choice"` and not `local` (`main.ts:1291-1303`); click = `setPrefersRawAudio` + `location.reload()` (codec is a URL parameter). **Handheld: hidden twice** — `srvLocal` is true on loopback, and `handheld.ts:248` CSS hides `#rawAudioRow, #rawAudioNote`. Do not port.

### 3.9 RAW IQ OUT

`#iqRow`/`#iqNote` (`index.html:5320-5330`), `refreshIqRow` (`main.ts:695-760`). Label `RAW IQ OUT`; `<select id=iqRate>` 48k / 96k / 192k / 250k (+ `<n>M FULL` when `rawIqFull>0` on the LAN; tooltip "Sample rate of the IQ stream. 48 kHz covers every digital voice mode; wider only on the local network."), toggle `#iqBtn` OFF/ON. Shown when `/vibeserver.json` `rawIq` ≠ `off` and not a shared dial / front door; rate picker only when `lan`. Sends `{"type":"iqout","on":1|0,"rate":<Hz>}` (`spectrum.ts:2406`); answer fills the note (host:port, or a VibeIQ code). Not hidden by handheld.ts; it is a network side-channel (an rtl_tcp server for another device), so it only makes sense on the handheld if the device is on a network. No audio effect — speaker mode irrelevant.

### 3.10 NOISE REDUCTION (the audio denoiser)

| | |
|---|---|
| Label | `NOISE REDUCTION` (`index.html:5331-5335`), readout `#nrVal` `OFF` or `<n>%` |
| Type | slider `#nr` **0..100, step 1, default 0 (= OFF)** |
| Sends | `{"type":"nr","on":v>0,"strength":nrStrength(v)}` (`spectrum.ts:2479`); `nrStrength`: 0..80 % → 0..1.0, 80..100 % → 1.0..1.4 (`main.ts:17634-17637`). Sent on wiring (slider applies once) and every input. |
| State from | pref `nr` (percent); hwinfo `nr` (bool) + `nrStrength` (0..1.4) → slider via the exact inverse `nrPercent` (`main.ts:2009-2022`, `:17652`) — the slider dispatches `input`, so the echo re-sends (inverse must stay exact). |
| Engine | `local_sdr_shim.cpp:21139-21147` (shared gate; `setNR`, `setNrStrength` clamp 0..1.4) |
| Shown | always (every mode; the BROADCAST FM `NR` button is a different control) |
| Speaker mode | **Works** — engine DSP. |

### 3.11 AUTO NOTCH

Label `AUTO NOTCH`, toggle `#notch` OFF/ON (`index.html:5336-5339`). `toggle('notch', …, 'notch')` pushOnInit=true (`main.ts:17770`). Sends `{"type":"notch","on":true|false}` (`spectrum.ts:2480`); state: pref `notch`, hwinfo `notch`. Engine `local_sdr_shim.cpp:21265-21268` (shared gate). Shown always. **Works** in speaker mode.

### 3.12 VFO A/B STEREO — only where hwinfo `watch` ≥ 1

`#abStereoRow` (`index.html:5348-5352`), shown when `spec.watchMax >= 1` (`main.ts:20905-20906`) — i.e. whenever the radio offers VFO A/B, whether or not B is on.
- Label `VFO A/B STEREO`, sub `Lower VFO left, higher right`; toggle `#abStereoBtn` ON/OFF (default ON), tooltip "With VFO A/B on: the lower-frequency VFO plays in your LEFT ear and the higher in your RIGHT, so the waterfall and your ears agree. Off: both VFOs in both ears."
- Sends `{"type":"watch","stereo":true|false}` only while B is on (`spectrum.ts:927`); always included in the `watch` on-message (`:841`). Pref `abStereo` (`main.ts:20798`, default true).
- `#abMonoHint` "Mono output — both VFOs are mixed." shown when `audio.outputChannels < 2` → `{"type":"watch","monoOut":true}` (`main.ts:20993`, `spectrum.ts:934`).
- Speaker mode: stereo **works** (engine pans). **Mono detection is BROWSER-ONLY and inert in speaker mode**: `outputChannels` reads the AudioContext, which does not exist, so it always says 2 — `monoOut` is never sent. If the R36S speaker is mono, the engine (or native UI) must set `monoOut` itself (headphones vs speaker: the `/handheld/events` `output` event).

### 3.13 NOISE BLANKER (every mode but WFM)

`#nbxRow` hidden until the hwinfo report carries `nbx` (bool) (`main.ts:2054-2057`). Label `NOISE BLANKER`, toggle `#nbxBtn` OFF/ON, default OFF, pushOnInit=false (`main.ts:17805`). Tooltip: "Impulse noise blanker on your own channel — the crackle of distant lightning, power-line hash, ignition, thermostats, switch-mode supplies. Removes the moments an impulse occupies rather than filtering continuously; on a clean signal it blanks nothing. Not for broadcast FM, which has its own NB below. The owner may also run a wider one on the whole receiver." Sends `{"type":"nbx","on":true|false}` (`spectrum.ts:2498`); pref `nbx`. Engine `local_sdr_shim.cpp:21206-21210` (per listener). Shown in every mode once known (not hidden in WFM). **Works**.

### 3.14 NFM AUDIO (VOICE / RAW) — NFM only

`#nfmVoiceRow`, shown only when hwinfo has reported `nfmvoice` AND mode is `nfm` or `fm` (`syncNfmVoiceRow` `main.ts:10116-10119`, every frame). Label `NFM AUDIO`; single toggle `#nfmVoiceBtn` reading `VOICE` (on, default) / `RAW`. Tooltip: "VOICE: 300 Hz to 3 kHz, as a normal NFM radio sounds — removes the repeater's sub-audible CTCSS tone and softens the hiss. RAW: the flat, unfiltered demodulator output, for piping into an external decoder (pagers, 9600-baud packet) that needs the low frequencies." Sends `{"type":"nfmvoice","on":true|false}` (`spectrum.ts:2485`); pref `nfmVoice` WINS over the report (re-sent if they differ, `main.ts:2043-2052`). Engine `local_sdr_shim.cpp:21290-21293`. **Works**.

### 3.15 BROADCAST FM — FM DE-EMPHASIS

Sub-heading `BROADCAST FM` (`index.html:5368`). Label `FM DE-EMPHASIS`; segmented `#deemphSeg`: `OFF` (tau 0) · `50µs` (50, default/marked) · `75µs` (75). Sends `{"type":"deemph","tau":<seconds: 0 | 5e-5 | 7.5e-5>}` (`spectrum.ts:2482`, `main.ts:17833`) on click only. State: pref `deemph` (µs, number), hwinfo `deemph` (seconds → µs, `main.ts:2039-2040`). Engine `local_sdr_shim.cpp:21295-21299`. **Shown in every mode** (no mode gate in the code; only `.icomHide`) though it only affects WFM. **Works**.

### 3.16 BROADCAST FM — PROCESSING (six toggles on one row)

Label `PROCESSING` (`index.html:5387-5403`). All six read the server first (pushOnInit=false except where noted); first paint = markup/`initial`, then hwinfo overwrites. Shown in every mode (only `.icomHide`); they act on WFM.

| Button | Labels | Default | Tooltip (verbatim) | Sends | State | Engine |
|---|---|---|---|---|---|---|
| `#stereoBtn` | `ST ON` / `ST OFF` | on | "Stereo decoding. Off forces mono — which is a legitimate thing to want on a weak signal, though NR now treats mono hiss too, so it is no longer the only way to escape it." | `{"type":"stereo","on":b}` | pref `stereo`; hwinfo `stereo` | `:21301-21304` (shared gate) |
| `#wspBtn` | `NR ON` / `NR OFF` | off | "NR — weak-signal noise treatment: stereo high-blend plus an audio high-cut. WORKS IN MONO TOO — the high-cut treats baseband hiss, which is there whether you are in stereo or not; only the stereo half needs L-R. Acts only on a noisy signal: a strong station is bypassed entirely. Off unless you (or the server's owner) switch it on — it is for a weak, hissy station." | `{"type":"wsp","on":b}` | pref `wsp`; hwinfo `wsp` (absent = true) | `:21196-21199` (per listener) |
| `#imsBtn` | `IMS ON` / `IMS OFF` | on | "IMS — Intelligent Multipath Suppression, as on the TEF6686. Treats a REFLECTION that CEQ cannot correct: below 18 dB MPX S/N a channel equaliser would contort itself around noise, so it stands down, and until now nothing treated multipath as multipath in that range. Multipath is phase distortion and the 38 kHz stereo subcarrier suffers it far worse than the mono sum, so this blends toward mono in proportion to the measured multipath depth — suppressing the distortion specifically rather than retreating from noise. Stands down entirely while CEQ is engaged, because CEQ is correcting the same fault properly. Watch MULTIPATH in the Advanced RDS panel." | `{"type":"ims","on":b}` | pref `ims`; hwinfo `ims` (absent = true) | `:21260-21263` |
| `#ceqBtn` | `CEQ ON` / `CEQ OFF` | on | "CEQ — blind channel equaliser. Undoes a REFLECTION (multipath), which is distortion rather than weakness. FM is a constant-amplitude signal, so the equaliser adapts by driving the envelope back toward constant — no training signal needed. It engages only on a good signal with real multipath, because on noise it would amplify what it cannot fix. Watch MULTIPATH in the Advanced RDS panel: it shows the depth before and after." | `{"type":"ceq","on":b}` | pref `ceq`; hwinfo `ceq` (absent = true) | `:21212-21215` |
| `#autoBwBtn` | `AUTO BW ON` / `AUTO BW OFF` | server's (`srvAutoBw`, else off) | "AUTO BW — automatic demodulator bandwidth, the TEF6686's trick. A strong station keeps the full width it was transmitted with; a weak one is narrowed toward 134 kHz, because at the edge of the channel there is more noise than signal. Driven by the MPX S/N, and it changes the filter WITHOUT interrupting the audio or the RDS. FM broadcast only. Watch AUTO BW in the Advanced RDS panel to see what it is choosing." | `{"type":"autobw","on":b}` | pref `autobw`; hwinfo `autobw` (+ `autobwHz`, `autoBwModes`) | `:21217-21258` — it is the CURRENT mode's AUTO (WFM TEF-style, or AM/SSB/CW passband AUTO). Same switch as the `AUTO` key `#bwAuto` in the mode picker's bandwidth row; each keeps the other in step (`main.ts:17820-17832`). |
| `#nbBtn` | `NB ON` / `NB OFF` | on | "Noise blanker — impulse noise: ignition, thermostats, switch-mode supplies, electric fences. A different fault again: impulses are brief and enormous, so the cure is to remove the moments they occupy rather than to filter continuously. On a clean signal it blanks nothing. The BLANKER figure in the Advanced RDS panel shows how much it is actually removing." | `{"type":"nb","on":b}` | pref `nb`; hwinfo `nb` (absent = true) | `:21201-21204` (per listener) |

All six: **work** in speaker mode (engine DSP).

### 3.17 Not on this sheet (for the avoidance of doubt)

- The old AM AUTO BW row moved to the `AUTO` key in the mode picker's bandwidth row (`index.html:5353-5354` comment).
- No EQ, no client-side noise reduction, no client-side squelch, no browser recording effects exist in this sheet; every
  DSP row is an engine message. The only Web Audio things the browser did were volume/mute (forwarded in speaker mode),
  the mono-output probe (inert in speaker mode) and `<audio>` playback of recordings (browser-only).
- `#dspTags` on the deck status line ("Audio processing is on — click to open the audio panel") reads these same rows
  (`main.ts:6040-6060`: NR slider > 0 outside WFM, `nbxBtn`, `notch`).
## 4a. MENU → SDR HARDWARE

**What goes in it.** `buildHandheldMenu` (web/client/src/handheld.ts:616-635) takes every top-level node of `#menu` from `<h2>RADIO</h2>` (index.html:5885) up to `<h2>DISPLAY</h2>` (index.html:6393) and moves it into the SDR HARDWARE accordion. That range includes the ANTENNA sub-section (`#antHead`, index.html:6386). It also stops early if it meets `#adminServerRow`, which does not happen inside this range. Every listener stays bound because the nodes are moved, not copied. The `<h2>RADIO</h2>` itself is removed. Nodes with the `.mOnly` class go to **DISPLAY** instead (see 4a.0).

**Which radio.** Branch on **hwinfo `radio.driver`** (`RadioCaps.driver`, spectrum.ts:268), *not* on `/handheld/session` `radio.driver`. The values the code tests (main.ts:19954-19984) are:

| hwinfo `radio.driver` | Radio |
|---|---|
| `rtl` | RTL-SDR dongle. ★ It is `rtl`, not `rtlsdr`. Shim: local_sdr_shim.cpp:44026 |
| `sdrplay` | SDRplay RSP |
| `airspyhf` | Airspy HF+ |
| `airspy` | Airspy R2 / Mini |
| `hackrf` | HackRF One |
| `usrp` | Ettus USRP B2xx |
| `icom` | Icom transceiver, receive only. Probably not reachable on the R36S, which offers USB SDRs only (handheld.ts:289) |

**Where the code lives.**
- Per-driver show/hide: `applyRadioCaps(caps)` (main.ts:19934-20107). It runs on every hwinfo that carries `radio`.
- Generic rows (gain slider, rates): `populateHw()` (main.ts:18237-18432).
- Server state each hwinfo: the `onHwInfo` callback (main.ts:2167ff), from `case 'hwinfo'` in spectrum.ts:1663-1745.

**hwinfo fields used in this section** (spectrum.ts:1663-1745):
- Gain and rate: `gains[]` (tenths of dB), `rates[]`, `lockedRate`, `rateNow`, `gainNow` (tenths), `gainCap` (tenths, -1 = none), `gainLocked`, `agcLocked`, `agc` (VibeAGC running, 1/true), `ifGrFloor`.
- Direct sampling and dongle AGC: `digitalAgc`, `dsActive`, `autoDs`, `dsBelowHz`, `ds`.
- IF filter: `tunerBw`, `tunerBwAuto`, `tunerBws[]`, `ifCap`, `rfCentre`.
- Window and admin: `lockedCentre`, `adminOk`.
- DSP state: `biasT` and `rspBiasT`, read via onDspState. RSP `rfNotch` / `dabNotch` also come via onDspState.
- Everything else: the `radio{}` object (RadioCaps, spectrum.ts:268-345).

**RSP telemetry** is the `rspstat` message (main.ts:2332ff). Its fields are `sysGain`, `lna`, `ifgr`, `overload`, `settling`, `vagc`, `vtgt`, `agcInit`, `gainStuck`, `agcReinit`, `autoNotch`, `userNotch`, `lnaN`, `ds`, `dsBelowHz`.

**Gates on the engine** (local_sdr_shim.cpp:18905-18921):
- `adminGate` passes for a loopback caller when no admin password is set, or when not serving on the LAN. On the handheld the page is a loopback caller, so **owner-only controls work**.
- `sharedGate` refuses only on a locked centre for a non-admin.
- Client side, `rspRestricted()` (main.ts:20199) is true only when (`lockedCentre`>0 AND admin protected AND not unlocked) OR `gainLocked`. On the handheld that means only the owner's per-band **gainLocked**.

**Hidden on the handheld** (handheld.ts:245-249, injectStyle): `#adminRow` and `#adminPwRow` are hidden. `#adminNote` (index.html:6335) is *not* in the hide list; it shows only when the server is admin-protected and locked.

**Browser-audio flag:** none of these controls touch the browser's audio path. They are all WS control messages to the engine, so they work unchanged in speaker mode.

**How the slider and toggle helpers behave** (main.ts:17505-17605):
- `toggle(id, apply, prefKey, initial=false, pushOnInit=true)`:
  - Button text becomes `ON`/`OFF` and the `.on` class is set.
  - With pushOnInit, it **sends the saved pref (or `initial`) once at page build**.
  - A click saves `prefKey`.
- `setToggleTo` paints state from the server. It ignores a report for 4 s after a local press, and also saves the pref.
- `segment(id, attr, apply, prefKey)`: paints the saved value at build and never sends at build.
- Prefs live in `localStorage['vibesdr_web_prefs_v1']`, which `savePref`/`prefs()` read and write. On the handheld these keys are per-radio memory (handheld.ts:113: everything except the LOOK keys).

---

### 4a.0 `.mOnly` rows — these go to DISPLAY, not SDR HARDWARE (cross-reference for the DISPLAY slice)

| Row | Label (verbatim) | Type | Behaviour / message | Ref |
|---|---|---|---|---|
| VFO CENTRE | `VFO CENTRE`; button text `LOCKED` / `FREE`; title "LOCKED: the view stays centred on the dial. FREE: pan the view independently." | toggle button `#mPanelLock` (class `.free` when free) | Client-only: flips `spec.followVfo`. No message at the moment of the click. Afterwards view messages `{type:"zoom",frequency,binBandwidth}` carry `free:1` while FREE (spectrum.ts:2533). Row class `icomHide` hides it on an Icom. Default LOCKED. | index.html:5895-5899; main.ts:9771-9776 |
| CENTRE | `CENTRE`; title "Snap the view back onto the dial" | button `#mPanelCentre` | `spec.pan(spec.frequency)`, which sends a view/zoom message re-centred on the dial | index.html:5901; main.ts:9800-9801 |
| FIT | `FIT`; title "Fit the whole band" | button `#mPanelFit` | `spec.resetView()` → `{type:"reset"}`. Ignored while DAB/ADS-B/ISM/AIS/NDB is held (spectrum.ts:2414). Disabled in DAB etc. (main.ts:9680) | index.html:5902; main.ts:9769 |

---

### 4a.1 Rows common to every driver (order as in the markup)

| # | Label / help (verbatim) | Type | Options / range / default | State from | Sends (exact) | Shown when | Ref |
|---|---|---|---|---|---|---|---|
| 1 | `RECEIVER` | read-only text `#radioName` | — | `radio.model`. On sdrplay it is prefixed `SDRplay `. With no model and driver `rtl` it reads `RTL-SDR`; otherwise `—`. On an Icom it reads `Icom <model> · receive only` | nothing | always | index.html:5906-5909; main.ts:20053-20056 |
| 2 | `PORTABLE SERVER WI-FI` (+ sub `#pcLine`); buttons `NETWORK`, `OWN WI-FI`, plus one per saved network | segmented | — | pocketConn.ts | (pocket only) | only on a pocket VibeServer, on its own network. **Never on the handheld** in practice | index.html:5912-5918 |

---

### 4a.2 `rtl` (RTL-SDR dongle)

On `rtl`, the RSP/HF+/Airspy/HackRF/USRP panels are all hidden. The rows that show are `#rowGain`, `#rowAgc`, `#rowRate`, `#rowTunerBw`, the `.rtlOnly` rows and the `.rtlRspOnly` row (main.ts:19979-20006).

| Label / help (verbatim) | Type | Options / range / default | State from | Sends (exact) | Shown when / notes | Ref |
|---|---|---|---|---|---|---|
| `GAIN`, value `#gainVal` = `"<dB 1dp> dB"` | slider `#gain` | **index** into hwinfo `gains[]`, min 0, max `gains.length-1`. The max is cut to the highest index with gain ≤ `gainCap`. Title when capped: "The owner has limited gain to X dB on this band". Default: the index nearest `gainNow`; else pref `gainIdx`; else the top index | hwinfo `gains`, `gainNow`, `gainCap`. Disabled while VibeAGC is on (`agc`) | `{"type":"gain","value":<tenths dB, int>}`, coalesced to 1 per 120 ms with the trailing edge kept (spectrum.ts:2437-2455). Saves pref `gainIdx`. Sent once on load **only** if the server gave no `gainNow` and a pref exists | hidden on sdrplay / airspyhf / hackrf. Also hidden when `gainLocked` (applyGainLocked main.ts:18213). Shim: local_sdr_shim.cpp:20767 (sharedGate "gain") | index.html:5919-5923; main.ts:18238-18275, 18142-18165 |
| `VIBE AGC`, sub `VibeSDR Custom AGC for RTL-SDR`; button `AUTO`; title (runtime) "VibeAGC — VibeSDR's own AGC for RTL-SDR. It watches the ADC for overload and moves the tuner gain to suit. The dongle's built-in AGC is unreliable and is never used." When locked: "VibeAGC — VibeSDR's own AGC for RTL-SDR. The owner has locked it on for this receiver." | toggle `#gainAuto` (`.on` = AUTO; `.locked` class when `agcLocked`) | on/off | hwinfo `agc` (VibeAGC running) and `agcLocked` | ON → `{"type":"gain","auto":true}`. OFF → `{"type":"gain","value":<gains[slider]>}`. Turning it OFF is refused client-side while `agcLocked` | shown only on `rtl`; hidden on sdrplay / airspyhf / hackrf / airspy / usrp, and when `gainLocked` | index.html:5931-5935; main.ts:17742-17757, 2196-2219, 20076 |
| `SAMPLE RATE` | select `#rate` | hwinfo `rates[]` ascending, minus anything above `lockedRate` (an up-to ceiling). Labels: `"<x.xxx> MS/s"`, with ` (may drop samples)` added when the driver is not sdrplay and the rate is > 2.4 MS/s. On sdrplay the label is `"<x> MS/s — <bits>-bit ADC"` (≤6.048M=14, ≤8.064M=12, ≤9.216M=10, else 8). Shows `rateNow` (an option is added if it is not in the list). In DAB: disabled, one option `"<rfRateHz> MS/s (DAB)"`. In ADS-B: disabled, title "Held in ADS-B — the receiver keeps its own capture rate" | hwinfo `rates`, `lockedRate`, `rateNow` | `{"type":"sampleRate","value":<int Hz>}` on change, and saves pref `sampleRate`. Dropped client-side while ADS-B/ISM/AIS/NDB is held. Asserted on load only when the server gives no `rateNow` (saved pref, else 2.4 M, else the fastest ≤2.4 M) | hidden on airspyhf. Hidden when `lockedCentre`>0 or (shared dial and not admin); `#rateLocked` then shows instead. Shim: local_sdr_shim.cpp:20963 | index.html:6237-6240; main.ts:18301-18432 |
| `SAMPLE RATE` (locked variant), text `"<x> MS/s — set by the server."` or `Set by the server.` | read-only `#rateLocked .val` | — | `lockedRate` | nothing | only when the rate is not yours (see above); never on the handheld unless the centre is locked | index.html:6242-6245; main.ts:18301-18311 |
| `IF FILTER` | select `#tunerBw` | Options: `-1` "Auto — follows zoom", `0` "Wide — set by sample rate", then the widths. Markup default list: 2.8 MHz, 2.0 MHz, 1.4 MHz, 1.0 MHz, 700 kHz, 500 kHz, 350 kHz — narrowest. **That list is replaced by hwinfo `tunerBws[]`** sorted descending, labelled `"x.x MHz"` / `"N kHz"`, with ` — narrowest` on the last. Options wider than `ifCap` (and Wide) are disabled, title "The owner has capped this band at N kHz". Shows `-1` when `tunerBwAuto`, else `tunerBw` | hwinfo `tunerBw`, `tunerBwAuto`, `tunerBws`, `ifCap` | `{"type":"tunerbw","value":<int>}` (-1 auto, 0 wide, or Hz). Dropped client-side while ADS-B/ISM/AIS/NDB is held. **Not saved in prefs** | class `rtlOnly`. Whole row hidden on a shared dial or a locked multi-listener range unless admin (syncIfMenu main.ts:3404). Shim: local_sdr_shim.cpp:20633 (sharedGate) | index.html:6267-6303; main.ts:2561-2597, 3404-3433, 18355-18358 |
| `PPM`, value `#ppmVal` | slider `#ppm` | -50…50, step 1, default 0 | pref `ppm` (painted). Pushed on connect **only if adminUnlocked** (main.ts:18113) | `{"type":"ppm","value":<int>}` on every input. Saves pref `ppm`. Shim: local_sdr_shim.cpp:20950 (adminGate "ppm") | class `rtlRspOnly`: shown on rtl and sdrplay | index.html:6341-6345; main.ts:17663-17668 |
| `BIAS-T`, button `OFF`/`ON` | toggle `#biasT` | default OFF | pref `biasT`. Painted from hwinfo `biasT` (main.ts:2092) | `{"type":"biasT","on":<bool>}`. ★ **`toggle()` with pushOnInit=true: it SENDS the saved pref (or false) once at page build** (main.ts:17725). Saves pref `biasT`. Shim: local_sdr_shim.cpp:20917 (adminGate, persisted `biasT`) | class `rtlOnly` | index.html:6346-6349; main.ts:17725 |
| `DIGITAL AGC`, button `OFF`/`ON` | toggle `#agc` | default OFF | hwinfo `digitalAgc` (onDigitalAgc main.ts:2137). Pref `agc` is saved but never pushed (pushOnInit=false) | `{"type":"agc","on":<bool>}`: the RTL2832's own AGC. Shim: local_sdr_shim.cpp:20929 (sharedGate; OFF refused while `agcLocked` for a non-admin; persisted `ifAgc`) | class `rtlOnly` | index.html:6350-6353; main.ts:17729 |
| `DIRECT SAMPLING` + note `#dsNote` (verbatim, below) + `#dsAutoNote` | segmented `#dsSeg`: `OFF`(data-ds 0), `ON`(2), `AUTO`(-1) | default OFF. AUTO is shown only when the server sent `autoDs` (hwHasAutoDs). The segment shows AUTO when `autoDs` is true | hwinfo `autoDs`, `dsBelowHz`, `dsActive`. Pref `directSampling` (pushed on connect only if adminUnlocked, main.ts:18128) | OFF/ON → if auto was on, first `{"type":"autoDirectSampling","value":0}`, then `{"type":"directSampling","value":0|2}`. AUTO → `{"type":"autoDirectSampling","value":1}`. Saves pref `directSampling`. Shim: local_sdr_shim.cpp:21064 (adminGate), 21084 | class `rtlOnly` (block layout) | index.html:6354-6381; main.ts:17736-17740, 20007-20051 |

**`#dsNote`, default text** (index.html:6359-6363):
"To receive HF and below — the short wave bands, medium wave and long wave — this radio needs direct sampling ON. The tuner is bypassed, so the gain controls go with it, and the VHF and UHF bands stay quiet until you switch it back off."

**On a Blog V4/V4L** (`radio.model` matches `/\bblog\s*v4l?\b/i`), the note is replaced by (main.ts:20027-20029, with the model name in bold):
"This radio is **<model>** and does **not require direct sampling**: it has an up-converter built in, so short wave, medium wave and long wave already arrive through the tuner. Leaving it off is correct here — switching it on bypasses the tuner and takes the gain controls with it."

**`#dsAutoNote`** shows only when `autoDs` is true and the radio is not a V4:
"This receiver's owner has set AUTOMATIC direct sampling: the server switches the tuner out by itself below <dsBelowHz MHz> MHz, and back in above it."

**RTL toast:** a VTS notice — `AGC: On  |  IF Filter: Auto`, `IF Filter: Auto` or `AGC: On`, for 8 s — appears on an rtl that is running its own automation (main.ts:3323-3336). It is informational only.

---

### 4a.3 `sdrplay` (SDRplay RSP) — panel `#rspCtls` (index.html:5947-6031)

On sdrplay these rows are hidden: `#rowGain`, `#rowAgc`, `.rtlOnly` (IF FILTER, BIAS-T, DIGITAL AGC, DIRECT SAMPLING). These are shown: `SAMPLE RATE` (labels with ADC bits) and `PPM` (`.rtlRspOnly`).

**Every RSP message** is `{"type":"rsp_control", <key>: <value>}` (rspSend, main.ts:20188). The shim handler is local_sdr_shim.cpp:19256 (sharedGate "gain" at the top).

**On connect,** unless `rspRestricted()`, `pushAllRspSettings()` (main.ts:20422) asserts the saved state:
1. `{type:"rsp_control", lna:<state>?, agcset, rfnotch, dabnotch, biast}`. `lna` is left out while RF AGC is on.
2. `{type:"rsp_control", ifagc:0|1}`.
3. If the IF AGC is off, `{type:"rsp_control", ifgr}`.

**Prefs:**
- Sliders: `rsp_lna` (the slider *position*), `rsp_ifgr`, `rsp_agcset`.
- Toggles: `rsp_ifagc` (default **on**), `rsp_rfagc`, `rsp_rfnotch`, `rsp_dabnotch`, `rsp_biast`, `rsp_amnotch`, `rsp_hdr`, `rsp_extref` (all default off). Defined at main.ts:20408-20414.

| Label / help (verbatim) | Type | Options / range / default | State from | Sends | Shown when / notes | Ref |
|---|---|---|---|---|---|---|
| `SYSTEM GAIN` `#rspSysGain` = `"<x.x> dB"` or `—`; `OVERLOAD` badge `#rspOverload` | read-only | — | rspstat `sysGain`, `overload` | — | always in the panel | index.html:5950-5954; main.ts:2439-2441 |
| AGC status line `#rspAgcLine` (text plus a `.why` span; `.warn` = stuck, amber) | read-only | — | rspstat fields via src/services/rspAgcReadout.ts (rspAgcFeed / rspAgcDraw) | — | sdrplay only. Cleared on other drivers | index.html:5958; main.ts:3480-3505 |
| `RF GAIN (LNA)`, value `#rspLnaVal` = `"<pos>/<max> · LNA <state>"` + ` · max` / ` · min` + ` · AGC` | slider `#rspLna` | 0 … `lnaStates-1` (rspstat `lnaN` overrides). **Right = more gain**: the wire `lna` = max − pos. Default mid (`floor((n-1)/2)`) unless pref `rsp_lna` is set | radio.`lnaStates`; rspstat `lna` moves the thumb | `{"type":"rsp_control","lna":<state>}`. Ignored while RF AGC drives the LNA (read-only, `.agc` class) | hidden when restricted | index.html:5960-5967; main.ts:20511-20517 |
| `IF GAIN RED.`, value `"<n> dB"` + ` · max gain` (≤20) / ` · min gain` (≥59) + ` · VibeAGC` / ` · IF AGC`; title when manual: "IF gain reduction, in dB. Available only with the IF AGC off."; when locked: "Locked: the owner has locked the IF AGC on, and the AGC owns this gain." | slider `#rspIfGr` | min `ifGrMin` (20), raised to the owner's `ifGrFloor` (title "The owner requires at least N dB of IF gain reduction on this band"); max `ifGrMax` (59); default 40 | radio.`ifGrMin/Max`, hwinfo `ifGrFloor`; rspstat `ifgr` tweens the thumb | `{"type":"rsp_control","ifgr":<dB>}`. Refused client-side while `agcLocked`; read-only while the IF AGC is on | always in the panel | index.html:5968-5972; main.ts:20518-20521, 20446-20460 |
| `AGC` + `#rspVagcVal` note; buttons `AUTO`(0) "VibeAGC with the OpenRSP driver, the SDRplay's own IF AGC with SDRplay's official API" · `VIBEAGC`(1) "VibeAGC: VibeSDR's own gain control for both stages of the RSP" · `SDRPLAY`(2) "The RSP's own IF AGC (in the SDRplay API), with the RF AGC row stepping the LNA" | segmented `#rspVagcSeg` | 0/1/2 | radio.`vagcMode`, `vagcAuto` | `{"type":"rsp_control","vagcmode":0|1|2}`. Owner only (adminGate "the AGC choice", shim 19256+107). If the page is not the owner, the buttons are hidden and the choice is shown as text | row `#rowRspVagc` only when `radio.vagcMode` is a number. Owner = adminUnlocked, or (no admin password AND srvLocal) → **true on the handheld** | index.html:5978-5986; main.ts:3516-3560 |
| `IF AGC` + tag `#rspIfAgcBy` (` · VIBEAGC` / ` · SDRPLAY`); button text stays `AUTO`, lit = on; title "AUTO: the SDRplay's own IF AGC. Tap for MANUAL gain." or, under VibeAGC, "AUTO: VibeAGC runs the IF (and the RF, with RF AGC on). Tap for MANUAL gain — that stops VibeAGC."; when locked: "The owner has locked the IF AGC on for this receiver, so the manual IF gain is not available." | toggle `#rspIfAgc` | default ON | pref `rsp_ifagc`; hwinfo `agcLocked` (forces on) | `{"type":"rsp_control","ifagc":0|1}`. Turning it off also sends `{..."ifgr":<slider>}` | hidden when restricted | index.html:5987-5990; main.ts:20473-20489, 20527-20543 |
| `RF AGC` + tag `#rspRfAgcBy`; button `AUTO` (lit = on) | toggle `#rspRfAgc` | default OFF (pref `rsp_rfagc`) | painted from the server's `rfAgc` when restricted (main.ts:2085) | `{"type":"rsp_control","rfagc":0|1}`. Turning it off also sends `{..."lna":<state>}`. The server persists `rfAgc` | hidden when restricted | index.html:5996-5999 |
| `AGC TARGET` + tag `#rspAgcSetBy`, value `"<n> dBfs"` + ` · DAB` / ` · default` (−30) / ` · hard` (≥−25) / ` · gentle` (≤−60), or VibeAGC's target text | slider `#rspAgcSet` | −72…−10, default −30, snaps to −30 within ±2 | pref `rsp_agcset` | `{"type":"rsp_control","agcset":<dBfs>}`. Read-only (`.agc`) while VibeAGC is running; title then "VibeAGC's own target (the ADC peak it holds), set per band by VibeAGC — not adjustable here. Choose SDRPLAY on the AGC row to steer the SDRplay's own AGC with this slider." | row `#rowAgcSet` hidden when restricted | index.html:6003-6007; main.ts:20522-20526 |
| `ANTENNA` (RSP's own row) | segmented `#rspAntSeg` | — | — | (would be `{"type":"rsp_control","antenna":<port>}`) | **Always hidden now** (renderRspAntenna main.ts:20166-20187): the generic ANTENNA section (4a.8) replaces it | index.html:6014-6017 |
| `FILTERS` + tag `#notchOwner` (` · AUTO` / ` · OPERATOR`) | button group | — | see the next six rows | — | row `#rowRspNotch` hidden when restricted | index.html:6018-6031 |
| ↳ `RF NOTCH`; title "Broadcast notch — covers MW and FM. Wanted ON elsewhere in the spectrum, where a strong local broadcast transmitter is what overloads the front end, and OFF when MW or FM is what you came to hear" | toggle `#rspRfNotch` | default OFF | pref `rsp_rfnotch`; server `rfNotch` (onDspState); rspstat `autoNotch` / `userNotch` | `{"type":"rsp_control","rfnotch":0|1}`. **Click ignored** while autoNotch is on or userNotch is false (greyed `.agc`; title explains) | only if radio.`rfNotch` | main.ts:20044-20046 |
| ↳ `DAB NOTCH`; title "DAB band notch, same reasoning as the FM notch" | toggle `#rspDabNotch` | OFF | pref `rsp_dabnotch` | `{"type":"rsp_control","dabnotch":0|1}` (same auto rule) | only if radio.`dabNotch` | |
| ↳ `BIAS-T`; title "Bias-T — powers an active antenna or a mast-head amplifier" | toggle `#rspBiasT` | OFF | pref `rsp_biast`; server `rspBiasT` | `{"type":"rsp_control","biast":0|1}` (adminGate) | only if radio.`biasT`; its row is hidden when restricted | |
| ↳ `AM NOTCH`; title "MW notch — cuts the medium-wave broadcast band, separate from the RF notch" | toggle `#rspAmNotch` | from radio.`amNotchOn` | radio.`amNotchOn` | `{"type":"rsp_control","amnotch":0|1}` | only if radio.`amNotch` (RSPdx / dx-R2) | |
| ↳ `HDR`; title "High dynamic range — the dx family's path below 2 MHz, where a strong medium-wave signal is what overloads the front end" | toggle `#rspHdr` | from radio.`hdrOn` | | `{"type":"rsp_control","hdr":0|1}` | only if radio.`hdr` | |
| ↳ `24 MHz REF`; title "24 MHz reference output — for locking another receiver to this one's clock" | toggle `#rspExtRef` | from radio.`extRefOn` | | `{"type":"rsp_control","extref":0|1}` | only if radio.`extRef` | |

**Gain-stuck chip:** `#mGainStuckFloat`, on the main screen rather than in the menu. When rspstat reports `gainStuck` it reads "SDRPLAY GAIN API FAILURE · TAP TO RESET (MAY CAUSE AN AUDIO DROP)". A tap sends `{"type":"rsp_agc_restart"}` (main.ts:9782-9787).

---

### 4a.4 `airspyhf` (Airspy HF+) — panel `#ahfCtls` (index.html:6044-6100)

On airspyhf these rows are hidden: `#rowGain`, `#rowAgc`, **SAMPLE RATE** (and `#rateLocked`), `.rtlOnly` and `.rtlRspOnly` (no PPM).

**Every HF+ message** is `{"type":"ahf_control", …}` (main.ts:19487). The shim handler is local_sdr_shim.cpp:19158.

**On every caps message,** `applyAhfCaps` → `pushAllAhfSettings()` **asserts unconditionally**:
1. `{type:"ahf_control", att, lna, thresh, ppb}`.
2. `{type:"ahf_control", agc}`.

**Prefs:** `ahf_agc`, `ahf_lna` (bool), `ahf_att`, `ahf_ppb`, `ahf_thresh`.

| Label / help (verbatim) | Type | Options / default | Sends | Shown / notes | Ref |
|---|---|---|---|---|---|
| `AGC`; button `AUTO` / `MANUAL` | toggle `#ahfAgc` | default ON (AUTO) | `{"type":"ahf_control","agc":1|0}`. Turning it off also sends `{..."att":<n>}`. Under AUTO, ATTENUATION is disabled (opacity .45) and THRESHOLD is enabled; under MANUAL it is the other way round | always | main.ts:19555-19563 |
| `AGC THRESHOLD`; label title "Airspy: the threshold tells the AGC to tolerate an extra 3 dB of signal power before setting the next attenuation level. HIGH = the front end is MORE sensitive, and is what they recommend when chasing marginal signals in the presence of very strong blockers (~100 dB apart). LOW = less sensitive, engaging earlier."; `LOW`(0) "Backs off earlier. Conservative — for a crowded band with strong local stations." · `HIGH`(1) "Backs off later, keeping more gain. Better for weak stations; more overload risk." | segmented `#ahfThreshSeg` | default LOW | `{"type":"ahf_control","thresh":0|1}` | enabled only under AGC AUTO | index.html:6056-6062; main.ts:19583 |
| `ATTENUATION`; title "Airspy's own rule of thumb with the AGC off: push the attenuation until the noise floor sits around -100 dBFS. Less attenuation than that does not necessarily improve SNR, but it definitely costs dynamic range. If in doubt, turn the AGC on and let it decide."; value `"<n×6> dB"` (+ ` · none` at 0), or `set by AGC` | slider `#ahfAtt` | 0 … `attSteps-1` (default 8 steps); step dB = `attStepDb` (6); default 0 | `{"type":"ahf_control","att":<step index>}` | disabled under AUTO | index.html:6067-6071; main.ts:19571-19575 |
| `PREAMP`; title "Airspy calls this misleading: it is really a POSTAMP, acting on the ADC and DSP back-end rather than the antenna. Turning it OFF buys about 12 dB of dynamic range by scrapping the quantisation-noise margin. Leave it off unless the band is genuinely quiet and the signal is weak." | toggle `#ahfLna` `ON`/`OFF` | default OFF | `{"type":"ahf_control","lna":0|1}` | always | index.html:6085-6088; main.ts:19564-19570 |
| `CALIBRATION`; value `"<n> ppb"` | slider `#ahfPpb` | −5000…5000, step 5, default 0; detent: \|v\| ≤ 20 → 0 | `{"type":"ahf_control","ppb":<int>}` (adminGate "calibration") | always | index.html:6093-6099; main.ts:19576-19582 |

---

### 4a.5 `airspy` (Airspy R2 / Mini) — panel `#aspCtls` (index.html:6110-6169)

On airspy, **`#rowGain` STAYS** (it is the preset curve, 0-21, from hwinfo `gains[]`). Hidden: `#rowAgc`, `.rtlOnly`, `.rtlRspOnly`. SAMPLE RATE is shown.

**Every R2/Mini message** is `{"type":"airspy_control", …}` (main.ts:19613). The shim handler is local_sdr_shim.cpp:19089.

**On caps** it pushes `{type:"airspy_control", mode}`, unless an owner band limit is in force.

**State** is read from the radio: `gainMode`, `lna`, `mixer`, `vga`, `lnaAgc`, `mixerAgc`, `biasT`, `packing`, `hasPacking`, `stageMax`, `aspLimit`. **Pref:** `asp_mode`.

| Label / help (verbatim) | Type | Options / default | Sends | Shown | Ref |
|---|---|---|---|---|---|
| `GAIN MODE`; title "Sensitive and Linear set all three stages from the one gain slider above. Free hands you the three stages. Linear is the safer default on a busy band."; `SENSITIVE`(0) "Digs for the weakest signal; more prone to overload on a crowded band." · `LINEAR`(1) "Keeps strong neighbours from spoiling what you are on." · `FREE`(2) "Set the LNA, mixer and VGA yourself." | segmented `#aspModeSeg` | default LINEAR; painted from radio.`gainMode` | `{"type":"airspy_control","mode":0|1|2}` | hidden when radio.`aspLimit` is set. It is replaced by the note `#aspLimitNote`: "Gain: <Linearity|Sensitivity> <max> — locked by the owner on this band." or "Gain: <curve>, up to <max> of 21 — limited by the owner on this band. Free mode and the manual stages are off here." | index.html:6120-6127; main.ts:19675-19704, 19736 |
| `LNA AGC`; title "The radio's own automatic gain for the LNA. With this on, the LNA slider below is set by the radio and yours does nothing." | toggle `#aspLnaAgc` ON/OFF | from radio.`lnaAgc` | `{"type":"airspy_control","lnaAgc":0|1}` | FREE mode only (`.aspFreeOnly`) | index.html:6133-6136 |
| `MIXER AGC`; title "The radio's own automatic gain for the mixer. With this on, the mixer slider below is set by the radio and yours does nothing." | toggle `#aspMixerAgc` | from radio.`mixerAgc` | `{"type":"airspy_control","mixerAgc":0|1}` | FREE only | 6137-6140 |
| `LNA` value or `set by AGC` | slider `#aspLna` | 0…`stageMax` (15), step 1 | `{"type":"airspy_control","lna":<n>}` | FREE only; disabled while LNA AGC is on | 6143-6147 |
| `MIXER` | slider `#aspMixer` | 0…15 | `{"type":"airspy_control","mixer":<n>}` | FREE only; disabled while MIXER AGC is on | 6148-6152 |
| `VGA`; title "The VGA has no automatic mode on this radio — it is always yours to set." | slider `#aspVga` | 0…15 | `{"type":"airspy_control","vga":<n>}` | FREE only | 6153-6157 |
| note `#aspManualNote`: "The VGA has no automatic setting on this radio — it is always yours." | text | | | FREE only | 6158 |
| `BIAS-T`; title "4.5 V on the aerial socket for a powered antenna or an LNA. DC into the wrong antenna can damage it." | toggle `#aspBiasT` | from radio.`biasT` | `{"type":"airspy_control","biast":0|1}` (adminGate) | always | 6159-6162 |
| `USB PACKING`; title "Sends samples packed 12-bit instead of padded to 16, which cuts the USB bandwidth by a quarter. Useful on a shared hub or a marginal cable; no effect on what you hear." | toggle `#aspPacking` | from radio.`packing` | `{"type":"airspy_control","packing":0|1}` | hidden when radio.`hasPacking === false` | 6165-6168 |

The shim also accepts `curve` in `airspy_control` (local_sdr_shim.cpp:19089+117). The client does not send it.

---

### 4a.6 `hackrf` (HackRF One, EXPERIMENTAL) — panel `#hrfCtls` (index.html:6195-6236)

On hackrf these rows are hidden: `#rowGain`, `#rowAgc`, `.rtlOnly`, `.rtlRspOnly`. SAMPLE RATE is shown. **The whole panel is hidden when `gainLocked`.**

**Every HackRF message** is `{"type":"hackrf_control", …}` (main.ts:19751). The shim handler is local_sdr_shim.cpp:19010.

**On caps** it pushes `{lna, vga}` from the sliders, unless restricted. Amp and bias-T are **never** pushed from prefs. **Prefs:** `hrf_lna`, `hrf_vga`.

| Label / help (verbatim) | Type | Options / default | Sends | Notes | Ref |
|---|---|---|---|---|---|
| `HACKRF` · `EXPERIMENTAL · manual gain` | read-only | | | | 6196-6199 |
| `RF AMP (owner)`; title "A 0 or +14 dB amplifier ahead of everything (the U14 MGA-81563). It is the part of a HackRF that most commonly dies: the front end is largely unprotected, maximum safe RX power is -5 dBm, and static or a nearby transmitter can destroy it. A blown one goes DEAF — switching the amp on makes signals drop instead of rise. Owner-only, and off unless you know the aerial." | toggle `#hrfAmp` ON/OFF | from radio.`amp` | `{"type":"hackrf_control","amp":0|1}`. Turning it ON first shows `confirm()`: "Switch the HackRF RF amp ON?\n\n+14 dB in front of everything, on a radio whose front end is unprotected. Maximum safe input is -5 dBm — a strong signal or a static discharge can destroy the amplifier.\n\nOnly do this if you know what is on the aerial." | **A native confirm dialog is needed** | 6213-6216; main.ts:19900-19910 |
| `LNA GAIN`; title "The front-end (IF) gain stage, 0-40 dB in 8 dB steps. Raise this BEFORE the VGA: gain taken here costs the least noise figure. Back it off if the waterfall shows images or the audio breaks up on peaks — an 8-bit radio clips where others cope."; value `"<n> dB"` (+ ` · none`) | slider `#hrfLna` | 0…40, step 8, default 0 (from radio.`lna`) | `{"type":"hackrf_control","lna":<dB>}` | LNA+VGA ≤ `gainCap`/10. Each slider's max is the cap minus the other slider (applyHrfGainCap main.ts:19784-19827). Disabled when restricted | 6218-6222 |
| `VGA GAIN`; title "The baseband (VGA) gain stage, 0-62 dB in 2 dB steps. It comes AFTER the mixer, so it amplifies noise along with signal — use it to top up once the LNA is set, not as the main control." | slider `#hrfVga` | 0…62, step 2, default 0 | `{"type":"hackrf_control","vga":<dB>}` | same cap rule | 6225-6229 |
| `BIAS-T (owner)`; title "Puts 3.3 V at 50 mA on the antenna port to power a mast-head amplifier. Leave it OFF unless you know what is on the end of the coax — owner-only." | toggle `#hrfBiasT` | from radio.`biast` | `{"type":"hackrf_control","biast":0|1}` (adminGate) | | 6232-6235 |

---

### 4a.7 `usrp` (Ettus USRP B2xx, EXPERIMENTAL) — panel `#usrpCtls` (index.html:6185-6194)

On usrp, **`#rowGain` STAYS**, using hwinfo `gains[]` and sending the generic `{"type":"gain","value":…}`. Hidden: `#rowAgc` (VibeAGC), `.rtlOnly`, `.rtlRspOnly`. SAMPLE RATE is shown.

| Label / help | Type | Sends | Shown | Ref |
|---|---|---|---|---|
| `RADIO AGC`; title "The USRP's own automatic gain, in its AD9361 chip — not VibeSDR's AGC. Moving the gain slider turns it off." | toggle `#usrpAgc` ON/OFF; state from radio.`hwAgc && agcOn` | ON → `{"type":"gain","auto":true}`. OFF → `{"type":"gain","value":<gains[slider]>}` | only when radio.`hwAgc`. Hidden when `gainLocked` | main.ts:19842-19855 |
| `REFERENCE` `#usrpRef` = `GPSDO` / `External 10 MHz` / <clock>, + ` · locked` / ` · NOT locked`, + ` · GPS locked` / ` · no GPS lock` | read-only | — | only when radio.`clockSources` is non-empty and `clock` ≠ `internal` | main.ts:19856-19867 |

---

### 4a.8 ANTENNA section — every driver (index.html:6386-6391)

| Label | Type | State from | Sends | Shown | Ref |
|---|---|---|---|---|---|
| heading `ANTENNA`; row `IN USE` + ` · OWNER` tag when radio.`antennaLocked` | segmented `#antSeg`: one button per radio.`antennas[]`. Label = the owner's name from radio.`antennaNames{port:name}` (portLabel). Lit = radio.`antenna`. Title `Switch to <label>`, or when locked "The owner has fixed the aerial for this receiver" (disabled) | radio.`antennas`, `antenna`, `antennaLocked`, `antennaNames`, `antennaSwitch` | `{"type":"antenna","antenna":"<port name, not the label>"}`. Shim: local_sdr_shim.cpp:19237 (adminGate when locked) | only when `antennas.length ≥ 2`. Locked only if `antennaLocked` and the page is not admin (always admin on the handheld unless a password is set) | main.ts:20135-20164 |
| note `#antNote` | text | radio.`antennaSwitch` | — | "The antenna switch is not answering (<error>)." / "The switch reports more than one antenna connected." / "Waiting for the switch to say which antenna is connected." | |

---

### 4a.9 `icom` (for completeness)

The CSS rule `body.icom` (index.html:898-901) hides `.icomHide`, `#rowRate`, `#rateLocked`, `#rowGain`, `#rowAgc`, `#rowTunerBw`, `.rtlOnly`, `.rtlRspOnly` and every SDR panel. Instead, `#icomCtls` is built by icomUi.ts `renderIcomPanel` from hwinfo `radio` (parseIcomRadio). Its rows, each drawn only when it has more than one option:
- `RADIO MODE`
- `FIL1/2/3`
- the scope span
- `SCOPE`
- `SCOPE SPEED`
- `ATT`
- `PREAMP`
- `AGC` (sub "the radio’s own")
- toggles
- an RF gain slider (`rfGain`)

All of them send `{"type":"icom_control", …}` via `spec.sendIcomControl` → icomControlMsg (spectrum.ts:2130; shim local_sdr_shim.cpp:18434). This is not expected on the R36S (USB SDRs only).

---

### 4a.10 Admin rows inside this range (hidden on the handheld)

| Row | Label / text | Notes | Ref |
|---|---|---|---|
| `#adminRow` | `PROTECTED SETTINGS` / `UNLOCK` | **hidden on the handheld** (injectStyle) | index.html:6315-6318 |
| `#adminPwRow` | `PASSWORD` input + `GO` | **hidden on the handheld** | 6322-6327 |
| `#adminNote` | "Radio controls are locked by this receiver's operator. Gain, AGC, filters and calibration change the radio for everyone listening, not just for you, and Bias-T puts DC on the feedline. You can still see what they are set to. Enter the admin password to unlock them." | NOT in the handheld hide list. It only shows on an admin-protected, locked server, so in practice it never appears. Native: omit | 6335-6339 |
## 4b. MENU → DISPLAY, GAME CONTROLLER, rest

How DISPLAY is assembled on the handheld (`web/client/src/handheld.ts:636-657`):

1. The handheld's own four rows: BRIGHTNESS, SPECTRUM RATE, DRAWING RATE, SCREEN OFF AFTER (documented in §4a, not here).
2. The two `.mOnly` view rows taken out of the RADIO section (`index.html:5895-5904`): VFO CENTRE, then CENTRE / FIT.
3. Every `#menu` node from `<h2>DISPLAY</h2>` (`index.html:6393`) up to `<h2>GAME CONTROLLER</h2>` (`index.html:6669`). That covers the sub-headings DISPLAY, WATERFALL, SPECTRUM, VFO and CONTROLS, which stay in place as plain `<h2>` sub-headings. Only the first `<h2>DISPLAY</h2>` is removed (`handheld.ts:652`).
4. Hidden with `.hhHide`: the `.mrow` rows holding `#wfScrollSeg`, `#wfDataSeg` and `#wfBoostSeg`, plus `#wfScrollNote` (`handheld.ts:656-657`).
   - ★ **Two notes are never hidden.** `#wfDataNote` and `#wfBoostNote` do not exist as ids: both notes are bare `<div class="rowNote">` (`index.html:6476`, `index.html:6497`). On the handheld the help text for DATA and SPEED UP WHILE TUNING still shows with no control above it. The native UI should leave both notes out.

**Persistence.** "Pref" means a key in `localStorage['vibesdr_web_prefs_v1']`, read with `prefs()` and written with `savePref(k, v)`. The handheld's per-radio memory keeps every key except those matching `GLOBAL_KEY` (`handheld.ts:113`):
`/^(palette|ctl|freqUnit|vfoColor|volume|specRatio|specShow|wf(Scroll|Speed|Boost|Data|DataRate)$|lastHost|tapSnap|ptrFreq|font|look|theme|disp|padTuck|wheelTunes)/`.

- **G** means GLOBAL: the device's setting, whichever radio is plugged in.
- **R** means PER-RADIO: saved to the engine with POST `/handheld/memory` and put back when that radio returns.
- ★ **Most of the waterfall and spectrum look is R, not G.** The comment at `handheld.ts:110` says the look is the device's, but only some keys match the regex. Per-radio: `autoContrast`, `minDb`, `maxDb`, `wfBrightness`, `wfContrast`, `wfSharpness`, `wfCoarse`, `spatialSmooth`, `smoothingFrames`, `specFloor`, `specPeakScale`, `specAlpha`, `peakHold`, `knownLabels`, `vfoIntensity`, `vfoFrost`, `wheelAction`. The regex has `wheelTunes`, but the pref key is `wheelAction`. Global: `palette`, `specRatio`, `specShow`, `vfoColor`, `ctl*`, `ptrFreq`, `padTuck`.

**Effect.** Every DISPLAY row is **client-only rendering**: a Waterfall property or `wf.applySettings({...})`, with no WS message. The exceptions are VFO CENTRE, CENTRE and FIT (see below). There are no audio-path controls in this slice, so no speaker-mode flag applies.

**Shared helpers.**

| Helper | Defined at | Behaviour |
|---|---|---|
| `slider(id, valId, fmt, apply, prefKey)` | `main.ts:17399` | Loads the saved number if there is one, otherwise the HTML `value`. Applies at once on load. Saves on every `input`. |
| `toggle(id, apply, prefKey, initial)` | `main.ts:17505` | Writes the text ON or OFF and the `.on` class. Initial state is the saved boolean, otherwise `initial`. Applies on load. |
| `segment(id, attr, apply, prefKey)` | `main.ts:17586` | The default is the button marked `.on` in the HTML. Does not fire `apply` on load. |

### Moved from RADIO (`.mOnly`)

| Label (verbatim) | Type | Options / default | State / pref | What it does | Shown | Ref |
|---|---|---|---|---|---|---|
| **VFO CENTRE**. Button title: "LOCKED: the view stays centred on the dial. FREE: pan the view independently." | Toggle button `#mPanelLock`. Its text is the state: `LOCKED` or `FREE`. Gets class `.free` when FREE. | LOCKED (default at every load) | `spec.followVfo` (`spectrum.ts:780`). **Not persisted.** | Flips `followVfo`. Afterwards every view message is `{"type":"zoom","frequency":<Hz>,"binBandwidth":<Hz/bin>}` with `"free":1` added while FREE (`spectrum.ts:2533`). Also redraws the walls and RF-centre marker (`updateViewOverlays`). | Hidden on an Icom (`icomHide`) | `index.html:5895`, `main.ts:9773-9781` |
| **CENTRE**. Title: "Snap the view back onto the dial" | Button `#mPanelCentre` | — | — | `spec.pan(spec.frequency)` sends `{"type":"zoom","frequency":<dial Hz>,"binBandwidth":<current>[, "free":1]}` (`spectrum.ts:2410`). The floating `#mCentreFloat` does the same thing. | Always | `index.html:5901`, `main.ts:9800-9802` |
| **FIT**. Title: "Fit the whole band" | Button `#mPanelFit` | — | — | `spec.resetView()` sends `{"type":"reset"}`. Ignored while DAB, ADS-B, ISM, AIS or NDB holds the view (`spectrum.ts:2414`). | Always. **Disabled** in DAB, ADS-B and NDB (`wholeRadioLock`), with the title replaced by the reason, e.g. "Zoom is locked in DAB — the span is the multiplex". | `index.html:5902`, `main.ts:9680`, `main.ts:9769` |

The row's third grid cell is an empty `<span>`.

### DISPLAY

| Label (verbatim) | Type | Options / range / default | Pref (G/R) | What it does | Shown | Ref |
|---|---|---|---|---|---|---|
| **COLOUR MAP** | `<select id="palette">` | The 26 names in `COLORMAP_NAMES` (`src/assets/colormapUtils.ts:98`), sorted case-insensitively. Default `gqrx`. | `palette` (G) | `wf.setPalette(name)` | Always | `index.html:6394`, `main.ts:17851-17858`, `main.ts:1470` |
| **SPECTRUM / WATERFALL** | Range `#specRatio` with `#specRatioVal` showing "25%" | 0–60, step 1, default 25 | `specRatio` (G), stored as the slider value, not a fraction | `wf.setSpecRatio(v/100)`. **Disabled and dimmed** while the spectrum trace is hidden. | Always | `index.html:6398`, `main.ts:17860`, `main.ts:17928` |
| **ALL SERVERS**: SAVE FOR ALL VIBESDR.NET SERVERS | Button `#dispSaveAll` | — | — | Saves the view to the directory's store | Only on a `*.vibeserver.vibesdr.net` address (`portable.ts:92` `onVibeDomain`). **Never on the handheld (127.0.0.1)**, so leave it out. | `index.html:6404`, `main.ts:17950` |
| **RESET**: ↺ DEFAULTS | Button `#dispReset` | — | Deletes `autoContrast`, `minDb`, `maxDb`, `wfBrightness`, `wfContrast`, `wfSharpness`, `smoothingFrames`, `specFloor`, `specPeakScale`, `specAlpha`, `specRatio`, `spatialSmooth`, `peakHold`, `specShow`, `wfCoarse`, `palette` and `knownLabels`, then **reloads the page**. | Back to the defaults listed here | Always | `index.html:6414`, `main.ts:17963-17972` |

The help texts `#dispAllCaption` and `#dispAllHint` are hidden unless on vibesdr.net.

### WATERFALL

| Label (verbatim) | Type | Options / range / default | Pref | What it does | Shown | Ref |
|---|---|---|---|---|---|---|
| WATERFALL (MINIMUM · LOW · ECO · NATIVE · DEFAULT · SMOOTH) | Segmented `#wfScrollSeg` | — | `wfScroll` (G) | — | **Hidden on the handheld.** SPECTRUM RATE drives `wfScroll` instead (`sharp`, or `default` when interpolating; `main.ts:3145`). | `index.html:6449` |
| SPEED UP WHILE TUNING (ON/OFF) | Segmented `#wfBoostSeg` | — | `wfBoost` (G) | — | **Hidden on the handheld.** Forced off (`wfBoostWanted=false`, `main.ts:3146`). Its note still shows: "Speeds up the waterfall when tuning or zooming to make it feel more responsive. Turn off for signal accuracy, at the cost of it feeling slower." | `index.html:6491` |
| WATERFALL SPEED 10/20/30 | Segmented `#wfSpeedSeg` | — | `wfSpeed` | — | Hidden everywhere (`main.ts:10473`) | `index.html:6498` |
| DATA (LOW · AUTO · FULL) | Segmented `#wfDataSeg` | — | `wfData` (G) | — | **Hidden on the handheld.** Its note still shows: "How much spectrum data the server sends. Low saves data; Auto adapts to your connection; Full is the server's full rate." | `index.html:6512` |
| **RANGE** | Segmented `#wfCoarse` | AUTO (default) / MANUAL | `wfCoarse` (R), a string | AUTO: `wf.applySettings({manualRange:null})` and shows AUTO CONTRAST. MANUAL: `applySettings({manualRange:{minDb,maxDb}})` and shows MIN dB and MAX dB in place of AUTO CONTRAST. | Always | `index.html:6530`, `main.ts:17865-17877` |
| **AUTO CONTRAST** | Range `#autoContrast` | 0–20, default 5 | `autoContrast` (R) | `wf.applySettings({autoContrast:v})` | While RANGE is AUTO | `index.html:6537`, `main.ts:17880` |
| **MIN dB** | Range `#minDb`, value shown with a "−" sign | −160 to −60, default −120 | `minDb` (R) | `applySettings({manualRange:{minDb:min(lo,hi−1),maxDb:hi}})` | While RANGE is MANUAL | `index.html:6542`, `main.ts:17882-17888` |
| **MAX dB** | Range `#maxDb` | −100 to 0, default −20 | `maxDb` (R) | As MIN dB | While RANGE is MANUAL | `index.html:6547`, `main.ts:17889` |
| **MOUSE WHEEL** | Segmented `#wheelAction` | ZOOM (`data-v=0`, default) / TUNE (`1`) | `wheelAction` (R), a number | Sets `wheelTunes` | Always. No wheel on the R36S, so **dead on the handheld** and a candidate to drop. | `index.html:6555`, `main.ts:17894-17899` |
| **FREQUENCY AT POINTER** | Toggle `#ptrFreqTg` | ON (default) | `ptrFreq` (G) | The frequency label that follows the mouse over the waterfall | Only where the `HOVER_MQ` media query matches (a real pointer). Normally hidden on the R36S. | `index.html:6568`, `main.ts:17902`, `main.ts:17907` |
| **BRIGHTNESS** (the waterfall's, not the backlight's) | Range `#bright` | −20 to 20, default 0 | `wfBrightness` (R) | `wf.applySettings({wfBrightness:v})` | Always | `index.html:6573`, `main.ts:17891` |
| **CONTRAST** | Range `#contrast` | −10 to 10, default 0 | `wfContrast` (R) | `applySettings({wfContrast:v})` | Always | `index.html:6578`, `main.ts:17910` |
| **SHARPNESS** | Range `#sharp` | 0–10, default 0 | `wfSharpness` (R) | `applySettings({wfSharpness:v})` | Always | `index.html:6583`, `main.ts:17912` |
| **SPATIAL SMOOTH** | Toggle `#spatialSmooth` | ON (default) | `spatialSmooth` (R) | `applySettings({spatialSmooth:on})` | Always | `index.html:6588`, `main.ts:17914` |

★ **Name clash.** The waterfall BRIGHTNESS row sits in the same accordion as the handheld's BRIGHTNESS (the screen backlight, 1–255). Two rows with the same label: rename one natively.

The help text for the hidden WATERFALL row (`#wfScrollNote`) is hidden too.

### SPECTRUM

| Label (verbatim) | Type | Options / range / default | Pref | What it does | Shown | Ref |
|---|---|---|---|---|---|---|
| **TRACE** | Button `#specShow`. Its text is the action: **HIDE** while shown, **SHOW** while hidden. | Shown (default) | `specShow` (G), boolean | `wf.showSpec = on`. Also disables and dims SPECTRUM / WATERFALL while hidden. | Always | `index.html:6594`, `main.ts:17917-17932` |
| **SMOOTHING** | Range `#smooth` | 1–10, default 5 | `smoothingFrames` (R) | `applySettings({smoothingFrames:v})` | Always | `index.html:6598`, `main.ts:17934` |
| **FLOOR** | Range `#specFloor` | −20 to 20, default 0 | `specFloor` (R) | `applySettings({specFloor:v})` | Always | `index.html:6615`, `main.ts:17936` |
| **PEAK SCALE** | Range `#specPeak`, displayed as `(v/10).toFixed(1)+'×'`, e.g. "1.0×" | 1–30, default 10 | `specPeakScale` (R) | `applySettings({specPeakScale:v})` | Always | `index.html:6620`, `main.ts:17938` |
| **FILL OPACITY** | Range `#specAlpha`, displayed as "85%" | 10–100, default 85 | `specAlpha` (R) | `wf.specAlpha = v/100` | Always | `index.html:6625`, `main.ts:17940` |
| **PEAK HOLD** | Toggle `#peakHold` | ON (default) | `peakHold` (R) | `applySettings({peakHold:on})` | Always | `index.html:6630`, `main.ts:17942` |
| **SIGNAL LABELS** | Toggle `#knownLabels` | ON (default) | `knownLabels` (R) | `applyKnownSignals()`: known-signal, EiBi-utility and bookmark labels on the waterfall. Uses GET `/stations` (an HTTP fetch, not WS). Off: `wf.knownSignals=null`. | Always | `index.html:6636`, `main.ts:17437-17441`, `main.ts:17944` |

Help text for SIGNAL LABELS (`#knownLabelsHint`), verbatim: "Names the decoder for well-known signals in view — RTTY on DWD's 4583 kHz, WEFAX on Northwood's 4610, NAVTEX, DSC, FT8, time signals — and EiBi's utility stations on air now; and by name your own bookmarks, the receiver's saved ones and, on FM, station names it learnt from RDS. Drawn at the foot of the spectrum, or at the top of the waterfall when the spectrum is hidden. Click or tap a label to tune to it and open its decoder with that signal's settings (a drag still pans). EiBi's broadcast schedule stays in the station strip."

### VFO

| Label (verbatim) | Type | Options / range / default | Pref | What it does | Ref |
|---|---|---|---|---|---|
| **NEEDLE COLOUR** | `<input type=color id="vfoColor">` | Default `#ff2020` | `vfoColor` (G) | `wf.vfoColor`. Also VFO A's colour in A/B (`main.ts:20818`). | `index.html:6644`, `main.ts:17976-17980` |
| **GLOW** | Range `#vfoGlow` | 1–10, default 5 | `vfoIntensity` (R) | `wf.vfoIntensity = v` | `index.html:6648`, `main.ts:17982` |
| **FROSTED BACKING** | Range `#vfoFrost`, displayed as "OFF" at 0, otherwise the number | 0–10, default 0 | `vfoFrost` (R) | `wf.vfoFrost = v`. Fills black at alpha `(v/10)·0.72` behind the passband (`waterfall.ts:1388`). | `index.html:6653`, `main.ts:17984` |

All three are always shown.

### CONTROLS (controlColours.ts)

| Label (verbatim) | Type | Options / default | Pref | What it does | Ref |
|---|---|---|---|---|---|
| **BACKGROUND** | Swatch row `#ctlBgSw`, plus a custom colour picker | DEF (`''` = today's `#0a0a0a`), Green `#3dff72`, Red `#ff3a2e`, Amber `#ffae1a`, Blue `#3d9bff`, Teal `#46ffd7`, Neon `#ff7a26`, White `#eef3ff` | `ctlBg` (G), a hex string or `''` | `applyControlLook()`: CSS variables on the deck card background | `index.html:6663`, `controlColours.ts:39`, `main.ts:17988-18036` |
| **BUTTONS** | Swatches `#ctlBtnSw` plus a picker | The same eight colours. Colours the button **ring**; the fill stays dark. Today's is `#140a00`. | `ctlBtn` (G) | As above | `controlColours.ts:47` |
| **FONT** | Swatches `#ctlFontSw` plus a picker | DEF (`#ffb833`), White `#ffffff`, Silver `#c8c8c8`, Ice `#8fd3ff`, Gold `#ffd24d`, Black `#111111` | `ctlFont` (G) | As above. May show the warning `#ctlWarn` when the font colour was adjusted to stay readable. | `controlColours.ts:55`, `main.ts:17368-17372` |
| **TRANSPARENCY** | Range `#ctlSolid`, step 5. Label: 0 shows "GLASS", 100 shows "SOLID", otherwise "N%". | 0–100. Default 0, or the browser's automatic solid default when nothing is stored. | `ctlSolid` (G) | `applyCtlLook()` | `index.html:6670`, `main.ts:17375`, `main.ts:18039` |
| **RESET**: ↺ DEFAULT COLOURS | Button `#ctlReset` | — | Writes `ctlBg`, `ctlBtn` and `ctlFont` as `''` and `ctlSolid` as `null` | Restores today's look | `index.html:6677`, `main.ts:18042` |

Swatch tooltips are the swatch names; the DEF tooltip is "Default (as it has always looked)" and the picker's is "Custom colour".

Help text under TRANSPARENCY, verbatim: "Slide to SOLID to make every control and panel opaque. Solid is easier on older devices: nothing see-through is redrawn over the moving waterfall."

### GAME CONTROLLER

This section is **not** inside an accordion. It stays as an ordinary `<h2>` section in the menu, after the three accordions.

| Label (verbatim) | Type | Default | Pref | What it does | Ref |
|---|---|---|---|---|---|
| **GUIDE**: CONTROLLER GUIDE | Button `#btnPadGuide` | — | — | `togglePadGuide()` opens the `#padGuide` panel (`main.ts:11981`) on `activePadLayout()` (`gamepad.ts:453`). SELECT on the pad toggles it too. The panel has a CLOSE button (`#pgClose`) and the tabs TWO STICKS / BUTTONS (`#pgTabs`). D-pad left and right switch tabs. The guide's text is the data in `src/utils/padGuide.ts:100-165`. The R36S uses the **BUTTONS** layout: L back, R enter, top zoom in, right tune up, bottom zoom out, left tune down, SELECT for the guide, hold START 1 s to switch layout, tap START for VFO A↔B. | `index.html:6671`, `main.ts:17656`, `padGuide.ts` |
| **TUCK AWAY TUNE AND ZOOM KEYS** | Toggle `#padTuckTg` | ON | `padTuck` (G) | Sets the `body.padTuck` class when the toggle is on **and** a pad, keyboard or remote was the last input (`padTuckSync`, `main.ts:21004`). That hides `#mVfoDial` and `#mZoomDial`, the ‹ › and − + keys (`index.html:553`). | `index.html:6680`, `main.ts:17905` |

Help text under GUIDE, verbatim: "A game controller or a handheld drives this page: the D-pad moves a highlight, and the guide shows what every button does. SELECT on the controller opens it too. Enter on a text box opens an on-screen keyboard for the controller; touch, a click or a key on a real keyboard goes back to ordinary typing."

Help text under TUCK AWAY, verbatim: "While a game controller, a keyboard or a remote is driving, the ‹ › tune keys and the − + zoom keys slide into the control panel to make room — the controller tunes and zooms itself (see the guide), a keyboard with < > and − +. Touch the screen or use a mouse or trackpad and they slide back out. OFF leaves them where they are."

### SERVER and ABOUT on the handheld

- **SERVER** (`#adminServerRow`, SERVER ADMIN button `#btnServerAdmin`): always hidden on the handheld by the `injectStyle` CSS (`handheld.ts:245`). Leave it out.
- **ABOUT** (`index.html:6708` onward) is static text only, with links that open nothing useful in Cog.
  - `#aboutSrvVer` reads "This receiver runs VibeServer <version>." (`main.ts:881-889`). It comes from `/vibeserver.json` (`version`, `flavour`, `forkName`, `forkVersion`), and a fork reads "This receiver runs <fork> <forkVer>, a community fork of <flavour> <version>.".
  - The rest is credits paragraphs: GPL v3 and source, opus-decoder / libopus / MapLibre / PMTiles / Leaflet and map data, the NDB list, FAAD2 / libxaac, the Signal ID Wiki, QRZ.com, the DAB site regulators and the ADS-B data notice. These are verbatim in `index.html:6708-6770`; copy them as text natively.
## 4c. MENU → the handheld's own rows (handheld.ts `buildHandheldMenu`, 616-779)

**How the menu is built.**
- The accordion buttons are inserted after `#menu`'s own `#menuHead`, in this order: **R36S HARDWARE**, **SDR HARDWARE**,
  **DISPLAY**. They are `.btn.hhAcc` with `aria-expanded`; `▸` is drawn closed and `▾` open.
- They are one level deep, and opening one closes the others (`setOpen`, `handheld.ts:571-580`).
- An open accordion registers `pushBack`, so B / back collapses it.
- Opening any accordion calls `refreshDeviceRows()`.
- Everything after the accordions (GAME CONTROLLER, ABOUT) stays as plain sections (see 4b).

**Shared widgets.**
- Sliders here (`slider()`, `handheld.ts:562-568`) are `<input type=range>` with a value span. They send 120 ms after the
  last `input`.
- Segmented rows (`segOf`) mark the chosen button `.on`.

### 4c.1 DISPLAY — the four handheld rows (first in the accordion, `handheld.ts:637-649`)

| Row label (verbatim) | Help text (verbatim) | Control | Options / range | State from | Sends |
|---|---|---|---|---|---|
| `BRIGHTNESS` | — | slider `#hhBright` | 1-255. Initial 120 until refreshed. The supervisor clamps to 1-255, so it is never black | `GET /handheld/device` → `state.brightness` | `POST /handheld/device` `{"set":"brightness","value":N}` |
| `SPECTRUM RATE` | `Real frames from the radio each second — every waterfall row is real` | seg | `5` `10` `20`, default 20 | `vibesdr_handheld_v1.specRate` | `host.setSpectrumRate(fps, interpolate)` (`main.ts:3141-3148`): sets `hhSpecFps`, `wfScroll='sharp'` (or `'default'` when interpolating), boost off, `applyWaterfallRates()`. The rate then reaches the engine as WS `{"type":"fftRate","value":<fps>}` (`spectrum.ts:2423`; shim `local_sdr_shim.cpp:21308`, per client, capped by the owner's max). `wf20=interp` (URL `?wf20=interp`, or `vibesdr_handheld_v1.wf20`) turns 20 into 10 real frames plus repeated rows |
| `DRAWING RATE` | `How often the screen is redrawn` | seg | `30 fps` `60 fps`, default 30 | `vibesdr_handheld_v1.uiFps` | client only: `hhUiCap` caps the render loop |
| `SCREEN OFF AFTER` | `Dims at half the time; audio keeps playing; any button wakes it` | seg | `NEVER`(0) `1 m` `2 m` `5 m` `10 m`, default NEVER | `vibesdr_handheld_v1.screenOffMin` | See "SCREEN OFF AFTER timer" below |

**SCREEN OFF AFTER timer** (`handheld.ts:351-373`). The page runs it:
- **Dims** at half the time: `GET /handheld/device` (saves the current brightness), then `POST {"set":"brightness","value":min(saved,20)}`.
- **At the full time:** `POST {"action":"screen-off"}` and drawing stops (`host.setScreenOff(true)`).
- **Any input wakes it:** keydown, pointerdown or wheel, plus a 4 Hz gamepad poll (any button down, or a stick past 0.5).
  Waking sends `POST {"action":"screen-on"}` and restores the brightness.
- **The service's own `screen` event** (a short press of power) also stops or starts drawing.

### 4c.2 R36S HARDWARE (`handheld.ts:659-751`)

| Row label (verbatim) | Help text (verbatim) | Control | Options / behaviour | State from | Sends |
|---|---|---|---|---|---|
| `VOLUME` | — | slider `#hhVolume` | 0-100. Initial 60 until refreshed | `GET /handheld/device` `state.volume`; live from event `volume` | `POST /handheld/device` `{"set":"volume","value":N}` (the ALSA master; the supervisor clamps to 0-100). This is NOT the AUDIO sheet's volume (that is the engine's speaker gain `{"type":"speaker","level"}`). Two volumes in series: flag for native |
| `BATTERY` | — | text `#hhBattVal` | `83 %` / `83 % · charging`, + ` · about 4 h 12 min left` (from `/handheld/perf`). `the hardware service is not running` when `state.ok === false`; else `—` | `/handheld/device` `state.battery{pct,charging}`; `/handheld/perf` `battery.minutesLeft` | — |
| `CLOCK` | `UTC` | text `#hhClockNow` | `YYYY-MM-DD HH:MM:SS.s`, refreshed every 100 ms while open | browser `Date` (the device clock) | — |
| `NUDGE` | — | 6 buttons | `−1 m` `−1 s` `−0.1 s` `+0.1 s` `+1 s` `+1 m` | — | `POST /handheld/clock` `{"deltaMs":±60000\|±1000\|±100,"source":"nudge <label>"}` |
| `SET UTC` | — | text `#hhClockSet` (placeholder `YYYY-MM-DD HH:MM:SS`) + `SET` | regex `^(\d{4})-(\d\d)-(\d\d)[ T](\d\d):(\d\d)(?::(\d\d(?:\.\d+)?))?$`; bad input toasts `Not a time` / `Type it as YYYY-MM-DD HH:MM:SS, in UTC` | — | `POST /handheld/clock` `{"utcMs":<ms>,"source":"by hand"}` |
| (no label) | — | `#hhClkStation` button, hidden unless a sync is under 90 s old | `SET CLOCK FROM <station>` | `noteTimeSync(t)` from the decoder socket (`main.ts:13198`): `{station, utcMs, rxDelayMs, hostMs}` | `POST /handheld/clock` `{"deltaMs": utcMs+rxDelayMs−hostMs, "source":"<station>"}` |
| (no label) | — | `#hhClkFt8` button, hidden unless 3 or more FT8 decodes in 2 min with \|dt\| < 5 | `ALIGN TO FT8 (DT +0.31 s, 12)`: median DT | `noteSpot(s)` (mode `FT8`, `s.dt`) | `POST /handheld/clock` `{"deltaMs": round(−medianDt×1000), "source":"FT8"}` |
| (note) | `"Set clock from" appears after a time station (MSF, DCF77, WWV, WWVB, JJY) has given two consecutive parity-checked minutes that agree; "Align to FT8" after three or more FT8 decodes.` | label | | | |
| `LOCATION` | `Where this receiver is — for distances, ranges and the map` | text `#hhLocNow` + `SET FROM MAP` | `not set` / the locator / `lat, lon` | `GET /handheld/location` → `{locator,lat,lon,set}` | `SET FROM MAP` opens `handheldMap.ts` `openMapPick`, centred on the current location or the whole world. The pick sends `POST /handheld/location` `{"lat":…,"lon":…}` |
| `SET LOCATION` | — | text `#hhLocIn` (placeholder `IO92nh  or  52.25, -0.89`) + `SET` + `CLEAR` | `lat, lon` regex `^(-?\d+(?:\.\d+)?)\s*[, ]\s*(-?\d+(?:\.\d+)?)$`, else treated as a locator | | SET: `POST /handheld/location` `{"lat":n,"lon":n}` or `{"locator":"<text>"}`. CLEAR: `{"clear":true}`. See "Location replies" below |
| `CARD` | — | text `#hhCardVal` | `12.3 GB free of 29.7 GB` / `no card` / `—` | `GET /handheld/card` → `{present,freeBytes,totalBytes}` | — |
| `FILES` | — | 3 buttons | `IMPORT`, `EXPORT BOOKMARKS`, `EXPORT DIAGNOSTICS` (shows `WORKING…` while running) | | See "FILES buttons" below |
| `CONTROLLER` | `Shows each button and stick as the receiver sees it` | button `TEST BUTTONS` | opens `handheldPadTest.ts` | | — (client) |
| `TOUR` | `The controls, one at a time` | button `#hhTourBtn` `VIEW TUTORIAL` | closes the menu, `startTutorial()` (`tutorial.ts`) | | — |
| `PERFORMANCE` | `An overlay at the top of the screen` | toggle `#hhPerfBtn` `ON`/`OFF` | default OFF | `vibesdr_handheld_v1.perf` | client: polls `GET /handheld/perf` every 1 s and paints `#hhPerf` 2×/s (§6) |
| (figure) | — | `#hhPerfFig` | the same four lines as the overlay, while the accordion is open | `/handheld/perf` | — |
| `POWER` | — | `POWER OFF`, `RESTART` | two-press arm: the first press shows `PRESS AGAIN` for 3 s | | `pushMemory(true)` then `POST /handheld/device` `{"action":"poweroff"}` / `{"action":"reboot"}` (the supervisor also saves every radio's memory) |

**Location replies.** The supervisor's errors are toasted:
- `a locator is 4 or 6 characters, e.g. IO92 or IO92nh`
- `latitude -90 to 90, longitude -180 to 180`. 0,0 is refused.

On success the toast is `Location set` / `Location cleared`. The page re-reads `/location` at 1.3 s and 3 s.

**FILES buttons.**
- **IMPORT:** `GET /handheld/import` → `{files:[{name,text}|{name,tooBig:true}]}`. Each file is parsed in the page
  (`parseBookmarksAny`) and pushed to the RECEIVER's list via `POST /bookmarks/import`: a JSON array of
  `{name,frequency,mode,bandwidth_low,bandwidth_high}`, batched under 55 KB (`main.ts:3149-3171`). Then
  `POST /handheld/import/result` `{"name","ok","imported","message"}`, which moves the file to `import/done/` and writes a
  `.result.txt`. Toast `Imported N bookmarks` / `K of M files — results in import/done/`, or `Nothing to import` /
  `Put bookmark backups in the card's import folder.` Also runs once at start, silently.
- **EXPORT BOOKMARKS:** `POST /handheld/export?kind=bookmarks`, body = `exportBookmarks()`. ⚠️ That is the browser-local
  "YOUR BOOKMARKS" list only, not the receiver's list (`search.ts:285`). Toast `Bookmarks exported` + `export/<name>`.
- **EXPORT DIAGNOSTICS:** `POST /handheld/diagnostics` (empty body) → `{ok,name}` in `Diagnostic logs/`.

**Open behaviour.** `refreshDeviceRows` (`handheld.ts:754-777`) runs every time an accordion opens. It fetches
`/handheld/device`, `/handheld/card` and `/handheld/perf`, and starts the 100 ms clock tick, which stops when R36S HARDWARE
is hidden.

## 5. Bookmarks sheet and Recordings sheet

### 5.1 Bookmarks (`#bookmarksPanel`, index.html:5822-5877; `main.ts:11455-11560`, `renderBookmarks` 11672+)

| Control | Label (verbatim) | Behaviour / message |
|---|---|---|
| header | `★ BOOKMARKS` + `CLOSE` | |
| `#bmName` | placeholder `Bookmark name` | text |
| `#bmAdd` | `+ ADD HERE` (title `Save in this browser only`) | Local list (`src/services/userBookmarks.ts` `saveUserBookmarks`) with `{name (or "<MHz> MHz <MODE>"), frequency, mode, bandwidth_low, bandwidth_high}`. On the handheld this is Cog's localStorage, and it is per DEVICE |
| `#bmAddServer` | `+ ADD TO SERVER` (title `Save on the receiver, shared with everyone`) | `POST /bookmarks?frequency=<Hz>&name=<enc>&mode=<m>&bandwidth_low=&bandwidth_high=` (`search.ts:155-178`). Hidden only when the server has an admin password and you are not unlocked (`main.ts:1179-1183`). On the handheld the receiver's list is the OWNER's, one file, whichever radio (`vibeserver/main.cpp:4624`) |
| `#bmExport` | `EXPORT` | Browser download (`a.download`). ❌ Cog has no downloads: dead on the handheld. Use R36S HARDWARE → FILES → EXPORT BOOKMARKS |
| `#bmImport` / `#bmImportServer` | `IMPORT HERE` / `IMPORT TO SERVER` | `<input type=file>`. ❌ There is no file picker on the handheld; use FILES → IMPORT (card `import/`) |
| `#bmMsg` | status line | e.g. `Saved "<name>" on the receiver`, `Could not save on the receiver (is the PIN right?)` |
| list `#bmList` | Three sections (`renderBookmarks`): **YOUR BOOKMARKS** (`Saved in this browser`), **SAVED ON THE RECEIVER** (`Saved by the owner, shared with everyone`), **HEARD BY THIS RECEIVER** (`Found automatically over RDS or on a DAB multiplex — expires if it stops being heard`) | Row tap = `tuneTo` (§2.5). `EDIT` opens the `#bmEdit` form. `✕` removes: local, or `DELETE /bookmarks?frequency=<Hz>[&sid=]` |
| filter bar | from the DAB box's bookmark key, `bmFilter='dab'` | DAB rows only, with a link back to all |

**Edit form `#bmEdit`.**
- Title `EDIT BOOKMARK`.
- Fields: `Name` (max 60), `Frequency` `MHz`, `Mode` (select), checkbox `Use the mode's default passband`,
  `Passband (Hz)` low/high (placeholders `low, e.g. -5000`, `high, e.g. 5000`).
- `USE WHAT I'M LISTENING TO` (title `Frequency, mode and passband from the dial; the name stays`), `SAVE`, `CANCEL`.
- Server rows: `POST /bookmarks?was=…` (admin-gated). Rules in `src/utils/bookmarkEdit.ts`.

The server list comes from `GET /bookmarks` (`search.ts:96-114`).

### 5.2 Recordings (`#recordingsPanel`, index.html:5814-5820; handheld branch `main.ts:17314` → `renderCardRecordings` 3192-3228)

- Header `RECORDINGS` + `CLOSE`. Opened from the AUDIO sheet's RECORDINGS key (§3).
- List = `GET /handheld/recordings` → `{recordings:[{name,bytes,mtime}]}`. Shown newest first, as the server returns them.
- Empty: `No recordings on the card yet — press ● REC.`
- **Row:** the name without `.wav` · size; `✕` (press twice within 3 s: `DELETE?`, then
  `DELETE /handheld/recordings/<name>`); an `<audio controls preload=none src=/handheld/recordings/<name>>`.
  The server answers Range requests.
- ❌ **Playback is browser audio** (an `<audio>` element), so it does NOT come out of the engine's speaker path. Native
  needs its own player, or a new engine "play file" endpoint.
- **Saving:** stopping REC POSTs the WAV to `POST /handheld/recordings?hz=<Hz>&mode=<MODE or "DAB <block>">&start=<unix s>`
  (`handheld.ts:412-419`). Toast `Saved to the card` / `Recordings/<name>`, or `Recording NOT saved` + the error.
  - Supervisor errors: `no card to save to` (503), `a recording must be a WAV of up to 2 GB` (413), `the card is full` (507),
    `the recording did not arrive whole` (500).

## 6. Handheld-only screens and overlays (handheld.ts)

| Element | When | Content (verbatim) | Driven by |
|---|---|---|---|
| `#hhConnect` (full screen) | session `state` `none`, or the `radio` event `none` | `VIBESDR` / `Please connect an SDR` / `RTL-SDR · Airspy HF+ · Airspy R2 / Mini` `SDRplay RSP · HackRF · USRP` `into the USB-C port`; status line `Starting <name>…`; `Location: <text>`; button `SET LOCATION FROM MAP` | `/handheld/session`, events |
| `#hhVol` | event `volume` | `VOL 60` or `MUTED` + a bar; 1.5 s | event `{volume, muted}` |
| `#hhBatt` (red banner) | event `battery_low` | `Battery 10 % — plug in the charger. VibeSDR switches off cleanly at 3 %.`; cleared by a `battery` event with charging or pct > 10 | events |
| `#hhToast` | various | `Headphones` / `Speaker` (event `output`); `Switching off` + `Settings saved.` (event `shutdown`); `Radio settings reset` + `Start + Select at power-on put this radio back to its defaults.` (session `resetSeq` > 0, shown once per seq); plus every toast in §4c/§5; 3.5 s | |
| `#hhPerf` | PERFORMANCE ON | `CPU 42.0 %  51.0 °C` (boxed) / `DSP 31.0 %  RAM 512/1024 MB` / `UI 30 fps  Spectrum 20.0 fps` / `Battery 83 % ~4h12` or `… charging` | `/handheld/perf` + `host.uiFps()` + `host.specFps()` |

**Radio events.**
- `state:'running'` with a different serial, or while the connect screen is up: the page RELOADS. One clean start per
  radio; memory and caps are reloaded.
- `state:'none'`: memory is pushed, then the connect screen is shown.

## 7. `/handheld/*` endpoint reference (vibeserver/handheld.cpp:782-1100; supervisor routes 1137-1159)

| Method + path | Body / query | Reply |
|---|---|---|
| `GET /handheld/session` | — | `{"handheld":true,"state":"none"\|"starting"\|"running","radio":{serial,driver,name}\|null,"memory":{…}\|null,"resetSeq":N,"resetSerial":"…","eventSeq":N,"hwd":bool,"card":bool}` |
| `POST /handheld/memory` | `{"serial":"…","prefs":{…radio part…}}` | `{"ok":true}`; 409 `that radio is not plugged in` |
| `GET /handheld/events?since=N` | long-poll ≤ 25 s | `{"seq":N,"events":[…]}`. Events: `{"event":"volume","volume","muted"}`, `output`, `battery{pct,charging}`, `battery_low{pct}`, `shutdown{reason,inSec}`, `screen{screen}`, `radio{state,radio}`, `reset{serial,seq}`, `hwd{up}`. A `seq` lower than `since` means the engine restarted |
| `GET /handheld/device` | — | `{"state":{ok,brightness,maxBrightness,volume,muted,output,battery{pct,charging,status},screen}}` |
| `POST /handheld/device` | `{"set":"brightness"\|"volume","value":N}` or `{"action":"poweroff"\|"reboot"\|"screen-off"\|"screen-on"}` | the hwd reply `{"ok":true}` / `{"ok":false,"error"}`; 400 `not a request the hardware service takes` |
| `POST /handheld/clock` | `{"utcMs":N}` or `{"deltaMs":N}`, + `source` | `{"ok":true,"utcMs":N}`. Refused unless 2024-01-01 ≤ t ≤ 2100: `that time is not plausible` |
| `GET /handheld/perf` | — | `{cpu,tempC,engineCpu,cores,ramUsedMb,ramTotalMb,battery{pct,charging,minutesLeft}}` |
| `GET /handheld/card` | — | `{present,freeBytes,totalBytes}` |
| `GET /handheld/location` | — | `{locator,lat,lon,set}` (lat and lon are strings) |
| `POST /handheld/location` | `{"locator"}` / `{"lat","lon"}` / `{"clear":true}` | the same as GET, or 400 `{error}` |
| `GET /handheld/recordings` | — | `{recordings:[{name,bytes,mtime}]}` |
| `POST /handheld/recordings?hz=&mode=&start=` | a WAV body | `{"ok":true,"name"}` |
| `GET` / `DELETE /handheld/recordings/<name>` | Range supported | the WAV / `{"ok":true}` |
| `GET /handheld/import` | — | `{files:[{name,text}\|{name,tooBig:true}]}` (≤ 20 files, ≤ 1 MB each) |
| `POST /handheld/import/result` | `{name,ok,imported,message}` | `{"ok":true,"movedTo":"done/<name>"}` |
| `POST /handheld/export?kind=bookmarks\|dxlog[&ext=json\|csv\|adi\|txt]` | the file body | `{"ok":true,"name":"export/<UTC> - bookmarks.json"}` |
| `POST /handheld/diagnostics` | — | `{"ok":true,"name":…}` in `Diagnostic logs/` |

**Not `/handheld/*` but used by the handheld page on the radio child:**
- `GET /vibeserver.json`: caps such as `dab`, `modes`, `adsb`/`adsbWhy`, `ism`/`ismBands`/`ismWhy`, `ndb`, `ais`, decoder caps.
- `GET`/`POST`/`DELETE /bookmarks`, `POST /bookmarks/import`, `GET /location`.

## 8. Per-radio memory (what the native UI must reproduce)

- **Session:** `/handheld/session.memory` is the radio's saved prefs blob, or null.
- **Boot:** the page overlays the radio part onto `vibesdr_web_prefs_v1` and keeps the DEVICE keys (`GLOBAL_KEY`,
  `handheld.ts:113`). If that changed anything it reloads once (`sessionStorage vsHhMemReload` guards against a loop).
- **While running:** every 2 s, and on `pagehide`, before power off or reboot, and on unplug, it POSTs
  `/handheld/memory {"serial","prefs":<the non-GLOBAL keys, sorted>}`. It only posts when the body changed.
- **The engine:** writes the memory to `VIBESERVER_DATA_DIR/handheld/radio-<serial>.json` (every ~30 s, on unplug, on
  power-off).
- **Device-global keys** (from GLOBAL_KEY): `palette`, `ctl*`, `freqUnit`, `vfoColor`, `volume`, `specRatio`, `specShow`,
  `wfScroll`/`wfSpeed`/`wfBoost`/`wfData`/`wfDataRate`, `lastHost`, `tapSnap`, `ptrFreq`, `font*`, `look*`, `theme*`,
  `disp*`, `padTuck`, `wheelTunes`.
  - The 4b report: several "look" keys (waterfall contrast, brightness, range etc.) do NOT match the regex, so they follow
    the radio.
  - `wheelTunes` is listed, but the real key is `wheelAction`.
- **Everything else is the radio's:** step, mode and dial per server, bwSync, squelch, gain/front-end prefs, recbuf, DAB,
  decoders' choices, the view.
