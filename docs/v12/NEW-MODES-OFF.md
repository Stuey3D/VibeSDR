# NEW MODES OFF (beta96) — an update never switches a new mode on behind a restricted owner's back

Branch `b96-newmodes` (off v12-experimental = beta95). Stuart 11-26, from Paul's multi-radio server ("Almere"):

1. **The bug.** Paul set a radio up as pure ADS-B (every other switch off), which turns on the only-mode exit lock
   (`onlyModeNow`). The owner's list is a BLOCKED list, so every mode a later beta added came in ON — the lock stopped
   applying and listeners could leave ADS-B. beta93's fix covered only VOR MAP, and only on a list that named EVERY
   demodulator; his did not name `dvsig` (generation 87), so it never fired.
2. **(B) "When I went back in, all modes were ticked again."** Not a lost save: the Setup page drew the Modes card only in
   `renderBands` AFTER its locked-range `return` (and `capsApply` skipped locked radios). That code dates from 2026-09-05,
   when the modes list was part of the single-mode band card. His ADS-B radio is LOCKED (1090 MHz), so its tab kept
   showing the previous tab's boxes (an all-ticked radio). Fixed: the card is drawn for every radio.
3. **(A) SATELLITE on a pure ADS-B radio.** hwinfo `doppler` was sent on reach alone. Now also needs the owner's
   switches to leave SATELLITE something to do (`vsSatModesAllowed`: "sat" not blocked, one of NFM / USB / LSB on — a
   single-mode radio has them all off); a pick is refused the same way; the locked-window reach check applies to ANY
   locked centre (it needed more than one listener). "sat" is a new switch (generation 96). Clients: `satCapFrom`
   honours `blocked` (sat, or nfm+usb+lsb) and `onlyMode`. The coordinator's b95-satonly hotfix (satPickKey refusal on an
   only-mode radio, the web/app null caps) is separate and merges on top.

## The rule (vibe_decoder_reach.h `newModesState`; copies: vibe_setup_page.h, src/utils/newModes.ts)

* Every switch on the Modes card has a **generation** — the release it first appeared in (`modeGens()`): 0 = the 11.x list,
  63, 80, 87, 90, 91, 93, 96 (`sat`). Derived from git (`git log -S'id: "x"'` on vibe_setup_page.h → first tag containing
  it; lite tags cover 63–95, gaps at 76/82/85/86 round UP to the next tag). `kModeGenNow = 96`.
* The owner's list carries two words of its own (never published — `effectiveBlocked` drops them):
  `seen@N` (they have been shown every switch up to N) and `newmodes@on` / `newmodes@off` (absent = automatic).
* A switch newer than `seen` is **NEW**. A new switch is OFF unless the owner ticked it (`+id`) when:
  * the radio is **single-mode by the owner's ticks** (every listening demodulator they saw off, ≤ 1 whole-radio mode they
    saw on) — ALWAYS, whatever the answer: the exit lock must hold;
  * or `newmodes@off`;
  * or automatic and the owner blocked ANY switch of the table (restricted). Nothing blocked = new modes ON, as ever.
* **Migration (no `seen@`)**: unrestricted → everything seen. Restricted → `seen` = the newest generation any token in the
  list names (switches, `+id`, and the options `lora+all`, `avdata+8m`, `ft8±deep` …) — the oldest release that can have
  written it.
* **Save** (`newModesStamp`, called by `collect()` for every radio whose card was drawn this visit): each new switch keeps
  exactly the state it has (off → its id written, on → not; a region default's `+id` kept), then `seen@<now>`. The
  enforced list is identical before and after. On opening a card, a list with no stamp gets its derived `seen@N`
  (`newModesFreeze`) so editing cannot move what counts as new.
* The app's server screen has no Save: every tap stamps (`NM.stamp`), and "Keep as shown" stamps without a change.

## Setup page (modeCard) and the app (ServerModeScreen)

* One-line notice "This update added N new switches to this radio: … They are off until you tick them (marked NEW below)".
* NEW switches carry "NEW (off until you tick it)" / "NEW in this update" beside them.
* "When an update adds new modes: Switch them on / Leave them off" — fixed at off (disabled) on a single-mode radio.
* **All on / All off** per radio (Stuart's addition): boxes only, nothing saved until Save (the app: an Undo button, as it
  saves every tap). All on ticks what the radio draws, never a `+` opt-in (message text etc.). All off switches off
  every switch — hidden ones too, so one whole-radio mode left IS single-mode — except the landing mode
  (`landingAdsb` → adsb, a DAB landing station → dab, else the landing demodulator; `cw` → `cwu`); no landing mode it can
  name = refused with why. Not grouped by category (it is a parent/child tree), so no per-group buttons.
* **Never a radio with no modes**: both save buttons refuse (`modesStop`) when a drawn radio has every listening
  demodulator and whole-radio mode off; the app refuses the tap that would do it.
* `vibe_radio_caps.h allModes` now names `varatag`, `satlabels` and `sat` — the first two were drawn by the page but never
  named, so on any radio with caps they were HIDDEN.
* Android: `blockedModes` cap raised 400 → 1200 (every switch off + `seen@96` passes 400; a cut list loses its last ids).
* `sdrReloadRadios` keeps the open radio by serial.

## Tests (how)

* `vibeserver/test-new-modes.cpp` (CMake `new-modes`, run from the repo root): migration (unrestricted, restricted,
  single-mode), **Paul's exact shape** (all switches ≤ gen 80 off except ADS-B/Comm-B → ADS-B only, lock ON; with
  "switch them on" still locked; +vormap = two modes), stamped lists, the words never published, the table vs the
  page's BLOCKABLE / MODE_GENS and the app's BLOCKABLE, source contracts (onlyModeNow → `vibereach::onlyModeLeft`,
  hwinfo `modes` via each WhyNot's `vsModeBlocked`). 575 checks.
* `vibeserver/test-decoder-reach.cpp`: the beta93 VOR MAP cases moved to generations; region cases stamped `seen@96`.
* `scripts/test_new_modes_clients.ts` (tsx, silent): the page's real functions (vm-lifted) and `src/utils/newModes.ts`
  agree on every case; the stamp keeps the enforced list; a simulated generation 97 after a save; All on / All off; the
  wiring; `satCapFrom` on Paul's radio shape.
* `scripts/test_decoder_reach_clients.ts`: updated for the new rule.
* `scripts/test-setup-paul-tabs.mjs` (headless MUTED Edge + a real front door, no radio process): a 7-radio config shaped
  like Paul's (made up — never his), ADS-B radio fifth, locked 1090 MHz. HackRF tab then ADS-B tab: only ADS-B ticked;
  NEW marks; notice; only-mode note; policy fixed; All on / All off save nothing; Save → `seen@96`, the other radios
  untouched; reopen → still ADS-B only; a no-modes radio is not saved.
* `scripts/test-setup-mode-tree.mjs`: its saved-CSV reader ignores `seen@` / `newmodes@`.

## RESUME HERE

* Results of the runs are in the agent's final report (build, Edge test, gradle).
* Open for Stuart: the restricted-config default (below), and whether "absent" radios need a tab mark beyond the existing
  "missing" alarm (Stuart's rule: a radio on loan is never called unplugged — left as it was).
* ★ When a switch is added to BLOCKABLE: add it to `modeGens()` with the release it ships in, raise `kModeGenNow`, and
  copy it into the page's `MODE_GENS` / `MODE_GEN_NOW` — `test-new-modes` fails otherwise.
* ★ Collateral to know: a restricted list with no stamp whose newest token is old (e.g. only `dab` blocked) gets every
  switch since that generation OFF after this update (ADS-B, ISM … for an 11.x-era `dab`). That is the brief's rule
  ("restricted → new modes off"); the notice on the Setup page names them. A benchmark-written list (a weak box's
  `rds`, `spotsband`, `rade` …) counts as restricted too.
