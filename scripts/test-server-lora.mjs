// test-server-lora.mjs — V12 LoRa in ISM mode through the REAL server (docs/v12/LORA-MESH.md): a fake rtl_tcp replaying
// a synthetic 868 MHz clip (`test-lora --write-clip clip.u8 868600000 2400000`: Meshtastic node info, a position, a
// public text, a private-channel packet and a direct message; a MeshCore repeater's advert, a Public-channel message and
// a message on another channel; three LoRaWAN uplinks), a spectrum socket, and the contract: the rows by kind, their
// lines and fields, the positions, the public messages, the `lora` status block — and the PRIVACY rule on the wire (no
// private or direct text, no LoRaWAN payload byte in any form). Silent: no audio socket is opened.
//   VIBESERVER_BIN=…/vibeserver VIBE_LORA_CLIP=…/clip.u8 node scripts/test-server-lora.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_LORA_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_LORA_CLIP (make one: test-lora --write-clip clip.u8 868600000 2400000)');
  process.exit(3);
}
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 100; i++) { if (await f()) return true; await sleep(100); } return !!(await f()); };

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-lora-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, name: 'LoRa test', locator: 'IO92nh', adminPass: 'lora-test-admin',
  radios: [{ serial: 'lora-test', label: 'Test radio', enabled: true, configured: true, users: 1 }],
}));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--ism868', CLIP], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const log = fs.openSync(path.join(dir, 'server.log'), 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', '1'],
                  { env, stdio: ['ignore', log, log] });
const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };

try {
  ok(await until(async () => !!(await status()), 15000), 'the server is up');
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=loratest01&bins=256&proto=1`);
  ws.binaryType = 'arraybuffer';
  const msgs = [];
  let last = null, off = false;
  ws.onmessage = (e) => {
    if (typeof e.data !== 'string') return;
    msgs.push(e.data);
    const j = JSON.parse(e.data);
    if (j.type === 'ism') last = j;
    if (j.type === 'ism_off') off = true;
  };
  await new Promise((r) => { ws.onopen = r; });
  await sleep(1200);
  ws.send(JSON.stringify({ type: 'ism', on: 1, band: '868' }));
  ok(await until(() => !!last && last.band === '868', 5000), `on the 868 band: ${last && last.bandName}`);
  ok(await until(() => !!last && !!last.lora, 5000), `the report carries a lora block: ${JSON.stringify(last && last.lora)}`);
  ok(last.lora && last.lora.chans >= 5 && last.lora.dets >= 20, `channels and detectors running (${last.lora && last.lora.chans} / ${last.lora && last.lora.dets})`);
  const row = (f) => last && last.devices.find(f);
  const base = () => row((d) => d.kind === 'meshtastic' && d.node === '!1a2b3c4d');
  ok(await until(() => { const b = base(); return b && b.long === 'Northampton Base' && b.lat !== undefined; }, 40000),
     `the Meshtastic base: ${base() ? base().line : '-'}`);
  const b = base();
  ok(b && b.short === 'NBAS' && b.hw === 'Heltec V3' && b.role === 'router' && Math.abs(b.lat - 52.24) < 1e-5 && Math.abs(b.lon + 0.895) < 1e-5 && b.sf === 11,
     'its fields: short name, hardware, role, position, SF');
  const rptr = () => row((d) => d.kind === 'meshcore' && d.long === 'Moulton Rptr');
  ok(await until(() => !!rptr(), 30000), `the MeshCore repeater: ${rptr() ? rptr().line : '-'}`);
  ok(rptr() && Math.abs(rptr().lat - 52.29) < 1e-5 && rptr().role === 'repeater', 'its position and kind');
  ok(await until(() => ['260B1234', '01ABCDEF', '27000042'].every((n) => !!row((d) => d.kind === 'lorawan' && d.node === n)), 30000),
     `three LoRaWAN devices: ${last.devices.filter((d) => d.kind === 'lorawan').map((d) => d.line).join(' | ')}`);
  const ttn = row((d) => d.node === '260B1234');
  ok(ttn && ttn.net === 'The Things Network' && ttn.fcnt === 4211 && ttn.fport === 2 && ttn.encLen === 12 && / 12 bytes encrypted/.test(ttn.line), `the TTN line: ${ttn && ttn.line}`);
  ok(await until(() => Array.isArray(last.msgs) && last.msgs.some((m) => m.text === 'Hello from the Racecourse')
                    && last.msgs.some((m) => m.text === 'anyone on the mesh tonight?' && m.from === 'Alice'), 30000),
     `the public messages: ${(last.msgs || []).map((m) => `${m.from}: ${m.text}`).join(' | ')}`);
  ok(last.lora.mapped >= 2, `nodes with a position for the map: ${last.lora.mapped}`);
  const priv = row((d) => d.node === '!99aabbcc');
  ok(priv && priv.priv >= 1 && /private channel/.test(priv.line), `a private-channel node, header only: ${priv && priv.line}`);
  // ★★★ PRIVACY ON THE WIRE
  await sleep(1500);
  const wire = msgs.join('\n');
  for (const bad of ['PRIVATE-SECRET', 'DIRECT-SECRET', 'MESHCORE-SECRET']) ok(!wire.includes(bad), `the wire never carries "${bad}"`);
  let leak = 0;
  for (let i = 0; i + 4 <= 20; i++) {
    const v = [0, 1, 2, 3].map((k) => 0xE0 + i + k);
    for (const f of [v.map((x) => x.toString(16).toUpperCase().padStart(2, '0')).join(''), v.map((x) => x.toString(16)).join(''), v.join(','), v.map((x) => `\\u00${x.toString(16)}`).join('')]) if (wire.includes(f)) leak++;
  }
  ok(leak === 0, `no LoRaWAN payload byte on the wire, in any form (${leak})`);
  ws.send(JSON.stringify({ type: 'ism', on: 0 }));
  ok(await until(() => off, 5000), 'ism_off arrives');
  ws.close();
} finally {
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(500);
  if (fail) console.log(`   (server log: ${path.join(dir, 'server.log')})`);
  else fs.rmSync(dir, { recursive: true, force: true });
}
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
