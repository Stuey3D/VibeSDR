// test_ndb_layer.mjs — web/mapkit/ndblayer.js and the rings it shares with the ADS-B map (adsblayer.js): the generated
// copy is current, the bars rule matches ndb.ts, the rings are ADS-B's (nautical miles, the radar ladder, four labels a
// ring), and the layer refuses to run without the ADS-B layer (it would draw different rings). Node only; no browser.
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';
const root = join(dirname(fileURLToPath(import.meta.url)), '..');
let pass = 0, fail = 0;
const ok = (c, w) => { c ? pass++ : fail++; console.log(`${c ? 'ok  ' : 'FAIL'} ${w}`); };
const src = readFileSync(join(root, 'web/mapkit/ndblayer.js'), 'utf8');
const gen = readFileSync(join(root, 'web/client/src/generated/ndbLayerSource.ts'), 'utf8');
ok(gen.includes(JSON.stringify(src).replace(/<\/script/gi, '<\\/script')), 'web/client/src/generated/ndbLayerSource.ts matches web/mapkit/ndblayer.js (else run node scripts/gen-ndblayer-source.mjs)');
ok(!src.includes('`'), 'no backticks in ndblayer.js');
const ctx = { window: {}, console };
ctx.globalThis = ctx.window;
vm.createContext(ctx);
vm.runInContext(readFileSync(join(root, 'web/mapkit/adsblayer.js'), 'utf8'), ctx);
vm.runInContext(src, ctx);
const A = ctx.window.VibeAdsbLayer, N = ctx.window.VibeNdbLayer;
ok(!!A && !!A.shared && typeof A.shared.ringsFor === 'function', 'the ADS-B layer shares its rings');
ok(!!N && typeof N.create === 'function', 'VibeNdbLayer loads');
const bars = (s, l) => N.barsFor(s, l);
ok(bars(5, true) === 1 && bars(15, true) === 2 && bars(30, true) === 3 && bars(30, false) === 0, 'bars: <10 dB one, <20 two, else three; not heard none');
const ndbTs = readFileSync(join(root, 'web/client/src/ndb.ts'), 'utf8');
ok(/snr < 10 \? 1 : snr < 20 \? 2 : 3/.test(ndbTs), 'ndb.ts ndbBars is the same rule');
const rings = A.shared.ringsFor({ lat: 52.3, lon: -0.85 }, 7, { w: 1000, h: 700 }, { x: 0, y: 0 }, 512);
ok(rings.length >= 1 && rings.every((r) => A.maths.RING_LADDER.some((s) => r.nm % s === 0)), `rings on the radar ladder: ${rings.map((r) => r.nm).join(', ')} NM`);
ok(rings.every((r) => r.lbls.length === 4 && r.lbls.map((l) => l.a).join() === 'bottom,left,top,right'), 'four labels a ring (top, right, bottom, left)');
const d = A.maths.distNm(52.3, -0.85, rings[0].pts[0][1], rings[0].pts[0][0]);
ok(Math.abs(d - rings[0].nm) < 0.05 * rings[0].nm, `the first ring is ${rings[0].nm} NM from the receiver (${d.toFixed(2)})`);
let threw = '';
const ctx2 = { window: {}, console, document: {} }; ctx2.globalThis = ctx2.window; vm.createContext(ctx2);
vm.runInContext(src, ctx2);
try { ctx2.window.VibeNdbLayer.create({}, {}); } catch (e) { threw = String(e.message || e); }
ok(/adsblayer/.test(threw), `refuses without the ADS-B layer: ${threw}`);
console.log(`${fail ? 'FAILED' : 'ok'} — ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
