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
ok(M.altText(35000) === '35,000 ft', 'feet, always (Stuart 2026-10-24) — no flight levels');
ok(M.altText(12240) === '12,200 ft', 'to the nearest hundred, with the thousands comma');
ok(M.altText(3420) === '3,400 ft', 'low altitude in feet too');
ok(M.altText('ground') === 'GND', 'on the ground');
ok(M.altText(undefined) === '—', 'unknown altitude');
ok(M.altShort(36975) === '37,000 ft' && M.altShort(2500) === '2,500 ft' && M.altShort(null) === '', 'label altitude = the list\'s format');
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

// ── military (Stuart, 2026-10-24): blue, from the military address blocks we are sure of and military callsigns ──
{
  ok(M.isMilitary({ hex: '43c0a1' }), 'UK military block 43C000-43CFFF');
  ok(M.isMilitary({ hex: 'ae1234' }), 'US military block AE0000-AFFFFF');
  ok(!M.isMilitary({ hex: '4840d6', flight: 'KLM1023' }), 'an airliner is not military');
  ok(M.isMilitary({ hex: '400001', flight: 'RRR4951' }), 'an RAF callsign');
  ok(M.isMilitary({ hex: 'a00001', flight: 'RCH871' }), 'a US airlift callsign');
  ok(!M.isMilitary({ hex: '~43c0a1' }), 'a non-ICAO (~) address is not read as a block');
  ok(!M.isMilitary({ hex: '400001', flight: 'RRRX' }), 'a callsign must carry digits after the prefix');
  // ★★ V12 aircraft data: the server's mil:1 wins (its block table + the Mictronics flag), else the rule above
  ok(M.isMilitary({ hex: '3f1234', mil: 1 }), 'the server says military (a German block we do not list)');
  ok(!M.isMilitary({ hex: '3f1234' }), '…and without the server it is not');
}
{
  // ── V12 aircraft data in the card: the small pure helpers ──
  ok(M.engText('2J') === '2 \u00d7 jet' && M.engText('4T') === '4 \u00d7 turboprop' && M.engText('') === '' && M.engText('9Z') === '', 'engines in words');
  ok(M.flagEmoji('gb') === '\u{1F1EC}\u{1F1E7}' && M.flagEmoji('G') === '' && M.flagEmoji('<b') === '', 'flag emoji from ISO alpha-2 only');
  ok(M.routeShort({ from: 'EHAM', to: 'EGSS', fromIata: 'AMS', toIata: 'STN', plausible: true }) === 'AMS \u2192 STN', 'route short, IATA');
  ok(M.routeShort({ from: 'EHAM', to: 'EGSS', fromIata: 'AMS' }) === 'EHAM \u2192 EGSS', 'route short, ICAO when an end lacks IATA');
  ok(M.routeShort({ from: 'EHAM', to: 'EGSS', plausible: false }) === '', 'an implausible route is not shown short');
  ok(M.opBadge({ op: 'RYR' }) === 'RYR' && M.opBadge({ op: '<s>' }) === '' && M.opBadge({}) === '', 'operator badge');
  ok(M.imgKey('RYR-B738') === 'RYR-B738' && M.imgKey('../etc') === '' && M.imgKey('a/b') === '' && M.imgKey('x'.repeat(41)) === '', 'image keys are the server index\'s keys only');
  ok(M.WAKE.H === 'Heavy' && M.WAKE.J === 'Super', 'wake categories');
  ok(M.shortAirport('London Luton Airport') === 'London Luton' && M.shortAirport('Belfast International Airport') === 'Belfast'
     && M.shortAirport('RAF Lakenheath') === 'RAF Lakenheath', 'airport names shortened for the card');
  ok(M.toneKey({ hex: '43c0a1', seen_pos: 1 }) === 'military', 'military takes the blue');
  ok(M.toneKey({ hex: '43c0a1', squawk: '7700', seen_pos: 1 }) === 'alert', 'an emergency still wins over military');
  ok(M.toneKey({ hex: '43c0a1', alt_baro: 'ground' }) === 'ground', 'on the ground is still grey');
}

// ── the altitude arrow: direction from the vertical rate, colour from the altitude, fading through the spectrum ──
{
  ok(M.trendOf({ alt_baro: 20000, baro_rate: 1500 }) === '\u25B2', 'climbing = up arrow');
  ok(M.trendOf({ alt_baro: 20000, baro_rate: -900 }) === '\u25BC', 'descending = down arrow');
  ok(M.trendOf({ alt_baro: 35000, baro_rate: 64 }) === '\u25CF', 'level (within 300 ft/min) = the dot');
  ok(M.trendOf({ alt_baro: 'ground' }) === '' && M.trendOf({ flight: 'X' }) === '', 'nothing on the ground or without an altitude');
  near(M.altHue(0), 15, 0.01, 'near the ground: orange-red');
  near(M.altHue(10000), 120, 0.01, '10 000 ft: green');
  near(M.altHue(40000), 285, 0.01, 'FL400: violet');
  ok(M.altHue(15000) > 120 && M.altHue(15000) < 180, 'between the stops it fades (15 000 ft between green and cyan)');
  ok(M.altHue(60000) === 285 && M.altHue(-50) === 15, 'clamped at both ends');
}

// ── ONE ALTITUDE COLOUR, THREE READERS: the layer, the web client's list and the app's list must agree ──
{
  for (const rel of ['web/client/src/adsb.ts', 'src/services/adsbList.ts']) {
    const t = readFileSync(path.join(root, rel), 'utf8');
    const m = t.match(/ALT_HUES[^=]*=\s*(\[\[[^;]*\]\])/);
    ok(!!m && JSON.stringify(JSON.parse(m[1])) === JSON.stringify(M.ALT_HUES), `${rel} uses the layer's altitude colours`);
  }
}

// ── ONE MILITARY RULE, THREE READERS ──
{
  const lay = readFileSync(path.join(root, 'web/mapkit/adsblayer.js'), 'utf8');
  const blocks = (t) => (t.match(/MIL_BLOCKS[^=]*=\s*(\[\[[^;]*\]\])/) || [])[1];
  const calls = (t) => (t.match(/MIL_CALLS\s*=\s*(\/[^\n;]*\/)/) || [])[1];
  for (const rel of ['web/client/src/adsb.ts', 'src/services/adsbList.ts']) {
    const t = readFileSync(path.join(root, rel), 'utf8');
    ok(blocks(t) === blocks(lay) && !!blocks(lay), `${rel} uses the layer's military address blocks`);
    ok(calls(t) === calls(lay) && !!calls(lay), `${rel} uses the layer's military callsigns`);
  }
}

// ── the live list's corner: the cheaper inset, and the camera kept clear of it ──
{
  const wide = M.insetFor({ corner: 'bottom-left', w: 420, h: 340 }, 2000, 1100);
  ok(wide.l === 420 && wide.b === 0, 'wide screen: a left margin costs less than a bottom one');
  const phone = M.insetFor({ corner: 'bottom-left', w: 360, h: 260 }, 390, 780);
  ok(phone.b === 260 && phone.l === 0, 'phone: a bottom margin costs less than a left one');
  const none = M.insetFor(null, 800, 600);
  ok(!none.l && !none.r && !none.t && !none.b, 'no obstacle, no inset');
  const capped = M.insetFor({ corner: 'bottom-left', w: 2000, h: 2000 }, 800, 600);
  ok(capped.l <= 480 && capped.b <= 360, 'an inset never takes more than 60 % of the view');
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
