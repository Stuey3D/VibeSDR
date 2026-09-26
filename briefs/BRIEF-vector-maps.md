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

## 2026-09-26 (later) — a realistic planet, and the build-heavy/runtime-light rule

Stuart: *"I would like the map to look like a realistic representation of the world just at a more
coarse detail level for performance and space saving"* and *"if you need big downloads to compose
the detail into the map that is fine, as long as our map remains compact and high performance."*

★★★ **THAT IS THE GOVERNING RULE NOW: SPEND ANY AMOUNT AT BUILD TIME, SHIP ALMOST NOTHING.** The
generator's cache is ~1.7 GB of other people's release artefacts (HydroLAKES 820 MB, Ecoregions
243 MB, ETOPO2 73 MB, Natural Earth ~50 MB). None of it reaches the repo, an artefact or a user.

### ★★★ TERRAIN AND BIOME ARE RASTERS, AND THAT IS NOT A RETREAT FROM DROPPING TILES
What we threw away was a **DEPENDENCY** — images fetched on demand from a server that blocked us.
A bundled image has none of those failure modes. And both of these are genuinely **FIELDS**: every
point on Earth has exactly one elevation and one biome. Expressing a field as polygons is what
produced the mountain blobs. Coastlines, roads and lakes are shapes and stay vectors.

| what | source | licence | how it ships |
|---|---|---|---|
| elevation + **bathymetry** | ETOPO2v2c | public domain | painted into relief.png |
| biome (14 classes, 847 ecoregions) | RESOLVE Ecoregions 2017 | **CC BY 4.0** | painted into relief.png |

★★ **ETOPO2v2c WAS CHOSEN OVER ETOPO 2022 DELIBERATELY** — it is a plain int16 grid with no
container format, so no GDAL and no netCDF library stands between anyone and a map rebuild.

★★★ **HALF THE GRID WAS BEING THROWN AWAY.** The first relief discarded every cell at or below sea
level, so the map had the Himalaya and a flat blue nothing where the Mid-Atlantic Ridge, the
trenches and the shelves are. Same file, already parsed: the ocean floor cost NOTHING. ★ And it is
not decoration on a radio map — the shelf edge is where the HF ground-wave path changes, and for
AIS the shelf IS where the shipping is.

### Three things in the relief that are correctness, not style
1. ★★★ **REPROJECTED TO WEB MERCATOR AT BUILD TIME.** `L.imageOverlay` stretches linearly in
   PROJECTED space. An equirectangular image at ±85° slides Britain hundreds of km south. Doing it
   in the generator also keeps the renderer dumb, which is where that belongs.
2. ★★★ **ALPHA FADES OUT WITHIN 120 m OF SEA LEVEL, BOTH WAYS.** NOAA's grid and Natural Earth's
   coastline disagree by a cell or two *everywhere*; a hard edge scatters green specks into the
   vector sea. Fading makes the two sources agree by construction — neither asserts anything where
   they differ.
3. ★ **The biome owns the lowlands, elevation takes over above ~1200 m**, blended not switched. A
   hard switch draws a contour line across every mountain.

### ★★★ WHAT THE BIOME RASTER RETIRED
`COVER_CLASSES` is now **empty**. It held Desert/Tundra/Wetlands — 58, 4 and 3 Natural Earth
polygons picked because they looked like the right idea. A peer-reviewed global classification
covers all land correctly, so the old ones would sit ON TOP of a better answer and contradict it.
Only **ice stays a vector**: an ice sheet has a hard edge worth keeping crisp, and it must still
draw when the relief image is hidden past z9.

### The mountain-blob lesson, kept
✗ **NEVER fill Natural Earth's `Range/mtn`.** They are envelopes drawn around a range so a LABEL
can be placed on it. One covers Belgium. Stuart: *"the mountains look a bit shit, bit like big
random blobs."* Removed from the DATA, not merely hidden — same rule as AGENTS.md's dead control:
a feature that misdescribes the world is worse than an absent one, because the user believes it.

### Sharding, and why it is not just a size workaround
tier2-lakes broke Cloudflare's 25 MiB asset limit, and the fix turned out to be the feature:
heavy layers split **recursively by density** (a fixed 6x3 grid put 89,390 lakes in one cell
covering Europe). Each shard records its own DATA bbox, not its grid cell, so an item overhanging
its cell is still found — testing against the cell clips features at every seam, which looks like
missing data and is very hard to see. Measured over Northampton: **606 lakes loaded, not 184,869.**

### ★★★ STORE POLICY — A SEPARATE MAP PACK IS FINE, AND WHY
Both stores restrict **executable code**, not data: Apple 2.5.2 (no downloading/executing code),
Play's Device and Network Abuse policy (no DEX/native from outside Play). Map data, game assets and
media are explicitly normal — TomTom and every large mobile game work this way, and both platforms
ship first-party mechanisms (Apple On-Demand Resources, Google Play Asset Delivery) we could adopt
later. ▶ Owed regardless: **state the size before downloading**, and **default to Wi-Fi only**.
★ basic (~18 MB) is nowhere near Play's 200 MB base-APK limit and the detail pack is outside the
APK entirely.

### ✗ Jr stays on MapKit
Stuart: *"Jr doesnt need them since they already use apple's mapkit."* Native Swift, no flyover
animation, MapKit already present and free. ✗ Bundle nothing into Jr. And ✗ do NOT move the iOS app
to MapKit: it would refetch during the HFDL flyover, split the app into two renderers, and **die
completely offline** — which kills the pocket-VibeServer case where an iPhone is the likeliest
client. We would still need the vectors for Android, web and the server, so MapKit is extra code,
not less.

## ✗ SUBMARINE CABLES — CHECKED, NOT AVAILABLE, DROPPED (2026-09-26)

The most on-theme layer anyone proposed for a comms app, and we cannot have it. TeleGeography's
own FAQ, read before anything was built:

> *"The raw, geocoded data underlying TeleGeography's interactive maps is available via an **annual
> license**."*

★★★ **WHAT IS CC BY-SA 4.0 IS THEIR MAPS AND SCREENSHOTS, NOT THE DATA** — the FAQ's licence
sentence covers *"any reference to a TeleGeography map, URL, or any related screen capture"*. The
GeoJSON their site serves is therefore commercial-licence-only, and bundling it in a paid app would
be taking data we have not paid for. ✗ The endpoint being publicly readable is NOT a licence.

Alternatives considered and rejected:
- **OpenStreetMap cables** (ODbL, free): coverage is partial and inconsistent. ★★ A half-complete
  cable map is WORSE than none — it reads as authoritative and quietly misinforms, the same failure
  shape as the mountain blobs and as a tour card that misdirects (AGENTS.md).
- **Ask TeleGeography** (`cablemap@telegeography.com`): offered, and still open if anyone wants it.

★ Stuart, 2026-09-26: *"drop the submarine cables its fine."* ✗ Do not re-add from the public
endpoint on the grounds that it is reachable.

## ★★★ "WE NOW OWN THE TILES" — no: THERE ARE NO TILES
Worth keeping the framing straight, because it explains what the day actually bought. There is
nothing to serve, nothing to expire, nothing to cache and nothing to go black. A tile was always
somebody else's RENDERING of the world handed over as a picture; this is the world as DATA and the
picture is ours. That is why the palette is a one-line change, why airports are filterable objects
rather than baked pixels, and why a Pi Zero with no uplink can still draw a map.

## ▶ PARKED — "airports dropdown -> nearby airband servers" (2026-09-26)

Stuart's idea, and the data for it is ALREADY BUNDLED: 1,174 large airports with ICAO/IATA, and the
directory already ranks receivers by distance for "Find a station". It is roughly a twenty-minute
job whenever it is wanted.

★★ **PARKED DELIBERATELY, AND FOR THE RIGHT REASON.** Stuart, 2026-09-26: *"there is build it and
they will come and there is build an entire city and they will come ... without servers we cannot
test it accurately either."* With 7 servers the ranking returns whatever it returns and NOBODY CAN
TELL IF IT IS GOOD. A ranking that cannot be evaluated cannot be trusted, and shipping one teaches
users to distrust the next one.

### ★★★ THE DESIGN INSIGHT THAT MUST SURVIVE THE PARKING: THREE RANGES, NOT ONE
"Nearest server to the airport" is the WRONG query, and Stuart's own station proves it — he hears
UK area control clearly from Daventry, nowhere near an airport.

| what you hear | transmitted from | useful range |
|---|---|---|
| Tower / ground / ATIS | the airfield itself | ~50–80 km (line of sight to the surface) |
| The AIRCRAFT side only | aircraft at FL350 | ~300–400 km |
| **Area control (e.g. London Control)** | **NATS remote relay sites** | wherever the relay reaches |

★★★ A plain distance sort presents all three identically, so a user picks a far receiver expecting
a tower and gets one side of every conversation. Worse, it would MISS THE BEST UK CASE ENTIRELY:
a receiver far from any airport that hears en-route ATC beautifully via a relay.
▶ Owed if built: a list of NATS relay sites (Daventry, Clee Hill, Great Dun Fell…). No open dataset
found; it would need compiling.
★ Carry over the existing honesty caveat verbatim — we know a receiver's LOCATION and HARDWARE
RANGE, never its ANTENNA, and airband is AM. [[client_infers_server_decisions]] in spirit: do not
let the UI imply knowledge the data does not contain.

## ★★★ THE PALETTE IS NOW A DECISION, NOT A DEFAULT — do not drift it
Stuart, 2026-09-26, on the finished basemap: *"the colours of that map are sublime, just the right
blend of dark and light, not face melting bright but not too dim to be barely readable."*

That is a SIGN-OFF, and it was reached by following one constraint he gave early and repeated —
*"the map doesnt have to be super dark it can have some colour i just dont want eyeball melting
white"*. Everything else derives from it, and a casual tweak to any of these breaks the balance:

- **Biomes separate by LIGHTNESS, not hue** (rainforest darkest -> desert lightest). A true-colour
  biome map is what an atlas does and is unreadable under amber.
- **The elevation ramp starts at exactly `MAP_LAND`**, so low ground is indistinguishable from the
  flat vector fill and only climbs to pale snow at genuine altitude.
- **Hillshade MODULATES (0.72–1.34), it does not replace.** Full-range shading turns every
  north-east slope black and reads as a hole in the land.
- **The sea's shading is flatter still (0.88–1.12).** Abyssal slopes are enormous; at land contrast
  a trench reads as a black gash.
- **Urban is semi-transparent (0.62)** so the ground tints through — a solid grey lozenge reads as
  a hole, not a city.
- **Capitals are the only amber on the map** that is not a landform, so they cannot be mistaken
  for terrain.

✗ Do not "improve" these individually. They were balanced against each other and against an amber
UI. If a change is wanted, change it and SHOW HIM, do not assume.

## ★★★ THE LABEL LADDER (Stuart, 2026-09-26) — the map's editorial policy
> *"continents/geographic regions so things like SAHARA DESERT, THE ANDES ... Then Countries. Then
> Large Cities and major ports and airports. Then medium cities and towns and larger regional
> airfields. then the full detail."*

| zoom | rung |
|---|---|
| 0–2 | continents, oceans |
| 3–4 | **physical regions** (SAHARA, ANDES, AMAZON BASIN) and seas |
| 4–6 | countries |
| 5–7 | large cities, capitals, major ports, large international airports |
| 7–9 | medium cities and towns, regional airfields |
| 10+ | everything: minor strips, runway designators, Maidenhead subsquares |

★★ **EACH RUNG ANSWERS A DIFFERENT QUESTION.** At z3 nobody is asking which town — they are asking
what that huge sand-coloured area IS. Skipping a rung leaves the user unable to orient at that
scale, which is what *"this zoom level is very cluttered"* and *"just needs continents"* were both
reporting: not too much ink, but ink at the WRONG RUNG.
★ It lives in `LADDER` in one place. ✗ Do not reintroduce scattered `z >= n` checks; they drift.
★ Region names cost nothing — `ne_10m_geography_regions_polys` was ALREADY downloaded for the old
cover classes, and carries SAHARA, ANDES, HIMALAYAS, GOBI DESERT, AMAZON BASIN with LABELRANKs.

### ★★★ LABELS ARE PLACED BY COLLISION, AND THE DOT FOLLOWS THE LABEL
A count cap is the wrong control — 300 is right for the world and absurd over London. Greedy
placement in RANK ORDER means the important name claims its space first, so LONDON survives and
Chalfont St Peter drops; a naive pass lets whichever town came first in the file evict the capital.
★★ And the DOT IS ONLY DRAWN IF THE LABEL WAS PLACED. Drawing dots first meant every rejected
place still left its mark, so the thinning was invisible and the world view stayed speckled. An
unlabelled dot says "something is here" and nothing else.

## ★★★ MAP PROFILES — the dividend of owning the data
Stuart, 2026-09-26: *"the best thing about these maps now is that we can tailor what is shown based
on what the map is used for. HFDL/ACARS/ADS-B then show the airports and large cities only. AIS show
the ports and large cities. Digital Spots show Cities and towns."*

★★ **A PROFILE IS A SUBTRACTION, NOT A THEME.** Palette, terrain and coastline are identical in all
of them; what changes is which FEATURES earn screen space. An aircraft map with sea ports on it is
not richer, it is NOISIER — every symbol the user must discard is a tax on the one they are looking
for. ✗ Do not add a profile that turns ON something the default lacks.

| profile | airports | ports | roads | grid | places |
|---|---|---|---|---|---|
| `directory` | ✓ | ✓ | ✓ | ✓ | all |
| `aero` (HFDL/ACARS/ADS-B) | ✓ + runways | ✗ | ✗ | ✗ | major only |
| `marine` (AIS) | ✗ | ✓ | ✗ | ✗ | major only |
| `spots` (FT8/CW) | ✗ | ✗ | ✗ | ✓ | all |

MEASURED over the Thames Estuary at z8 (not assumed):
`directory` 16 airports / 24 ports / 62 roads / 174 places · `aero` 16 / 0 / 0 / 44 ·
`marine` 0 / 24 / 0 / 44 · `spots` 0 / 0 / 0 / 174 with the grid on.

★ Selected with `?map=aero`, or `window.setMapProfile('aero')` — so a host page or the app's WebView
picks one with no rebuild. ★ `places` is a LADDER RUNG, not a boolean: an aeronautical map still
wants big cities to orient by, it just does not want Brixworth.
▶ **This is why the renderer must be EXTRACTED AND SHARED.** It currently lives only in
`directory/public/index.html`; the app's `MapOverlay.tsx` and the server's web client each have
their own copy of the old one. The profiles are for the APP's maps above all, and they cannot reach
them until the renderer is one file. That extraction is the next real job.

## ★ THE INSTRUMENT TRAP, TWICE IN ONE DAY
`pgrep -f gen-map-data` matches the **zsh wrapper whose command text contains the string**, not just
node. I reported the generator "still running" for 31 minutes off a stale wrapper, having flagged
exactly this failure mode earlier the same morning and then reused the command.
★★ The working check is `ps -Ao args | grep -q "^node scripts/gen-map-data"`.
▶ Same family: [[test_that_cannot_fail]], and the `iqDrops` wrong-instrument on the Pi 2.

## ★★★ AN UNASSERTED `replace()` IS A SILENT NO-OP — three deploys were wrong because of it
Editing this file with Python `str.replace()` and no assertion leaves **no trace at all** when the
anchor has drifted. `MAP_REEF` and `MAP_CAPITAL` were referenced for an hour before they were
defined; the capital one only threw when London first entered a viewport.
★★ And `cmd-that-failed && npx wrangler deploy` STILL DEPLOYED three times, because the `&&` was
chained to a later check rather than to the edit. **The fix is structural: the edit, the checks and
the deploy all live in ONE script, and the deploy is a `subprocess.run` that is only reached if
every anchor matched and every check passed.** ✗ Never deploy from a separate shell command.
★ The checks that now gate every directory deploy: every inline `<script>` through `node --check`,
and every `MAP_*` used must be declared.

## ▶ PARKED — TERRAIN AS A RECEIVER ATTRIBUTE (Stuart's idea, 2026-09-26)
> *"topographic may be a good way to identify receivers in high ground, the higher the antenna the
> better usually so someone up a mountain may end up with AMAZING signal"*

★★★ This turns the relief from DECORATION into INFORMATION, and it is the first thing we could
honestly add to the station ranking's standing caveat (*"we know a receiver's location and its
hardware range, not its antenna"*) — because terrain height is a property of WHERE THE RECEIVER IS,
which we do know, unlike the aerial, which we do not. [[client_infers_server_decisions]] in spirit.

★★ **TWO REASONS NOT TO BUILD IT ON THE CURRENT DEM:**
1. **ETOPO2 averages over 3.7 km cells.** A receiver on a 200 m hill in lowland reads the AREA
   average — so it would understate exactly the advantage being surfaced. Fine for "800 m, in the
   Alps"; useless for distinguishing a hilltop from the valley floor 2 km away, which is the
   distinction that matters in Britain.
2. **Elevation alone is the wrong metric.** VHF range follows height above SURROUNDING TERRAIN and
   a clear horizon, not height above sea level: 50 m on a coastal cliff beats 400 m in a bowl. The
   real figure is something like "height above the median terrain within 10 km", which needs a
   finer DEM to mean anything.

▶ So this CHANGES THE CASE FOR A FINER DEM (GEBCO 15 arc-sec ≈ 460 m, or SRTM 90 m). It stops being
"prettier mountains" and becomes a receiver attribute and a possible ranking signal.
★ Parked with [[the airband search]] for the same reason: a ranking nobody can evaluate on seven
servers is a ranking that teaches users to distrust the next one.

## ★★★ RELIEF IS TILED, for the same reason the lakes are sharded
Stuart, at z9 over Tenerife: *"that blur bothers me its weird how the next zoom level is clearer but
the mountain is missing."* Both halves were real:
- A single global image is only as sharp as its total size allows. At 4096 across the planet one
  pixel is 9.8 km, so at z9 each pixel smears over ~125 screen pixels — and **Mount Teide is one
  ETOPO cell** (a 3 km cone at 2 arc-min), so it can only ever be a blob.
- Then at z10 the relief switched OFF entirely and the land went flat, so the blurriest view was
  the only one carrying the mountain.

★★ **TILING BREAKS THE TRADE.** The grid renders at 8192 across the world — near ETOPO's own
10,800 — split 8×8, and a viewer at z10 fetches ONE tile rather than the planet. Same principle as
the vector shards: *do not make somebody download the world to look at their own island.*
★★★ ✗ AND THIS IS NOT THE TILE DEPENDENCY WE REMOVED. These are OUR tiles, generated at build time
and BUNDLED; nothing is ever fetched from anyone, and nobody can block them. The thing we deleted
was a runtime dependency on someone else's servers, not the idea of a grid.
★ Built once at full size and then SLICED, rather than rendering each tile's window separately —
re-deriving the hillshade per tile would show as a visible seam at every edge.
