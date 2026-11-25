// test_skyplot_heat.ts — ★★★ beta94 THE SKY PLOT'S HEATMAP, the clients' half (src/utils/skyPlot.ts): the server's cells read
// (test/fixtures/skyplot/*-SYNTHETIC.json, written by vibeserver/tool-skyplot-fixtures.cpp from vibe_skyplot.h itself),
// the grid the same as the server's, AUTO's switch, the live pass (Stuart: "when in user mode you still see a track for the
// satellite being tracked, once the pass ends it goes to the heatmap"), the overrides, the paths, the summary line.
import fs from 'node:fs';
import {
  parseSkyPlotJson, skyCellIndex, skyCellBounds, skyCellsAddDots, skyCellsHeatDue, skyLivePass, skyViewOf, skySceneFor, skyPlotSvg,
  skyStrongest, skySummaryFor, skyHeatPaths, skyModesHtml, skyModeOf, skyPlotQuery, SKY_LIVE_SEC, type SkyDot,
} from '../src/utils/skyPlot.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const load = (f: string) => parseSkyPlotJson(JSON.parse(fs.readFileSync(new URL(`../test/fixtures/skyplot/${f}`, import.meta.url), 'utf8')))!;

const heat = load('heatmap-SYNTHETIC.json'), few = load('tracks-SYNTHETIC.json');
const T = 1795600000;
console.log('── the cells read ──');
ok(!!heat.cells && heat.cells.total === 2292 && heat.cells.az.length === 30 && heat.cells.az[0] === 120 && heat.cells.az[29] === 3, `the grid from the server: ${heat.cells?.total} cells, ${heat.cells?.az.length} bands`);
ok(heat.cells!.auto === 'heatmap' && few.cells!.auto === 'tracks' && heat.cells!.days === 30 && heat.cells!.heatCoverage === 0.3, 'AUTO\'s verdict and rule travel');
{
  let same = true;
  for (let i = 0; i < heat.cells!.total; i++) { const b = skyCellBounds(heat.cells!, i); if (skyCellIndex(heat.cells!, (b.az0 + b.az1) / 2, (b.el0 + b.el1) / 2) !== i) same = false; }
  ok(same, 'every cell\'s middle maps back to it (the server\'s grid, not a copy)');
}
ok(skyCellsHeatDue(heat.cells!) && !skyCellsHeatDue(few.cells!), 'the rule re-judged here agrees with the server');
const st = skyStrongest(heat.cells!);
ok(!!st && st.name === 'east' && st.azLo === 70 && st.azHi === 110, `strongest east (az 70–110°), as the server says: ${JSON.stringify(st)}`);
ok(skySummaryFor('auto', heat.cells, heat.dots, T + 86400) === `heatmap · 30 days · ${[...heat.cells!.cells.values()].length.toLocaleString('en-GB')} cells heard · strongest east (az 70–110°) · 363 dots`,
   `the line: ${skySummaryFor('auto', heat.cells, heat.dots, T + 86400)}`);
ok(/^363 dots · 3 satellites · \d+ grey · since /.test(skySummaryFor('auto', few.cells, few.dots, T + 86400)), `little data: the tracks' line (${skySummaryFor('auto', few.cells, few.dots, T + 86400)})`);

console.log('── AUTO switching ──');
{
  let c = few.cells;
  ok(skyViewOf('auto', c, few.dots, T + 86400).base === 'tracks', 'three passes: tracks');
  const add: SkyDot[] = [];
  for (let i = 0; i < c!.total * 0.4; i++) { const b = skyCellBounds(c!, i); for (let k = 0; k < 4; k++) add.push({ az: (b.az0 + b.az1) / 2, el: (b.el0 + b.el1) / 2, snr: 9, t: T, norad: 1 }); }
  c = skyCellsAddDots(c, add);
  ok(c!.auto === 'heatmap' && skyViewOf('auto', c, few.dots, T + 86400).base === 'heatmap', 'enough added: the heatmap');
}
console.log('── the live pass: pick → track over the heatmap → LOS → the heatmap only ──');
{
  const now = T + 86400;
  const norad = 41189;
  let cells = heat.cells;
  let dots = heat.dots.slice();
  const live: SkyDot[] = [];
  for (let k = 0; k < 30; k++) live.push({ az: 100 + k, el: 20 + k, snr: 12, t: now - 150 + k * 5, norad });
  dots = [...live].reverse().concat(dots);
  const before = cells!.cells.get(skyCellIndex(cells!, 110, 30))?.n ?? 0;
  cells = skyCellsAddDots(cells, live);   // ★ the box counts the live dots in as they come
  const v = skyViewOf('auto', cells, dots, now, norad);
  ok(v.base === 'heatmap' && v.live && v.overlay.length === 30 && v.overlay.every((d) => d.norad === norad), `live: the heatmap with the picked bird's pass on top (${v.overlay.length} dots)`);
  const sc = skySceneFor('auto', cells, dots, now, 300, 3000, norad);
  ok(sc.mode === 'heatmap' && sc.cells.length > 3 && sc.dots.length === 30, `the scene: ${sc.cells.length} colour paths + ${sc.dots.length} track dots`);
  ok(skySummaryFor('auto', cells, dots, now, false, norad).endsWith('· the live pass on top'), 'the line says the live pass is on top');
  const los = now + SKY_LIVE_SEC + 5;
  const w = skyViewOf('auto', cells, dots, los, norad);
  const sc2 = skySceneFor('auto', cells, dots, los, 300, 3000, norad);
  ok(w.base === 'heatmap' && !w.live && sc2.dots.length === 0 && sc2.cells.length > 3, 'LOS: the heatmap only');
  ok((cells!.cells.get(skyCellIndex(cells!, 110, 30))?.n ?? 0) > before, '…with the finished pass merged into its cells');
  ok(skyLivePass(dots, now, 0).length === 30 && skyLivePass(dots, now, 99999).length === 0, 'no pick: the newest bird; another bird picked: nothing of this one');
  // little data: the live pass is simply among the tracks
  const t = skyViewOf('auto', few.cells, [...live].reverse().concat(few.dots), now, norad);
  ok(t.base === 'tracks' && t.overlay.length === 0, 'little data: tracks (the live pass among them)');
}
console.log('── the overrides ──');
ok(skyViewOf('tracks', heat.cells, heat.dots, T).base === 'tracks' && skyViewOf('heatmap', few.cells, few.dots, T).base === 'heatmap', 'TRACKS and HEATMAP override AUTO');
ok(skyViewOf('heatmap', null, few.dots, T).base === 'tracks', 'an older server (no cells): tracks whatever is asked');
ok(skyModeOf('heatmap') === 'heatmap' && skyModeOf('x') === 'auto' && /data-v="heatmap" class="btn on" aria-pressed="true"/.test(skyModesHtml('heatmap')), 'the buttons');
ok(skyPlotQuery('orbcomm', '', 0, 0, true, 'heatmap').endsWith('&mode=heatmap') && !skyPlotQuery('orbcomm', '', 0, 0, true).includes('mode='), 'the PNG asked in the view chosen');
console.log('── the drawing ──');
{
  const p = skyHeatPaths(heat.cells!, 150, 150, 129);
  ok(p.length >= 5 && p.length <= 20 && p[0].fill === 'rgb(110,110,110)', `one path per colour, grey first (${p.length} paths)`);
  const svg = skyPlotSvg(skySceneFor('auto', heat.cells, heat.dots, T + 86400), 'x');
  const svgT = skyPlotSvg(skySceneFor('tracks', heat.cells, heat.dots, T + 86400), 'x');
  ok((svg.match(/data-heat="1"/g) || []).length === p.length && !/<circle[^>]+r="2/.test(svg.replace(/<circle cx="150.0" cy="150.0"[^>]*>/g, '')), 'the heatmap SVG: the paths, no track dots');
  ok(!svgT.includes('data-heat') && (svgT.match(/<circle/g) || []).length > 300, 'the tracks SVG: dots, no paths');
}
console.log(`\ntest_skyplot_heat: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
