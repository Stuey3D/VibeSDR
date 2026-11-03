// ★★★ THE OWNER'S NAMES FOR A RADIO'S OWN ANTENNA PORTS, END TO END (V12, 2026-11-03) — the real vibeserver on a fake
// rtl_tcp. Unit rules: vibeserver/test-port-names.cpp. Here, the paths a name travels:
//   1. a config.json with untidy and hostile names loads; the running radio takes them CLEANED (its log says what it
//      holds) and the config API gives them back cleaned, bounded and escaped (no raw "</script>" in the response);
//   2. a dongle has ONE input, so hwinfo lists no ports and carries no `antennaNames` — nothing to name on screen;
//   3. a port this radio does not have ({type:"antenna"}) changes nothing and the server carries on;
//   4. a save through the config API replaces the names LIVE — the running radio logs the new set with no restart — and
//      the file holds them cleaned;
//   5. the escaping holds in the file too (a quote and a backslash in a name survive a save and a reload).
// ★ SILENT: no audio, no browser. Loopback only. usage: VIBESERVER_BIN=… node scripts/test-antenna-port-names.mjs
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

const ADMIN = 'portnames-admin-pass';
const SERIAL = '31A0B7C';
const HOSTILE = '</script><b onclick="x">y</b>';
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-portnames-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const cfgPath = path.join(dir, 'config.json');
fs.writeFileSync(cfgPath, JSON.stringify({
  configured: true, name: 'Port Names Test', locator: 'IO92nh', adminPass: ADMIN,
  radios: [{ serial: SERIAL, driver: 'rtlsdr', label: 'Test radio', enabled: true, configured: true, users: 3,
    antenna: 'Roof mast',
    antennaNames: JSON.stringify({ 'RX2 (A)': '  Disc\tone  ', 'TX/RX (A)': HOSTILE, 'RX2 (B)': 'z'.repeat(90), 'TX/RX (B)': '   ' }) }],
}));

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [] };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (buf.length > (8 << 20)) { buf = Buffer.alloc(0); return; }
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let l = buf[1] & 0x7f, off = 2;
      if (l === 126) { if (buf.length < 4) break; l = buf.readUInt16BE(2); off = 4; }
      else if (l === 127) { if (buf.length < 10) break; l = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + l) break;
      const p = buf.subarray(off, off + l); buf = buf.subarray(off + l);
      if (op === 0x9) { const q = Buffer.alloc(6 + p.length); q[0] = 0x8a; q[1] = 0x80 | p.length; q.writeUInt32BE(0, 2); p.copy(q, 6); s.write(q); continue; }
      if (op === 0x1) { st.txt.push(p.toString()); if (st.txt.length > 200) st.txt.shift(); }
    }
  });
  st.send = (o) => { const b = Buffer.from(JSON.stringify(o)); const h = Buffer.alloc(b.length < 126 ? 6 : 8); h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b])); };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  st.radio = () => { const h = st.txt.filter((x) => x.includes('"type":"hwinfo"')).map((x) => JSON.parse(x)); return h.length ? h[h.length - 1].radio : null; };
  st.hwCount = () => st.txt.filter((x) => x.includes('"type":"hwinfo"')).length;
  return st;
}
const until = async (f, ms = 5000) => { for (let i = 0; i < ms / 50; i++) { if (await f()) return true; await sleep(50); } return !!(await f()); };

const rtlPort = await freePort();
let port = await freePort();
while (port === rtlPort) port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: cfgPath, VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), HOME: dir };
const logPath = path.join(dir, 'server.log');
const logFd = fs.openSync(logPath, 'w');
const srv = spawn(BIN, ['--radio-serial', SERIAL, '--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000',
                        '--freq', '96100000', '--mode', 'wfm', '--users', '3'], { env, stdio: ['ignore', logFd, logFd] });
const stopAll = () => { for (const p of [srv, rtl]) { try { p.kill('SIGKILL'); } catch {} } };
process.on('exit', stopAll);
const deadline = setTimeout(() => { console.log('   FAIL the 90 s deadline passed'); stopAll(); process.exit(1); }, 90000);
const L = () => { try { return fs.readFileSync(logPath, 'utf8').slice(-262144); } catch { return ''; } };
const namesLogged = () => L().split('\n').filter((l) => l.includes('antenna port names:'));
const admin = async () => {
  const nonce = (await fetch(`http://127.0.0.1:${port}/vibeserver/auth`).then((r) => r.json())).nonce;
  return `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(nonce).digest('hex')}`;
};

try {
  for (let i = 0; i < 100; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  // 1. loaded and cleaned
  ok(await until(() => namesLogged().length > 0), 'the running radio took the names from config.json');
  const first = namesLogged()[0] || '';
  ok(first.includes('"RX2 (A)":"Disc one"'), `…cleaned (tab → space, trimmed): ${first.slice(first.indexOf('{'))}`);
  ok(!first.includes('TX/RX (B)'), '…an all-space name is no entry');
  ok(first.includes('"RX2 (B)":"' + 'z'.repeat(40) + '"') && !first.includes('z'.repeat(41)), '…a 90-character name bounded to 40');
  ok(!first.includes('</script>') && first.includes('\\u003c/script\\u003e'), '…and < > escaped where it is written');
  const getCfg = async () => fetch(`http://127.0.0.1:${port}/vibeserver/config?${await admin()}`).then((r) => r.text()).catch(() => '');
  const raw = await getCfg();
  ok(raw.length > 0 && !raw.includes('</script>'), 'the config API answers with no raw "</script>" anywhere');
  let cfg = null; try { cfg = JSON.parse(raw); } catch {}
  const mine = cfg?.radios?.find((r) => r.serial === SERIAL);
  let names = {}; try { names = JSON.parse(mine?.antennaNames || '{}'); } catch {}
  ok(names['RX2 (A)'] === 'Disc one' && names['TX/RX (A)'] === HOSTILE && !('TX/RX (B)' in names),
     `the config API gives the names back cleaned, the owner's text intact once parsed: ${JSON.stringify(names)}`);
  ok(mine?.antenna === 'Roof mast', '…the aerial description beside them is its own field');

  // 2. one input = no ports, no names in hwinfo
  const a = ws(port, '/ws/user-spectrum?user_session_id=aaaaaaaa&bins=256');
  ok(await until(() => a.radio() !== null), 'hwinfo arrives');
  ok(!a.radio()?.antennas?.length && a.radio()?.antennaNames === undefined,
     `a dongle lists no ports and carries no antennaNames (nothing to name on screen): ${JSON.stringify({ antennas: a.radio()?.antennas, antennaNames: a.radio()?.antennaNames })}`);

  // 3. a port it does not have
  a.send({ type: 'antenna', antenna: 'RX2 (A)' });
  await sleep(500);
  a.send({ type: 'ping' });
  ok(await until(() => a.txt.some((x) => x.includes('"type":"pong"'))), 'a port this radio does not have changes nothing, and the server answers on');
  ok(!a.radio()?.antennas?.length, '…still no ports listed');

  // 4. a save replaces them live
  const before = namesLogged().length;
  const next = JSON.parse(raw);
  const r0 = next.radios.find((r) => r.serial === SERIAL);
  r0.antennaNames = JSON.stringify({ 'RX2 (A)': 'Discone', 'TX/RX (A)': 'FM "Yagi" \\ east' });
  const post = await fetch(`http://127.0.0.1:${port}/vibeserver/config?${await admin()}`, { method: 'POST', body: JSON.stringify(next),
                           headers: { 'Content-Type': 'application/json' } }).then(async (r) => `${r.status} ${await r.text()}`).catch((e) => String(e));
  ok(/^200 /.test(post), `the save is accepted: ${post.slice(0, 120)}`);
  ok(await until(() => namesLogged().length > before, 5000), 'the running radio took the new names LIVE, without a restart');
  const live = namesLogged().slice(-1)[0] || '';
  ok(live.includes('"RX2 (A)":"Discone"') && !live.includes('Disc one'), `…the new set replaced the old: ${live.slice(live.indexOf('{'))}`);
  ok(srv.exitCode === null && srv.signalCode === null, '…and the server is still the same process');

  // 5. escaping in the file, across a reload
  let disk = null; try { disk = JSON.parse(fs.readFileSync(cfgPath, 'utf8')); } catch {}
  let dn = {}; try { dn = JSON.parse(disk?.radios?.find((r) => r.serial === SERIAL)?.antennaNames || '{}'); } catch {}
  ok(dn['TX/RX (A)'] === 'FM "Yagi" \\ east', `the file holds the quote and the backslash intact: ${JSON.stringify(dn)}`);
  const again = await getCfg();
  let n2 = {}; try { n2 = JSON.parse(JSON.parse(again).radios.find((r) => r.serial === SERIAL).antennaNames); } catch {}
  ok(n2['TX/RX (A)'] === 'FM "Yagi" \\ east' && n2['RX2 (A)'] === 'Discone', '…and read back through the API the same');
  a.close();
} finally {
  clearTimeout(deadline);
  stopAll();
}
console.log(`   ${pass} passed, ${fail} failed`);
if (fail) console.log(`   (server log: ${logPath})`);
process.exit(fail ? 1 : 0);
