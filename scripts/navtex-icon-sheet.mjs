#!/usr/bin/env node
// navtex-icon-sheet.mjs — the NAVTEX hazard icons' comparison sheet for Stuart's review (src/services/navtexIcons.ts):
// every glyph at 16 / 24 / 32 px on dark and on light, the unlit mark on a light, a buoy and a structure, and the map
// marker as the map draws it (live and faded). Written as SVG, then PNG at 1x and 2x with rsvg-convert.
//   node --no-warnings scripts/navtex-icon-sheet.mjs [outDir]    (default ~/Desktop/navtex-icons)
import { execFileSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';
import { NAV_HAZARDS, NAV_HAZARD_COLOURS, navGlyphSvg, navHazardMarker } from '../src/services/navtexIcons.ts';

const out = process.argv[2] || join(homedir(), 'Desktop', 'navtex-icons');
mkdirSync(out, { recursive: true });

const rows = [...NAV_HAZARDS.map((h) => ({ id: h.id, words: h.words, unlit: false })),
  { id: 'light', words: 'Light — UNLIT (the mark)', unlit: true },
  { id: 'buoy', words: 'Buoy — unlit', unlit: true },
  { id: 'structure', words: 'Wind turbine — no light', unlit: true }];
const ROW = 46, TOP = 56, W = 1040;
const H = TOP + rows.length * ROW + 20;
const col = { name: 14, d16: 250, d24: 290, d32: 336, l16: 470, l24: 510, l32: 556, mk: 700, mkf: 760, mkl: 860, mklf: 920 };
const inner = (svg) => svg.replace(/^<svg /, '<svg ');
let s = `<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}" font-family="Helvetica" font-size="12">`;
s += `<rect width="${W}" height="${H}" fill="#ffffff"/>`;
s += `<rect x="236" y="0" width="200" height="${H}" fill="#14191f"/><rect x="456" y="0" width="200" height="${H}" fill="#f4f1ea"/>`;
s += `<rect x="686" y="0" width="150" height="${H}" fill="#1d2a36"/><rect x="846" y="0" width="150" height="${H}" fill="#dfe6ea"/>`;
const hd = (x, t, c = '#000') => `<text x="${x}" y="22" fill="${c}" font-weight="bold">${t}</text>`;
s += hd(14, 'NAVTEX hazard icons — v1 for review') + hd(250, 'on dark · 16 / 24 / 32', '#fff') + hd(470, 'on light · 16 / 24 / 32')
  + hd(700, 'map marker', '#fff') + hd(860, 'map marker');
s += `<text x="700" y="40" fill="#9fb0c2" font-size="10">live · faded</text><text x="860" y="40" fill="#555" font-size="10">live · faded</text>`;
s += `<text x="250" y="40" fill="#9fb0c2" font-size="10">16-unit box · stroke 1.4 · round caps</text>`;
rows.forEach((r, i) => {
  const y = TOP + i * ROW;
  const c = NAV_HAZARD_COLOURS[r.id];
  s += `<text x="${col.name}" y="${y + 20}" fill="#000">${r.words}</text><text x="${col.name}" y="${y + 34}" fill="#888" font-size="10">${r.id}${r.unlit ? ' + unlit' : ''}</text>`;
  const put = (x, px, colour, bg) => { s += `<g transform="translate(${x},${y + (32 - px) / 2 + 4})">${inner(navGlyphSvg(r.id, px, colour, r.unlit, bg))}</g>`; };
  put(col.d16, 16, c, '#14191f'); put(col.d24, 24, c, '#14191f'); put(col.d32, 32, c, '#14191f');
  put(col.l16, 16, '#1a1a1a', '#f4f1ea'); put(col.l24, 24, '#1a1a1a', '#f4f1ea'); put(col.l32, 32, '#1a1a1a', '#f4f1ea');
  for (const [x, faded] of [[col.mk, false], [col.mkf, true], [col.mkl, false], [col.mklf, true]]) {
    const m = navHazardMarker(r.id, r.unlit, faded);
    s += `<g transform="translate(${x},${y + 8})">${m.html.replace('<svg ', '<svg xmlns="http://www.w3.org/2000/svg" ').replace(/ style="opacity:([\d.]+)"/, ' opacity="$1"')}</g>`;
  }
});
s += '</svg>';
const svgPath = join(out, 'navtex-icons.svg');
writeFileSync(svgPath, s);
execFileSync('rsvg-convert', ['-o', join(out, 'navtex-icons-1x.png'), svgPath]);
execFileSync('rsvg-convert', ['-z', '2', '-o', join(out, 'navtex-icons-2x-retina.png'), svgPath]);
console.log(`wrote ${out}/navtex-icons.svg, -1x.png, -2x-retina.png (${rows.length} rows)`);
