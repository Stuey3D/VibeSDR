/* ══════════════════════════════════════════════════════════════════════════════════════════════
 * VIBEMAP — the vector basemap renderer, in ONE place.
 *
 * ★★★ THIS FILE EXISTS BECAUSE THE RENDERER WAS BUILT INSIDE directory/public/index.html AND THE
 *     APP HAD A CRUDER COPY. Three readers want this map — the directory page, the web client
 *     compiled into the Linux server, and the app's WebView — and a basemap maintained three times
 *     is a basemap that is right once. Every improvement below (relief tiles, the label ladder, the
 *     collision placer, world wrapping, the profiles) was paid for on the directory page; none of
 *     it reached the other two. ✗ Do not copy this code into a host page again. Load it.
 *
 * ★★ PLAIN JAVASCRIPT ON PURPOSE. No modules, no TypeScript, no build step: it is loaded by a
 *    <script> tag on the directory, embedded as a string into the server's compiled web page, and
 *    injected into a React Native WebView. Anything that needs a bundler would need three of them.
 *
 * USE:
 *     const vm = VibeMap.attach(map, { dataBase: '/mapdata/v1/', profile: 'directory',
 *                                     reserve: serverBoxes });
 *     vm.setProfile('aero');   vm.redraw();   vm.detach();
 *
 *   `map`     — an existing Leaflet map. VibeMap does NOT create it: the host owns the gestures
 *               (the directory trades the wheel and vertical touch back to the page) and a
 *               renderer that created the map would own decisions that are not its own.
 *   dataBase  — where /mapdata/v1/ lives for this host. Trailing slash required.
 *   profile   — 'directory' | 'aero' | 'marine' | 'spots' (see PROFILES).
 *   reserve   — () => [{x,y,w,h}] in CONTAINER pixels: boxes no basemap label may cover. This is
 *               how the directory keeps its receiver glyphs clear; see serverBoxes() there.
 *
 * ★★★ THE CSS TRAVELS WITH THE CODE (VibeMap.CSS, injected once by attach). The label classes and
 *     the code that emits them are one decision, and when they lived in two files the host page
 *     owned half a renderer. ★ A host may still override any of it — the sheet is injected at
 *     attach time, so a rule the host declares later wins.
 *
 * ★★ EMBEDDING NOTE FOR THE APP: this file CONTAINS BACKTICKS, so it must be inserted as a JS
 *    STRING VALUE (scripts/gen-vibemap-source.mjs writes it with JSON.stringify) and interpolated
 *    at RUNTIME — `${VIBEMAP_JS}` inside MapOverlay's page template is fine because substitution is
 *    runtime, not lexical. ✗ Never paste this source INTO a template literal: a backtick anywhere
 *    inside one, a comment included, closes the string and the error points at the wrong line.
 * ══════════════════════════════════════════════════════════════════════════════════════════════ */
(function (global) {
  'use strict';

  const L = global.L;

  /* ══ PALETTE ═══════════════════════════════════════════════════════════════════════════════════
   * ★★★ NO TILES. The basemap is drawn from bundled vector data -- so the dark map costs one
   *  polygon pass and nothing on the wire. What it replaces: OpenStreetMap raster tiles run through
   *  a CSS invert+hue-rotate to fake a dark theme, because CARTO's proper dark tiles would be
   *  commercial use (VibeSDR sells on the App Store) and OSM's own are light. CARTO now requires a
   *  key and stamps unauthenticated tiles "API KEY REQUIRED" across the whole map -- still HTTP 200,
   *  still a valid PNG, so nothing errored and no deploy was involved (2026-08-26).
   *  ★★ AND WE HAD ALREADY BEEN REFUSED TILES (2026-09-26): Stuart's PC showed the blocked-tile
   *  image while his Mac drew from cache. An inverted basemap we do not control was a dependency on
   *  a free service, for a picture we were fighting anyway.
   *  ★★★ THE TERMINATOR IS THE REAL WIN. Over raster tiles the day/night shadow is grey smeared
   *  across somebody else's LIGHT basemap. Here land and sea are OUR fills, so night genuinely
   *  darkens the countries underneath it -- which is what a greyline is for.
   *  ★ Palette chosen by measuring perceptual lightness, not by eye: sea L* 18.9, land L* 35.3,
   *    dL* 16.4. Two dark colours that differ only in HUE are indistinguishable on a cheap screen
   *    and to a colour-blind reader -- the land has to be LIGHTER, not just greener. (White is
   *    L* 100, for scale: Stuart, 2026-09-26, "it can have some colour i just dont want eyeball
   *    melting white".) */
  /* ★★★ LANDCOVER PALETTE. Stuart, 2026-09-26: "make the built up areas grey with the rural green
   *  around it ... things like the sahara could be made sand coloured ... fake the experience of a
   *  real map that little bit more."
   *  ★★ EVERY ONE OF THESE IS A NUDGE FROM MAP_LAND, NOT A NEW COLOUR. The map has to stay legibly
   *  dark under an amber UI -- he has said twice that it must not melt eyeballs -- so a desert is a
   *  DESATURATED sand a couple of steps off the green, not the ochre a paper atlas would use. The
   *  cue only has to be strong enough to say "this ground is different", and at these lightnesses a
   *  small step is plenty. ✗ Do not brighten these to match a printed map. */
  const MAP_SHELF = '#19406094', MAP_LAKE = '#1b4a6b', MAP_RIVER = '#3f7ba0',
        MAP_ADMIN = '#5a7a5f';
  const MAP_URBAN = '#5a5d62', MAP_DESERT = '#6d6142', MAP_TUNDRA = '#4a5750',
        MAP_WETLAND = '#33523f', MAP_ICE = '#8d9aa2', MAP_ROAD = '#6a6a4e',
        /* ★ Mountain ranges: a pale grey-stone, DIMMER than the ice. An alpine range and a glacier
         *  next to each other must still be distinguishable -- the Alps carry both. Held here for
         *  the day a DEM is contoured; see drawCover's ✗ note on Natural Earth's mtn envelopes. */
        MAP_ALPINE = '#6f7581';
  /* ★ Reef, salt flat and capital-city colours. These were REFERENCED for an hour before they were
   *  DEFINED: two `replace()` edits silently matched nothing and I did not assert on them, so the
   *  code compiled and only threw when a capital first entered the viewport. ✗ Never edit this file
   *  with an unasserted replace -- a no-op edit leaves no trace at all. */
  const MAP_REEF = '#4e8f93', MAP_PLAYA = '#8a8470', MAP_CAPITAL = '#f0c56a';
  const MAP_SEA = '#123049', MAP_LAND = '#3c5a3f', MAP_COAST = '#6fa37b';
  const MAP_TOWN = '#9ec9a8', MAP_LABEL = 'rgba(210,235,215,0.82)';   // MAP_LABEL: see .mapLbl below

  /* ══ THE LABEL STYLESHEET ══════════════════════════════════════════════════════════════════════
   * ★ Place labels: small, dim, and non-interactive -- the map is CONTEXT, the receivers are the
   *   subject. A label that competes with a server pin is a label in the way.
   * ★ Country names sit ABOVE the town labels in weight and BELOW them in brightness -- larger so
   *   they read as the bigger thing, dimmer so they stay behind the receivers. */
  const CSS = `
  .reliefImg { image-rendering: auto; }
  .mapLbl.major { font-size: 11px; font-weight: 700; opacity: 0.95; letter-spacing: 0.3px; }
  .mapLbl.admin1 { opacity: 0.85; }
  /* ★ A capital is amber against every other label's pale grey -- the one colour on the map that
   *  is not a landform, so it cannot be mistaken for terrain. */
  .mapLbl.capital { font-size: 12px; font-weight: 700; color: ${MAP_CAPITAL} !important;
                    opacity: 1; letter-spacing: 0.6px; }
  /* ★ Centred over the place, with a heavy shadow so it stays readable across terrain. */
  .mapLbl.shout { transform: translate(-50%, -140%); letter-spacing: 2px; font-weight: 700;
                  text-shadow: 0 0 5px #000, 0 0 9px #000, 0 1px 2px #000; }
  /* ★ Runway designators sit ON the threshold, centred, in the same pale tone as the tarmac. */
  .rwLbl { font: 9px ui-monospace, Menlo, monospace !important; color: #e8e2d4; opacity: 0.85;
           font-weight: 700; letter-spacing: 0.5px; white-space: nowrap;
           text-shadow: 0 0 3px #000, 0 0 4px #000; transform: translate(-50%, -50%); }
  .gridLbl { font: 9px ui-monospace, Menlo, monospace !important; color: #c98f2e; opacity: 0.4;
             letter-spacing: 1px; white-space: nowrap; transform: translate(-50%, -50%); }
  .contLbl { font-size: 13px !important; letter-spacing: 3px; opacity: 0.5; }
  .rgnLbl { font: 400 11px ui-monospace, Menlo, monospace; color: #cbb98f; opacity: 0.6;
            letter-spacing: 2.2px; white-space: nowrap; transform: translate(-50%, -50%);
            text-align: center; line-height: 1.3;
            text-shadow: 0 0 4px #000, 0 0 7px #000; pointer-events: none; }
  .rgnLbl.sea { color: #8fb6cb; font-style: italic; opacity: 0.42; letter-spacing: 2px; }
  /* ★ Airports amber, heliports and ports dimmer -- the airfield is what an ATC listener is
   *  scanning for, so it wins the eye; a port is context until AIS makes it the subject. */
  .apLbl { font: 9px ui-monospace, Menlo, monospace !important; color: #e8b13a; opacity: 0.8;
           white-space: nowrap; text-shadow: 0 0 3px #000, 0 0 3px #000; margin: -4px 0 0 4px; }
  .apLbl .g { font-style: normal; opacity: 0.75; margin-right: 2px; }
  /* ★ Size AND weight climb together: at 9 px a bold weight alone is nearly invisible. */
  .apLbl.big { font-size: 12px !important; font-weight: 700; opacity: 1; letter-spacing: 0.5px; }
  .apLbl.med { font-size: 10px !important; opacity: 0.88; }
  .apLbl.small { opacity: 0.62; }
  .apLbl.heli { color: #9fb3c8; opacity: 0.55; }
  .ptLbl .g { font-style: normal; opacity: 0.8; margin-right: 2px; }
  .ptLbl { font: 9px ui-monospace, Menlo, monospace !important; color: #6fc3d6; opacity: 0.62;
           white-space: nowrap; text-shadow: 0 0 3px #000, 0 0 3px #000; margin: -4px 0 0 4px; }
  .ctyLbl i { display: block; font-style: normal; text-transform: none; letter-spacing: 0.4px;
              font-size: 9px; opacity: 0.66; }
  .ctyLbl { font: 10px ui-monospace, "SF Mono", Menlo, Consolas, monospace !important;
            letter-spacing: 1.4px; text-transform: uppercase; color: rgba(190,225,200,0.55);
            white-space: nowrap; transform: translate(-50%, -50%); text-shadow: 0 1px 3px rgba(0,0,0,0.95);
            pointer-events: none; }
  .mapLbl { font: 9px ui-monospace, "SF Mono", Menlo, Consolas, monospace !important;
            color: ${MAP_LABEL}; white-space: nowrap; text-shadow: 0 1px 2px rgba(0,0,0,0.9);
            pointer-events: none; }
`;

  /* ★★★ LAYER ORDER IS DONE WITH PANES, NEVER WITH bringToFront(). `bringToFront`/`bringToBack`
   *  live on Leaflet's FeatureGroup, NOT on LayerGroup -- calling them on an `L.layerGroup()` throws
   *  a TypeError. That is what killed the landcover: drawBase() added the country polygons (which is
   *  why the map still LOOKED fine), then threw on the re-stacking loop, and redrawMap's .catch
   *  swallowed it. Stuart saw "not seeing any changes" and there was nothing in the console to find
   *  because the error never reached it.
   *  ★★ A pane fixes the class of bug, not the instance: the z-order is DECLARED ONCE here and no
   *  redraw has to re-assert it, so a layer can never end up behind the ground it sits on.
   *  ★ Canvas renderers per pane too -- these layers are thousands of polygons, and SVG makes one
   *  DOM node each. */
  const PANES = { shelf: 350, land: 360, relief: 365, cover: 370, urban: 375,
                  water: 380, admin: 385, road: 390, place: 395 };

  /* ★★★ MAP PROFILES — what the map is FOR decides what it draws. Stuart, 2026-09-26: "the best
   *  thing about these maps now is that we can tailor what is shown based on what the map is used
   *  for." That is the dividend of owning the data: a raster tile is somebody else's rendering of
   *  EVERYTHING, baked in, take it or leave it. A vector map can answer ONE question well.
   *
   *  ★★ EACH PROFILE IS A SUBTRACTION, NOT A THEME. Palette, terrain and coastline are identical
   *  everywhere; what changes is which FEATURES earn screen space. An aircraft map with sea ports on
   *  it is not richer, it is noisier -- every symbol the user must discard is a tax on the one they
   *  are actually looking for.
   *  ★ `places` is a LADDER RUNG, not a boolean: an aeronautical map still wants big cities to
   *  orient by, it just does not want Brixworth.
   *  ✗ Do not add a profile that turns ON something the default lacks. A profile only narrows.
   */
  const PROFILES = {
    // The directory: somebody is hunting for a RECEIVER, so the map is orientation, broadly drawn.
    directory: { airports: true, runways: true, ports: true, rail: true,
                 places: 'all', grid: true, roads: true },
    // HFDL / ACARS / ADS-B: aircraft. Airfields and runways matter; a sea port never does.
    aero: { airports: true, runways: true, ports: false, rail: false,
            places: 'major', grid: false, roads: false },
    // AIS: ships. Ports matter, and a reef is a navigational fact; an airfield is noise.
    marine: { airports: false, runways: false, ports: true, rail: false,
              places: 'major', grid: false, roads: false },
    // FT8 / CW / digital spots: the question is WHERE someone is -- towns and the locator grid.
    spots: { airports: false, runways: false, ports: false, rail: false,
             places: 'all', grid: true, roads: false },
  };

  /* ★★★ THE LABEL LADDER, STATED ONCE. Stuart set it out on 2026-09-26 and it is the map's whole
   *  editorial policy, so it belongs in ONE place rather than scattered across a dozen `z >= n`
   *  checks that drift apart:
   *
   *    z0-2   continents, oceans
   *    z3-4   physical regions (SAHARA, THE ANDES, AMAZON BASIN) and seas -- these span countries
   *           and are the reason a zoomed-out map of coloured ground makes sense at all
   *    z4-6   countries
   *    z5-7   large cities, capitals, major ports, large international airports
   *    z7-9   medium cities and towns, regional airfields
   *    z10+   everything: lanes, strips, subsquares, runway designators
   *
   *  ★★ Each rung ANSWERS a different question. At z3 nobody is asking which town; they are asking
   *  what that huge sand-coloured area IS. A map that skips a rung leaves the user without a way to
   *  orient at that scale, which is what "very cluttered" and "needs continents" were both about. */
  const LADDER = {
    continents: [0, 3],
    regions: [3, 6],
    seas: [2, 6],
    countries: [4, 7],
    /* ★★★ CITIES START AT z5. They were appearing at z3 and Stuart called it: "the cities are still
     *  showing in zoomed out view." The ladder puts large cities on the FOURTH rung, and a rung that
     *  leaks upward defeats the whole point of having one — at z3 the map should answer "what is
     *  that region", and a scatter of capitals over it answers a question nobody asked.
     *  ★ No upper bound: towns stay all the way in, they just get denser. */
    cities: [5, 22],
  };
  const rung = (k, z) => z >= LADDER[k][0] && z <= LADDER[k][1];

  /* ★★★ AT WORLD VIEW THE COUNTRY NAMES COLLIDE, so the world view does not get country names.
   *  Stuart, 2026-09-26, on the z2 map: "Little bit too much overlap, maybe have continents at this
   *  zoom level". THE LABELS ARE HARD-CODED and that is deliberate -- seven labels whose positions
   *  are a cartographic judgement, not data; no source file could place them better and none can go
   *  stale. Land centroids would put "Asia" in western China and "Africa" in the Sahara: fine, but
   *  these are nudged to where the eye expects them over the landmass. */
  const CONTINENT_LABELS = [
    ['NORTH AMERICA',  -100,  46], ['SOUTH AMERICA', -60, -15], ['EUROPE',  18, 53],
    ['AFRICA',           20,   3], ['ASIA',           90,  45], ['OCEANIA', 140, -25],
    ['ANTARCTICA',        0, -78],
  ];

  /* ★★★ THE TIER IS CHOSEN PER LAYER, NOT PER ZOOM, because the layers are not the same weight.
   *  Holding EVERYTHING at tier1 until z10 was a blunt rule that existed to avoid one file:
   *  tier2-countries is 8.8 MB and 546,031 points. But tier2's roads, rivers, lakes, urban extents
   *  and admin-1 lines are what actually make a local view look local, and at z8 the coarse versions
   *  of those are a visible detail cliff -- 11 road segments over the whole of England.
   *  ★★ So the fine COASTLINE waits for z10 (at z8 a 50m outline is still smooth on screen and the
   *  8.8 MB buys nothing you can see), while everything else steps up at z8 where it is needed.
   *  Northampton needs tier2's GeoNames towns at z8, and the towns arrive the moment they are needed.
   *  ✗ Do not collapse this back into one rule: it is the difference between a 0.3 MB step and a
   *  9 MB one, and they do not deserve the same threshold. */
  const TIER_STEP = { countries: 10, cover: 8, urban: 8, roads: 8, rivers: 7, lakes: 7,
                      admin1: 8, shelf: 8, places: 8, airports: 8, islands: 7,
                      reefs: 7, playas: 7 };
  function tierFor(what, z) {
    if (z <= 4) return 'tier0';
    return z >= (TIER_STEP[what] ?? 8) ? 'tier2' : 'tier1';
  }

  /* ★★★ EVERY POINT LAYER IS CLIPPED TO THE VIEWPORT AND CAPPED, and that is not an optimisation --
   *  it is the fix for two faults Stuart hit within a minute of the first deploy (2026-09-26):
   *  "really slow to respond" and "smaller towns appeared as i zoomed in then disappeared when i
   *  zoomed in more".
   *
   *  ★★ BOTH WERE ONE BUG. The old thinning was a GLOBAL RANK CUT: `rank <= (z - 2) * 1.7` over the
   *  whole world's list. At z8 the renderer switched to tier2 -- GeoNames, 69,753 towns -- and that
   *  cut passes EVERY ONE of them, because GeoNames ranks run 0-8 and the cut was 10.2. So it cleared
   *  the layer and then tried to build 69,753 Leaflet markers for a viewport showing about forty
   *  towns. The clear is instant and the rebuild never finishes: the towns VANISH and the page hangs.
   *  The user sees "disappeared" and "slow" as two complaints; they are one line of arithmetic.
   *
   *  ★★★ SO THE SELECTION IS NOW: take what is IN VIEW, sort by rank, draw the best N. This is
   *  monotonic by construction -- zooming in shrinks the viewport, so fewer candidates compete for
   *  the same N and LOWER-ranked places appear. Detail can only ever GROW on the way in, which is
   *  what a map is expected to do and what the global cut could not promise across two datasets with
   *  incompatible rank scales (Natural Earth scalerank vs a rank synthesised from population).
   *  ✗ Do not reintroduce a global cut. It cannot be right for both datasets at once. */
  // ★ Generous now: the COLLISION PLACER decides what actually appears, not this number.
  const CAP = { places: 700, airports: 200, ports: 80 };

  const COVER_COLOUR = { desert: MAP_DESERT, tundra: MAP_TUNDRA, wetland: MAP_WETLAND,
                         ice: MAP_ICE };

  /* ★★★ AN AIRFIELD WITH NO REAL CODE GETS NO LABEL. OurAirports assigns a SYNTHETIC ident to
   *  strips that have neither ICAO nor IATA -- "GB-1137", "US-0892" -- and drawing those put
   *  meaningless tags all over Northamptonshire. The whole reason the code is the label is that it
   *  is what you TUNE FOR; "GB-1137" cannot be tuned for, is not spoken on the air, and is not
   *  searchable. A label that carries no information is worse than no label, because the reader
   *  spends attention deciding it is useless.
   *  ★ The test is the synthetic form itself (two letters, a dash, digits), not the absence of IATA
   *  -- Sywell has no IATA and EGBK is exactly what an airband listener wants. */
  const SYNTHETIC_IDENT = /^[A-Z]{2}-\d+$/;
  const hasCode = (r) => Boolean(r[4]) || (Boolean(r[0]) && !SYNTHETIC_IDENT.test(r[0]));

  /* ★ The renderer's own escaper. It does NOT borrow the host page's: a label is built from data we
   *  did not write (GeoNames town names carry quotes and ampersands), and a renderer that depends on
   *  a helper defined somewhere in its host is a renderer that breaks on the second host. */
  const esc = (s) => String(s ?? '').replace(/[&<>"']/g,
    (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

  const inBox = ([w, s, e, n], lon, lat) => lat >= s && lat <= n
    && (w <= e ? (lon >= w && lon <= e) : (lon >= w || lon <= e));   // the antimeridian case

  /** Pick the best `cap` rows in view, cheapest first: clip, then sort only the survivors. */
  function pick(rows, box, cap, rankOf, keep) {
    const hits = [];
    for (const r of rows) {
      if (keep && !keep(r)) continue;
      if (!inBox(box, r[1], r[2])) continue;
      hits.push(r);
    }
    if (hits.length > cap) {
      hits.sort((a, b) => rankOf(a) - rankOf(b));
      hits.length = cap;
    }
    return hits;
  }

  /* ★★ THE FINE OUTLINES ARE CLIPPED TOO. tier2-countries is 9 MB and ~1.5 M points; handing the
   *  whole world to Leaflet at z8 is the same mistake in polygon form. A ring is drawn only if its
   *  own bounding box meets the view. */
  function ringInView(ring, box) {
    let w = 180, s = 90, e = -180, n = -90;
    for (const [lon, lat] of ring) {
      if (lon < w) w = lon; if (lon > e) e = lon;
      if (lat < s) s = lat; if (lat > n) n = lat;
    }
    return !(e < box[0] || w > box[2] || n < box[1] || s > box[3]);
  }

  /* ★★★ THE MAIDENHEAD GRID IS COMPUTED, NOT DOWNLOADED. Stuart asked whether it needed licensing
   *  (2026-09-26); it does not, because it is not data. The locator system is 18x18 fields of
   *  20 deg x 10 deg, each divided into 10x10 squares of 2 deg x 1 deg, and that definition is the
   *  whole dataset. It cannot go stale, it cannot be wrong, and it weighs nothing.
   *  ★★ SIX CHARACTERS, NOT FOUR. The locator has THREE pairs and the third is the one people
   *  actually quote: Stuart's own square is IO92NH, and a grid that stops at IO92 does not resolve
   *  to the thing he reads off his own receiver. Field 20 x 10 deg, square 2 x 1 deg, subsquare
   *  1/24th of the square = 5 x 2.5 arcmin. ★ The subsquare pair is conventionally LOWER case. */
  function maidenhead(lon, lat) {
    const x = lon + 180;
    const y = lat + 90;
    const f1 = Math.floor(x / 20);
    const f2 = Math.floor(y / 10);
    const s1 = Math.floor((x % 20) / 2);
    const s2 = Math.floor(y % 10);
    const u1 = Math.floor(((x % 2) / 2) * 24);
    const u2 = Math.floor((y % 1) * 24);
    return String.fromCharCode(65 + f1) + String.fromCharCode(65 + f2) + s1 + s2
         + String.fromCharCode(97 + u1) + String.fromCharCode(97 + u2);
  }

  /* ★ Injected once per document, not once per map: two maps on one page would otherwise stack
   *  identical sheets. An unasserted no-op is the bug class this file's palette comment warns
   *  about, so the guard is an explicit id rather than "probably already there". */
  function injectCss(doc) {
    if (doc.getElementById('vibemapCss')) return;
    const st = doc.createElement('style');
    st.id = 'vibemapCss';
    st.textContent = CSS;
    doc.head.appendChild(st);
  }

  /* ══ ATTACH ════════════════════════════════════════════════════════════════════════════════════
   * Everything below is PER MAP. The constants above are shared and read-only; every layer group,
   * cache and flag is created here, because two maps on one page (a picker and a detail view) must
   * not share a manifest promise, a tier cache or a `drawing` latch. */
  function attach(map, opts) {
    const o = opts || {};
    const MD = o.dataBase || '/mapdata/v1/';
    /* ★ The host's reserved boxes. Default is "nothing reserved", which is correct for a map with no
     *  markers of its own -- not an error. ✗ Do not reach for a host global here: that is exactly
     *  what made this renderer un-shareable (`markers` was a directory-page const). */
    const reserve = typeof o.reserve === 'function' ? o.reserve : () => [];
    injectCss(map.getContainer().ownerDocument || global.document);

    let P = PROFILES[o.profile] || PROFILES.directory;

    const paneRenderer = {};
    for (const [name, z] of Object.entries(PANES)) {
      map.createPane(name);
      map.getPane(name).style.zIndex = String(z);
      map.getPane(name).style.pointerEvents = 'none';
      paneRenderer[name] = L.canvas({ pane: name });
    }
    const inPane = (name, extra = {}) =>
      ({ pane: name, renderer: paneRenderer[name], interactive: false, ...extra });
    /* ★★★ THE RECEIVERS SIT ABOVE EVERYTHING. Stuart, 2026-09-26, seeing "London" cross a server
     *  glyph: "our radio icon does need to be above the label though since the servers are what
     *  people are after here." That is a statement of PRIORITY, not of style -- this page exists to
     *  find receivers, and the basemap is context for them. A map label may never obscure one.
     *  ★ Leaflet's default markerPane is 600 and every basemap label lives there too, so order came
     *  down to DOM insertion -- i.e. to luck. Its own pane at 620 settles it permanently.
     *  ★★ CREATED HERE, BY THE RENDERER, even though the HOST fills it: the pane is half of the
     *  promise that a label never covers a receiver (the other half is `reserve`), and a host that
     *  had to remember to create it would be a host that forgets. */
    map.createPane('servers');
    map.getPane('servers').style.zIndex = '620';

    // ★ The sea is a FILL now, not a tile, so the container's own background IS the ocean.
    map.getContainer().style.background = MAP_SEA;

    /* ★ Painted in this order and never another: cover (what the ground is) -> urban (what was built
     *  on it) -> roads (what joins it). Each one is a correction to the one below, so a town in a
     *  desert must be able to cover the sand, and a road must sit on top of both. */
    const basemap = L.layerGroup().addTo(map);
    const placeLayer = L.layerGroup().addTo(map);
    const countryLabelLayer = L.layerGroup().addTo(map);
    const regionLabelLayer = L.layerGroup().addTo(map);
    const gridLayer = L.layerGroup().addTo(map);
    const coverLayer = L.layerGroup().addTo(map);
    const urbanLayer = L.layerGroup().addTo(map);
    const roadLayer = L.layerGroup().addTo(map);
    /* ★★ THE SHELF SITS UNDER THE LAND, NOT OVER IT. ne_10m_bathymetry_K_200 is the 200 m contour --
     *  the continental shelf -- and drawing it above the countries would flood every coast. Its pane
     *  (350, below land's 360) is what keeps it there. ★ For an HF listener the shelf edge is a real
     *  landmark, not decoration. */
    const shelfLayer = L.layerGroup().addTo(map);
    const waterLayer = L.layerGroup().addTo(map);      // lakes + rivers
    const adminLayer = L.layerGroup().addTo(map);      // state/province lines
    /* ★★★ MINOR ISLANDS, REEFS AND SALT FLATS.
     *  ★★ The islands are a CORRECTNESS fix, not decoration: Natural Earth's country polygons drop
     *  the small ones, and on a DX map those are exactly the wrong ones to lose -- Ascension,
     *  Tristan da Cunha, St Helena and Rockall are what a listener is hunting for.
     *  ★ Reefs draw as LINES rather than fills: the Great Barrier Reef is a hazard boundary, and a
     *  solid fill would read as shallow land. */
    const islandLayer = L.layerGroup().addTo(map);
    /* ★ Equator, tropics and polar circles -- and on a radio map these are not trivia: the
     *  greyline's behaviour changes at exactly those latitudes, which is why they are drawn. */
    const geoLineLayer = L.layerGroup().addTo(map);
    const runwayLayer = L.layerGroup().addTo(map);
    const airportLayer = L.layerGroup().addTo(map);
    const portLayer = L.layerGroup().addTo(map);
    const allLayers = [basemap, placeLayer, countryLabelLayer, regionLabelLayer, gridLayer,
                       coverLayer, urbanLayer, roadLayer, shelfLayer, waterLayer, adminLayer,
                       islandLayer, geoLineLayer, runwayLayer, airportLayer, portLayer];

    /* ══ DATA ════════════════════════════════════════════════════════════════════════════════════
     * ★★★ TIERED, BECAUSE 110m LOOKS LIKE A CHILD'S DRAWING. The old country-shapes.json was
     *  Natural Earth 110m -- 175 countries, 10,554 points, ~1 km precision -- fine as a lookup table
     *  and crude as a coastline (Stuart, 2026-09-26: "it looks too course and like a childs
     *  drawing"). tier1 is the 50m set: 237 countries, 99,354 points, NINE TIMES the detail, and it
     *  brings the cities the old file never had.
     *  ★★ Fetched by ZOOM, not all at once: tier0 (254 kB) draws the world instantly, tier1 (2 MB)
     *  arrives when somebody zooms in. Cached by the browser, so it is one download per version.
     *  ★★ THREE TIERS, and the third one is what puts Northampton on the map. tier1 is Natural
     *  Earth's ranked places -- Coventry and Cambridge make that list, Northampton does NOT (NE
     *  carries 57 UK places and his own town is not among them). tier2 is GeoNames cities5000:
     *  69,753 towns, and Sywell (EGBK) with it. It is 2 MB, so it is fetched ONLY on a deep zoom. */
    const mapTier = { loaded: {}, at: -1 };
    let mapIndex = null;
    let countryLabels = null;

    /* ★★★ THE INDEX SAYS WHAT IS INSTALLED, AND THE RENDERER ASKS RATHER THAN ASSUMES. The detail
     *  pack (tier2, 11.8 MB compressed) is an OPTIONAL one-time download on a VibeServer or in the
     *  app; the basic pack is bundled and always there. A server without the detail pack must draw a
     *  correct, complete map -- just a coarser one. ✗ It must never 404 its way to a black hole at
     *  z8, which is exactly what asking for tier2 unconditionally would do.
     *  ★ The probe is ONE fetch of index.json, and if even that fails we assume the basic pack, which
     *  is the safe direction: we may under-use data that is present, never over-ask for data that is
     *  not. */
    function mapManifest() {
      if (!mapIndex) {
        /* ★★ `cache: 'no-cache'` revalidates. A browser holding an index.json from an earlier deploy
         *  would report layers we now ship as ABSENT, and the renderer would dutifully skip them --
         *  a stale manifest is indistinguishable from an uninstalled pack. */
        mapIndex = fetch(MD + 'index.json', { cache: 'no-cache' })
          .then((r) => (r.ok ? r.json() : null))
          .then((ix) => ({ files: new Set(Object.keys((ix && ix.files) || {})),
                           sharded: (ix && ix.sharded) || {},
                           relief: (ix && ix.relief) || null }))
          .catch(() => ({ files: new Set(), sharded: {}, relief: null }));
      }
      return mapIndex;
    }

    /** One cached promise per file, so a shard shared by two layers is fetched once. */
    function fetchJson(file) {
      if (!mapTier.loaded[file]) {
        mapTier.loaded[file] = fetch(MD + file)
          .then((r) => (r.ok ? r.json() : null)).catch(() => null);
      }
      return mapTier.loaded[file];
    }

    /* ★★★ A SHARDED LAYER IS FETCHED BY VIEWPORT, and that is the point of it -- somebody looking at
     *  Northampton downloads the shard holding Britain, not all 184,869 lakes on Earth. Each shard is
     *  matched against its OWN recorded data bbox, so a lake overhanging its grid cell is still found.
     *  ★ The caller passes the box it is about to draw; with no box (a caller that wants everything)
     *  every shard is loaded, which is correct but rarely what is wanted. */
    async function layer(t, f, box) {
      const base = t + '-' + f;
      const man = await mapManifest();
      const shards = man.sharded[base];
      if (shards) {
        if (t === 'tier2' && !man.files.has(shards[0].file)) return null;
        const wanted = box ? shards.filter((sh) => !(sh.box[2] < box[0] || sh.box[0] > box[2]
                                                  || sh.box[3] < box[1] || sh.box[1] > box[3]))
                           : shards;
        const parts = await Promise.all(wanted.map((sh) => fetchJson(sh.file)));
        const out = [];
        for (const p of parts) if (p) out.push(...p);
        return out;
      }
      /* ★★★ ONLY tier2 IS GATED ON THE INDEX. tier0/tier1 are the BUNDLED pack -- they are present by
       *  definition, and asking a manifest for permission to read them means any hiccup in one small
       *  JSON file blanks the entire basemap. The index exists to answer one question: is the optional
       *  detail pack installed? */
      if (t === 'tier2' && !man.files.has(base + '.json')) return null;
      return fetchJson(base + '.json');
    }

    /** ★ Best tier actually installed, at or below the one the zoom wants. */
    async function bestTier(want) {
      if (want !== 'tier2') return want;
      return (await mapManifest()).files.has('tier2-countries.json') ? 'tier2' : 'tier1';
    }

    /* ══ VIEW GEOMETRY ═══════════════════════════════════════════════════════════════════════════ */
    /** The view, grown by a margin so a small pan does not strip the edges before `moveend` fires. */
    function viewBox() {
      const b = map.getBounds().pad(0.25);
      const w = b.getWest(); const e = b.getEast();
      /* ★★ NORMALISED TO ONE WORLD for data lookup. On a wide screen at z3 the bounds run past ±180
       *  and every bbox test then fails against data that only exists once. The COPIES are drawn by
       *  worldOffsets() below; the culling box always describes a single world. */
      if (e - w >= 360) return [-180, b.getSouth(), 180, b.getNorth()];
      return [((w + 180) % 360 + 360) % 360 - 180, b.getSouth(),
              ((e + 180) % 360 + 360) % 360 - 180, b.getNorth()];
    }

    /* ★★★ THE WORLD IS DRAWN ONCE PER VISIBLE COPY. Stuart on a wide display, 2026-09-26: "the map can
     *  wrap around here ... joys of a wide screen." A TILE layer wraps for free because Leaflet asks
     *  for tile (x mod n); VECTORS do not — a polygon at lon -170 exists once, and past the antimeridian
     *  there is simply nothing, which is the dark band either side of the world he is seeing.
     *  ★★ So each layer is drawn at lon + k*360 for every k the viewport touches. Leaflet is perfectly
     *  happy with coordinates outside ±180 — it projects them, which is exactly what a second copy is.
     *  ★ Applied to the layers VISIBLE when wrapping is possible (z<=4-ish): terrain, land, cover,
     *  water, islands, and the labels that ride on them. ✗ NOT to roads, runways or the locator grid —
     *  those only draw from z5, z9 and z12, by which point one world fills the screen many times over
     *  and the extra copies would be pure work for nothing. */
    function worldOffsets() {
      const b = map.getBounds();
      const lo = Math.floor((b.getWest() + 180) / 360);
      const hi = Math.floor((b.getEast() + 180) / 360);
      if (hi - lo > 4) return [0];                 // pathological zoom-out: one world is enough
      const out = [];
      for (let k = lo; k <= hi; k++) out.push(k * 360);
      return out.length ? out : [0];
    }

    /* ══ THE COLLISION PLACER ════════════════════════════════════════════════════════════════════
     * ★★★ LABELS ARE PLACED BY COLLISION, NOT BY COUNT. A cap of 300 is right for the WORLD and
     *  absurd over London, where 300 GeoNames towns land in one viewport and the map vanishes under
     *  its own place names. The count was never the real control: what matters is whether two labels
     *  overlap ON SCREEN, which depends on zoom, text length and font size, none of which a cap knows.
     *  ★★ Greedy, in rank order: the most important place is placed first and keeps its space, so
     *  LONDON survives and Chalfont St Peter is the one that drops. That ordering is the whole point --
     *  a naive pass would let whichever town came first in the file evict the capital.
     *  ★ Cheap on purpose: an axis-aligned box per label and a linear scan. At a few hundred
     *  candidates that is nothing, and it runs once per redraw, not per frame.
     *  ★★★ THE HOST'S MARKERS ARE RESERVED BEFORE ANY LABEL IS PLACED. Stacking order alone would
     *  leave a name half-hidden UNDER a receiver glyph, which is still a name you cannot read next to
     *  a receiver you cannot see cleanly. Seeding the placer with `reserve()` means the label simply
     *  goes elsewhere -- or is dropped, which is the right answer when a receiver is what the user
     *  came for. */
    function labelPlacer(padPx = 2) {
      const placed = reserve().slice();
      return (latlng, text, fontPx, anchor = 'right', lines = 1) => {
        const p = map.latLngToContainerPoint(latlng);
        const w = text.length * fontPx * 0.62 + padPx * 2;
        // ★ A stacked label is taller, not wider — the box has to follow or it will overlap below.
        const h = fontPx * (lines === 1 ? 1 : lines * 1.3) + padPx * 2;
        const x = anchor === 'centre' ? p.x - w / 2 : p.x + 4;
        const y = anchor === 'centre' ? p.y - h * 1.4 : p.y - h / 2;
        for (const b of placed) {
          if (x < b.x + b.w && x + w > b.x && y < b.y + b.h && y + h > b.y) return false;
        }
        placed.push({ x, y, w, h });
        return true;
      };
    }

    /* ══ THE LOCATOR GRID ════════════════════════════════════════════════════════════════════════
     * ★★ Only the lines CROSSING THE VIEW are generated, and only at a zoom where they mean
     *  something: fields from z3, the finer squares from z6. Drawing the whole world's grid at z2 is
     *  324 fields of unreadable labels over a map nobody is reading the grid on.
     *  ★ The labels sit at each cell's CENTRE rather than a corner -- a locator names an area, and a
     *  label on the line reads as belonging to whichever cell the eye picks. */
    function drawGrid() {
      gridLayer.clearLayers();
      const z = map.getZoom();
      // ★ The locator grid belongs on a map about WHERE SOMEONE IS, not on an aircraft or ship map.
      if (z < 3 || !P.grid) return;
      /* ★ THREE LEVELS, matching the locator's three pairs: fields at world view, squares regionally,
       *  and subsquares once you are close enough that IO92nh is a place rather than a county. */
      /* ★★★ SUBSQUARES START AT z12, NOT z9. At z9 a wide window spans ~8 deg of longitude, which is
       *  96 subsquare columns by 140 rows -- THIRTEEN THOUSAND labels, and the map disappeared under
       *  them. The grid is a reference, not the subject; the moment it is easier to read than the
       *  coastline it has stopped being useful. */
      const level = z >= 12 ? 2 : z >= 6 ? 1 : 0;
      const dLon = [20, 2, 2 / 24][level];
      const dLat = [10, 1, 1 / 24][level];
      const chars = [2, 4, 6][level];
      const fine = level > 0;
      const b = map.getBounds();
      const w = Math.max(-180, Math.floor(b.getWest() / dLon) * dLon);
      const e = Math.min(180, Math.ceil(b.getEast() / dLon) * dLon);
      const s = Math.max(-90, Math.floor(b.getSouth() / dLat) * dLat);
      const n = Math.min(90, Math.ceil(b.getNorth() / dLat) * dLat);
      /* ★★★ A HARD CELL BUDGET, not just a per-axis guard. The old check allowed 140 x 140 = 19,600
       *  cells and still called that safe -- per-axis limits multiply, which is exactly how a guard
       *  that looks reasonable lets through a number that is not. */
      const cols = (e - w) / dLon;
      const rows = (n - s) / dLat;
      if (cols * rows > 900) return;
      const style = { color: '#c98f2e', weight: fine ? 0.4 : 0.7,
                      opacity: level === 2 ? 0.18 : fine ? 0.22 : 0.34,
                      interactive: false, pane: 'admin', renderer: paneRenderer.admin };
      for (let lon = w; lon <= e; lon += dLon) {
        L.polyline([[s, lon], [n, lon]], style).addTo(gridLayer);
      }
      for (let lat = s; lat <= n; lat += dLat) {
        L.polyline([[lat, w], [lat, e]], style).addTo(gridLayer);
      }
      // ★ Labels are dearer than lines: a DOM node each. Their budget is tighter than the grid's.
      if (z < 4 || cols * rows > 320) return;
      for (let lon = w; lon < e; lon += dLon) {
        for (let lat = s; lat < n; lat += dLat) {
          const code = maidenhead(lon + dLon / 2, lat + dLat / 2);
          L.marker([lat + dLat / 2, lon + dLon / 2], { interactive: false, icon: L.divIcon({
            className: 'gridLbl', html: code.slice(0, chars),
            iconSize: [0, 0], iconAnchor: [0, 0],
          }) }).addTo(gridLayer);
        }
      }
    }

    /* ══ SHADED RELIEF ═══════════════════════════════════════════════════════════════════════════
     * ★★★ SHADED RELIEF -- a raster, on purpose, in a basemap that deleted raster TILES. What was
     *  thrown away was a DEPENDENCY: images fetched on demand from somebody else's server, which went
     *  black when they were slow. This is one bundled file that ships with the map, so it has none of
     *  those failure modes -- and terrain genuinely is a continuous field, which is why the attempt to
     *  draw mountains as polygons produced blobs.
     *  ★★ IT SITS ABOVE THE LAND FILL AND BELOW EVERYTHING ELSE. The vector land is the ground truth
     *  for WHERE land is; the relief only says what the ground is doing. Its alpha fades out at sea
     *  level, so where the two sources disagree about a coastline neither one asserts anything.
     *  ★ Bounds are Web Mercator's own cut-off latitude, read from the index -- the image was
     *  reprojected with that exact constant, and 85 instead of 85.0511 slides it off the coast.
     *  ★★★ ONE GLOBAL IMAGE FAR OUT, A TILE GRID CLOSE IN. A single global image can only be as sharp
     *  as its total size allows -- at 4096 across the planet a pixel is 9.8 km, so at z9 it smears
     *  over ~125 screen pixels and Mount Teide (one ETOPO cell) is a blur. Stuart: "that blur bothers
     *  me its weird how the next zoom level is clearer but the mountain is missing."
     *  ★★ The tiles render at 8192 across the world and only the ones in view are fetched, which is
     *  the same bargain as the vector shards: resolution goes up without the download going up.
     *  ★ From z6 upward, and only if the detail pack is installed -- a basic-only install keeps the
     *  global image all the way in, which is coarse but never absent. */
    let reliefOverlay = null;
    let reliefTiles = null;
    let reliefKey = '';
    async function drawRelief() {
      const ix = await mapManifest();
      const cfg = ix.relief;
      if (!cfg) return;
      const z = map.getZoom();
      const lat = cfg.mercatorLat || 85.0511287798066;
      const offs = worldOffsets();

      if (cfg.tiles && reliefTiles === null) {
        reliefTiles = ix.files.has(cfg.tiles)
          ? await fetch(MD + cfg.tiles).then((r) => (r.ok ? r.json() : [])).catch(() => [])
          : [];
      }
      const useTiles = z >= 6 && reliefTiles && reliefTiles.length;
      const box = viewBox();
      const wanted = useTiles
        ? reliefTiles.filter((t) => !(t.bounds[2] < box[0] || t.bounds[0] > box[2]
                                  || t.bounds[3] < box[1] || t.bounds[1] > box[3]))
        : [];
      /* ★ A cheap identity for "the same set of images in the same places", so a pan that changes
       *  nothing does not tear the terrain down and rebuild it. */
      const key = (useTiles ? wanted.map((t) => t.file).join(',') : 'global') + '|' + offs.join(',');
      if (reliefOverlay && reliefKey === key) return;
      if (reliefOverlay) map.removeLayer(reliefOverlay);
      reliefKey = key;
      reliefOverlay = L.layerGroup();
      const add = (file, b) => {
        for (const dx of offs) {
          L.imageOverlay(MD + file, [[b[1], b[0] + dx], [b[3], b[2] + dx]],
            { pane: 'relief', opacity: 0.92, interactive: false, className: 'reliefImg' })
            .addTo(reliefOverlay);
        }
      };
      if (useTiles) for (const t of wanted) add(t.file, t.bounds);
      else add(cfg.basic, [-180, -lat, 180, lat]);
      reliefOverlay.addTo(map);
    }

    /* ══ LABELS: REGIONS, SEAS, CONTINENTS, COUNTRIES ════════════════════════════════════════════
     * ★★★ PHYSICAL REGIONS AND SEAS -- the rung between "continent" and "country". At z3 the map
     *  showed a vast sand-coloured area with no name on it; now it says SAHARA. These names span
     *  countries, so they cannot come from the country layer, and they are the answer to the only
     *  question anyone asks at that zoom.
     *  ★ Sea names are ITALIC and cooler-toned: a sea name in the land palette reads as somewhere you
     *  could stand. */
    async function drawRegionLabels() {
      regionLabelLayer.clearLayers();
      const z = map.getZoom();
      if (!rung('regions', z) && !rung('seas', z)) return;
      const regions = await layer('tier0', 'regions');
      if (!regions) return;
      const box = viewBox();
      // ★ Same rank-against-zoom thinning as everything else, then the collision placer has the
      //   final say -- these are the biggest labels on the map and they collide hardest.
      const cut = z <= 3 ? 2 : z <= 4 ? 3 : z <= 5 ? 4 : 6;
      const place = labelPlacer(8);
      for (const [name, lon, lat, rank, kind] of regions) {
        if (rank > cut) continue;
        if (kind === 'sea' ? !rung('seas', z) : !rung('regions', z)) continue;
        if (!inBox(box, lon, lat)) continue;
        /* ★★ SEA NAMES ARE SMALLER THAN LAND ONES. Stuart: "Font size could be smaller on the seas."
         *  They cover enormous empty areas, so at the same size as a mountain range they dominate a
         *  view whose subject is the land. A sea label only has to be findable, not prominent. */
        const fontPx = kind === 'sea' ? Math.max(7, 10 - rank) : Math.max(9, 13 - rank);
        /* ★★★ LONG NAMES STACK, the way a paper atlas sets them:
         *      NORTH
         *      ATLANTIC
         *      OCEAN
         *  Stuart, 2026-09-26: "Dont be afraid to use multiple lines like other maps do." It is not
         *  only prettier -- a 20-character label laid flat spans thousands of kilometres and sprawls
         *  off the water it names, while a stacked one sits INSIDE the shape. That is also why the
         *  collision box below measures the LONGEST WORD, not the whole string. */
        const words = String(name).toUpperCase().split(/\s+/).filter(Boolean);
        const stack = words.length > 1 && name.length > 11;
        const lines = stack ? words : [words.join(' ')];
        const widest = lines.reduce((m, l) => Math.max(m, l.length), 0);
        if (!place([lat, lon], 'x'.repeat(widest), fontPx, 'centre', lines.length)) continue;
        L.marker([lat, lon], { interactive: false, icon: L.divIcon({
          className: 'rgnLbl' + (kind === 'sea' ? ' sea' : ''),
          html: '<span style="font-size:' + fontPx + 'px">' + lines.map(esc).join('<br>') + '</span>',
          iconSize: [0, 0], iconAnchor: [0, 0],
        }) }).addTo(regionLabelLayer);
      }
    }

    /* ★★ COUNTRY NAMES USE NATURAL EARTH'S OWN LABEL POINTS, not polygon centroids. A centroid puts
     *  "Norway" in the North Sea and "Chile" in Argentina; LABEL_X/LABEL_Y are placed by hand by the
     *  cartographers for exactly this. LABELRANK then thins them by zoom, biggest countries first --
     *  242 labels, 7.4 kB.
     *  ★ Thin the labels by RANK against zoom, or the world view is a wall of overlapping names. */
    async function drawCountryNames() {
      if (countryLabels === null) {
        countryLabels = await fetch(MD + 'country-labels.json')
          .then((r) => (r.ok ? r.json() : [])).catch(() => []);
      }
      countryLabelLayer.clearLayers();
      const z = map.getZoom();
      if (rung('continents', z)) {
        for (const [name, lon, lat] of CONTINENT_LABELS) {
          L.marker([lat, lon], { interactive: false, icon: L.divIcon({
            className: 'ctyLbl contLbl', html: name, iconSize: [0, 0], iconAnchor: [0, 0],
          }) }).addTo(countryLabelLayer);
        }
      }
      if (!rung('countries', z)) return;
      const cut = z <= 4 ? 3 : z <= 5 ? 5 : 7;
      for (const [name, lon, lat, rank, ...rest] of countryLabels) {
        if (rank > cut) continue;
        /* ★ The local name rides UNDER the English one, and only where Natural Earth actually has a
         *  distinct endonym -- 70 of 239 countries. Deutschland, Türkiye, Việt Nam, Україна all land;
         *  the rest simply show one line rather than a blank second one. */
        const local = rest[0];
        const html = esc(name) + (local && z >= 4 ? '<i>' + esc(local) + '</i>' : '');
        L.marker([lat, lon], { interactive: false, icon: L.divIcon({
          className: 'ctyLbl', html, iconSize: [0, 0], iconAnchor: [0, 0],
        }) }).addTo(countryLabelLayer);
      }
    }

    /* ══ GROUND ══════════════════════════════════════════════════════════════════════════════════ */
    async function drawIslands() {
      const z = map.getZoom(), box = viewBox();
      const t = await bestTier(tierFor('islands', z));
      const [islands, reefs, playas] = await Promise.all([
        layer(t, 'islands', box),
        z >= 4 ? layer(t === 'tier0' ? 'tier1' : t, 'reefs', box) : Promise.resolve(null),
        z >= 4 ? layer(t === 'tier0' ? 'tier1' : t, 'playas', box) : Promise.resolve(null),
      ]);
      islandLayer.clearLayers();
      const offs = worldOffsets();
      for (const ring of islands || []) {
        if (!ringInView(ring, box)) continue;
        for (const dx of offs) L.polygon(ring.map(([lon, lat]) => [lat, lon + dx]),
          inPane('land', { color: MAP_COAST, weight: 0.5, opacity: 0.8,
                           fillColor: MAP_LAND, fillOpacity: 1 })).addTo(islandLayer);
      }
      for (const ring of playas || []) {
        if (!ringInView(ring, box)) continue;
        L.polygon(ring.map(([lon, lat]) => [lat, lon]),
          inPane('cover', { stroke: false, fillColor: MAP_PLAYA, fillOpacity: 0.8 })).addTo(islandLayer);
      }
      for (const line of reefs || []) {
        if (!ringInView(line, box)) continue;
        L.polyline(line.map(([lon, lat]) => [lat, lon]),
          inPane('water', { color: MAP_REEF, weight: 0.7, opacity: 0.5 })).addTo(islandLayer);
      }
    }

    async function drawGeoLines() {
      geoLineLayer.clearLayers();
      const z = map.getZoom();
      if (z < 2 || z > 8) return;
      const gl = await layer('tier0', 'geolines');
      for (const { name, lines } of gl || []) {
        /* ★★ THE INTERNATIONAL DATE LINE IS IN THIS FILE TOO, and it is the only entry that is not a
         *  line of latitude: it zigzags around Kiribati and the Aleutians. Drawn in the same amber
         *  dash as the tropics it reads as a rendering fault -- Stuart's first look at the finished
         *  map picked out "rectangles" near Patagonia and Alaska that were exactly this. It says
         *  nothing about propagation, so it goes. */
        if (/Date Line/i.test(name)) continue;
        const tropic = /Tropic/i.test(name);
        for (const line of lines) {
          L.polyline(line.map(([lon, lat]) => [lat, lon]),
            inPane('admin', { color: '#c98f2e', weight: 0.6, opacity: tropic ? 0.3 : 0.42,
                              dashArray: tropic ? '6,6' : null })).addTo(geoLineLayer);
        }
      }
    }

    async function drawWater() {
      const z = map.getZoom(), box = viewBox();
      const pick2 = (what) => bestTier(tierFor(what, z)).then((t) => layer(t, what, box));
      const [shelf, lakes, rivers, admin1] = await Promise.all([
        pick2('shelf'), pick2('lakes'), pick2('rivers'),
        z >= 5 ? pick2('admin1') : Promise.resolve(null),
      ]);
      shelfLayer.clearLayers(); waterLayer.clearLayers(); adminLayer.clearLayers();
      const offs = worldOffsets();
      for (const ring of shelf || []) {
        if (!ringInView(ring, box)) continue;
        for (const dx of offs) L.polygon(ring.map(([lon, lat]) => [lat, lon + dx]),
          inPane('shelf', { stroke: false, fillColor: MAP_SHELF, fillOpacity: 0.55 })).addTo(shelfLayer);
      }
      for (const ring of lakes || []) {
        if (!ringInView(ring, box)) continue;
        for (const dx of offs) L.polygon(ring.map(([lon, lat]) => [lat, lon + dx]),
          inPane('water', { stroke: false, fillColor: MAP_LAKE, fillOpacity: 1 })).addTo(waterLayer);
      }
      let n = 0;
      for (const line of rivers || []) {
        if (n > 2000) break;
        if (!ringInView(line, box)) continue;
        n++;
        L.polyline(line.map(([lon, lat]) => [lat, lon]),
          inPane('water', { color: MAP_RIVER, weight: z >= 8 ? 0.9 : 0.6, opacity: 0.55 })).addTo(waterLayer);
      }
      /* ★ Admin-1 lines are DASHED and dim on purpose: a solid line the same weight as a coastline
       *  reads as a border between countries, which is a factual error on a map, not a style one. */
      n = 0;
      for (const line of admin1 || []) {
        if (n > 2000) break;
        if (!ringInView(line, box)) continue;
        n++;
        L.polyline(line.map(([lon, lat]) => [lat, lon]),
          inPane('admin', { color: MAP_ADMIN, weight: 0.5, opacity: 0.4, dashArray: '3,3' })).addTo(adminLayer);
      }
    }

    /* ★★★ NO `alpine` LAYER HERE, DELIBERATELY. Natural Earth's 'Range/mtn' polygons are ENVELOPES
     *  DRAWN AROUND A RANGE FOR LABEL PLACEMENT, not terrain. Filled, they are pale grey slabs that
     *  bear no relation to where the ground is high -- one of them covers Belgium. Stuart,
     *  2026-09-26: "the mountains look a bit shit, bit like big random blobs ... they may have to go."
     *  ★★ The deserts work BECAUSE a desert genuinely is an area; a mountain range is a shape, and the
     *  only honest source for it is elevation data -- which is what drawRelief does. ✗ Do not re-add
     *  this layer from NE polygons. If peaks are wanted, contour a DEM (ETOPO2v2c is a 73 MB plain
     *  int16 grid) -- that is the same standard the rest of this basemap is held to.
     *  ★ Same rule as AGENTS.md's dead control: a feature that misdescribes the world is worse than an
     *  absent one, because the user believes it. */
    async function drawCover() {
      const z0 = map.getZoom(), box = viewBox();
      const [cover, urban] = await Promise.all([
        bestTier(tierFor('cover', z0)).then((t) => layer(t, 'cover', box)),
        bestTier(tierFor('urban', z0)).then((t) => layer(t, 'urban', box)),
      ]);
      coverLayer.clearLayers(); urbanLayer.clearLayers();
      const offs = worldOffsets();
      const paint = (rings, target, pane, colour, opacity) => {
        for (const ring of rings || []) {
          if (!ringInView(ring, box)) continue;
          for (const dx of offs) {
            L.polygon(ring.map(([lon, lat]) => [lat, lon + dx]),
              inPane(pane, { stroke: false, fillColor: colour, fillOpacity: opacity })).addTo(target);
          }
        }
      };
      for (const [k, colour] of Object.entries(COVER_COLOUR)) {
        paint(cover && cover[k], coverLayer, 'cover', colour, 0.85);
      }
      /* ★★ URBAN IS SEMI-TRANSPARENT ON PURPOSE. Natural Earth's urban extent is a blob around a
       *  city, not its street plan; at full opacity a solid grey lozenge reads as a hole in the map.
       *  Letting the ground tint through keeps it looking like a built-up AREA. */
      paint(urban, urbanLayer, 'urban', MAP_URBAN, 0.62);
    }

    async function drawRoads() {
      roadLayer.clearLayers();
      const z = map.getZoom();
      if (z < 5 || !P.roads) return;            // below this a road is a scratch, not a road
      const box = viewBox();
      const lines = await layer(await bestTier(tierFor('roads', z)), 'roads', box);
      if (!lines) return;
      let n = 0;
      for (const line of lines) {
        if (n > 2500) break;                     // the same cap that keeps the point layers honest
        if (!ringInView(line, box)) continue;
        n++;
        L.polyline(line.map(([lon, lat]) => [lat, lon]),
          inPane('road', { color: MAP_ROAD, weight: z >= 9 ? 1 : 0.7, opacity: 0.5 })).addTo(roadLayer);
      }
    }

    async function drawBase() {
      const z = map.getZoom();
      const box = viewBox();
      const offs = worldOffsets();
      /* ★★★ AT HIGH ZOOM THE LAND COMES FROM A COASTLINE DATASET, NOT FROM COUNTRY POLYGONS. We were
       *  deriving the shore from a POLITICAL layer, which is not what it is for: Natural Earth's 10m
       *  countries give Madeira TEN POINTS for a 57 km island, so Cristiano Ronaldo International's
       *  over-the-sea runway platform sat stranded in open water. Stuart asked whether it was really
       *  in the sea -- it genuinely is, on 180 columns, but the COAST beside it was the vague thing.
       *  ★★ MEASURED, against the runway's own published thresholds:
       *       country fills  -- both thresholds stranded
       *       ne_10m_land    -- 05 is 0.45 km offshore, 23 is 2.07 km offshore (does NOT fix it)
       *       OSM coastline  -- BOTH THRESHOLDS ON LAND
       *  I first recommended ne_10m_land on point count alone, which is a proxy; the proxy said 5x
       *  better and the actual test said still broken. ★ Test the thing you care about.
       *  ★ Borders then have to arrive as their OWN lines, the way a real map separates coast from
       *  boundary -- with the fill no longer political, the country edges are no longer free. */
      const coast = z >= 10 ? await layer(await bestTier('tier2'), 'coast', box) : null;
      basemap.clearLayers();
      if (coast) {
        for (const ring of coast) {
          if (!ringInView(ring, box)) continue;
          for (const dx of offs) {
            L.polygon(ring.map(([lon, lat]) => [lat, lon + dx]), inPane('land', {
              color: MAP_COAST, weight: 0.6, opacity: 0.8, fillColor: MAP_LAND, fillOpacity: 1,
            })).addTo(basemap);
          }
        }
        const borders = await layer('tier2', 'borders', box);
        for (const line of borders || []) {
          if (!ringInView(line, box)) continue;
          for (const dx of offs) {
            L.polyline(line.map(([lon, lat]) => [lat, lon + dx]),
              inPane('admin', { color: MAP_COAST, weight: 0.8, opacity: 0.6 })).addTo(basemap);
          }
        }
        mapTier.at = 'coast';
        return;
      }
      const t = await bestTier(tierFor('countries', z));
      const countries = await layer(t, 'countries', box);
      if (!countries) return;
      for (const rings of Object.values(countries)) {
        for (const ring of rings) {
          if (!ringInView(ring, box)) continue;
          for (const dx of offs) {
            L.polygon(ring.map(([lon, lat]) => [lat, lon + dx]), inPane('land', {
              color: MAP_COAST, weight: 0.6, opacity: 0.8, fillColor: MAP_LAND, fillOpacity: 1,
            })).addTo(basemap);
          }
        }
      }
      mapTier.at = t;
      // ★ Nothing to re-stack: the panes above hold the order. See the bringToFront() note.
    }

    /* ══ PLACES ══════════════════════════════════════════════════════════════════════════════════ */
    async function drawPlaces() {
      const z = map.getZoom(), box = viewBox();
      const places = await layer(await bestTier(tierFor('places', z)), 'places', box);
      placeLayer.clearLayers();
      if (!places) return;
      /* ★★★ A LABEL'S WEIGHT IS ITS IMPORTANCE. Every town drawn at the same 9 px meant London and
       *  Brixworth carried equal visual weight, which is not a style problem -- it is a MAP that
       *  refuses to say which place matters. Rank drives the size, and a CAPITAL gets its own colour
       *  plus a hollow ring, because a capital is the thing a listener orients by and colour reads
       *  faster than size at a glance.
       *  ★ Capital flag: 1 = national, 2 = regional. Absent means 0 -- the generator omits it rather
       *  than write seven thousand trailing zeroes. */
      /* ★ Candidates come in rank order so the placer's greedy pass is also an importance pass. */
      const cands = pick(places, box, CAP.places, (r) => r[3] ?? 9)
        .sort((a, b) => (a[3] ?? 9) - (b[3] ?? 9));
      /* ★★ PADDING SCALES WITH ZOOM. At world view a label needs breathing room measured in DEGREES,
       *  not pixels: two capitals 300 km apart are adjacent on screen at z3, and a 2 px gap between
       *  them still reads as a smear. Close in, tight packing is fine and wanted. */
      const place = labelPlacer(z <= 3 ? 9 : z <= 5 ? 6 : 3);
      /* ★★★ AT WORLD VIEW MOST PLACES ARE NOT WORTH DRAWING AT ALL. Collision alone cannot fix z3:
       *  it thins the LABELS but still scatters a DOT for every candidate, and 700 pale dots across
       *  the world was the clutter Stuart saw. A place must earn its place on the map before it
       *  competes for space on it. */
      /* ★★ A 'major' profile stops the ladder at large cities however far you zoom in: on an
       *  aircraft map Brixworth is not orientation, it is interference. */
      if (!rung('cities', z)) return;           // ★ the ladder decides, not a per-site zoom check
      const rankCut = P.places === 'major' ? 3
        : z <= 5 ? 4 : z <= 6 ? 6 : 9;
      for (const [name, lon, lat, rank, cap] of cands) {
        if ((rank ?? 9) > rankCut && cap !== 1) continue;
        // ★ Even capitals thin at the very top: Vaduz and San Marino are not world-view cities.
        if (cap === 1 && z <= 3 && (rank ?? 9) > 3) continue;
        const r = rank ?? 9;
        const major = r <= 2;
        const cls = cap === 1 ? 'mapLbl capital' : major ? 'mapLbl major' : cap === 2 ? 'mapLbl admin1' : 'mapLbl';
        /* ★★★ A MAJOR CITY'S LABEL GROWS WITH ZOOM. Stuart at z9 over the south-east: "at this level
         *  London needs a big LONDON label over it." He is right, and a fixed 12 px could never do it
         *  -- at world view that is already shouting, and at z9 it whispers. The size is therefore a
         *  function of BOTH rank and zoom: important places get louder as you close in on them, the
         *  way a real map's type does.
         *  ★ Uppercase and centred above the dot for the top tier only: it is a label for a REGION of
         *  the map at that point, not a pin for a point on it. */
        const shout = cap === 1 || r === 0;
        const px = shout ? Math.min(22, 11 + Math.max(0, z - 5) * 1.5)
          : major ? Math.min(14, 10 + Math.max(0, z - 6) * 0.6) : 0;
        /* ★★★ THE DOT FOLLOWS THE LABEL, NEVER THE REVERSE. An unlabelled dot on a world map says
         *  "something is here" and nothing more -- pure noise. Drawing it first meant every place the
         *  placer REJECTED still left its mark, so the thinning was invisible. */
        const big = shout && z >= 7;
        const text = big ? String(name).toUpperCase() : String(name);
        const fontPx = px || (major ? 11 : 9);
        const wantLabel = z >= 4 || major || cap === 1;
        if (!wantLabel || !place([lat, lon], text, fontPx, big ? 'centre' : 'right')) continue;
        L.circleMarker([lat, lon], inPane('place', {
          radius: cap === 1 ? 2.6 : major ? 2.2 : 1.5,
          weight: cap === 1 ? 1 : 0, color: MAP_CAPITAL,
          fillColor: cap === 1 ? MAP_CAPITAL : MAP_TOWN,
          fillOpacity: cap === 1 ? 0.55 : 0.9,
        })).addTo(placeLayer);
        /* ★ The size is inline because it is computed from rank AND zoom together; a CSS class can
         *  express one or the other but not the product. */
        L.marker([lat, lon], { interactive: false, icon: L.divIcon({
          className: cls + (big ? ' shout' : ''),
          html: '<span style="font-size:' + fontPx.toFixed(1) + 'px">' + esc(text) + '</span>',
          iconSize: [0, 0], iconAnchor: big ? [0, 0] : [-5, 6],
        }) }).addTo(placeLayer);
      }
    }

    /* ══ AIRFIELDS, RUNWAYS AND PORTS ════════════════════════════════════════════════════════════
     * ★★★ RUNWAYS, DRAWN AS REAL GEOMETRY AND LABELLED WITH WHAT ATC SAYS. Stuart, 2026-09-26: "a
     *  user could hear ATC talking about runway 21L or whatever they use, our maps could show them."
     *  That is the difference between a map that decorates the audio and one that ANSWERS it.
     *  ★★ Both thresholds come from the source, so this is the actual centreline and the actual
     *  heading -- not a symbol rotated to a bearing. Each end is labelled with its own designator,
     *  because 09L and 27R are the SAME strip of tarmac and which one you hear tells you the wind. */
    async function drawRunways() {
      runwayLayer.clearLayers();
      const z = map.getZoom();
      if (z < 9 || !P.runways) return;
      /* ★★★ THE ZOOM GATE IS PER RUNWAY, BY LENGTH. A flat "z12 and up" hid Stansted's 10,003 ft
       *  04/22 at z10 and z11, where it is already 40 px long and perfectly legible -- Stuart looked
       *  for it and it was not there, which reads as MISSING DATA rather than a threshold. A runway
       *  should appear as soon as it is big enough to see, and that depends on how long it is.
       *  ★ Roughly: a runway earns its line once it would draw at about 25 px. */
      const minFt = z >= 12 ? 2000 : z >= 11 ? 4000 : z >= 10 ? 7000 : 9000;
      const box = viewBox();
      const rw = await layer(await bestTier('tier2'), 'runways', box);
      if (!rw) return;
      for (const [, aLon, aLat, bLon, bLat, leId, heId, len] of rw) {
        if (len < minFt) continue;
        if (!inBox(box, aLon, aLat) && !inBox(box, bLon, bLat)) continue;
        L.polyline([[aLat, aLon], [bLat, bLon]], inPane('road', {
          color: '#d8d2c4', weight: z >= 14 ? 3.5 : z >= 12 ? 2.2 : 1.6, opacity: 0.75, lineCap: 'butt',
        })).addTo(runwayLayer)
          .bindTooltip(esc(leId) + '/' + esc(heId) + ' · ' + len.toLocaleString() + ' ft');
        // ★ Designators once there is room for them beside the line, not before.
        if (z < 12) continue;
        for (const [id, la, lo] of [[leId, aLat, aLon], [heId, bLat, bLon]]) {
          if (!id) continue;
          L.marker([la, lo], { interactive: false, icon: L.divIcon({
            className: 'rwLbl', html: esc(id), iconSize: [0, 0], iconAnchor: [0, 0],
          }) }).addTo(runwayLayer);
        }
      }
    }

    /* ★★★ AIRPORTS AND SEA PORTS. Stuart, 2026-09-26: "the port names and airports would be useful on
     *  this map for those seeking out ATC traffic from around the world." Somebody hunting airband
     *  picks a receiver by what is near a field, and a receiver's marker means nothing until you can
     *  see the airport it is pointed at.
     *  ★★ THE CODE IS THE LABEL, not the name -- "EGBK" is what you tune for and what fits; the full
     *  name is in the tooltip. IATA where it exists (people say "BHX"), ICAO otherwise: Sywell has no
     *  IATA, and EGBK is what an ATC listener wants anyway.
     *  ★ Class from the source: 0=large, 1=medium, 2=small, 3=heliport, 4=seaplane. The class gate is
     *  a FLOOR on what is eligible; the viewport cap above decides how many of them actually draw. */
    async function drawPoi() {
      const z = map.getZoom();
      airportLayer.clearLayers(); portLayer.clearLayers();
      if (z < 5 || !(P.airports || P.ports)) return;
      const box = viewBox();
      const t = await bestTier(tierFor('airports', z));
      const [airports, ports] = await Promise.all([layer(t, 'airports', box), layer('tier1', 'ports', box)]);
      const aCut = z <= 6 ? 0 : z <= 8 ? 1 : 4;
      // ★ Airport codes compete with city names for the same pixels, so they get a placer too.
      const placeAp = labelPlacer(z <= 5 ? 6 : 2);
      for (const [icao, lon, lat, cls, iata, name] of
           pick(airports || [], box, CAP.airports, (r) => r[3], (r) => r[3] <= aCut && hasCode(r))) {
        const code = iata || icao;
        /* ★★ A GLYPH BEFORE THE CODE, so the kind of field is readable without parsing the code:
         *  an aeroplane for a runway airport, [H] for a heliport. Stuart asked for both by name.
         *  ★★★ AND THE BIG FIELDS ARE LOUDER. "Large international airports get a larger easier to
         *  identify label compared to the hoardes of tiny airfields" -- with one size, EGLL and a
         *  farm strip competed equally, and the map failed to answer the only question an airband
         *  listener is asking: which of these is the busy one? */
        if (!P.airports) continue;
        if (!placeAp([lat, lon], iata || icao, cls === 0 ? 12 : 9)) continue;
        const kind = cls >= 3 ? 'heli' : cls === 0 ? 'big' : cls === 1 ? 'med' : 'small';
        const glyph = cls >= 3 ? '<i class="g">[H]</i>' : '<i class="g">✈</i>';
        L.marker([lat, lon], { icon: L.divIcon({
          className: 'apLbl ' + kind, html: glyph + esc(code),
          iconSize: [0, 0], iconAnchor: [0, 0],
        }) }).addTo(airportLayer).bindTooltip(esc(name || code));
      }
      if (z < 6 || !P.ports) return;
      for (const [name, lon, lat] of pick(ports || [], box, CAP.ports, () => 0)) {
        // ★ A ship for a sea port -- the same reasoning as the aeroplane: the symbol says what KIND
        //   of thing this is before the name is read, which matters on a map carrying five kinds.
        L.marker([lat, lon], { icon: L.divIcon({
          className: 'ptLbl', html: '<i class="g">⚓</i>' + esc(name),
          iconSize: [0, 0], iconAnchor: [0, 0],
        }) }).addTo(portLayer);
      }
    }

    /* ══ THE REDRAW ══════════════════════════════════════════════════════════════════════════════
     * ★ One coalesced redraw for zoom AND pan. Panning changes what is drawn, so `moveend` must
     *  redraw too -- and a drag fires it often enough that the work needs a frame to settle. */
    let drawing = false;
    let redrawT = null;
    function redrawMap() {
      clearTimeout(redrawT);
      redrawT = setTimeout(() => {
        if (drawing) return;
        drawing = true;
        drawGrid();
        Promise.all([drawBase(), drawRelief(), drawRegionLabels(), drawCover(), drawWater(), drawRoads(),
                     drawIslands(), drawGeoLines(), drawRunways(),
                     drawCountryNames(), drawPlaces(), drawPoi()])
          // ★★★ NEVER SWALLOW A DRAW ERROR AGAIN. The silent .catch here is precisely why a TypeError
          //   in drawBase looked like "the feature did not ship" for two deploys.
          .catch((e) => console.error('map draw failed:', e))
          .finally(() => { drawing = false; });
      }, 120);
    }
    map.on('zoomend moveend', redrawMap);
    redrawMap();

    return {
      map,
      redraw: redrawMap,
      /* ★★★ THE LAYERS ARE EXPOSED SO THE MAP CAN BE MEASURED. "It still looks fine" is not a check:
       *  scratchpad/baseline.js counts what each group actually contains at nine fixed views, and
       *  that is how this extraction was verified rather than eyeballed. They were page globals
       *  before, so an instrument could reach them; making them per-map state would have taken the
       *  only test away. ✗ Do not draw into these from a host -- read them. */
      layers: { basemap, place: placeLayer, country: countryLabelLayer, region: regionLabelLayer,
                grid: gridLayer, cover: coverLayer, urban: urbanLayer, road: roadLayer,
                shelf: shelfLayer, water: waterLayer, admin: adminLayer, island: islandLayer,
                geo: geoLineLayer, runway: runwayLayer, airport: airportLayer, port: portLayer },
      /** ★ A getter, not a field: the relief group is REPLACED on every tile-set change, so a field
       *   captured at attach would name a layer that has since been removed from the map. */
      relief: () => reliefOverlay,
      /** ★ Selected at attach or switched later, so a host page or the app's WebView picks a profile
       *   without a rebuild. Returns false for a name that is not a profile rather than silently
       *   drawing the default -- a caller with a typo should find out. */
      setProfile(name) {
        if (!PROFILES[name]) return false;
        P = PROFILES[name];
        redrawMap();
        return true;
      },
      profile: () => P,
      /** ★ Everything this attach added, removed. A host that tears its map down and rebuilds it
       *   (the app's WebView does, on a profile change from native) would otherwise stack renderers
       *   and every redraw would do the work twice. */
      detach() {
        clearTimeout(redrawT);
        map.off('zoomend moveend', redrawMap);
        for (const lg of allLayers) map.removeLayer(lg);
        if (reliefOverlay) { map.removeLayer(reliefOverlay); reliefOverlay = null; }
      },
    };
  }

  global.VibeMap = { attach, PROFILES, LADDER, CSS, maidenhead, MAP_SEA, MAP_LAND, MAP_CAPITAL };
})(typeof window !== 'undefined' ? window : globalThis);
