// test_tlm_charts.ts — ★★ V12 beta91 TELEMETRY CHARTS, the clients' pure half (src/utils/tlmCharts.ts):
// the server's per-pass .csv read back (scripts/fixtures/fctlm/ — written by `test-fctlm-gallery --write-fixture`, the
// server's own writer on SYNTHETIC readings, not on-air data), [raw] / unit / count headings, what is charted and in which
// order, the live history (bounded), the chart geometry (in the box, broken at gaps, flat channels centred), the
// across-passes summary, and which gallery items are a satellite's passes.
process.env.TZ = 'UTC';
import { readFileSync } from 'fs';
import { join } from 'path';
import {
  tlmHeading, tlmChartable, tlmParseCsv, tlmLivePass, tlmLivePush, tlmChartOrder, tlmValueText, tlmPassHead, tlmAcrossHead,
  tlmSpark, tlmRange, tlmAcross, tlmPassItems, tlmSatOfTitle, tlmLoadPasses, TLM_LIVE_MAX, type TlmLiveFrame,
} from '../src/utils/tlmCharts';
import { satTelemetryMerge, parseSatTelemetry } from '../src/services/satellites';

let fail = 0, pass = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (c) pass++; else fail++; };
const FIX = join(__dirname, 'fixtures', 'fctlm');
const csv = (n: number) => readFileSync(join(FIX, `pass-${n}.csv`), 'utf8');

console.log('§1 headings');
{
  const a = tlmHeading('Battery voltage [raw]'), b = tlmHeading('Bus [V]'), c = tlmHeading('Reboot count');
  ok(a.name === 'Battery voltage' && a.raw && a.unit === '', '"[raw]": raw, no unit');
  ok(b.name === 'Bus' && !b.raw && b.unit === 'V', '"[V]": a unit');
  ok(c.name === 'Reboot count' && !c.raw && !c.unit && !tlmChartable(c), 'no bracket: a count — read, not charted');
  ok(tlmChartable(a) && tlmChartable(b), 'raw and calibrated channels are charted');
}

console.log('§2 the server\'s pass CSV');
const p1 = tlmParseCsv(csv(1))!, p2 = tlmParseCsv(csv(2))!, p3 = tlmParseCsv(csv(3))!;
{
  ok(!!p1 && p1.sat === 'AO-73', `the satellite from the head (${p1?.sat})`);
  ok(p1.t.length === 24 && p2.t.length === 18 && p3.t.length === 24, `rows: 24 / 18 (a fade) / 24 (${p1.t.length}/${p2.t.length}/${p3.t.length})`);
  ok(p1.channels.length === 58 && p1.v.every((col) => col.length === p1.t.length), 'every channel, a reading per row');
  const bi = p1.channels.findIndex((c) => c.name === 'Battery voltage');
  ok(bi >= 0 && p1.channels[bi].raw && p1.v[bi][0] === 1433, `Battery voltage raw, row 1 = 1433 (${p1.v[bi]?.[0]})`);
  ok(p1.start === 1795356150 && p1.t[1] - p1.t[0] === 5, 'times from the unix column, 5 s a frame');
  ok(tlmParseCsv('hello') === null && tlmParseCsv('') === null && tlmParseCsv('# x\na,b\n1,2') === null, 'not a pass CSV → null');
  ok(tlmParseCsv(csv(1), 5)!.t.length === 5, 'bounded: read to maxRows');
  ok(!!tlmParseCsv(csv(1).replace(/\n/g, '\r\n')), 'CRLF (a spreadsheet re-save) reads too');
}

console.log('§3 what is charted, in which order');
{
  const o = tlmChartOrder(p1);
  const names = o.map((i) => p1.channels[i].name);
  ok(names[0] === 'Battery voltage' && names[1] === 'Battery temp' && names[2] === 'Total photo current', `the power system first (${names.slice(0, 3).join(', ')})`);
  ok(!names.includes('Reboot count') && !names.includes('Sequence number') && !names.includes('In eclipse mode'), 'counts and flags are not charted');
  ok(names.length === p1.channels.filter(tlmChartable).length, `every analogue channel charted (${names.length})`);
  ok(tlmValueText(1433, { name: '', unit: '', raw: true }) === '1433 raw' && tlmValueText(8.134, { name: '', unit: 'V', raw: false }) === '8.13 V'
     && tlmValueText(null, { name: '', unit: '', raw: true }) === '—', 'values in words: "1433 raw", "8.13 V", "—"');
  ok(tlmPassHead(p1, false) === 'THE PASS · 24 frames · 14:02–14:04 UTC', tlmPassHead(p1, false));
  ok(tlmPassHead(null, true) === 'THIS PASS · waiting for a frame' && tlmAcrossHead(0).includes('none saved'), 'heads before there is data');
}

console.log('§4 the live history');
{
  let t = satTelemetryMerge(null, parseSatTelemetry({ frames: 3, good: 2, failed: 1 }));
  ok(!t?.hist?.length, 'a status message adds no frame');
  for (let i = 0; i < 5; i++) t = satTelemetryMerge(t, parseSatTelemetry({ sat: 'AO-73', t: 1000 + 5 * i, values: [{ k: 'eps.vbat', name: 'Battery voltage', v: 1400 + i, raw: true }, { k: 'x', name: 'Bus', v: 8.1, unit: 'V' }] }));
  ok(t!.hist!.length === 5 && t!.values[0].value === '1404', 'each sat_tlm frame joins the history; the box still shows the last');
  t = satTelemetryMerge(t, parseSatTelemetry({ frames: 9 }));
  ok(t!.hist!.length === 5, 'a status after keeps the history');
  const lp = tlmLivePass(t!.hist, 'AO-73')!;
  ok(lp.t.length === 5 && lp.channels[0].raw && lp.channels[1].unit === 'V' && lp.v[0][4] === 1404, 'the history as a pass: channels, raw / unit, readings');
  let h: TlmLiveFrame[] = [];
  for (let i = 0; i < TLM_LIVE_MAX + 50; i++) h = tlmLivePush(h, { t: i + 1, values: [{ name: 'a', value: String(i), unit: '', raw: true }] });
  ok(h.length === TLM_LIVE_MAX && h[0].t === 51, `bounded at ${TLM_LIVE_MAX} frames, the oldest dropped`);
  const nx = tlmLivePush(h, { t: h[h.length - 1].t + 3600, values: [{ name: 'a', value: '1', unit: '', raw: true }] });
  ok(nx.length === 1, 'the next pass on the same pick (an hour on) begins THIS PASS again');
  ok(tlmLivePush(h, { t: h[h.length - 1].t, values: [{ name: 'a', value: '9', unit: '', raw: true }] }).length === TLM_LIVE_MAX, 'the same frame twice is one');
}

console.log('§5 the geometry');
{
  const bi = p2.channels.findIndex((c) => c.name === 'Battery voltage');
  const W = 150, H = 40;
  const sp = tlmSpark(p2.t, p2.v[bi], W, H, 3);
  const nums = sp.line.match(/-?\d+(\.\d+)?/g)!.map(Number);
  const xs = nums.filter((_, i) => i % 2 === 0), ys = nums.filter((_, i) => i % 2 === 1);
  ok(xs.every((x) => x >= 3 - 1e-6 && x <= W - 3 + 1e-6) && ys.every((y) => y >= 3 - 1e-6 && y <= H - 3 + 1e-6), 'every point inside the box');
  ok(Math.min(...ys) === 3 && Math.max(...ys) === H - 3, 'the range fills the box (min at the bottom, max at the top)');
  ok((sp.line.match(/M/g) ?? []).length === 2, `the fade breaks the line: two runs (${(sp.line.match(/M/g) ?? []).length})`);
  ok(sp.n === 18 && sp.lastV === p2.v[bi][17] && !!sp.last && Math.abs(sp.last.x - (W - 3)) < 0.06, 'the newest point, for its dot, at the right');
  const flat = tlmSpark([1, 2, 3], [5, 5, 5], W, H, 3);
  ok(flat.line.includes(`${(H / 2)}`) && flat.lo < 5 && flat.hi > 5, 'a flat channel sits on the middle');
  const one = tlmSpark([1], [7], W, H, 3);
  ok(one.n === 1 && one.last!.x === W / 2, 'a single frame: a point in the middle');
  ok(tlmSpark([], [], W, H).line === '' && tlmSpark([1, 2], [null, null], W, H).last === null, 'nothing to draw → nothing drawn');
  const r = tlmRange([1, 2], [0, 10]);
  ok(r.lo === 0 && r.hi === 10, 'the band widens the range');
}

console.log('§6 across passes');
{
  const a = tlmAcross([p1, p2, p3], 'Battery voltage');
  ok(a.t.length === 3 && a.t[0] < a.t[1] && a.t[1] < a.t[2], 'a point per pass, oldest first');
  const bi = p1.channels.findIndex((c) => c.name === 'Battery voltage');
  const vals = p1.v[bi].filter((x): x is number => x != null);
  ok(Math.abs(a.mean[0]! - vals.reduce((s, x) => s + x, 0) / vals.length) < 1e-9 && a.min[0] === Math.min(...vals) && a.max[0] === Math.max(...vals), 'mean, min, max of the pass');
  const sp = tlmSpark(a.t, a.mean, 150, 40, 3, Infinity, a.min, a.max);
  ok(sp.band.startsWith('M') && sp.band.endsWith('Z') && (sp.line.match(/M/g) ?? []).length === 1, 'the band is one closed shape, the means one line');
  const miss = tlmAcross([p1], 'No such channel');
  ok(miss.mean[0] === null, 'a pass without the channel: a gap, not a zero');
}

console.log('§7 the gallery\'s passes');
{
  const items = [
    { f: 'a.csv', k: 'fctlm', t: 300, title: 'AO-73 telemetry · pass 14:02 UTC · 24 frames' },
    { f: 'b.csv', k: 'fctlm', t: 100, title: 'AO-73 telemetry · pass 10:00 UTC · 3 frames' },
    { f: 'c.txt', k: 'fctlm', t: 200, title: 'AO-73 telemetry log · 22 Nov · 66 frames' },
    { f: 'd.csv', k: 'fctlm', t: 250, title: 'SONATE-2 telemetry · pass 12:00 UTC · 4 frames' },
    { f: 'e.csv', k: 'rtty', t: 260, title: 'AO-73 telemetry · pass x' },
  ];
  ok(tlmPassItems(items, 'AO-73').map((x) => x.f).join() === 'a.csv,b.csv', 'AO-73\'s pass CSVs, newest first (not the day log, not another satellite)');
  ok(tlmPassItems(items, 'AO-73', 10, 200).map((x) => x.f).join() === 'b.csv', 'upTo: the passes up to the one open');
  ok(tlmPassItems(items, 'AO-73', 1).length === 1, 'limit');
  // ★ Stuart 11-22 "only one chart needed regardless of background or user listened": a background profile's passes and a
  //   listener's picks are ONE history — never filtered by origin or profile
  const mixed = [
    { f: 'bg1.csv', k: 'fctlm', t: 100, o: 'background', p: 'Profile 1', title: 'AO-73 telemetry · pass 10:00 UTC · 20 frames' },
    { f: 'lv1.csv', k: 'fctlm', t: 200, o: 'live', p: '', title: 'AO-73 telemetry · pass 11:40 UTC · 18 frames' },
    { f: 'bg2.csv', k: 'fctlm', t: 300, o: 'background', p: 'Night', title: 'AO-73 telemetry · pass 13:20 UTC · 22 frames' },
  ];
  ok(tlmPassItems(mixed, 'AO-73').map((x) => x.f).join() === 'bg2.csv,lv1.csv,bg1.csv', 'background and listener passes: one history, in time order');
  ok(tlmSatOfTitle(items[0].title) === 'AO-73' && tlmSatOfTitle('AO-73 (FUNcube-1) telemetry · pass 1') === 'AO-73 (FUNcube-1)', 'the satellite from a title');
}

(async () => {
  console.log('§8 loading the passes (one request at a time)');
  const files: Record<string, string> = { 'p1.csv': csv(1), 'p2.csv': csv(2), 'p3.csv': csv(3) };
  const list = { items: [
    { f: 'p3.csv', k: 'fctlm', t: p3.start, title: 'AO-73 telemetry · pass 17:22 UTC · 24 frames' },
    { f: 'p1.csv', k: 'fctlm', t: p1.start, title: 'AO-73 telemetry · pass 14:02 UTC · 24 frames' },
    { f: 'p2.csv', k: 'fctlm', t: p2.start, title: 'AO-73 telemetry · pass 15:42 UTC · 18 frames' },
    { f: 'gone.csv', k: 'fctlm', t: p1.start - 9000, title: 'AO-73 telemetry · pass 11:00 UTC · 2 frames' },
  ] };
  let inFlight = 0, most = 0;
  const get = async (path: string) => {
    inFlight++; most = Math.max(most, inFlight);
    await new Promise((r) => setTimeout(r, 1));
    inFlight--;
    if (path.startsWith('/vibeserver/gallery.json')) return JSON.stringify(list);
    const f = decodeURIComponent(path.replace('/vibeserver/gallery/file/', ''));
    return files[f] ?? null;
  };
  const got = await tlmLoadPasses(get, 'AO-73');
  ok(got.length === 3 && got[0].start === p1.start && got[2].start === p3.start, 'three passes, oldest first (the evicted one skipped)');
  ok(most === 1, 'never more than one request at a time');
  {   // ★ mixed sources chart as ONE series: a point per pass whoever decoded it
    const mixedList = { items: [
      { f: 'p1.csv', k: 'fctlm', o: 'background', p: 'Profile 1', t: p1.start, title: 'AO-73 telemetry · pass 14:02 UTC · 24 frames' },
      { f: 'p2.csv', k: 'fctlm', o: 'live', p: '', t: p2.start, title: 'AO-73 telemetry · pass 15:42 UTC · 18 frames' },
      { f: 'p3.csv', k: 'fctlm', o: 'background', p: 'Night', t: p3.start, title: 'AO-73 telemetry · pass 17:22 UTC · 24 frames' },
    ] };
    const g2 = async (path: string) => path.startsWith('/vibeserver/gallery.json') ? JSON.stringify(mixedList)
      : files[decodeURIComponent(path.replace('/vibeserver/gallery/file/', ''))] ?? null;
    const mp = await tlmLoadPasses(g2, 'AO-73');
    const a = tlmAcross(mp, 'Battery voltage');
    ok(mp.length === 3 && a.t.length === 3 && a.mean.every((x) => x != null), 'background + listener passes chart as ONE series (3 points)');
  }
  const upTo = await tlmLoadPasses(get, 'AO-73', p2.start);
  ok(upTo.length === 2 && upTo[1].start === p2.start, 'up to an open pass');
  ok((await tlmLoadPasses(async () => null, 'AO-73')).length === 0, 'no gallery → no passes, no throw');
  console.log(`\n${pass} passed, ${fail} failed`);
  process.exit(fail ? 1 : 0);
})();
