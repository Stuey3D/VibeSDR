// test-server-tone.mjs — V12 CTCSS / DCS tone ID through the REAL server: a fake rtl_tcp replaying a synthetic 2 m clip
// (`test-tone-id --write-clip`: three NFM carriers round 145.500 MHz — +25 kHz CTCSS 88.5 Hz under voice, −50 kHz DCS 023
// under voice, +150 kHz voice and NO tone), and the {"type":"tone","label":…} contract on the spectrum socket:
//   • a ONE-PIPELINE (shared dial) radio: tuned in NFM to each carrier the label is the carrier's — "CTCSS 88.5 Hz",
//     "DCS 023N (= 047I)", nothing on the toneless one; a retune clears it first (''); leaving NFM clears it; a listener
//     ARRIVING on a dial whose tone is locked is told at once (sendFullState);
//   • a RANGE-LOCKED radio (per-listener channels): two listeners on two carriers each get their own label, never the
//     other's.
// Silent: no audio is played (an audio socket is opened only so the listener has audio, and its frames are dropped).
//   VIBESERVER_BIN=…/vibeserver VIBE_TONE_CLIP=…/clip.cu8 node scripts/test-server-tone.mjs
//   (make the clip: test-tone-id --write-clip clip.cu8 — the run-tests binary, or any build of vibeserver/test-tone-id.cpp)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_TONE_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_TONE_CLIP (make one: test-tone-id --write-clip clip.cu8)');
  process.exit(3);
}
const RATE = 960000, CENTRE = 145500000;
const CTCSS = 'CTCSS 88.5 Hz', DCS = 'DCS 023N (= 047I)';
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 200; i++) { if (await f()) return true; await sleep(200); } return !!(await f()); };

/** A minimal WebSocket client (the probes' own), recording text and binary frames. */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { bin: [], txt: [], open: false };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; st.open = true; buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const p = buf.subarray(off, off + len); buf = buf.subarray(off + len);
      if (op === 0x9) { const q = Buffer.alloc(6 + p.length); q[0] = 0x8a; q[1] = 0x80 | p.length; q.writeUInt32BE(0, 2); p.copy(q, 6); s.write(q); continue; }
      if (op === 0x2 && !st.dropBin) st.bin.push(Buffer.from(p));
      if (op === 0x1) st.txt.push(p.toString());
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b]));
  };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
const tones = (st) => st.txt.filter((t) => t.includes('"type":"tone"')).map((t) => JSON.parse(t).label);
const lastTone = (st) => { const t = tones(st); return t.length ? t[t.length - 1] : null; };

async function server(args, tag) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-tone-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Tone test', locator: 'IO92nh', adminPass: 'tone-test-admin',
    radios: [{ serial: 'tone-test', label: 'Test radio', enabled: true, configured: true }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', CLIP, '--hf-centre', String(CENTRE)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), ...args], { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  return { port, dir, status, stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}
async function listener(port, sid, f, mode) {
  const L = {};
  L.spec = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`);
  await sleep(400);
  L.aud = ws(port, `/ws/audio?user_session_id=${sid}`); L.aud.dropBin = true;   // ★ silent: frames dropped unread
  await sleep(400);
  if (f) L.spec.send({ type: 'tune', frequency: f, mode });
  await sleep(600);
  L.close = () => { L.spec.close(); L.aud.close(); };
  return L;
}

// ── 1. A one-pipeline radio: the shared dial's one label, to everyone ────────────────────────────────────────────────
{
  console.log('── one-pipeline radio (shared dial) ──');
  const S = await server(['--freq', String(CENTRE + 25000), '--mode', 'nfm', '--users', '2'], 'one');
  try {
    const L = await listener(S.port, 'toneone01', CENTRE + 25000, 'nfm');
    ok(await until(() => lastTone(L.spec) === CTCSS, 15000), `+25 kHz NFM: "${CTCSS}" (got ${JSON.stringify(tones(L.spec))})`);
    // ★ a listener arriving on a locked dial is told at once
    const J = await listener(S.port, 'toneone02', 0, 'nfm');
    ok(await until(() => lastTone(J.spec) === CTCSS, 3000), `a listener arriving is told at once (got ${JSON.stringify(tones(J.spec))})`);
    J.close();
    L.spec.txt.length = 0;
    L.spec.send({ type: 'tune', frequency: CENTRE - 50000, mode: 'nfm' });
    ok(await until(() => lastTone(L.spec) === DCS, 15000), `−50 kHz NFM: "${DCS}" (got ${JSON.stringify(tones(L.spec))})`);
    ok(tones(L.spec)[0] === '', 'the retune cleared the old tone first (\'\' before the new label)');
    ok(!tones(L.spec).includes(CTCSS), 'never the old repeater\'s CTCSS on the new one');
    L.spec.txt.length = 0;
    L.spec.send({ type: 'tune', frequency: CENTRE + 150000, mode: 'nfm' });
    await sleep(10000);
    ok(tones(L.spec).length >= 1 && tones(L.spec).every((t) => t === ''), `+150 kHz (voice, no tone): cleared and nothing for 10 s (got ${JSON.stringify(tones(L.spec))})`);
    L.spec.send({ type: 'tune', frequency: CENTRE + 25000, mode: 'nfm' });
    ok(await until(() => lastTone(L.spec) === CTCSS, 15000), 'back on +25 kHz: the CTCSS again');
    L.spec.txt.length = 0;
    L.spec.send({ type: 'tune', frequency: CENTRE + 25000, mode: 'am' });
    ok(await until(() => lastTone(L.spec) === '', 5000), `leaving NFM clears it (got ${JSON.stringify(tones(L.spec))})`);
    await sleep(5000);
    ok(tones(L.spec).every((t) => t === ''), 'and nothing while in AM');
    L.close();
  } finally { S.stop(); }
}

// ── 2. A range-locked radio: each listener its own ToneId ────────────────────────────────────────────────────────────
{
  console.log('── range-locked radio (per-listener channels) ──');
  const S = await server(['--freq', String(CENTRE), '--lock-freq', String(CENTRE), '--users', '4', '--mode', 'nfm', '--idle-grace', '0'], 'lock');
  try {
    const A = await listener(S.port, 'tonelock1', CENTRE + 25000, 'nfm');
    const B = await listener(S.port, 'tonelock2', CENTRE - 50000, 'nfm');
    const C = await listener(S.port, 'tonelock3', CENTRE + 150000, 'nfm');
    ok(await until(() => lastTone(A.spec) === CTCSS && lastTone(B.spec) === DCS, 15000),
       `A (+25 kHz) "${lastTone(A.spec)}", B (−50 kHz) "${lastTone(B.spec)}"`);
    await sleep(3000);
    ok(!tones(A.spec).includes(DCS) && !tones(B.spec).includes(CTCSS), 'neither ever hears the other\'s label');
    ok(tones(C.spec).every((t) => t === ''), `C (+150 kHz, no tone): nothing (got ${JSON.stringify(tones(C.spec))})`);
    A.spec.txt.length = 0;
    A.spec.send({ type: 'tune', frequency: CENTRE - 50000, mode: 'nfm' });
    ok(await until(() => lastTone(A.spec) === DCS, 15000), `A retuned to B's carrier: the DCS (got ${JSON.stringify(tones(A.spec))})`);
    for (const L of [A, B, C]) L.close();
  } finally { S.stop(); }
}

console.log(`\n${fail ? '✗' : '✓'} test-server-tone: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
