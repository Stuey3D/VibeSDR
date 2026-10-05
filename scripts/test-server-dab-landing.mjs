// test-server-dab-landing.mjs — a radio set to START IN DAB starts in DAB, end to end.
//
// ★★★ THE FAULT (Stuart, 2026-10-05): "No server seems to honour the start in DAB mode." The
//     owner's app carries the admin credential on EVERY socket once the password is saved, and the
//     landing skipped admins — so on every one of his servers the first arrival after a start was
//     exempt, the radio stayed on FM, and the once-per-start landing was SPENT on him. On a shared
//     dial that was final: the stranger after him was never landed either, until the next restart.
//     Reproduced here before the fix: admin first on a shared dial, ZERO `dab` reports to anybody.
//
// ★★ WHAT THIS PINS, on a REAL vibeserver (fake rtl_tcp, landingDabChannel = 7C in config.json):
//     1. shared dial, the OWNER (admin ticket) arrives first: the first `config` on his spectrum
//        socket is already centred on the block and `dab` reports 7C follow — the full state, with
//        no request from the client;
//     2. the client restoring its own tune/mode (what the app and web do on connect) does NOT knock
//        it out of DAB — the server refuses both and keeps reporting 7C, no `dab_off`;
//     3. a stranger joining that shared dial is in DAB from their first config too;
//     4. a single-listener radio, a stranger first: landed in DAB (the path that always worked);
//     5. ★ the exemption still stands AFTER the start: that stranger leaves DAB on purpose and goes,
//        and the owner arriving next is NOT moved (no `dab` report) — the fix is the radio's FIRST
//        state, not a licence to land owners;
//     6. ★ NEGATIVE, on its own fresh server: no DAB landing configured → no `dab` report at all, so
//        the positive cases above cannot pass by accident.
// ★ SILENT: audio frames are received and discarded, never played.
//
//   usage: VIBESERVER_BIN=… node scripts/test-server-dab-landing.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) {
  console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree');
  process.exit(3);
}
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const BLOCK_7C = 10, CENTRE_7C = 192352000, FM = 100000000;
const ADMIN_PASS = 'dab-landing-test';

/** A minimal WebSocket client, recording every text frame (binary frames are counted, not kept). */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], bin: 0, open: false, closed: false };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('close', () => { st.closed = true; });
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
      if (op === 0x2) st.bin++;
      if (op === 0x1) st.txt.push(p.toString());
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b]));   // zero mask: payload unchanged
  };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
const parsed = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => { try { return JSON.parse(t); } catch { return {}; } });
const firstConfig = (st) => parsed(st, 'config')[0] || {};
const dabs = (st) => parsed(st, 'dab');

/** An admin ticket, minted as the server mints one (vibe_admin_ticket.h, v2) — what the owner's app carries. */
function adminTicket() {
  const now = Math.floor(Date.now() / 1000), exp = now + 600;
  const mac = crypto.createHmac('sha256', ADMIN_PASS).update(`vsadmin2|${now}|${exp}`).digest('hex');
  return `2.${now}.${exp}.${mac}`;
}

async function server(radio, tag) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-dabland-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  const rtlPort = await freePort(), port = await freePort(), front = await freePort();
  // ★ The landing lives in the RADIO entry, where the setup page writes it (radioToJson). The front
  //   door gets its own free port so a VibeServer already running on this machine is never touched.
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, fullMode: true, sharing: 'public', port: front,
    radios: [{ serial: 'fake', driver: 'rtlsdr', enabled: true, configured: true, freq: FM, demodMode: 'wfm',
               landingFreq: FM, ...radio }],
  }));
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
  await sleep(300);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000',
                          '--idle-grace', '0', '--admin-pass', ADMIN_PASS], { env, stdio: ['ignore', log, log] });
  for (let i = 0; i < 100; i++) {            // wait for the port
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  await sleep(500);
  return { port, dir, stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}
/** Spectrum first, then audio — the web client's order. `admin` = the owner's app, credential on every socket. */
async function listener(port, sid, { admin = false } = {}) {
  const adm = admin ? `&vs_admin_ticket=${adminTicket()}` : '';
  const L = {};
  L.spec = ws(port, `/ws/user-spectrum?user_session_id=${sid}&mode=binary8&bins=256${adm}`);
  await sleep(300);
  L.aud = ws(port, `/ws?user_session_id=${sid}&frequency=${FM}&modulation=wfm${adm}`);
  await sleep(2500);
  L.close = () => { L.spec.close(); L.aud.close(); };
  return L;
}

// ── 1-3. A SHARED dial, the owner first ─────────────────────────────────────────────────────────
{
  console.log('── shared dial (10 listeners), DAB landing 7C, the OWNER arrives first ──');
  const S = await server({ users: 10, landingDabChannel: BLOCK_7C, landingDabSid: 0 }, 'shared');
  try {
    const A = await listener(S.port, 'aaaa1111', { admin: true });
    ok(firstConfig(A.spec).centerFreq === CENTRE_7C,
       `owner's FIRST config is already on block 7C (centerFreq ${firstConfig(A.spec).centerFreq})`);
    ok(dabs(A.spec).length > 0 && dabs(A.spec).every((d) => d.channel === '7C'),
       `owner receives dab reports for 7C with no request of his own (${dabs(A.spec).length})`);
    // ★ What a client restoring its remembered dial sends on connect. The server must refuse both.
    const before = dabs(A.spec).length;
    A.spec.send({ type: 'mode', mode: 'wfm' });
    A.spec.send({ type: 'tune', frequency: FM, mode: 'wfm' });
    await sleep(2000);
    ok(dabs(A.spec).length > before && !A.spec.txt.some((t) => t.includes('"type":"dab_off"')),
       `a client's own tune/mode does not knock the radio out of DAB (${dabs(A.spec).length - before} more reports, no dab_off)`);
    const B = await listener(S.port, 'bbbb2222');
    ok(firstConfig(B.spec).centerFreq === CENTRE_7C && dabs(B.spec).some((d) => d.channel === '7C'),
       `a stranger joining the shared dial is in DAB from the first config (${firstConfig(B.spec).centerFreq})`);
    A.close(); B.close();
  } finally { S.stop(); }
  await sleep(500);
}

// ── 4-5. A single-listener radio: a stranger lands; the owner after them is still exempt ─────────
{
  console.log('── single listener, DAB landing 7C ──');
  const S = await server({ users: 1, landingDabChannel: BLOCK_7C, landingDabSid: 0 }, 'single');
  try {
    const A = await listener(S.port, 'cccc3333');
    ok(firstConfig(A.spec).centerFreq === CENTRE_7C && dabs(A.spec).some((d) => d.channel === '7C'),
       'a stranger, first after the start, lands on 7C');
    A.spec.send({ type: 'dab', on: 0 });   // ★ leaving DAB ON PURPOSE clears the remembered block
    await sleep(1500);
    A.close();
    await sleep(2500);
    const C = await listener(S.port, 'dddd4444', { admin: true });
    ok(dabs(C.spec).length === 0 && firstConfig(C.spec).centerFreq !== CENTRE_7C,
       `the owner arriving AFTER the start is not moved — the exemption stands (centerFreq ${firstConfig(C.spec).centerFreq})`);
    C.close();
  } finally { S.stop(); }
  await sleep(500);
}

// ── 6. NEGATIVE, fresh server: no DAB landing → no DAB ──────────────────────────────────────────
{
  console.log('── shared dial, NO DAB landing (negative) ──');
  const S = await server({ users: 10 }, 'none');
  try {
    const A = await listener(S.port, 'eeee5555', { admin: true });
    ok(dabs(A.spec).length === 0 && firstConfig(A.spec).centerFreq !== CENTRE_7C,
       `no DAB landing configured → no dab report (centerFreq ${firstConfig(A.spec).centerFreq})`);
    A.close();
  } finally { S.stop(); }
}

console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
