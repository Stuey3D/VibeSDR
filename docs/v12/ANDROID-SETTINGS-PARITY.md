# Android server screen ↔ Setup page — settings parity (V12, 2026-11-03)

Stuart: *"does the android apps have the background decoder settings ported over? If not please port over all the new
settings including the background decoding settings that we've added since v12 has been testing"*.

**Answer on 2026-11-03: no.** Background Decoding and the gallery reached the Linux/Mac Setup page in beta3 (2026-10-22)
and the phone's server screen never heard of them. The engine on the phone (the same `local_sdr_shim.cpp`) could run them;
nothing told it to. This branch ports them, end to end, plus the other V12 settings that were missing.

- **Reference:** `android/app/src/main/cpp/vibe_setup_page.h` (the Setup page), `vibeserver/vibeserver_config.*` (the keys),
  `vibeserver/main.cpp` (how they are applied).
- **Android:** `src/screens/ServerModeScreen.tsx` (rendered by the main app's Server mode **and** VibeServer Lite),
  `src/services/vibeServer.ts` (the config sent), `VibeServerBoot.kt` (applies it on every start path: Start, crash
  restore, Lite, the private DAB scan engine), JNI in `vibe_localsdr_jni.cpp`.
- **The test:** `scripts/check-android-settings-parity.mjs` (in `run-tests.sh`) classifies **every** Setup-page control id
  (150). A new Setup control fails it until it is either ported (on the screen, sent, applied at boot) or listed as not
  ported with the reason.

"On Android?" means the setting is on the screen **and** the native side applies it — not merely drawn.

## ★★★ Background decoding and the gallery (priority 1) — PORTED

| Setting (Setup page words) | Config key | On Android? | Notes |
|---|---|---|---|
| Background Decoding (switch) | `RadioConfig::bgDecode` → `on` | **ports** (this branch) | RADIO SETTINGS → after WHEN NOBODY IS LISTENING, beside the power save (Setup: Radio tab, beside power save / hand-off). `components/BackgroundDecodingSetup.tsx`. |
| "When the last user leaves the server after [N]" | `bgDecode.waitMin` | **ports** | Same seven choices (1 min … 1 hour); a stored odd value is kept and shown. |
| Keep decoding stations still in range while listeners are on | `bgDecode.keepInRange` | **ports** | Absent = on, as the engine reads it. |
| Profile: Decodes — Stations on VFOs / ISM sensors on a whole band / AIS ships | `profiles[].ism`, `profiles[].ais` | **ports** | ISM/AIS offered only where the radio reaches them (by driver, `vibeserver/main.cpp`'s save rules), never on a locked radio, never when the owner switched ISM/AIS off. A saved profile the radio can no longer run stays visible with the reason. |
| ISM band | `profiles[].ism` | **ports** | Band **ids** from the engine (`vibeism::bands()`); the descriptive words are the Setup page's. |
| RF Centre | `profiles[].centre` | **ports** | Not drawn on a locked radio (its window is the lock). |
| VFO row: frequency → demodulator → decoder | `profiles[].vfos[] {hz, mode, dec}` | **ports** | Decoders come from the **engine's table** (`vibe::bg::decoders()` via `LocalSdrShim::backgroundCapsJson`), filtered by demodulator — no copy in the app. Band VFO picks a band from the engine's Band list; NDB skimmer greys frequency + demodulator. |
| Priority order (tier list) | order of `vfos[]` | **ports** | ▲ / ▼ keys (D-pad friendly — Lite runs on TV boxes); drawn where the Setup page draws it on a one-radio server (keep-in-range on, or locked). The phone serves one radio, so the multi-radio case never arises. |
| Cap / slot lines | (decoder slots) | **ports** | From "Decoders at once" or the engine's `decoderDefaultMax()`. |
| + Add another VFO | — | **ports** | Up to the engine's `kMaxVfos`, or the slots on a single radio with no contention. |
| + DSC watch | — | **ports** | Same arithmetic as the Setup page (`bgDscWatch`), hidden when DSC is switched off. |
| Background profile active between [00:00] - [00:00] (UTC) + server time | `profiles[].from/to` | **ports** | The phone's own local time is the server's. |
| Want to add another background decoding profile? | — | **ports** | Only under a profile that is not all day; up to `kMaxProfiles` (4). |
| ADS-B kept running note | (`adsbKeepRunning`) | **ports** | Same sentence, pointing at "Where listeners start". |
| Release-when-idle share-by-time note | `releaseWhenIdle` | **not ported** | "Let another program use this radio" does not exist on Android (no other app can take a USB radio the app holds), so the note has nothing to explain. |
| Save refused in the engine's words | (`vibe::bg::validate`) | **ports** | `LocalSdrShim::validateBackgroundDecoding` (new JNI): the sentence shows under the profiles as they are edited, and **Start refuses** those profiles with it — the Setup page refuses the save. |
| Gallery size [N] MB + free space | `ServerConfig::galleryMb` | **ports** | SERVER SETTINGS → GALLERY (Setup: Server tab after "Decoders at once"). Free space and what the gallery holds from `galleryDisk` (Kotlin). Default 100. |
| Everyone may see the gallery | `galleryPublic` | **ports** | Default on. |
| Keep listeners' decodes in the gallery too | `gallerySaveLive` | **ports** | Default on. |
| (gallery directory, radio label, serial redaction) | — | **ports** | `VibeServerBoot` → `setGallery(files/gallery, …)`, label = SDR display name or the USB model, the USB serial kept out of every item (as `main.cpp`'s `gallerySerials`). |
| (private DAB scan engine) | `bgDecode` | — | `privateScanConfig` drops `bgDecode`: a scan never runs the owner's profiles. |
| **DX Scanner** (switch) — V12 2026-11-04 | `RadioConfig::dxScan` → `on` | **ports** | RADIO SETTINGS → WHEN NOBODY IS LISTENING, under Background Decoding (Setup: Radio tab, under Background Decoding). `components/DxScanSetup.tsx`; one JSON blob as the Setup page saves it. **Lite too** — the brief allowed Lite to ship without it, but the engine is the same shim and the benchmark's `dec_dxscan` row already makes a weak box sweep in smaller steps, so leaving it out would only take the feature away. Not drawn on an Icom. |
| DX Scanner: bands (FM and DAB / FM only / DAB only) | `dxScan.bands` | **ports** | Not offered on an Airspy HF+ (FM only — it cannot hold a DAB multiplex), as on the Setup page. |
| DX Scanner: start after nobody has listened for [N] | `dxScan.waitMin` | **ports** | The same seven choices; a stored odd value is kept and shown. Default 5 min. |
| DX Scanner: FM channel raster 100 / 50 kHz | `dxScan.raster` | **ports** | Only while FM is swept; with the hop count at the radio's rate. |
| DX Scanner: only the owner may view the DX Log | `dxScan.viewAdmin` | **ports** | Default off (open). |
| (DX scanner's log file) | — | **ports** | `VibeServerBoot` → `setDxLogFile(files/dxlog.json)` (main.cpp: `data/dxlog[-serial].json`). |
| (private DAB scan engine) | `dxScan` | — | `privateScanConfig` drops `dxScan` too. |

Applied: on every start (`VibeServerBoot.applyAndStart` — Start, crash restore, Lite). The settings screen is only shown
while the server is stopped, so "live" here is the next Start; the engine's tick picks a new profile up within two seconds.

## Everything else since V12 testing began

| Setting | Config key | On Android? | Notes |
|---|---|---|---|
| Modes and decoders tree (every decoder a switch, options under it) | `blockedModes` | ports (verified) | `BLOCKABLE` in step with the Setup page. |
| **Band, mode by mode (Auto / Always / Off)** | `blockedModes` `spotsband±<mode>` | **ports** (this branch) | Was "the setup page's alone". Now the `bandModes` node under Band; modes from the build's own registry (`DIGI_MODE_STYLE` — the server on this phone is this build). |
| LoRa: listen on every channel | `blockedModes` `lora+all` | ports (verified) | |
| DAB station-label scan | `dabScanLabels` | ports (verified) | Lite only, as decided (main app: always on). |
| AIS ship store | `blockedModes` `aisstore` | ports (verified) | |
| Region defaults of the owner's switches | (`regionJson`) | ports (verified) | `regionDefaults`, the owner's `+id` word kept. |
| Benchmark-tailored defaults (`defaultOff`) | (benchmark) | ports (verified) | `applyBench`; every switch it names is on the screen (never hidden). |
| Broadcast FM defaults (+ remember / return) | `fm*` | ports (verified) | `setVibeServerFmDefaults(cfg)` reads the flat keys. |
| Antenna port names | `antennaNames` | ports — **fixed** (this branch) | The screen called `NativeModules.VibeLocalSdrModule`, which does not exist (the module is `VibeLocalSDR`), so the radio's ports were never read and the names card never drew. Also the antenna switch's live apply and search. |
| ADS-B as the landing / keep it running | `landingAdsb`, `adsbKeepRunning` | ports (verified) | |
| ADS-B at 2.56 MS/s (RTL) | `adsb256` | ports (verified) | |
| Change the sample rate when a mode needs it + the rate guard | `dabRateBoost` | ports (verified) | |
| VFO A/B | `vfoB` | ports (verified) | |
| Keep the spectrum sharp when zoomed in (zoom detail) | `zoomSpectrum` | ports (verified) | Default on; benchmark decides. |
| Icom IC-7300 (baud, CI-V, scope speed, tune lock, power on over CI-V) | `icom*` | ports (verified) | `check-icom-servermode.mjs`. `icomTuneLock` replaced `icomOwnerLock` (2026-11-04, default now OFF — the old key is not read); `icomPowerOn` reaches the phone's server through `setIcomNext`. RADIO ON / RADIO OFF are the app's admin controls (`icom_power`), not settings. `icomRfOpen` is gone from the phone (2026-11-04): an Icom's ATT/preamp/AGC/RF gain follow the SDRs' gain rules on the server. `icomPort` / `icomTtyOk` / `icomAudio` are Linux's (not ported — Android finds the radio and its audio itself). |
| **VibeAGC (SDRplay)** | `rspVibeAgc` | **ports** (this branch) | Drawn only for an RSP; new JNI `setRspVibeAgc`; Android's RSP driver is OpenRSP, so Automatic = on. |
| USRP options and FPGA image | `usrpClock/Time/DcOffset/IqBalance/RxBw` | **not ported** | No USRP on Android (UHD not built; `usrp_source.cpp` not in the app's CMakeLists). |
| Locked range (centre, rate, users) | `lockedCentre`, `lockedRate`, `maxUsers` | ports (verified) | Background decoding follows it (never moves a locked radio). |
| Chat / canned-chat translation | — | n/a | Not an owner setting on the Setup page. |
| Landing message / link | `landingMessage…` | ports (verified) | |
| **DAB band scan for the owner ("Scan all DAB blocks")** — on Linux it moved from the Setup page to SERVER ADMIN → DAB SCAN (2026-11-04) | — (bookmarks) | **ports** (this branch) | The engine's GET /vibeserver/dab-bandscan, on the running server or on the owner's radio in a private loopback engine (privateScanConfig) while the screen is up; progress, Cancel, each multiplex with its stations, "Use <block> as the landing block"; stations go to the bookmarks. Start waits for it. |
| Pause / remove a radio | (multi-radio) | not ported | A phone serves one radio; stopping the server is the pause. |

## Deliberately not ported (no dead controls — AGENTS.md)

| Setup page | Why not on Android |
|---|---|
| Sign-in, first-run claim | the screen is on the device itself |
| Pocket Wi-Fi, hotspot, SSH, Wi-Fi country | Android owns its network |
| CPU governor | an app cannot set one |
| Server port | the phone takes the first free port of 48000–48049 |
| Radio serials, per-radio PIN | one radio per phone; the server PIN covers it |
| Let another program use this radio | no other program can take a USB radio the app holds |
| USRP | not built for Android |
| External antenna switch setup | **ported, behind the same gate as the Setup page** (`ANT_SWITCH_SETUP` here, `SW_SETUP` there — both false since V11, Stuart 2026-10-10); flipping both to true is Stuart's call |
| Icom port / tty / audio device | Linux's; Android finds them itself |

## Ported in the second pass (Stuart, 2026-11-04: "all the ones currently in Linux go to android")

| Setting (Setup page words) | Config key | On Android? | Notes |
|---|---|---|---|
| Broadcast notch | `rfNotch` | **ports** | RADIO SETTINGS → SDRPLAY (`components/RspSetupCard`), only on models that have it; greyed under automatic notching. |
| DAB notch | `dabNotch` | **ports** | Not on the RSP1 or RSP2. |
| AM broadcast notch (RSPduo) | `rspAmNotch` | **ports** | RSPduo only. |
| HDR mode below 2 MHz (RSPdx family) | `rspHdr` | **ports** | RSPdx / dx-R2 only. |
| 24 MHz reference output (RSP2, RSPduo) | `rspExtRef` | **ports** | RSP2 / RSPduo only. |
| DAB through the API's decimation | `rspDabDecim` | **ports** | |
| Automatic RF gain | `rfAgc` | **ports** | |
| VibeAGC (SDRplay) | `rspVibeAgc` | **ports** | Now inside the card, in the Setup page's place. |
| Automatic notch filtering | `autoNotch` | **ports** | Only where the model has a notch. |
| Let listeners toggle the notches | `userNotch` | **ports** | Default on. |
| Use a separate IF AGC target for DAB + DAB AGC target | `dabAgcOverride`, `dabAgcTarget` | **ports** | − / + keys (D-pad), -60…-10, default on at -40. |
| IF AGC target + Lock the AGC target | `agcSet`, `agcSetLock` | **ports** | -72…-10, default -30 (SDRplay's own). |
| Aerial socket: Switch aerial by band + Only I may change the aerial | `antennaMap`, `antennaPortLocked` | **ports** | Only on a model with more than one socket; sent as antennaMap / antennaLocked (setRspAntennaMap / setAntennaLocked) when no external switch is set up. |
| Calibration (ppb) | `ppb` | **ports** | Airspy HF+ only; 0 = the radio's own flash calibration (never written as 0). |
| Impulse noise blanker | `nbWide` | **ports** | Off / Auto (below 30 MHz, default) / On; not on an Icom. |

Which rows a model has: the RSP's own hwinfo `radio` object, read while the server runs and remembered (`vs_rspcaps`),
else the model from its USB name, else everything — the Setup page's `rspCaps`, line for line (`services/rspCard.ts`).
All applied by `VibeServerBoot` before the radio opens, as `main.cpp` does, every value definite (new JNI
`setRspFrontEnd`, `setAhfPpb`).

★ Found on the way: `VibeServerBoot` read `rawIq`, `rawIqMax`, `rawIqLanMaxHz`, `nbWide` and `vfoB` through
`optString().toIntOrNull()`. The app's map arrives through `ReadableMap.toHashMap` (every number a Double) and
`optString(1.0)` is `"1.0"`, so on an ordinary Start those always fell back to their defaults — VFO B could not be switched
off and raw IQ never came on (a crash restore re-parses JSON where 1.0 is written "1", so only the ordinary path was
wrong). Now read as numbers; the parity check refuses the pattern.

## Left

- **No GAP rows.** Every Setup-page control is either on the phone (applied by the engine) or listed above as unable to
  work on Android, with why. `scripts/check-android-settings-parity.mjs` fails on any GAP row or any new unclassified
  control.
- Untested by hand on a device: compiled (Mac vibeserver; Android main app arm64 and Lite armeabi-v7a, Kotlin + native)
  and statically tested only.
