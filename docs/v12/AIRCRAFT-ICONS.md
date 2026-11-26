# The right aircraft glyph everywhere (beta96, branch b96-acicons)

Stuart, 2026-11-26: "for the ADS-B aircraft on the community map can they be the correct icon for the aircraft type like
the ADS-B map … now it's the aviation map I hope it's not just a generic plane icon".

## Before
- **ADS-B map / AIR & SEA, live ADS-B:** the class glyph from the DO-260B emitter category (`adsblayer.js iconFor`). This
  survived the one-map merge (proved: `test-web-ac-icons` against the pre-beta96 layer passes its four live rows).
- **AIR & SEA / aviation map, everything else** (VDL2, ACARS, HFDL, ADS-C, remembered ADS-B): no category, so **every one
  drew the generic jet**, even with the aircraft data (type, wake, engines, description letter) on the row.
- **Community map (directory):** **every aircraft** was one hand-drawn silhouette (`AC_SVG`) in its mode's colour.

## Now — one rule, one set of shapes
`web/mapkit/adsblayer.js`, the **ACICONS block** (between `ACICONS — BEGIN` and `ACICONS — END`): `CAT_ICON`, `iconFor`,
`iconClassOf`, the `ICONS` geometry, `NO_ROTATE`, `acIconSvg`, `AC_CLASS_NAME`. Self-contained on purpose.

`iconClassOf(a)`, in order:
1. the emitter category when it says something (A0 / none / unknown says nothing);
2. the aircraft data: `GLID` / "glider" → glider; `BALL` / `SHIP` / "balloon" → balloon; description letter H or G →
   heli; a known fast-jet designator → fighter; B752/B753 → b757; `ULAC` → ultralight; wake J or H → heavy; engines …T →
   regional; …P / …E → light; wake L → light (a light **jet** → regional, never the propeller glyph);
3. no wake / engines / letter (a server before beta96 sends the type alone): the designator's family — wide-bodies heavy,
   common helicopters, turboprops, light singles;
4. else the jet.

Readers:
- the layer itself (`s.kind = iconClassOf(raw)` on every update — so the data arriving a poll later re-picks the glyph):
  server web client, app WebView, `/mapgl/vendor/adsblayer.js` — copies via `scripts/gen-adsblayer-source.mjs`;
- the AIR & SEA / aviation rows already carried `t`, `wtc`, `eng`, `species`, `mil` (`aviationMap.ts avPlaneAdsb`);
- the community kit: `scripts/build-community-kit.mjs` lifts the ACICONS block (esbuild plugin, import `vibe:acicons`) —
  `CommunityKit.aircraftIconClass / aircraftIconSvg / AIRCRAFT_CLASS_NAMES / AIRCRAFT_ICONS`. Never copied by hand.

Server: `vibe_community.h aircraftCompact` field 11 `what` = `"wake|letter|engines|m"` from the aircraft data cache
(`vibe_aircraft_info.h`, `Extra.wtc/species/eng/mil`), and the data's type where the row has none. The rule is NOT in C++.
`aircraft.json` already carried these (`Extra.fields`). The emitter category is not in the shared table, so the community
feed has none: the data decides there (it covers almost every ADS-B aircraft).

Directory (`main`, branch dir-acicons): `community-map.js whatOf` parses field 11; the merge keeps a type / what another
server knew (`known()`); `community-map.html iconOf` draws `KIT.aircraftIconSvg(class, mode colour, 34)` rotated by track
(not balloon / parachute / obstacle); the KEY lists the classes. An older kit (no `aircraftIconClass`) falls back to AC_SVG.

## Tests
- `scripts/test_adsb_layer.mjs` 239/0 — the iconClassOf table (category wins; DB fallbacks; type alone; unknown → jet),
  the ACICONS block evaluated on its own gives the same answers and SVG, iconSvg unchanged, copies fresh.
- `scripts/test-web-ac-icons.mjs` 16/0 (new; in run-tests.sh) — real MapLibre + the layer in headless muted Edge: 5 live
  ADS-B + 7 rows through `avPlaneAdsb` draw heli / heavy / light / jet / regional; a late answer re-picks (→ glider).
  The pre-beta96 layer: 8 fail (all datalink/remembered = jet).
- `vibeserver/test-community.cpp` 76/0 — field 11, the data's type fill-in, separator-safe, empty when unknown; fixture
  now carries a B77W and an EC35.
- directory `test-community-render.mjs` 86/0 — heavy / heli / jet from beta96 rows, heavy from an older server's type
  alone, the merge keeps the type, each drawn with the kit's shape on the GPU layer, rotation, the KEY. `test-community.mjs` 77/0.
- The shim syntax-checks (`-fsyntax-only`, the server's own flags); `vibe_web_page.h` rebuilt via the lock.

## RESUME HERE
- Not verified on a live server or a phone. Not built into the iOS prebuilt lib (`build_ios.sh`) — the community feed is
  server-side; the app's map gets the layer via `src/generated/adsbLayerSource.ts` (regenerated).
- The directory's `community-kit.js` was regenerated from b96 V12 (whole kit, not just the aircraft part).
- `avPlaneIcon` (aviationMap.ts) still draws one airliner for the KEY's source colours — it is a colour key, not a
  per-aircraft glyph.
