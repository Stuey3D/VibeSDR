# SAT-SCHEDULE — schedule a satellite pass (beta96)

Branch `b96-satsched` (off v12-experimental = beta95). Client-only: no server change was needed.

## RESUME HERE
- **Built and committed:** the pure rules (`src/services/satSchedule.ts`), the web panel (`web/client/src/satellites.ts`
  + `main.ts`), the app (`src/components/SatellitePanel.tsx` + `src/screens/SDRScreen.tsx`). Also the copy (About beta96
  note, web tutorial line) and the satellite box polish (below).
- **Tested:** pure suites and the headless web panel only. **No device run**: the app UI (rows, keys, the interval
  runner) has been type-checked, not seen. The first real check is a phone on a single-user server or Local Hardware:
  schedule two overlapping passes, PRIORITISE one, leave the app open over AOS/LOS.
- `vibe_web_page.h` was rebuilt and committed in its own commit. Drop it and regenerate once at the merge if other
  branches also touched the web.
- **Open for Stuart** (see the end of this file): REMIND now works on a shared dial; "alone on a shared dial" counts as SCHEDULE.

## Stuart's decision (11-26)
> "single user and locked range local hardware whereby other users are not affected, the remind button should be a
> SCHEDULE button, and if a pass overlaps another then a button to PRIORITISE the pass should be present. Shared VFOs
> keep the remind system."

Mac Ha's request: "select from the satellites list those interesting, and it tunes to each one once they get into view".

## Which key — `satSession` / `satPassKey`
The key is decided from what the server says, never guessed:
- `sat_list source.pickPrivate`
- the dial message, which is sent only for a shared tuner, and its `listeners`
- config `locked`
- for the app, whether the session is a loopback one (this device's own radio)

| situation | key |
|---|---|
| shared dial, others on it (or the count is unknown) | **REMIND** |
| shared dial, you are the only listener now | **SCHEDULE** (`alone`; says it becomes reminders if someone joins) |
| `pickPrivate` true: a single-user radio | **SCHEDULE** (`single`) |
| `pickPrivate` true + locked range | **SCHEDULE** (`locked`); downlinks outside the range are greyed, so they get no key |
| the app's own radio (Local Hardware) | **SCHEDULE** (`local`) |
| an older server that does not send `pickPrivate` | no key (as before) |

- SCHEDULE may go on any listed pass, later ones included. REMIND stays on the current-or-next pass of each downlink.
- The line under the list says why it is SCHEDULE or REMIND here. It also says honestly that the schedule runs only
  while the page or app is open and connected.

## SCHEDULE
- **At AOS:** the client makes the ordinary pick, exactly as a tap on the row does: tune, AUTO DOPPLER and the
  decoder. It enters SATELLITE first if the listener was not in it.
- **After LOS:** it goes back to what the listener was on before. That is their satellite pick, or the frequency and
  mode; if they were in SATELLITE with nothing picked, the mode stays on with nothing picked.
- **Back-to-back passes:** if another scheduled pass starts within `SAT_SCHED_BRIDGE_S` (120 s), the radio stays where
  it is and that pass takes over. The place to go back to stays the original one.
- **Take-over:** once the server has confirmed the scheduled pick, any other pick, or leaving SATELLITE, counts as the
  listener taking over, and nothing is put back. Cancelling the pass while it is running has the same effect.
- **On the row:** "SCHEDULED · tunes 20:42–20:55, then back", and the key reads CANCEL. A status line shows the pass
  running now (and what it will go back to), or the next one.
- **Storage:** kept per server in `vibe.satSchedule`, in localStorage on the web and AsyncStorage in the app.
  - Capped at 20 per server and 60 in all.
  - Passes that are over are dropped.
  - Each `sat_list` moves an entry with its pass, as reminders already were.
  - The web page creates the panel at start-up when a schedule is stored, so it runs with the mode off.
- **Where it runs:**
  - **Web:** only while the page is open and connected. A browser notification is shown when it tunes with the tab
    hidden, if permission was granted. Permission is asked on the first SCHEDULE press.
  - **App:** only while the app is open and connected. Reminders use the in-app banner; there is no OS notification
    module (Stuart's 1.6 decision). If the app comes back mid-pass, the pass is picked then.
- **Fire-time check:** if the downlink is out of reach, the orbit is dead or the satellite is no longer listed, it does
  not tune and the line says why.

## Overlaps — `satSchedPlan`
- **Ranking:** PRIORITISED first, then the higher pass (max elevation), then the earlier one.
- **The loser** runs only for the minutes left over: before, after, or both (two pieces). A remainder under
  `SAT_SCHED_MIN_S` (60 s) is skipped, and its row says so.
- **PRIORITISE** appears on each scheduled pass that overlaps another one. It is a toggle with one winner per overlap.
  Pressing it again goes back to the default.
- **Under the list:** "Overlapping passes: the higher pass wins unless you PRIORITISE one".

## Degrade
If the dial becomes shared with someone else, the session kind becomes `remind`:
- A run in progress ends where it is, with no return ("Another listener joined this dial — the schedule stopped tuning").
- Stored passes fire as reminders at AOS − lead, with the note "Scheduled — but another listener is on this dial now,
  so it did not tune by itself".
- Their rows read "SCHEDULED · reminds you", and FORGET cancels them.
- If the listener is alone again before AOS, the passes tune again.

## Box polish (tester's screenshot, AO-73 on a phone, 11-26)
1. **Waterfall labels showing through the text.** The app box body is now opaque behind its text
   (`decoderTextBacking`):
   - the faceplate's solid colour when transparency is off;
   - the metal window's colour on metal faceplates;
   - otherwise the glass tint at 0.96 alpha, which is the web's `--ov-panel96`.

   The web box was already at 0.96.
2. **Globe over the text.** The readout now uses `satReadoutLayout`: the small globe sits beside the words only where
   they keep a column of at least 200 px, and goes under them otherwise. The VFO B keys wrap (web) or shrink (app)
   instead of running under the globe.
3. **"192.168.1.2:48000" on the globe.** The app passed the server entry's name (`instanceName`) as the receiver label,
   and for a server added by address that name *is* the address. `globeRxName` now hides anything address-like, so the
   label reads RECEIVER instead. It catches:
   - IPv4 and IPv6 addresses;
   - `host:port`;
   - URLs;
   - `localhost`, `.local`, `.lan`, `.home`, `.internal`, `.localdomain` and `.home.arpa`.

   No other place feeds a server name into the waterfall or globe labels (grep `rxName`).
4. **SMALL globe draws no text** (Stuart). `globeLayers`: SMALL keeps only the orbit, the footprint and the receiver dot.
   BIG is unchanged.
5. **SMALL glyph** (Stuart). `globeGlyph` draws a 5 px square body with panel lines 4–9 px from the centre.
   - It lies along the BIG bird's boom axis `u`, so the two views agree.
   - Corners are snapped to whole pixels, panel lines are 1.5 px, and it uses the bird's colour.

## Tests (2026-11-26, M5, all synthetic)
| suite | result | covers |
|---|---|---|
| `scripts/test_sat_schedule.ts` | 70/0 | key choice: shared / alone / single / locked in and out of range / local / old; the book: add, refuse, cap, store round trip, refresh, prioritise; overlaps with and without priority, skipped and split remainders, three-way; the runner: AOS, back, bridge, take-over, a long gap; degrade; words |
| `scripts/test_sat_layout.ts` | 51/0 | addresses hidden, `globeLabels` never shows one, SMALL/BIG layers, glyph square + heading + whole pixels, readout layout, opaque backing on default/silver/black × transparency on/off |
| `scripts/test_satellite_mode.ts` | 238/0 | one assertion changed on purpose: a shared dial no longer blocks LISTEN |
| `scripts/test-web-satellite-mode.mjs` | 168/0 | REMIND section now runs on a shared dial; new SCHEDULE section: keys, row words, storage, PRIORITISE, AOS pick, overlap hand-over, back after LOS, degrade toast, mid-pass join; SMALL orbit ink threshold lowered (no arrows now) |
| `scripts/test-web-sat-filter.mjs` | 70/0 | fixture moved to a shared dial (REMIND's home) |
| `test_sat_filter`, `test_satellites_app`, `test_nav_coverage` | pass | |
| tsc (4 GB) | clean on `src/` + `web/client/src/` | |

- `test-web-satellite-mode`'s "re-asked every 3 s while fetching" failed once and passed on the next run. It is a
  timer-edge flake (a 3.6 s sleep against a 1 s tick), and it existed before this branch.
- Not run here: `test-web-sat` and `test-web-sat-labels`, which need `VIBESERVER_BIN` (server suites; no server change).
- New suites are registered in `scripts/run-tests.sh`.

## Open for Stuart
- **REMIND on a shared dial now works** (key shown, LISTEN live, through ask-before-tuning). In 1.6 it was hidden there.
  "Shared VFOs keep the remind system" was read as "REMIND belongs there"; say if it should stay hidden on shared dials
  instead.
- **Alone on a shared dial counts as SCHEDULE.** If someone joins, scheduled passes degrade to reminders. The
  alternative is REMIND whenever the dial is shared, however many are on it.
- **Schedules are kept across reloads and restarts.** Reminders are not. A scheduled pass is a plan, not a pop-up.
