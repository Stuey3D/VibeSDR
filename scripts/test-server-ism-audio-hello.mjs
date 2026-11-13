// test-server-ism-audio-hello.mjs — beta73 (Stuart's video, 2026-11-13). Two faults on the Pi 500's V4L, one listener:
//  1. Every return from the background put ISM back on 867.197 MHz. The app's native audio pump says hello with the last
//     tune IT sent, and an ISM band pick never goes through it; the server took that `tune` on the AUDIO socket as an
//     exact ISM frequency. Now an audio socket's tune leaves ISM on its band — a deliberate frequency still works on the
//     spectrum socket (the frequency box).
//  2. "Another user has tuned to …" on his own band picks. On a SHARED dial the server never named the asker as the tuner
//     for an ISM band change; now it does (the asker's `dial` says mine, the other listener's names the asker).
// Real server, fake rtl_tcp, a synthetic 433 clip. Silent: the audio socket's frames are dropped, nothing is played.
//   VIBESERVER_BIN=…/vibeserver VIBE_ISM_CLIP=…/clip.u8 node scripts/test-server-ism-audio-hello.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_ISM_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_ISM_CLIP (make one: test-ism --write-clip clip.u8 434130000 2400000)');
  process.exit(3);
}
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 100; i++) { if (await f()) return true; await sleep(100); } return !!(await f()); };

async function server(users) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-ismhello-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'ISM hello test', locator: 'IO92nh', adminPass: 'ism-hello-admin',
    radios: [{ serial: 'ism-hello', label: 'Test radio', enabled: true, configured: true, users }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--ism', CLIP], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', String(users)],
                    { env, stdio: ['ignore', log, log] });
  const up = await until(async () => { try { return (await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) })).ok; } catch { return false; } }, 15000);
  return { dir, port, up, stop: async () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); await sleep(400); } };
}
function spec(port, sid) {
  const w = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=${sid}&bins=256&proto=1`);
  w.binaryType = 'arraybuffer';
  const s = { w, ism: null, cfg: null, dial: null };
  w.onmessage = (e) => {
    if (typeof e.data !== 'string') return;
    const j = JSON.parse(e.data);
    if (j.type === 'ism') s.ism = j;
    if (j.type === 'config') s.cfg = j;
    if (j.type === 'dial') s.dial = j;
  };
  s.open = new Promise((r) => { w.onopen = r; });
  return s;
}
const dirs = [];
try {
  // ── 1. one listener: the audio socket's hello does not move ISM ──
  {
    const S = await server(1); dirs.push(S);
    ok(S.up, 'the one-listener server is up');
    const sid = 'ismhello01';
    const a = spec(S.port, sid); await a.open; await sleep(1000);
    a.w.send(JSON.stringify({ type: 'ism', on: 1, band: '868' }));
    ok(await until(() => a.ism && a.ism.band === '868', 6000), `ISM on 868 by name: ${a.ism && a.ism.bandName}`);
    // the native pump's reconnect: the audio socket opens and restates the old tune it last sent
    const au = new WebSocket(`ws://127.0.0.1:${S.port}/ws/audio?user_session_id=${sid}&codec=opus`);
    au.binaryType = 'arraybuffer';
    au.onmessage = () => {};   // ★ silent: frames dropped
    await new Promise((r) => { au.onopen = r; });
    au.send(JSON.stringify({ type: 'tune', frequency: 867197000, mode: 'am', bandwidthLow: -5000, bandwidthHigh: 5000 }));
    await sleep(2500);
    ok(a.ism && a.ism.band === '868' && !a.ism.custom, `★ the audio socket's hello leaves ISM on its band: ${a.ism && a.ism.band} custom ${a.ism && a.ism.custom}`);
    ok(a.cfg && Math.abs(a.cfg.vfo - 868.6e6) < 1, `★ …and the dial on 868.600: vfo ${a.cfg && a.cfg.vfo}`);
    const logTxt = fs.readFileSync(path.join(S.dir, 'server.log'), 'utf8');
    ok(/a tune on an audio socket ignored/.test(logTxt), 'the server says why it ignored it');
    // the frequency box still works — on the spectrum socket
    a.w.send(JSON.stringify({ type: 'tune', frequency: 868300000 }));
    ok(await until(() => a.ism && a.ism.custom === 1 && Math.abs(a.ism.centre - 868.3e6) < 1e5, 6000) || (a.cfg && Math.abs(a.cfg.vfo - 868.3e6) < 1),
       `a typed frequency on the spectrum socket is still an exact ISM frequency: vfo ${a.cfg && a.cfg.vfo}`);
    ok(!a.dial, 'a one-listener radio sends no dial state (so the app must read null as "nobody else")');
    au.close(); a.w.close();
    await S.stop();
  }
  // ── 2. a shared dial: the asker of an ISM band change is named the tuner ──
  {
    const S = await server(3); dirs.push(S);
    ok(S.up, 'the shared server is up');
    const a = spec(S.port, 'ismshareA'); await a.open;
    const b = spec(S.port, 'ismshareB'); await b.open;
    ok(await until(() => a.dial && b.dial, 5000), 'both listeners have dial state');
    b.w.send(JSON.stringify({ type: 'tune', frequency: 96500000 }));    // B tunes first: B is the tuner
    ok(await until(() => a.dial && b.dial && b.dial.mine === true, 5000), `B tuned: B's dial says mine (${b.dial && b.dial.mine})`);
    a.w.send(JSON.stringify({ type: 'ism', on: 1, band: '433' }));
    ok(await until(() => a.ism && a.ism.band === '433', 6000), 'A put the radio into ISM 433');
    ok(await until(() => a.dial && a.dial.mine === true, 4000), `★ A's dial now says mine (${a.dial && a.dial.mine}, tuner ${a.dial && a.dial.tuner}, you ${a.dial && a.dial.you})`);
    ok(b.dial && b.dial.mine === false && b.dial.tuner === a.dial.you, `★ B's dial names A as the tuner (tuner ${b.dial && b.dial.tuner}, A is ${a.dial && a.dial.you})`);
    a.w.send(JSON.stringify({ type: 'ism', on: 0 }));
    await sleep(800);
    a.w.close(); b.w.close();
    await S.stop();
  }
} finally {
  for (const S of dirs) { try { await S.stop(); } catch {} }
  if (fail) for (const S of dirs) console.log(`   (server log: ${path.join(S.dir, 'server.log')})`);
  else for (const S of dirs) fs.rmSync(S.dir, { recursive: true, force: true });
}
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
