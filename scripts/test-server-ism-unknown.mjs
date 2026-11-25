// test-server-ism-unknown.mjs — beta94 ISM's UNIDENTIFIED BURSTS and the ISM AUTO chip through the REAL server. A fake rtl_tcp
// replays `test-ism-unknown --write-clip` (an OOK burst at 433.80, an FSK burst at 434.05, a steady carrier at 433.65 MHz —
// nothing any decoder names):
//   1. every listener's report DESCRIBES the bursts (modulation, coding, rate, length, frequency, level) and flags which have
//      samples kept (`iq`) — and carries no samples itself; the carrier is listed under `steady`, not as an unidentified burst;
//   2. the chip's figures ride the same report: `gain` (ISM's own rule on the radio's gain list), `gainAuto` 1, `peak`, `clipPct`;
//   3. THE SAMPLES ARE THE OWNER'S: /vibeserver/admin/ism-clip?id=N with no credentials → 401, with a wrong password → 401,
//      with the admin proof → the file, named ism_<MHz>M_<rate>k_<UTC>.cu8, as many bytes as the report says; an id no longer
//      kept → 404.
// Silent: no audio socket is opened.
//   VIBESERVER_BIN=…/vibeserver VIBE_ISM_UNK_CLIP=…/clip.u8 node scripts/test-server-ism-unknown.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_ISM_UNK_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_ISM_UNK_CLIP (make one: test-ism-unknown --write-clip clip.u8 434130000 2400000)');
  process.exit(3);
}
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 100; i++) { if (await f()) return true; await sleep(100); } return !!(await f()); };
const ADMIN = 'ismunk-admin';

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-ismunk-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, name: 'ISM unknown test', locator: 'IO92nh', adminPass: ADMIN,
  radios: [{ serial: 'ismunk', label: 'Test radio', enabled: true, configured: true, users: 2, rate: 2400000 }],
}));
const rtlPort = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--ism', CLIP], { stdio: 'ignore' });
await sleep(400);
const port = await freePort();
const logPath = path.join(dir, 'server.log');
const log = fs.openSync(logPath, 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', '2'],
  { env: { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') },
    stdio: ['ignore', log, log] });
const base = `http://127.0.0.1:${port}`;
const adminQ = async (pw = ADMIN) => { const n = (await fetch(`${base}/vibeserver/auth`).then((r) => r.json())).nonce;
  return `vs_admin_nonce=${n}&vs_admin_auth=${crypto.createHmac('sha256', pw).update(n).digest('hex')}`; };
function spectrum(id) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=${id}&bins=256&proto=1`);
  ws.binaryType = 'arraybuffer';
  const st = { last: null, raw: '', ws };
  ws.onmessage = (e) => { if (typeof e.data !== 'string') return; const j = JSON.parse(e.data); if (j.type === 'ism') { st.last = j; st.raw = e.data; } };
  st.open = new Promise((r) => { ws.onopen = r; });
  return st;
}

try {
  ok(await until(async () => { try { return (await fetch(`${base}/vibeserver.json`, { signal: AbortSignal.timeout(2000) })).ok; } catch { return false; } }, 15000), 'the server is up');
  const a = spectrum('ismunk01');
  await a.open;
  await sleep(800);
  a.ws.send(JSON.stringify({ type: 'ism', on: 1, band: '433' }));
  const unk = () => (a.last && Array.isArray(a.last.unk) ? a.last.unk : []);
  const has = (mod, mhz) => unk().some((u) => u.mod === mod && Math.abs(u.hz - mhz * 1e6) < 15e3);
  ok(await until(() => has('OOK', 433.80) && has('FSK', 434.05), 20000), `both bursts described: ${unk().map((u) => `${u.mod} ${(u.hz / 1e6).toFixed(4)} ${u.code} ${u.rate} ${u.ms} ms`).join(' | ')}`);
  const o = unk().find((u) => u.mod === 'OOK'), f = unk().find((u) => u.mod === 'FSK');
  ok(!!o && Math.abs(o.rate - 3333) < 170 && o.code === 'NRZ/Manchester' && Math.abs(o.ms - 36.6) < 4, `the OOK burst: ${o && `${o.code} ${o.rate} Bd ${o.ms} ms`} (want NRZ 3333 Bd, ~36.6 ms)`);
  ok(!!f && Math.abs(f.rate - 4800) < 240 && Math.abs(f.dev - 30000) < 7500, `the FSK burst: ${f && `${f.rate} Bd ±${f.dev} Hz`} (want 4800 Bd, ±30 kHz)`);
  ok(unk().every((u) => typeof u.lvl === 'number' && typeof u.snr === 'number' && u.mod !== 'carrier'), 'each with a level and SNR; no carrier among them');
  ok(await until(() => (a.last?.steady || []).some((c) => Math.abs(c.hz - 433.65e6) < 20e3), 15000), `the carrier at 433.65 is listed as steady: ${JSON.stringify(a.last?.steady)}`);
  ok((a.last?.steadyN ?? 0) >= 1 && a.last.unknown <= a.last.bursts - a.last.steadyN, `carriers not counted as unidentified (bursts ${a.last?.bursts}, unidentified ${a.last?.unknown}, carriers ${a.last?.steadyN})`);
  ok(unk().some((u) => u.iq === 1 && u.kb > 0), 'their samples are kept (iq, kb)');
  ok(!/"data"|"bytes"|"iqData"/.test(a.raw) && a.raw.length < 64 * 1024, `the report carries no samples (${a.raw.length} bytes)`);
  // ── the chip ──
  ok(typeof a.last.gain === 'number' && a.last.gain >= 0 && a.last.gainAuto === 1, `ISM AUTO's gain on the report: ${a.last.gain} dB, gainAuto ${a.last.gainAuto}`);
  ok(typeof a.last.peak === 'number' && a.last.peak <= 0.1 && a.last.peak > -60 && typeof a.last.clipPct === 'number', `peak ${a.last.peak} dBFS, near full scale ${a.last.clipPct} %`);
  // ── the owner's download ──
  const it = unk().find((u) => u.iq === 1);
  const url = (q) => `${base}/vibeserver/admin/ism-clip?id=${it.id}${q ? '&' + q : ''}`;
  let r = await fetch(url(''));
  ok(r.status === 401, `no credentials → ${r.status}`);
  r = await fetch(url(await adminQ('not-the-password')));
  ok(r.status === 401, `a wrong password → ${r.status}`);
  r = await fetch(url(await adminQ()));
  const cd = r.headers.get('content-disposition') || '';
  const buf = r.ok ? Buffer.from(await r.arrayBuffer()) : Buffer.alloc(0);
  ok(r.status === 200 && /^attachment; filename="ism_43[34]\.\d{4}M_\d+k_\d{8}T\d{6}Z\.cu8"$/.test(cd), `the owner gets the file: ${r.status} ${cd}`);
  const mhz = Number((cd.match(/ism_([\d.]+)M_/) || [])[1]);
  ok(Math.abs(mhz * 1e6 - it.hz) < 100, `its name carries the burst's frequency (${mhz} MHz vs ${it.hz})`);
  ok(buf.length > 0 && buf.length % 2 === 0 && Math.ceil(buf.length / 1024) === it.kb, `${buf.length} bytes of cu8 (the report said ${it.kb} KB)`);
  const lo = Math.min(...buf), hi = Math.max(...buf);
  ok(hi >= 220 && lo <= 35, `scaled to its peak (bytes ${lo}..${hi})`);
  r = await fetch(`${base}/vibeserver/admin/ism-clip?id=999999&${await adminQ()}`);
  ok(r.status === 404, `an id no longer kept → ${r.status}`);
  a.ws.send(JSON.stringify({ type: 'ism', on: 0 }));
  await sleep(500);
  a.ws.close();
} catch (e) {
  ok(false, `threw: ${e && e.stack}`);
} finally {
  srv.kill('SIGTERM');
  rtl.kill('SIGTERM');
  await sleep(500);
  if (fail) { try { console.log(fs.readFileSync(logPath, 'utf8').split('\n').filter((l) => /ISM|admin/.test(l)).slice(-20).join('\n')); } catch {} }
  try { fs.rmSync(dir, { recursive: true, force: true }); } catch {}
}
console.log(`   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
