// test-web-ais-aprs.mjs — the web client's AIS mode and APRS decoder against the REAL server (fake rtl_tcp replaying the
// `test-ais` / `test-aprs --write-clip` clips), in headless Edge with --mute-audio (tests are silent), at desktop and 390 px:
//   AIS: the demodulator menu's AIS entry, the full-screen ship map and its list, ships drawn on the map, a row → its card,
//        the list fitting a phone, EXIT AIS giving the dial back.
//   APRS: the DECODERS button, the station rows, MAP → the DIGI MODES MAP window on its APRS layer (beta90): symbols, the weather card.
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
  // ★ The browser profile (~0.5 GB) never outlives the run — a failed or timed-out run keeps its logs, not Edge's cache
  //   (53 kept waterfall-history runs held 25 GB of /var/folders, 2026-10-31).
  process.on('exit', () => { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
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
    await run({ ev, send, shot, until, dbg });
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
  // ★★★ beta96 AIS mode's map IS the AIR & SEA map, AIS live: the title, EXIT AIS and the SHIPS tab come from the mode
  ok(await until(`document.querySelector('.trkView:not([hidden]) .ttl')?.textContent === 'AVIATION & MARITIME \u00b7 AIS 161.975 + 162.025 MHz'`, 4000),
     `beta96: the AIR & SEA map, AIS live (${await ev(`document.querySelector('.trkView:not([hidden]) .ttl')?.textContent`)})`);
  ok(await ev(`document.querySelector('.trkView:not([hidden]) .exit')?.textContent`) === 'EXIT AIS', 'beta96: EXIT AIS (the map is the mode)');
  ok(await ev(`window.__vibeAirSea && window.__vibeAirSea().mode`) === 'ais', 'beta96: the current mode is AIS');
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
  ok(await ev(`!window.__vibeAirSea || !window.__vibeAirSea().open`), 'beta96: the map the mode opened went with the mode');
  await sleep(1500);
  const after = await ev(`document.getElementById('mFreq')?.textContent`);
  ok(/96\.100/.test(after || ''), `the dial is back: ${after}`);
});

console.log('── the APRS decoder in the web client ──');
await session('aprs', APRS, 144800000, 144800000, 'nfm', async ({ ev, send, shot, until, dbg }) => {
  ok(await ev(`(() => { const b = document.querySelector('#decodersPanel [data-dec="aprs"]'); return !!b && !b.hidden; })()`), 'the DECODERS panel offers APRS');
  await ev(`document.querySelector('#decodersPanel [data-dec="aprs"]').click()`);
  ok(await until(`document.getElementById('decTitle')?.textContent === 'APRS'`, 4000), 'the box is APRS');
  // ★ 11-24 (Stuart: "why is there a big empty box in the aprs decoder?"): the lists ARE the box — no empty text pane above them
  ok(await ev(`(() => { const t = document.getElementById('decText'); return t.classList.contains('off') && t.offsetHeight === 0; })()`), 'no empty text pane above the APRS lists');
  ok(await until(`document.querySelectorAll('#aprsRows .ar').length >= 4`, 40000), `stations listed: ${await ev(`[...document.querySelectorAll('#aprsRows .ar .c')].map(x => x.textContent).join(' | ')`)}`);
  ok(/See you at the club/.test(await ev(`document.getElementById('aprsMsgs')?.textContent`) || ''), 'the message is shown');
  ok(/G4ABC-9/.test(await ev(`document.getElementById('aprsLog')?.textContent`) || ''), 'the packet log');
  console.log('   screenshot', await shot('aprs-box'));
  // ★★★ beta90 APRS is a LAYER of the ONE DIGI MODES MAP (Stuart, 2026-11-21): MAP opens that map's window on its APRS
  //     layer — the stations with their symbols and cards, the weather station's report on its card
  const before = new Set((await fetch(`http://127.0.0.1:${dbg}/json/list`).then((r) => r.json())).map((t) => t.id));
  await send('Runtime.evaluate', { expression: `document.getElementById('aprsMapBtn').click()`, userGesture: true });
  let mapT = null;
  for (let i = 0; i < 40 && !mapT; i++) { await sleep(250); mapT = (await fetch(`http://127.0.0.1:${dbg}/json/list`).then((r) => r.json())).find((t) => t.type === 'page' && !before.has(t.id)); }
  ok(!!mapT, 'MAP opens the DIGI MODES MAP window');
  if (mapT) {
    const mws = new WebSocket(mapT.webSocketDebuggerUrl);
    let mid = 0; const mp = new Map();
    mws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && mp.has(m.id)) { mp.get(m.id)(m); mp.delete(m.id); } };
    await Promise.race([new Promise((r) => { mws.onopen = r; }), sleep(5000)]);
    const msend = (method, params = {}) => Promise.race([new Promise((res) => { const i = ++mid; mp.set(i, res); mws.send(JSON.stringify({ id: i, method, params })); }), sleep(10000).then(() => ({}))]);
    const mev = async (x) => (await msend('Runtime.evaluate', { expression: x, returnByValue: true })).result?.result?.value;
    for (let i = 0; i < 40; i++) { if (await mev(`typeof aprs !== 'undefined' && aprs.length >= 3`)) break; await sleep(250); }
    // ★★★ beta96 stage 7 THE AMATEUR MAP: every layer in the window shown, APRS (live: the box's decoder) emphasised and first
    ok(/Amateur Map/.test(await mev(`document.title`) || '') && /AMATEUR MAP/.test(await mev(`document.getElementById('amTitle').textContent`) || ''), `…titled the Amateur Map (${await mev(`document.title`)})`);
    ok(await mev(`show.aprs === true && show.spots === true && openedOn === 'aprs' && liveLayer === 'aprs'`), `opened on APRS (live), the spots shown too: ${await mev(`JSON.stringify({ show, openedOn, liveLayer })`)}`);
    ok((await mev(`aprs.length`)) >= 3, `stations on the map: ${await mev(`aprs.map(a => a.call).join(' ')`)}`);
    ok(await mev(`aprs.filter(a => a.svg).length >= 3`), 'each drawn with its APRS symbol (our SVG)');
    ok(/1012/.test(await mev(`(aprs.find(a => /MB7UH/.test(a.call)) || {}).wx || ''`) || ''), `the weather station's report: ${await mev(`(aprs.find(a => /MB7UH/.test(a.call)) || {}).wx`)}`);
    ok(/Weather: .*1012/.test(await mev(`aprsCard(aprs.find(a => /MB7UH/.test(a.call)))`) || ''), 'its card says it');
    ok(await mev(`(() => { const l = document.getElementById('layers'); const k = [...l.querySelectorAll('button')]; return !l.hidden && k[0].textContent === 'APRS' && k[0].classList.contains('live') && k.every(b => b.getAttribute('aria-pressed') === 'true'); })()`),
       `the layer keys, the live one first: ${await mev(`[...document.getElementById('layers').querySelectorAll('button')].map(b => b.textContent + '=' + b.getAttribute('aria-pressed') + (b.classList.contains('live') ? '(live)' : '')).join(',')`)}`);
    ok(/APRS \d/.test(await mev(`document.getElementById('amTabs').textContent`) || '') && /G4ABC|MB7UH/.test(await mev(`document.getElementById('amRows').textContent`) || ''),
       `the list's APRS tab: ${await mev(`document.getElementById('amTabs').textContent + ' | ' + document.getElementById('amRows').textContent.slice(0, 120)`)}`);
    /* ★★★ STUART'S RULE: ANY station in the HEARD IN window — a background-decoded and a listener-decoded spot (the store's
     *  src 1 and 2) both shown inside the window and gone outside it; APRS follows the SAME window */
    const W = await mev(`(() => {
      const t = Date.now(), out = {};
      stored.clear();
      for (const s of cleanStations([{ call: 'G4BGD', grid: 'IO91', mode: 'FT8', band: '20m', freq: 14074000, last: Math.round(t / 1000) - 1800, first: Math.round(t / 1000) - 1800, count: 1, src: 1, lat: 51.5, lon: -1.0 },
                                     { call: 'DL1LIS', grid: 'JO62', mode: 'CW', band: '40m', freq: 7020000, last: Math.round(t / 1000) - 2400, first: Math.round(t / 1000) - 2400, count: 1, src: 2, lat: 52.5, lon: 13.4 }])) stored.set(s.callsign, s);
      live = []; srvOk = true;
      aprs.forEach((a, i) => { a.age = 600 + i * 300; });
      win = '1h'; srvNow = Math.round(t / 1000); mergeSpots(); render();
      out.in = spots.map(s => s.callsign + ':' + s.src).sort().join(',') + ' | aprs ' + aprsIn().length + '/' + aprs.length + ' | rows ' + document.querySelectorAll('#amRows .amRow').length;
      srvNow = Math.round(t / 1000) + 7200; aprs.forEach((a) => { a.age += 7200; }); mergeSpots(); render();
      out.out = spots.length + ' | aprs ' + aprsIn().length;
      win = '24h'; mergeSpots(); render();
      out.day = spots.map(s => s.callsign).sort().join(',') + ' | aprs ' + aprsIn().length;
      return JSON.stringify(out);
    })()`);
    const Wj = JSON.parse(W || '{}');
    ok(/^DL1LIS:2,G4BGD:1 \| aprs (\d+)\/\1/.test(Wj.in || ''), `1 H: the background (src 1) and the listener (src 2) spot both shown, every APRS station too: ${Wj.in}`);
    ok(Wj.out === '0 | aprs 0', `two hours later in 1 H: both spots gone, and APRS with them: ${Wj.out}`);
    ok(/^DL1LIS,G4BGD \| aprs [1-9]/.test(Wj.day || ''), `24 H: all back: ${Wj.day}`);
    ok(/APRS/.test(await mev(`document.getElementById('legend').textContent`) || ''), 'the legend has APRS');
    const sc = await mev(`[...document.querySelectorAll('.maplibregl-ctrl-scale, .leaflet-control-scale-line')].map(e => e.textContent).join(' | ')`);
    ok(/km/.test(sc || ''), `a scale bar (${sc})`);
    const r = await msend('Page.captureScreenshot', { format: 'png' });
    if (r.result) { const f = path.join(os.tmpdir(), 'vibesdr-aisaprs-aprs-map.png'); fs.writeFileSync(f, Buffer.from(r.result.data, 'base64')); console.log('   screenshot', f); }
    mws.close();
    await fetch(`http://127.0.0.1:${dbg}/json/close/${mapT.id}`).catch(() => {});
  }
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1000);
  ok((await ev(`document.documentElement.scrollWidth`)) <= 390, 'no sideways scroll at 390 px');
  console.log('   screenshot', await shot('aprs-phone'));
});
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
