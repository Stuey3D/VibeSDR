// test-server-tones.mjs — V12 DTMF + 5-tone selective calling through the REAL server (vibe_tones_run.h, docs/v12/TONES.md):
// a fake rtl_tcp replaying test-tones' synthetic NFM clip — our own transmitter, one channel sending, on a ~6.5 s loop, DTMF
// "*73#1234", a ZVEI-1 call "12345" and a CCIR call "40719" — and the PRIVACY CONTRACT:
//   • vibeserver.json "tones": true and modes lists it; the owner's block makes it false;
//   • ON 2 m (145.5875 MHz, amateur in every region): the DTMF digits and both 5-tone calls' digits, named; {"type":"tones"}
//     says amateur, "2 m amateur"; the pins ("standard":"ccir" — only CCIR; "dtmf":false — no DTMF);
//   • ON PMR446 (446.00625 MHz, not amateur): "DTMF heard" and "ZVEI-1 5-tone heard" / "CCIR 5-tone heard" — and the digits
//     NOWHERE in anything any socket was sent, opt-in or not;
//   • the owner's opt-in ("tones+digits") on a SHARED dial: the signed-in owner's socket gets the 5-tone numbers, a listener's
//     mirror of the same decoder never does, and DTMF digits reach nobody;
//   • a range-locked radio (per-listener channels): the amateur lines.
// Silent: no audio is played (the audio socket's frames are dropped unread). Every socket sends the client's ping.
//   VIBESERVER_BIN=…/vibeserver VIBE_TONES_CLIP=…/clip.cu8 node scripts/test-server-tones.mjs   (test-tones clip clip.cu8 288000 30)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_TONES_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_TONES_CLIP (make one: test-tones clip clip.cu8 288000 30)');
  process.exit(3);
}
const RATE = 288000, PASS = 'tones-test-admin';
const HAM = 145587500, PMR = 446006250;   // ★ the clip's channel is 25 kHz above its centre
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 200; i++) { if (await f()) return true; await sleep(200); } return !!(await f()); };

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
  // ★ THE REAL CLIENT'S KEEPALIVE: the server releases a proto-1 session silent for 30 s
  const keep = setInterval(() => { if (st.open) st.send({ type: 'ping' }); }, 8000);
  s.on('close', () => clearInterval(keep));
  st.close = () => { clearInterval(keep); try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
const jsons = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => { try { return JSON.parse(t); } catch { return null; } }).filter(Boolean);
const lastOf = (st, type) => jsons(st, type).at(-1) || null;
const text01 = (st) => st.bin.filter((b) => b[0] === 0x01 && b.length >= 13).map((b) => b.subarray(13, 13 + b.readUInt32BE(9)).toString()).join('');
/** Everything a socket was sent, as one string (text frames and binary frames as latin1) — for "never sent" checks. */
const everything = (st) => st.txt.join('\n') + '\n' + st.bin.map((b) => b.toString('latin1')).join('\n');

async function server(args, tag, centre, radioExtra = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-tones-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Tones test', locator: 'IO92nh', adminPass: PASS,
    radios: [{ serial: `tones-${tag}`, label: 'Test radio', enabled: true, configured: true, ...radioExtra }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', CLIP, '--hf-centre', String(centre)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--admin-pass', PASS, ...args],
                    { env, stdio: ['ignore', log, log] });
  const getJson = async (p) => { try { const r = await fetch(`http://127.0.0.1:${port}${p}`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  const status = () => getJson('/vibeserver.json');
  await until(async () => !!(await status()), 15000);
  return { port, dir, status, getJson, stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}
async function listener(port, sid, dial) {
  const L = {};
  L.spec = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256&proto=1`);
  await sleep(400);
  L.aud = ws(port, `/ws/audio?user_session_id=${sid}`); L.aud.dropBin = true;   // ★ silent: frames dropped unread
  L.dx = ws(port, `/ws/dxcluster?user_session_id=${sid}`);
  await sleep(400);
  L.spec.send({ type: 'tune', frequency: dial, mode: 'nfm' });
  await sleep(600);
  return L;
}
async function signIn(S, sock) {
  const nonce = (await S.getJson('/vibeserver/auth'))?.nonce || '';
  sock.send({ type: 'admin_unlock', nonce, token: crypto.createHmac('sha256', PASS).update(nonce).digest('hex') });
  return until(() => sock.txt.some((m) => m.includes('"type":"admin","ok":true')), 4000);
}
const close = (L) => { for (const k of ['spec', 'aud', 'dx']) L[k]?.close(); };

// ── 1. 2 m: an amateur frequency ──
{
  console.log('── one-pipeline radio on 2 m (145.5875 MHz NFM): an amateur frequency ──');
  const S = await server(['--freq', String(HAM), '--mode', 'nfm', '--users', '1'], 'ham', HAM - 25000);
  try {
    const st = await S.status();
    ok(st && st.tones === true, `vibeserver.json: tones ${JSON.stringify(st && st.tones)}`);
    ok(st && Array.isArray(st.modes) && st.modes.includes('tones'), 'modes lists tones');
    const L = await listener(S.port, 'tonesham1', HAM);
    L.dx.send({ type: 'audio_extension_attach', extension_name: 'tones' });
    const all = () => /DTMF  \*73#1234/.test(text01(L.dx)) && /ZVEI-1 5-tone  12345/.test(text01(L.dx)) && /CCIR 5-tone  40719/.test(text01(L.dx));
    ok(await until(all, 30000), 'the DTMF string and both 5-tone calls, digits shown (amateur)');
    const t = text01(L.dx);
    ok(/2 m amateur/.test(t), `…each line names the band: ${JSON.stringify(t.split('\n').find((l) => /DTMF/.test(l)))}`);
    const lines = t.split('\n').filter(Boolean);
    // ★ the decoder may start in the middle of the DTMF string: its first line can be the END of what was sent
    const sentOk = (l) => { const m = /DTMF  (\S+)/.exec(l); return m ? '*73#1234'.endsWith(m[1]) : /ZVEI-1 5-tone  12345|CCIR 5-tone  40719/.test(l); };
    ok(lines.every(sentOk), `nothing that was not sent (${lines.map((l) => l.replace(/^\S+\s+/, '').split('  ·')[0]).join(' | ')})`);
    const j = lastOf(L.dx, 'tones');
    ok(j && j.data.amateur === true && j.data.band === '2 m amateur' && j.data.pin === 'auto', `{"type":"tones"}: ${JSON.stringify(j && j.data)}`);
    const states = L.dx.bin.filter((b) => b[0] === 0x03).map((b) => b[1]);
    ok(states.includes(1) && states.includes(0), `states 1 and 0 sent (${[...new Set(states)].join(',')})`);
    // ★ the pins
    L.dx.bin.length = 0; L.dx.txt.length = 0;
    L.dx.send({ type: 'audio_extension_attach', extension_name: 'tones', standard: 'ccir', dtmf: false });
    ok(await until(() => /CCIR 5-tone  40719/.test(text01(L.dx)), 20000), 'pinned to CCIR, DTMF off: the CCIR call');
    await sleep(7000);
    ok(!/ZVEI-1/.test(text01(L.dx)) && !/DTMF/.test(text01(L.dx)), '…and neither the ZVEI-1 call nor the DTMF');
    ok(lastOf(L.dx, 'tones')?.data.pin === 'ccir' && lastOf(L.dx, 'tones')?.data.dtmf === false, 'the status says so');
    L.dx.send({ type: 'audio_extension_detach' });
    await sleep(1500);
    const n = L.dx.bin.filter((b) => b[0] === 0x01).length;
    await sleep(8000);
    ok(L.dx.bin.filter((b) => b[0] === 0x01).length === n, 'detached: the lines stop');
    close(L);
  } finally { S.stop(); }
}

// ── 2. PMR446: not amateur — format only, whatever happens ──
{
  console.log('── one-pipeline radio on PMR446 (446.00625 MHz NFM): not amateur ──');
  const S = await server(['--freq', String(PMR), '--mode', 'nfm', '--users', '1'], 'pmr', PMR - 25000);
  try {
    const L = await listener(S.port, 'tonespmr1', PMR);
    L.dx.send({ type: 'audio_extension_attach', extension_name: 'tones' });
    const all = () => /DTMF heard/.test(text01(L.dx)) && /ZVEI-1 5-tone heard/.test(text01(L.dx)) && /CCIR 5-tone heard/.test(text01(L.dx));
    ok(await until(all, 30000), '"DTMF heard", "ZVEI-1 5-tone heard", "CCIR 5-tone heard"');
    await sleep(7000);
    const e = everything(L.dx);
    ok(!/\*73#1234|73#12|12345|40719/.test(e), '★ the digits are in NOTHING this socket was sent');
    const j = lastOf(L.dx, 'tones');
    ok(j && j.data.amateur === false && j.data.band === '', `{"type":"tones"}: not amateur (${JSON.stringify(j && j.data)})`);
    close(L);
  } finally { S.stop(); }
}

// ── 3. PMR446, the owner's opt-in, a shared dial: the owner sees 5-tone numbers, a listener never ──
{
  console.log('── PMR446, shared dial, the owner opted in (tones+digits) ──');
  const S = await server(['--freq', String(PMR), '--mode', 'nfm', '--users', '4'], 'opt', PMR - 25000, { users: 4, blockedModes: 'tones+digits' });
  try {
    const O = await listener(S.port, 'tonesown1', PMR);
    const G = await listener(S.port, 'tonesgst1', PMR);
    ok(await signIn(S, O.spec), 'the owner signs in (on the spectrum socket)');
    O.dx.send({ type: 'audio_extension_attach', extension_name: 'tones' });
    await sleep(500);
    G.dx.send({ type: 'audio_extension_attach', extension_name: 'tones' });   // ★ a mirror of the same decoder on a shared dial
    ok(await until(() => /ZVEI-1 5-tone  12345.*to you only/.test(text01(O.dx)) && /CCIR 5-tone  40719/.test(text01(O.dx)), 30000),
       'the owner: "ZVEI-1 5-tone  12345 … shown to you only" and the CCIR call');
    ok(await until(() => /ZVEI-1 5-tone heard/.test(text01(G.dx)), 15000), 'the listener: "ZVEI-1 5-tone heard"');
    await sleep(7000);
    ok(!/12345|40719|\*73#1234/.test(everything(G.dx)), '★ the listener was sent no number at all');
    ok(/DTMF heard/.test(text01(O.dx)) && !/\*73#1234/.test(everything(O.dx)), '★ DTMF digits reach not even the owner off the amateur bands');
    close(O); close(G);
  } finally { S.stop(); }
}

// ── 4. the same opt-in, nobody signed in ──
{
  console.log('── PMR446, opted in, nobody signed in ──');
  const S = await server(['--freq', String(PMR), '--mode', 'nfm', '--users', '1'], 'opt0', PMR - 25000, { blockedModes: 'tones+digits' });
  try {
    const L = await listener(S.port, 'tonesnob1', PMR);
    L.dx.send({ type: 'audio_extension_attach', extension_name: 'tones' });
    ok(await until(() => /ZVEI-1 5-tone heard/.test(text01(L.dx)), 30000), '"ZVEI-1 5-tone heard"');
    await sleep(7000);
    ok(!/12345|40719/.test(everything(L.dx)), '★ no number without the owner signed in');
    close(L);
  } finally { S.stop(); }
}

// ── 5. range-locked radio ──
{
  console.log('── range-locked radio (per-listener channels) on 2 m ──');
  const C = HAM - 25000;
  const S = await server(['--freq', String(C), '--lock-freq', String(C), '--users', '4', '--mode', 'nfm', '--idle-grace', '0'], 'lock', C);
  try {
    const L = await listener(S.port, 'toneslck1', HAM);
    L.dx.send({ type: 'audio_extension_attach', extension_name: 'tones' });
    ok(await until(() => /DTMF  \*73#1234/.test(text01(L.dx)) && /ZVEI-1 5-tone  12345/.test(text01(L.dx)), 30000), 'per-listener: the DTMF string and the ZVEI-1 call');
    close(L);
  } finally { S.stop(); }
}

// ── 6. the owner's switch ──
{
  console.log('── the owner blocked it ──');
  const S = await server(['--freq', String(HAM), '--mode', 'nfm', '--users', '1'], 'off', HAM - 25000, { blockedModes: 'tones' });
  try {
    const st = await S.status();
    ok(st && st.tones === false, `vibeserver.json: tones ${JSON.stringify(st && st.tones)}`);
    const L = await listener(S.port, 'tonesoff1', HAM);
    L.dx.send({ type: 'audio_extension_attach', extension_name: 'tones' });
    await sleep(9000);
    ok(!/DTMF|5-tone/.test(text01(L.dx)), 'an attach is refused: nothing decoded');
    close(L);
  } finally { S.stop(); }
}

console.log(`\n${fail ? '✗' : '✓'} test-server-tones: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
