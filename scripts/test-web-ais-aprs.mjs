// test-web-ais-aprs.mjs — the web client's AIS mode and APRS decoder against the REAL server (fake rtl_tcp replaying the
// `test-ais` / `test-aprs --write-clip` clips), in headless Edge with --mute-audio (tests are silent), at desktop and 390 px:
//   AIS: the demodulator menu's AIS entry, the full-screen ship map and its list, ships drawn on the map, a row → its card,
//        the list fitting a phone, EXIT AIS giving the dial back.
//   APRS: the DECODERS button, the station rows, MAP → the station map with its markers, a station's card.
// Screenshots to $TMPDIR/vibesdr-aisaprs-*.png.
//   VIBESERVER_BIN=…/vibeserver VIBE_AIS_CLIP=…/a.cu8 VIBE_APRS_CLIP=…/b.cu8 node scripts/test-web-ais-aprs.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '', AIS = process.env.VIBE_AIS_CLIP || '', APRS = process.env.VIBE_APRS_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !AIS || !fs.existsSync(AIS) || !APRS || !fs.existsSync(APRS) || !fs.existsSync(EDGE)) { console.log('   not run — VIBESERVER_BIN, VIBE_AIS_CLIP, VIBE_APRS_CLIP and Edge needed'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });


async function session(tag, clip, centre, freq, mode, run) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-web${tag}-`));
  for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'AIS/APRS web test', locator: 'IO90bs', adminPass: 'aisaprs-web-admin',
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
    const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(os.tmpdir(), `vibesdr-aisaprs-${name}.png`); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
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

console.log('── AIS mode in the web client ──');
await session('ais', AIS, 162000000, 96100000, 'wfm', async ({ ev, send, shot, until }) => {
  await ev(`document.getElementById('mMode')?.click()`);
  await sleep(700);
  const opts = await ev(`[...document.querySelectorAll('.mModeOpt')].map(b => b.textContent.trim()).join(',')`);
  ok(/\bAIS\b/.test(opts || ''), `the demodulator menu offers AIS: ${opts}`);
  await ev(`(() => { const b = [...document.querySelectorAll('.mModeOpt')].find(x => x.textContent.trim() === 'AIS'); b && b.click(); return !!b; })()`);
  ok(await until(`[...document.querySelectorAll('.trkView')].some(v => !v.hidden)`, 6000), 'the ship map opens');
  ok(await until(`document.querySelectorAll('.trkView:not([hidden]) .trkRow').length >= 5`, 30000), `ships listed: ${await ev(`[...document.querySelectorAll('.trkView:not([hidden]) .trkRow b')].map(x => x.textContent).join(' | ')`)}`);
  ok(/TEST SHIP/.test(await ev(`document.querySelector('.trkView:not([hidden]) .rows')?.textContent`) || ''), 'the rows name the ships');
  ok(await until(`document.querySelectorAll('.trkView:not([hidden]) .vtl-mk').length >= 5`, 20000), `ships on the map: ${await ev(`document.querySelectorAll('.trkView:not([hidden]) .vtl-mk').length`)} markers`);
  await ev(`[...document.querySelectorAll('.trkView:not([hidden]) .trkRow')].find(r => /TEST SHIP 0/.test(r.textContent))?.click()`);
  ok(await until(`(() => { const c = document.querySelector('.trkView:not([hidden]) .vtl-card'); return c && !c.hidden && /MMSI/.test(c.textContent) && /SOUTHAMPTON/.test(c.textContent); })()`, 6000),
     `a row opens its card: ${await ev(`document.querySelector('.trkView:not([hidden]) .vtl-card')?.textContent?.slice(0, 160)`)}`);
  ok(await ev(`!!document.querySelector('.trkView:not([hidden]) .trkRow.sel')`), 'the row is marked');
  console.log('   screenshot', await shot('ais'));
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1200);
  const r = JSON.parse(await ev(`JSON.stringify(document.querySelector('.trkView:not([hidden]) .trkList').getBoundingClientRect())`));
  ok(r.left >= 5 && r.right <= 390 - 5 && r.bottom <= 844, `the list fits a phone: ${Math.round(r.left)}–${Math.round(r.right)}, bottom ${Math.round(r.bottom)}`);
  const sw = await ev(`document.documentElement.scrollWidth`);
  ok(sw <= 390, `no sideways scroll at 390 px (${sw})`);
  console.log('   screenshot', await shot('ais-phone'));
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  await sleep(600);
  await ev(`document.querySelector('.trkView:not([hidden]) .exit').click()`);
  ok(await until(`![...document.querySelectorAll('.trkView')].some(v => !v.hidden)`, 6000), 'EXIT AIS closes the map');
  await sleep(1500);
  const after = await ev(`document.getElementById('mFreq')?.textContent`);
  ok(/96\.100/.test(after || ''), `the dial is back: ${after}`);
});

console.log('── the APRS decoder in the web client ──');
await session('aprs', APRS, 144800000, 144800000, 'nfm', async ({ ev, send, shot, until }) => {
  ok(await ev(`(() => { const b = document.querySelector('#decodersPanel [data-dec="aprs"]'); return !!b && !b.hidden; })()`), 'the DECODERS panel offers APRS');
  await ev(`document.querySelector('#decodersPanel [data-dec="aprs"]').click()`);
  ok(await until(`document.getElementById('decTitle')?.textContent === 'APRS'`, 4000), 'the box is APRS');
  ok(await until(`document.querySelectorAll('#aprsRows .ar').length >= 4`, 40000), `stations listed: ${await ev(`[...document.querySelectorAll('#aprsRows .ar .c')].map(x => x.textContent).join(' | ')`)}`);
  ok(/See you at the club/.test(await ev(`document.getElementById('aprsMsgs')?.textContent`) || ''), 'the message is shown');
  ok(/G4ABC-9/.test(await ev(`document.getElementById('aprsLog')?.textContent`) || ''), 'the packet log');
  console.log('   screenshot', await shot('aprs-box'));
  await ev(`document.getElementById('aprsMapBtn').click()`);
  ok(await until(`document.querySelectorAll('.trkView:not([hidden]) .vtl-mk').length >= 3`, 20000), `stations on the map: ${await ev(`document.querySelectorAll('.trkView:not([hidden]) .vtl-mk').length`)}`);
  await ev(`[...document.querySelectorAll('.trkView:not([hidden]) .trkRow')].find(r => /MB7UH/.test(r.textContent))?.click()`);
  ok(await until(`(() => { const c = document.querySelector('.trkView:not([hidden]) .vtl-card'); return c && !c.hidden && /Weather/.test(c.textContent) && /1012/.test(c.textContent); })()`, 6000),
     `the weather station's card: ${await ev(`document.querySelector('.trkView:not([hidden]) .vtl-card')?.textContent?.slice(0, 160)`)}`);
  console.log('   screenshot', await shot('aprs-map'));
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1000);
  ok((await ev(`document.documentElement.scrollWidth`)) <= 390, 'no sideways scroll at 390 px');
  console.log('   screenshot', await shot('aprs-phone'));
  await ev(`document.querySelector('.trkView:not([hidden]) .exit').click()`);
  ok(await until(`![...document.querySelectorAll('.trkView')].some(v => !v.hidden)`, 4000), 'CLOSE MAP goes back to the box');
});
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
