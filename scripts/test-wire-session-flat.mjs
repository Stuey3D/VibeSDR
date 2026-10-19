// ★★★ A LONG SESSION MUST NOT COST MORE THAN A SHORT ONE (2026-10-19). Stuart's app read 93-103 KB/s in DAB after
// 23 minutes of tuning (12 multiplexes, DAB ↔ FM, a second listener in and out) and 45 KB/s on the same multiplex
// after a server restart. Reproduced on the bench: an Advanced RDS subscription made on FM was never ended by the
// mode, so every rdsx (5 a second, eye grids ~9 KB each) went on arriving in DAB — 28-44 KB/s of a stale analyser.
// The server now sends rdsx only to a listener whose demodulator is WFM (local_sdr_shim.cpp, the rdsx send loop).
//   1. on FM with rdsx on, the analyser arrives (it still works);
//   2. entering DAB with it never turned off: no rdsx;
//   3. back on FM, it resumes with no message from the client (the subscription is kept);
//   4. on AM: no rdsx;
//   5. after a session's churn — 12 multiplexes up and back, DAB → FM → DAB, a second listener joining and leaving,
//      the Signal pane opened and closed, rdsx left on — a dab=2 socket's per-type bytes equal the baseline's
//      (each type within 15 %, the total within 10 %).
// ★ Real server + the DAB bench clip (vibeserver/bench-clip/dab-bench-clean.vbu8, make-clip.sh) + a synthetic
//   WFM station from fake-rtl-tcp. Without the clip or VIBESERVER_BIN this reports NOT RUN, never a pass.
//   Silent (no audio socket), loopback only, the server under nice. ~2 minutes.
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = path.join(root, 'vibeserver/bench-clip/dab-bench-clean.vbu8');
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN)'); process.exit(3); }
if (!fs.existsSync(CLIP)) { console.log('   (not run — no bench clip; vibeserver/bench-clip/make-clip.sh builds it)'); process.exit(3); }

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-flat-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Flat session', locator: 'IO92nh',
  adminPass: 'flat-admin', radios: [{ serial: 'flat', label: 'T', enabled: true, configured: true, users: 4 }] }));
const raw = path.join(dir, 'clip.u8');
fs.writeFileSync(raw, fs.readFileSync(CLIP).subarray(28));   // VIBEBU81 header
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn('nice', ['-n', '10', process.execPath, path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort),
  '--rate', '2048000', '--dab', raw, '--wfm', '96.1'], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const log = fs.openSync(path.join(dir, 'server.log'), 'w');
const srv = spawn('nice', ['-n', '10', BIN, '--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000',
  '--mode', 'wfm', '--users', '4'], { env, stdio: ['ignore', log, log] });

const BLOCKS = ['5A','5B','5C','5D','6A','6B','6C','6D','7A','7B','7C','7D','8A','8B','8C','8D','9A','9B','9C','9D','10A','10N','10B','10C','10D','11A','11N','11B','11C','11D','12A','12N','12B','12C','12D'];
const ch = (b) => BLOCKS.indexOf(b);

// An app-style listener: dab=2, 1024 bins, eyeEvery 2 on its rdsx — counts every message by type.
function listener(tag) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=${tag}${Date.now()}&mode=binary8&bins=1024&client=VibeSDR&proto=1&dab=2`);
  ws.binaryType = 'arraybuffer';
  const c = { ws, stats: new Map(), seen: [], opened: null };
  c.opened = new Promise((r) => { ws.onopen = r; });
  ws.onmessage = (e) => {
    let t = 'binary', n = 0;
    if (typeof e.data === 'string') { n = Buffer.byteLength(e.data); try { t = JSON.parse(e.data).type ?? '?'; } catch { t = '?'; } }
    else n = e.data.byteLength;
    const s = c.stats.get(t) || { n: 0, b: 0 }; s.n++; s.b += n; c.stats.set(t, s);
    c.seen.push(t);
  };
  c.send = (o) => ws.send(JSON.stringify(o));
  c.ping = setInterval(() => { try { ws.send('{"type":"ping"}'); } catch {} }, 5000);
  c.count = async (secs) => { c.stats = new Map(); const t0 = Date.now(); await sleep(secs * 1000); const dt = (Date.now() - t0) / 1000;
    const out = {}; for (const [k, s] of c.stats) out[k] = { rate: s.n / dt, kbs: s.b / dt / 1024 }; return out; };
  c.close = () => { clearInterval(c.ping); ws.close(); };
  return c;
}
const kb = (m, k) => (m[k]?.kbs ?? 0);
const total = (m) => Object.entries(m).filter(([k]) => k !== 'pong').reduce((a, [, v]) => a + v.kbs, 0);

try {
  for (let i = 0; i < 100; i++) {
    if (await new Promise((r) => { const s = net.connect(port, '127.0.0.1'); s.on('connect', () => { s.end(); r(true); }); s.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const a = listener('flat');
  await a.opened;

  console.log('── Advanced RDS follows the mode ──');
  a.send({ type: 'tune', frequency: 96100000 }); a.send({ type: 'mode', mode: 'wfm' });
  a.send({ type: 'rdsx', on: 1, eyeEvery: 2 });
  await sleep(2500);
  let m = await a.count(4);
  ok((m.rdsx?.rate ?? 0) > 3, `on FM, rdsx arrives (${(m.rdsx?.rate ?? 0).toFixed(1)}/s, ${kb(m, 'rdsx').toFixed(1)} KB/s)`);

  a.send({ type: 'dab', on: 1, channel: ch('12D') });
  await sleep(6000);
  m = await a.count(5);
  ok(!m.rdsx && (m.dab?.rate ?? 0) > 1, `in DAB with rdsx never turned off: none sent (${(m.rdsx?.rate ?? 0).toFixed(1)}/s), dab ${(m.dab?.rate ?? 0).toFixed(1)}/s`);

  a.send({ type: 'dab', on: 0 });
  await sleep(1500);
  a.send({ type: 'tune', frequency: 96100000 }); a.send({ type: 'mode', mode: 'wfm' });
  await sleep(3000);
  m = await a.count(4);
  ok((m.rdsx?.rate ?? 0) > 3, `back on FM it resumes with no message from the client (${(m.rdsx?.rate ?? 0).toFixed(1)}/s)`);

  a.send({ type: 'mode', mode: 'am' });
  await sleep(1500);
  m = await a.count(4);
  ok(!m.rdsx, `on AM: none sent (${(m.rdsx?.rate ?? 0).toFixed(1)}/s)`);
  a.send({ type: 'mode', mode: 'wfm' });
  await sleep(1000);

  console.log('── a session\'s churn costs nothing afterwards ──');
  a.send({ type: 'dab', on: 1, channel: ch('12D') });
  await sleep(10000);
  const before = await a.count(10);
  const span = BLOCKS.slice(ch('10C'), ch('12C') + 1);
  for (const b of [...span, ...span.slice().reverse()]) { a.send({ type: 'dab', on: 1, channel: ch(b) }); await sleep(700); }
  a.send({ type: 'dab', on: 0 }); await sleep(1500);
  a.send({ type: 'tune', frequency: 96100000 }); a.send({ type: 'mode', mode: 'wfm' }); await sleep(2500);
  a.send({ type: 'dab', on: 1, channel: ch('12D') }); await sleep(3000);
  const b2 = listener('two'); await b2.opened; await sleep(3000); b2.close();
  a.send({ type: 'dab_scopes', on: 1 }); await sleep(2000); a.send({ type: 'dab_scopes', on: 0 });
  a.send({ type: 'dab', on: 1, channel: ch('12D') });
  await sleep(10000);
  const after = await a.count(10);
  const fmt = (x) => Object.entries(x).sort((p, q) => q[1].kbs - p[1].kbs).map(([k, v]) => `${k} ${v.kbs.toFixed(2)}`).join(', ');
  console.log(`      before: ${fmt(before)}  = ${total(before).toFixed(1)} KB/s`);
  console.log(`      after:  ${fmt(after)}  = ${total(after).toFixed(1)} KB/s`);
  const types = new Set([...Object.keys(before), ...Object.keys(after)].filter((k) => k !== 'pong' && k !== 'dab_dls' && k !== 'dab_list'));
  const grown = [...types].filter((k) => kb(after, k) > kb(before, k) * 1.15 + 0.1);
  ok(grown.length === 0, `no message type grew (${grown.length ? grown.join(', ') : 'none'})`);
  ok(total(after) <= total(before) * 1.10, `the total stayed flat: ${total(before).toFixed(1)} → ${total(after).toFixed(1)} KB/s`);
  ok(!after.rdsx, 'and no Advanced RDS left running in DAB');
  a.close();
} finally {
  try { srv.kill('SIGTERM'); } catch {}
  try { rtl.kill('SIGTERM'); } catch {}
}
console.log(`test-wire-session-flat: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
