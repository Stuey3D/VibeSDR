#!/usr/bin/env node
/* ══════════════════════════════════════════════════════════════════════════════════════════════
 * sync-mapgl-assets.mjs — put the GPU map's bundled files where the APP loads them from.
 *
 *   node scripts/gen-map-tiles.mjs && node scripts/gen-map-glyphs.mjs && node scripts/sync-mapgl-assets.mjs
 *
 * Copies build/maptiles/{vibemap-basic.pmtiles, vibemap-relief.pmtiles, fonts/, icons/} and
 * web/mapkit/vibemap-style.json into assets/mapgl/, and writes src/generated/mapglAssets.ts — one
 * require() per file with the path it must have on disk (mapglPack.ts copies them there once).
 * ★ BINARY FILES AS NATIVE ASSETS, not text in the JS bundle: the JSON pack rides in the bundle as a
 *   15.5 MB string (mapdataBundle.ts), which works for JSON; tiles, WebP and glyph PBFs are binary and
 *   would need base64 (+33 %) and bloat the bundle. Metro ships these as ordinary assets instead.
 * ★ The DETAIL pack (169 MB) is NOT bundled — it is the optional download, exactly as today.
 *
 *   node scripts/sync-mapgl-assets.mjs --scripts
 *
 * ★★ --scripts: after an edit to web/mapkit/vibemapgl.js / vibemapgl-compat.js (or the vendored MapLibre /
 *    pmtiles), re-copy ONLY the scripts and rewrite the manifest — no build/maptiles needed. A hand copy of
 *    the .txt is NOT enough: the manifest's version is what tells an installed app to take the new file
 *    (scripts/lib/mapgl-manifest.mjs has the 2026-10-27 story — the spots map's mode letters).
 * ══════════════════════════════════════════════════════════════════════════════════════════════ */
import { cpSync, rmSync, mkdirSync, readdirSync, statSync, writeFileSync, existsSync, readFileSync } from 'node:fs';
import path from 'node:path';
import { mapglManifest } from './lib/mapgl-manifest.mjs';

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const BUILD = path.join(root, 'build/maptiles'), DEST = path.join(root, 'assets/mapgl');
const DIR_OUT = path.join(root, 'directory/public/mapgl');
const SCRIPTS_ONLY = process.argv.includes('--scripts');
const APP_SCRIPTS = [['web/mapkit/vendor/maplibre-gl.js', 'maplibre-gl.js'], ['web/mapkit/vendor/maplibre-gl.css', 'maplibre-gl.css'],
                     ['web/mapkit/vendor/pmtiles.js', 'pmtiles.js'], ['web/mapkit/vibemapgl.js', 'vibemapgl.js'],
                     ['web/mapkit/vibemapgl-compat.js', 'vibemapgl-compat.js']];
const DIR_SCRIPTS = [...APP_SCRIPTS,
                     // ★ The ADS-B layer (V12) — gen-adsblayer-source.mjs writes it here too; this wipe must not lose it.
                     ['web/mapkit/adsblayer.js', 'adsblayer.js'],
                     // ★ …and the ship/station layer (V12 AIS + APRS) — gen-tracklayer-source.mjs writes it here too.
                     ['web/mapkit/tracklayer.js', 'tracklayer.js'],
                     ['web/mapkit/vendor/LICENSE-maplibre-gl.txt', 'LICENSE-maplibre-gl.txt'], ['web/mapkit/vendor/LICENSE-pmtiles.txt', 'LICENSE-pmtiles.txt']];
function writeManifest() {
  const m = mapglManifest(DEST);
  writeFileSync(path.join(root, 'src/generated/mapglAssets.ts'), m.text);
  console.log(`assets/mapgl: ${m.count} files, ${(m.total / 1048576).toFixed(1)} MB, version ${m.version} -> src/generated/mapglAssets.ts`);
}
if (SCRIPTS_ONLY) {
  for (const [src, name] of APP_SCRIPTS) cpSync(path.join(root, src), path.join(DEST, 'vendor', name + '.txt'));
  for (const [src, name] of DIR_SCRIPTS)
    if (existsSync(path.join(root, src))) cpSync(path.join(root, src), path.join(DIR_OUT, 'vendor', name));
  writeManifest();
  process.exit(0);
}
for (const need of ['vibemap-basic.pmtiles', 'vibemap-relief.pmtiles', 'vibemap-runways.pmtiles', 'fonts', 'icons'])
  if (!existsSync(path.join(BUILD, need))) { console.error(`missing build/maptiles/${need} — run gen-map-tiles / gen-map-glyphs first`); process.exit(1); }
rmSync(DEST, { recursive: true, force: true }); mkdirSync(DEST, { recursive: true });
for (const f of ['vibemap-basic.pmtiles', 'vibemap-relief.pmtiles', 'vibemap-runways.pmtiles', 'icons']) cpSync(path.join(BUILD, f), path.join(DEST, f), { recursive: true });
/* ★★ FONT FOLDERS WITHOUT SPACES IN THE BUNDLE. MapLibre needs them on disk as the font's own name
 *  ("JetBrains Mono Bold/0-255.pbf"), but spaces in BUNDLED asset paths are a known React Native
 *  packaging hazard. So the asset is stored as JetBrains_Mono_Bold/…, and the manifest names the spaced
 *  path mapglPack.ts writes it to. */
for (const d of readdirSync(path.join(BUILD, 'fonts'))) cpSync(path.join(BUILD, 'fonts', d), path.join(DEST, 'fonts', d.replace(/ /g, '_')), { recursive: true });
cpSync(path.join(root, 'web/mapkit/vibemap-style.json'), path.join(DEST, 'vibemap-style.json'));
/* ★★ THE PAGE'S SCRIPTS TRAVEL AS ASSETS TOO — MapLibre GL JS, pmtiles, and our vibemapgl.js + compat.
 *  The map page lives on disk and loads them beside it (offline; today's Leaflet comes from unpkg.com).
 *  Metro would treat a .js file as APP SOURCE and try to bundle it, so they are stored with a .txt
 *  suffix and the manifest names their real path; mapglPack.ts writes them under that name. */
mkdirSync(path.join(DEST, 'vendor'), { recursive: true });
for (const [src, name] of APP_SCRIPTS) cpSync(path.join(root, src), path.join(DEST, 'vendor', name + '.txt'));

writeManifest();

/* ══ THE DIRECTORY: the same files, served from directory/public/mapgl/ ═══════════════════════════
 * ★★ Stuart, 2026-09-27: "get that etopo on the directory too" — the directory drew the relief on the
 *    old Leaflet renderer, under the crude 1:110M outlines, with seams between its relief images. It
 *    now draws the app's GPU map: same style, same packs, same relief.
 * ★ Plain names here (a browser has no Metro), and the style as a SCRIPT (window.VM_STYLE) because the
 *   page creates its map synchronously — the same reason the app's page embeds it.
 * ★ The packs are fetched by HTTP RANGE — only the tiles in view — which the directory Worker answers
 *   from Cloudflare's edge cache (servePmtiles in directory/src/index.js).
 * ✗ The DETAIL pack is not here: 169 MB is over Workers' 25 MiB per-file limit. Close in, the
 *   directory over-zooms the basic pack, as the app does without the download.
 * ★★ RUN BEFORE `wrangler deploy` in directory/ — a stale copy deploys as happily as a fresh one. */
rmSync(DIR_OUT, { recursive: true, force: true }); mkdirSync(path.join(DIR_OUT, 'vendor'), { recursive: true });
for (const f of ['vibemap-basic.pmtiles', 'vibemap-relief.pmtiles', 'vibemap-runways.pmtiles', 'icons', 'fonts']) cpSync(path.join(BUILD, f), path.join(DIR_OUT, f), { recursive: true });
for (const [src, name] of DIR_SCRIPTS)
  if (existsSync(path.join(root, src))) cpSync(path.join(root, src), path.join(DIR_OUT, 'vendor', name));
writeFileSync(path.join(DIR_OUT, 'vibemap-style.js'),
  '/* GENERATED by scripts/sync-mapgl-assets.mjs from web/mapkit/vibemap-style.json — DO NOT EDIT. */\nwindow.VM_STYLE = '
  + JSON.stringify(JSON.parse(require_json(path.join(root, 'web/mapkit/vibemap-style.json')))) + ';\n');
function require_json(f) { return readFileSync(f, 'utf8'); }
let dirBytes = 0;
(function walk(d) { for (const n of readdirSync(d)) { const f = path.join(d, n); if (statSync(f).isDirectory()) walk(f); else dirBytes += statSync(f).size; } })(DIR_OUT);
console.log(`directory/public/mapgl: ${(dirBytes / 1048576).toFixed(1)} MB`);
