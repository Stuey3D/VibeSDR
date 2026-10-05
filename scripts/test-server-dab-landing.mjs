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
//     7. ★★★ THE APP'S OWN ORDER (2026-10-05, RC14 on the Pi 2: "first thing I asked for has failed").
//        The app opens AUDIO first — /ws/audio?user_session_id=…&codec=opus, NO frequency — and its
//        spectrum socket a second later; then it restores its remembered tune on both sockets. Its own
//        audio socket made the room look occupied, `firstOfSession` was false, and the landing block
//        decided nothing in silence. Pinned on a shared dial (owner) and a single radio (stranger),
//        with the capture idle-parked before anyone arrives, as on the Pi;
//     8. ★ the same through a FRONT DOOR handing the sockets to a separate radio process (/r/<serial>),
//        the Pi's multi-radio shape.
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

async function server(radio, tag, { frontDoor = false } = {}) {
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
  // ★ As a radio PROCESS behind a front door, it names its serial — which is what gives it a hand-off
  //   socket (<runtime>/fake.sock) and the /r/fake prefix — exactly as the door's own children are run.
  const srv = spawn(BIN, [...(frontDoor ? ['--radio-serial', 'fake'] : []),
                          '--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000',
                          '--idle-grace', '0', '--admin-pass', ADMIN_PASS], { env, stdio: ['ignore', log, log] });
  const waitPort = async (p) => {
    for (let i = 0; i < 100; i++) {
      const up = await new Promise((r) => { const c = net.connect(p, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
      if (up) return true;
      await sleep(100);
    }
    return false;
  };
  await waitPort(port);
  await sleep(500);
  let door = null;
  if (frontDoor) {
    // ★★ THE FRONT DOOR: no --tcp and no radio named, in full mode = a process that owns no radio and
    //    hands each connection to the radio's process over the hand-off socket. It would also fork a
    //    child for `fake` (no service manager here); that child finds our radio process holding the
    //    radio's lock and leaves it to us — the one-process-per-radio rule doing its job.
    const dlog = fs.openSync(path.join(dir, 'door.log'), 'w');
    door = spawn(BIN, ['--admin-pass', ADMIN_PASS], { env, stdio: ['ignore', dlog, dlog] });
    await waitPort(front);
    await sleep(800);
  }
  return { port: frontDoor ? front : port, prefix: frontDoor ? '/r/fake' : '', dir,
           log: () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } },
           stop: () => { srv.kill('SIGTERM'); door?.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
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

/** ★★★ THE APP'S ORDER against a VibeServer: AUDIO first (LocalAudioPlayer → /ws/audio, the session id and
 *  codec only — "THE URL NEVER STATES A FREQUENCY"), the spectrum socket about a second later
 *  (VibeServerWsClient), and then the remembered tune going out on both sockets — the native audio
 *  pump and the spectrum client (the Pi 2's "tune -> 96100.000 kHz asked by session" right after
 *  "spectrum WS connected"). That restore is not a user action and must not move a radio in DAB. */
async function appListener(S, sid, { admin = false } = {}) {
  const adm = admin ? `&vs_admin_ticket=${adminTicket()}` : '';
  const L = {};
  L.aud = ws(S.port, `${S.prefix}/ws/audio?user_session_id=${sid}&codec=opus${adm}`);
  await sleep(1000);
  L.spec = ws(S.port, `${S.prefix}/ws/user-spectrum?user_session_id=${sid}&mode=binary8&bins=256${adm}`);
  await sleep(600);
  const restore = { type: 'tune', frequency: FM, mode: 'wfm' };
  L.spec.send(restore); L.aud.send(restore);
  await sleep(2500);
  L.close = () => { L.spec.close(); L.aud.close(); };
  return L;
}
const inDab = (L) => dabs(L.spec).some((d) => d.channel === '7C') && !L.spec.txt.some((t) => t.includes('"type":"dab_off"'));
const lastConfig = (st) => { const c = parsed(st, 'config'); return c[c.length - 1] || {}; };

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

// ── 7. THE APP'S ORDER — audio first, then spectrum, then the restore — on a parked capture ─────
for (const [label, radio, admin] of [
  ['shared dial (5 listeners), the OWNER', { users: 5 }, true],
  ['single listener, a stranger', { users: 1 }, false],
]) {
  console.log(`── app order (audio first), ${label}, DAB landing 7C, capture parked ──`);
  const S = await server({ ...radio, landingDabChannel: BLOCK_7C, landingDabSid: 0 }, 'app');
  try {
    // ★ Park the capture first, as the Pi's was (22:40:45, two minutes before the listener), so the
    //   arrival also resumes a paused dongle — the Pi's exact path. A boot-time park waits on a USB
    //   radio's watchdog tick that an rtl_tcp source does not run, so an audio socket with NO session
    //   comes and goes instead: its close arms the park (--idle-grace 0 = at once), and an audio
    //   socket never spends the once-per-start landing (only a spectrum socket decides it).
    const probe = ws(S.port, `${S.prefix}/ws/audio?codec=opus`);
    await sleep(800); probe.close();
    for (let i = 0; i < 150 && !/dongle capture paused/.test(S.log()); i++) await sleep(100);
    ok(/dongle capture paused/.test(S.log()), 'the capture was idle-parked before anyone arrived');
    const A = await appListener(S, 'ffff6666', { admin });
    ok(inDab(A) && lastConfig(A.spec).centerFreq === CENTRE_7C,
       `${label} arriving audio-first lands on 7C, and the restore tune does not undo it `
       + `(${dabs(A.spec).length} dab reports, centerFreq ${lastConfig(A.spec).centerFreq})`);
    ok(/\[DAB\] new session — landing on DAB block 7C/.test(S.log()), 'the server log says it landed');
    A.close();
  } finally { S.stop(); }
  await sleep(500);
}

// ── 8. THROUGH A FRONT DOOR, to a separate radio process (the Pi's multi-radio shape) ────────────
{
  console.log('── front door → radio process, shared dial (5), app order, the OWNER, DAB landing 7C ──');
  const S = await server({ users: 5, landingDabChannel: BLOCK_7C, landingDabSid: 0 }, 'door', { frontDoor: true });
  try {
    await sleep(2000);
    const A = await appListener(S, 'abab7777', { admin: true });
    ok(A.spec.open && A.aud.open, 'both sockets were handed through the front door');
    ok(inDab(A) && lastConfig(A.spec).centerFreq === CENTRE_7C,
       `the owner, audio-first through the door, lands on 7C (${dabs(A.spec).length} dab reports, `
       + `centerFreq ${lastConfig(A.spec).centerFreq})`);
    A.close();
  } finally { S.stop(); }
}

console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
