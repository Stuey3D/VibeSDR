#!/usr/bin/env node
/**
 * check-servers-menu-layer.mjs — the open Servers menu is the top layer on the SDR screen.
 *
 * ★★★ WHY THIS EXISTS. Twice the ‹ Servers menu opened UNDER the DAB box (2026-10-30, and again in beta93 on
 *   2026-11-25: "I can't exit the server when DAB is running"). The first fix put a high zIndex on the menu's ROWS,
 *   which are children of the chip's own full-screen root — and zIndex / Android elevation only order a view among
 *   its siblings, so it did nothing against the boxes. This holds the two things that actually matter:
 *
 *   1. The open layer is applied to ServersChip's ROOT view (the one SDRScreen stacks against the boxes), and the
 *      outside-tap backdrop is inside that root (so an outside tap closes the menu, never reaches a box).
 *   2. SDRScreen renders the chip as a DIRECT child of the screen's root view (only PanelBoundary, a fragment, in
 *      between), so that band competes with the boxes — inside a wrapper it would be confined to that wrapper again.
 *   3. Its zIndex AND elevation beat every other zIndex / elevation in the app's screens and components, except the
 *      few layers that must draw over an open menu (the tour, the keyboard focus ring, the controller notice, the
 *      debug perf overlay).
 *
 * Usage: node scripts/check-servers-menu-layer.mjs      (exit 0 = holds)
 */
import { readFileSync, readdirSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let failed = 0;
const fail = (m) => { console.error(`  FAIL  ${m}`); failed++; };
const ok = (m) => console.log(`  ok    ${m}`);

const chipPath = path.join(root, 'src/components/ServersChip.tsx');
const chip = readFileSync(chipPath, 'utf8');

const band = chip.match(/export const TOP_MENU_LAYER = \{ zIndex: (\d+), elevation: (\d+) \}/);
if (!band) { fail('TOP_MENU_LAYER { zIndex, elevation } not found in ServersChip.tsx'); process.exit(1); }
const Z = Number(band[1]), E = Number(band[2]);
ok(`TOP_MENU_LAYER = zIndex ${Z}, elevation ${E}`);

// 1. The component's first returned view carries the band while open.
const ret = chip.indexOf('\n  return (');
const firstView = chip.slice(ret).match(/<View\b[^>]*>/s)?.[0] ?? '';
if (/expanded && TOP_MENU_LAYER/.test(firstView)) ok('the band is on the ROOT view, while expanded');
else fail(`the root view does not carry \`expanded && TOP_MENU_LAYER\`: ${firstView.replace(/\s+/g, ' ')}`);
const body = chip.slice(ret);
const backdrop = body.indexOf('accessibilityLabel="Close server menu"');
if (backdrop > 0 && body.indexOf('onPress={collapse}') < backdrop) ok('the outside-tap backdrop is inside the root and closes the menu');
else fail('outside-tap backdrop (onPress={collapse}) not found inside the root');
// The band on a CHILD is the 10-30 mistake: harmless, but it reads as if it were the fix.
const uses = [...chip.matchAll(/(?:&&|\[|,|\()\s*TOP_MENU_LAYER\b/g)].length;
if (uses === 1) ok('the band is applied once — to the root');
else fail(`TOP_MENU_LAYER is applied ${uses} times — expected the root only`);

// 2. SDRScreen mounts it directly under the root view.
{
  const ts = createRequire(import.meta.url)('typescript');
  const sdrPath = path.join(root, 'src/screens/SDRScreen.tsx');
  const sf = ts.createSourceFile(sdrPath, readFileSync(sdrPath, 'utf8'), ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX);
  const found = [];
  const walk = (n, anc) => {
    if ((ts.isJsxOpeningElement(n) || ts.isJsxSelfClosingElement(n)) && n.tagName.getText() === 'ServersChip') found.push(anc.join(' > '));
    const next = ts.isJsxElement(n) ? anc.concat(n.openingElement.tagName.getText()) : anc;
    ts.forEachChild(n, (c) => walk(c, next));
  };
  walk(sf, []);
  if (found.length === 1 && found[0] === 'View > PanelBoundary') ok('SDRScreen mounts ServersChip directly under its root view');
  else fail(`ServersChip's JSX ancestors in SDRScreen are [${found.join(' | ')}] — expected exactly "View > PanelBoundary"`);
}

// 3. Nothing else on the screen outranks it, bar the allowed few.
const ABOVE_OK = new Set(['Coachmark.tsx', 'ScreenNav.tsx', 'PadNotice.tsx', 'PerfOverlay.tsx']);
const files = [
  ...readdirSync(path.join(root, 'src/components')).filter((f) => f.endsWith('.tsx')).map((f) => path.join(root, 'src/components', f)),
  path.join(root, 'src/screens/SDRScreen.tsx'),
];
let worstZ = 0, worstE = 0;
for (const f of files) {
  const base = path.basename(f);
  if (base === 'ServersChip.tsx' || ABOVE_OK.has(base)) continue;
  const src = readFileSync(f, 'utf8');
  for (const m of src.matchAll(/\b(zIndex|elevation):\s*(\d+)/g)) {
    const v = Number(m[2]);
    const line = src.slice(0, m.index).split('\n').length;
    if (m[1] === 'zIndex') { worstZ = Math.max(worstZ, v); if (v >= Z) fail(`${base}:${line} zIndex ${v} ≥ the Servers menu's ${Z}`); }
    else                   { worstE = Math.max(worstE, v); if (v >= E) fail(`${base}:${line} elevation ${v} ≥ the Servers menu's ${E}`); }
  }
}
ok(`highest other zIndex ${worstZ} < ${Z}; highest other elevation ${worstE} < ${E}`);

if (failed) { console.error(`check-servers-menu-layer: ${failed} failure(s)`); process.exit(1); }
console.log('check-servers-menu-layer: all good');
