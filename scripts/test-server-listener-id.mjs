// test-server-listener-id.mjs — THE APP SENDS THE WEB'S LISTENER ID (V12, 2026-10-27, branch v12-app-client-id).
//
// ★★★ STUART: "2 radios one server is allowed unless server owner blocks the same connection using both radios exactly
//     how the web client handles it now".
//
// ★★ WHAT THIS PINS, on two REAL radio processes of one machine (fake rtl_tcp each, started the way systemd starts
//    vibeserver@<serial>, sharing the runtime dir that holds the machine-wide occupant registry):
//     • the web's per-browser `bid` and the app's per-install `bid` (src/services/listenerIdCore.ts) get EXACTLY the same
//       answer for "this listener on a second radio", under each of the owner's settings (maxRadiosPerIp 1 / 2 / 0 —
//       the "Allow several connections" switch is 1 ↔ 0): refused with the same `elsewhere` naming the first radio
//       where the owner limits it, both radios where the owner allows it;
//     • two DIFFERENT installs behind ONE address are two listeners (cap 1: both get a radio) — the old app, sending
//       no id, was judged by the address and the second was refused, as an address-only client still is;
//     • a server that predates `bid` ignores an unknown query parameter — shown by the address-only rule applying
//       unchanged when the parameter is junk the server must discard (the server's own shape check).
// ★ Connections come from the machine's LAN address, so the loopback exemption (which only an ADDRESS key gets) is out.
// ★ SILENT: audio sockets are opened only so each listener is a real one; their frames are counted, never played.
//   usage: VIBESERVER_BIN=… node scripts/test-server-listener-id.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }
let lan = process.env.LAN_IP || '';
if (!lan) for (const list of Object.values(os.networkInterfaces())) for (const a of list || []) if (a.family === 'IPv4' && !a.internal && !lan) lan = a.address;
if (!lan) { console.log('   (not run — no LAN address; the address-only cases need one, loopback is exempt)'); process.exit(3); }

let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const children = new Set();
const killAll = () => { for (const c of children) { try { c.kill('SIGTERM'); } catch {} } children.clear(); };
process.on('exit', killAll);
const deadline = setTimeout(() => { console.log('   FAIL deadline — the run took over 4 minutes'); killAll(); process.exit(1); }, 4 * 60_000);
deadline.unref();

/** A minimal WebSocket client recording text frames. */
function ws(port, pathq, host) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, host);
  const st = { txt: [], bin: 0, open: false, closed: false };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: ${host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
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
    const h = Buffer.alloc(6); h[0] = 0x81; h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2);
    s.write(Buffer.concat([h, b]));
  };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
const waitFor = async (fn, ms) => { const t = Date.now(); while (Date.now() - t < ms) { if (fn()) return true; await sleep(100); } return fn(); };

// ★ The two clients' URLs, as each builds them: the web (main.ts) and the app (VibeServerWsClient + LocalAudioPlayer).
const WEB = {
  spec: (sid, bid) => `/ws/user-spectrum?user_session_id=${sid}&bid=${bid}&mode=binary8&bins=1024&proto=1&dab=2`,
  audio: (sid, bid) => `/ws/audio?user_session_id=${sid}&bid=${bid}&proto=1&codec=opus`,
};
const APP_Q = `&client=${encodeURIComponent('VibeSDR/12.0.0 (iOS)')}&proto=1&dab=2`;
const APP = {
  spec: (sid, bid) => `/ws/user-spectrum?user_session_id=${sid}&mode=binary8&bins=1024${APP_Q}${bid ? `&bid=${bid}` : ''}`,
  audio: (sid, bid) => `/ws/audio?user_session_id=${sid}${bid ? `&bid=${bid}` : ''}&codec=opus`,
};

/** One listener on one radio: spectrum + audio. Resolves to 'in' (hwinfo/config arrived), or the refusal's radio name. */
async function listen(kind, port, sid, bid) {
  const spec = ws(port, kind.spec(sid, bid), lan);
  const verdict = await waitFor(() => spec.txt.some((t) => /"type":"(elsewhere|config|hwinfo)"/.test(t)), 6000);
  const el = spec.txt.find((t) => t.includes('"type":"elsewhere"'));
  const audio = el ? null : ws(port, kind.audio(sid, bid), lan);
  if (audio) await waitFor(() => audio.bin > 0 || audio.closed, 4000);
  return { spec, audio, result: !verdict ? 'nothing' : el ? `elsewhere:${JSON.parse(el).radio}` : 'in',
           audioOk: !!audio && audio.bin > 0 && !audio.closed };
}
async function leave(...ls) {
  for (const l of ls) { try { l.spec.send({ type: 'leave' }); } catch {} }
  await sleep(150);
  for (const l of ls) { l.spec.close(); l.audio?.close(); }
}

async function machine(tag, cap) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-lid-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  const cfg = { configured: true, name: 'Listener id test', adminPass: 'lid-test-admin',
    radios: [{ serial: 'lid-a', driver: 'rtlsdr', label: 'Radio A', enabled: true, configured: true, users: 4, rate: 2048000 },
             { serial: 'lid-b', driver: 'rtlsdr', label: 'Radio B', enabled: true, configured: true, users: 4, rate: 2048000 }] };
  if (cap !== null) cfg.maxRadiosPerIp = cap;   // ★ null = the owner never touched it: the default
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify(cfg));
  const env = { ...process.env, VIBESERVER_NO_RADIO_SPAWN: '1', VIBESERVER_CONFIG: path.join(dir, 'config.json'),
                VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const ports = {};
  const logs = [];
  for (const serial of ['lid-a', 'lid-b']) {
    const rtlPort = await freePort(), port = await freePort();
    const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
    children.add(rtl);
    await sleep(300);
    const lf = path.join(dir, `${serial}.log`); logs.push(lf);
    const srv = spawn('nice', ['-n', '19', BIN, '--radio-serial', serial, '--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port),
                               '--freq', '7074000', '--mode', 'usb'], { env, stdio: ['ignore', fs.openSync(lf, 'w'), fs.openSync(lf, 'a')] });
    children.add(srv);
    ports[serial] = port;
  }
  for (const p of Object.values(ports))
    for (let i = 0; i < 150; i++) {
      if (await new Promise((r) => { const c = net.connect(p, lan); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
      await sleep(100);
    }
  const occ = path.join(dir, 'run', 'occupants');
  return { A: ports['lid-a'], B: ports['lid-b'], log: () => logs.map((l) => fs.readFileSync(l, 'utf8')).join('\n'),
           // ★ Between cases: both radios' claims released (a leave clears the file; a stale one ages out at 15 s).
           settle: () => waitFor(() => !fs.existsSync(occ) || fs.readdirSync(occ).length === 0, 17_000),
           stop: () => killAll() };
}

const id = () => crypto.randomUUID();
/** One listener (one browser / one install) on radio A, then on radio B. */
async function bothRadios(M, kind, bid, bid2 = bid) {
  const a = await listen(kind, M.A, id(), bid);
  await sleep(1500);   // ★ the claim is written on admission and refreshed every second
  const b = await listen(kind, M.B, id(), bid2);
  await leave(a, b);
  await M.settle();
  return [a, b];
}

try {
  for (const [tag, cap] of [['default', null], ['allow2', 2], ['nolimit', 0], ['one', 1]]) {
    const capName = cap === null ? 'the default (owner never set it)' : cap === 0 ? '0 = no limit ("Allow several connections" ON)'
                  : cap === 1 ? '1 ("Allow several connections" OFF)' : String(cap);
    console.log(`\nmaxRadiosPerIp ${capName}`);
    const M = await machine(tag, cap);
    try {
      const [wa, wb] = await bothRadios(M, WEB, id());
      const [aa, ab] = await bothRadios(M, APP, id());
      ok(wa.result === 'in' && wa.audioOk && aa.result === 'in' && aa.audioOk, 'web and app each get radio A (spectrum + audio)');
      ok(wb.result === ab.result, `★★★ the app's answer on radio B is the web's: web "${wb.result}", app "${ab.result}"`);
      const blocked = cap === null || cap === 1;
      if (blocked) ok(ab.result === 'elsewhere:Radio A', '...refused, naming the radio already held — the owner\'s block applies to the app');
      else ok(ab.result === 'in' && ab.audioOk, '...both radios, spectrum + audio — the owner allows it');

      // ★ Two installs behind ONE address are two listeners — the app with its id, as two browsers are.
      const [h1, h2] = await bothRadios(M, APP, id(), id());
      ok(h1.result === 'in' && h2.result === 'in', `two app installs on one address: two listeners, a radio each ("${h1.result}", "${h2.result}")`);
      // ★ An address-only client (the app before this change) — and, being the same request, how an OLD SERVER sees the
      //   new app once it drops the parameter it does not know.
      const [o1, o2] = await bothRadios(M, APP, '');
      if (blocked) ok(o1.result === 'in' && o2.result === 'elsewhere:Radio A', `no id: judged by the address — the second refused ("${o2.result}"), the old app's fault`);
      else ok(o1.result === 'in' && o2.result === 'in', `no id: judged by the address — allowed here too ("${o2.result}")`);
      // ★ A `bid` the server's shape check discards (what a server that never reads `bid` does with any) — no harm:
      //   the address rule, exactly as with none.
      const [j1, j2] = await bothRadios(M, APP, 'bad!id', 'bad!id');
      ok(j1.result === 'in' && j2.result === o2.result, `an id the server ignores changes nothing ("${j2.result}")`);
    } finally { M.stop(); await sleep(400); }
  }
} catch (e) { fail++; console.log(`   FAIL ${e?.stack || e}`); }
console.log(`\nlistener id (server): ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
