# Setup UX: collapsible sections, the red "!", the app in Stuart's order, the server's frame rate (b96-setupux, 2026-11-26)

Stuart, 11-26, on the Sony TV: scrolling the long Setup screen with a remote "took ages". Four asks, all built here:

1. **Collapsible sections** on the web Setup page (server tab + every radio tab) and the app's server screen.
2. **The red "!"** on a section that holds something the owner must set before things work.
3. **The app's server screen in Stuart's order** (the web page already was, b96-bgsplit).
4. **Frame rate**: a machine-wide default on the server tab, and a per-radio override beside the sample rate.

## RESUME HERE

**★★★ Superseded in part by b96-appserver2 (2026-11-26, Stuart):** the red "!" is no longer shown while browsing. Headers
have **three states** — green (set up), amber (worth setting: location, PSK Reporter with no callsign, a radio not detected,
on the app remote settings off; a blocking reason before any Save/Start also reads amber) and **red "!" only after a SAVE
(web) or START (app) it stopped**: that tab selected, the section opened, the save/start refused with one line, red until
fixed. Web: `SEC_BLOCK` / `SEC_ADVISE` / `secSaveStops` / `SEC_TRIED` (vibe_setup_page.h); app: `secState`, `startRefusal`,
`startBlocks` (setupSections.ts, ServerModeScreen). The app's server screen is now **tabbed SERVER | RADIO** and its
Simple / Advanced row is gone. Details: SERVER-CHOICE.md RESUME HERE. Tests: test-setup-sections.mjs 63/0,
test_setup_sections.ts 89/0. The table below is the b96-setupux record (its "red" rows now read "red after a refused save").

| Piece | Where | State |
|---|---|---|
| Web sections: a `<button class="secHead">` put in front of every `.card` of `#serverPane` / `#radioPane` (title + one line), closed by default, CSS hides the rest of a closed card — **nothing is moved**, every id and show/hide keeps working | `vibe_setup_page.h` `secEnsure`, `secRefresh`, `SEC_SUM`, CSS `.secHead` / `.card.sec` | built, headless-tested |
| Remembered per device: `localStorage["vibeSetupSections"]` (try/catch; a private window starts closed) | `SEC_STATE`, `secSave` | tested (reload) |
| OPEN ALL / CLOSE ALL (`#secAllRow`) — the tab in front of you | `secAll` | tested |
| Keys: ↑ ↓ / W S header to header (a closed section's down steps on; an open one's goes in), Enter / OK opens and moves focus inside, Escape (Backspace outside a text box) back to the header | `secHeadKey`, the document keydown | tested with real CDP key events |
| Red "!": `SEC_MUST` — the SAVE PATH'S OWN refusals, refactored into one rule each (`dirWhy`, `pskrWhy`, `afWhy`, `modesWhy`; the Beast feed's `beastAddrWhy`), the server's facts (a radio not `configured` is not served; `SDR_ALARM` = the server says it is gone; no location = the satellite engine's "this server has no location set") | `vibe_setup_page.h` | tested: location, PSK Reporter, never set up, every mode off |
| A "!" opens its section; closed by hand while red stays closed for THAT reason; fixed under the owner's hands it stays open (never on a tab switch) | `secWantOpen`, `__mustDismissed`, `__stayOpen`, `__mustCtx` | tested |
| A card whose rows another function moves (layoutLockedRadio inserts the span at the top of the hardware card) keeps ONE header, first | `secHeadOf` + re-seat in `secEnsure` | tested (locked ↔ unlocked) |
| The loose "Keep the spectrum sharp when zoomed in" line is its own card, **Spectrum detail** | markup | tested |
| App sections: `SetupSection` (header, one D-pad stop with its own focus ring; a closed section mounts nothing, so the remote steps header to header), `SectionAllRow` | `src/components/SetupSection.tsx` | esbuild OK; **not type-checked** (tsc on the screen needs > 1 GB) |
| App rules (order, open/closed, OPEN/CLOSE ALL, the stay-open, parse) — pure | `src/utils/setupSections.ts` | unit tested |
| App screen cut into section bodies (`secBody_*`, each keeping its own condition), drawn through `secRender(serverSecs, SERVER_ORDER)` / `secRender(radioSecs, RADIO_ORDER)`; AsyncStorage `vs_setup_sections` | `ServerModeScreen.tsx` | esbuild OK; source-tested; **not run on a device** |
| App red "!": no admin password (Start refuses), the flight-tracker entry (`afProblem`, Start refuses), PSK Reporter with no name (the server's "Not sending: it needs your callsign"), no location (`locationMust`) | `serverSecs` | unit tested (rule), source tested (wiring) |
| Frame rate: `ServerConfig::fpsDefault` (10; a hand-edited value outside 1–60 reads 10), `RadioConfig::fpsFollow` (ABSENT = false = the radio's own rate, so every existing config runs exactly as before), applied in `effectiveFor`; `effectiveFps()`; the two live write-backs in `main.cpp` keep a following radio's own numbers | `vibeserver_config.*`, `main.cpp` | built, tested (test-config-radios) |
| hwinfo `fpsEff` / `fpsFollow` / `fpsServer` (only once told — a Linux radio process; Android sends none) | `local_sdr_shim.cpp` `setVibeServerFpsSource` | built (vibeserver compiled); **not read by a client yet** |
| Page: the server tab's **Frame rate** card (`#fpsDefault`, "Used by: …"), each radio's rate menu offers "Use the server's frame rate (N fps)"; a radio never set up follows by default, an existing one keeps its own; not on a phone's page (one radio) | `fpsDefaultShow`, `fpsFollows`, `fpsEffWords` | tested |
| About beta96 note; the b96-bgsplit order note no longer says the rate is only per radio | `AboutOverlay.tsx` | done |

### ★ The hook for b96-serverchoice (the other agent)

- **TOP BAR**: `const serverChoiceTop: React.ReactNode = null;` in ServerModeScreen, drawn as `{serverChoiceTop}` directly under
  the title and the one-line description — above the prefs banners, OPEN ALL / CLOSE ALL and every section. Nothing else is
  drawn above it. Fill it (or replace the const) on that branch.
- **Device-only "Change admin password / PIN"**: `const serverChoiceSections: { devicePassword?: SecDef } = {};` — a `SecDef`
  (`{ key: 'devicePassword', title, summary, must, body }`) put there is drawn in the SERVER group at `SERVER_ORDER`'s
  `'devicePassword'` slot (after the benchmark and Protocol, before "Admin password and PIN", the existing SECURITY / PASSWORD /
  PIN block). If that branch REPLACES the existing password UI, drop `secBody_security` from `serverSecs`.
- **Simple / Full / RTL-TCP entry**: the existing PROTOCOL block is the `protocol` section (second, after the benchmark), and the
  Simple / Advanced MODE row (`secBody_mode`) is drawn plain above RADIO SETTINGS — both are that branch's to replace.
- A section key not in `SERVER_ORDER` / `RADIO_ORDER` is drawn after the listed ones, never lost.

### Decisions taken (open for Stuart, one line each)

- App: the SERVER group is drawn before the RADIO group (as the screen was, and as the web page's SERVER tab comes first).
- App: Protocol stays second (it decides whether the rest exists); MODE (Simple / Advanced) stays a plain row above RADIO SETTINGS.
- App: no per-radio frame-rate override — the phone serves ONE radio, so its one rate is the server's (the section is "Frame rate").
- Web: a radio never set up follows the server's rate by default; every configured radio keeps its own (identical behaviour).
- App location "!" shows whenever no location is set (Not set) — Stuart's example; an owner who prefers not to share it sees it red.

## Tests (all silent)

| Test | Result | What |
|---|---|---|
| `scripts/test-setup-sections.mjs` | 50/50 | headless MUTED Edge + a real front door built from this tree (no radio process): headers on every card, closed by default, the red "!" (location, PSK Reporter, never set up, every mode off) clearing live and staying open, toggle + localStorage + reload, OPEN / CLOSE ALL, real ↓ ↑ S Enter Escape keys, a tab switch not keeping a "!" open, locked ↔ unlocked keeping one header, the frame rate saved (server 20, the new radio following, the old one at its own 5) |
| `scripts/test-setup-maxfps.mjs` | all good | + "Use the server's frame rate": follow / override / an old radio unchanged / a new radio follows / a phone never follows; the handler; the save |
| `vibeserver/test-config-radios.cpp` | 311 checks, 0 fail | two radios: an old file unchanged; follow vs override; save/reload; the server's rate changing; bad values |
| `scripts/test_setup_sections.ts` | 70/70 | the app's order, open/closed rules, OPEN / CLOSE ALL, the stay-open, the one-line summaries, the screen's wiring + the serverchoice hooks |
| `scripts/test_nav_coverage.ts` | ok | + SetupSection: every touchable registered, focusable on the TV, a closed section mounts nothing, the D-pad order (OPEN/CLOSE ALL → server headers → radio headers) |
| `check-android-settings-parity` | 218/0 | the titles moved to the sections; `fpsDefault` classified (not on a phone) |
| `check-icom-servermode` | 67/0 | the Icom gates now read the section bodies' conditions |
| `test-setup-order` 15/15, `check-setup-page`, `test-setup-android` 77, `test-setup-tabswitch`, `test-setup-keep-run` 87, the other node-only `test-setup-*` | pass | unchanged behaviour |

Pre-existing, not from this branch (same result on the parent): `test-setup-paul-tabs` "a radio with every mode off is not
saved" (the radio is single-mode derived from its ticks, so the refusal does not apply — the logic is unchanged here),
`check-remote-keys` 2, `test_popup` 1, `test-ft8-deep-owner-only` 1.

Pictures looked at (headless, 1280 and 390 px): `SECTIONS_SHOT=<dir>` on test-setup-sections writes them.

## Not done / owed

- The app screen on a real device (the Sony with its remote, a phone): the TV focus walk header → header → into a section is
  Android's own focus search over `focusable` headers — **not run on a TV**.
- The app's type-check (tsc on ServerModeScreen needs > 1 GB).
- hwinfo `fpsEff` is published but no client shows it yet.
- Android gradle not run (no Kotlin / JNI changes on this branch).
