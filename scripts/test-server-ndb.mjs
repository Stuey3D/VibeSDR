// test-server-ndb.mjs — V12 NDB mode through the REAL server: a fake rtl_tcp (auto direct sampling, so the RTL reaches LF)
// replaying a synthetic 2.4 MS/s clip of the beacon band (`test-ndb --write-clip`: WOD on 352.0 kHz, CAM on 333.0 kHz), a
// spectrum socket, and the contract of docs/v12/NDB-SKIMMER.md: the capability (ndb, ndbLo/ndbHi, modes), entering, the
// radio on the band, both beacons identified and named from the published list, the
// radio held there (a tune ignored — ADS-B's shape), the waterfall at 1 fps, confidence on every reading, Find NDB, the
// stats, and leaving with the receiver given back exactly as found. Also: an
// RTL WITHOUT direct sampling offers no NDB at all (no dead control). Silent: no audio socket is opened.
//   VIBESERVER_BIN=…/vibeserver VIBE_NDB_CLIP=…/clip.u8 node scripts/test-server-ndb.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_NDB_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_NDB_CLIP (make one: test-ndb --write-clip clip.u8 362750 2400000 10)');
  process.exit(3);
}
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 100; i++) { if (await f()) return true; await sleep(100); } return !!(await f()); };

async function server(autoDs) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-ndb-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'NDB test', locator: 'IO92nh', adminPass: 'ndb-test-admin',
    radios: [{ serial: 'ndb-test', label: 'Test radio', enabled: true, configured: true, users: 1, autoDirectSampling: autoDs }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000',
                                       '--hf', CLIP, '--hf-centre', '362750', '--tones', '0'], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '7074000', '--mode', 'usb', '--users', '1'],
                    { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  const stop = () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill(); } catch {} };
  return { dir, port, status, stop, srv };
}

let s = await server(false);
try {
  ok(await until(async () => !!(await s.status()), 15000), 'an RTL without direct sampling: the server is up');
  const st = await s.status();
  ok(st.ndb === undefined && !(st.modes || []).includes('ndb'), `…and offers NO NDB at all (ndb ${JSON.stringify(st.ndb)})`);
} finally { s.stop(); await sleep(600); fs.rmSync(s.dir, { recursive: true, force: true }); }

s = await server(true);
try {
  ok(await until(async () => !!(await s.status()), 15000), 'with automatic direct sampling: the server is up');
  const st = await s.status();
  ok(st.ndb === true, `vibeserver.json: ndb ${JSON.stringify(st.ndb)} ${st.ndbWhy || ''}`);
  ok(st.ndbLo === 190000 && st.ndbHi === 535000, `ndbLo/ndbHi ${st.ndbLo}–${st.ndbHi}`);
  ok(Array.isArray(st.modes) && st.modes.includes('ndb'), 'modes lists ndb');

  const ws = new WebSocket(`ws://127.0.0.1:${s.port}/ws/user-spectrum?user_session_id=ndbtest01&bins=256`);
  ws.binaryType = 'arraybuffer';
  let last = null, lastCfg = null, off = false, err = null, found = null, stats = null;
  ws.onmessage = (e) => {
    if (typeof e.data !== 'string') return;
    const j = JSON.parse(e.data);
    if (j.type === 'ndb') last = j;
    if (j.type === 'ndb_off') off = true;
    if (j.type === 'ndb_error') err = j.why;
    if (j.type === 'ndb_found') found = j;
    if (j.type === 'ndb_stats') stats = j;
    if (j.type === 'config') lastCfg = j;
  };
  await new Promise((r) => { ws.onopen = r; });
  await sleep(1200);
  const before = lastCfg && { c: lastCfg.centerFreq, v: lastCfg.vfo, m: lastCfg.mode };

  ws.send(JSON.stringify({ type: 'ndb', on: 1 }));
  ok(await until(() => !!last, 5000), 'an ndb report arrives');
  ok(last && last.lo === 190000 && last.hi === 535000, `the band searched: ${last && last.lo}–${last && last.hi}`);
  ok(await until(() => lastCfg && Math.abs(lastCfg.centerFreq - 362750) < 1, 5000), `the radio is on the band: centre ${lastCfg && lastCfg.centerFreq}`);
  const named = (id, name) => !!last && last.beacons.some((b) => b.ident === id && b.db && b.db.k === 'match' && b.db.name === name);
  const t0 = Date.now();
  ok(await until(() => named('WOD', 'Woodley') && named("CAM", "Cambridge"), 150000),
     `WOD (Woodley) and CAM (Cambridge) identified and named in ${((Date.now() - t0) / 1000).toFixed(0)} s: ${last ? last.beacons.filter((b) => b.ident).map((b) => `${b.ident}@${b.hz}`).join(' ') : '-'}`);
  const wod = last && last.beacons.find((b) => b.ident === 'WOD');
  ok(wod && Math.abs(wod.hz - 352000) < 30 && Math.abs(wod.tone - 400) < 25, `WOD's carrier ${wod && wod.hz} Hz, tone ${wod && wod.tone} Hz`);
  ok(wod && wod.db.km > 50 && wod.db.km < 150, `WOD's distance from IO92nh: ${wod && wod.db.km} km`);
  ok(last && last.identified === 2 && last.beacons.filter((b) => b.ident).length === 2, `nothing else identified (${last && last.identified})`);
  ok(last && Array.isArray(last.log) && last.log.some((e) => e.ident === 'WOD'), 'the heard log has WOD');

  // ★ ADS-B's shape (2026-10-27): the receiver is the mode's — a tune is ignored, the radio stays on the band
  ws.send(JSON.stringify({ type: 'tune', frequency: 7074000 }));
  await sleep(1500);
  ok(lastCfg && Math.abs(lastCfg.centerFreq - 362750) < 1, `a tune is ignored: radio still ${lastCfg && lastCfg.centerFreq}`);
  const logTxt = fs.readFileSync(path.join(s.dir, 'server.log'), 'utf8');
  ok(/engine fft rate: 1\.0 fps/.test(logTxt) || /fft rate [\d.]+ -> 1\.0 fps/.test(logTxt), 'the waterfall is throttled to 1 fps behind the map');
  ok(/tune ignored — NDB owns the receiver/.test(logTxt), 'the server says why the tune was ignored');
  ok(last && last.copy.length > 0 && last.copy.every((c) => ['high', 'medium', 'low', 'poor'].includes(c.q)), `every live reading carries its confidence: ${last && last.copy.slice(-3).map((c) => c.text + '/' + c.q).join(' ')}`);
  ws.send(JSON.stringify({ type: 'ndb_find', q: 'woodley', limit: 10 }));
  ok(await until(() => !!found, 3000) && found.list.some((f) => f.ident === 'WOD' && f.heard === true), 'Find NDB finds Woodley, marked heard');
  ws.send(JSON.stringify({ type: 'ndb_stats' }));
  ok(await until(() => !!stats, 3000) && stats.beacons.some((b) => b.ident === 'WOD'), 'the stats carry WOD');

  ws.send(JSON.stringify({ type: 'ndb', on: 0 }));
  ok(await until(() => off, 4000), 'ndb_off');
  ok(await until(() => before && lastCfg && lastCfg.centerFreq === before.c && lastCfg.vfo === before.v && lastCfg.mode === before.m, 5000),
     `the receiver is back exactly as found: ${JSON.stringify(before)} → ${lastCfg && JSON.stringify({ c: lastCfg.centerFreq, v: lastCfg.vfo, m: lastCfg.mode })}`);
  ws.close();
} finally {
  s.stop(); await sleep(800);
  if (fail) console.log(`   (server log kept: ${path.join(s.dir, 'server.log')})`); else fs.rmSync(s.dir, { recursive: true, force: true });
}
console.log(`   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
