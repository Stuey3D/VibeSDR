// test-web-packet9600.mjs — the web client's packet decoder (AUTO / 1200 / 9600 / 300, FX.25 + IL2P, the FRAMES log)
// against the REAL server (fake rtl_tcp replaying `test-ax25hs --write-clip`: 9600 bursts on 435 MHz — plain AX.25, an
// FX.25-only GRBAlpha telemetry frame, IL2P), in headless Edge with --mute-audio (tests are silent), at desktop and 390 px:
// the choices drawn from the capability (AUTO first and on), the FEC switches only under 1200 / 9600, no TUNE in 9600,
// FRAMES listing the three receivers' frames with the satellite named and the hex, the status line's FX.25 / IL2P counts.
// docs/v12/AX25-9600-FX25-IL2P.md. Screenshots to $TMPDIR/vibesdr-pkt9600-*.png.
//   VIBESERVER_BIN=…/vibeserver VIBE_PKT9600_CLIP=…/p.cu8 node --max-old-space-size=512 scripts/test-web-packet9600.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_PKT9600_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP) || !fs.existsSync(EDGE)) { console.log('   not run — VIBESERVER_BIN, VIBE_PKT9600_CLIP and Edge needed'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

async function session(tag, clip, centre, freq, mode, run) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-web${tag}-`));
  // ★ The browser profile (~0.5 GB) never outlives the run — a failed or timed-out run keeps its logs, not Edge's cache
  //   (53 kept waterfall-history runs held 25 GB of /var/folders, 2026-10-31).
  process.on('exit', () => { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
  for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Packet 9600 web test', locator: 'IO90bs', adminPass: 'pkt9600-web-admin',
    radios: [{ serial: `${tag}-web`, label: 'Test radio', enabled: true, configured: true, users: 1 }] }));
  const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--hf', clip, '--hf-centre', String(centre)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', String(freq), '--mode', mode, '--users', '1'],
                    { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });
  const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
    `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
  let wsC; const failBefore = fail;
  try {
    await sleep(3000);
    const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
    wsC = new WebSocket(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
    let id = 0; const pend = new Map();
    wsC.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
    await new Promise((r) => { wsC.onopen = r; });
    const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); wsC.send(JSON.stringify({ id: i, method, params })); });
    const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.text; };
    const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(os.tmpdir(), `vibesdr-pkt9600-${name}.png`); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
    const until = async (x, ms) => { for (let i = 0; i < ms / 250; i++) { if (await ev(x)) return true; await sleep(250); } return false; };
    await send('Page.enable'); await send('Runtime.enable');
    await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
    await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
    await until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 15000);
    await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim())); b && b.click(); return !!b; })()`);
    await until(`!!document.querySelector('#audioGate button')`, 15000);
    {
      const r = JSON.parse(await ev(`JSON.stringify(document.querySelector('#audioGate button').getBoundingClientRect())`));
      const x = r.x + r.width / 2, y = r.y + r.height / 2;
      await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
      await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
    }
    ok(await until(`!document.getElementById('audioGate') || document.getElementById('audioGate').style.display === 'none' || !document.getElementById('audioGate').isConnected`, 8000), `${tag}: START RADIO pressed`);
    await sleep(2500);
    await run({ ev, send, shot, until });
  } catch (e) { ok(false, `${tag} driver: ${e}`); }
  finally {
    try { wsC?.close(); } catch {}
    edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM');
    await sleep(600);
    if (fail === failBefore) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
  }
}

console.log('── the packet decoder in the web client ──');
await session('pkt', CLIP, 435000000, 435000000, 'nfm', async ({ ev, send, shot, until }) => {
  ok(await ev(`(() => { const b = document.querySelector('#decodersPanel [data-dec="aprs"]'); return !!b && !b.hidden; })()`), 'the DECODERS panel offers APRS');
  await ev(`document.querySelector('#decodersPanel [data-dec="aprs"]').click()`);
  ok(await until(`document.getElementById('decTitle')?.textContent === 'APRS'`, 4000), 'the box is APRS');
  const segs = await ev(`[...document.querySelectorAll('#aprsBaud .btn')].map(b => b.textContent + (b.classList.contains('on') ? '*' : '') + (b.classList.contains('unreach') ? '!' : '')).join(',')`);
  ok(segs === 'AUTO*,VHF 1200,9600,HF 300', `the choices from the capability, AUTO on: ${segs}`);
  ok(await ev(`document.getElementById('aprsFecRow').hidden === true && document.getElementById('aprsTuneRow').hidden === false`), 'AUTO: no FEC switches (all on), TUNE shown');
  ok(await until(`/OM9GRB/.test(document.getElementById('aprsFrames')?.textContent || '') && /G4ILP/.test(document.getElementById('aprsFrames')?.textContent || '') && /G4ABC-9/.test(document.getElementById('aprsFrames')?.textContent || '')`, 40000),
     `FRAMES lists the three receivers' frames: ${(await ev(`document.getElementById('aprsFrames')?.textContent`) || '').slice(0, 200)}`);
  const fr = await ev(`document.getElementById('aprsFrames')?.innerHTML || ''`);
  ok(/<span class="s">GRBAlpha<\/span>/.test(fr) && /FX\.25 · \d+ fixed/.test(fr) && /IL2P · CRC/.test(fr), 'GRBAlpha named in colour, FX.25 with its fixes, IL2P with its CRC');
  ok(/class="x">[0-9a-f]{8} [0-9a-f]{8}/.test(fr), 'the telemetry hex, grouped');
  ok(await until(`document.querySelectorAll('#aprsRows .ar').length >= 2`, 10000), `the APRS stations listed: ${await ev(`[...document.querySelectorAll('#aprsRows .ar .c')].map(x => x.textContent).join(' | ')`)}`);
  ok(!/OM9GRB/.test(await ev(`document.getElementById('aprsRows')?.textContent`) || ''), 'the binary telemetry is not a station');
  console.log('   screenshot', await shot('box'));
  // 9600 manual: the FEC switches appear, TUNE goes (no one channel); FX.25 off re-attaches
  await ev(`[...document.querySelectorAll('#aprsBaud .btn')].find(b => b.textContent === '9600').click()`);
  ok(await until(`document.getElementById('decTitle')?.textContent === 'APRS · 9600'`, 3000), 'the box says APRS · 9600');
  ok(await ev(`document.getElementById('aprsFecRow').hidden === false && document.getElementById('aprsTuneRow').hidden === true`), '9600: FX.25 / IL2P switches shown, no TUNE');
  await ev(`document.getElementById('aprsFx25').click()`);
  ok(await ev(`!document.getElementById('aprsFx25').classList.contains('on') && document.getElementById('aprsIl2p').classList.contains('on')`), 'FX.25 off, IL2P on');
  ok(await until(`/G4ILP/.test(document.getElementById('aprsFrames')?.textContent || '')`, 20000), 'frames come again after the switch');
  await ev(`document.getElementById('aprsFx25').click()`);
  await ev(`[...document.querySelectorAll('#aprsBaud .btn')].find(b => b.textContent === 'HF 300').click()`);
  ok(await ev(`document.getElementById('aprsFecRow').hidden === true && /10\.1476/.test(document.getElementById('aprsTune').textContent)`), 'HF 300: no FEC switches, TUNE 10.1476 USB');
  await ev(`[...document.querySelectorAll('#aprsBaud .btn')].find(b => b.textContent === 'AUTO').click()`);
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1200);
  ok((await ev(`document.documentElement.scrollWidth`)) <= 390, 'no sideways scroll at 390 px');
  console.log('   screenshot', await shot('phone'));
});
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
