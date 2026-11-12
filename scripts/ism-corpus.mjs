#!/usr/bin/env node
// ism-corpus.mjs — how many real recorded ISM devices our decoder reads CORRECTLY: every .cu8 in the rtl_433_tests
// corpus through tools/ism-bench/ism_replay (the real front end + Service::decodeBurst), scored against the corpus's own
// expected output (the .json beside each recording). The corpus is test INPUT only — kept outside the repository
// (~/VibeSDR-test-audio/rtl_433_tests), never committed or shipped; every decoder is our own code.
//
//   node scripts/ism-corpus.mjs [--corpus DIR] [--bin PATH] [--jobs N] [--only SUBSTR] [--out results.json] [--lead S] [--all-formats]
//   (--lead: passed to ism_replay — noise before each recording; a diagnostic, not the score: see ism_replay.cpp)
//
// Per recording:
//   match   we decoded it AND a reading agrees on a value both report (temperature ±0.25 °C, humidity ±1 %, tyre
//           pressure ±2.5 kPa, power ±2 %, wind ±0.5 km/h)
//   decoded we decoded it, but neither side carries a value to compare (a remote, a doorbell, a key fob)
//   wrong   we decoded it, values to compare exist, none agree
//   miss    the corpus decodes it, we do not
//   fp      the corpus decodes nothing there, we report something (a false alarm)
// A protocol folder counts as READ when at least one of its recordings is a match or decoded, and none is wrong.
// ★ Silent; ~8 processes at once (memory: ≤10 on the M5 Pro).
import { spawn } from 'node:child_process';
import { existsSync, readFileSync, readdirSync, statSync, writeFileSync } from 'node:fs';
import { homedir, cpus } from 'node:os';
import path from 'node:path';

const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const CORPUS = arg('--corpus', path.join(homedir(), 'VibeSDR-test-audio/rtl_433_tests/tests'));
const BIN = arg('--bin', path.join(homedir(), 'R36S-debug/ism/ism_replay'));
const JOBS = Number(arg('--jobs', Math.min(8, cpus().length)));
const ONLY = arg('--only', '');
const OUT = arg('--out', '');
const LEAD = arg('--lead', '');
// ★ --all-formats: the .cs8 / .cs16 recordings too (BMW, Schrader MRXBC5A4 …). OFF by default so the headline score
//   stays comparable with every earlier run (it has always been the .cu8 recordings only).
const ALL_FORMATS = process.argv.includes('--all-formats');
if (!existsSync(CORPUS) || !existsSync(BIN)) { console.error(`need the corpus (${CORPUS}) and ism_replay (${BIN})`); process.exit(3); }

const files = [];
(function walk(d) {
  for (const e of readdirSync(d)) {
    const p = path.join(d, e);
    if (statSync(p).isDirectory()) walk(p);
    else if ((e.endsWith('.cu8') || (ALL_FORMATS && /\.cs(8|16)$/.test(e))) && (!ONLY || p.includes(ONLY))) files.push(p);
  }
})(CORPUS);

const folderOf = (f) => path.relative(CORPUS, f).split(path.sep)[0];
const groups = new Map();
for (const f of files) { const k = folderOf(f); if (!groups.has(k)) groups.set(k, []); groups.get(k).push(f); }

function run(list) {
  return new Promise((res) => {
    const p = spawn(BIN, LEAD ? ['--lead', LEAD, ...list] : list, { stdio: ['ignore', 'pipe', 'ignore'] });
    let out = '';
    p.stdout.on('data', (d) => { out += d; });
    p.on('close', () => res(out.split('\n').filter(Boolean).map((l) => { try { return JSON.parse(l); } catch { return null; } }).filter(Boolean)));
  });
}
function expected(f) {
  const j = f.replace(/\.(cu8|cs8|cs16)$/, '.json');
  if (!existsSync(j)) return null;   // no answer key: not scored
  return readFileSync(j, 'utf8').split('\n').map((l) => { try { return JSON.parse(l); } catch { return null; } }).filter((x) => x && x.model);
}
const near = (a, b, tol) => typeof a === 'number' && typeof b === 'number' && Math.abs(a - b) <= tol;
/* ★ THE SAME SENSOR IN A SECOND FORMAT (2026-11-11, the TPMS makes). Multi-make aftermarket tyre sensors (Redi-Sensor
 *  and the like) answer a learn tool in several OEM formats at once, so a recording keyed with ONE format holds real
 *  frames of others. Such a reading is not compared — it is "decoded" — only when ALL of these hold: it is a TPMS
 *  reading of a family whose rtl_433 model names are listed here, none of those names is in the answer key, and its
 *  ID (bench builds of ism_replay print it as "sid") ends with an ID the key gives for that recording (≥ 6 hex
 *  digits) — proof it is the same physical sensor, not a misread. A reading of a family the key DOES list is always
 *  compared, so a wrong scale in our own decoder still counts as WRONG. */
const RTL_NAMES = {
  'Schrader': ['Schrader'], 'Schrader EG53MA4': ['Schrader-EG53MA4'], 'Schrader SMD3MA4 / 3039': ['Schrader-SMD3MA4', 'Schrader-NIS315G3'], 'Porsche 987': ['Porsche'],
  'Toyota PMV-C210': ['Toyota'], 'Toyota PMV-107J': ['PMV-107J'], 'Renault': ['Renault'], 'Renault 0435R': ['Renault-0435R'],
  'Hyundai / Kia (VDO)': ['Hyundai-VDO'], 'EGQ Q85': ['Shenzhen-EGQQ85'], 'Abarth 124 / MX-5 (VDO TG1C)': ['Abarth-124Spider'],
  'PSA / Jeep (VDO)': ['Citroen', 'Jeep'], 'Ford (VDO)': ['Ford'], 'Truck solar TPMS': ['Truck'], 'Steelmate': ['Steelmate'], 'Jansite TY02S': ['Jansite'], 'Jansite Solar': ['Jansite-Solar'], 'Nissan (Schrader FSK)': ['Nissan'], 'GM aftermarket': ['GM-Aftermarket'], 'Honda (TRW)': ['Honda-TRW'], 'Gear Hive': ['Gear-Hive'], 'SmarTire': ['SmarTire-AM'], 'EezTire E618 / Carchet': ['EezTire-E618'], 'Sefis M3': ['Sefis-M3'], 'Mercedes-Benz Sprinter': ['MercedesBenz-Sprinter'],
  'BMW Gen4/Gen5': ['BMW-GEN4', 'BMW-GEN5'], 'Audi pressure alert': ['Audi-PressureAlert'],
  'Elantra 2012 / Civic (TRW)': ['Elantra2012'],
};
function sameSensorOtherFormat(o, exp) {
  const names = RTL_NAMES[o.model];
  if (o.kind !== 'tpms' || !names || !o.sid) return false;
  if (exp.some((e) => names.includes(e.model))) return false;
  const sid = String(o.sid).toLowerCase().replace(/^0+/, '');
  return exp.some((e) => {
    const id = String(e.id ?? '').toLowerCase().replace(/^0+/, '');
    return id.length >= 6 && sid.length >= 6 && (sid.endsWith(id) || id.endsWith(sid));
  });
}
function compare(ours, exp) {
  let comparable = false, agree = false;
  ours = ours.filter((o) => !sameSensorOtherFormat(o, exp));
  for (const o of ours) for (const e of exp) {
    const eT = e.temperature_C ?? (typeof e.temperature_F === 'number' ? (e.temperature_F - 32) / 1.8 : undefined);
    const eK = e.pressure_kPa ?? (typeof e.pressure_PSI === 'number' ? e.pressure_PSI * 6.894757 : undefined)
      ?? (typeof e.pressure_bar === 'number' ? e.pressure_bar * 100 : undefined);
    const eW = e.wind_avg_km_h ?? (typeof e.wind_avg_m_s === 'number' ? e.wind_avg_m_s * 3.6 : undefined);
    const eP = e.power_W ?? e.power0_W;
    const pairs = [[o.temperature_C, eT, 0.25], [o.humidity, e.humidity, 1.01], [o.pressure_kPa, eK, 2.5], [o.wind_avg_km_h, eW, 0.5]];
    for (const [a, b, t] of pairs) if (typeof a === 'number' && typeof b === 'number') { comparable = true; if (near(a, b, t)) agree = true; }
    if (typeof o.power_W === 'number' && typeof eP === 'number') { comparable = true; if (Math.abs(o.power_W - eP) <= Math.max(2, 0.02 * eP)) agree = true; }
  }
  return { comparable, agree };
}

const results = [];
const queue = [...groups.entries()].flatMap(([k, fs]) => { const c = []; for (let i = 0; i < fs.length; i += 12) c.push([k, fs.slice(i, i + 12)]); return c; });
let done = 0;
async function worker() {
  while (queue.length) {
    const [k, list] = queue.shift();
    const lines = await run(list);
    for (const f of list) {
      const ours = lines.filter((l) => l.file === f && !l.done && !l.error);
      const exp = expected(f);
      let status;
      if (exp === null) status = ours.length ? 'decoded-nokey' : 'nokey';
      else if (!exp.length) status = ours.length ? 'fp' : 'quiet';
      else if (!ours.length) status = 'miss';
      else { const c = compare(ours, exp); status = !c.comparable ? 'decoded' : c.agree ? 'match' : 'wrong'; }
      results.push({ folder: k, file: path.relative(CORPUS, f), status, ours: ours.map((o) => `${o.make} ${o.model}`.trim()),
                     exp: exp ? [...new Set(exp.map((e) => e.model))] : [] });
    }
    done += list.length;
    process.stderr.write(`\r  ${done}/${files.length} recordings`);
  }
}
await Promise.all(Array.from({ length: JOBS }, worker));
process.stderr.write('\n');

const by = new Map();
for (const r of results) { if (!by.has(r.folder)) by.set(r.folder, []); by.get(r.folder).push(r); }
const count = (s) => results.filter((r) => r.status === s).length;
const keyed = results.filter((r) => !['nokey', 'decoded-nokey', 'quiet'].includes(r.status) && r.status !== 'fp');
const read = [], partial = [], wrong = [];
for (const [k, rs] of by) {
  const good = rs.filter((r) => r.status === 'match' || r.status === 'decoded').length;
  const bad = rs.filter((r) => r.status === 'wrong').length;
  const tot = rs.filter((r) => ['match', 'decoded', 'wrong', 'miss'].includes(r.status)).length;
  if (bad) wrong.push(`${k} (${bad} wrong of ${tot})`);
  else if (good && good === tot) read.push(`${k} (${good}/${tot})`);
  else if (good) partial.push(`${k} (${good}/${tot})`);
}
console.log(`\nISM corpus: ${files.length} recordings in ${by.size} protocol folders (${keyed.length} with an answer key and a device in them)`);
console.log(`  match ${count('match')} · decoded (nothing to compare) ${count('decoded')} · WRONG ${count('wrong')} · miss ${count('miss')} · false alarms ${count('fp')}`);
console.log(`  folders read fully: ${read.length} · partly: ${partial.length} · with a wrong reading: ${wrong.length} · of ${by.size}`);
console.log(`\n  READ FULLY: ${read.join(', ') || '—'}`);
console.log(`  PARTLY:     ${partial.join(', ') || '—'}`);
console.log(`  WRONG:      ${wrong.join(', ') || '—'}`);
const fps = results.filter((r) => r.status === 'fp' || r.status === 'wrong').slice(0, 40);
if (fps.length) console.log(`\n  false alarms / wrong (first 40):\n${fps.map((r) => `    ${r.status.padEnd(5)} ${r.file}  ours: ${r.ours.join(' | ')}  key: ${r.exp.join(' | ') || '—'}`).join('\n')}`);
if (OUT) writeFileSync(OUT, JSON.stringify({ at: new Date().toISOString(), files: files.length, results }, null, 1));
