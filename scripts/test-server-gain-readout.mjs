// test-server-gain-readout.mjs — V12: a listener's manual gain / AUTO is broadcast to every listener's readout (hwinfo agc +
// gainNow). The real server on a fake rtl_tcp. Needs VIBESERVER_BIN and VIBE_APRS_CLIP (exit 3 = not run). Silent.
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_APRS_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_APRS_CLIP (test-aprs --write-clip b.cu8 2400000: any RTL clip will do)');
  process.exit(3);
}
const RATE = 2400000;
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 200; i++) { if (await f()) return true; await sleep(200); } return !!(await f()); };

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], open: false, dropBin: false };
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
const jsons = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => JSON.parse(t));
const lastOf = (st, type) => { const a = jsons(st, type); return a[a.length - 1] || null; };

async function server(clip, centre, args, tag, radioExtra = {}, envExtra = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Gain readout test', locator: 'IO92nh', adminPass: 'gain-test-admin',
    radios: [{ serial: `${tag}-test`, label: 'Test radio', enabled: true, configured: true, users: 1, ...radioExtra }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', clip, '--hf-centre', String(centre)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), ...envExtra };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), ...args], { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  const gallery = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver/gallery.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  return { port, dir, status, gallery, stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}


// ★★★ A LISTENER'S GAIN IS EVERYONE'S READOUT (Stuart, 2026-11-10, the Pi 500 V4L: "I've gone manual to boost the gain but
//     the AGC status in the bottom still shows AGC 38.6"). A manual gain switches VibeAGC off and AUTO switches it on; the
//     server must SAY so to every listener on the dial (hwinfo agc + gainNow), not leave the last report standing.
{
  console.log('── a manual gain / AUTO reaches every listener’s readout ──');
  const S = await server(CLIP, 144800000, ['--freq', '144800000', '--mode', 'nfm', '--users', '2'], 'gain', { users: 2 });
  const hw = (st) => jsons(st, 'hwinfo').filter((h) => typeof h.agc !== 'undefined');
  try {
    const a = ws(S.port, '/ws/user-spectrum?user_session_id=gaintestA1&bins=256&proto=1');
    const b = ws(S.port, '/ws/user-spectrum?user_session_id=gaintestB1&bins=256&proto=1');
    await until(() => a.open && b.open, 4000);
    await sleep(1500);
    a.send({ type: 'gain', auto: true });
    ok(await until(() => { const h = hw(b).at(-1); return !!h && h.agc === 1 || h?.agc === true; }, 6000), `AUTO: the OTHER listener is told VibeAGC is on (${JSON.stringify(hw(b).at(-1)?.agc)})`);
    const nA = hw(a).length, nB = hw(b).length;
    a.send({ type: 'gain', value: 496 });
    ok(await until(() => hw(a).slice(nA).some((h) => !h.agc), 6000), `manual 49.6 dB: the sender is told VibeAGC is off`);
    ok(await until(() => hw(b).slice(nB).some((h) => !h.agc), 6000), `…and so is the other listener (${JSON.stringify(hw(b).at(-1)?.agc)})`);
    ok(await until(() => { const h = hw(b).at(-1); return !!h && Math.abs((h.gainNow ?? -1) - 496) <= 20; }, 6000), `…with the gain the radio is on (${hw(b).at(-1)?.gainNow})`);
    const nB2 = hw(b).length;
    a.send({ type: 'gain', auto: true });
    ok(await until(() => hw(b).slice(nB2).some((h) => h.agc === 1 || h.agc === true), 6000), 'AUTO again: the other listener is told VibeAGC is back on');
    a.close(); b.close();
  } finally { S.stop(); }
}
console.log(`\ntest-server-gain-readout: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
