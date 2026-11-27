// test-web-lrpt-channels.mjs — ★★★ beta96 METEOR LRPT's CHANNEL PICKER in the web client, through a real vibeserver and the
// REAL built web page (docs/v12/LRPT-CHANNEL-PICKER.md). A fake rtl_tcp replays a synthetic NIGHT pass (test-lrpt
// --write-clip: channels 1, 2, 4 as Meteor-M2-3 sends them, channels 1 and 2 DARK — the sensor's noise, as NickB's 19:43 UTC
// pass) at 137.9 MHz. Headless Edge then:
//   • the wire: every 0x01 preview row is followed by its 0x09 (R = ch 2, B = ch 1, + ch 4);
//   • METEOR LRPT opens as a picture (its key re-added in the test page only — see addScriptToEvaluateOnNewDocument); the channel key appears beside MAP OUTLINE and reads "AUTO · CH 4 IR (NIGHT)";
//     the visible picture is GREY (R = G = B) with a picture in it — channel 4, not the speckled composite;
//   • the key cycles COMPOSITE (colour: R ≠ B somewhere) → CH 1 → CH 2 → CH 4 IR → AUTO, the whole pass redrawn each time;
//   • the pick is remembered (localStorage) across a reload; no exception; 390 px: no sideways scroll.
// ★ SILENT: headless Edge with --mute-audio and --disable-audio-output.
//   VIBESERVER_BIN=…/vibeserver VIBE_LRPT_CLIP=…/lrpt-night.cu8 node scripts/test-web-lrpt-channels.mjs
//   (make the clip: test-lrpt --write-clip lrpt-night.cu8 960000 night 48). The page is the one compiled into the server.
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_LRPT_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_LRPT_CLIP)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }
const RATE = 960000, DIAL = 137900000;

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-weblrptch-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'METEOR channel test', locator: 'IO92nh', adminPass: 'wlc-admin',
  radios: [{ serial: 'wlc', label: 'Test radio', enabled: true, configured: true }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--hf', CLIP, '--hf-centre', String(DIAL)], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(DIAL), '--mode', 'nfm', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-weblrptch-edge-'));
let edge = null, keep = false;
const deadline = setTimeout(() => { console.log('   FAIL deadline (200 s)'); try { edge?.kill('SIGKILL'); } catch {} srv.kill('SIGKILL'); rtl.kill('SIGKILL'); process.exit(1); }, 200000);
try {
  for (let i = 0; i < 150; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  const st = await fetch(`${H}/vibeserver.json`).then((r) => r.json());
  ok(st.lrpt === true || (Array.isArray(st.modes) && st.modes.includes('lrpt')), 'vibeserver.json offers LRPT');

  edge = spawn(EDGE, ['--headless=new', `--remote-debugging-port=${cdp}`, '--no-first-run', '--no-default-browser-check', '--mute-audio',
    '--disable-audio-output', `--user-data-dir=${prof}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
  let page = null;
  for (let i = 0; i < 50 && !page; i++) { await sleep(200); page = await fetch(`http://127.0.0.1:${cdp}/json/list`).then((r) => r.json()).then((l) => l.find((t) => t.type === 'page')).catch(() => null); }
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  let id = 0; const pend = new Map(); const errs = [];
  // ★ the wire: 0x01 rows and their 0x09 (y must match, R = 65, B = 64, one extra = 67)
  const wire = { r01: 0, r09: 0, paired: 0, shaped: 0, lastY: -1, b01: 0, b09: 0 };
  ws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); }
    if (m.method === 'Network.webSocketFrameReceived' && m.params.response.opcode === 2) {
      const b = Buffer.from(m.params.response.payloadData, 'base64');
      if (b[0] === 1 && b.length === 9 + 392 * 3) { wire.r01++; wire.b01 += b.length; wire.lastY = b.readUInt32BE(1); }
      if (b[0] === 9 && b.length >= 12) { wire.r09++; wire.b09 += b.length; if (b.readUInt32BE(1) === wire.lastY) wire.paired++;
        if (b[9] === 65 && b[10] === 64 && b[11] === 1 && b[12] === 67 && b.length === 13 + 392) wire.shaped++; } }
    if (m.method === 'Runtime.exceptionThrown') errs.push(m.params.exceptionDetails?.exception?.description || 'exception'); };
  await new Promise((r) => { ws.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => (await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true })).result?.result?.value;
  const until = async (x, ms) => { for (let t = 0; t < ms; t += 250) { if (await ev(x)) return true; await sleep(250); } return !!(await ev(x)); };
  const shot = async (name) => { const r = await send('Page.captureScreenshot', { format: 'png' }); const p = path.join(SHOTS, name); fs.writeFileSync(p, Buffer.from(r.result.data, 'base64')); return p; };
  const trustedClick = async (sel) => {
    await ev(`document.querySelector(${JSON.stringify(sel)})?.scrollIntoView({ block: 'center' })`);
    const r = JSON.parse(await ev(`JSON.stringify(document.querySelector(${JSON.stringify(sel)})?.getBoundingClientRect() || {x:0,y:0,width:0,height:0})`));
    const x = r.x + r.width / 2, y = r.y + r.height / 2;
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
  };
  await send('Page.enable'); await send('Runtime.enable'); await send('Network.enable');
  /* ★ METEOR LRPT has no decoder key of its own since SATELLITE took it over (2026-11-20) — a pick there attaches it, which
   *  needs the satellite engine, elements and a pass overhead. So this test puts the key back IN THE TEST PAGE ONLY, before
   *  the page's script binds the DECODERS keys: the very attach a SATELLITE pick sends ('lrpt', AUTO), and the box shown
   *  the way any picture decoder's is. Nothing in the product changes. */
  await send('Page.addScriptToEvaluateOnNewDocument', { source: `new MutationObserver((_, o) => { const p = document.getElementById('decodersPanel');
    if (p && !p.querySelector('[data-dec="lrpt"]')) { const b = document.createElement('button'); b.className = 'btn'; b.dataset.dec = 'lrpt'; b.textContent = 'METEOR LRPT'; p.appendChild(b); o.disconnect(); } })
    .observe(document, { childList: true, subtree: true });` });
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  const start = async () => {
    await send('Page.navigate', { url: `${H}/index.html?join` });
    await sleep(4000);
    await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
    await until(`!!document.querySelector('#audioGate button')`, 15000);
    await trustedClick('#audioGate button');
    await until(`!document.getElementById('audioGate') || document.getElementById('audioGate').style.display === 'none' || !document.getElementById('audioGate').isConnected`, 8000);
    await sleep(2500);
    ok(await until(`!document.querySelector('#decodersPanel [data-dec="lrpt"]').hidden`, 8000), 'the METEOR LRPT key is offered at 137.9 MHz');
    await ev(`document.querySelector('#decodersPanel [data-dec="lrpt"]').click()`);
    ok(await until(`document.getElementById('decBox').classList.contains('open') && document.getElementById('decImage').classList.contains('on')`, 6000), 'the box opens as a picture');
    await ev(`document.getElementById('decodersPanel').classList.remove('open')`);
  };
  // the visible canvas: how grey it is (R = G = B everywhere?) and how much picture (spread of values)
  const readPic = `(() => { const c = document.getElementById('decImage'); const x = c.getContext('2d'); const w = c.width, h = c.height;
     if (!w || !h) return null; const d = x.getImageData(0, 0, w, h).data; let colour = 0, n = 0, mn = 255, mx = 0;
     for (let i = 0; i < d.length; i += 4 * 7) { if (d[i + 3] === 0) continue; n++; if (Math.abs(d[i] - d[i + 2]) > 12) colour++; mn = Math.min(mn, d[i]); mx = Math.max(mx, d[i]); }
     return JSON.stringify({ w, h, n, colour: n ? colour / n : 0, spread: mx - mn }); })()`;
  const key = () => ev(`(() => { const k = document.getElementById('decChan'); return k && !k.hidden && k.offsetParent ? k.textContent : ''; })()`);

  console.log('── a NIGHT pass: AUTO shows channel 4 ──');
  await start();
  ok(await until(`(() => { const k = document.getElementById('decChan'); return !!k && !k.hidden; })()`, 60000), 'the channel key appears once the pass has channels');
  ok(await until(`/AUTO · CH 4 IR \\(NIGHT\\)/.test(document.getElementById('decChan').textContent)`, 15000), `AUTO says what it shows: "${await key()}"`);
  await sleep(1500);
  const auto = JSON.parse((await ev(readPic)) || '{}');
  ok(auto.n > 500 && auto.colour < 0.02 && auto.spread > 80, `the picture is channel 4 — grey with a picture in it (colour ${(auto.colour * 100).toFixed(1)} %, spread ${auto.spread}, ${auto.w}×${auto.h})`);
  ok(wire.r01 >= 4 && wire.paired === wire.r09 && wire.shaped === wire.r09 && wire.r09 >= wire.r01 - 1,
     `the wire: ${wire.r01} preview rows, ${wire.r09} channel rows each after its own (${wire.paired} paired, ${wire.shaped} shaped 65/64 + ch 4), +${Math.round(100 * wire.b09 / Math.max(1, wire.b01))} % bytes`);
  const s0 = await shot('lrpt-channels-night-auto.png');

  console.log('── the key cycles ──');
  await trustedClick('#decChan'); await sleep(600);
  const comp = JSON.parse((await ev(readPic)) || '{}');
  ok((await key()) === 'COMPOSITE' && comp.colour > 0.2, `COMPOSITE: the speckled night composite, in colour (${(comp.colour * 100).toFixed(0)} % coloured) — "${await key()}"`);
  const s1 = await shot('lrpt-channels-night-composite.png');
  await trustedClick('#decChan'); await sleep(400);
  ok((await key()) === 'CH 1', `next: "${await key()}"`);
  await trustedClick('#decChan'); await sleep(400);
  ok((await key()) === 'CH 2', `next: "${await key()}"`);
  await trustedClick('#decChan'); await sleep(400);
  const ch4 = JSON.parse((await ev(readPic)) || '{}');
  ok((await key()) === 'CH 4 IR' && ch4.colour < 0.02 && ch4.spread > 80, `next: "${await key()}", grey with a picture`);
  ok(await ev(`localStorage.getItem('vibe.lrptChannel')`) === 'ch4', 'the pick is kept in this browser');
  ok(!!(await ev(`(() => { const b = document.getElementById('decOutlineBar'); return b && !b.hidden; })()`)), 'the key sits in the bar over the picture (with MAP OUTLINE when there is one)');

  console.log('── reload: the pick is remembered ──');
  await start();
  ok(await until(`document.getElementById('decChan') && !document.getElementById('decChan').hidden && document.getElementById('decChan').textContent === 'CH 4 IR'`, 60000), `after a reload: "${await key()}"`);
  const hit = await ev(`(() => { const k = document.getElementById('decChan'); k.scrollIntoView({ block: 'center' }); const r = k.getBoundingClientRect();
     const e = document.elementFromPoint(r.x + r.width / 2, r.y + r.height / 2); return (e && (e.id || e.className || e.tagName)) + ' @' + Math.round(r.x) + ',' + Math.round(r.y); })()`);
  console.log(`   (the key's centre hits: ${hit})`);
  await trustedClick('#decChan');   // → AUTO (CH 4 IR was the last choice)
  ok(await until(`/^AUTO/.test(document.getElementById('decChan').textContent)`, 3000), `and the cycle comes back round to AUTO: "${await key()}"`);

  console.log('── phone width ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(800);
  ok(await ev(`document.documentElement.scrollWidth <= window.innerWidth + 1`), `390 px: no sideways scroll (${await ev('document.documentElement.scrollWidth')})`);
  const s2 = await shot('lrpt-channels-phone.png');
  ok(errs.length === 0, `no exception${errs.length ? ': ' + errs.slice(0, 3).join(' | ') : ''}`);
  console.log(`   screenshots: ${s0} ${s1} ${s2}`);
} catch (e) {
  fail++; console.log('   FAIL', e?.stack || e); keep = true;
} finally {
  clearTimeout(deadline);
  try { edge?.kill('SIGKILL'); } catch {}
  srv.kill('SIGKILL'); rtl.kill('SIGKILL');
  await sleep(300);
  if (fail) { keep = true; console.log(`   server log: ${logPath}`); }
  if (!keep) fs.rmSync(dir, { recursive: true, force: true });
  fs.rmSync(prof, { recursive: true, force: true });
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
