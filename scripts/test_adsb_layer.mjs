#!/usr/bin/env node
/* test_adsb_layer.mjs — the PURE half of web/mapkit/adsblayer.js (VibeAdsbLayer.maths): distance and
 * bearing, dead-reckoning, the icon table, the altitude text, ring spacing, the fit / centred camera,
 * the auto-zoom decisions (calm under drift, prompt when an aircraft leaves the view) and the label
 * placer. Silent: no browser, no audio, no network. Also checks the generated copies are not stale.
 *
 *   node scripts/test_adsb_layer.mjs
 */
import { readFileSync, existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const SRC = path.join(root, 'web/mapkit/adsblayer.js');
const src = readFileSync(SRC, 'utf8');
const ctx = { console, Math, Number, String, Set, Map, Array, Object, JSON, isFinite, setTimeout, clearTimeout };
ctx.globalThis = ctx;
vm.createContext(ctx);
vm.runInContext(src, ctx, { filename: SRC });
const M = ctx.VibeAdsbLayer && ctx.VibeAdsbLayer.maths;

let fails = 0, passes = 0;
function ok(cond, what) { if (cond) passes++; else { fails++; console.error('FAIL', what); } }
function near(a, b, tol, what) { ok(Math.abs(a - b) <= tol, `${what}: ${a} vs ${b} (±${tol})`); }

ok(!!M, 'VibeAdsbLayer.maths is exported');
ok(!src.includes('`'), 'no backticks in adsblayer.js (the app injects it into a template literal)');

// ── distance, bearing, destination ──
near(M.distNm(0, 0, 1, 0), 60.04, 0.05, 'one degree of latitude ≈ 60 NM');
near(M.distNm(51.4700, -0.4543, 49.0097, 2.5479), 188, 2, 'Heathrow → Charles de Gaulle ≈ 188 NM');
near(M.bearingDeg(0, 0, 0, 1), 90, 1e-6, 'due east');
near(M.bearingDeg(0, 0, 1, 0), 0, 1e-6, 'due north');
near(M.bearingDeg(52, 0, 51, 1), 148, 2, 'south-east-ish');
{
  const d = M.destination(52.29, -0.85, 135, 100);
  near(M.distNm(52.29, -0.85, d.lat, d.lon), 100, 0.01, 'destination → distance round trip');
  near(M.bearingDeg(52.29, -0.85, d.lat, d.lon), 135, 0.01, 'destination → bearing round trip');
}
ok(M.cardinal(0) === 'N' && M.cardinal(135) === 'SE' && M.cardinal(359) === 'N' && M.cardinal(-90) === 'W', 'cardinal points');
near(M.wrapLon(179, -179), -181, 1e-9, 'wrapLon unwraps across the antimeridian');

// ── dead-reckoning ──
{
  const fix = { lat: 0, lon: 0, gs: 360, track: 90, t: 0 };
  const p = M.deadReckon(fix, 10000);          // 360 kt × 10 s = 1 NM east
  near(M.distNm(0, 0, p.lat, p.lon), 1, 0.001, 'dead-reckon 10 s at 360 kt = 1 NM');
  near(p.lat, 0, 1e-9, '…due east');
  const far = M.deadReckon(fix, 600000);
  near(M.distNm(0, 0, far.lat, far.lon), 360 * M.MAX_DR_S / 3600, 0.001, 'dead-reckoning holds after MAX_DR_S');
  const still = M.deadReckon({ lat: 1, lon: 2, gs: null, track: 90, t: 0 }, 5000);
  ok(still.lat === 1 && still.lon === 2, 'no speed → no movement');
  const early = M.deadReckon(fix, -5000);
  ok(early.lat === 0 && early.lon === 0, 'never reckons backwards');
}

// ── the icon table ──
const want = { A1: 'light', A2: 'regional', A3: 'jet', A4: 'b757', A5: 'heavy', A6: 'fighter', A7: 'heli',
               B1: 'glider', B2: 'balloon', B3: 'parachute', B4: 'ultralight', B6: 'uav', B7: 'space',
               C1: 'ground', C2: 'ground', C3: 'obstacle', A0: 'jet', '': 'jet', ZZ: 'jet', a5: 'heavy' };
for (const [c, k] of Object.entries(want)) ok(M.iconFor(c) === k, `category ${JSON.stringify(c)} → ${k} (got ${M.iconFor(c)})`);
ok(M.iconFor(undefined) === 'jet', 'no category → generic jet');
for (const k of new Set(Object.values(want))) ok(ctx.VibeAdsbLayer.ICONS.includes(k), `an icon is drawn for ${k}`);
{
  const svg = ctx.VibeAdsbLayer.iconSvg('heavy', '80,255,120', 0.9, 45);
  ok(svg.startsWith('<svg') && svg.includes('rotate(45.0,20,20)'), 'iconSvg rotates about the centre');
  ok(ctx.VibeAdsbLayer.iconSvg('balloon', '1,2,3', 1, 45).includes('rotate(0,20,20)'), 'a balloon never rotates');
}

// ── altitude / vertical rate text ──
ok(M.altText(35000) === 'FL350', 'FL350');
ok(M.altText(18000) === 'FL180', 'transition altitude reads as a flight level');
ok(M.altText(3420) === '3,400 ft', 'feet below transition');
ok(M.altText('ground') === 'GND', 'on the ground');
ok(M.altText(undefined) === '—', 'unknown altitude');
ok(M.altShort(36975) === 'FL370' && M.altShort(2500) === '2500' && M.altShort(null) === '', 'label altitude');
ok(M.vsText(50) === 'level' && M.vsText(1216).startsWith('↑ +1,216') && M.vsText(-640).startsWith('↓'), 'vertical rate');
ok(M.emergencyOf({ squawk: '7700' }) === 'EMERGENCY 7700' && M.emergencyOf({ emergency: 'none' }) === '' &&
   M.emergencyOf({ emergency: 'general' }) === 'EMERGENCY' && M.emergencyOf({}) === '', 'emergency states');

// ── ring spacing ──
ok(M.ringStep(0.5, 110) === 50, '0.5 NM/px at 110 px → 50 NM rings');
ok(M.ringStep(0.02, 110) === 2, 'close in → 2 NM rings');
ok(M.ringStep(8, 110) === 1000, 'far out → 1000 NM rings');
{
  // ★ The step never shrinks as the view zooms OUT (monotonic in NM per pixel).
  let prev = 0, mono = true;
  for (let z = 12; z >= 2; z -= 0.1) { const s = M.ringStep(M.nmPerPx(52, z, 512), 110); if (s < prev) mono = false; prev = s; }
  ok(mono, 'ring step is monotonic with zoom');
}

// ── the camera ──
const rx = { lat: 52.29, lon: -0.85 };
const VIEW = { w: 800, h: 600, pad: 48, tileSize: 512, minZoom: 1.5, maxZoom: 10 };
{
  const pts = [M.destination(rx.lat, rx.lon, 0, 60), M.destination(rx.lat, rx.lon, 200, 120), M.destination(rx.lat, rx.lon, 80, 30)];
  const cam = M.cameraFor(Object.assign({ points: pts, rx, fit: false }, VIEW));
  ok(cam.center.lat === rx.lat && cam.center.lon === rx.lon, 'fit off: receiver dead centre');
  ok(!M.anyOutside(pts, cam, VIEW.w, VIEW.h, VIEW.pad - 1, 512), 'fit off: every aircraft inside the padded view');
  // ★ "just" contains: a quarter of a zoom level closer and something falls out.
  ok(M.anyOutside(pts, { center: cam.center, zoom: cam.zoom + 0.25 }, VIEW.w, VIEW.h, VIEW.pad, 512), 'fit off: the view is as close as it can be');
}
{
  // Traffic mostly to the SOUTH-EAST: the receiver ends up towards the TOP-LEFT and the zoom is closer.
  const pts = [100, 120, 140, 160].map((b, i) => M.destination(rx.lat, rx.lon, b, 60 + 25 * i));
  const fit = M.cameraFor(Object.assign({ points: pts, rx, fit: true }, VIEW));
  const centred = M.cameraFor(Object.assign({ points: pts, rx, fit: false }, VIEW));
  const o = M.screenOffset(fit, rx.lat, rx.lon, 512);
  ok(o.x < 0 && o.y < 0, `fit on: receiver in the top-left (offset ${o.x.toFixed(0)}, ${o.y.toFixed(0)})`);
  ok(fit.zoom > centred.zoom + 0.5, `fit on zooms closer than centred (${fit.zoom.toFixed(2)} vs ${centred.zoom.toFixed(2)})`);
  ok(!M.anyOutside(pts.concat([rx]), fit, VIEW.w, VIEW.h, VIEW.pad - 1, 512), 'fit on: receiver and aircraft all inside');
}
{
  const none = M.cameraFor(Object.assign({ points: [], rx, fit: true, emptyZoom: 7 }, VIEW));
  ok(none && none.zoom === 7 && none.center.lat === rx.lat, 'no aircraft: the receiver at the empty zoom');
  ok(M.cameraFor(Object.assign({ points: [], rx: null }, VIEW)) === null, 'nothing at all: no camera');
  const one = M.cameraFor(Object.assign({ points: [{ lat: rx.lat, lon: rx.lon }], rx, fit: false }, VIEW));
  ok(one.zoom === VIEW.maxZoom, 'an aircraft overhead: clamped to maxZoom, never street level');
  const noRx = M.cameraFor(Object.assign({ points: [{ lat: 10, lon: 10 }, { lat: 11, lon: 12 }], rx: null, fit: false }, VIEW));
  near(noRx.center.lat, 10.5, 0.05, 'no receiver: always fits the box');
  // Across the antimeridian: a box of 2 degrees, not 358.
  const am = M.cameraFor(Object.assign({ points: [{ lat: 0, lon: 179 }, { lat: 0, lon: -179 }], rx: null, fit: true }, VIEW));
  ok(am.zoom > 5, `antimeridian: a small box (zoom ${am.zoom.toFixed(2)})`);
}

// ── auto-zoom: prompt OUT, patient IN, calm under drift ──
{
  const az = new M.AutoZoom();
  const cur = { center: rx, zoom: 7 };
  // An aircraft appears 200 NM out: out at once.
  const pts = [M.destination(rx.lat, rx.lon, 90, 200)];
  const tgt = M.cameraFor(Object.assign({ points: pts, rx, fit: false }, VIEW));
  const out = M.anyOutside(pts, cur, VIEW.w, VIEW.h, 24, 512);
  ok(out, 'the 200 NM arrival is outside the old view');
  const g = az.decide(10000, cur, tgt, out, VIEW.w, VIEW.h, 512, false);
  ok(g && g.zoom < tgt.zoom && Math.abs(g.zoom - (tgt.zoom - az.slack)) < 1e-9, 'zooms out at once, with slack');
  ok(az.decide(10500, cur, tgt, true, VIEW.w, VIEW.h, 512, false) === null, 'never twice inside minGap');
  ok(az.decide(20000, cur, tgt, true, VIEW.w, VIEW.h, 512, true) === null, 'never while a move is running');
}
{
  // It drops off: back IN to the next furthest — only after it has stayed that way for inHold.
  const az = new M.AutoZoom();
  const cur = { center: rx, zoom: 5 };
  const tgt = M.cameraFor(Object.assign({ points: [M.destination(rx.lat, rx.lon, 10, 40)], rx, fit: false }, VIEW));
  ok(tgt.zoom > cur.zoom + az.inHyst, 'the near aircraft wants a much closer view');
  let moved = null, at = 0;
  for (let t = 0; t <= 6000 && !moved; t += 1000) { moved = az.decide(t, cur, tgt, false, VIEW.w, VIEW.h, 512, false); at = t; }
  ok(moved && at >= az.inHold, `zooms in only after inHold (at ${at} ms)`);
}
{
  // ★★ CALM: twenty aircraft drifting and jittering for five minutes, the view following them. The camera
  //    may move, but rarely — never the once-a-second nudging that reads as a nervous map.
  const az = new M.AutoZoom();
  let cur = null, moves = 0;
  const base = Array.from({ length: 20 }, (_, i) => ({ b: i * 18, d: 20 + (i * 7) % 90, gs: 250 + (i * 31) % 200, trk: (i * 47) % 360 }));
  for (let t = 0; t <= 300000; t += 1000) {
    const pts = base.map((a) => {
      const p = M.destination(rx.lat, rx.lon, a.b, a.d);
      const q = M.destination(p.lat, p.lon, a.trk, a.gs * t / 3600000);         // flying on
      return M.destination(q.lat, q.lon, (t * 7) % 360, 0.05);                   // ±0.05 NM position noise
    });
    const tgt = M.cameraFor(Object.assign({ points: pts, rx, fit: true }, VIEW));
    if (!cur) { cur = { center: tgt.center, zoom: tgt.zoom - az.slack }; continue; }
    const g = az.decide(t, cur, tgt, M.anyOutside(pts, cur, VIEW.w, VIEW.h, 24, 512), VIEW.w, VIEW.h, 512, false);
    if (g) { moves++; cur = g; }
  }
  ok(moves <= 8, `five minutes of drifting traffic moved the camera ${moves} times (≤ 8)`);
  console.log(`  (calm check: ${moves} camera moves in 300 s of drift)`);
}

// ── labels ──
{
  const items = [{ id: 'a', x: 100, y: 100, w: 50, h: 24, pri: 3 }, { id: 'b', x: 100, y: 108, w: 50, h: 24, pri: 1 },
                 { id: 'c', x: 400, y: 300, w: 50, h: 24, pri: 2 }];
  const first = M.placeLabels(items, new Set(), 800, 600, 60);
  ok(first.has('b') && !first.has('a') && first.has('c'), 'overlapping labels: the higher priority wins');
  const sticky = M.placeLabels(items, new Set(['a']), 800, 600, 60);
  ok(sticky.has('a') && !sticky.has('b'), 'a label already shown keeps its place (no flicker)');
  const edge = M.placeLabels([{ id: 'e', x: 780, y: 100, w: 50, h: 24, pri: 0 }], new Set(), 800, 600, 60);
  ok(!edge.has('e'), 'a label that would run off the edge is not drawn');
  const many = Array.from({ length: 200 }, (_, i) => ({ id: 'm' + i, x: (i % 20) * 40, y: Math.floor(i / 20) * 60 + 20, w: 30, h: 22, pri: i }));
  ok(M.placeLabels(many, new Set(), 800, 600, 60).size <= 60, 'label count capped');
}

// ── animation curves ──
{
  let lo = 1, hi = 0;
  for (let t = 0; t < 2400; t += 10) { const b = M.breath(t); lo = Math.min(lo, b); hi = Math.max(hi, b); }
  near(lo, 0.15, 0.01, 'breath floor = the HFDL pglow 0.15'); near(hi, 1, 0.01, 'breath peak');
  near(M.easeInOut(0), 0, 1e-12, 'ease starts at 0'); near(M.easeInOut(1), 1, 1e-12, 'ease ends at 1');
  ok(M.GLIDE_MS === 2200 && M.RING_MS === 1400 && M.RING_REPEATS === 3 && M.BREATHE_MS === 1200, 'the HFDL constants');
}

// ── the generated copies (scripts/gen-adsblayer-source.mjs) ──
{
  const copies = [['directory/public/mapgl/vendor/adsblayer.js', (t) => t],
                  ['web/client/src/generated/adsbLayerSource.ts', (t) => JSON.parse(t.slice(t.indexOf('= ') + 2, t.lastIndexOf(';')).replace(/<\\\/script/gi, '</script'))],
                  ['src/generated/adsbLayerSource.ts', (t) => JSON.parse(t.slice(t.indexOf('= ') + 2, t.lastIndexOf(';')).replace(/<\\\/script/gi, '</script'))]];
  for (const [rel, read] of copies) {
    const f = path.join(root, rel);
    if (!existsSync(f)) { ok(false, `${rel} missing — run node scripts/gen-adsblayer-source.mjs`); continue; }
    let got = null;
    try { got = read(readFileSync(f, 'utf8')); } catch (e) { got = null; }
    ok(got === src, `${rel} matches web/mapkit/adsblayer.js (else run node scripts/gen-adsblayer-source.mjs)`);
  }
}

console.log(`${fails ? 'FAILED' : 'ok'} — ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
