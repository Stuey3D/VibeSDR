/* test_adsb_delta.ts — ★★ V12 beta96 the ADS-B DELTA protocol end to end: the server's encoder (vibe_adsb_delta.h, driven
 * by the REAL Tracker on a synthetic 150-aircraft sky — vibeserver/test-adsb-delta.cpp --stream) into the clients' own
 * store (src/services/adsbDelta.ts — the app and the web client both import it).
 *
 * At EVERY tick the table rebuilt from the deltas must equal the full `adsb` report an older client gets, field for field,
 * after the documented quantising (altitudes 25 ft, speeds 1 kt, track/heading 1°, signal 1 dB down, ages within 0.65 s):
 *   A. a listener from the start (keyframe, then deltas, with the 60 s keyframes);
 *   B. a listener joining mid-stream (the keyframe it is sent on joining);
 *   C. a listener whose delta is lost: it must ask for a resync, keep its table, and match again from the keyframe;
 *   D. what the screens draw from it is the same: the app's parsed report and its list order (adsbToAircraft);
 *   E. junk: a bad message never corrupts the table — it asks for a resync instead.
 * Silent; compiles the C++ stream with the system c++ (one file, nice'd). */
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const { AdsbDeltaStore } = await import('../src/services/adsbDelta.ts');
const { parseAdsbMessage, adsbToAircraft } = await import('../src/services/adsbTypes.ts');

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let pass = 0, fail = 0;
const fails: string[] = [];
function ok(what: string, c: boolean) { if (c) pass++; else { fail++; if (fails.length < 25) fails.push(what); } }

const TICKS = Number(process.env.ADSB_DELTA_TICKS || 900);
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'adsb-delta-'));
const bin = path.join(tmp, 'test-adsb-delta');
let lines: any[] = [];
try {
  execFileSync('nice', ['-n', '10', 'c++', '-std=c++17', '-O2', '-I', path.join(root, 'android/app/src/main/cpp'),
    path.join(root, 'vibeserver/test-adsb-delta.cpp'), '-o', bin], { stdio: 'inherit' });
  const out = execFileSync(bin, ['--stream', String(TICKS)], { maxBuffer: 1 << 30 }).toString();
  lines = out.trim().split('\n').map((l) => JSON.parse(l));
} finally { fs.rmSync(tmp, { recursive: true, force: true }); }
ok(`the stream ran ${TICKS} ticks`, lines.length === TICKS);

type Rec = Record<string, unknown>;
/** The full report's aircraft, quantised as the protocol quantises (vibe_adsb_delta.h). */
function quant(a: Rec): Rec {
  const q: Rec = { ...a };
  if (typeof q.alt_baro === 'number') q.alt_baro = Math.round(q.alt_baro / 25) * 25;
  if (typeof q.alt_geom === 'number') q.alt_geom = Math.round(q.alt_geom / 25) * 25;
  for (const k of ['gs', 'track', 'mag_heading']) if (typeof q[k] === 'number') q[k] = Math.round(q[k] as number);
  if (typeof q.rssi === 'number') q.rssi = Math.floor(q.rssi);
  return q;
}
const AGE_TOL = 0.65;
function sameAircraft(full: Rec[], got: Rec[], tag: string): boolean {
  const m = new Map(got.map((a) => [a.hex as string, a]));
  if (m.size !== full.length) { ok(`${tag}: ${full.length} aircraft in the full list, ${m.size} rebuilt`, false); return false; }
  for (const f0 of full) {
    const f = quant(f0), g = m.get(f.hex as string);
    if (!g) { ok(`${tag}: ${f.hex} missing`, false); return false; }
    const keys = new Set([...Object.keys(f), ...Object.keys(g)]);
    for (const k of keys) {
      const a = f[k], b = g[k];
      if (k === 'seen' || k === 'seen_pos') {
        if (typeof a !== 'number' || typeof b !== 'number' || Math.abs(a - b) > AGE_TOL) { ok(`${tag}: ${f.hex}.${k} full ${a} rebuilt ${b}`, false); return false; }
      } else if (JSON.stringify(a) !== JSON.stringify(b)) { ok(`${tag}: ${f.hex}.${k} full ${JSON.stringify(a)} rebuilt ${JSON.stringify(b)}`, false); return false; }
    }
  }
  return true;
}
function sameHeader(full: Rec, got: Rec, tag: string): boolean {
  for (const k of Object.keys(full)) if (k !== 'aircraft' && JSON.stringify(full[k]) !== JSON.stringify(got[k])) {
    ok(`${tag}: header ${k} full ${JSON.stringify(full[k])} rebuilt ${JSON.stringify(got[k])}`, false); return false;
  }
  for (const k of Object.keys(got)) if (!(k in full)) { ok(`${tag}: header ${k} only in the rebuilt`, false); return false; }
  return true;
}

// ── A / B / C ──
const A = new AdsbDeltaStore(), B = new AdsbDeltaStore(), C = new AdsbDeltaStore();
let aOk = 0, bOk = 0, cOk = 0, cResync = 0, cSkipped = 0, dOk = 0, emergSeen = 0, keyAlls = 0, maxAc = 0, ageMax = 0;
const JOIN = 300, DROP = 450;
const t0 = Date.now();
for (const L of lines) {
  const tick = L.tick as number, msg = L.keyAll ? L.k : L.d;
  keyAlls += L.keyAll ? 1 : 0;
  maxAc = Math.max(maxAc, L.full.aircraft.length);
  // A: from the start (the server sends a joiner the keyframe; tick 0 is a keyAll anyway)
  const ra = A.take(tick === 0 ? L.k : msg, t0 + tick * 1000);
  if (ra.report && sameHeader(L.full, ra.report, `A tick ${tick}`) && sameAircraft(L.full.aircraft, ra.report.aircraft as Rec[], `A tick ${tick}`)) aOk++;
  for (const a of (ra.report?.aircraft as Rec[]) || []) {
    const f = L.full.aircraft.find((x: Rec) => x.hex === a.hex);
    if (f && typeof f.seen === 'number') ageMax = Math.max(ageMax, Math.abs((a.seen as number) - f.seen));
  }
  // B: joins at JOIN with the keyframe the server would send it
  if (tick >= JOIN) {
    const rb = B.take(tick === JOIN ? L.k : msg, t0 + tick * 1000);
    if (rb.report && sameAircraft(L.full.aircraft, rb.report.aircraft as Rec[], `B tick ${tick}`)) bOk++;
  }
  // D: what the app draws — the parsed report and its list order, from either
  if (ra.report) {
    const pf = parseAdsbMessage(L.full), pd = parseAdsbMessage(ra.report);
    const rx = { lat: 52.29, lon: -0.85 };
    const qf = { ...pf, aircraft: pf.aircraft.map((a: any) => quant(a)) };
    // ★ equal distances keep their input order (a stable sort): the full list comes in the server's hash order, the rebuilt
    //   one in id order — neither means anything, so both go in by address and only a real difference shows
    const byHex = (l: any[]) => [...l].sort((x, y) => (x.hex < y.hex ? -1 : x.hex > y.hex ? 1 : 0));
    const lf = adsbToAircraft(byHex(qf.aircraft) as any, rx).map((a: any) => a.icao), ld = adsbToAircraft(byHex(pd.aircraft), rx).map((a: any) => a.icao);
    const em = pd.aircraft.filter((a: any) => a.emergency).map((a: any) => a.hex + ':' + a.emergency + ':' + a.squawk).join();
    const emF = pf.aircraft.filter((a: any) => a.emergency).map((a: any) => a.hex + ':' + a.emergency + ':' + a.squawk).join();
    if (em) emergSeen++;
    if (JSON.stringify(lf) === JSON.stringify(ld) && em === emF && pd.aircraft.length === pf.aircraft.length) dOk++;
    else if (fails.length < 25) fails.push(`D tick ${tick}: list order or emergencies differ (${emF} vs ${em})`);
  }
  // C: as A, but the delta of tick DROP never arrives; on its resync request it gets that tick's keyframe
  if (tick === DROP && !L.keyAll) { cSkipped++; continue; }
  const rc = C.take(tick === 0 ? L.k : msg, t0 + tick * 1000);
  if (rc.resync) {
    cResync++;
    const rk = C.take(L.k, t0 + tick * 1000);   // the server's answer: the table as it stands
    if (rk.report && sameAircraft(L.full.aircraft, rk.report.aircraft as Rec[], `C resync ${tick}`)) cOk++;
  } else if (rc.report && sameAircraft(L.full.aircraft, rc.report.aircraft as Rec[], `C tick ${tick}`)) cOk++;
}
const N = lines.length;
console.log(`   up to ${maxAc} aircraft; ${keyAlls} keyframes for everyone in ${N} ticks; largest age difference ${ageMax.toFixed(2)} s`);
ok(`A: every tick matches the full list (${aOk}/${N})`, aOk === N);
ok(`B: joined at ${JOIN}, every tick from there matches (${bOk}/${N - JOIN})`, bOk === N - JOIN);
ok(`C: the lost delta asked for one resync (${cResync})`, cResync === 1 && cSkipped === 1);
ok(`C: every tick it drew matches, the resync included (${cOk}/${N - 1})`, cOk === N - 1);
ok(`D: the app's list order and emergencies are the same as from the full list (${dOk}/${N})`, dOk === N);
ok(`D: the 7700 emergency was drawn (${emergSeen} ticks)`, emergSeen >= 150);
ok('keyframes every 60 s', keyAlls >= Math.floor(N / 60));

// ── E: junk ──
{
  const S = new AdsbDeltaStore();
  const k = lines[10].k, d = lines[11].d;
  S.take(k, 0);
  const before = JSON.stringify((S as any).rows.size);
  ok('a delta with a broken row asks for a resync', S.take({ ...d, u: [0, 1, 'x'] }, 5000).resync === true);
  ok('…and the next one is not applied over it (asks again only after 2 s)', S.take(lines[12].d, 6000).resync === false);
  ok('a keyframe brings it back', !!S.take(lines[12].k, 9000).report && JSON.stringify(S.size) !== 'null' && before !== '');
  const T = new AdsbDeltaStore();
  ok('a delta before any keyframe asks for one', T.take(d, 0).resync === true);
  ok('a second within 2 s does not ask again', T.take(lines[12].d, 1000).resync === false);
  ok('nonsense types are refused', T.take({ type: 'adsb_k', s: 'x', t: 1, h: {}, a: [] }, 5000).report === null);
  ok('an id out of range is refused', new AdsbDeltaStore().take({ type: 'adsb_k', s: 1, t: 1, h: {}, a: [-1, 'abcdef', 0] }, 0).report === null);
  ok('a hex longer than 8 is refused', new AdsbDeltaStore().take({ type: 'adsb_k', s: 1, t: 1, h: {}, a: [1, 'abcdefabc', 0] }, 0).report === null);
  const U = new AdsbDeltaStore(); U.take(k, 0);
  ok('an old delta (already in the keyframe) is ignored quietly', U.take(lines[10].d, 0).report === null && U.take(lines[10].d, 0).resync === false);
}

// ── bytes ──
{
  let f = 0, dd = 0, kk = 0, n = 0;
  for (const L of lines.slice(120)) { f += JSON.stringify(L.full).length; dd += JSON.stringify(L.keyAll ? L.k : L.d).length; kk += JSON.stringify(L.k).length; n++; }
  console.log(`   bytes a tick: full list ${(f / n / 1024).toFixed(2)} KB, as sent to a delta listener ${(dd / n / 1024).toFixed(2)} KB (keyframe ${(kk / n / 1024).toFixed(2)} KB)`);
  ok('a delta listener gets under a sixth of the full list', dd < f / 6);
}

for (const f of fails) console.error('FAIL ' + f);
console.log(`\ntest_adsb_delta: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
