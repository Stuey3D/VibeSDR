#!/usr/bin/env node
/* ══════════════════════════════════════════════════════════════════════════════════════════════
 * gen-mapchrome-source.mjs — ★★★ b97 THE MAP CARD SYSTEM for the pages that are not the radio page.
 *
 *   node scripts/gen-mapchrome-source.mjs          (write)
 *   node scripts/gen-mapchrome-source.mjs --check  (fail if a copy is stale — run-tests.sh; build-web.mjs writes it first)
 *
 * ★★ ONE SOURCE: web/client/src/mapChrome.ts + mapChromeApi.ts (the radio page's maps run them as modules). This bundles
 *    web/client/src/mapChromeStandalone.ts into ONE plain script, window.VibeMapChrome, for:
 *       the AMATEUR / spots map's window — web/client/src/generated/mapChromeSource.ts (MAP_CHROME_JS, a STRING it inlines)
 *       the directory's COMMUNITY MAP   — directory/public/map-chrome.js (a static file, like mapgl/vendor/tracklayer.js)
 * ★ index.html's CSS the chrome leans on (the edge dock, the controller highlight, the view's slots) comes with it
 *   (vibe:pagecss). The radio page's own machinery a standalone page cannot have is STUBBED (STUBS below): these maps
 *   bring their own map (mountMapChrome's hostMap), so TrackView never loads its layer sources or probes for a GPU map.
 * ══════════════════════════════════════════════════════════════════════════════════════════════ */
import { build } from 'esbuild';
import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');

/** index.html's rules the chrome needs: every top-level rule (or @media / @keyframes block) naming one of these. */
const CSS_NEEDS = ['.edgeDock', '.trkView', '.gpFocus', 'hBorderPulse'];
function pageCss() {
  const html = readFileSync(join(root, 'web/client/index.html'), 'utf8');
  const out = [];
  for (const m of html.matchAll(/<style[^>]*>([\s\S]*?)<\/style>/g)) {
    const css = m[1].replace(/\/\*[\s\S]*?\*\//g, '');
    let depth = 0, start = 0;
    for (let i = 0; i < css.length; i++) {
      const c = css[i];
      if (c === '{') depth++;
      else if (c === '}') {
        depth--;
        if (depth === 0) {
          const rule = css.slice(start, i + 1).trim();
          start = i + 1;
          if (!CSS_NEEDS.some((k) => rule.includes(k))) continue;
          // an @media block: keep only its inner rules that name one of ours
          if (/^@media/.test(rule)) {
            const head = rule.slice(0, rule.indexOf('{') + 1), body = rule.slice(rule.indexOf('{') + 1, -1);
            const inner = body.split('}').map((r) => r.trim()).filter((r) => r && CSS_NEEDS.some((k) => r.includes(k))).map((r) => r + '}');
            if (inner.length) out.push(head + inner.join('') + '}');
          } else out.push(rule);
        }
      }
    }
  }
  if (!out.some((r) => r.includes('.edgeDock .hCard'))) throw new Error('gen-mapchrome-source: index.html has no .edgeDock rules — the chips would be unstyled');
  return out.join('\n').replace(/\s*\n\s*/g, '\n');
}

/** Modules a standalone page cannot have → stand-ins (only these names are imported from them by the chrome). */
const STUBS = {
  // ★★ 11-28 NO './gamepad' STUB any more: the REAL controller layer (gamepad.ts) is bundled — mapChromeStandalone's
  //    initMapPad drives the community map with the pad and the keyboard, exactly as the radio maps are driven.
  './chunkCache': "export function loadVibemapSource(){return Promise.reject(new Error('no vibemap here'))}",
  './leafletAsset': 'export async function loadLeafletInto(){return false}',
  './mapgl': 'export async function probeMapGL(){return null} export async function loadMapGLScripts(){return false} export function mapglLoad(){return Promise.resolve(null)}',
  './origin': 'export function dataUrl(p){return p} export function dataOrigin(){return ""} export function assetOrigin(){return ""}',
  './generated/trackLayerSource': "export const TRACK_LAYER_JS=''",
  './generated/adsbLayerSource': "export const ADSB_LAYER_JS=''",
  './generated/ndbLayerSource': "export const NDB_LAYER_JS=''",
  './generated/vibemapSource': "export const VIBEMAP_JS=''",
};
const plugin = {
  name: 'mapchrome-standalone',
  setup(b) {
    b.onResolve({ filter: /^vibe:pagecss$/ }, () => ({ path: 'pagecss', namespace: 'vibe' }));
    b.onLoad({ filter: /.*/, namespace: 'vibe' }, () => ({ contents: 'export default ' + JSON.stringify(pageCss()) + ';', loader: 'js' }));
    b.onResolve({ filter: /^\.\/(chunkCache|leafletAsset|mapgl|origin|generated\/\w+)$/ }, (a) => (a.importer.includes('web/client/src') && STUBS[a.path] ? { path: a.path, namespace: 'stub' } : undefined));
    b.onLoad({ filter: /.*/, namespace: 'stub' }, (a) => ({ contents: STUBS[a.path], loader: 'js' }));
  },
};
const r = await build({ entryPoints: [join(root, 'web/client/src/mapChromeStandalone.ts')], bundle: true, format: 'iife', globalName: 'VibeMapChrome',
  minify: true, target: 'es2019', write: false, platform: 'browser', logLevel: 'warning', plugins: [plugin], legalComments: 'none' });
const js = '/* GENERATED by scripts/gen-mapchrome-source.mjs from web/client/src/mapChrome.ts + mapChromeApi.ts (mapChromeStandalone.ts) — DO NOT EDIT.\n'
  + ' * The VibeSDR map card system for a page of its own (window.VibeMapChrome). GPL-3.0, as the rest of VibeSDR. */\n' + r.outputFiles[0].text;
const literal = JSON.stringify(js).replace(/<\/script/gi, '<\\/script').replace(/<!--/g, '<\\!--');
const outs = [
  ['web/client/src/generated/mapChromeSource.ts', '/* GENERATED by scripts/gen-mapchrome-source.mjs — DO NOT EDIT. The amateur / spots map window inlines it. */\n'
    + '/* eslint-disable */\nexport const MAP_CHROME_JS: string = ' + literal + ';\n'],
  ['directory/public/map-chrome.js', js],
];
if (process.argv.includes('--check')) {
  const stale = outs.filter(([rel, body]) => !existsSync(join(root, rel)) || readFileSync(join(root, rel), 'utf8') !== body).map(([x]) => x);
  if (stale.length) { console.error(`map chrome copies are STALE — run node scripts/gen-mapchrome-source.mjs: ${stale.join(', ')}`); process.exit(1); }
  console.log('  ok  the map chrome\'s standalone copies match mapChrome.ts / mapChromeApi.ts');
  process.exit(0);
}
for (const [rel, body] of outs) {
  const out = join(root, rel);
  mkdirSync(dirname(out), { recursive: true });
  if (existsSync(out) && readFileSync(out, 'utf8') === body) continue;
  writeFileSync(out, body);
  console.log(`wrote ${rel} (${(body.length / 1024).toFixed(1)} KB)`);
}
