// test-server-ism256.mjs — V12 "Use 2.56 MS/s sample rate for ISM" (config ism256, an RTL only, never the default) through the
// REAL server on a fake rtl_tcp (docs/v12/LORA-MESH.md §MeshCom): with the switch on, entering ISM takes the radio from
// 2.048 to 2.56 MS/s (hwinfo rateNow), the 433 band then runs MORE LoRa plan channels (MeshCom 433.175 fits), and leaving
// puts 2.4 back; with the switch off nothing changes rate. The switch toggles live ({"type":"ism256"}, admin-gated) and is
// persisted to the radio's config. Silent: no audio socket. Keepalive pings on the spectrum socket.
//   VIBESERVER_BIN=…/vibeserver VIBE_ISM_CLIP=…/clip.u8 node scripts/test-server-ism256.mjs
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

async function scenario(label, ism256, body) {
  console.log(`── ${label} ──`);
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-ism256-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  const cfgPath = path.join(dir, 'config.json');
  fs.writeFileSync(cfgPath, JSON.stringify({
    configured: true, name: 'ISM 2.56 test', locator: 'IO92nh', adminPass: 'ism-test-admin',
    radios: [{ serial: 'ism-test', label: 'Test radio', enabled: true, configured: true, users: 1, ism256 }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000', '--ism', CLIP], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: cfgPath, VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', '1'],
                    { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  let ws = null, ping = null;
  try {
    ok(await until(async () => !!(await status()), 15000), 'the server is up');
    ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=ism256t01&bins=256&proto=1`);
    ws.binaryType = 'arraybuffer';
    const S = { ism: null, hw: null, off: false, txt: [] };
    ws.onmessage = (e) => {
      if (typeof e.data !== 'string') return;
      S.txt.push(e.data);
      const j = JSON.parse(e.data);
      if (j.type === 'ism') S.ism = j;
      if (j.type === 'ism_off') S.off = true;
      if (j.type === 'hwinfo' || j.rateNow !== undefined) S.hw = j;
    };
    await new Promise((r) => { ws.onopen = r; });
    ping = setInterval(() => { try { ws.send(JSON.stringify({ type: 'ping' })); } catch {} }, 2000);   // ★ keepalive
    await sleep(1200);
    await body({ ws, S, dir, cfgPath, st: await status(), log: () => fs.readFileSync(path.join(dir, 'server.log'), 'utf8') });
  } finally {
    clearInterval(ping);
    try { ws?.close(); } catch {}
    srv.kill('SIGTERM'); rtl.kill('SIGTERM');
    await sleep(600);
    try { fs.rmSync(dir, { recursive: true, force: true }); } catch {}
  }
}

let chans256 = 0, chans24 = 0;
await scenario('the switch ON (an RTL over rtl_tcp)', true, async ({ ws, S, log, st }) => {
  ok(st && st.ism256 === true, `vibeserver.json says ism256 true (${st && st.ism256})`);
  ok(S.hw && Math.abs(S.hw.rateNow - 2048000) < 1000, `before ISM: 2.048 MS/s (${S.hw && S.hw.rateNow})`);
  ws.send(JSON.stringify({ type: 'ism', on: 1, band: '433' }));
  ok(await until(() => !!S.ism && S.ism.band === '433', 6000), 'ISM on 433');
  ok(await until(() => S.hw && Math.abs(S.hw.rateNow - 2560000) < 1000, 6000), `in ISM: 2.56 MS/s (${S.hw && S.hw.rateNow})`);
  ok(/\[ISM\] rate 2\.048 -> 2\.560 MS\/s/.test(log()), 'the server log says the rate went to 2.56 for ISM');
  ok(await until(() => !!S.ism.lora && S.ism.lora.chans > 0, 6000), `LoRa channels run: ${JSON.stringify(S.ism.lora)}`);
  chans256 = S.ism.lora ? S.ism.lora.chans : 0;
  ws.send(JSON.stringify({ type: 'ism', on: 0 }));
  ok(await until(() => S.off, 5000), 'ISM off');
  ok(await until(() => S.hw && Math.abs(S.hw.rateNow - 2048000) < 1000, 6000), `after ISM: 2.048 MS/s again (${S.hw && S.hw.rateNow})`);
});
await scenario('the switch OFF (the default)', false, async ({ ws, S, log, cfgPath, st }) => {
  ok(st && st.ism256 === false, `vibeserver.json says ism256 false (${st && st.ism256})`);
  ws.send(JSON.stringify({ type: 'ism', on: 1, band: '433' }));
  ok(await until(() => !!S.ism && S.ism.band === '433', 6000), 'ISM on 433');
  await sleep(1500);
  ok(S.hw && Math.abs(S.hw.rateNow - 2048000) < 1000, `in ISM the rate is untouched: 2.048 MS/s (${S.hw && S.hw.rateNow})`);
  ok(!/\[ISM\] rate /.test(log()), 'no rate change in the log');
  ok(await until(() => !!S.ism.lora && S.ism.lora.chans > 0, 6000), `LoRa plan channels at 2.048: ${JSON.stringify(S.ism.lora)}`);
  chans24 = S.ism.lora ? S.ism.lora.chans : 0;
  ws.send(JSON.stringify({ type: 'ism', on: 0 }));
  await until(() => S.off, 5000);
  // ★ the live switch: an admin-gated message; a listener without the admin ticket is refused
  ws.send(JSON.stringify({ type: 'ism256', on: 1 }));
  await sleep(800);
  ok(!JSON.parse(fs.readFileSync(cfgPath, 'utf8')).radios[0].ism256, 'a listener without the admin ticket cannot switch it on');
});
ok(chans256 > chans24, `more 433 plan channels fit at 2.56 than at 2.048 (${chans256} vs ${chans24}) — MeshCom 433.175 among them`);

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
