/**
 * gen-country-shapes.mjs — country outlines for the directory map, from the ONE copy we already
 * have: src/services/countryBounds.ts (Natural Earth 110m admin-0, public domain, ~1km precision,
 * itself generated). The app highlights countries from that file; the directory page now does the
 * same, from the same data, rather than fetching a world map from somewhere else or carrying a
 * second set of outlines that could disagree with the app's.
 *
 *   node scripts/gen-country-shapes.mjs           -> directory/public/country-shapes.json
 *   node scripts/gen-country-shapes.mjs --check   -> fail if the checked-in file has drifted
 *
 * ★ Rings only: the bbox in the source is for point-in-country lookup, which the page does not do.
 */
import { readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const src = await readFile(path.join(root, 'src/services/countryBounds.ts'), 'utf8');
const start = src.indexOf('= {', src.indexOf('COUNTRY_BOUNDS'));
const end = src.lastIndexOf('};');
if (start < 0 || end < 0) { console.error('countryBounds.ts: could not find the object'); process.exit(1); }
const shapes = JSON.parse(src.slice(start + 2, end + 1));

const out = {};
for (const [iso, v] of Object.entries(shapes)) if (v && Array.isArray(v.r)) out[iso] = v.r;
const json = JSON.stringify(out);

const dest = path.join(root, 'directory/public/country-shapes.json');
if (process.argv.includes('--check')) {
  const have = await readFile(dest, 'utf8').catch(() => '');
  if (have !== json) { console.error('country-shapes.json is STALE — run scripts/gen-country-shapes.mjs'); process.exit(1); }
  console.log('country-shapes.json matches countryBounds.ts');
} else {
  await writeFile(dest, json);
  console.log(`wrote ${dest} (${Object.keys(out).length} countries, ${(json.length / 1024).toFixed(0)} KB)`);
}
