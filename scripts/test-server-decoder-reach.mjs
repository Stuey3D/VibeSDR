// test-server-decoder-reach.mjs — V12 (2026-11-03) DECODER REACH and the REGION DEFAULTS through the REAL server
// (android/app/src/main/cpp/vibe_decoder_reach.h). A fake rtl_tcp, no radio, nothing to hear — the capability only:
//   REACH: on a locked range 90.8–93.2 MHz every fixed-frequency decoder is sent false WITH its reason (`<id>Why`), is
//          left out of `modes`, and an attach is refused with the same words ("unreachable").
//   REGION: SAME is off by default outside ITU Region 2 and DAB in it — a DEFAULT of the owner's switch, never a grey:
//          false with NO reason (the owner-blocked shape), in `blocked`, with `regionDefaults` for the Setup page; the
//          owner's "+same" switches it back on; no position = no region default.
// Silent: no audio socket is opened.
//   VIBESERVER_BIN=…/vibeserver node scripts/test-server-decoder-reach.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN'); process.exit(3); }
const RATE = 2400000;
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 200; i++) { if (await f()) return true; await sleep(200); } return !!(await f()); };

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], open: false };
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

async function server(tag, args, cfgExtra = {}, radioExtra = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-reach-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Reach test', adminPass: 'reach-test-admin', ...cfgExtra,
    radios: [{ serial: `${tag}-test`, label: 'Test radio', enabled: true, configured: true, users: 1, ...radioExtra }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), ...args], { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  await sleep(800);   // ★ the radio's rate known
  return { port, dir, status, log: () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } },
           stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}
/** hwinfo's `blocked` — what every client hides (the spectrum socket's first hwinfo). */
async function blockedOf(S, tag) {
  const sp = ws(S.port, `/ws/user-spectrum?user_session_id=${tag}&bins=256&proto=1`);
  let b = null;
  await until(() => { const t = sp.txt.find((x) => x.includes('"type":"hwinfo"')); if (t) b = JSON.parse(t).blocked; return !!t; }, 6000);
  sp.close();
  return b;
}
const FIXED = ['same', 'dsc', 'aprs', 'navtex', 'selcal', 'cw', 'psk', 'jtty'];
const rd = (st, id) => (st && Array.isArray(st.regionDefaults) ? st.regionDefaults.find((d) => d.id === id) : null);

// ── 1. Region 1 (a UK locator), a free radio, a fresh config ──
{
  console.log('── Region 1, a fresh config ──');
  const S = await server('r1', ['--freq', '96100000', '--mode', 'wfm', '--users', '1'], { locator: 'IO92nh' });
  try {
    const st = await S.status();
    ok(st && st.region === 1, `region ${st && st.region}`);
    ok(!!rd(st, 'same') && /North America/.test(rd(st, 'same').why) && !rd(st, 'dab'), `regionDefaults: SAME only (${JSON.stringify(st && st.regionDefaults)})`);
    ok(st && st.same === false && st.sameWhy === undefined, 'SAME off by its region default — the owner-blocked shape (false, no reason)');
    const bl = await blockedOf(S, 'r1b01');
    ok(Array.isArray(bl) && bl.includes('same'), `…and in hwinfo's "blocked" (${JSON.stringify(bl)})`);
    ok(st && Array.isArray(st.modes) && !st.modes.includes('same'), 'SAME not in the offer');
    for (const id of FIXED.filter((x) => x !== 'same')) ok(st && st[id] === true && st[id + 'Why'] === undefined, `${id}: live (an RTL reaches it)`);
    ok(/region default: same off/.test(S.log()), 'the log says the default');
  } finally { S.stop(); }
}
// ── 2. Region 1, the owner switched SAME ON ("+same") ──
{
  console.log('── Region 1, the owner switched SAME on ──');
  const S = await server('r1on', ['--freq', '96100000', '--mode', 'wfm', '--users', '1'], { locator: 'IO92nh' }, { blockedModes: '+same' });
  try {
    const st = await S.status();
    ok(st && st.same === true, `SAME live (${JSON.stringify(st && st.same)})`);
    const bl = await blockedOf(S, 'r1on01');
    ok(Array.isArray(bl) && !bl.includes('same') && !bl.some((b) => b[0] === '+'), `hwinfo's "blocked" holds neither same nor +same (${JSON.stringify(bl)})`);
    ok(st && st.modes.includes('same'), 'SAME in the offer');
  } finally { S.stop(); }
}
// ── 3. Region 2 (New York), a fresh config ──
{
  console.log('── Region 2, a fresh config ──');
  const S = await server('r2', ['--freq', '96100000', '--mode', 'wfm', '--users', '1'], { locator: 'FN31pr' });
  try {
    const st = await S.status();
    ok(st && st.region === 2, `region ${st && st.region}`);
    ok(!!rd(st, 'dab') && /Americas/.test(rd(st, 'dab').why) && !rd(st, 'same'), `regionDefaults: DAB only (${JSON.stringify(st && st.regionDefaults)})`);
    ok(st && st.same === true, 'SAME live in Region 2');
    const bl = await blockedOf(S, 'r2b01');
    ok(st && st.dab === false && Array.isArray(bl) && bl.includes('dab'), `DAB off by its region default (dab ${st && st.dab}, blocked ${JSON.stringify(bl)})`);
  } finally { S.stop(); }
}
// ── 4. No position given: no region default at all ──
{
  console.log('── no position ──');
  const S = await server('none', ['--freq', '96100000', '--mode', 'wfm', '--users', '1']);
  try {
    const st = await S.status();
    ok(st && st.region === undefined && st.regionDefaults === undefined, 'no region, no regionDefaults');
    ok(st && st.same === true, 'SAME live (nothing switched off on a guess)');
  } finally { S.stop(); }
}
// ── 5. A LOCKED RANGE 90.8–93.2 MHz (the owner said SAME on): every fixed-frequency decoder greyed with its reason ──
{
  console.log('── a locked range 90.8–93.2 MHz ──');
  const S = await server('lock', ['--freq', '92000000', '--lock-freq', '92000000', '--users', '4', '--mode', 'wfm', '--idle-grace', '0'],
                         { locator: 'IO92nh' }, { blockedModes: '+same' });
  try {
    const st = await S.status();
    for (const id of FIXED) ok(st && st[id] === false && typeof st[id + 'Why'] === 'string' && /^Outside this radio's locked range \(90\.8–93\.2 MHz\)/.test(st[id + 'Why']),
                               `${id}: false + "${st && st[id + 'Why']}"`);
    ok(st && st.sameWhy === 'Outside this radio\'s locked range (90.8–93.2 MHz) — SAME is at 162.400–162.550 MHz', 'SAME\'s reason, word for word');
    ok(st && Array.isArray(st.modes) && FIXED.every((id) => !st.modes.includes(id)), `none of them in the offer (${JSON.stringify(st && st.modes)})`);
    ok(st && st.modes.includes('rtty') && st.modes.includes('wfm'), 'RTTY and WFM still offered (not fixed-frequency)');
    // ★ an attach is refused with the same words
    const dx = ws(S.port, '/ws/dxcluster?user_session_id=reach01');
    await until(() => dx.open, 3000);
    dx.send({ type: 'audio_extension_attach', extension_name: 'same', params: {} });
    ok(await until(() => dx.txt.some((t) => t.includes('"decoder_refused"') && t.includes('"unreachable"') && t.includes('SAME is at')), 4000),
       `attach SAME refused: ${dx.txt.find((t) => t.includes('decoder_refused')) || '(nothing)'}`);
    dx.close();
  } finally { S.stop(); }
}

console.log(`\n${fail ? 'FAIL' : 'ok'}  ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
