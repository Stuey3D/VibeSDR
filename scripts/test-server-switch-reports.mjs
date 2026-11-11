// test-server-switch-reports.mjs — V12: every audio switch a listener presses is ANSWERED with that listener's hwinfo
// showing the new state (nr, notch, nb, nbx, ceq, wsp, nfmvoice). The real server on a fake rtl_tcp. Needs VIBESERVER_BIN and VIBE_APRS_CLIP (exit 3 = not run). Silent.
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
    configured: true, name: 'Switch report test', locator: 'IO92nh', adminPass: 'gain-test-admin',
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


// ★★★ A PRESS IS ANSWERED (Stuart, 2026-11-11, the R36S: Bias-T / AUTO NOTCH / NOISE BLANKER "will switch back on again"
//     while the menu stays open). A client shows its press for a few seconds, then the radio's report — so a handler that
//     acts and never reports leaves the OLD report standing and the row flips back. Each switch, on then off: a hwinfo
//     arrives for this listener, within 3 s (the R36S's hold), saying the state just set. nr / notch must be THIS
//     listener's (they were reported from the shared pair while stored per listener).
async function round(withAudio) {
  console.log(`── each audio switch is answered with the listener’s new state — ${withAudio ? 'a range-locked radio: each listener its own pipeline' : 'the shared dial (one pipeline — the R36S, a single-user server)'} ──`);
  // ★ a listener's own pipeline exists only on a range-locked radio with room for more than one (perClientDsp)
  const S = await server(CLIP, 144800000, ['--freq', '144800000', '--mode', 'nfm', '--users', '4', ...(withAudio ? ['--lock-freq', '144800000'] : [])],
                         withAudio ? 'swa' : 'sws', { users: 4 });
  const tag = withAudio ? 'P' : 'S';
  try {
    const a = ws(S.port, `/ws/user-spectrum?user_session_id=sw${tag}A1&bins=256&proto=1`);
    const b = ws(S.port, `/ws/user-spectrum?user_session_id=sw${tag}B1&bins=256&proto=1`);
    const aa = withAudio ? ws(S.port, `/ws/audio?user_session_id=sw${tag}A1&codec=opus`) : null;
    const ba = withAudio ? ws(S.port, `/ws/audio?user_session_id=sw${tag}B1&codec=opus`) : null;
    await until(() => a.open && b.open && (!aa || (aa.open && ba.open)), 4000);
    await sleep(2000);
    const tx = a;
    for (const k of ['notch', 'nr', 'nb', 'nbx', 'ceq', 'wsp', 'nfmvoice']) {
      const start = lastOf(a, 'hwinfo');
      if (!start || typeof start[k] === 'undefined') { ok(false, `${k}: hwinfo reports it`); continue; }
      for (const want of [!start[k], !!start[k]]) {
        const n = jsons(a, 'hwinfo').length;
        tx.send({ type: k, on: want });
        ok(await until(() => jsons(a, 'hwinfo').slice(n).some((h) => h[k] === want) || jsons(tx, 'hwinfo').slice(n).some((h) => h[k] === want), 3000), `${k} ${want ? 'on' : 'off'}: answered with ${k}:${want}`);
      }
    }
    const nb = jsons(b, 'hwinfo').length;
    tx.send({ type: 'notch', on: true });
    if (withAudio) {
      // ★ per listener: B's own report never takes A's notch
      await sleep(1500);
      ok(jsons(b, 'hwinfo').slice(nb).every((h) => h.notch !== true) && lastOf(b, 'hwinfo')?.notch !== true,
         'notch is the listener’s own — the other listener’s report never says it is on');
    } else {
      // ★ shared: the other listener's readout follows (the shared-dial contract)
      ok(await until(() => jsons(b, 'hwinfo').slice(nb).some((h) => h.notch === true), 3000), 'shared notch: the other listener is told too');
    }
    a.close(); b.close(); aa?.close(); ba?.close();
  } finally { S.stop(); }
}
await round(false);
await round(true);
console.log(`\ntest-server-switch-reports: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
