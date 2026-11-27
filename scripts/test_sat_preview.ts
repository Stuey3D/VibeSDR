// test_sat_preview.ts — ★★★ beta97 PASS PREVIEW, the clients' half (src/services/satPreview.ts): the pass-track sampler on
// the server's OWN replies (test/fixtures/sat-preview/passes.json — vibeserver/gen-sat-preview-fixture.cpp ran the real
// engine on the 2026-11-19 CelesTrak fixtures): AOS / MAX / LOS and the elevations match the server's pass prediction,
// the Doppler matches the engine's dopplerFor; the look angles match Skyfield (testdata-sat/reference-passes.txt); the
// player (≈10 s a pass, stops at LOS, replay, pause, seek, no frames when paused); the verdicts — a pass through a
// high-history sector is "good", one through a sector with no history says "geometry only", no history at all says so;
// the trail's runs; the live-pass priority; the words.
// Silent: no audio. Run: TZ=UTC npx tsx scripts/test_sat_preview.ts
import fs from 'node:fs';
import { parseSatTrack, type SatList, type SatTrack } from '../src/services/satellites';
import { parseSkyCells, skyCellIndex, type SkyCells } from '../src/utils/skyPlot';
import {
  dopplerHzFor, PassPlayer, passHistoryKinds, passKey, passLiveNow, passMomentsText, passReadout, passSampleAt, passSkyPaths,
  passStripText, passTrackFrom, passTrailRuns, passVerdict, PASS_NO_HISTORY, PASS_PLAY_SEC, skyGradeAt, skyHistory, lookAt, geodeticToEcef,
  passPreviewPress, passPreviewRow, previewId, PASS_LIVE_NOW, PASS_START_DELAY_MS, type PassTrack,
} from '../src/services/satPreview';

let fail = 0, pass = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (c) pass++; else fail++; };

interface Case { name: string; norad: number; rx: { lat: number; lon: number }; hz: number;
  pass: { aos: number; tca: number; los: number; maxEl: number; aosAz: number; losAz: number }; track: unknown; doppler: Array<[number, number, number]> }
const fx = JSON.parse(fs.readFileSync('test/fixtures/sat-preview/passes.json', 'utf8')) as { cases: Case[] };

console.log('── the sampler vs the server\'s own pass prediction ──');
ok(fx.cases.length >= 4, `${fx.cases.length} fixture passes`);
const tracks: Record<string, PassTrack> = {};
for (const c of fx.cases) {
  const tr = parseSatTrack(c.track);
  ok(!!tr && tr.geoT0 != null && tr.geoStep != null && tr.tca != null, `${c.name}: the reply keeps geoT0 / geoStep / tca`);
  const pt = passTrackFrom(tr, c.rx, c.pass);
  ok(!!pt, `${c.name}: a pass track`);
  if (!pt) continue;
  tracks[c.name] = pt;
  const dA = pt.aos - c.pass.aos, dL = pt.los - c.pass.los, dT = pt.tca - c.pass.tca, dE = pt.maxEl - c.pass.maxEl;
  console.log(`      AOS ${dA.toFixed(2)} s · TCA ${dT.toFixed(2)} s · LOS ${dL.toFixed(2)} s · max el ${dE.toFixed(3)}° · ${pt.samples.length} samples`);
  ok(Math.abs(dA) <= 2, `${c.name}: AOS within 2 s of the server's (${dA.toFixed(2)} s)`);
  ok(Math.abs(dL) <= 2, `${c.name}: LOS within 2 s (${dL.toFixed(2)} s)`);
  ok(Math.abs(dT) <= 5, `${c.name}: the highest point within 5 s (${dT.toFixed(2)} s)`);
  ok(Math.abs(dE) <= 0.2, `${c.name}: max elevation within 0.2° (${pt.maxEl.toFixed(2)}° vs ${c.pass.maxEl.toFixed(2)}°)`);
  ok(Math.abs(((pt.aosAz - c.pass.aosAz + 540) % 360) - 180) <= 1 && Math.abs(((pt.losAz - c.pass.losAz + 540) % 360) - 180) <= 1, `${c.name}: AOS / LOS azimuths within 1°`);
  // every sample's elevation against the engine's at the same moment (its dopplerFor carries el)
  let worstEl = 0, worstDop = 0;
  for (const [t, dop, el] of c.doppler) {
    const s = passSampleAt(pt, t);
    worstEl = Math.max(worstEl, Math.abs(s.el - el));
    worstDop = Math.max(worstDop, Math.abs(dopplerHzFor(c.hz, s.rrKmS) - dop));
  }
  ok(worstEl <= 0.15, `${c.name}: elevation within 0.15° of the engine's every 30 s (worst ${worstEl.toFixed(3)}°)`);
  ok(worstDop <= 30, `${c.name}: Doppler at ${(c.hz / 1e6).toFixed(3)} MHz within 30 Hz of the engine's (worst ${worstDop.toFixed(1)} Hz)`);
  ok(pt.samples[0].t === pt.aos && pt.samples[pt.samples.length - 1].t === pt.los && pt.samples.every((s, i) => !i || s.t - pt.samples[i - 1].t <= pt.step + 1e-6),
     `${c.name}: samples AOS → LOS every ≤ ${pt.step} s`);
}

console.log('── look angles vs Skyfield (reference-passes.txt) ──');
{
  const ref = fs.readFileSync('vibeserver/testdata-sat/reference-passes.txt', 'utf8').split('\n').filter((l) => l.startsWith('LOOK 25544 -23.5'));
  const pt = tracks['ISS Sao Paulo 56'];
  let n = 0, worst = 0, worstR = 0, worstRR = 0;
  for (const l of ref) {
    const [, , , , t, az, el, rng, rr] = l.split(/\s+/).map(Number);
    if (!pt || t < pt.aos || t > pt.los) continue;
    const s = passSampleAt(pt, t); n++;
    worst = Math.max(worst, Math.abs(s.el - el), Math.abs(((s.az - az + 540) % 360) - 180) * Math.cos(el * Math.PI / 180));
    worstR = Math.max(worstR, Math.abs(s.rangeKm - rng)); worstRR = Math.max(worstRR, Math.abs(s.rrKmS - rr));
  }
  ok(n >= 5, `${n} Skyfield looks inside the 56° pass`);
  ok(worst <= 0.2, `az / el within 0.2° of Skyfield (worst ${worst.toFixed(3)}°)`);
  ok(worstR <= 3, `range within 3 km (worst ${worstR.toFixed(2)} km)`);
  ok(worstRR <= 0.06, `range rate within 0.06 km/s (worst ${worstRR.toFixed(4)})`);
  // the geometry itself: straight up from the receiver is el 90, range = the height
  const up = lookAt({ lat: 52, lon: -1 }, geodeticToEcef(52, -1, 500));
  ok(Math.abs(up.el - 90) < 0.01 && Math.abs(up.rangeKm - 500) < 0.01, 'a point 500 km straight up: el 90°, 500 km');
}

console.log('── refusals ──');
ok(passTrackFrom(null, { lat: 0, lon: 0 }) === null, 'no track → null');
ok(passTrackFrom({ norad: 1, aos: 1, los: 2, pts: [] }, { lat: 0, lon: 0 }) === null, 'an older server\'s reply (no geo) → null');
{
  const c = fx.cases[0], tr = parseSatTrack(c.track)! as SatTrack;
  ok(passTrackFrom({ ...tr, geoT0: undefined }, c.rx) === null, 'geo without its clock → null (no guessing the times)');
  ok(passTrackFrom(tr, null) === null, 'no receiver position → null');
  ok(passTrackFrom(tr, { lat: -c.rx.lat, lon: c.rx.lon + 180 }) === null, 'the far side of the Earth → nothing above the horizon → null');
}

console.log('── the player ──');
{
  const pt = tracks['ISS Sao Paulo 56'];
  const p = new PassPlayer(pt.aos, pt.los);
  let frames = 0, ms = 0;
  while (p.advance(16.67)) { frames++; ms += 16.67; if (frames > 5000) break; }
  ok(p.ended && !p.playing && p.t === pt.los, 'plays to LOS and stops there');
  ok(Math.abs(ms / 1000 - PASS_PLAY_SEC) < 0.2, `a whole pass in ~${PASS_PLAY_SEC} s at 1× (${(ms / 1000).toFixed(2)} s)`);
  ok(!p.advance(16), 'ended → no more frames');
  p.toggle(); ok(p.playing && p.t === pt.aos, '▶ after LOS replays from AOS');
  p.advance(1000); const t1 = p.t; p.toggle(); ok(!p.playing && !p.advance(1000) && p.t === t1, '❚❚ pauses: no frames, the clock holds');
  p.setSpeed(2); p.toggle(); p.advance(100); const r2 = p.rate(); p.setSpeed(0.5); ok(Math.abs(r2 / p.rate() - 4) < 1e-9, '2× is four times 0.5×');
  p.setSpeed(7); ok(p.speed === 1, 'an unknown speed → 1×');
  p.seek(0.5); ok(Math.abs(p.frac() - 0.5) < 1e-9, 'seek to the middle');
  const p2 = new PassPlayer(pt.aos, pt.los); p2.advance(60000); ok(p2.t - pt.aos <= 0.25 * p2.rate() + 1e-6, 'a stalled tab (60 s frame) moves at most 250 ms of play');
}

console.log('── readouts and the words ──');
{
  const pt = tracks['ISS Sao Paulo 56'];
  const r = passReadout(pt, pt.tca, 145.8e6);
  ok(/^\d\d:\d\d:\d\d UTC · \d\d:\d\d:\d\d local$/.test(r.clock), `the clock: ${r.clock}`);
  ok(/^EL 5[67]° · AZ \d+° [NESW]{1,2}$/.test(r.look), `at the top: ${r.look}`);
  ok(/km$/.test(r.range), `range: ${r.range}`);
  ok(/^Doppler [+−]0\.\d kHz at 145\.800 MHz$/.test(r.doppler), `Doppler near zero at the top: ${r.doppler}`);
  const rA = passReadout(pt, pt.aos + 5, 145.8e6);
  ok(/^Doppler \+3\.\d kHz/.test(rA.doppler), `approaching: high (+): ${rA.doppler}`);
  ok(passReadout(pt, pt.aos).doppler === '', 'no downlink → no Doppler line');
  const m = passMomentsText(pt);
  ok(/^AOS \d\d:\d\d [NESW]{1,2} · MAX 56° \d\d:\d\d · LOS \d\d:\d\d [NESW]{1,2} · \d+:\d\d$/.test(m), `moments: ${m}`);
  ok(passStripText('ISS', pt.aos).startsWith('▶ PREVIEW ISS · '), passStripText('ISS', pt.aos));
  ok(passKey(25544, 1795609400.7) === '25544:1795609401', 'pass key');
  ok(passHistoryKinds('lrpt').join() === 'meteor,orbcomm' && passHistoryKinds('').join() === 'orbcomm', 'history kinds: the decoder\'s own, then ORBCOMM');
}

console.log('── the verdict from the station\'s history ──');
// a grid like the server's (9 bands of 10°, fewer cells towards the zenith)
const grid = { elStep: 10, az: [72, 72, 64, 56, 48, 40, 28, 16, 4] };
function cellsWith(fill: (az: number, el: number) => [number, number] | null): SkyCells {
  const c0 = parseSkyCells({ ...grid, c: [] })!;
  const rows: unknown[] = [];
  for (let b = 0; b < grid.az.length; b++)
    for (let k = 0; k < grid.az[b]; k++) {
      const az = (k + 0.5) * 360 / grid.az[b], el = (b + 0.5) * 10;
      const v = fill(az, el);
      if (v) rows.push([skyCellIndex(c0, az, el), v[0], v[1], 120, 15, 1795600000]);
    }
  return parseSkyCells({ ...grid, c: rows })!;
}
const inArc = (az: number, lo: number, hi: number) => (lo <= hi ? az >= lo && az < hi : az >= lo || az < hi);
{
  const pt = tracks['ISS Sao Paulo 9'];   // a low pass — which way?
  const mid = passSampleAt(pt, pt.tca).az;
  const lo = (mid - 90 + 360) % 360, hi = (mid + 90) % 360;
  console.log(`      the 9° pass: AOS ${pt.aosAz.toFixed(0)}° · top ${mid.toFixed(0)}° · LOS ${pt.losAz.toFixed(0)}°`);
  // a station that hears this pass's side of the sky well (decodes in 9 of 10 buckets) and the other side never
  const good = skyHistory(cellsWith((az) => (inArc(az, lo, hi) ? [9, 1] : [0, 6])));
  const v = passVerdict(pt, good);
  ok(v.grade === 'good' && /^Mostly over your good sector \([NESW]{1,2}\) · max 9°$/.test(v.text), `high-history sector → good: "${v.text}"`);
  ok(skyGradeAt(good, mid, 5) === 'good' && skyGradeAt(good, (mid + 180) % 360, 5) === 'poor', 'the grade at a point follows its cell');
  // the opposite station: this side tracked, never decoded
  const weak = skyHistory(cellsWith((az) => (inArc(az, lo, hi) ? [0, 8] : [9, 1])));
  const vw = passVerdict(pt, weak);
  ok(vw.grade === 'poor' && /^Low and through your weak sector/.test(vw.text), `tracked-but-never-heard sector → weak: "${vw.text}"`);
  // history only on the OTHER side of the sky: this pass's sector has none
  const other = skyHistory(cellsWith((az) => (inArc(az, lo, hi) ? null : [9, 1])));
  const vo = passVerdict(pt, other);
  ok(vo.grade === 'none' && /geometry only/.test(vo.text), `a sector with no history → geometry only: "${vo.text}"`);
  // no history at all
  const vn = passVerdict(pt, skyHistory(cellsWith(() => null)));
  ok(vn.grade === 'none' && vn.text.startsWith(PASS_NO_HISTORY) && !vn.history, `no history → "${vn.text}"`);
  ok(skyHistory(null) === null && passVerdict(pt, null).text.startsWith(PASS_NO_HISTORY), 'no cells (an older server) → no history');
  // sparse: one dot per cell is too thin alone, the 45° sector's sum decides (the ring of sectors)
  const sparse = skyHistory(cellsWith((az, el) => (el < 20 && inArc(az, lo, hi) ? [1, 0] : null)));
  ok(skyGradeAt(sparse, mid, 5) === 'good', 'sparse: one dot per cell — its sector\'s sum grades it');
  // marginal: decodes in a third of the buckets
  const fair = skyHistory(cellsWith((az) => (inArc(az, lo, hi) ? [3, 6] : [0, 6])));
  ok(passVerdict(pt, fair).grade === 'fair', `a third heard → marginal: "${passVerdict(pt, fair).text}"`);
  // a high pass that crosses both: mixed
  const hp = tracks['ISS Sao Paulo 56'];
  const hmid = passSampleAt(hp, hp.aos).az;
  const mixed = skyHistory(cellsWith((az) => (Math.abs(((az - hmid + 540) % 360) - 180) < 90 ? [9, 1] : [0, 8])));
  const vm = passVerdict(hp, mixed);
  ok(/^Mixed: |^Mostly|^Through/.test(vm.text) && vm.history, `a pass across both halves: "${vm.text}"`);
  // the trail's runs: contiguous, coloured by grade, ending at the time asked
  const runs = passTrailRuns(hp, mixed, hp.tca);
  ok(runs.length >= 1 && runs.every((r, i) => !i || r.pts[0].t === runs[i - 1].pts[runs[i - 1].pts.length - 1].t), `trail runs join end to end (${runs.map((r) => r.grade).join(' → ')})`);
  ok(Math.abs(runs[runs.length - 1].pts.slice(-1)[0].t - hp.tca) < 1e-6, 'the trail ends at the bird');
  const sp = passSkyPaths(hp, mixed, 150, 150, 130);
  ok(sp.runs.every((r) => /^M[\d.]+ [\d.]+(L[\d.]+ [\d.]+)+$/.test(r.d)) && Math.hypot(sp.max[0] - 150, sp.max[1] - 150) < 130 * 0.4, 'the polar plot: run paths, the top near the centre for a 56° pass');
}

console.log('── a live pass takes priority ──');
{
  const list = { time: 'ok', located: true, now: 100, sats: [{ id: 'ISS', name: 'ISS', norad: 25544, note: '', age: 1, ageState: 'fresh', tx: [], passes: [{ aos: 90, tca: 95, los: 200, maxEl: 30, aosAz: 0, losAz: 180 }] }] } as SatList;
  ok(passLiveNow({ norad: 25544 }, null, list, 100), 'the picked bird\'s pass is in progress → live');
  ok(!passLiveNow({ norad: 25544 }, null, list, 300), 'after LOS → not live');
  ok(!passLiveNow(null, null, list, 100), 'nothing picked → no live pass on this radio');
  ok(passLiveNow({ norad: 1 }, { state: 'TRACK', norad: 1, time: 'ok' }, list, 100), 'the lock tracking → live');
}

console.log('── the panel\'s rules (web satellites.ts / app SatellitePanel share them) ──');
{
  const base = { sat: { norad: 25544, name: 'ISS', tx: [{}, {}] }, tx: { label: 'Voice', hz: 145.8e6, decoder: 'sstv' }, i: 1, grey: false, up: false, next: { aos: 1000, los: 1600 } };
  const r = passPreviewRow(base, true)!;
  ok(!!r && r.name === 'ISS · Voice' && r.hz === 145.8e6 && r.aos === 1000 && r.tx === 1, 'a future hearable pass → a ▶ (named with its downlink when the bird has more than one)');
  ok(passPreviewRow({ ...base, grey: true }, true) === null, 'a greyed row (this radio cannot hear it) → no ▶');
  ok(passPreviewRow({ ...base, up: true }, true) === null, 'a pass already up → no ▶ (it is live, not a preview)');
  ok(passPreviewRow({ ...base, next: undefined }, true) === null, 'no pass to come → no ▶');
  ok(passPreviewRow(base, false) === null, 'no receiver position → no ▶ (no geometry to show)');
  ok(previewId(r) === '25544:1000:1', 'one preview per pass and downlink');
  ok(JSON.stringify(passPreviewPress(null, 'a', false)) === '{"id":"a","note":""}', '▶ → that preview');
  ok(JSON.stringify(passPreviewPress('a', 'b', false)) === '{"id":"b","note":""}', '▶ on another row → that one instead (one at a time)');
  ok(passPreviewPress('a', 'a', false).id === null, 'the same ▶ again → back to the live view');
  const lv = passPreviewPress(null, 'a', true);
  ok(lv.id === null && lv.note === PASS_LIVE_NOW, 'a live pass on the pick → no preview, the note');
  ok(PASS_START_DELAY_MS >= 250 && PASS_START_DELAY_MS <= 500, `the beat after the scroll: ${PASS_START_DELAY_MS} ms`);
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
