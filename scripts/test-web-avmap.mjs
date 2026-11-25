// test-web-avmap.mjs — V12 2026-11-16 THE AVIATION MAP in the web client (docs/v12/AVIATION.md §The aviation map), against
// TWO REAL radio processes on one runtime directory: radio A in ADS-B (fake rtl_tcp, `test-adsb --write-clip`), radio B in
// the DATALINK mode on VDL2 (`test-vdl2 --write-clip`) — the page is radio B's, in headless Edge with --mute-audio (tests
// are silent), at desktop and 390 px. Checked: DATALINK's view IS the AVIATION MAP; 4CA97B ONCE in the list, "VDL2 + ADS-B";
// 400F11 (radio A's ADS-B only) under OTHER SOURCES and on the map; the layer keys (ADS-B, VDL2, ACARS, ADS-C, AIRPORTS —
// no HFDL: nothing heard there); ADS-B off hides 400F11 and keeps 4CA97B; AIRPORTS off switches the basemap to
// 'aero-plain'; the card says which sources heard it; the KEY; never a message body; the HFDL decoder's MAP opens the map
// standalone (CLOSE MAP). Needs the page BUILT INTO the server (node scripts/build-web.mjs, then the server rebuilt).
//   VIBESERVER_BIN=…/vibeserver VIBE_VDL2_CLIP=…/v.cu8 VIBE_ADSB_SYN_CLIP=…/a.u8 node scripts/test-web-avmap.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_VDL2_CLIP || '', ADSB = process.env.VIBE_ADSB_SYN_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP) || !ADSB || !fs.existsSync(ADSB) || !fs.existsSync(EDGE)) { console.log('   not run — VIBESERVER_BIN, VIBE_VDL2_CLIP, VIBE_ADSB_SYN_CLIP and Edge needed'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

// ── radio A: ADS-B, driven over its socket (a second listener's radio on the same server) ─────────────────────────────
const RUN = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-avmap-run-'));
process.on('exit', () => { try { fs.rmSync(RUN, { recursive: true, force: true }); } catch {} });
async function radioA() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-avmapA-'));
  fs.mkdirSync(path.join(dir, 'data'));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Aviation map web test A', locator: 'IO92nh', adminPass: 'avmap-web-admin',
    radios: [{ serial: 'avmapA-web', label: 'ADS-B radio', enabled: true, configured: true, users: 2 }] }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--adsb', ADSB], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: RUN };
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', '2', '--idle-grace', '0'],
                    { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });
  for (let i = 0; i < 60; i++) { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`); if (r.ok) break; } catch {} await sleep(250); }
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  s.on('error', () => {});
  await new Promise((r) => s.on('connect', r));
  s.write(`GET /ws/user-spectrum?user_session_id=avmapwebA&bins=256&proto=1 HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`);
  s.on('data', () => {});
  const send = (o) => { const b = Buffer.from(JSON.stringify(o)); const h = Buffer.alloc(6); h[0] = 0x81; h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); s.write(Buffer.concat([h, b])); };
  await sleep(1500);
  send({ type: 'adsb', on: 1 });
  const keep = setInterval(() => { try { send({ type: 'ping' }); } catch {} }, 10000);   // ★ 30 s silent drop
  return { adsb: (on) => send({ type: 'adsb', on: on ? 1 : 0 }), stop: () => { clearInterval(keep); try { s.end(); } catch {} srv.kill('SIGTERM'); rtl.kill('SIGTERM'); try { fs.rmSync(dir, { recursive: true, force: true }); } catch {} } };
}

async function session(tag, clip, centre, freq, mode, run, runDir) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-web${tag}-`));
  // ★ The browser profile (~0.5 GB) never outlives the run — a failed or timed-out run keeps its logs, not Edge's cache
  //   (53 kept waterfall-history runs held 25 GB of /var/folders, 2026-10-31).
  process.on('exit', () => { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
  for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Datalink web test', locator: 'IO90bs', adminPass: 'avdata-web-admin',
    radios: [{ serial: `${tag}-web`, label: 'Test radio', enabled: true, configured: true, users: 1 }] }));
  const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--hf', clip, '--hf-centre', String(centre)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: runDir || path.join(dir, 'run') };
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
    const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(os.tmpdir(), `vibesdr-avmap-${name}.png`); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
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

console.log('── the AVIATION MAP in the web client (two radios, one table) ──');
const A = await radioA();
try {
  await session('avm', CLIP, 136850000, 96100000, 'wfm', async ({ ev, send, shot, until }) => {
    const V = `document.querySelector('.trkView:not([hidden])')`;
    await ev(`document.getElementById('mMode')?.click()`);
    await sleep(700);
    await ev(`(() => { const b = [...document.querySelectorAll('.mModeOpt')].find(x => x.textContent.trim() === 'DATALINK'); b && b.click(); return !!b; })()`);
    ok(await until(`!!${V}`, 6000), 'DATALINK opens its view');
    ok(/AVIATION MAP · DATALINK/.test(await ev(`${V}?.querySelector('.ttl')?.textContent`) || ''), `…the AVIATION MAP (${await ev(`${V}?.querySelector('.ttl')?.textContent`)})`);
    await until(`[...${V}.querySelectorAll('button')].some(b => b.textContent.trim() === 'VDL2' && b.dataset.rng)`, 8000);
    await ev(`[...${V}.querySelectorAll('[data-rng]')].find(b => b.textContent.trim() === 'VDL2')?.click()`);
    const rowsTxt = () => ev(`${V}?.querySelector('.rows')?.textContent || ''`);
    ok(await until(`/BAW117/.test(${V}?.querySelector('.rows')?.textContent || '') && /VDL2 \\+ ADS-B|ADS-B \\+ VDL2/.test(${V}?.querySelector('.rows')?.textContent || '')`, 45000),
       `radio A's ADS-B joins radio B's datalink: ${(await rowsTxt()).slice(0, 300)}`);
    const n4 = await ev(`[...${V}.querySelectorAll('.trkRow')].filter(r => r.dataset.key === 'a:4CA97B').length`);
    ok(n4 === 1, `4CA97B is ONE row (${n4})`);
    ok(await ev(`[...${V}.querySelectorAll('.sec')].some(s => /OTHER SOURCES/.test(s.textContent))`), 'a section for the other sources on this server');
    ok(await ev(`(() => { const secs = [...${V}.querySelectorAll('.rows > *')]; const i = secs.findIndex(e => e.classList.contains('sec') && /OTHER/.test(e.textContent)); return i >= 0 && secs.slice(i).some(e => e.dataset && e.dataset.key === 'a:400F11'); })()`),
       '400F11 (ADS-B only, radio A) is under OTHER SOURCES');
    await until(`[...${V}.querySelectorAll('[data-tgl]')].map(b => b.textContent.trim()).join(',') === 'VDL2,ACARS,ADS-B,ADS-C,AIRPORTS'`, 15000);
    const tg = await ev(`[...${V}.querySelectorAll('[data-tgl]')].map(b => b.textContent.trim()).join(',')`);
    ok(/^VDL2,ACARS,ADS-B,ADS-C,AIRPORTS$/.test(tg || ''), `the layer keys: ${tg} (no HFDL: nothing heard there)`);
    ok(await ev(`[...${V}.querySelectorAll('[data-tgl]')].every(b => b.getAttribute('aria-pressed') === 'true')`), 'every layer on by default (AIRPORTS too)');
    // ★★ beta90 the aircraft are the ADS-B layer's (adsblayer.js): its controls, its rings, every aircraft drawn by it
    const AC = `(window.__vibeAcLayer ? window.__vibeAcLayer.shown().length : -1)`;
    ok(await until(`${AC} >= 2`, 15000), `beta90: aircraft drawn by the ADS-B layer: ${await ev(AC)}`);
    // ★★ beta91 the ADS-B aircraft data on every source (vibe_aircraft_info.h): the VDL2-only 3C6548 carries its country's
    //    flag; 4CA97B's row names its operator (from its flight — the starter tables, no download in a test)
    const rowTxt = (k) => `[...${V}.querySelectorAll('.trkRow')].find(r => r.dataset.key === '${k}')?.textContent || ''`;
    ok(await until(`/\u{1F1E9}\u{1F1EA}|\bDE\b/u.test(${rowTxt('a:3C6548')})`, 15000), `beta91: VDL2-only 3C6548's row has Germany's flag (${await ev(rowTxt('a:3C6548'))})`);
    ok(await until(`/Ryanair/.test(${rowTxt('a:4CA97B')})`, 15000), `beta91: 4CA97B's row names its operator (${await ev(rowTxt('a:4CA97B'))})`);
    ok(await until(`/Mictronics/.test(${V}?.querySelector('.rows .avCredit')?.textContent || '') && !!${V}?.querySelector('.rows .avCredit a[href="https://opendatacommons.org/licenses/by/1-0/"]')`, 6000),
       `beta91: the ODC-By notice at the list's foot (${await ev(`${V}?.querySelector('.rows .avCredit')?.textContent`)})`);
    ok(await ev(`[...document.querySelectorAll('.trkView:not([hidden]) .vsadsb-ctl button')].map(b => b.textContent).join(',')`) === 'AUTO,FIT,RINGS,RANGE,LABELS,⌖',   // ★ beta94 + RANGE: the ADS-B layer's range outline key (b94-accard) is the aviation map's too
       `beta90: the ADS-B map's controls on the aviation map (${await ev(`[...document.querySelectorAll('.trkView:not([hidden]) .vsadsb-ctl button')].map(b => b.textContent).join(',')`)})`);
    ok(await ev(`document.querySelectorAll('.trkView:not([hidden]) .vtl-mk[data-key^="a:"]').length === 0`), 'beta90: no aircraft drawn twice (the track layer has only the stations)');
    const scale = await ev(`[...document.querySelectorAll('.trkView:not([hidden]) .maplibregl-ctrl-scale, .trkView:not([hidden]) .leaflet-control-scale-line')].map(e => e.textContent).join(' | ')`);
    ok(/km/.test(scale || '') && /nm|NM/.test(scale || ''), `beta90: the scale bar, km and NM (${scale})`);
    // ★★ beta90 a datalink aircraft (no track, no speed): two fixes → it faces the bearing between them and GLIDES there
    const glide = JSON.parse(await ev(`(() => { const L = window.__vibeAcLayer; const a = { hex: '~abc123', flight: 'TST1', lat: 51.0, lon: -1.0, alt_baro: 35000, sparse: true, seen_pos: 30 };
      L.update([a]); L.update([Object.assign({}, a, { lat: 51.0, lon: 0.0 })]);
      const s = L.shown().find(x => x.hex === '~abc123'); return JSON.stringify(s || null); })()`) || 'null');
    ok(glide && Math.abs(glide.track - 90) < 2 && glide.gliding && glide.sparse, `beta90: a sparse aircraft faces east (${glide && glide.track?.toFixed(1)}°) and glides to its new fix (${glide && glide.gliding})`);
    // ★ beta88 the status line counts NOW: radio A's ADS-B is decoding, its three aircraft heard within the minute
    ok(await until(`/\\bADS-B 3\\b/.test(${V}?.querySelector('.st')?.textContent || '') && !/ADS-B:/.test(${V}?.querySelector('.st')?.textContent || '')`, 10000),
       `beta88: the status line counts ADS-B now (${await ev(`${V}?.querySelector('.st')?.textContent`)})`);
    await sleep(4000);   // ★ the next poll gives the layer the real list back (the test aircraft fades)
    const mk0 = await ev(AC);
    ok(!/TEST MESSAGE BODY|EGLLXYA|#M1BPOS/.test(await ev(`document.body.innerHTML`) || ''), '★ never a message body on the page');
    // the card: which sources heard it
    await ev(`[...${V}.querySelectorAll('.trkRow')].find(r => r.dataset.key === 'a:4CA97B')?.click()`);
    const CARD = `document.querySelector('.vsadsb-pop-dark')`;
    ok(await until(`/Heard on/.test(${CARD}?.textContent || '') && /ADS-B/.test(${CARD}?.textContent || '') && /VDL2/.test(${CARD}?.textContent || '')`, 6000),
       `the card (the ADS-B layer's) names both sources: ${(await ev(`${CARD}?.textContent`) || '').slice(0, 200)}`);
    // ADS-B off: 400F11 goes, 4CA97B stays
    await ev(`[...${V}.querySelectorAll('[data-tgl]')].find(b => b.textContent.trim() === 'ADS-B').click()`);
    ok(await until(`!/BAW117/.test(${V}?.querySelector('.rows')?.textContent || '')`, 6000), 'ADS-B off: 400F11 leaves the list');
    ok(/FR1234/.test(await rowsTxt()), '…4CA97B stays (VDL2 heard it too)');
    ok(await until(`${AC} < ${mk0}`, 6000), `…and the map (${mk0} → ${await ev(AC)})`);
    await ev(`[...${V}.querySelectorAll('[data-tgl]')].find(b => b.textContent.trim() === 'ADS-B').click()`);
    // AIRPORTS off: the basemap profile
    ok(await ev(`(window.__vibeTrkProfile || 'aero') === 'aero'`), `AIRPORTS on: the 'aero' basemap (${await ev(`window.__vibeTrkProfile`)})`);
    await ev(`[...${V}.querySelectorAll('[data-tgl]')].find(b => b.textContent.trim() === 'AIRPORTS').click()`);
    ok(await until(`window.__vibeTrkProfile === 'aero-plain'`, 4000), `AIRPORTS off: 'aero-plain' (${await ev(`window.__vibeTrkProfile`)})`);
    ok(await until(`(() => { try { return JSON.parse(localStorage.getItem('vibesdr.avMapLayers')).airports === false; } catch { return false; } })()`, 2000), 'remembered');
    await ev(`[...${V}.querySelectorAll('[data-tgl]')].find(b => b.textContent.trim() === 'AIRPORTS').click()`);
    ok(await until(`window.__vibeTrkProfile === 'aero'`, 4000), 'AIRPORTS back on: aero');
    const key = await ev(`${V}?.querySelector('.trkKey')?.textContent || ''`);
    ok(/last two fixes/.test(key) && /Airports and runways/.test(key) && /datalink/.test(key), `the KEY (the ADS-B layer's tones): ${key.slice(0, 160)}`);
    console.log('   screenshot', await shot('desktop'));
    await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
    await sleep(1200);
    const sw = await ev(`document.documentElement.scrollWidth`);
    ok(sw <= 390, `no sideways scroll at 390 px (${sw})`);
    console.log('   screenshot', await shot('phone'));
    await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
    await sleep(600);
    await ev(`${V}.querySelector('.exit').click()`);
    ok(await until(`![...document.querySelectorAll('.trkView')].some(v => !v.hidden)`, 6000), 'EXIT DATALINK closes the view');
    await sleep(1500);
    // the HFDL decoder box's MAP: the same map, standalone, HFDL first
    await ev(`document.querySelector('#decodersPanel [data-dec="hfdl"]')?.click()`);
    ok(await until(`getComputedStyle(document.getElementById('hfdlMapBtn')).display !== 'none'`, 4000), 'the HFDL box has MAP');
    await ev(`document.getElementById('hfdlMapBtn').click()`);
    ok(await until(`!!${V} && /^AVIATION MAP$/.test(${V}.querySelector('.ttl')?.textContent || '')`, 5000), 'MAP opens the AVIATION MAP (standalone)');
    const tg2 = await ev(`[...${V}.querySelectorAll('[data-tgl]')].map(b => b.textContent.trim()).join(',')`);
    ok(/^HFDL,HFDL STATIONS,/.test(tg2 || '') && /ADS-B/.test(tg2 || ''), `HFDL first (what it was opened for), the others after: ${tg2}`);
    ok(await until(`/BAW117/.test(${V}?.querySelector('.rows')?.textContent || '')`, 8000), 'the other radios\' aircraft are on it');
    // ★★ beta88 Stuart's case (the Pi 500 in ACARS mode on the V4L that normally does ADS-B: "there should be 0 then"):
    //    radio A leaves ADS-B. Within kAdsbLiveSec (10 s) + a poll, no radio is decoding ADS-B: the ADS-B-only aircraft
    //    leave the map, 4CA97B stays for its VDL2 with its ADS-B position "last seen", and the status line says why.
    A.adsb(false);
    ok(await until(`/ADS-B: this server's ADS-B radio is in use for something else/.test(${V}?.querySelector('.st')?.textContent || '')`, 30000),
       `beta88: radio A left ADS-B — the status line says so (${await ev(`${V}?.querySelector('.st')?.textContent`)})`);
    ok(await until(`!/BAW117/.test(${V}?.querySelector('.rows')?.textContent || '')`, 6000), '…ADS-B-only BAW117 is off the list (0 ADS-B, not the hour\'s)');
    ok(!/\bADS-B \d/.test(await ev(`${V}?.querySelector('.st')?.textContent`) || ''), '…and no ADS-B count in the status line');
    ok(/FR1234|EI-EST|4CA97B/.test(await ev(`${V}?.querySelector('.rows')?.textContent || ''`) || ''), '…4CA97B stays, for its VDL2');
    ok(/ADS-B last seen \d\d:\d\d UTC/.test(await ev(`[...${V}.querySelectorAll('.trkRow')].find(r => r.dataset.key === 'a:4CA97B')?.textContent || ''`) || ''),
       `…its ADS-B position is "last seen HH:MM UTC" (${(await ev(`[...${V}.querySelectorAll('.trkRow')].find(r => r.dataset.key === 'a:4CA97B')?.textContent || ''`) || '').slice(0, 140)})`);
    await sleep(800);
    console.log('   screenshot', await shot('adsb-in-use'));
    ok(/CLOSE MAP/.test(await ev(`${V}.querySelector('.exit').textContent`) || ''), 'CLOSE MAP');
    await ev(`${V}.querySelector('.exit').click()`);
    ok(await until(`![...document.querySelectorAll('.trkView')].some(v => !v.hidden)`, 4000), 'CLOSE MAP closes it');
  }, RUN);
} finally { A.stop(); }
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
