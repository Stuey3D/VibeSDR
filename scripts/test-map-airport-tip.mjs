#!/usr/bin/env node
// test-map-airport-tip.mjs — THE AIRPORT NAME POPUP IS READABLE (Stuart 11-25, the Sony's RADIOSONDES page: "Cranfield
// Airport / EGTC" in faint light grey on MapLibre's white popup — "can we improve the font when hovering over airports, this
// affects the aviation map too"). The popup lives in web/mapkit/vibemapgl.js, so every GPU map shares it.
//
// A real MapLibre map (the vendored build, empty style, our sea colour) in headless Edge with --mute-audio (tests are silent),
// on a page whose own text is LIGHT GREY — the colour that leaked into the white box. A real ml.Popup with the map's class
// and VibeMapGL.airportTip's sheet and content, at each anchor. Checked, from computed styles:
//   · the panel is dark (luminance < 0.05) and the tip's visible border is the panel's colour, at every anchor;
//   · code and name each reach WCAG AA (≥ 4.5:1) against the panel — heliport code too;
//   · the code is bold and comes first, the name under it; the font is the map's monospace stack (JetBrains Mono first), ≥ 12 px;
//   · a name is text, never markup.
// OUTDIR=dir also writes before/after screenshots (before = MapLibre's default popup with the old content, as shipped).
//   node scripts/test-map-airport-tip.mjs            OUTDIR=/some/dir node scripts/test-map-airport-tip.mjs
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const EDGE = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!fs.existsSync(EDGE)) { console.log('   not run — Edge needed'); process.exit(3); }
const OUT = process.env.OUTDIR || '';
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-airport-tip-'));
process.on('exit', () => { try { fs.rmSync(tmp, { recursive: true, force: true }); } catch {} });
const u = (f) => pathToFileURL(path.join(root, f)).href;

/** mode: 'after' (the map's popup) or 'before' (MapLibre's default + the old inline content, for the screenshot). */
function page(mode, anchor) {
  return `<!doctype html><html><head><meta charset="utf-8">
<link rel="stylesheet" href="${u('web/mapkit/vendor/maplibre-gl.css')}">
<style>html,body{margin:0;height:100%;background:#0d1210;color:#b8c4bc;font:14px system-ui,sans-serif}#m{position:absolute;inset:0}
#out{display:none}</style>
<script src="${u('web/mapkit/vendor/maplibre-gl.js')}"></script>
<script src="${u('web/mapkit/vibemapgl.js')}"></script></head>
<body><div id="m"></div><pre id="out">PENDING</pre><script>
(function () {
  const out = document.getElementById('out');
  try {
    const map = new maplibregl.Map({ container: 'm', center: [-0.62, 52.07], zoom: 9, attributionControl: false, fadeDuration: 0,
      style: { version: 8, sources: { a: { type: 'geojson', data: { type: 'Feature', properties: {}, geometry: { type: 'Point', coordinates: [-0.62, 52.07] } } } },
               layers: [{ id: 'sea', type: 'background', paint: { 'background-color': '#123049' } },
                        { id: 'a', type: 'circle', source: 'a', paint: { 'circle-radius': 5, 'circle-color': '#e8b13a' } }] } });
    const T = VibeMapGL.airportTip;
    const before = ${JSON.stringify(mode)} === 'before';
    const old = (p) => {               // ★ the content as it shipped before 11-25, for the BEFORE shot only
      const code = [p.icao, p.iata].filter(Boolean).join(' / ');
      const el = document.createElement('div');
      el.style.cssText = 'font-size:12px;line-height:1.35;white-space:normal;overflow-wrap:anywhere';
      el.textContent = p.name || code;
      if (p.name && code) { const c = document.createElement('div'); c.style.opacity = '0.6'; c.textContent = code; el.appendChild(c); }
      return el;
    };
    if (!before) T.install(document);
    const mk = (props, ll, anchor) => new maplibregl.Popup({ closeButton: false, closeOnClick: false, className: 'vs-airport-tip',
        offset: 10, maxWidth: '260px', anchor: anchor || undefined })
      .setLngLat(ll).setDOMContent((before ? old : T.content)(props)).addTo(map);
    const a = mk({ name: 'Cranfield Airport', icao: 'EGTC', cls: 1 }, [-0.62, 52.07], ${JSON.stringify(anchor)});
    const h = before ? null : mk({ name: 'Bedford Hospital <b>Helipad</b>', icao: 'EGHB', cls: 3 }, [-0.70, 52.00], 'top');
    const rgb = (s) => { const m = s.match(/rgba?\\(([^)]+)\\)/); if (!m) return null; const v = m[1].split(',').map(Number); return { r: v[0], g: v[1], b: v[2], a: v.length > 3 ? v[3] : 1 }; };
    const lum = (c) => { const f = (x) => { x /= 255; return x <= 0.03928 ? x / 12.92 : Math.pow((x + 0.055) / 1.055, 2.4); }; return 0.2126 * f(c.r) + 0.7152 * f(c.g) + 0.0722 * f(c.b); };
    // The panel over the map's sea (alpha 0.96): blend, then measure.
    const over = (c, bg) => ({ r: c.r * c.a + bg.r * (1 - c.a), g: c.g * c.a + bg.g * (1 - c.a), b: c.b * c.a + bg.b * (1 - c.a), a: 1 });
    const sea = { r: 0x12, g: 0x30, b: 0x49, a: 1 };
    const ratio = (x, y) => { const L1 = lum(x), L2 = lum(y); return (Math.max(L1, L2) + 0.05) / (Math.min(L1, L2) + 0.05); };
    const report = (pop) => {
      const el = pop.getElement(), cs = (n) => getComputedStyle(n);
      const content = el.querySelector('.maplibregl-popup-content'), tipEl = el.querySelector('.maplibregl-popup-tip');
      const bg = over(rgb(cs(content).backgroundColor), sea);
      const anc = [...el.classList].find((c) => c.startsWith('maplibregl-popup-anchor-')).replace('maplibregl-popup-anchor-', '');
      const side = anc.startsWith('bottom') ? 'Top' : anc.startsWith('top') ? 'Bottom' : anc === 'left' ? 'Right' : 'Left';
      const tipC = rgb(cs(tipEl)['border' + side + 'Color']);
      const code = el.querySelector('.vs-at-code'), name = el.querySelector('.vs-at-name');
      const kids = [...content.firstElementChild.children].map((k) => k.className);
      const one = (n) => n && { color: cs(n).color, weight: cs(n).fontWeight, size: parseFloat(cs(n).fontSize), family: cs(n).fontFamily,
                                ratio: ratio(over(rgb(cs(n).color), bg), bg), text: n.textContent, html: n.innerHTML };
      return { anchor: anc, panelLum: lum(bg), panel: cs(content).backgroundColor, tip: tipC && over(tipC, sea), tipMatches: !!tipC
               && Math.abs(over(tipC, sea).r - bg.r) < 2 && Math.abs(over(tipC, sea).g - bg.g) < 2 && Math.abs(over(tipC, sea).b - bg.b) < 2,
               kids, code: one(code), name: one(name), bodyColor: cs(content).color, textRatio: ratio(over(rgb(cs(content).color), bg), bg) };
    };
    map.once('idle', () => { out.textContent = JSON.stringify({ a: report(a), h: h && report(h) }); });
    setTimeout(() => { if (out.textContent === 'PENDING') out.textContent = JSON.stringify({ a: report(a), h: h && report(h), late: true }); }, 4000);
  } catch (e) { out.textContent = 'ERROR ' + e.message; }
})();
</script></body></html>`;
}

const FLAGS = ['--headless=new', '--mute-audio', '--disable-gpu-sandbox', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
  '--allow-file-access-from-files', '--no-first-run', '--no-default-browser-check', '--hide-scrollbars', '--virtual-time-budget=6000'];
function edge(file, extra) {
  const prof = fs.mkdtempSync(path.join(tmp, 'p-'));
  const r = spawnSync(EDGE, [...FLAGS, `--user-data-dir=${prof}`, ...extra, pathToFileURL(file).href], { encoding: 'utf8', timeout: 60000, maxBuffer: 32 << 20 });
  return r.stdout || '';
}
function run(mode, anchor, shot) {
  const f = path.join(tmp, `${mode}-${anchor}.html`);
  fs.writeFileSync(f, page(mode, anchor));
  if (shot) edge(f, ['--window-size=520,380', `--screenshot=${shot}`]);
  const dom = edge(f, ['--dump-dom']);
  const m = dom.match(/<pre id="out">([^<]*)<\/pre>/);
  const txt = m ? m[1].replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"').replace(/&amp;/g, '&') : 'NO OUTPUT';
  try { return JSON.parse(txt); } catch { return { error: txt.slice(0, 200) }; }
}

console.log('airport name popup — computed colours, every anchor');
if (OUT) fs.mkdirSync(OUT, { recursive: true });
for (const anchor of ['bottom', 'top', 'left', 'right', 'bottom-left', 'top-right']) {
  const r = run('after', anchor, OUT && anchor === 'bottom' ? path.join(OUT, 'airport-tip-after.png') : '');
  if (r.error) { ok(false, `${anchor}: page ran (${r.error})`); continue; }
  const a = r.a;
  ok(a.anchor === anchor, `${anchor}: MapLibre placed it at that anchor (${a.anchor})`);
  ok(a.panelLum < 0.05, `${anchor}: panel is dark (${a.panel}, L=${a.panelLum.toFixed(3)})`);
  ok(a.tipMatches, `${anchor}: the tip is the panel's colour`);
  ok(a.code && a.code.ratio >= 4.5, `${anchor}: code ${a.code && a.code.text} ${a.code && a.code.color} ${a.code && a.code.ratio.toFixed(1)}:1 ≥ 4.5 (AA)`);
  ok(a.name && a.name.ratio >= 4.5, `${anchor}: name ${a.name && a.name.color} ${a.name && a.name.ratio.toFixed(1)}:1 ≥ 4.5 (AA)`);
  ok(a.textRatio >= 4.5, `${anchor}: the panel's own text colour ${a.bodyColor} is not the page's grey on white (${a.textRatio.toFixed(1)}:1)`);
  if (anchor !== 'bottom') continue;
  ok(a.kids[0] === 'vs-at-code' && a.kids[1] === 'vs-at-name', `code first, name under it (${a.kids.join(', ')})`);
  ok(Number(a.code.weight) >= 700 && Number(a.name.weight) < 600, `code bold (${a.code.weight}), name regular (${a.name.weight})`);
  ok(/^"?JetBrains Mono"?,/.test(a.code.family) && /monospace/.test(a.code.family), `the map's label font stack (${a.code.family})`);
  ok(a.code.size >= 12 && a.name.size >= 12, `≥ 12 px (code ${a.code.size}, name ${a.name.size})`);
  ok(a.code.text === 'EGTC' && a.name.text === 'Cranfield Airport', 'reads "EGTC" / "Cranfield Airport"');
  const h = r.h;
  ok(h && h.code.ratio >= 4.5 && /159, 179, 200/.test(h.code.color), `heliport code in the heliport label colour (${h && h.code.color}, ${h && h.code.ratio.toFixed(1)}:1)`);
  ok(h && h.name.html.includes('&lt;b&gt;') && !/<b>/.test(h.name.html), 'a name is text, never markup');
  ok(h && h.tipMatches, 'heliport (top anchor): tip matches');
}
if (OUT) {
  const b = run('before', 'bottom', path.join(OUT, 'airport-tip-before.png'));
  if (b && b.a) console.log(`   (before: panel ${b.a.panel}, text ${b.a.bodyColor} ${b.a.textRatio.toFixed(2)}:1 — the bug, for the record)`);
  console.log(`   screenshots: ${path.join(OUT, 'airport-tip-before.png')}  ${path.join(OUT, 'airport-tip-after.png')}`);
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
