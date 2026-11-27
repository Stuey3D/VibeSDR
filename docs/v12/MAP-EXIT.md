# The way off a map (beta96, branch b96-mapexit)

Stuart, 2026-11-27, Almere's ADS-B radio from the Mac app: "right now i think if i were to go to almere's adsb map on the app
i'd be stuck and need to force quit the app to exit the server". His recording showed something worse: the map had CLOSE MAP and
no Servers chip, and CLOSE MAP dropped him on a 1090 MHz waterfall "which may have caused the server to exit a mode its not
meant to exit".

## RESUME HERE
- Built and tested; not on a device yet. **Owed:** a real pass on the Mac app and an iPhone against a Single mode: ADS-B radio
  (a local VibeServer with `radios[0].singleMode: "adsb"` will do; not Almere). Check: no radio screen before the map, the
  Servers tab on the left edge, LEAVE SERVER, Android Back leaves, "Other radios on this server" on a two-radio server.
- The web page has the same gap (below). b96-mapcards is putting the Servers chip on the web map; web/client was not edited here.

## What it was (before)
- The maps are a React Native `Modal` (MapOverlay). The waterfall's ServersChip sits *under* it, so a map had no Servers chip.
- **Single mode / only mode radio** (`onlyMode` adsb): the map's key read CLOSE MAP (`only`), which only put the map away. The
  listener was left on the radio screen at 1090 MHz with nothing to do there; ‹ Servers on that screen was the way out.
- **Shared dial, ask-before-tuning lock or an owner's scan, mode not "only"** (a landing map without Single mode): EXIT <MODE>
  was greyed, its press refused (nudge), Android Back (`onRequestClose = closeGuarded`) refused too, no Servers chip, no
  swipe on a Modal: **stuck until a force quit**. (A pad's Back went to `onClose` directly, bypassing the guard.)
- Android Back on the radio screen only ever closes the chat (`BackHandler` consumes it).
- **What CLOSE MAP sent to the server: nothing.** In the only mode `closeAirSea` only cleared the view flags (exitLabel CLOSE
  MAP, so no `exitAdsb`). And the server refuses every leave anyway: `test-server-map-locked.mjs` sends ADS-B off, tune, mode,
  passband, zoom/reset, rate, IF filter, DAB/AIS/datalink/ISM/NDB/SONDES/VOR on and a SATELLITE pick to a Single mode: ADS-B
  radio — no adsb_off, reports carry on, a new listener still lands in ADS-B (passed on the unmodified server too).

## What it is now
- `src/utils/mapExit.ts` — the rules (pure). `scripts/test_map_exit.ts` — them, plus the wiring.
- The Servers chip is drawn **inside every map's Modal** (MapOverlay `servers`), tucked to its left-edge tab, from SDRScreen's
  **one** `serversChipProps` object (the waterfall's chip spreads the same object). Leaving is never behind the ask lock.
- ServersChip: **Other radios on this server** (only where the door, or the server behind a `/r/<id>` address, has another
  radio) → that door's radio list, the visit carried (doorReturn.ts).
- **Back on a map** (Android, pad, keyboard — one rule, `mapBackAction`): menu open → close it; map-locked → leave the server;
  dial-locked EXIT → offer "Leave this server"; else the map's own EXIT.
- **Map-locked radio** (onlyMode adsb / ais / ndb / sondes / vormap / avdata; the signed-in admin is told '' and keeps the
  radio screen): `shownMapOf` `lockedMap` keeps the AIR & SEA map up; the mode's view is opened at once and put back; EXIT
  reads **LEAVE SERVER** and leaves the server; Back leaves; no slide animation.
- **No radio-screen flash:** VibeServer's `/vibeserver.json` now says the listener's `onlyMode` before any socket; the app reads
  it before connecting (no-door and `/r/<id>` addresses; a radio chosen at the door, 1.5 s cap; the per-radio probe).

## Web (read only)
On a map-locked radio the web map's EXIT key is hidden (syncOnlyMode) and Escape is refused with a pill (asExit →
adsbSetMode(false) → onlyModeRefuses). There is no in-page way from the map to the landing or another radio: only the
browser's own Back / reload / address bar.

## Tests
- `test-server-map-locked.mjs` (VIBESERVER_BIN): 10/1 before (onlyMode not in /vibeserver.json) → 11/0.
- `test-server-only-mode.mjs`: 36/0 after the server change.
- `test_map_exit.ts`: 36/21 with the base tree's sources (MAPEXIT_ROOT) → 57/0.
- Updated for the new shapes: test_map_shared_app 28/0, test_dual_server all, test-only-mode-clients 96/0.
- Unchanged and passing: test_nav_coverage, test_gamepad_nav, test_servers_chip, test_map_page_kind, test_map_scope_app,
  test_picker_return, test_ask_before_tuning, test_airsea_viewer, test_airsea_standalone, test_popup, check-servers-menu-layer,
  check-map-overlay; `tsc` clean for src/.
