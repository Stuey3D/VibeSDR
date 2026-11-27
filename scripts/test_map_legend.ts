// test_map_legend.ts — ★★★ beta97 THE KEY INSIDE THE MAP CARD (Stuart, build 586, the HFDL map on the Mac: the page drew its
// legend at its old top-left place UNDER the MAP card). The page READS its own legend into items (LEGEND_READ_JS) and React
// Native draws them in the card's KEY fold (legendClean → KeyFold); the page's legend panel never shows on a card map. And
// the credits fold to their (i) after 5 s on every map (MapLibre's compact, Leaflet's att-fold).
//   npx tsx scripts/test_map_legend.ts
// The fixture is the reader's real output, run in headless Edge over each map's legend markup (the track maps' / HFDL
// .lg-row, the ADS-B layer's .t / .r / .bar / .ax and its All-time switch, the digital map's .lrow with counts and SVGs).
import { readFileSync } from 'node:fs';
import { legendClean, legendColour, legendSvg, LEGEND_READ_JS } from '../src/utils/mapLegend';

let pass = 0, fail = 0;
const ok = (c: unknown, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const fx = JSON.parse(readFileSync(new URL('./fixtures/map_legend_read.json', import.meta.url), 'utf8'));

console.log('── the reader\'s items, cleaned ──');
const hf = legendClean(fx.hfdl);
ok(hf.length === 8 && hf[0].k === 'h' && (hf[0] as any).text === 'AIRCRAFT AGE', 'HFDL: AIRCRAFT AGE heading first (the scroll arrows skipped)');
ok(hf.some((x) => x.k === 'row' && x.text === 'no sig' && x.dot === 'rgba(120, 120, 140, 0.6)'), 'HFDL: strong / medium / weak / NO SIG with their dots');
const ad = legendClean(fx.adsb);
ok(ad.some((x) => x.k === 'row' && x.glyph === '✈' && x.gc === 'rgb(255, 80, 60)' && x.text === 'Emergency'), 'ADS-B: the aircraft glyph in its colour');
ok(ad.some((x) => x.k === 'bar' && x.stops.length === 3) && ad.some((x) => x.k === 'axis' && x.labels.join() === '0,10k,20k,30k,40k ft'), 'ADS-B: the altitude bar and its axis');
ok(!ad.some((x) => x.k === 'h' && x.text === 'ADS-B RANGE'), 'ADS-B: a hidden part (the range key while RANGE is off) is not read');
ok(ad.some((x) => x.k === 'check' && x.i === 0 && !x.on), 'ADS-B: the All-time outline switch, pressed back to the page (__legCheck)');
const dg = legendClean(fx.digi);
ok(dg.some((x) => x.k === 'row' && x.text === 'FT8' && x.letter === '8' && x.n === '1,204' && x.sq), 'spots: the lettered mode mark and its count');
ok(dg.some((x) => x.k === 'row' && !!x.svg && x.text === 'Station'), 'spots: the APRS symbol as the page drew it (SVG)');

console.log('── nothing from the page is trusted ──');
ok(legendColour('url(javascript:x)') === undefined && legendColour('rgba(0, 0, 0, 0)') === undefined && legendColour('#4FC3F7') === '#4FC3F7', 'colours: rgb / hsl / #hex only; transparent = none');
ok(legendSvg('<svg onload="x()"></svg>') === undefined && legendSvg('<svg><script>x</script></svg>') === undefined
   && legendSvg('<svg><a href="https://x"></a></svg>') === undefined && !!legendSvg('<svg viewBox="0 0 2 2"><path d="M0 0"/></svg>'), 'SVGs: no scripts, events or links');
ok(legendClean('x').length === 0 && legendClean(Array(500).fill({ k: 'h', text: 'A' })).length === 120, 'not a list → nothing; bounded at 120 items');
ok(legendClean([{ k: 'check', text: 'x', i: 99 }]).length === 0, 'a switch index out of range is dropped');

console.log('── the page and the card ──');
const mo = readFileSync(new URL('../src/components/MapOverlay.tsx', import.meta.url), 'utf8');
const mc = readFileSync(new URL('../src/components/MapChrome.tsx', import.meta.url), 'utf8');
ok(!/--keyTop|--keyLeft/.test(mo), 'the page\'s legend positioning (--keyTop / --keyLeft) is gone');
ok(/#dleg,#legend\{display:none!important;\}/.test(mo) && /#legend\{display:none!important;\}/.test(mo), 'card maps: the page\'s legend panels never show');
ok(!/legEl\.classList\.toggle\('open'/.test(mo) && !/acLayer\.toggleLegend\(/.test(mo), '__cardsKey never opens the page legend or the ADS-B layer\'s');
ok((mo.match(/window\.__cardsKey=function\(on\)\{legOn=!!on;/g) ?? []).length === 2, 'both page bridges (the track maps, and HFDL / spots / CW / AMATEUR) read their legend into the card');
ok((mo.match(/keyItems: (p\.)?legendItems, onKeyCheck:/g) ?? []).length === 3, 'every MAP card (the track maps incl. AIR & SEA, spots / CW / HFDL, AMATEUR) gets the items');
ok(/\{m\.keyOn && <KeyFold items=\{m\.keyItems \?\? \[\]\}/.test(mc), 'the KEY fold sits inside the MAP card (its scroll)');
ok(/function legItems\(roots\)/.test(LEGEND_READ_JS) && mo.includes('${LEGEND_READ_JS}'), 'the reader is in the page\'s shared script (every map)');
console.log('── the credits fold to their (i) on every map ──');
ok(/setTimeout\(fold,5000\)/.test(mo) && (mo.match(/setTimeout\(fold,5000\)/g) ?? []).length === 1 && mo.indexOf('setTimeout(fold,5000)') > mo.indexOf('${LEGEND_READ_JS}') - 50 && mo.indexOf('setTimeout(fold,5000)') < mo.indexOf('${LEGEND_READ_JS}') + 2500,
   'one fold, in the shared script (not only the track host\'s)');
ok(/l\.classList\.add\('att-fold'\)/.test(mo) && /\.leaflet-control-attribution\.att-fold\{/.test(mo), 'Leaflet\'s credits fold too (att-fold); a tap on the (i) opens them');

console.log(`test_map_legend: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
