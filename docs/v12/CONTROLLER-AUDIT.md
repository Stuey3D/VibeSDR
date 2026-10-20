# CONTROLLER AUDIT — every screen, sheet, card and prompt (V12, 2026-10-20)

**Branch:** `v12-experimental` at `b9f643a0`. Read-only audit; nothing was built or run on a device.
**Why:** Stuart: *"there are screens these fail on such as selecting a vibeserver from the directory and it shows you
the radios — you cannot use the controls to select the radio as no selection box appears on screen, so we need a
complete audit of all screens in the app."* And, confirmed on device the same day: *"right now even with the on screen
keyboard there is no way of controlling the system keyboard with the controller."*

The companion design is [BRIEF-controller-keyboard.md](BRIEF-controller-keyboard.md). Its Phase 1 is this audit's
MUST list.

---

## 0. How a pad reaches anything (read this first — it decides most verdicts)

| Layer | Where | What it does |
|---|---|---|
| Native report | `ios/VibeSDR/VibeGamepad.swift`, `android/.../VibeGamepad.kt` | Whole pad, buttons by POSITION (`PAD_BIT`: D-pad, S/E/W/N, L1, R1, START, SELECT). **No L2/R2 triggers are reported.** |
| Mapping | `src/utils/padActions.ts` `padKey` / `PadMapper` | Stuart's two profiles → the keyboard's own key names. |
| Bus | `src/utils/hardwareKeys.ts` → `src/utils/keyActions.ts` `KeyBus` | Newest listener first; `true` consumes. Screens listen at `SCREEN_PRIORITY` (-1). |
| Panels | `src/components/PanelNav.tsx` | `usePanelNav`+`NavRow`+`useNavButton`/`useNavRange` (grid), `useListNav` (list), `useRepeatingKeys` (raw), owner stack, 10 s idle close, `keyboardMode` (pad counts). |
| Screen highlight | `src/components/ScreenNav.tsx` | `screenNavRef(id, press)` + `useScreenHighlight` — only the receiver and FM-DX screens. |

Facts that matter, all from the code (device behaviour unverified unless stated):

1. **A pad cannot type.** While a field has the cursor, only Enter and Back reach the app (`keyActions.ts:89`), and
   nothing anywhere sends pad input to the system keyboard. Stuart confirmed this on device. **Every text field is a
   dead end for a pad today**, whatever else works on its screen.
2. **iOS native alerts are a pad dead end AND a hazard.** `Alert.alert` / `Alert.prompt` present a UIAlertController in
   RN's own window (`node_modules/react-native/React/CoreModules/RCTAlertController.mm:20-50`). The pad cannot press
   its buttons, but GameController events still reach JS, so **the list or screen underneath keeps moving and Enter
   still presses things behind the alert**. 82 `Alert.alert`/`Alert.prompt` calls in 15 files. On Android an
   `AlertDialog` is not hooked (`VibeGamepad.kt` `hookDialogs` matches only `ReactModalHostView`), so the platform's
   own fallback probably drives it (unverified).
3. **Android consumes the pad's B and D-pad** in the activity and in hooked RN Modals (`VibeGamepad.kt:215-245`), so
   the system Back never fires from a pad. A screen or Modal with no bus listener is completely dead to a pad,
   Back included.
4. **Android: a hardware keyboard and a TV remote never reach the bus** (`VibeGamepad.kt:96-99` returns false for
   non-pad keys). They use Android's native focus, which draws no ring on our Touchables (no TV focus styling in
   `src/`). Only `TvTextInput` fields have a TV focus style.
5. **The receiver screen yields to "panels" by a hand-kept list.** `anyPanelOpen` (`SDRScreen.tsx:10321-10326`) hides
   the screen highlight (`:10352`) and stops the screen acting on arrows and Enter (`:10415`) — on the assumption that
   the open surface has PanelNav. Back calls `closeAll` (`:10369-10375`, `:10406`).
6. **A control that never registers is silently skipped.** `useNavButton` registers only under a `NavRow`
   (`PanelNav.tsx:588`); `PopupKey`, `TouchableOpacity`, `Pressable`, `Switch` and `Slider` never register on their own.
   Nothing warns. This is how most PARTIAL verdicts below happened.

---

## 1. Stuart's case: the radio chooser (multi-radio VibeServer)

**Surface:** "Choose a receiver", drawn over the receiver screen while `awaitingRadio` is true —
`src/screens/SDRScreen.tsx:953` (the condition) and `:10662-10920` (the render).

**Why no selection box appears and nothing can be chosen:**

1. **It is registered as a panel for SUPPRESSION but was never given panel NAVIGATION.** The 2026-08-16 fix (Enter in
   the admin box opened the frequency box behind it) added `(awaitingRadio && !!door)` to `anyPanelOpen`
   (`SDRScreen.tsx:10324`). From then on:
   - the screen highlight is switched off: `useScreenHighlight(kbInUse && !anyPanelOpen, …)` (`:10352`) — **no box**;
   - the screen's listener returns before arrows and Enter: `if (panelOpenRef.current) return;` (`:10415`), whose
     comment says "movement and Enter belong to it (PanelNav)" — **but the chooser has no PanelNav hook at all**.
2. **The rows are plain `Pressable`s** (`:10751-10752`) with no `screenNavRef`, no `useListNav`, no focus style. The
   way out, `‹ SERVERS` (`:10675`), is a plain `TouchableOpacity`.
3. **Back does nothing.** Back reaches `closeAll` (`:10406`), which clears thirteen panel flags (`:10369-10375`) but
   neither `awaitingRadio` nor `door`, and never calls `navigation.goBack()`. The user is stuck on the screen.
4. **Its two fields are unreachable and untypeable**: radio PIN (`:10850`, `number-pad`, secure) and admin password
   (`:10890`, secure). "Take over this receiver?" is a native alert (`:10766`) — dead end on iOS (fact 2).

**Root cause, in one line:** a surface that is *in the suppression list but has no key handling of its own* — the
screen stops listening and nobody else starts. Section 3 finds seven more surfaces with exactly this shape, and the
refusal card (`SDRScreen.tsx:12079-12135`) has the same `closeAll` hole as well.

**The fix (MUST-1):** a `useListNav` over `[‹ SERVERS, each choosable radio, PIN field, UNLOCK, admin field, UNLOCK]`
with a ring on each row, `startAt` = the first free radio, Enter = the row's own `onPress`, `onBack` = `goBack()`.
Blocked rows (unsupported, in use by another app, PIN-locked) are focusable but Enter on them moves the highlight to
the field that would open them — they are "listed and greyed, never hidden" (`:876`), and a member holding the right
PIN must be able to get from the row to the box. The take-over confirm and both fields follow MUST-3 and MUST-4.

---

## 2. Every surface

Legend — **Pad** = game controller through the bus. **Kbd** = hardware keyboard (iPhone/iPad/Mac; Android has no
keyboard path, fact 4). Reach = can focus reach every control; Ring = visible highlight; A/B = enter/back work.
**Text fields are FAIL for the pad in every row** (fact 1); the Text column says which, and whether a no-typing path
exists today. Verdicts are for the pad.

### 2.1 Front door and server list — `src/screens/InstancePickerScreen.tsx`

| Surface | Opens | Pad: reach / ring / A / B | Kbd | Text fields | Verdict | Fix |
|---|---|---|---|---|---|---|
| Server list (chooser view) | App start | Rows, headers, custom URL, discovered, DIRECTORIES, call sign, maps, Export logs reach (`useListNav` `:1996`, rings `:2096 :2113 :2151`). NOT: ⏺ RECORDINGS `:2503`, ⚙ reset `:2510`, banner ✕ `:2530`, URL-row ♡ ☆ CONNECT `:2576 :2618 :2643`, ⇅ sort `:2122`, per-row ✎ ☆ ♥ ≡ `:2209-2221`, drag reorder `:2155`, discovered ☆ `:2903`, TCP ✕ `:2859`. A: yes, **but SpyServer rows do nothing** (`:2011`). B: collapses a group — **off by the header count** (`:2048` walks `listData` from the global index). | Same | Custom URL `:2559` (URL) | PARTIAL | Row sub-buttons as a 2-D row (`rowOf`), header buttons into `hdrActions`; fix `:2048` to `navFocus - hdrActions.current.length`; SpyServer Enter = tap. |
| Directory view | Enter on a DIRECTORY | Rows reach. NOT: search `:2658`, sort NEAR/SNR/COUNTRY `:2667`, ALL `:2684`, Retry `:2702`, restricted SHOW/HIDE `:2736`, ☆ ♡ on rows `:2361 :2365`. **Enter ≠ tap**: omits `blocksApps` (restricted receiver not opened in compatibility mode) and ignores `isFull` (`:2013` vs `:2264-2266`). B leaves the directory (`:2044`). | Same | Search `:2658` (search) — alternative: scroll with the D-pad, once sort is reachable | PARTIAL | Search/sort/ALL/Retry into the header bank; Enter must call exactly what the tap calls. |
| Add custom server (`tcpModal`) | "+ Add custom server" | Nothing: no nav, list disabled under it (`:1965`). **Android pad: no way out** (B consumed, fact 3). | None | Name `:2437`, Host `:2440` (URL), Port `:2443` (number) — **no alternative** except the discovered list (LAN only) | FAIL | `useListNav` over chips, fields, buttons; Back = Cancel. |
| Edit favourite (`editFav`) | ✎ (itself unreachable) | Nothing | None | Name `:2468`, Address `:2471` (URL) | FAIL | As above. |
| VibeServer PIN | Connect to a PIN server | Android/Mac: `VibePinPrompt` — field auto-focused, Enter submits; Cancel / Connect / Save & Connect (`VibePinPrompt.tsx:133-141`) unreachable, no ring; Back leaves the field, box stays. **iOS: `Alert.prompt`** (`:962`) — native dead end. | Field only | PIN (number) — **no alternative** | FAIL | One `VibePinPrompt` on every platform when a pad is in use; nav its buttons; numeric ControllerKeyboard. |
| IdentModal (KiwiSDR call sign) | Call sign row / Kiwi connect | Nothing; not auto-focused; **keys leak to the list beneath** (`listNavActive` `:1965` ignores it) | Field only after a tap | Call sign `IdentModal.tsx:68` — no alternative | FAIL | Nav + gate `listNavActive`. |
| PasswordModal | Password-protected receiver | Nothing; `autoFocus={false}`; `onRequestClose` no-op (`PasswordModal.tsx:52`); **leaks to the list** | None | Password `:62` — no alternative | FAIL | Nav, Back = Cancel, gate the list. |
| RecordingsOverlay | ⏺ RECORDINGS (unreachable here) | Up/Down/Enter(play)/Back + ring (`RecordingsOverlay.tsx:203`). NOT: share `:250`, delete `:253`, seek `:260`. Left/Right leak to the list. | Same | — | PARTIAL | Row as 2-D (play / share / delete); seek = adjust mode; consume Left/Right; gate the list. |
| `pickerTour` coachmarks | First run | Nothing: no keys, Skip/Next `Coachmark.tsx:228/231` touch-only; **list underneath live** | None | — | FAIL | See group B: Enter = Next, Back = Skip. |
| First-launch splash CONTINUE | `App.tsx:1054` | Nothing | None | — | FAIL | Enter/Back = Continue. |
| "High Detail Maps?" first-launch alert | `App.tsx:319` (`cancelable:false`) | iOS dead end | UIKit | — | FAIL | In-app confirm (MUST-3). |
| Alerts: connect errors (19), compatibility Back/Continue `:943`, set/remove default `:1686 :1693 :2626`, factory reset `:1703 :1716`, TCP delete `:2846 :2860`, maps `:2977 :2980`, Export logs `ExportLogs.tsx:41-66` (+ Share sheet) | Various | iOS dead end, list live underneath | UIKit | — | FAIL | MUST-3. |
| TlsTrustPrompt — **incoming from main** (`f6035f0d`, not on V12 yet) | Any first connect to a self-signed https server; mounted once in `App.tsx` | Nothing: two `PopupKey`/`TouchableOpacity` buttons (CANCEL, TRUST), no nav; `onRequestClose` no-op; screen beneath not told | None | — | FAIL | `useListNav` [CANCEL, TRUST], start on CANCEL (the safe answer), Back = Cancel; register as an overlay. Its "Forget certificate" lives in the long-press alert and the ✎ sheet — both unreachable today. |

### 2.2 Receiver screen — `src/screens/SDRScreen.tsx`

| Surface | Opens | Pad: reach / ring / A / B | Kbd | Text fields | Verdict | Fix |
|---|---|---|---|---|---|---|
| Receiver screen | Connect | Ring over freqBox, modeBtn, stepBtn, audioBtn, menuBtn, chatBtn (`ControlsBar.tsx`), serversChip, decoderBox, restoreControls. Tune/zoom from anywhere. NOT: CenterVfoButton `:11865`, EdgeChips (health `:11661`, time `:11725`), owner notice dismiss `:11640`, AntennaBandNotice `:11040`, waterfall tap-to-tune. | Same | — | OK (gaps SHOULD) | `screenNavRef` on CenterVfo and the chips. |
| **Radio chooser** | Multi-radio door | See §1 | None | PIN `:10850`, admin pw `:10890` — **no alternative** | **FAIL** | MUST-1 |
| **Refusal card** (IN USE / refused) | `setRefusal` `:5846-5961`; RN Modal `:12079` | In `anyPanelOpen`, no nav; TAKE OVER `:12109`, TRY AGAIN `:12118`, BACK TO SERVERS `:12129` unreachable; **Back does nothing** (`closeAll` ignores `refusal`) | None | Admin pw `:12099` — no alternative | FAIL | Nav; Back = BACK TO SERVERS. |
| Connection cards: serverLost `:11196`, serverBusy `:11259`, kiwiRefused `:11283`, compatWarn `:11307`, specFailed `:11392`, connLost `:11413`, connTimedOut `:11437` | Connection events | Not in `anyPanelOpen`: **the ring walks the controls behind the card**; buttons unreachable; Back only clears the ring. compatWarn CONTINUE is greyed in keyboard mode on purpose (`:11320-11328`). | None | — | FAIL | One `ServerCard` nav (Enter = primary, Back = SERVER LIST); add to the overlay registry. |
| Tap banners: reconnect failed `:11186`, paused `:11463`, muted `:11473` | Events | Touch-only | None | — | FAIL | `screenNavRef` each. |
| "STILL LISTENING?" `:11076` | Idle | YES, I'M HERE `:11086` unreachable; a tune/zoom probably answers it via `markInteract` | None | — | PARTIAL | Any key answers it; register as overlay. |
| PasswordModal `:12746` | Receiver asks for a password (`:6804 :6817`, or the alert `:6821`) | Not in `anyPanelOpen` (keys leak); no nav; Back no-op | None | Password — no alternative | FAIL | As 2.1. |
| sdrTour coachmarks `:12155` | ~1.5 s after first run (`:10619`); REPLAY TUTORIAL | Not in `anyPanelOpen`: **Enter presses the control under the card**; Skip/Next `Coachmark.tsx:169/172` touch-only | None | — | FAIL | Group B. |
| FreqModal `:12632` | freqBox | Tune grid (`useListNav` `:473`, starts on the field), bookmarks pane (`:506`). NOT: SHARE `:890-918`, magic key `:850`, DAB rows `:972`, EiBi toggle `:1075`, sync `:1139`, delete ✕ `:1151`, import `:1164-1203` | Good: Enter in the field tunes (`:441`) | Frequency `:732` (decimal) — **alternative**: jog/keys, STEP, VTS ◄ ►, the receiver's station list; Search `:941` — alternative: saved list + receiver list; Bookmark name `:1086` — **none** (SAVE is disabled while blank, `:1106`); Magic key `:850` (password); Import `:1190` (paste — touch only by nature) | PARTIAL | Register the missing controls; default the bookmark name (RDS PS or the frequency) so saving needs no typing. |
| ProfilePicker (in FreqModal `:824`) | 'profile' item | `useListNav` `ProfilePicker.tsx:110`, ring, Back closes | Same | — | OK | — |
| MenuSheet `:11872` | menuBtn | `usePanelNav` `MenuSheet.tsx:1357`, rings, Back peels sub-pages. NOT: **SDR CONTROLS ›** `:1576` (a `Btn` outside any `BtnRow`/`NavRow`), **VFO lock** `:1651` (`VfoLockBtn` `:677` never registers), **VibeServer admin field + UNLOCK** `:2012-2025`, **About** footer `:2119`. Control Customisation rows that scroll sideways (`KeyScrollRow :841-856`) keep focus off-screen (`revealIn` is vertical only). Admin/setup pages `skipNav` on purpose. | Same | Admin pw `:2012` — no alternative | PARTIAL | Register the four; horizontal reveal. |
| Display Settings / Control Customisation sub-pages | In MenuSheet | Registered (sliders = adjust mode). RESET/THIS SERVER/GLOBAL raise alerts `:3570 :3578 :3588`. | Same | — | PARTIAL (alerts) | MUST-3. |
| SpecRatioOverlay `:11008` | Display Settings ratio | In `anyPanelOpen`, no keys: slider `SpecRatioOverlay.tsx:131` and ✓ `:117` unreachable; Back closes | Same | — | FAIL | `usePanelNav` + `useNavRange`. |
| AudioSheet `:12338` | audioBtn | `usePanelNav` `AudioSheet.tsx:704`. NOT: every `Toggle` (`:451` — MUTE `:767`, buffer `:803`, notch `:902`, binaural `:917`, NBX `:934`, VOICE/RAW `:944`, raw `:955`, IQ OUT `:979`, de-emph `:999`, stereo `:1010`, FM NR/IMS/CEQ/NB/A-BW `:1043`), IQ rate `:966/:971`, DSP TYPE row `:1109` (no `NavRow`) | Same | — | PARTIAL | Make `Toggle` call `useNavButton`; wrap DSP TYPE in `NavRow`. |
| StepPicker `:12159` | stepBtn | `useListNav`, ring, Enter picks, Back closes | Same | — | OK | — |
| ModeSelector (+ decoder picker) `:12170` | modeBtn | `usePanelNav` `ModeSelector.tsx:678`; everything registers; decoder list reachable | Same | — | OK | (Its OWRX MAP/FILES opens BrowserOverlay — see below.) |
| ServersChip `:11750` | serversChip | `useListNav`, ring, Back closes. "Default Set/Cleared" alerts `:8045/:8052` | Same | — | OK (alert) | MUST-3 |
| ChatDrawer `:12694` | chatBtn | In `anyPanelOpen`, **no PanelNav at all**: header (close, name, users, zoom sync, mute `:382-433`), JOIN `:458`, send `:747`, user rows, tune buttons, hide, **canned-phrase pad** `:633` all unreachable; Back closes | Fields only | Handle `:444`, manual freq `:665` (decimal), message `:732` — alternative for the message: canned phrases (shared-dial mode), unreachable today | FAIL | `usePanelNav` over header, canned pad, rows. |
| AboutOverlay `:12139` | MenuSheet footer (unreachable) | No keys; ← SDR, Export logs, links unreachable; can't scroll; Back closes | Same | — | FAIL | Up/Down scroll (as KeyboardShortcuts does), buttons registered. |
| KeyboardShortcuts `:12142` | ⌨ KEYBOARD | Up/Down scroll, Back closes | Same | — | OK | Update its GAME CONTROLLERS text when ControllerKeyboard lands (AGENTS.md rule). |
| PadGuide (`App.tsx:1036`) | SELECT / CONTROLLER GUIDE | Priority 100; Back closes, ←→ tabs, ↑↓ scroll | Same | — | OK | — |
| PadNotice | HOLD START | Toast | — | — | OK | — |
| FkaSplash `:12144` | FKA suspected | Enter/Back close; help button `:89` unreachable | Same | — | OK | (nice) |
| RecordingsOverlay `:12146` | Menu / AudioSheet | As 2.1; ring also shows for touch (`:195`) | Same | — | PARTIAL | As 2.1. |
| LocalHardwarePanel `:12413` | MenuSheet SDR CONTROLS (unreachable) | Own slots (`useRepeatingKeys :502`) cover only `Seg` rows and PPM ±. NOT: 18 `Switch`es, 11 sliders incl. GainSlider `:1294`, UNLOCK `:644`, RSP AGC restart `:804`, steps `:839/:845` | Same | Admin pw `:631`; LO MHz `:1487`, converter range `:1505/:1512` (decimal) — no alternative | FAIL | Rebuild on `usePanelNav` (it predates it); keep the per-driver branches. |
| MapOverlay `:12565`, local digi map `:12575` | ModeSelector map modes | Back only (`MapOverlay.tsx:1463`); **not in `anyPanelOpen` — arrows/Enter press the receiver controls behind the map**; disconnected-card buttons `:1724-1730` unreachable; WebView map is gesture-only | Same | — | PARTIAL | Register as overlay; nav the card; ↑↓←→ pan, ± zoom the map (our own page — Stuart: "our maps are super simple so they can be accessed"). |
| CityPickerModal `:12593` | Local map "pick city" | No keys; rows `:113`, CANCEL unreachable; **Back closes the MAP beneath and leaves the picker up** (MapOverlay's newer listener consumes it) | Field only | City search `:95` (search) — alternative: scroll the list, once rows register | FAIL | `useListNav` with higher priority than the map. |
| BrowserOverlay (compat mode `:11363`, admin pages `:12607`, OWRX MAP/FILES) | Several, incl. a reachable ModeSelector key | **Every key off, Back included** (`useSuppressShortcuts`, `BrowserOverlay.tsx:76`); back/Save/history unreachable; "Leave VibeSDR?" alert `:30` | Off | (page's own) | FAIL | Back (pad B / Escape) must still close the overlay; content stays out of scope. |
| DecoderPanel / DecoderShell `:11110` | decoderBox stop | Enter grabs, ↑↓ list, ←→ header keys, Back releases; DAB/ADS-B auto-grab; rings | Same | — | PARTIAL | Spot rows (`:159`) need Enter = tune. |
| AircraftPanel | In the decoder box | Read-only, scrolls | Same | — | OK | — |
| **DabPanel** `:11815` | **Opens by itself on entering DAB** | Not in `anyPanelOpen`, no screen stop, no keys: service rows `:359`, STATIONS/SIGNAL, BIG, ★, **EXIT DAB** `:889` unreachable; Back does not close it | None | — | FAIL | Make it a decoder-box-style region (it is the DAB box). |
| AdvRdsPanel `:11786` | ModeSelector | Same shape: BIG, ✕ unreachable, can't scroll, Back doesn't close | None | — | FAIL | As DabPanel. |
| PanelBoundary fallback | Any panel crash | Retry/Close `PanelBoundary.tsx:137/141` touch-only | None | — | FAIL (rare) | Enter = Retry, Back = Close. |
| CrashBoundary (`App.tsx:996`) | App crash | "Try again" `:70` touch-only; alert `:47` | None | — | FAIL (rare) | Enter = Try again. |
| LegacyPackageNotice (`App.tsx:1014`) | Old package id only | No keys, leaks beneath | None | — | FAIL | (nice — legacy build) |
| Native alerts raised here: background audio `:1487`, display reset/save `:3570-3588`, recording error `:3641`, OWRX pause `:3882`, language download `:4985`, raw IQ `:5554`, Update VibeSDR `:6783`, Receiver full `:6806`, Session ended `:6811`, Connection Error `:6819`, listen-only `:7186`, disconnected `:7209`, default set/cleared `:8045/:8052`, no bookmarks `:9549`, Ask to tune `:10193`, take over radio `:10766`, keep bookmarks `:11992` | — | iOS dead end, screen live underneath | UIKit | — | FAIL | MUST-3 |

### 2.3 FM-DX — `src/screens/TunerScreen.tsx`

| Surface | Opens | Pad | Kbd | Text fields | Verdict | Fix |
|---|---|---|---|---|---|---|
| Tuner screen | FM-DX server | `useScreenHighlight` `:932`; freqBox, mode, step, audio, ‹ Back (menuBtn `:1256`), chat; tune/zoom. NOT: ♥ `:1076`, KEYS/DRUMS `:1089`, **PAUSED — TAP TO RECONNECT** `:1105` (no key path back after a power-save pause), band chip `:1138`, AF chips `:1209`, dial station pins (`FmdxDial.tsx:107`) | Same | — | PARTIAL | `screenNavRef` on all five; AF chips are tune shortcuts worth having on a pad. |
| Shared-tuner notice `:1295` | Arrival | Enter/Back dismiss, ring | Same | — | OK | — |
| Demodulator sheet `:1319` | modeBtn | Own keys `:911`, rings | Same | — | OK | — |
| FreqModal / StepPicker / AudioSheet / Recordings | As receiver | As receiver | | As receiver | as 2.2 | |
| ChatDrawer | chatBtn | As 2.2 (`:956` yields to it) | | As 2.2 | FAIL | As 2.2. |

### 2.4 Serving from the phone — Android only

| Surface | Opens | Pad | Kbd / TV remote | Text fields | Verdict | Fix |
|---|---|---|---|---|---|---|
| ServerModeScreen (`ServerModeThemed`) + BandLimitEditor, AntennaBandsEditor, AntennaSwitchSetup, LiteUpdate | "Use as server" row | **No bus listener at all**: pad dead, B included (fact 3) | Native Android focus; fields are `TvTextInput` (focusable box + edit sheet on TV) | 21 fields (table in the brief §8): admin password `:2253` is **required to Start** with no alternative; PIN has Random/No PIN pills; listener count has ±; antenna has icon chips | FAIL (pad) / PARTIAL (TV) | SHOULD: a screen `usePanelNav`, or at minimum pad Back = goBack. A "generate password" key removes the one forced typing step. |
| RtlTcpServerScreen | ServerMode → RTL-TCP | No listener | Plain `TextInput` `:238` — **traps the TV D-pad** on the Android 6 box (TvTextInput's own story); Switch, rows, Stop unreachable | Name `:238` — has a default | FAIL | Swap to `TvTextInput`; pad Back. |
| System prompts: notifications `ServerModeScreen:1633`, `RtlTcpServerScreen:96` | Start | System dialog | System | — | out of scope | — |

### 2.5 Platform-only

| Surface | Finding |
|---|---|
| Mac menu bar | No `UIMenuBuilder`/`UIKeyCommand` anywhere; the Mac runs "Designed for iPad" with the system menu only. Nothing to audit; adding commands is NICE. |
| Android TV | No LEANBACK launcher; TV detected at runtime (`VibeLocalSdrModule.kt:41-49`). Remote = native focus, no ring on our Touchables (fact 4). Only ServerMode uses `TvTextInput`; the picker's URL and search fields are D-pad traps on the Android 6 box. |
| tvOS (`tvos/`) | Native focus engine only; no `VibeKeyDown`. Out of this audit. |
| Share sheets (19 calls), document picker (`SDRScreen:9659/9680`), expo-web-browser (SIGNAL ID WIKI), location / notification permission prompts | Native OS surfaces — cannot be driven by our bus. See §5. |
| Watch apps | Separate (digital crown); not in scope. |

---

## 3. Root causes — one fix per cause

| # | Root cause | Surfaces | Count | One fix |
|---|---|---|---|---|
| **A** | **Suppressed but not navigated.** The surface is in the screen's yield list, so the screen stops listening, but the surface has no key handling — the radio chooser. Two also escape Back because `closeAll` does not know them. | Radio chooser, refusal card, AboutOverlay, SpecRatioOverlay, CityPickerModal, ChatDrawer (receiver + FM-DX), picker `tcpModal`, picker `editFav` | **8** | An **overlay contract** (`useOverlay(visible, { onBack })` in PanelNav) that does three things in one call: registers the surface in a global overlay count (replacing the hand-kept `anyPanelOpen` and `listNavActive` lists), pushes a nav owner, and makes Back run the surface's own `onBack`. A surface cannot suppress the screen without also owning Back. Then each gets its `useListNav`/`usePanelNav`. |
| **B** | **Not in the list, so keys leak underneath.** The surface draws over a live screen or list that keeps acting on arrows and Enter. | sdrTour, pickerTour, PasswordModal (×2 hosts), IdentModal, VibePinPrompt, MapOverlay, DabPanel, AdvRdsPanel, 7 connection cards, 3 tap banners, idle card, owner notice, LegacyPackageNotice, RecordingsOverlay on the picker, TlsTrustPrompt (incoming) | **23** (incl. TlsTrustPrompt) | Same `useOverlay` contract. Coachmark gets keys once, in `Coachmark.tsx`, for both tours: Enter = Next, Back = Skip. The seven connection cards share one component and one nav. |
| **C** | **Silent skip.** A control inside a navigated surface that never registers (plain Touchable, `PopupKey`, `Switch`, `Slider`, or a `Btn` outside a `NavRow`). | MenuSheet (4), AudioSheet (~14), LocalHardwarePanel (~33), FreqModal (~8), RecordingsOverlay (3), server list (~20), FM-DX screen (5), receiver screen (4), decoder spot rows, sideways selector rows | **10 surfaces, ~90 controls** | Make the shared building blocks register themselves: `PopupKey` takes an optional nav slot; `Toggle`, `VfoLockBtn`, LocalHardware's switches and sliders go through `useNavButton`/`useNavRange`. Then a **coverage test** (`scripts/test_nav_coverage.ts`) that fails when a file using PanelNav renders a `TouchableOpacity`/`Pressable`/`Switch`/`Slider` with no nav registration or an explicit `skipNav` reason — so the next silent skip fails CI instead of reaching Stuart's sofa. |
| **D** | **Native OS surfaces outside the bus.** iOS UIAlertController (and `Alert.prompt`) cannot be answered by a pad, and the UI underneath keeps acting. | 82 alert calls in 15 files; iOS PIN prompt | **82 call sites** | One `appAlert()` helper with `Alert.alert`'s signature: touch → the native alert, unchanged; keyboard/pad in use → an in-app `PopupShell` dialog with `useListNav` (Back = the cancel button). While a native alert IS showing, a counter suppresses the bus. iOS PIN: `VibePinPrompt` whenever a pad is in use. Migrate sofa-path calls first (connect errors, take over, PIN, set default, delete, reset). |
| **E** | **Whole screens with no listener.** | ServerModeScreen (+4 editors), RtlTcpServerScreen, first-launch splash, CrashBoundary, PanelBoundary fallback | **5** | Splash and boundaries: Enter/Back handlers (a few lines each). ServerMode: SHOULD, a screen `usePanelNav` (Android-only, owner task, rarely from a sofa). |
| **F** | **Keys deliberately off on a page we did not write — Back included.** | BrowserOverlay (compat mode, admin pages, OWRX MAP/FILES) | **1** | Keep every shortcut off on the page, but let Back through to close the overlay. The rule was about authorship; the way out is ours. |
| **G** | **A pad cannot type.** | Every text field — ~60 (list in the brief §8) | **all** | **ControllerKeyboard** (the brief). Until then, where a no-typing path exists, make it reachable (bookmark default name, sort chips, canned chat, generate password). |
| **H** | **Logic bugs in existing nav.** | Picker Back index (`:2048`); Enter ≠ tap on directory rows (`blocksApps`, `isFull`) and SpyServer rows (`:2011-2013`); CityPicker Back closes the map beneath; horizontal reveal; Recordings ring for touch users; web pad does not blur a field on Back (`web/client/src/gamepad.ts`) | **6** | Individual fixes, each with a test where the logic is pure. |

---

## 4. Ranked work

### MUST (Phase 1 — the sofa path: list → server → radio → listen → tune → leave)
1. **Radio chooser** (§1).
2. **Overlay contract** (`useOverlay`) and its application to groups A and B on the sofa path: refusal card, the seven connection cards and three tap banners, PasswordModal, VibePinPrompt (+ iOS `Alert.prompt` → `VibePinPrompt` for pads), IdentModal, both tours, the splash, DabPanel (opens by itself), MapOverlay, TlsTrustPrompt when it merges. Fix `closeAll` so Back on the chooser and refusal card goes back to the servers.
3. **`appAlert()`** and migration of the sofa-path alerts: connect errors, compatibility Back/Continue, take over, Receiver full, Connection Error → password, Session ended, set/remove default, "High Detail Maps?".
4. **ControllerKeyboard** — numeric then QWERTY — wired to: frequency, VibeServer PIN and radio PIN, every password box (radio chooser, refusal, PasswordModal, MenuSheet admin, OWRX magic key), custom URL / Add custom server, directory search, bookmark search and bookmark name.
5. **Group C on the sofa path:** MenuSheet SDR CONTROLS / VFO lock / About / admin unlock; AudioSheet toggles + DSP TYPE; server-list row buttons, header buttons, directory search/sort/Retry; FreqModal SHARE.
6. **BrowserOverlay Back** (group F) — a reachable key opens it today and there is no way out.
7. **Group H picker bugs** (Back index, Enter = tap).

### SHOULD
- ChatDrawer nav (and the canned-phrase pad — a typing-free chat).
- LocalHardwarePanel rebuilt on `usePanelNav`.
- AboutOverlay scroll + buttons; SpecRatioOverlay slider; CityPickerModal + its Back clash; AdvRdsPanel.
- FreqModal bookmarks-pane extras; bookmark default name; RecordingsOverlay share/delete/seek.
- FM-DX ♥, KEYS/DRUMS, PAUSED banner, band and AF chips.
- Receiver-screen CenterVfo, EdgeChips, idle card; decoder spot rows tune on Enter.
- Horizontal reveal for sideways selector rows.
- The remaining ~60 alerts through `appAlert()`.
- ServerMode / RtlTcpServer pad support; RtlTcpServer's field → `TvTextInput`.
- Android hardware keyboard into the bus (today "keyboard does nothing there" — the one-scheme promise is iOS/Mac only).
- Web: pad Back blurs a field; share the mapping (brief §11).
- The nav-coverage test.

### NICE
- Mac menu-bar commands; Android TV native focus ring on Touchables.
- Owner notice, AntennaBandNotice, FkaSplash help button, LegacyPackageNotice, PanelBoundary/CrashBoundary keys.
- Favourites reorder by pad (move up / move down instead of drag).
- A waterfall "signal cursor" (hop to the next/previous peak) — the pad's answer to tap-to-tune.

---

## 5. What cannot be made controller-accessible, and why

| Thing | Why |
|---|---|
| Third-party pages (compatibility mode, OWRX/Kiwi/UberSDR admin pages, OWRX MAP/FILES) | Not ours: we cannot know what a key means there (`PanelNav.tsx:131-145`, Stuart 2026-07-25). Only the way out is fixable (group F). |
| The iOS system keyboard | Apple provides no controller input to it; Stuart confirmed on device. ControllerKeyboard is the only way. |
| Share sheet, document picker, in-app browser (expo-web-browser), location / notification permission prompts, iOS Settings | OS-owned windows outside the bus. Mitigation: suppress the bus while they show, and say "finish on the touchscreen" in the opener's line. |
| Paste (bookmark import box `FreqModal:1190`) | Needs the clipboard and a pasted document; touch-only by nature. |
| Pinch / drag gestures (waterfall pan, FmdxDial pins, favourite drag) | Gestures have no pad equivalent as such; their functions are covered by tune/zoom keys, with reorder and signal-hop as NICE substitutes. |
| Android `AlertDialog` / iOS UIAlertController content as such | Replaceable by `appAlert()` (group D), but OS-raised alerts (permissions) are not ours to replace. |

---

## 6. Method and confidence

- Every surface was found from `src/screens`, `src/components`, `App.tsx`, every `<Modal>`, every `Alert.*`, every
  absolutely-positioned layer in SDRScreen, and the main-branch TLS commit. Line numbers are from `b9f643a0`.
- iOS alert and Modal behaviour with a pad is read from React Native's and our native source, **not tested on a
  device**. The first device check should confirm fact 2 (a pad under a UIAlertController) because MUST-3 rests on it.
- Android TV behaviour is from code plus the Kiko notes in `TvTextInput.tsx`.
