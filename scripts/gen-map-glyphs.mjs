#!/usr/bin/env node
/* ══════════════════════════════════════════════════════════════════════════════════════════════
 * gen-map-glyphs.mjs — the GPU map's label fonts, as MapLibre glyph PBFs, bundled so labels draw
 * OFFLINE.   node scripts/gen-map-glyphs.mjs  ->  build/maptiles/fonts/<Font Name>/<start>-<end>.pbf
 *
 * ★★ MONOSPACE, BECAUSE TODAY'S MAP IS. vibemap.js labels in ui-monospace / SF Mono / Menlo — part of
 *    the product's terminal look. Apple's fonts may not be redistributed, so the bundled face is
 *    JetBrains Mono (SIL OFL 1.1 — assets/mapfonts/OFL-JetBrainsMono.txt): close in character, with
 *    Regular / Bold / Italic and Latin (incl. Vietnamese), Cyrillic and Greek. CJK and other scripts
 *    are drawn by the device's own font via MapLibre's local-ideograph fallback.
 * ★ Only the 256-codepoint ranges the font actually covers are written; MapLibre asks for others and
 *   gets a 404, which it treats as "no glyphs here" — never a hang.
 * Needs fontnik (npm i -D fontnik; a native module).
 * ══════════════════════════════════════════════════════════════════════════════════════════════ */
import { readFileSync, writeFileSync, mkdirSync, rmSync, readdirSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import path from 'node:path';
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const fontnik = require('fontnik');

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const OUT = path.join(root, 'build/maptiles/fonts');
const FONTS = [['JetBrainsMono-Regular.ttf', 'JetBrains Mono Regular'],
               ['JetBrainsMono-Bold.ttf',    'JetBrains Mono Bold'],
               ['JetBrainsMono-Italic.ttf',  'JetBrains Mono Italic']];
const p = (fn, ...a) => new Promise((res, rej) => fn(...a, (e, r) => (e ? rej(e) : res(r))));

rmSync(OUT, { recursive: true, force: true });
for (const [file, name] of FONTS) {
  const buf = readFileSync(path.join(root, 'assets/mapfonts', file));
  const [face] = await p(fontnik.load, buf);
  // ★ MapLibre glyph ranges stop at U+FFFF; anything above (a few symbols in this font) is unreachable.
  const starts = [...new Set(face.points.filter((cp) => cp < 65536).map((cp) => cp - (cp % 256)))].sort((a, b) => a - b);
  const dir = path.join(OUT, name); mkdirSync(dir, { recursive: true });
  let bytes = 0;
  for (const start of starts) {
    const pbf = await p(fontnik.range, { font: buf, start, end: start + 255 });
    writeFileSync(path.join(dir, `${start}-${start + 255}.pbf`), pbf); bytes += pbf.length;
  }
  console.log(`${name}: ${face.points.length} glyphs in ${starts.length} ranges, ${(bytes / 1024).toFixed(0)} kB`);
}

/* ── Icons: assets/mapicons/*.svg -> build/maptiles/icons/<name>.png at 2x (rsvg-convert; brew install
 *  librsvg). White on transparent: the style tints them (SDF), so one image serves every colour. ── */
{
  const src = path.join(root, 'assets/mapicons'), out = path.join(root, 'build/maptiles/icons');
  rmSync(out, { recursive: true, force: true }); mkdirSync(out, { recursive: true });
  for (const f of readdirSync(src).filter((x) => x.endsWith('.svg'))) {
    execFileSync('rsvg-convert', ['-z', '2', '-o', path.join(out, f.replace('.svg', '.png')), path.join(src, f)]);
  }
  console.log('icons:', readdirSync(out).join(', '));
}
