// test_sat_layout.ts — ★★ beta96 satellite box polish (a tester's phone screenshot, AO-73, 11-26; Stuart 11-26):
// the receiver's label is never an address, SMALL draws no words (orbit, footprint, receiver dot, glyph only), the glyph
// is a square + two panel lines on the BIG bird's heading at whole pixels, the readout puts the small globe beside the
// words only where they keep their column, and the box's text backing is opaque on every faceplate.
import { globeGlyph, globeLabels, globeLayers, globeRxName, globeScene, satReadoutLayout, GLOBE_GLYPH, SAT_TEXT_MIN_W } from '../src/services/satellites';
import { decoderTextBacking, decoderTokensFor } from '../src/constants/decoderTokens';

let fail = 0, pass = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (c) pass++; else fail++; };

console.log('1. the receiver label is a name, never an address');
for (const a of ['192.168.1.2:48000', '192.168.1.2', '10.0.0.5', 'http://pi500:8073', 'pi500.local', 'pi500.local:8073', 'sdr.lan', 'localhost',
                 '[fe80::1]:8073', 'fe80::1', 'myhost:48000', 'nas.home.arpa', ' 172.16.4.9 ']) ok(globeRxName(a) === '', `"${a}" → hidden`);
for (const n of ['Stuart’s Pi 500', 'Lenovo RSP', 'M9PSY HF', 'sdr.example.org']) ok(globeRxName(n) === n, `"${n}" kept`);
{
  const g = globeScene(640, 300, { lat: 53.5, lon: -1.1 }, { lat: 55, lon: 2, altKm: 600 }, null);
  const L = globeLabels(g, '192.168.1.2:48000', 'AO-73', 10 * 640 / 420);
  ok(L.rx?.text === 'RECEIVER', 'globeLabels with an address → RECEIVER');
  ok(!JSON.stringify(L).includes('192.168'), 'no address anywhere in the labels');
}

console.log('2. SMALL draws no words');
{
  const s = globeLayers(true), b = globeLayers(false);
  ok(!s.labels && !s.bird && !s.beam && !s.arrows && !s.tick, 'SMALL: no labels, no drawn bird, no beam, no arrows, no tick');
  ok(s.orbit && s.foot && s.rx && s.glyph, 'SMALL: orbit, footprint, receiver dot and the glyph');
  ok(b.labels && b.bird && b.beam && b.arrows && b.tick && !b.glyph, 'BIG: everything as before, no glyph');
}

console.log('3. the glyph');
{
  const sc = 640 / 120;   // scene units per pixel at the small globe's 120 px
  const pts = (d: string) => [...d.matchAll(/(-?[\d.]+) (-?[\d.]+)/g)].map((m) => [Number(m[1]), Number(m[2])]);
  const onGrid = (v: number) => Math.abs(v / sc - Math.round(v / sc)) < 0.03;   // 0.1 printed precision
  for (const u of [[1, 0], [0.6, -0.8], [-0.7071, 0.7071]] as Array<[number, number]>) {
    const gl = globeGlyph({ x: 321.3, y: 97.7 }, u, sc);
    const body = pts(gl.body), pan = pts(gl.panels);
    ok(body.length === 4 && /Z$/.test(gl.body), `u=${u}: a closed four-corner body`);
    const side = (i: number, j: number) => Math.hypot(body[i][0] - body[j][0], body[i][1] - body[j][1]) / sc;
    ok(Math.abs(side(0, 1) - side(1, 2)) <= 1.5 && Math.abs(side(0, 1) - 2 * GLOBE_GLYPH.half) <= 1.5, `u=${u}: square (${side(0, 1).toFixed(1)} × ${side(1, 2).toFixed(1)} px)`);
    ok(pan.length === 4, `u=${u}: two panel lines`);
    const dx = pan[3][0] - pan[0][0], dy = pan[3][1] - pan[0][1], n = Math.hypot(dx, dy);
    ok(Math.abs(dx / n * u[0] + dy / n * u[1]) > 0.97, `u=${u}: the panels lie along the BIG bird's heading`);
    ok([...body, ...pan].every(([x, y]) => onGrid(x) && onGrid(y)), `u=${u}: every corner on a whole pixel`);
    ok(n / sc >= 2 * GLOBE_GLYPH.p1 - 2 && n / sc <= 2 * GLOBE_GLYPH.p1 + 2, `u=${u}: tip to tip ~${2 * GLOBE_GLYPH.p1} px`);
  }
}

console.log('4. the readout layout');
{
  ok(satReadoutLayout(420, false, 120) === 'beside', 'a wide box: the globe beside the words');
  ok(satReadoutLayout(300, false, 120) === 'stacked', 'a narrow box: the globe under the words, never over them');
  ok(satReadoutLayout(120 + 8 + SAT_TEXT_MIN_W, false, 120) === 'beside' && satReadoutLayout(120 + 8 + SAT_TEXT_MIN_W - 1, false, 120) === 'stacked', `the words keep ≥ ${SAT_TEXT_MIN_W} px`);
  ok(satReadoutLayout(900, true, 120) === 'stacked', 'BIG: its own row');
  ok(satReadoutLayout(0, false, 120) === 'stacked', 'not measured yet: stacked (never overlapping)');
}

console.log('5. the text backing is opaque');
{
  const opaque = (c: string) => !/rgba\(/.test(c) || /,\s*(0\.9[5-9]|1(\.0+)?)\)$/.test(c);
  for (const ch of ['default', 'silver', 'black'] as const)
    for (const tr of ['on', 'off'] as const) {
      const tk = decoderTokensFor(ch, '61,255,114', tr);
      const c = tk ? decoderTextBacking(tk) : '';
      ok(!!c && opaque(c), `${ch}, transparency ${tr}: ${c}`);
    }
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
