// test-server-dvsig.mjs — V12 DV ID (DMR, D-STAR, YSF, NXDN, dPMR signalling — never the voice) through the REAL server
// (vibe_dvsig.h, docs/v12/DV-SIGNALLING.md): a fake rtl_tcp replaying test-dvsig's synthetic clip — our own transmitter for
// each system in turn (a DMR repeater downlink with a talker alias and GPS, YSF DN with GPS, D-STAR with a slow-data
// message, NXDN48, dPMR), 25 kHz above the clip's centre, 20 dB C/N — and the contract:
//   • vibeserver.json offers "dvsig" (an RTL reaches VHF / UHF);
//   • on an AMATEUR frequency (70 cm, 433.425 MHz) a listener in mode "dvsig" gets {"type":"dvsig"} reports with
//     "ids":true — the DMR source, talkgroup and alias, the YSF and D-STAR callsigns, the NXDN unit → group, the dPMR IDs;
//     dvsig_status answers at once; dvsig_pin pins one system (and AUTO again); NFM after leaving: no more reports;
//   • OFF the amateur bands (PMR446, 446.075 MHz) the report is the PUBLIC copy: "DMR heard", the colour code, the slot —
//     and no ID, callsign, alias or position in ANYTHING the socket was sent; with the owner's opt-in ("dvsig+ids") on a
//     shared dial the signed-in owner's socket gets the identities and a listener's never does;
//   • the owner switched it off ("dvsig"): not offered.
// Silent: audio frames are dropped unread. Every socket sends the client's {type:'ping'} keepalive.
//   VIBESERVER_BIN=…/vibeserver VIBE_DVSIG_CLIP=…/dvsig.cu8 node scripts/test-server-dvsig.mjs   (test-dvsig clip dvsig.cu8 288000 20 20)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_DVSIG_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_DVSIG_CLIP (test-dvsig clip …)');
  process.exit(3);
}
const RATE = 288000, PASS = 'dvsig-test-admin';
const HAM = 433425000, PMR = 446075000;   // ★ each the clip's station: 25 kHz above the replayed centre
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 200; i++) { if (await f()) return true; await sleep(200); } return !!(await f()); };

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { bin: [], txt: [], open: false, dropBin: false };
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
const reps = (st) => st.txt.filter((t) => t.includes('"type":"dvsig"')).map((t) => JSON.parse(t));
const everything = (st) => st.txt.join('\n') + '\n' + st.bin.map((b) => b.toString('latin1')).join('\n');
/** Every entry any report carried (by id, the newest copy). */
const entries = (st) => { const m = new Map(); for (const r of reps(st)) for (const e of r.log || []) m.set(e.id, e); return [...m.values()]; };

async function server(args, tag, centre, radioExtra = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-dvsig-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'DV ID test', locator: 'IO92nh', adminPass: PASS,
    radios: [{ serial: `dvsig-${tag}`, label: 'Test radio', enabled: true, configured: true, ...radioExtra }],
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
  return { port, dir, status, getJson, stop: async () => {
    srv.kill('SIGTERM'); rtl.kill('SIGTERM');
    if (process.env.VIBE_KEEP) { console.log('   kept', dir); return; }
    await sleep(500); try { fs.rmSync(dir, { recursive: true, force: true }); } catch {}
  } };
}
async function listener(port, sid, dial, mode = 'dvsig') {
  const L = {};
  L.spec = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`);
  await sleep(400);
  L.aud = ws(port, `/ws/audio?user_session_id=${sid}`); L.aud.dropBin = true;   // ★ silent: frames dropped unread
  await sleep(400);
  L.spec.send({ type: 'tune', frequency: dial, mode });
  L.ka = setInterval(() => { try { L.spec.send({ type: 'ping' }); L.aud.send({ type: 'ping' }); } catch {} }, 5000);
  await sleep(600);
  return L;
}
const close = (L) => { clearInterval(L.ka); for (const k of ['spec', 'aud']) L[k]?.close(); };
async function signIn(S, sock) {
  const nonce = (await S.getJson('/vibeserver/auth'))?.nonce || '';
  sock.send({ type: 'admin_unlock', nonce, token: crypto.createHmac('sha256', PASS).update(nonce).digest('hex') });
  return until(() => sock.txt.some((m) => m.includes('"type":"admin","ok":true')), 4000);
}
const IDS_RE = /2345678|G4ABC|ID51|GB7XX|"alias"|"src"|"from"|"gps"|1234567|7654321|Hello from VibeSDR/;

// ── 1. 70 cm: an amateur frequency, one pipeline ──
{
  console.log('── one-pipeline radio on 70 cm (433.425 MHz): an amateur frequency ──');
  const S = await server(['--freq', String(HAM), '--mode', 'nfm', '--users', '1'], 'ham', HAM - 25000);
  try {
    const st = await S.status();
    ok(st && Array.isArray(st.modes) && st.modes.includes('dvsig'), `vibeserver.json offers dvsig (${JSON.stringify(st && st.modes)})`);
    const L = await listener(S.port, 'dvsigham01', HAM);
    ok(await until(() => reps(L.spec).length > 0, 5000), `reports on the spectrum socket (${JSON.stringify(reps(L.spec).at(-1))?.slice(0, 200)})`);
    const r0 = reps(L.spec).at(-1);
    ok(r0 && r0.ids === true && r0.amateur === true && /70 cm/.test(r0.band || ''), `the identities copy, with the band (${r0 && r0.band})`);
    const has = (f) => entries(L.spec).find(f);
    ok(await until(() => has((e) => e.sys === 'DMR' && e.alias === 'G4ABC Stuart' && e.src === 2345678 && e.dst === 235 && e.grp && e.cc === 1), 40000),
       `DMR: 2345678 → TG 235, CC 1, alias "G4ABC Stuart" (${JSON.stringify(has((e) => e.sys === 'DMR'))})`);
    ok(await until(() => has((e) => e.sys === 'DMR' && e.gps && Math.abs(e.gps.lat - 52.6235) < 1e-3), 30000), '…and its embedded GPS');
    ok(await until(() => has((e) => e.sys === 'YSF' && e.from === 'G4ABC' && e.to === 'ALL'), 30000), `YSF: G4ABC → ALL (${JSON.stringify(has((e) => e.sys === 'YSF'))})`);
    ok(await until(() => has((e) => e.sys === 'D-STAR' && e.from === 'G4ABC/ID51' && e.text === 'Hello from VibeSDR'), 30000), `D-STAR: G4ABC/ID51 and the slow-data message (${JSON.stringify(has((e) => e.sys === 'D-STAR'))})`);
    ok(await until(() => has((e) => e.sys === 'NXDN' && e.src === 1234 && e.dst === 10), 30000), `NXDN48: unit 1234 → group 10 (${JSON.stringify(has((e) => e.sys === 'NXDN'))})`);
    ok(await until(() => has((e) => e.sys === 'dPMR' && e.src === 1234567 && e.dst === 7654321), 30000), `dPMR: 1234567 → 7654321 (${JSON.stringify(has((e) => e.sys === 'dPMR'))})`);
    { const n = L.spec.txt.length; L.spec.send({ type: 'dvsig_status' });
      ok(await until(() => L.spec.txt.slice(n).some((t) => t.includes('"type":"dvsig"')), 3000), 'dvsig_status: the report at once'); }
    { const n = L.spec.txt.length; L.spec.send({ type: 'dvsig_pin', sys: 'dmr' });
      ok(await until(() => L.spec.txt.slice(n).some((t) => t.includes('"pin":"dmr"')), 3000), 'dvsig_pin dmr: the report says DMR is pinned');
      L.spec.send({ type: 'dvsig_pin', sys: 'auto' });
      ok(await until(() => reps(L.spec).at(-1)?.pin === 'auto', 3000), '…and AUTO again'); }
    { L.spec.send({ type: 'tune', frequency: HAM + 100000, mode: 'nfm' });
      await sleep(1500);
      const n = reps(L.spec).length; await sleep(3000);
      ok(reps(L.spec).length === n, `NFM: no more DV ID reports (${reps(L.spec).length - n})`); }
    close(L);
  } finally { await S.stop(); }
}

// ── 2. PMR446: not amateur, range-locked (each listener their own receiver), no opt-in ──
{
  console.log('── range-locked radio on PMR446 (446.075 MHz): not an amateur frequency ──');
  const S = await server(['--freq', String(PMR - 25000), '--lock-freq', String(PMR - 25000), '--users', '4', '--mode', 'nfm', '--idle-grace', '0'], 'pmr', PMR - 25000);
  try {
    const L = await listener(S.port, 'dvsigpmr01', PMR);
    ok(await until(() => entries(L.spec).some((e) => e.sys === 'DMR' && e.cc === 1), 40000), `DMR heard, CC 1 (${JSON.stringify(entries(L.spec).find((e) => e.sys === 'DMR'))})`);
    ok(await until(() => entries(L.spec).some((e) => e.sys === 'dPMR'), 30000) && entries(L.spec).some((e) => e.sys === 'YSF'), '…YSF and dPMR heard too');
    const r = reps(L.spec).at(-1);
    ok(r && r.ids === false && r.amateur === false, 'the public copy ("ids": false, not amateur)');
    ok(!IDS_RE.test(everything(L.spec)), '★ no ID, callsign, alias, text or position in anything this socket was sent');
    close(L);
  } finally { await S.stop(); }
}

// ── 3. PMR446, a shared dial, the owner opted in ──
{
  console.log('── PMR446, shared dial, the owner opted in (dvsig+ids) ──');
  const S = await server(['--freq', String(PMR), '--mode', 'nfm', '--users', '4'], 'opt', PMR - 25000, { users: 4, blockedModes: 'dvsig+ids' });
  try {
    const O = await listener(S.port, 'dvsigown1', PMR);
    const G = await listener(S.port, 'dvsiggst1', PMR);
    ok(await signIn(S, O.spec), 'the owner signs in (on the spectrum socket)');
    O.spec.send({ type: 'dvsig_status' });
    ok(await until(() => entries(O.spec).some((e) => e.sys === 'DMR' && e.src === 2345678), 40000), `the owner: the DMR IDs (${JSON.stringify(entries(O.spec).find((e) => e.sys === 'DMR'))})`);
    ok(reps(O.spec).some((r) => r.ids === true && r.amateur === false), '…in an identities copy off the amateur bands');
    ok(await until(() => entries(G.spec).some((e) => e.sys === 'DMR'), 20000), 'the listener: DMR heard');
    ok(!IDS_RE.test(everything(G.spec)), '★ the listener was sent no ID at all');
    close(O); close(G);
  } finally { await S.stop(); }
}

// ── 4. the owner switched it off ──
{
  console.log('── the owner blocked it ──');
  const S = await server(['--freq', String(HAM), '--mode', 'nfm', '--users', '1'], 'off', HAM - 25000, { blockedModes: 'dvsig' });
  try {
    const st = await S.status();
    ok(st && Array.isArray(st.modes) && !st.modes.includes('dvsig'), `not offered (${JSON.stringify(st && st.modes)})`);
  } finally { await S.stop(); }
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
