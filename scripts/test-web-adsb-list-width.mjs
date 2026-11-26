#!/usr/bin/env node
// test-web-adsb-list-width.mjs — V12 beta96 THE CALLSIGN IS NEVER SQUEEZED OUT of ADS-B's AIRCRAFT list (Stuart 11-26, a
// beta95 server in Safari, the list panel ~232 px wide: "the signal hides the aircraft details" — every row showed the
// signal and a clipped "[", no callsign: the fixed altitude / speed / range columns took the row, the chip beside the name
// never shrinks, and the only narrow rule was a VIEWPORT @media, which never fires for a narrow PANEL on a wide window).
// The list now sizes by its OWN width (a container query on #adsbList). Checked in the REAL page against a REAL vibeserver
// (fake rtl_tcp replaying `test-adsb --write-clip`: RYR12AB, EZY45KL, BAW117), headless Edge with --mute-audio (tests are
// silent): the AIR & SEA map's list panel forced to 232, 300, 400 and 600 px on a 1280 px window, and the natural layout
// on 390 and 320 px phones. Each row: the callsign element shows at least min(its whole text, 7 characters), it lies inside
// the row, nothing overlaps it, no cell is cut (altitude, range), and nothing overflows sideways (row or list).
// Screenshots: $VIBE_SHOTS (a directory) if set. Needs the page BUILT INTO the server (build-web, then the server rebuilt).
//   VIBESERVER_BIN=…/vibeserver VIBE_ADSB_SYN_CLIP=…/a.u8 node scripts/test-web-adsb-list-width.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '', ADSB = process.env.VIBE_ADSB_SYN_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !ADSB || !fs.existsSync(ADSB) || !fs.existsSync(EDGE)) { console.log('   not run — VIBESERVER_BIN, VIBE_ADSB_SYN_CLIP and Edge needed'); process.exit(3); }
const SHOTS = process.env.VIBE_SHOTS || '';
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-adsbw-'));
for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'ADS-B list width test', locator: 'IO92nh', adminPass: 'adsbw-web-admin',
  radios: [{ serial: 'adsbw-web', label: 'ADS-B radio', enabled: true, configured: true, users: 2 }] }));
const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--adsb', ADSB], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', '2', '--idle-grace', '0'],
                  { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });
let edge = null, sock = null, keep = null;
const done = (code) => {
  clearInterval(keep); try { sock?.end(); } catch {} try { edge?.kill(); } catch {} srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  setTimeout(() => { if (!code) { try { fs.rmSync(dir, { recursive: true, force: true, maxRetries: 10, retryDelay: 200 }); } catch {} }
    else { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true }); } catch {} console.log(`   (kept ${dir})`); } process.exit(code); }, 700);
};
const watchdog = setTimeout(() => { console.log('   FAIL timed out'); fail++; done(1); }, 240000);

try {
  for (let i = 0; i < 60; i++) { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`); if (r.ok) break; } catch {} await sleep(250); }
  // ── another listener puts the radio in ADS-B (its socket), so the page joins a radio already in the mode ──
  const key = crypto.randomBytes(16).toString('base64');
  sock = net.connect(port, '127.0.0.1'); sock.on('error', () => {});
  await new Promise((r) => sock.on('connect', r));
  sock.write(`GET /ws/user-spectrum?user_session_id=adsbwA&bins=256&proto=1 HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`);
  sock.on('data', () => {});
  const wsSend = (o) => { const b = Buffer.from(JSON.stringify(o)); const h = Buffer.alloc(6); h[0] = 0x81; h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); sock.write(Buffer.concat([h, b])); };
  await sleep(1500);
  wsSend({ type: 'adsb', on: 1 });
  keep = setInterval(() => { try { wsSend({ type: 'ping' }); } catch {} }, 10000);
  await sleep(5000);

  edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
    '--enable-unsafe-swiftshader', `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
  let page = null;
  for (let i = 0; i < 40 && !page; i++) { await sleep(250); try { page = (await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json()).find((t) => t.type === 'page'); } catch {} }
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  ws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  await new Promise((r) => { ws.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? ('ERR ' + (r.result?.exceptionDetails?.exception?.description || r.result?.exceptionDetails?.text)); };
  const until = async (x, ms) => { for (let i = 0; i < ms / 250; i++) { if ((await ev(x)) === true) return true; await sleep(250); } return false; };
  const shot = async (name) => { if (!SHOTS) return; fs.mkdirSync(SHOTS, { recursive: true }); const s = await send('Page.captureScreenshot', { format: 'png' }); fs.writeFileSync(path.join(SHOTS, `adsb-list-${name}.png`), Buffer.from(s.result.data, 'base64')); };
  await send('Page.enable'); await send('Runtime.enable');
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  await until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 15000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim())); b && b.click(); return !!b; })()`);
  if (await until(`!!document.querySelector('#audioGate button')`, 8000)) {
    const r = JSON.parse(await ev(`JSON.stringify(document.querySelector('#audioGate button').getBoundingClientRect())`));
    const x = r.x + r.width / 2, y = r.y + r.height / 2;
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
  }
  const V = `document.querySelector('.trkView:not([hidden])')`;
  ok(await until(`/ADS-B/.test(${V}?.querySelector('.ttl')?.textContent || '')`, 25000), `the page joins the radio in ADS-B: the AIR & SEA map (${await ev(`${V}?.querySelector('.ttl')?.textContent`)})`);
  ok(await until(`${V}.querySelectorAll('#adsbList .adsbRow').length >= 3 && [...${V}.querySelectorAll('#adsbList .adsbRow .id b')].every(b => /[A-Z]{3}\\d/.test(b.textContent))`, 40000),
     `the AIRCRAFT list has the three aircraft by callsign (${await ev(`[...document.querySelectorAll('#adsbList .adsbRow .id b')].map(b => b.textContent).join(', ')`)})`);
  await until(`[...${V}.querySelectorAll('#adsbList .adsbRow .snrChip')].length >= 3`, 15000);   // the reply SNR (the chip beside the name)
  ok(await ev(`${V}.querySelectorAll('#adsbList .adsbRow .snrChip').length >= 3`), `…each with its SNR chip (${await ev(`[...document.querySelectorAll('#adsbList .snrChip')].map(c => c.textContent).join(', ')`)})`);

  /** Every row's geometry: the callsign's shown width against its whole text and its first 7 characters (clones of the
   *  element itself — same font, flag and letter-spacing), the cells' boxes, and any sideways overflow. */
  const MEASURE = `(() => {
    const list = ${V}.querySelector('#adsbList'), host = ${V}.querySelector('.trkList');
    const rows = [...list.querySelectorAll('.adsbRow')];
    const box = (e) => { const r = e.getBoundingClientRect(); return { l: r.left, r: r.right, t: r.top, b: r.bottom, w: r.width }; };
    const shown = (e) => e && getComputedStyle(e).display !== 'none' && e.getBoundingClientRect().width > 0;
    const textW = (b, n) => { const c = b.cloneNode(true); const t = [...c.childNodes].filter(x => x.nodeType === 3).pop();
      if (n && t) t.textContent = t.textContent.slice(0, n);
      c.style.cssText = 'position:absolute;visibility:hidden;width:auto;max-width:none;overflow:visible;display:inline-block;flex:none';
      b.parentNode.appendChild(c); const w = c.getBoundingClientRect().width; c.remove(); return w; };
    return JSON.stringify({ panel: host.getBoundingClientRect().width, list: list.getBoundingClientRect().width,
      listOver: list.scrollWidth - list.clientWidth, rowsOver: host.querySelector('.rows').scrollWidth - host.querySelector('.rows').clientWidth,
      rows: rows.map((row) => { const b = row.querySelector('.id b'), chip = row.querySelector('.snrChip');
        const cells = ['.alt', '.spd', '.rng'].map(s => row.querySelector(s)).filter(shown);
        return { call: [...b.childNodes].filter(x => x.nodeType === 3).map(x => x.textContent).join(''), row: box(row), b: box(b), full: textW(b, 0), seven: textW(b, 7),
          rowOver: row.scrollWidth - row.clientWidth, chip: chip && shown(chip) ? box(chip) : null,
          cells: cells.map(c => ({ k: c.className, ...box(c), cut: c.scrollWidth - c.clientWidth, txt: c.textContent })),
          spd: shown(row.querySelector('.spd')), trk: shown(row.querySelector('.trk')), h: row.getBoundingClientRect().height }; }) }); })()`;
  const overlap = (a, c) => Math.min(a.r, c.r) - Math.max(a.l, c.l) > 0.5 && Math.min(a.b, c.b) - Math.max(a.t, c.t) > 0.5;
  const check = (tag, m) => {
    ok(m.listOver <= 0 && m.rowsOver <= 0, `${tag}: nothing overflows the list sideways (list ${m.listOver}, rows ${m.rowsOver})`);
    for (const r of m.rows) {
      const need = Math.min(r.full, r.seven);
      ok(r.b.w + 0.5 >= need, `${tag}: ${r.call}'s callsign shows ${r.b.w.toFixed(0)} px ≥ min(whole ${r.full.toFixed(0)}, 7 chars ${r.seven.toFixed(0)})`);
      ok(r.b.l >= r.row.l - 0.5 && r.b.r <= r.row.r + 0.5 && r.rowOver <= 0, `${tag}: ${r.call}: the callsign lies inside its row, the row does not overflow (${r.rowOver})`);
      const others = [...(r.chip ? [{ k: 'chip', ...r.chip }] : []), ...r.cells];
      const hit = others.filter((c) => overlap(r.b, c)).map((c) => c.k);
      ok(!hit.length, `${tag}: ${r.call}: nothing lies over the callsign${hit.length ? ' (' + hit.join(', ') + ')' : ''}`);
      const cut = r.cells.filter((c) => c.cut > 0 || c.l < r.row.l - 0.5 || c.r > r.row.r + 0.5).map((c) => `${c.k} "${c.txt}"`);
      const cross = r.cells.some((c, i) => r.cells.some((d, j) => j > i && overlap(c, d)));
      ok(!cut.length && !cross, `${tag}: ${r.call}: altitude / range whole, inside the row, not over each other${cut.length ? ' (' + cut.join(', ') + ')' : ''}`);
      ok(!r.chip || (r.chip.l >= r.row.l - 0.5 && r.chip.r <= r.row.r + 0.5), `${tag}: ${r.call}: the SNR chip is inside the row`);
    }
  };
  // ── the panel forced to each width on the 1280 px window (Stuart's case: a narrow PANEL, a wide window) ──
  await ev(`(() => { const s = document.createElement('style'); s.id = 'wtest'; document.head.appendChild(s); return 1; })()`);
  for (const w of [232, 300, 400, 600]) {
    await ev(`document.getElementById('wtest').textContent = '.trkView .trkList { width: ${w}px !important; max-width: none !important; max-height: 80% !important; }'; 1`);
    await sleep(700);
    const m = JSON.parse(await ev(MEASURE));
    ok(Math.abs(m.panel - w) < 1, `${w} px: the panel is ${m.panel} px`);
    check(`${w} px`, m);
    console.log(`      ${w} px: ${m.rows.map((r) => `${r.call} ${r.b.w.toFixed(0)}/${r.full.toFixed(0)} px${r.spd ? ' +kt' : ''}${r.trk ? ' +trk' : ''} h${r.h.toFixed(0)}`).join(' · ')}`);
    if (w === 600) ok(m.rows.every((r) => r.spd), '600 px: the speed column is there (room for every column)');
    await shot(`panel-${w}`);
  }
  await ev(`document.getElementById('wtest').remove(); 1`);
  // ── phones: the panel as the page lays it out (full width less its margins) ──
  for (const [w, h] of [[390, 844], [320, 640]]) {
    await send('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 2, mobile: true });
    await sleep(1200);
    const m = JSON.parse(await ev(MEASURE));
    check(`phone ${w}`, m);
    console.log(`      phone ${w} (panel ${m.panel.toFixed(0)}): ${m.rows.map((r) => `${r.call} ${r.b.w.toFixed(0)}/${r.full.toFixed(0)} px`).join(' · ')}`);
    await shot(`phone-${w}`);
  }
  ws.close();
} catch (e) { fail++; console.log('   FAIL', e && e.stack || e); }
clearTimeout(watchdog);
console.log(`test-web-adsb-list-width: ${pass} passed, ${fail} failed`);
done(fail ? 1 : 0);
