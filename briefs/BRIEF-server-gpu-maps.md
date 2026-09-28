# BRIEF — the GPU map on every VibeServer (web client admin + spots maps), served by the server itself

Stuart, 2026-09-28 00:30: *"build updated server software for all platforms with the new maps serving
for their admin pages and Digital spots (sent from server rendered on client) also the map download
trigger for the servers in their admin pages please."*

The app (MapOverlay) and the directory already run the GPU map: `web/mapkit/vibemapgl.js` (renderer,
`VibeMapGL.createNow` / `create`) + `web/mapkit/vibemapgl-compat.js` (the Leaflet-shaped adapter,
`VibeMapGLCompat.install(vm)` → an `L`) + `web/mapkit/vendor/{maplibre-gl.js,maplibre-gl.css,pmtiles.js}` +
`web/mapkit/vibemap-style.json` + packs built into `build/maptiles/` and copied by
`scripts/sync-mapgl-assets.mjs` into `assets/mapgl/` (app) and `directory/public/mapgl/` (directory).
Read `directory/public/index.html` (search "THE SWITCH") for the most recent working host integration,
and `src/components/MapOverlay.tsx` for the app's.

**"Sent from server, rendered on client"**: the SERVER serves the map files; the listener's BROWSER
renders them. Nothing is fetched from a CDN (today's web client loads Leaflet from unpkg — the GPU map
must not depend on the internet; a LAN-only server must still draw its maps).

## THE CONTRACT (every agent codes to this; do not change it without saying so)

### 1. URLs (C++ server serves, web client fetches) — all GET and HEAD
```
/mapgl/vendor/maplibre-gl.js      /mapgl/vendor/maplibre-gl.css      /mapgl/vendor/pmtiles.js
/mapgl/vendor/vibemapgl.js        /mapgl/vendor/vibemapgl-compat.js
/mapgl/vibemap-style.json
/mapgl/fonts/<Font Name With Spaces>/<start>-<end>.pbf      (URL-encoded spaces, %20)
/mapgl/icons/<name>.png
/mapgl/vibemap-basic.pmtiles      /mapgl/vibemap-relief.pmtiles       (BUNDLED, read-only dir)
/mapgl/vibemap-detail.pmtiles     (OPTIONAL, in the WRITABLE data dir; 404 when not installed)
```
- ★★★ **HTTP Range is REQUIRED for *.pmtiles** (single range `bytes=a-b`, `bytes=a-`, `bytes=-n`) →
  `206` + `Content-Range: bytes a-b/size` + `Accept-Ranges: bytes`; unsatisfiable → `416` +
  `Content-Range: bytes */size`. No Range → 200 full. The browser reads only the tiles in view.
- Content types: .js `text/javascript`, .css `text/css`, .json `application/json`, .pbf
  `application/x-protobuf`, .png `image/png`, .pmtiles `application/octet-stream`.
- Path safety: reject `..`, backslashes, absolute paths, NUL; only the listed subtrees.
- Allowed on the front door exactly where `/mapdata/v1/` is allowed today (anyone who may see the page
  may fetch its map). Cache-Control: bundled files `public, max-age=86400`; detail `no-cache` (it can
  be removed/replaced).

### 2. Where the files live
| platform | bundled dir (read-only) | writable data dir (detail pack) |
|---|---|---|
| Linux .deb | `<prefix>/lib/vibeserver/mapgl/` (CMake install, from `build/maptiles` via `assets/mapgl` — see naming note) | `/var/lib/vibeserver/mapgl/` |
| Android (main app + Lite) | `filesDir/mapgl-bundled/` unpacked from APK assets once (marker file written LAST, version = total bytes) | `filesDir/mapgl/` (★ the main app's React Native detail download ALREADY writes `Paths.document/mapgl/vibemap-detail.pmtiles` = `filesDir/mapgl/vibemap-detail.pmtiles` — so an app user who downloaded High Detail has it served too) |
| macOS VibeServer | `<bundle>/Contents/Resources/mapgl/` if the mac packaging has a Resources step; otherwise next to the exe per the existing `vibe_mapdata.h` search order | `~/Library/Application Support/VibeServer/mapgl/` or the existing data dir |

★ Naming note: `assets/mapgl/` stores fonts as `JetBrains_Mono_Bold/` (no spaces) and scripts as
`*.js.txt` for Metro. The SERVER must expose the URL names in §1 (spaces, `.js`). The cleanest source
for installs is `directory/public/mapgl/` (plain names, already correct layout — but it has
`vibemap-style.js` instead of `.json`; take the style from `web/mapkit/vibemap-style.json`).

### 3. C++ API (new header `android/app/src/main/cpp/vibe_mapgl.h`, owned by the SERVER agent)
```cpp
namespace vibemapgl {
  void setBundleDir(const std::string&);      // read-only files (§2)
  void setDataDir(const std::string&);        // writable; detail pack lives here
  // Platform downloader: Linux uses curl (like geoip.cpp); Android installs a JNI-backed one.
  // fn(url, destPath, progress(bytesWritten,totalBytes)) -> true on success. Must be callable from a
  // worker thread; progress may be called from any thread.
  using Downloader = std::function<bool(const std::string&, const std::string&, std::function<void(int64_t,int64_t)>)>;
  void setDownloader(Downloader);
  std::string statusJson();   // {"installed":bool,"bytes":N,"downloading":bool,"written":N,"total":N,"error":"","available":bool}
  bool startDetailInstall();  // async; downloads to <data>/vibemap-detail.pmtiles.part, checks size==DETAIL_BYTES
                              // and the 7-byte "PMTiles" magic, THEN renames. Half a download is never "installed".
  bool removeDetail();
}
```
- DETAIL_URL = `https://github.com/Stuey3D/VibeSDR/releases/download/mapgl-detail-v1/vibemap-detail.pmtiles`
- DETAIL_BYTES = `177024426` (same constants as `src/services/mapglDetail.ts` — one release asset).
- `available` = a downloader is installed (false → the admin UI says why instead of a dead button —
  AGENTS.md: never show a control that cannot work).

### 4. Admin API (web client ↔ server)
- `GET /vibeserver/admin/status` gains `"mapglDetail": <statusJson()>`.
- `POST /vibeserver/admin/action` with `{"action":"mapgl-detail-install"}` / `{"action":"mapgl-detail-remove"}`.
  Add both to the maintenance-actions allow-list wherever `maps-install` is (main.cpp:2413, :3169, shim
  adminAction ~:23540) — on EVERY platform (Android too, via the JNI downloader).
- The existing `maps-install`/`maps-remove` (apt package of the OLD Leaflet detail JSON) stay as they are.

### 5. Web client (web/client/src)
Two maps exist (no map in the listening view):
- **Admin "visitors by country" map** — `admin.ts` `renderCountryMap()` (~:575), Leaflet from unpkg (`loadLeaflet` ~:548).
- **Digital spots map** — `main.ts` `openSpotsMap()` (~:11032): builds a popup page via `window.open` +
  `document.write`, Leaflet from unpkg; ★ suspected bug: the popup references `VIBEMAP_JS` as literal
  text inside the template (~:11129) → ReferenceError → no basemap. Verify and fix.
Both: load `/mapgl/vendor/*` + style JSON from the server (absolute to the server's origin — the spots
popup is `about:blank`, so use the opener's `location.origin` + base path, and the page may be served
under a path prefix like `/r/<id>/` — find how other URLs are built, e.g. `/mapdata/v1/`), create with
`VibeMapGL.create(container, {style, load, base:'<origin>/mapgl/', rangeBase:'<origin>/mapgl/', profile, center, zoom})`
(`rangeBase` streams pmtiles by Range), and use `VibeMapGLCompat.install(vm)` so the existing Leaflet
calls keep working. The compat adapter now supports: marker, divIcon, circleMarker (GPU circles),
polygon, layerGroup, fitBounds, flyTo, setView, zoomIn/Out, latLngToContainerPoint, on/once/off,
bindPopup/openPopup, control.zoom. Anything else (e.g. `L.circle` range rings in the spots map) —
ADD it to `web/mapkit/vibemapgl-compat.js` (then run `node scripts/sync-mapgl-assets.mjs`; the app and
directory copies must stay in sync — `node scripts/check-map-overlay.mjs` enforces it).
- Detail: `HEAD /mapgl/vibemap-detail.pmtiles` → 200 means use it: pass `detail:true` (vibemapgl.js
  rangeBase path handles `o.detail`).
- No WebGL 2 or any failure → keep today's Leaflet + vibemap.js path exactly as it is.
- Greyline: `vm.setNight(true)` on the spots map (Stuart wants it on digital spots too).
- **Admin maintenance section**: a "High Detail Maps" row in Stuart's wording style:
  installed → "High Detail Maps installed, space used N MB — Remove?"; not installed → "High Detail Maps
  not installed. They need about 169 MB on this server and are a one-time download — Download now?";
  downloading → progress %; error → message + Retry; `available:false` → say the server cannot download
  (no button). Poll status while downloading.
- Build: `node scripts/build-web.mjs` regenerates `android/app/src/main/cpp/vibe_web_page.h` (check the
  script for its exact outputs) — the web client is COMPILED INTO the server.

## Rules (from AGENTS.md / memory — they have bitten before)
- UK English. "servers", never "instances".
- A silent catch hides bugs: log every failure path (`console.error`), never swallow.
- Verify the ARTEFACT, not the log: after build-web, decode-check that the header contains the new code.
- Do not deploy, publish, push, or touch any real server. Do not run `expo prebuild --clean`.
- Keep the Mac's disk in mind (it filled tonight): no multi-GB temp files; clean up what you create.
- Commit your own work with clear messages (`git add` only your files; do not commit others' files).
