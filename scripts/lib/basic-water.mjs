/* ══════════════════════════════════════════════════════════════════════════════════════════════
 * basic-water.mjs — put the coarse river areas INTO the basic pack (beta96, Stuart 11-26).
 *
 * ★★★ WHY. Without High Detail Maps the only water up a river was a Natural Earth centre line, so the AIS ships at
 *     Rotterdam (tester Listen-Amateur-VLD: 600 ships, all "on land"), London, Antwerp and Saigon sat on land. Stuart
 *     approved the measured option from docs/v12/BASIC-MAP-WATER.md: Overture river/canal/tidal/dock areas >= 200 m,
 *     z5–8 (+~7 MB), in the BASIC pack, so every server and the app draws them without the 250 MB download.
 *
 * ★★ ONE PACK, NOT A SIDECAR. The water pack (gen-river-areas.mjs --basic, one layer `riverareas`, tiles z5–8) is
 *    merged into vibemap-basic.pmtiles with tile-join. A z5–7 tile gains one layer; z8 tiles hold only the rivers. Every
 *    host already fetches, bundles, whitelists and caches that one file (vibe_mapgl.h resolve(), CMake install, the Mac
 *    app, the APK gradle task, the app manifest), so nothing new has to travel and no host can end up with half of it.
 *    The style reads the rivers through their own source on the same pack (vibemapgl.js prepareStyle, vs-basic-rivers,
 *    maxzoom 8); the basic source stays maxzoom 6, so the land never asks for the empty z7/z8 tiles.
 *
 * ★ tile-join re-encodes each tile but keeps every feature and geometry as it was; check-basic-water (below) and the
 *   commit's decode comparison are the proof. tile-join is part of tippecanoe (brew install tippecanoe).
 * ══════════════════════════════════════════════════════════════════════════════════════════════ */
import { execFileSync } from 'node:child_process';
import { renameSync, rmSync, statSync } from 'node:fs';

export const BASIC_WATER_LAYER = 'riverareas';
export const BASIC_WATER_MIN_WIDTH_M = 200, BASIC_WATER_MINZ = 5, BASIC_WATER_MAXZ = 8;
export const BASIC_WATER_ATTRIBUTION = '© OpenStreetMap contributors (rivers via Overture Maps) · ODbL';

/** The vector layer names in a pack's metadata. */
export function packLayers(pm) {
  const meta = JSON.parse(execFileSync('pmtiles', ['show', '--metadata', pm], { encoding: 'utf8', maxBuffer: 64 << 20 }));
  return (meta.vector_layers || []).map((l) => l.id);
}

/** basicIn (the basic pack WITHOUT river areas) + waterPack -> out. Refuses a basic pack that already has them, so the
 *  join is never applied twice. */
export function joinBasicWater(basicIn, waterPack, out) {
  if (packLayers(basicIn).includes(BASIC_WATER_LAYER)) throw new Error(`${basicIn} already carries ${BASIC_WATER_LAYER}`);
  const part = `${out}.part.pmtiles`;
  rmSync(part, { force: true });
  execFileSync('tile-join', ['-o', part, '--force', '-pk', '--quiet',
    '-n', 'VibeSDR basic', '-A', `Natural Earth · OurAirports · NGA World Port Index · ${BASIC_WATER_ATTRIBUTION}`,
    basicIn, waterPack], { stdio: 'inherit' });
  renameSync(part, out);
  return statSync(out).size;
}
