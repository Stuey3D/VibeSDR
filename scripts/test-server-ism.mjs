// test-server-ism.mjs — V12 ISM mode through the REAL server: a fake rtl_tcp replaying a synthetic 433 MHz clip
// (`test-ism --write-clip`: an Oregon THGR810, a WH1080, a TPMS twice, a fixed-code remote), a spectrum socket, and the
// contract of docs/v12/ISM-DECODER.md: the capability (ism / ismBands / ismLanding), entering by band, the device list
// with readable lines, the PRIVACY rule on the wire (no TPMS ID, no remote code), a band STEP, an exact frequency, and
// leaving with the receiver given back exactly as found. Silent: no audio socket is opened.
//   VIBESERVER_BIN=…/vibeserver VIBE_ISM_CLIP=…/clip.u8 node scripts/test-server-ism.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_ISM_CLIP || '';
// ★ V12 wM-Bus: a meter clip for the 868 band (`test-wmbus --write-clip clip868.u8 868600000 2400000`) — optional
const CLIP868 = process.env.VIBE_WMBUS_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_ISM_CLIP (make one: test-ism --write-clip clip.u8 434130000 2400000)');
  process.exit(3);
}
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 100; i++) { if (await f()) return true; await sleep(100); } return !!(await f()); };

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-ism-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, name: 'ISM test', locator: 'IO92nh', adminPass: 'ism-test-admin',
  radios: [{ serial: 'ism-test', label: 'Test radio', enabled: true, configured: true, users: 1 }],
}));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--ism', CLIP,
                                     ...(CLIP868 && fs.existsSync(CLIP868) ? ['--ism868', CLIP868] : [])], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const log = fs.openSync(path.join(dir, 'server.log'), 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', '1'],
                  { env, stdio: ['ignore', log, log] });
const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };

try {
  ok(await until(async () => !!(await status()), 15000), 'the server is up');
  const st = await status();
  ok(st.ism === true, `vibeserver.json: ism ${JSON.stringify(st.ism)} ${st.ismWhy || ''}`);
  const ids = (st.ismBands || []).map((b) => b.id);
  ok(JSON.stringify(ids) === JSON.stringify(['315', '433', '868', '915']), `ismBands (an R820T reaches all four): ${ids.join(' ')}`);
  ok(st.ismLanding === '433', `ismLanding for a UK locator: ${st.ismLanding}`);
  ok(Array.isArray(st.modes) && st.modes.includes('ism'), 'modes lists ism');

  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=ismtest01&bins=256&proto=1`);
  ws.binaryType = 'arraybuffer';
  const msgs = [];
  let last = null, lastCfg = null, off = false, err = null;
  ws.onmessage = (e) => {
    if (typeof e.data !== 'string') return;
    msgs.push(e.data);
    const j = JSON.parse(e.data);
    if (j.type === 'ism') last = j;
    if (j.type === 'ism_off') off = true;
    if (j.type === 'ism_error') err = j.why;
    if (j.type === 'config') lastCfg = j;
  };
  await new Promise((r) => { ws.onopen = r; });
  await sleep(1200);
  const before = lastCfg && { c: lastCfg.centerFreq, v: lastCfg.vfo, m: lastCfg.mode };

  ws.send(JSON.stringify({ type: 'ism', on: 1, band: '433' }));
  ok(await until(() => !!last && last.band === '433', 5000), `an ism report names the band: ${last && last.bandName}`);
  ok(lastCfg && Math.abs(lastCfg.vfo - 433.92e6) < 1, `the dial shows the band centre: vfo ${lastCfg && lastCfg.vfo}, radio ${lastCfg && lastCfg.centerFreq}`);
  const want = ['Oregon Scientific THGR810', 'Fine Offset WH1080', 'TPMS #1', 'Doorbell / remote #1'];
  const has = (s) => !!last && last.devices.some((d) => d.line.startsWith(s));
  ok(await until(() => want.every(has), 20000), `all four devices listed: ${last ? last.devices.map((d) => d.line).join(' | ') : '-'}`);
  const oregon = last.devices.find((d) => d.line.startsWith('Oregon'));
  ok(oregon && oregon.line === 'Oregon Scientific THGR810 / THGN801 · 18.4 °C · 62 % RH · battery OK · ch 2', `the Oregon line: ${oregon && oregon.line}`);
  ok(oregon && Math.abs(oregon.freq - 433.92e6) < 15e3, `its frequency: ${oregon && oregon.freq}`);
  const wire = msgs.join('\n');
  // ★ PRIVACY on the wire: the TPMS ID 11223344 and the remote's code A5C3E8 (20-bit address A5C3E) never appear, in any form
  for (const bad of ['11223344', '287454020', 'A5C3E', 'a5c3e', '678974', '10863592']) ok(!wire.includes(bad), `the wire never carries ${bad}`);
  const tp = last.devices.find((d) => d.kind === 'tpms');
  ok(tp && tp.nick === 1 && tp.id === undefined && tp.line === 'TPMS #1 · type B · 32 PSI · 18 °C', `the TPMS line: ${tp && tp.line}`);

  // a band STEP up: 868, the list kept
  ws.send(JSON.stringify({ type: 'ism', on: 1, step: 1 }));
  ok(await until(() => last.band === '868', 5000), `step +1 → ${last.band}`);
  ok(last.devices.length >= 4, `the list is kept across a band step (${last.devices.length})`);
  if (CLIP868 && fs.existsSync(CLIP868)) {
    // ★★ V12 wM-Bus METERS on the wire: the Sensus (encrypted) and the Kamstrup (readings), their serials, never a payload byte
    const meter = (id) => last.devices.find((d) => d.kind === 'meter' && d.id === id);
    ok(await until(() => !!meter('12345678') && !!meter('87654321'), 15000), `two meters listed: ${last.devices.filter((d) => d.kind === 'meter').map((d) => d.line).join(' | ')}`);
    const sen = meter('12345678'), kam = meter('87654321');
    ok(sen && sen.line.startsWith('Water meter · Sensus · 12345678 · v0x68 · encrypted (AES)') && / · signal \d+ dB$/.test(sen.line), `the Sensus line: ${sen && sen.line}`);
    ok(sen && sen.name === 'Water meter · Sensus · 12345678' && sen.wm === 'T1' && sen.sec === 5 && sen.encBlocks === 2, 'the Sensus fields');
    ok(kam && kam.line.startsWith('Cold water meter · Kamstrup · 87654321 · v0x1B · 123.456 m³ · flow 0.012 m³/h · 30 Sep 120.111 m³ · no alarms'), `the Kamstrup line: ${kam && kam.line}`);
    ok(kam && kam.wm === 'C1-A' && kam.vol === 123.456, 'the Kamstrup fields');
    ok(last.meters >= 2 && !last.wmGap, `the report counts meters (${last.meters}) and no 868.95 gap at 2.4 MS/s`);
    const w868 = msgs.join('\n');
    // ★★★ the encrypted payload (A0 A1 A2 …) in no form on the wire
    let leak = 0;
    for (let i = 0; i + 4 <= 32; i++) {
      const b = [0, 1, 2, 3].map((k) => 0xA0 + i + k);
      for (const f of [b.map((x) => x.toString(16).toUpperCase().padStart(2, '0')).join(''), b.map((x) => x.toString(16)).join(''), b.join(',')]) if (w868.includes(f)) leak++;
    }
    ok(leak === 0, `the encrypted payload never on the wire (${leak})`);
  } else console.log('   (meters not run — set VIBE_WMBUS_CLIP: test-wmbus --write-clip clip868.u8 868600000 2400000)');
  ws.send(JSON.stringify({ type: 'ism', on: 1, step: 1 }));
  ok(await until(() => last.band === '915', 5000), `step +1 → ${last.band}`);
  ws.send(JSON.stringify({ type: 'ism', on: 1, step: 1 }));
  await sleep(1500);
  ok(last.band === '915', 'clamped at the top (no wrap to 315)');
  ws.send(JSON.stringify({ type: 'ism', on: 1, band: '433' }));
  ok(await until(() => last.band === '433' && !last.custom, 5000), 'back to 433 by name');
  // an exact frequency (the frequency box): a `tune` in ISM mode
  ws.send(JSON.stringify({ type: 'tune', frequency: 433420000 }));
  ok(await until(() => last.custom === 1 && Math.abs(last.centre - 433.42e6) < 1, 5000), `a typed frequency: custom ${last.custom}, centre ${last.centre}`);
  // a zoom is held
  ws.send(JSON.stringify({ type: 'mode', mode: 'nfm' }));
  await sleep(600);
  ok(last && lastCfg.mode === before.m, 'a mode change is ignored inside ISM');
  // leave: the receiver exactly as found
  ws.send(JSON.stringify({ type: 'ism', on: 0 }));
  ok(await until(() => off, 5000), 'ism_off arrives');
  await sleep(1200);
  ok(lastCfg && lastCfg.vfo === before.v && lastCfg.mode === before.m, `the dial back: vfo ${lastCfg && lastCfg.vfo} (was ${before.v}), ${lastCfg && lastCfg.mode}`);
  ok(!err, `no ism_error (${err || 'none'})`);
  ws.close();
} finally {
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(500);
  if (fail) console.log(`   (server log: ${path.join(dir, 'server.log')})`);
  else fs.rmSync(dir, { recursive: true, force: true });
}
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
