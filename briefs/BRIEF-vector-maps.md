# BRIEF — Bundled vector maps, and the end of the tile dependency

**Status:** decided and being built, 2026-09-26
**Scope:** every map in the product — HFDL, digital spots, CW, the directory, admin, and ADS-B/AIS when they land

## What started it

The HFDL map snaps to each new aircraft, so it crosses the same ground constantly. Tiles that had
already been fetched showed as **black gaps** on the way back. Stuart, 2026-09-26: *"it went from
zoomed in to a zoomed out full world view and it went black and it shouldnt have as we already had
the full world view previously."*

## ★★★ FOUR WRONG TURNS BEFORE THE RIGHT ANSWER — ALL MINE, ALL WORTH NOT REPEATING

1. **PREFETCHING WHEN THE ASK WAS CACHING.** `warmLowZooms` pulled 21 tiles from OSM on every map
   open. It could not work — a `flyTo` arcs through zooms and positions no fixed warm-up predicts —
   and prefetching a free service is precisely what its usage policy forbids. Stuart had asked for
   *"the map to buffer so that on first movement it may be black underneath but subsequent movements
   it is clean"*, which is caching. ★ **Caching what you fetched is explicitly fine and makes you a
   LIGHTER user; prefetching what you have not needed is bulk downloading.** Opposites.
2. **DESIGNING HOSTING NOBODY NEEDED.** On "we are blocked" I proposed self-hosting tiles, Protomaps,
   R2 — and let him conclude he would have to lease a server. ✗ He would not have: the directory is
   already a Cloudflare Worker. But none of it was needed anyway.
3. **INDEXEDDB FOR THE CACHE.** The map WebView is loaded with `baseUrl` set to the *instance* URL so
   the HFDL polls stay same-origin, so IndexedDB is scoped **per receiver** — three servers, three
   caches — and React Native cannot read or clear it, which broke the "Map cache · Clear" line he
   asked for. ★ I had flagged the origin scoping myself an hour earlier and built it regardless.
4. **BUILDING A 150 MB CACHE WITH A SETTINGS PANEL** for a problem that disappears entirely with
   vectors. Stopped mid-flight when he asked *"are there not any vector maps with the city names?"*

★★ Each wrong turn came from solving the problem I had in my head rather than the one in his words.
He never changed the requirement once.

## The answer: bundle vector data, drop tiles entirely

| layer | source | licence | count | size |
|---|---|---|---|---|
| Countries 10m (colourable) | Natural Earth | public domain | 238 | 6.73 MB |
| State/province lines | Natural Earth | public domain | 33,326 | 4.61 MB |
| Lakes | Natural Earth | public domain | 1,347 | 1.77 MB |
| Airports + heliports + seaplane | **OurAirports** | public domain | 72,525 | 4.05 MB |
| Towns | **GeoNames cities5000** | **CC BY 4.0 — ATTRIBUTION REQUIRED** | 69,753 | 2.12 MB |
| Sea ports (for AIS) | **NGA World Port Index** | public domain | 3,807 | 0.13 MB |
| Maidenhead grid | computed | — | — | **0** |
| | | | | **~19.4 MB** |

★ Maidenhead is arithmetic, not data — 18×18 fields of 20°×10°, subdivided. It can never be stale.

## Why this is better than tiles, beyond fixing the gaps

- **Nothing to fetch ⇒ nothing can go black**, at any zoom, during any animation.
- **No OSM dependency.** We had already been refused. Four features were about to be built on it.
- **Airports as DATA, not pixels** — filterable, restylable, and the thing an aircraft map is for.
  No tile provider gives you that. Same for ports and AIS.
- **The palette is ours.** The OSM basemap looked wrong inside a dark amber app.
- **Zero network, so it works offline** — which is what makes the pocket VibeServer viable (see
  [[pocket_vibeserver_brief]]). Stuart: *"the little VibeServer running on a Pi 0 2W running in
  captive hotspot mode with no internet, this whole idea gives that a viable mapping system and
  means someone will never have a no maps issue either."*

## ★★★ THE REAL CONSTRAINT IS DRAW TIME, NOT BYTES

At 19 MB the file size stopped mattering. The limit is **~948,000 points for Leaflet on a 1 GB
Xcover while the map flies to a new aircraft every few seconds.** Hence **three tiers** — 110m
countries and major airports at world view, 50m and medium airports regionally, 10m with admin-1
lines, lakes, GeoNames towns and every airfield only when zoomed in. Most of those points are never
on screen at once.
✗ Do not emit one blob and simplify at runtime; generate the tiers.

## Why Natural Earth alone was not enough

Natural Earth has **57 UK places and no Northampton**. Measured within 40 km of Stuart's receiver:

| source | places within 40 km |
|---|---|
| Natural Earth | **1** (Leicester, 39 km) |
| GeoNames 5000 | **49** (Brixworth 3 km, Northampton 7 km, Wellingborough, Kettering…) |

His own server would have sat in an unlabelled void. ★ So the two layers are kept SEPARATE rather
than merged: Natural Earth's ranked places are what stays readable at world view, GeoNames provides
local detail on zoom. Flattening them would either clutter the world or leave the user invisible.

Natural Earth's airports are equally thin — **17 UK airports, no Stansted, no Sywell**. OurAirports
has Sywell (EGBK / ORM) and 72,525 others.

## Distribution: bundled everywhere, hosted nowhere

Considered GitHub Pages and the directory; landed on **shipping it with every artefact**:
- **App** — bundled assets, read locally.
- **VibeServer** — installed to `lib/vibeserver/mapdata/` and served to its own web clients.
  ★★★ **FILES ON DISK, NEVER COMPILED IN.** The web page is base64 inside `vibe_web_page.h`; doing
  that with 19 MB would put ~25 MB of base64 in the executable and pull it into RAM on a Pi 2.
- **Directory** — serves its own copy, as it already does for `country-shapes.json`.
- ★ Stuart's packaging idea: a **`vibeserver-maps` deb that `vibeserver` RECOMMENDS rather than
  DEPENDS on** — default installs get maps, constrained ones can skip, and the data refreshes on its
  own cadence without cutting a server release.

Package impact: deb 20.5 → ~40 MB, main APK 56 → ~75 MB, Lite 20 → ~39 MB. All unremarkable.

## Owed before this ships

- **The Xcover**, because it is the floor for draw performance and the tiering is the only real risk.
- **Stuart's eye on the colours** — for the first time the map's appearance is a choice rather than
  whatever OSM picked.
- **GeoNames attribution** in the About page (CC BY 4.0). Natural Earth, OurAirports and NGA need
  none, but it costs nothing to credit all four.
- ▶ Later: ports are to AIS what airports are to ADS-B. Shipping *lanes* are not needed — AIS draws
  its own.

## 2026-09-26 — landcover, roads, and the two-pack split

### The map now reads as a map
Stuart: *"if you could cross reference our vector maps with a real map and then make the built up
areas grey with the rural green around it ... things like the sahara could be made sand coloured."*
Natural Earth already ships all of it, PUBLIC DOMAIN, from the mirror we were already using:

| layer | source | what it gives |
|---|---|---|
| urban areas (50m/10m) | NE | built-up grey |
| geography regions, `FEATURECLA` | NE | **Desert** (58 polys), Tundra, Wetlands |
| glaciated areas | NE | ice white |
| roads (10m) | NE | 41,099 lines at tier2, 15,236 at tier1 |

★★ **THE COLOURS ARE NUDGES OFF `MAP_LAND`, NOT ATLAS COLOURS.** The map must stay legibly dark
under an amber UI (*"i just dont want eyeball melting white"*), so the desert is a desaturated sand
a couple of steps from the green. The cue only has to say "this ground is not grass".

★★ **URBAN IS SEMI-TRANSPARENT (0.62).** NE's urban extent is a blob around a city, not its street
plan; at full opacity a solid grey lozenge reads as a HOLE in the map.

★★★ **ROADS ARE FILTERED BY `scalerank`, NEVER BY `type`.** 25,766 of the 56,600 features are typed
"Unknown" — almost all in Asia, most of them real trunk roads. A type filter would silently delete
the road network of the largest continent *while looking like a sensible rule*. Ferry routes ARE
dropped: a line across open sea reads as a coastline error.
★ Roads are budgeted as DECORATION. An airport here is information — somebody picks a receiver by
it. Nobody tunes a radio by the M1.

### ★★★ TWO PACKS: basic bundled, detail optional
Stuart: *"ship the VibeServer with basic maps and give the server owner the option of a one time
download of the more detailed level 2 maps. same with the app too."*

| pack | tiers | size | where |
|---|---|---|---|
| **basic** | tier0 + tier1 | **7.7 MB** | bundled in the app and every VibeServer, always present |
| **detail** | tier2 | 40.3 MB raw, **11.8 MB gzipped** | one tarball, one sha256, optional |

★★★ **`detail` IS AN ENHANCEMENT, NEVER A REQUIREMENT.** The renderer reads `index.json` to learn
what is installed and falls back to tier1 SILENTLY — no error, no empty layer, no black map. A map
that looks broken without an optional download is not an optional download. If index.json itself
fails, assume basic: under-use data that is present, never over-ask for data that is not.

★★ **ONE TARBALL, NOT EIGHT FILES.** Eight parallel fetches have a partial-failure state, and a
half-installed pack is a map with the coastline of one tier and the towns of another. The installer
MUST verify the sha256 before unpacking — a truncated 40 MB download over a tether is not rare, and
it unpacks into a map that is subtly wrong.

### ★★★ TWO FAULTS FROM ONE LINE OF ARITHMETIC (and how they were reported)
Stuart, within a minute of the first deploy: *"really slow to respond"* and *"smaller towns appeared
as i zoomed in then disappeared when i zoomed in more"*. **Two complaints, one bug.**
The thinning was a GLOBAL RANK CUT — `rank <= (z-2)*1.7` over the whole world's list. At z8 the
renderer switched to tier2 (GeoNames, 69,753 towns, ranks 0–8) and a cut of 10.2 passed **every town
on Earth**. It cleared the layer instantly, then tried to build 69,753 markers for a viewport showing
forty. The clear is instant; the rebuild never finishes.
★★★ **THE FIX IS SELECTION, NOT TUNING: clip to the viewport, sort the survivors by rank, draw the
best N.** That is monotonic by construction — zooming in shrinks the viewport, so fewer candidates
compete for the same N and LOWER-ranked places appear. Detail can only grow on the way in.
✗ Do not reintroduce a global cut: it cannot be right for two datasets whose rank scales differ
(NE scalerank vs a rank synthesised from population).
★ Same mistake in polygon form: tier2-countries is 546,031 points; rings are bbox-clipped too.
★ And ONE LAYER PER FETCH, not one tier per fetch — zooming to z8 was pulling 15 MB and waiting for
all of it before drawing any of it.

### ★★★ A SYNTAX ERROR IN AN INLINE SCRIPT KILLS THE WHOLE PAGE
I deployed a `const esc` and a `const CONTINENTS` that the directory already had. Either one is a
SyntaxError, and that takes out **the entire script block** — the directory sat at "Loading…", not
just the map. ★★ I had "verified" the deploy by curling the map DATA files (200 OK). That tested the
assets, not the page.
**The check that actually works, now run before every directory deploy:** extract every inline
`<script>` and run `node --check` on it.

### ✗ DO NOT SWAP THE iOS APP TO MAPKIT
Considered 2026-09-26 and rejected. It would reintroduce fetch-on-demand during the HFDL flyover
(milder than raster, not gone), split the app into two renderers, and **die completely offline** —
which kills the pocket-VibeServer case where an iPhone is the likeliest client. We would still need
the vector data for Android, web and the server, so MapKit is extra code, not less. It also cannot
take our palette, and its POIs are pixels rather than filterable airport data for ADS-B.
★ **Jr IS THE EXCEPTION** (Stuart: *"Jr doesnt need them since they already use apple's mapkit"*) —
native Swift, no flyover, MapKit already present and free. ✗ Bundle nothing into Jr.
★ MapKit licensing, as understood (VERIFY before relying): native MapKit is free and unmetered on
Apple platforms; MapKit JS is metered (~250k map loads/day) against the paid developer membership.
