# "Nearby stations" — backend interface for the UI

The database, ranking, PI decode, and enrichment are built (`src/services/`).
This is the contract the **UI** (the "Nearby" button + station picker) calls.
Design the UI in Claude Design; wire it to these functions. The UI does **no**
data work — it renders what the facade returns and calls back on tap.

Import from `src/services/stationFinder.ts`.

## Types (from `src/services/stationTypes.ts`)

```ts
interface NearbyStation {
  callsign: string; callsignBase: string;
  frequencyMhz: number;          // e.g. 101.1
  service: 'FM' | 'FX' | 'FL' | string;  // full-power / translator / LPFM
  stationClass: string | null;   // 'C', 'A', ...
  erpKw: number | null;
  lat: number; lon: number; city: string | null; state: string | null;
  facilityId: number;
  distanceKm: number;            // already computed
  score: number;                 // receivability rank (HIGHER = better); already sorted
  genre: string | null;          // enrichment — may be null (offline / uncatalogued)
  logoUri: string | null;        // local file uri — may be null; DO NOT block on it
  homepage: string | null;
}

interface NearbyResult {
  location: { lat: number; lon: number } | null;  // null = no GPS fix yet
  radiusKm: number;
  stations: NearbyStation[];     // ranked best-first
  snapshotDate: string | null;   // "2026-06-14" or null if DB not built yet
}

interface StationIdentity {
  pi: number; callsign: string | null; confident: boolean;
  station: StationRow | null; note?: string;
}
```

## Functions

```ts
// The list. Offline-first: resolves from the bundled DB + logo cache and NEVER
// blocks on the network. Online, it background-fetches logos for the top rows
// (they appear on a later call). Default radius 100 km.
getNearbyStations(opts?: {
  radiusKm?: number; limit?: number;
  location?: { lat: number; lon: number };   // override GPS (manual city pick)
  enrich?: boolean;                            // default true
}): Promise<NearbyResult>;

// Live-tuning identity from the RDS PI (already wired into the FM face; exposed
// in case the picker wants it too). Hint only — prefer PS text when present.
identifyByPi(pi: number, psText?: string): Promise<StationIdentity>;

// On-demand enrichment for one station (e.g. when its row scrolls into view).
enrichNow(callsignBase: string, nameHint?: string): Promise<Enrichment | null>;

// LMS snapshot date for the unobtrusive "station data as of …" label.
getStationDataDate(): Promise<string | null>;
```

## Building the picker UI against this

- **Button:** a single "Nearby" button on the FM face. Big touch target (may be
  used at a stoplight).
- **On open:** `const res = await getNearbyStations();`
  - `res.location === null` → show "Waiting for GPS…" (and offer a manual city
    pick that calls `getNearbyStations({ location })`).
  - Otherwise render `res.stations` **in the order given** — it's already ranked
    by receivability. Do **not** re-sort by raw distance.
- **Row:** `frequencyMhz` + `callsign` **always**; then `city`, `genre`, and the
  `logoUri` logo **only when present** (all nullable — missing logo → show
  freq+callsign, missing genre → omit, no placeholder clutter).
- **Signal/receivability, if shown:** never a red→green gradient (user is
  red/green colourblind). Use bar count / position / the numeric `distanceKm` or
  a blue-amber-neutral ramp. `score` is a relative rank, not a dBu — label it
  loosely ("stronger/weaker") or just rely on list order.
- **Tap a row → tune it.** Call the existing tuner tune handler with
  `station.frequencyMhz * 1e6` (Hz), mode `wfm`. (Hook this to the same path the
  presets use.)
- **Long-press → save as preset** (reuse the FM face's save-preset flow).
- **Footer:** show `res.snapshotDate` unobtrusively ("FCC data as of …").

## Contract notes (don't fight the backend)

- Never block the list on a network call. `getNearbyStations()` already returns
  instantly from local data; logos fill in later — re-call it, or call
  `enrichNow()` per visible row, to pick them up.
- Distance and ranking are the backend's job; the UI just renders `score` order.
- A PI-derived callsign is a hint, not truth (translators mis-decode). The face
  already prefers PS text; mirror that if you surface identity in the picker.
- Until `tools/build_station_db` has produced a real DB, the placeholder ships 0
  rows, so `stations` is empty and `snapshotDate` is null — design an empty state
  ("Station database not installed yet").
```
