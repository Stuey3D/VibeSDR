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

console.log('── the AIR & SEA MAP (beta96, docs/v12/ONE-MAP.md) in the web client (two radios, one table) ──');
const A = await radioA();
try {
  await session('avm', CLIP, 136850000, 96100000, 'wfm', async ({ ev, send, shot, until }) => {
    const V = `document.querySelector('.trkView:not([hidden])')`;
    await ev(`document.getElementById('mMode')?.click()`);
    await sleep(700);
    await ev(`(() => { const b = [...document.querySelectorAll('.mModeOpt')].find(x => x.textContent.trim() === 'DATALINK'); b && b.click(); return !!b; })()`);
    ok(await until(`!!${V}`, 6000), 'DATALINK opens its view');
    ok(/^AVIATION & MARITIME · DATALINK$/.test(await ev(`${V}?.querySelector('.ttl')?.textContent`) || ''), `…the AIR & SEA map, DATALINK live (${await ev(`${V}?.querySelector('.ttl')?.textContent`)})`);
    ok(await ev(`${V}.querySelector('.exit').textContent`) === 'EXIT DATALINK', 'EXIT DATALINK (the map is the mode)');
    await until(`[...${V}.querySelectorAll('button')].some(b => b.textContent.trim() === 'VDL2' && b.dataset.rng)`, 8000);
    await ev(`[...${V}.querySelectorAll('[data-rng]')].find(b => b.textContent.trim() === 'VDL2')?.click()`);
    // ★★ beta96 DATALINK opens on MESSAGES (the feed, newest first) with the aircraft sort LATEST
    ok(await until(`${V}?.querySelector('.trkList .hd .tab[aria-selected="true"]')?.dataset.tab === 'messages'`, 4000), 'DATALINK opens on the MESSAGES tab');
    ok(await until(`[...${V}.querySelectorAll('.trkRow.trkMsg')].length >= 3`, 45000), `MESSAGES: the feed (${await ev(`${V}.querySelectorAll('.trkRow.trkMsg').length`)} messages)`);
    { const ts = JSON.parse(await ev(`JSON.stringify([...${V}.querySelectorAll('.trkRow.trkMsg .ag')].map(e => e.textContent))`) || '[]');
      ok(ts.every((t, i) => i === 0 || t <= ts[i - 1]), `MESSAGES newest first (${ts.slice(0, 5).join(' ')})`); }
    ok(!(await ev(`/TEST MESSAGE BODY|EGLLXYA|#M1BPOS/.test(${V}.querySelector('.rows').textContent)`)), '★ MESSAGES: labels only, never a body (not the owner)');
    await ev(`${V}.querySelector('.trkList .hd .tab[data-tab="aircraft"]').click()`);
    ok(await until(`${V}?.querySelector('.trkList .hd .tab[aria-selected="true"]')?.dataset.tab === 'aircraft'`, 3000), 'the AIRCRAFT tab');
    ok(await ev(`${V}.querySelector('.trkList .hd .srt [aria-pressed="true"]')?.textContent`) === 'LATEST', 'DATALINK: the aircraft sort is LATEST');
    const rowsTxt = () => ev(`${V}?.querySelector('.rows')?.textContent || ''`);
    ok(await until(`/BAW117/.test(${V}?.querySelector('.rows')?.textContent || '') && /VDL2 \\+ ADS-B|ADS-B \\+ VDL2/.test(${V}?.querySelector('.rows')?.textContent || '')`, 45000),
       `radio A's ADS-B joins radio B's datalink: ${(await rowsTxt()).slice(0, 300)}`);
    const n4 = await ev(`[...${V}.querySelectorAll('.trkRow')].filter(r => r.dataset.key === 'a:4CA97B').length`);
    ok(n4 === 1, `4CA97B is ONE row (${n4})`);
    ok(await ev(`[...${V}.querySelectorAll('.trkRow')].some(r => r.dataset.key === 'a:400F11')`), '400F11 (ADS-B only, radio A) is on the AIRCRAFT list beside the datalink\'s');
    // ★ NEAREST: the rows by range (the receiver's own position)
    await ev(`${V}.querySelector('.trkList .hd .srt [data-srt="nearest"]').click()`);
    { const nm = JSON.parse(await ev(`JSON.stringify([...${V}.querySelectorAll('.trkRow')].map(r => parseFloat((r.querySelector('.rg')?.textContent || '').replace(/[^0-9.]/g, ''))).filter(x => x === x))`) || '[]');
      ok(nm.length >= 2 && nm.every((x, i) => i === 0 || x >= nm[i - 1]), `NEAREST: by range (${nm.join(', ')} NM)`); }
    ok(await ev(`localStorage.getItem('vibesdr.airSeaSort')`) === 'nearest', '…remembered');
    await until(`[...${V}.querySelectorAll('[data-tgl]')].map(b => b.textContent.trim()).join(',').startsWith('VDL2,ACARS,ADS-C,ADS-B,AIRPORTS')`, 15000);
    const tg = await ev(`[...${V}.querySelectorAll('[data-tgl]')].map(b => b.textContent.trim()).join(',')`);
    ok(/^VDL2,ACARS,ADS-C,ADS-B,AIRPORTS(,SEA AREAS)?$/.test(tg || ''), `the layer keys, the live ones first: ${tg} (no HFDL: nothing heard there)`);
    ok(await ev(`JSON.stringify([...${V}.querySelectorAll('[data-tgl].live')].map(b => b.textContent.trim()))`) === JSON.stringify(['VDL2', 'ACARS', 'ADS-C']), 'the DATALINK\'s layers marked LIVE');
    ok(await ev(`[...${V}.querySelectorAll('[data-tgl]')].filter(b => b.textContent.trim() !== 'SEA AREAS').every(b => b.getAttribute('aria-pressed') === 'true')`), 'every layer on by default (AIRPORTS too)');
    ok(await ev(`[...${V}.querySelectorAll('[data-tgl]')].every(b => b.textContent.trim() !== 'SEA AREAS' || b.getAttribute('aria-pressed') === 'false')`), 'SEA AREAS (where offered) off by default with nothing maritime here');
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
    // ★★ beta96 THE CLICK ORDER (receiver → points → lines → areas; Stuart's community-map bug): an aircraft over a sea area's
    //    marker — a press there opens the AIRCRAFT's card, never the area's
    {
      await ev(`[...${V}.querySelectorAll('[data-tgl]')].find(b => b.textContent.trim() === 'SEA AREAS' && b.getAttribute('aria-pressed') === 'false')?.click()`);
      const areaSel = `[...document.querySelectorAll('.trkView:not([hidden]) .vtl-mk')].find(e => / sea area/.test(e.getAttribute('aria-label') || '') && getComputedStyle(e).display !== 'none' && !e.closest('.vtl-yhid') && e.getBoundingClientRect().width > 0 && (() => { const r = e.getBoundingClientRect(); return r.left > 60 && r.top > 120 && r.right < innerWidth - 60 && r.bottom < innerHeight - 200; })())`;
      const got = await until(`!!${areaSel}`, 12000);
      ok(got, 'SEA AREAS on: a sea area\'s marker in view');
      if (got) {
        const pt = JSON.parse(await ev(`(() => {
          const el = ${areaSel}; const r = el.getBoundingClientRect(); const x = r.left + r.width / 2, y = r.top + r.height / 2;
          el.setAttribute('data-test-area', '1');
          const gl = window.__vibeTrkMap._gl || window.__vibeTrkMap; const c = gl.getCanvas().getBoundingClientRect();
          const ll = gl.unproject([x - c.left, y - c.top]);
          window.__vibeAcLayer.update([{ hex: '~a5ea00', flight: 'OVERSEA', lat: ll.lat, lon: ll.lng, alt_baro: 30000, seen_pos: 1 }]);
          return JSON.stringify({ x, y });
        })()`) || '{}');
        await sleep(1500);
        await ev(`document.querySelectorAll('.vtl-card .vtl-x').forEach((b) => b.click())`);
        await ev(`(() => { const el = document.querySelector('[data-test-area="1"]'); el && el.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true, clientX: ${pt.x}, clientY: ${pt.y}, view: window })); })()`);
        await sleep(800);
        const res = JSON.parse(await ev(`(() => { const ac = document.querySelector('.vsadsb-pop-dark'); const vc = document.querySelector('.vtl-card:not([hidden])');
          return JSON.stringify({ ac: ac ? ac.textContent.slice(0, 80) : '', area: vc ? vc.textContent.slice(0, 60) : '' }); })()`) || '{}');
        ok(/OVERSEA/.test(res.ac || '') && !res.area, `beta96: a press on a sea area with an aircraft over it opens the aircraft (${JSON.stringify(res)})`);
        await ev(`document.querySelectorAll('.vsadsb-pop-dark .maplibregl-popup-close-button, .vsadsb-pop-dark [class*="close"]').forEach((b) => b.click())`);
        await ev(`[...${V}.querySelectorAll('[data-tgl]')].find(b => b.textContent.trim() === 'SEA AREAS')?.click()`);
      }
    }
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
    ok(await until(`(() => { try { return JSON.parse(localStorage.getItem('vibesdr.airSeaLayers')).airports === false; } catch { return false; } })()`, 2000), 'remembered');
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
    // ★★ beta96 a MODE CHANGE while the map is open: the chrome follows (the map is recomputed, never left on DATALINK)
    ok(!(await ev(`/DATALINK/.test(document.querySelector('.trkView')?.querySelector('.ttl')?.textContent || '')`)) || await ev(`document.querySelector('.trkView').hidden`), 'the map is not left titled DATALINK');
    // the HFDL decoder box's MAP: the same map, standalone, HFDL first
    await ev(`document.querySelector('#decodersPanel [data-dec="hfdl"]')?.click()`);
    ok(await until(`getComputedStyle(document.getElementById('hfdlMapBtn')).display !== 'none'`, 4000), 'the HFDL box has MAP');
    await ev(`document.getElementById('hfdlMapBtn').click()`);
    ok(await until(`!!${V} && /^AVIATION & MARITIME · HFDL$/.test(${V}.querySelector('.ttl')?.textContent || '')`, 5000), `MAP opens the AIR & SEA map, HFDL live (${await ev(`${V}?.querySelector('.ttl')?.textContent`)})`);
    ok(await ev(`${V}.querySelector('.exit').textContent`) === 'CLOSE MAP', '…CLOSE MAP (the decoder carries on)');
    ok(await ev(`${V}.querySelector('.trkList .hd .tab[aria-selected="true"]')?.dataset.tab`) === 'messages', '…on MESSAGES');
    await ev(`${V}.querySelector('.trkList .hd .tab[data-tab="aircraft"]').click()`);
    // ★★ tap an aircraft → MESSAGES filtered to it
    await until(`[...${V}.querySelectorAll('.trkRow')].some(r => r.dataset.key === 'a:4CA97B')`, 8000);
    await ev(`[...${V}.querySelectorAll('.trkRow')].find(r => r.dataset.key === 'a:4CA97B')?.click()`);
    await ev(`${V}.querySelector('.trkList .hd .tab[data-tab="messages"]').click()`);
    ok(await until(`!${V}.querySelector('.trkList .filt').hidden && [...${V}.querySelectorAll('.trkRow.trkMsg')].length > 0 && [...${V}.querySelectorAll('.trkRow.trkMsg')].every(r => r.dataset.focus === 'a:4CA97B')`, 4000),
       `tap an aircraft → MESSAGES shows only its messages (${await ev(`${V}.querySelector('.trkList .filt')?.textContent`)})`);
    await ev(`${V}.querySelector('.trkList .filt button').click()`);
    ok(await until(`${V}.querySelector('.trkList .filt').hidden && [...${V}.querySelectorAll('.trkRow.trkMsg')].some(r => r.dataset.focus !== 'a:4CA97B')`, 3000), '× ALL: every message again');
    await ev(`${V}.querySelector('.trkList .hd .tab[data-tab="aircraft"]').click()`);
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
