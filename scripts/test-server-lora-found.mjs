// test-server-lora-found.mjs — V12 LoRa FOUND ON AIR through the REAL server (vibe_lora_discover.h, docs/v12/LORA-MESH.md
// §Found on air): a fake rtl_tcp replaying a 915 MHz clip (`test-lora --write-clip-found clip.u8 915250000 2400000`: a
// MeshCore repeater on 915.000 MHz BW125 SF9 — on no plan's channel — every 2.5 s). A server in Brazil (GG33if, Kiko's
// locator) lands ISM on 923; on the 915 band the repeater is found, a detector started there, and its adverts decoded and
// labelled "found on air"; the report says "N plan channels + 1 found on air"; the server is restarted and the found
// channel is running from the start (its file beside the radio's bookmarks). Silent: no audio socket is opened.
//   VIBESERVER_BIN=…/vibeserver VIBE_LORA_FOUND_CLIP=…/clip.u8 node scripts/test-server-lora-found.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_LORA_FOUND_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_LORA_FOUND_CLIP (make one: test-lora --write-clip-found clip.u8 915250000 2400000)');
  process.exit(3);
}
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 100; i++) { if (await f()) return true; await sleep(100); } return !!(await f()); };

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-lorafound-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, name: 'LoRa found test', locator: 'GG33if', adminPass: 'lora-found-admin',
  radios: [{ serial: 'lora-found', label: 'Test radio', enabled: true, configured: true, users: 1 }],
}));
const rtlPort = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--ism915', CLIP], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
let srv = null, port = 0;
const startServer = async () => {
  port = await freePort();
  const log = fs.openSync(path.join(dir, 'server.log'), 'a');
  srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', '1'],
              { env, stdio: ['ignore', log, log] });
};
const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
const session = async (id) => {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=${id}&bins=256&proto=1`);
  ws.binaryType = 'arraybuffer';
  const s = { ws, last: null };
  ws.onmessage = (e) => { if (typeof e.data !== 'string') return; const j = JSON.parse(e.data); if (j.type === 'ism') s.last = j; };
  await new Promise((r) => { ws.onopen = r; });
  await sleep(1000);
  return s;
};
const findFile = (d) => { for (const f of fs.readdirSync(d, { withFileTypes: true })) {
  const p = path.join(d, f.name); if (f.isDirectory()) { const q = findFile(p); if (q) return q; } else if (f.name.endsWith('.lora-found.txt')) return p; } return ''; };

try {
  await startServer();
  ok(await until(async () => !!(await status()), 15000), 'the server is up');
  const st = await status();
  ok(st.ismLanding === '923', `a server in Brazil (GG33if) lands ISM on 923: ${st.ismLanding}`);
  let s = await session('lorafound01');
  s.ws.send(JSON.stringify({ type: 'ism', on: 1, band: '915' }));
  ok(await until(() => s.last && s.last.band === '915' && s.last.lora, 6000), `on the 915 band: ${JSON.stringify(s.last && s.last.lora)}`);
  const plan0 = s.last.lora.chans;
  const torre = () => s.last && s.last.devices.find((d) => d.kind === 'meshcore' && /^Torre Paran/.test(d.long || ''));
  ok(await until(() => !!torre(), 60000), `the repeater on 915.000 decoded via a channel found on air: ${torre() ? torre().line : '-'}`);
  ok(torre() && torre().onAir === 1 && / found on air/.test(torre().line), 'its row says "found on air"');
  const L = s.last.lora;
  const f0 = (L.foundList || [])[0];
  ok(L.found === 1 && L.chans === plan0 && f0 && Math.abs(f0.hz - 915.0e6) < 10e3 && f0.bw === 125000 && f0.run === 1 && f0.good.includes(9) && !f0.std,
     `the report: ${L.chans} plan channels + ${L.found} found on air (${JSON.stringify(f0)})`);
  s.ws.send(JSON.stringify({ type: 'ism', on: 0 }));
  await sleep(1500);
  s.ws.close();
  const file = findFile(path.join(dir, 'data'));
  ok(!!file && / 91500\d000 125000 /.test(fs.readFileSync(file, 'utf8')), `kept beside the radio's bookmarks: ${file ? path.relative(dir, file) : '(no file)'}`);

  // ★ a restart starts on it: running from the first report, the next advert decoded with no search first
  srv.kill('SIGTERM'); await sleep(1500);
  await startServer();
  ok(await until(async () => !!(await status()), 15000), 'the server is back up');
  s = await session('lorafound02');
  s.ws.send(JSON.stringify({ type: 'ism', on: 1, band: '915' }));
  ok(await until(() => s.last && s.last.band === '915' && s.last.lora, 6000), 'on the 915 band again');
  ok(s.last.lora.found === 1 && (s.last.lora.foundList || [])[0] && s.last.lora.foundList[0].run === 1, `the found channel runs from the start: ${JSON.stringify(s.last.lora.foundList)}`);
  ok(await until(() => !!torre(), 12000), `…and the next advert is decoded within one clip cycle: ${torre() ? torre().line : '-'}`);
  s.ws.send(JSON.stringify({ type: 'ism', on: 0 }));
  s.ws.close();
} finally {
  if (srv) srv.kill('SIGTERM');
  rtl.kill('SIGTERM');
  await sleep(500);
  if (fail) console.log(`   (server log: ${path.join(dir, 'server.log')})`);
  else fs.rmSync(dir, { recursive: true, force: true });
}
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
