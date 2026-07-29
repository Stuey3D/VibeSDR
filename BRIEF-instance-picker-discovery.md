# BRIEF: Instance Picker — Collapsible Grouped Lists + Better Sorting + Discoverability

**Project:** VibeSDR (React Native / Expo)
**Author:** Stuart Carr (Stuey3D), spec dictated 2026-07-17
**Status:** Draft for implementation. Pure client-side UI + local persistence — no server dependency.
**Target screen:** `src/screens/InstancePickerScreen.tsx` (currently: two sorts 📍NEAR / 📶SNR + a text filter over a flat list). Then PORT the concepts to the spike (standalone watch) as a second step.

---

## 1. Goal

Make **2000+ public SDR servers** browsable. The flat sorted list doesn't scale — replace it with **collapsible, grouped lists** where the group most relevant to the user is **open by default** and the rest are **collapsed**, plus richer sorting and a map view.

---

## 2. Directory views — grouping rules

The directory is presented as **collapsible section groups**. Which grouping is used depends on what we know about the user:

### 2.1 Distance bands (when Location IS granted)
Group receivers into distance bands from the user:
- **0–100 miles** (OPEN by default — the nearest)
- **100–500 miles** (collapsed)
- **500–1000 miles** (collapsed)
- **1000+ miles** … (collapsed; extend bands as needed)

Within each band, sort nearest-first. **Only the nearest band is expanded on load; the rest collapse by default** — this is the whole point: prevent information overload from 2000+ servers. The **Expand-all** button is the escape hatch for a user who just wants the big flat list we have today.
**Band edges = implementer's call (Claude's discretion).** Pick sensible brackets that keep the nearest group usefully small (candidate: 0–50 / 50–150 / 150–500 / 500–1000 / 1000+ mi — tune on real data). Miles for display (user's preference).

### 2.2 Country grouping (NO location needed)
We can determine the user's **country without Location Services** — via the device locale/region (`expo-localization` `getLocales()[0].regionCode`, no permission prompt). Group receivers **by country**:
- **User's country** — OPEN by default.
- **All other countries** — **alphabetical order**, **collapsed** by default.

Country comes from `SDRInstance.countryCode` (ISO-3166 alpha-2, already in the directory data) + `flagEmoji()`.

### 2.3 SNR grouping (instances that present SNR)
For directories/instances that report band-condition SNR (`bestSnr`):
- **Excellent SNR** servers listed first (OPEN).
- The rest **collapsed** by default.
(Define the "Excellent" threshold; tune on real data.)

### 2.4 The GROUP-BY control = the existing top-right cycle button
Today a top-right button cycles the listing type (Location ↔ SNR). Keep that pattern; the cycle's OPTIONS depend on location permission:
- **Location AVAILABLE** (device location granted **OR** a manual city/grid set — see below) → **Location and Countries are TWO SEPARATE options** → cycle is **[Location, Countries, SNR]**.
- **Location UNAVAILABLE** (denied AND no manual location) → cycle is **[Countries, SNR]** (no Location option). Message offers BOTH routes: *"To sort by distance, allow coarse location in Settings — or set your city / Maidenhead grid."* (Countries needs no permission — locale-derived.)

**Manual location fallback:** if the user won't grant location, let them enter a **city or Maidenhead grid locator** (reuse `CityPickerModal` + the Maidenhead-grid entry the VibeServer flow already uses). Setting one gives us coordinates → distances compute → the **Location option becomes available again** (and the nearest distance band opens). Persist the manual location.
(SNR only in the cycle where instances present SNR.) Whichever option is selected renders as its collapsible grouped list (§2.1–2.3).

### 2.5 Expand/Collapse-all
A single **Expand all / Collapse all** toggle button so a user can blow the whole directory open (or shut) at will.

---

## 3. Map view
One competitor offers a **map of servers**. We already ship **Leaflet maps** (MapOverlay / FT8 map) — reuse it to plot receivers by lat/long (`SDRInstance.latitude/longitude`), tap a pin → connect / details.
- **Entry point:** a **map button placed NEXT TO the sort button**, inside an open directory. Tapping it shows **that directory's** receivers on the map (list ↔ map for the current directory).

## 3a. Typography + accessibility (directory lists) — A PRIORITY, NOT POLISH
**Why it matters:** a large share of the SDR audience is the OLDER generation with failing eyesight. Legibility and honouring the system text-size setting are first-class requirements here, not nice-to-haves — design the lists for readability at large sizes from the start.
- **Bigger + lighter text.** The directory rows are currently small and dim — bump the base size and make them **white or cream** for readability over the dark UI. (Legibility first; this is a real complaint.)
- **Support OS-level text size (Dynamic Type).** Honour the system text-size setting in the **directory lists** at least (and consider the menus). RN `Text` scales with the OS by default unless `allowFontScaling` is disabled — audit where it's disabled and re-enable in these lists.
- **Don't let large text truncate off-screen.** Rows must **wrap / flex / reflow**, not run off the edge, when the user has large text set — flexible row layout, wrapping secondary info, `numberOfLines` used deliberately, and a sensible `maxFontSizeMultiplier` cap where a row genuinely can't grow. Verify at the largest accessibility sizes.

---

## 4. Favourites — reordering + smart sort (FAVOURITES ONLY)
Favourites get a **sort button** cycling / choosing between:
- **Alphabetical**
- **By server type** (ubersdr / owrx / kiwi / fmdx / spyserver / rtl-tcp)
- **By SNR**
- **By location** (distance)
- **By country**
- **Most visited** — track how many times the user opens each favourite (local counter, incremented on connect) and sort by it. **Favourites only.**
- **Manual** — tap-and-hold to **drag** favourites into the user's own order (persisted). (Needs a draggable list — e.g. react-native-draggable-flatlist or an RN gesture-handler reorder.)

The chosen favourites-sort mode persists.

---

## 5. List membership / separation
- **Local discovered** (VibeServers via mDNS + RTL-TCP discovered) get their **OWN "local discovered" list**, separate from the public directory.
- **Manually-added** and **favourited** servers save to **Favourites** regardless of source.

## 5a. Cross-device sync (phone ↔ watch) — WHOLE FEATURE PORTS TO THE WATCH
This picker is **back-ported to the standalone watch (spike)** so the FULL server experience is on the wrist — not just a subset. So:
- **Favourites SYNC bidirectionally between the phone app and the watch.** A favourite added/removed/reordered on EITHER device shows on both — explicitly INCLUDING a favourite **entered ON THE WATCH → synced back to the iPhone** (not just phone→watch). Merge, don't overwrite: both directions are equal peers. (Companion watch ↔ phone: WCSession is the obvious channel. Standalone spike ↔ phone: needs a shared store — decide mechanism, e.g. iCloud key-value / sync when in range. OPEN.)
- **Most-visited counter is COMBINED across phone + watch** — the total visit count for a server is phone-opens + watch-opens summed, so the "most visited" sort agrees on both devices. Implies the counter is part of the synced favourites payload, not a device-local tally.
- **Synced favourites MUST include manually-entered servers** (host/port/URL/grid — not just directory picks). Reason: so a user never has to enter server details on the watch's **tiny keypad** — they add it once on the phone, it appears in the watch's favourites. This is a primary motivation for the sync, not an edge case.
- **The whole picker (grouped lists + sorting + favourites) ports to BOTH the companion watch AND the standalone spike** — the full server experience on the wrist, watch-sized.

---

## 5b. Keep what already works
- **Directories stay SEPARATE** (the existing directory chooser / `selectedDir`). The new collapsible grouping (country / distance / SNR) applies **within** a selected directory — it does NOT merge all directories into one list.
- **Server-type glyphs** (`TYPE_LOGOS`: ubersdr / owrx / kiwi / fmdx / spyserver, + rtl-tcp) stay on each row exactly as now.

## 5c. VibeServer icon — REUSE the RTL-TCP icon (no new icon now)
VibeServer/local servers reuse the existing **RTL-TCP icon** we already made for local servers. A dedicated VibeServer icon is deferred (only if/when VibeServer becomes its own standalone app) — not needed for this work.

## 6. Existing data to build on (`SDRInstance`, instancesApi.ts)
`uuid, name, url, location, callsign, users, maxUsers, online, version, latitude, longitude, countryCode, distance (km, when location known), bestSnr, serverType, deviceType, full, sessionLimitMins`. Plus local: favourites (favourites.ts), tcpFavs, defaultInstance, discovered (mDNS).

---

## 7. Build targets & order (CORRECTED — Stuart 2026-07-17)
★ SCOPE: build the NEW picker in the **PHONE APP** and **PORT IT TO THE SPIKE (standalone watch) AT THE SAME TIME**. **DO NOT TOUCH THE COMPANION WATCH APP** (`ios/VibeSDRWatch`) yet — that's a later job.
- Favourites sync therefore = **phone ↔ SPIKE** (mechanism still OPEN — iCloud KV / in-range). Companion sync deferred.
- ★ **SYNC IS THE NORM, not a bonus — the phone app is ALWAYS installed (bundled).** In the real product (companion default) the phone side is always present, so phone↔watch favourites sync essentially always applies. The ONLY non-sync scenario: a user who **never opens the phone app at all** — then the watch runs on its OWN watch-LOCAL favourites until the phone is first opened, at which point the two **merge**.
- ★ So the watch must still be **self-sufficient**: own directory fetch, own watch-local favourites, fully usable with zero phone interaction (the never-open-the-phone user). FIRST-CLASS test case. Sync then merges when the phone is eventually opened.
- The **spike prototype** is standalone-only (no phone), so it just exercises the self-sufficient watch-local path — sync is proven later in the companion. The spike fetches its OWN directory/favourites.

Phase order (confirm before each):
Each phase is built on the PHONE and mirrored to the SPIKE together (companion untouched):
1. **Collapsible grouped directory** — the core: country grouping (no-location, ships to everyone) + distance bands (location) + SNR grouping, per-group open/closed + Expand/Collapse-all. (Biggest win, no new deps.)
2. **Favourites smart-sort** — the sort-button modes incl. combined most-visited counter. (No new deps.)
3. **Favourites manual drag-reorder** — needs a draggable-list dep.
4. **Map view** — reuse Leaflet (phone; spike map is a stretch — a watch map may not be worth it, decide later).
5. **Phone ↔ spike favourites sync** (incl. manual entries + combined most-visited) — mechanism TBD.

---

## 7a. Watch mode-selection flow (context — lands when the picker folds into the companion)
The bundled watch app chooses companion vs standalone like this:
- **Phone app OPEN** → watch **auto-connects to it** (companion), no prompt.
- **Phone app NOT open** → on launch, watch shows **TWO CHOICES**:
  - **"Control iPhone app"** — icon: watch + two arrows + iPhone → companion.
  - **"Use as standalone"** — icon: watch + two arrows + **globe outline** (indicates www) → standalone.
- **Companion chosen** → cold-boot the iPhone app; **if no default instance set → the server list (this picker) pops up.**
- **Standalone chosen** → the server list pops up **straight away.**
- **OLDER WATCH (can't do standalone DSP, pre-S9):** the two-choice screen is **NOT shown at all** — it goes straight to companion. The user never sees a standalone option they can't use (no "unavailable" message — silent graceful degradation). (Choice only appears on a standalone-capable watch when the phone app is closed.)
- From then on, **feature set and look are ALMOST IDENTICAL** across the two modes — the only real difference is the waterfall (different processing/rendering: phone-decoded vs watch-decoded). So THIS PICKER must look/behave the SAME in both modes. (This flow is companion-integration work — not built now while the companion is untouched; recorded so the picker is designed mode-agnostic.)

## 8. Open questions (decide during build)
- Distance band edges (100/500/1000 mi) — miles vs km display (user said miles).
- "Excellent SNR" threshold value.
- When BOTH location + country are available, which grouping is the default view, and can the user switch grouping (a group-by toggle: Distance / Country / SNR)?
- Favourites-sort: cycle-button vs a small menu.
