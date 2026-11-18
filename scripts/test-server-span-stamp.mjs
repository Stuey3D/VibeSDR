// test-server-span-stamp.mjs — V12 beta83 THE SPAN STAMP, the server half (NickB, HF+ Dual Port, 2026-11-18: after a keypad
// jump 7285 → 585 kHz the waterfall's span stayed wrong until he tuned or zoomed). A SPEC frame said the centre its bins
// were built for but never their span. Against the REAL server and a fake rtl_tcp:
//   1. a socket that asks `span=1` gets version-2 frames (header 30: the f64 Hz per bin at 22); one that does not ask gets
//      exactly the version-1 frames it always got (header 22) — same bin count, same rate;
//   2. on every path — the whole capture (the wide crop), a medium zoom (the crop), a narrow zoom (the zoom channel) — once
//      settled, the stamp IS the config's binBandwidth: a healthy server never makes the client's heal fire;
//   3. through big jumps (7285 → 585 kHz, zoomed, the keypad way) every stamp is a span the server really configured — the
//      stamp never invents one — and the frames settle on the last config's span;
//   4. the client sees the same SPEC rate either way (no extra frames).
//   VIBESERVER_BIN=…/vibeserver node scripts/test-server-span-stamp.mjs      (silent: no audio socket is opened)
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — VIBESERVER_BIN needed'); process.exit(3); }
const RATE = 2400000, CENTRE = 1000000, N = 1024;
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-stamp-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Span stamp test', locator: 'IO92nh', adminPass: 'span-stamp-admin',
  radios: [{ serial: 'span-stamp', label: 'Test radio', enabled: true, configured: true, users: 2 }] }));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, ['--max-old-space-size=256', path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--tones', '0', '--carrier', String(CENTRE + 100000), '--carrier-amp', '20'], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(CENTRE), '--mode', 'am', '--users', '2'],
                  { env, stdio: ['ignore', ...Array(2).fill(fs.openSync(path.join(dir, 'server.log'), 'w'))] });

/** A spectrum listener: every SPEC frame (version, length, stamp, centre) and every config's binBandwidth. */
function listener(sid, stamp) {
  const L = { frames: [], cfgs: [], lastCfg: null };
  const w = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=${sid}&mode=binary8&bins=${N}&proto=1${stamp ? '&span=1' : ''}`);
  w.binaryType = 'arraybuffer';
  w.onmessage = (e) => {
    if (typeof e.data === 'string') {
      try { const m = JSON.parse(e.data); if (m.type === 'config') { L.cfgs.push(m.binBandwidth); L.lastCfg = m; } } catch {}
      return;
    }
    const b = new Uint8Array(e.data);
    if (String.fromCharCode(b[0], b[1], b[2], b[3]) !== 'SPEC') return;
    const dv = new DataView(e.data);
    L.frames.push({ ver: b[4], len: b.length, centre: Number(dv.getBigUint64(14, true)), binHz: b[4] === 2 ? dv.getFloat64(22, true) : 0, at: Date.now() });
  };
  L.ws = w;
  L.send = (o) => w.send(JSON.stringify(o));
  L.open = new Promise((r) => { w.onopen = r; });
  L.since = (t) => L.frames.filter((f) => f.at >= t);
  return L;
}
const near = (a, b) => Math.abs(a - b) <= Math.abs(b) * 1e-6;

const deadline = setTimeout(() => { console.log('   FAIL deadline'); srv.kill('SIGTERM'); rtl.kill('SIGTERM'); process.exit(1); }, 120000);
try {
  for (let i = 0; i < 60; i++) { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`); if (r.ok) break; } catch {} await sleep(250); }
  const A = listener('stampA0001', true), B = listener('stampB0001', false);
  await Promise.all([A.open, B.open]);
  await sleep(1500);
  A.send({ type: 'tune', frequency: CENTRE, mode: 'am' });
  await sleep(2500);

  // ── 1. the formats ──
  {
    const t = Date.now(); await sleep(3000);
    const a = A.since(t), b = B.since(t);
    ok(a.length > 6 && a.every((f) => f.ver === 2 && f.len === 30 + N), `span=1: version-2 frames, header 30 (${a.length} frames, ${[...new Set(a.map((f) => f.ver + '/' + f.len))]})`);
    ok(b.length > 6 && b.every((f) => f.ver === 1 && f.len === 22 + N), `no span=1: the version-1 frame it always got, header 22 (${b.length} frames, ${[...new Set(b.map((f) => f.ver + '/' + f.len))]})`);
    ok(Math.abs(a.length - b.length) <= Math.max(2, b.length * 0.15), `the same rate either way (${(a.length / 3).toFixed(1)} vs ${(b.length / 3).toFixed(1)} SPEC/s)`);
  }

  // ── 2. every path, settled: the stamp is the config's span ──
  const settled = async (what, msg) => {
    if (msg) A.send(msg);
    await sleep(2500);
    const t = Date.now(); await sleep(1500);
    const a = A.since(t), cfg = A.lastCfg;
    ok(!!cfg && a.length > 3 && a.every((f) => near(f.binHz, cfg.binBandwidth)),
       `${what}: every stamp = the config's ${cfg && cfg.binBandwidth.toFixed(4)} Hz/bin (${[...new Set(a.map((f) => f.binHz.toFixed(4)))].join(', ')})`);
    return cfg;
  };
  await settled('the whole capture (wide crop)');
  await settled('a 600 kHz zoom (the crop)', { type: 'zoom', frequency: CENTRE, binBandwidth: 600000 / N });
  await settled('a 20 kHz zoom (the zoom channel)', { type: 'zoom', frequency: CENTRE, binBandwidth: 20000 / N });

  // ── 3. big jumps, zoomed, the keypad way (a tune, then the view at the same span around it) ──
  const t0 = Date.now(), cfg0 = A.cfgs.length;
  for (const f of [7285000, 585000, 7285000, 585000]) {
    A.send({ type: 'tune', frequency: f, mode: 'am' });
    A.send({ type: 'zoom', frequency: f, binBandwidth: 20000 / N });
    await sleep(1200);
  }
  const last = await settled('after four 7285 ⇄ 585 kHz jumps, settled');
  const seen = [...A.cfgs.slice(Math.max(0, cfg0 - 1))];
  const jumped = A.since(t0);
  const odd = jumped.filter((f) => !seen.some((c) => near(f.binHz, c)));
  ok(jumped.length > 10 && odd.length === 0,
     `through the jumps every stamp is a span the server configured (${jumped.length} frames, ${odd.length} with a span no config had${odd.length ? ': ' + [...new Set(odd.map((f) => f.binHz))].join(', ') : ''})`);
  ok(!!last && Math.abs(last.centerFreq - 585000) < 20000, `…and the view is at 585 kHz (${last && last.centerFreq})`);

  {
    const log = fs.readFileSync(path.join(dir, 'server.log'), 'utf8');
    ok(/zoom spectrum: \d+ frames sent/.test(log), 'the narrow zoom really went through the zoom channel (its first-frame line is in the log)');
  }
  // ── 4. back out to the whole capture ──
  await settled('zoomed back out', { type: 'zoom', frequency: 585000, binBandwidth: RATE / N });
  A.ws.close(); B.ws.close();
} catch (e) { ok(false, `driver: ${e && e.stack || e}`); }
finally {
  clearTimeout(deadline);
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(500);
  if (!fail) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
}
console.log(`   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
