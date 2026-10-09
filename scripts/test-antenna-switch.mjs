// ★★★ THE EXTERNAL ANTENNA SWITCH, END TO END (2026-10-09) — the real vibeserver against a fake MQTT broker that plays a
// Tasmota relay board (with Interlock). Stuart's design: docs/v12/ANTENNA-SWITCH-ROTATOR.md §3a.
//   1. hwinfo lists the owner's antenna NAMES, and the antenna the switch READS BACK as connected;
//   2. {type:"antenna"} switches it — break before make on the broker — and every listener hears the new state;
//   3. tuning into a band with a preset switches it (antennaMap with names);
//   4. the owner's lock refuses a listener;
//   5. the LAN search is admin-only, and finds the broker and the board.
// ★ SILENT: no audio. Loopback only. usage: VIBESERVER_BIN=… node scripts/test-antenna-switch.mjs
import crypto from 'node:crypto';
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
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN)'); process.exit(3); }

// ── a fake MQTT broker that is a Tasmota board ──
const relay = [false, false];
const fromClient = [];
let brokerSock = null;
const len = (n) => { const o = []; do { let b = n % 128; n = Math.floor(n / 128); if (n) b |= 0x80; o.push(b); } while (n); return Buffer.from(o); };
const str = (s) => { const b = Buffer.from(s); return Buffer.concat([Buffer.from([b.length >> 8, b.length & 255]), b]); };
const pkt = (h, body) => Buffer.concat([Buffer.from([h]), len(body.length), body]);
const pub = (t, p, retain = false) => pkt(0x30 | (retain ? 1 : 0), Buffer.concat([str(t), Buffer.from(p)]));
const state = (i) => brokerSock?.write(pub(`stat/antsw/POWER${i + 1}`, relay[i] ? 'ON' : 'OFF'));
const broker = net.createServer((s) => {
  brokerSock = s;
  let buf = Buffer.alloc(0);
  s.on('error', () => {});
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    for (;;) {
      if (buf.length < 2) return;
      let n = 0, m = 1, i = 1;
      for (;;) { if (i >= buf.length) return; n += (buf[i] & 127) * m; m *= 128; if (!(buf[i++] & 128)) break; }
      if (buf.length < i + n) return;
      const type = buf[0] >> 4, body = buf.subarray(i, i + n); buf = buf.subarray(i + n);
      if (type === 1) s.write(Buffer.from([0x20, 2, 0, 0]));
      else if (type === 8) {
        const tl = body.readUInt16BE(2), f = body.subarray(4, 4 + tl).toString();
        s.write(pkt(0x90, Buffer.from([body[0], body[1], 0])));
        if (f.startsWith('tasmota/discovery/'))
          s.write(pub('tasmota/discovery/A0B1C2D3E4F5/config', JSON.stringify({ dn: 'Generic Antenna Switch Example', t: 'antsw',
            tp: ['cmnd', 'stat', 'tele'], fn: ['Sky Loop', 'VHF Vertical'], rl: [1, 1, 0, 0] }), true));
      } else if (type === 3) {
        const tl = body.readUInt16BE(0), topic = body.subarray(2, 2 + tl).toString(), payload = body.subarray(2 + tl).toString();
        fromClient.push({ topic, payload });
        const mm = topic.match(/^cmnd\/antsw\/POWER(\d)$/);
        if (mm) {
          const k = Number(mm[1]) - 1;
          if (payload === '') state(k);                                  // a query: answer with the state
          else { const on = payload === 'ON'; if (on) relay.forEach((r, j) => { if (j !== k && r) { relay[j] = false; state(j); } }); relay[k] = on; state(k); }
        }
      } else if (type === 12) s.write(Buffer.from([0xd0, 0]));
    }
  });
});
const brokerPort = await freePort();
await new Promise((r) => broker.listen(brokerPort, '127.0.0.1', r));
relay[0] = true;   // the switch starts on Sky Loop

// ── the server ──
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [] };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let l = buf[1] & 0x7f, off = 2;
      if (l === 126) { if (buf.length < 4) break; l = buf.readUInt16BE(2); off = 4; }
      else if (l === 127) { if (buf.length < 10) break; l = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + l) break;
      const p = buf.subarray(off, off + l); buf = buf.subarray(off + l);
      if (op === 0x9) { const q = Buffer.alloc(6 + p.length); q[0] = 0x8a; q[1] = 0x80 | p.length; q.writeUInt32BE(0, 2); p.copy(q, 6); s.write(q); continue; }
      if (op === 0x1) st.txt.push(p.toString());
    }
  });
  st.send = (o) => { const b = Buffer.from(JSON.stringify(o)); const h = Buffer.alloc(b.length < 126 ? 6 : 8); h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b])); };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  st.radio = () => { const h = st.txt.filter((x) => x.includes('"type":"hwinfo"')).map((x) => JSON.parse(x)); return h.length ? h[h.length - 1].radio : null; };
  st.notices = () => st.txt.filter((x) => x.includes('"type":"notice"')).map((x) => JSON.parse(x).why);
  return st;
}
const until = async (f, ms = 5000) => { for (let i = 0; i < ms / 50; i++) { if (f()) return true; await sleep(50); } return !!f(); };

const run = async (locked) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-antsw-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Antenna Switch Test', locator: 'IO92nh', adminPass: 'antsw-admin-pass',
    radios: [{ serial: 'antsw-test', label: 'Test radio', enabled: true, configured: true, users: 3,
      antennaMap: '0-30MHz Sky Loop, 30MHz+ VHF Vertical', antennaPortLocked: locked,
      antennaSwitch: JSON.stringify({ enabled: true, host: '127.0.0.1', port: brokerPort, antennas: [
        { name: 'Sky Loop', cmd: 'cmnd/antsw/POWER1', state: 'stat/antsw/POWER1' },
        { name: 'VHF Vertical', cmd: 'cmnd/antsw/POWER2', state: 'stat/antsw/POWER2' }] }) }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
  await sleep(300);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', '3'],
                    { env, stdio: ['ignore', log, log] });
  for (let i = 0; i < 100; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  return { dir, port, stop: () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} } };
};

let s1 = null, s2 = null;
try {
  console.log('── the switch, unlocked ──');
  s1 = await run(false);
  const a = ws(s1.port, '/ws/user-spectrum?user_session_id=aaaaaaaa&bins=256');
  const b = ws(s1.port, '/ws/user-spectrum?user_session_id=bbbbbbbb&bins=256');
  ok(await until(() => a.radio()?.antennas?.length === 2), `hwinfo lists the owner's antennas: ${JSON.stringify(a.radio()?.antennas)}`);
  ok(await until(() => a.radio()?.antenna === 'Sky Loop'), `…and the one the switch READS BACK as connected: ${a.radio()?.antenna}`);
  ok(a.radio()?.antennaSwitch?.connected === true, 'antennaSwitch.connected is reported');
  fromClient.length = 0;
  a.send({ type: 'antenna', antenna: 'VHF Vertical' });
  ok(await until(() => b.radio()?.antenna === 'VHF Vertical'), 'choosing VHF Vertical: the OTHER listener sees it switched (read back from the switch)');
  const cmds = fromClient.filter((m) => m.payload !== '').map((m) => `${m.topic}=${m.payload}`);
  ok(cmds.join(' ') === 'cmnd/antsw/POWER1=OFF cmnd/antsw/POWER2=ON', `…break before make on the broker: ${cmds.join(' ')}`);
  a.send({ type: 'tune', frequency: 7100000, mode: 'lsb' });
  ok(await until(() => a.radio()?.antenna === 'Sky Loop'), 'tuning into 7.1 MHz: the per-band preset switches to Sky Loop');
  a.send({ type: 'antenna', antenna: 'No Such Aerial' });
  await sleep(400);
  ok(a.radio()?.antenna === 'Sky Loop', 'an antenna that does not exist changes nothing');
  const r401 = await fetch(`http://127.0.0.1:${s1.port}/vibeserver/antswitch/search`).then((r) => r.status).catch(() => 0);
  ok(r401 === 401, `the LAN search is admin-only (${r401} without the password)`);
  {
    // ★ As the owner: the admin proof is HMAC-SHA256(password, nonce) — the same pair the clients send.
    const nonce = (await fetch(`http://127.0.0.1:${s1.port}/vibeserver/auth`).then((r) => r.json())).nonce;
    const auth = crypto.createHmac('sha256', 'antsw-admin-pass').update(nonce).digest('hex');
    const found = await fetch(`http://127.0.0.1:${s1.port}/vibeserver/antswitch/search?host=127.0.0.1&port=${brokerPort}`
                              + `&vs_admin_nonce=${nonce}&vs_admin_auth=${auth}`).then((r) => r.json()).catch(() => null);
    const dev = found?.devices?.[0];
    ok(dev?.name === 'Generic Antenna Switch Example' && dev?.relays?.length === 2 && dev?.relays?.[1]?.label === 'VHF Vertical',
       `as the owner, "Search for switch…" finds it: ${dev ? `${dev.name} on ${dev.broker}, ${dev.relays.length} relays` : JSON.stringify(found)}`);
  }
  a.close(); b.close(); s1.stop(); s1 = null;
  await sleep(600);

  console.log('── the owner has locked the aerial ──');
  s2 = await run(true);
  const c = ws(s2.port, '/ws/user-spectrum?user_session_id=cccccccc&bins=256');
  ok(await until(() => c.radio()?.antennaLocked === true), 'hwinfo says the aerial is locked');
  const before = c.radio()?.antenna;
  c.send({ type: 'antenna', antenna: before === 'Sky Loop' ? 'VHF Vertical' : 'Sky Loop' });
  ok(await until(() => c.notices().some((w) => /fixed the aerial/.test(w))), 'a listener is refused, and told why');
  ok(c.radio()?.antenna === before, '…and the antenna has not moved');
  c.close();
} finally {
  s1?.stop(); s2?.stop(); broker.close();
}
console.log(`   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
