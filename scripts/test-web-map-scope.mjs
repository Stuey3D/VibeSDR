// test-web-map-scope.mjs — beta96 (Stuart 11-26 via the coordinator) THE MAPS' SCOPE on the web, against the REAL server (this
// tree's vibeserver, fake rtl_tcp — nothing on air), in headless Edge with --mute-audio (tests are silent). docs/v12/MAP-SHARED.md.
//   · vibeserver.json says this radio's id (radioId, from the config's serial) and radioCount.
//   · AIR & SEA LIVE → the map on AIS: aircraft.json / ais-ships.json are answered by a stub (this radio's id, another radio's
//     'bbbb0002', a feed, untagged; first-heard 5 s and 4000 s ago) → the OTHER RADIOS ON THIS SERVER chip is drawn (one radio
//     + another source's entries); OFF hides the other radio's and the feed's (the SHOW counts follow; untagged stays), is
//     remembered (localStorage), sends NOTHING to the server; THIS SESSION beside the window hides what was first heard
//     before; the scope line says so.
//   · AIR & SEA LIVE pressed while lit: the map stays / comes back and the mode carries on (leaving is EXIT).
//   · the viewer (VIEWS' AIR & SEA): no switch, no THIS SESSION; window.openAirSeaViewer is the same function.
//   VIBESERVER_BIN=…/vibeserver node scripts/test-web-map-scope.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !fs.existsSync(EDGE)) { console.log('   not run — VIBESERVER_BIN and Edge needed'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

/** A minimal WebSocket client (test-ask-before-tuning.mjs's), text frames only — the OTHER listener. */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], open: false, closed: false };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('close', () => { st.closed = true; });
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; st.open = true; buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const p = buf.subarray(off, off + len); buf = buf.subarray(off + len);
      if (op === 0x9) { const q = Buffer.alloc(6 + p.length); q[0] = 0x8a; q[1] = 0x80 | p.length; q.writeUInt32BE(0, 2); p.copy(q, 6); s.write(q); continue; }
      if (op === 0x1) { st.txt.push(p.toString()); if (st.txt.length > 400) st.txt.splice(0, 200); }
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b]));
  };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); setTimeout(() => s.destroy(), 300); };
  st.last = (type) => { for (let i = st.txt.length - 1; i >= 0; i--) if (st.txt[i].includes(`"type":"${type}"`)) return JSON.parse(st.txt[i]); return null; };
  return st;
}

/** ★ Every frame the page sends, logged before the page's own code runs — a SHOW chip must add none. */
const SEND_HOOK = `(() => { const s = WebSocket.prototype.send; window.__sent = [];
  WebSocket.prototype.send = function (d) { try { if (typeof d === 'string') window.__sent.push(d); } catch {} return s.call(this, d); }; })()`;

async function session(tag, users, before, run) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-mapscope-${tag}-`));
  process.on('exit', () => { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
  for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
  // ★ a configured server (an unconfigured one answers with the owner's sign-in page)
  // ★ only for the one-listener server: a configured one takes its ask-before-tuning from the config, not the flag
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Map scope test', locator: 'IO90bs', adminPass: 'mapscope-web-admin',
    radios: [{ serial: `${tag}-web`, label: 'Test radio', enabled: true, configured: true, users }] }));
  const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000'], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBE_ASK_GONE_GRACE_S: '2', VIBE_ASK_RANK_FORGET_S: '3',
                VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const args = ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', String(users)];
  if (users > 1) args.push('--ask-before-tuning');
  const srv = spawn(BIN, args, { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });
  for (let i = 0; i < 100; i++) {
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
    `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
  let wsC; const failBefore = fail; const others = [];
  try {
    const ctx = { port, others, stub: '', join: async (sid) => { const s = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`); others.push(s); await sleep(900); return s; } };
    await before?.(ctx);
    await sleep(2500);
    const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
    wsC = new WebSocket(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
    let id = 0; const pend = new Map();
    wsC.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
    await new Promise((r) => { wsC.onopen = r; });
    const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); wsC.send(JSON.stringify({ id: i, method, params })); });
    const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.text; };
    const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(os.tmpdir(), `vibesdr-mapscope-${name}.png`); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
    const until = async (x, ms) => { for (let i = 0; i < ms / 250; i++) { if (await ev(x)) return true; await sleep(250); } return false; };
    await send('Page.enable'); await send('Runtime.enable');
    await send('Page.addScriptToEvaluateOnNewDocument', { source: SEND_HOOK });
    if (ctx.stub) await send('Page.addScriptToEvaluateOnNewDocument', { source: ctx.stub });
    await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
    await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
    if (!await until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 20000)) {
      await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });   // ★ a slow first load: once more
      await until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 20000);
    }
    await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim())); b && b.click(); return !!b; })()`);
    await until(`!!document.querySelector('#audioGate button')`, 15000);
    if (await ev(`!!document.querySelector('#audioGate button')`)) {
      const r = JSON.parse(await ev(`JSON.stringify(document.querySelector('#audioGate button').getBoundingClientRect())`));
      const x = r.x + r.width / 2, y = r.y + r.height / 2;
      await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
      await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
    }
    await sleep(3000);
    await run({ ev, send, shot, until, ...ctx });
  } catch (e) { ok(false, `${tag} driver: ${e?.stack || e}`); }
  finally {
    for (const o of others) try { o.close(); } catch {}
    try { wsC?.close(); } catch {}
    edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM');
    await sleep(600);
    if (fail === failBefore) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
  }
}


const AS = `window.__vibeAirSea && window.__vibeAirSea()`;
const SC = `window.__vibeMapScope && window.__vibeMapScope()`;
const V = `document.querySelector('.trkView:not([hidden])')`;
const CHIPN = (id) => `(() => { const b = ${V}?.querySelector('.tgl [data-tgl="${id}"] .n'); return b ? Number(b.textContent) : -1; })()`;
const SCOPE = `${V}?.querySelector('.trkTop .scope [data-scope]')`;
const RNG = `[...(${V}?.querySelectorAll('.trkTop .rng:not(.tgl):not(.scope) [data-rng]') || [])].map(b => b.dataset.rng + (b.getAttribute('aria-pressed') === 'true' ? '*' : ''))`;

/** The stub: aircraft.json and ais-ships.json answered with tagged rows; everything else goes to the real server. */
function stubFor(mine) {
  const air = { now: 0, count: 4, aircraft: [
    { key: 'A11111', icao: 'A11111', flight: 'MINE1', src: ['VDL2'], srcAge: { VDL2: 2 }, age: 2, first: 5, n: 3, lat: 51.2, lon: -0.5, radios: [mine] },
    { key: 'B22222', icao: 'B22222', flight: 'OTHER2', src: ['VDL2'], srcAge: { VDL2: 2 }, age: 2, first: 5, n: 3, lat: 51.3, lon: -0.4, radios: ['bbbb0002'] },
    { key: 'C33333', icao: 'C33333', flight: 'OLD3', src: ['VDL2'], srcAge: { VDL2: 2 }, age: 2, first: 4000, n: 3, lat: 51.4, lon: -0.3 },
    { key: 'D44444', icao: 'D44444', flight: 'FEED4', src: ['VDL2'], srcAge: { VDL2: 2 }, age: 2, first: 5, n: 3, lat: 51.5, lon: -0.2, radios: ['feed'] },
  ], stations: [] };
  const ships = { now: 0, count: 2, total: 2, vessels: [
    { mmsi: 235000001, kind: 'A', name: 'OURS', lat: 50.8, lon: -1.1, seen: 10, age: 20, posAge: 10, count: 3, radios: [mine] },
    { mmsi: 235000002, kind: 'A', name: 'THEIRS', lat: 50.7, lon: -1.2, seen: 10, age: 20, posAge: 10, count: 3, radios: ['bbbb0002'] },
  ] };
  return `(() => { const real = window.fetch; const AIR = ${JSON.stringify(air)}, SHIPS = ${JSON.stringify(ships)};
    window.__stubHits = 0;
    window.fetch = function (u, o) { const s = String(u && u.url || u);
      const now = Math.floor(Date.now() / 1000);
      if (s.includes('/vibeserver/aircraft.json')) { window.__stubHits++; return Promise.resolve(new Response(JSON.stringify({ ...AIR, now }), { status: 200, headers: { 'Content-Type': 'application/json' } })); }
      if (s.includes('/gallery/ais-ships.json')) { window.__stubHits++; return Promise.resolve(new Response(JSON.stringify({ ...SHIPS, now }), { status: 200, headers: { 'Content-Type': 'application/json' } })); }
      return real.call(this, u, o); }; })()`;
}

console.log('── the scope on the AIR & SEA map ──');
await session('scope', 4, async (ctx) => {
  const vj = await (await fetch(`http://127.0.0.1:${ctx.port}/vibeserver.json`)).json();
  ctx.mine = vj.radioId;
  ok(/^[0-9a-z]{1,16}$/.test(vj.radioId || '') && vj.radioCount === 1, `vibeserver.json: radioId "${vj.radioId}", radioCount ${vj.radioCount}`);
  ctx.stub = stubFor(vj.radioId || 'none');
}, async ({ ev, until, shot }) => {
  ok(await until(`(${SC})?.mine?.length > 0`, 8000), `the page read its radio id: ${JSON.stringify(JSON.parse(await ev(`JSON.stringify(${SC})`) || 'null'))}`);
  await ev(`localStorage.removeItem('vibesdr.mapOtherRadios'); localStorage.setItem('vibesdr.airSeaLast', 'ais'); true`);
  await ev(`document.getElementById('mMode')?.click()`); await sleep(500);
  await ev(`document.getElementById('mModeAirSea')?.click()`);
  ok(await until(`(${AS})?.open && (${AS}).mode === 'ais'`, 12000), 'AIR & SEA LIVE: the map on AIS');
  ok(await until(`window.__stubHits > 1 && ${CHIPN('vdl2')} === 4`, 10000), `SHOW: VDL2 counts every source's aircraft (${await ev(CHIPN('vdl2'))})`);
  ok(await until(`!!${SCOPE} && !${SCOPE}.closest('.scope').hidden`, 4000), 'the OTHER RADIOS ON THIS SERVER chip is drawn (one radio + another source\'s entries)');
  ok(await ev(`${SCOPE}?.textContent === 'OTHER RADIOS ON THIS SERVER' && ${SCOPE}.getAttribute('aria-pressed') === 'true'`), 'ON by default');
  const n0 = await ev(`window.__sent.filter(s => !/"type":"(ping|state|keepalive)"/.test(s)).length`);
  await ev(`${SCOPE}.click(); true`);
  ok(await until(`${CHIPN('vdl2')} === 2`, 4000), `OFF: only this radio's own + the untagged one (VDL2 ${await ev(CHIPN('vdl2'))}; the other radio's and the feed's hidden)`);
  ok(await ev(`${CHIPN('ships')}`) === 1, `OFF: the ships follow (${await ev(CHIPN('ships'))} — ours only)`);
  ok(await ev(`localStorage.getItem('vibesdr.mapOtherRadios') === '0'`), 'remembered in this browser');
  ok(/Only this radio/.test(await ev(`${V}?.querySelector('.trkTop .scopeNote')?.textContent || ''`)), 'the line says what is hidden');
  await sleep(800);
  const n1 = await ev(`window.__sent.filter(s => !/"type":"(ping|state|keepalive)"/.test(s)).length`);
  ok(n1 === n0, 'the switch sends nothing to the server');
  const rng = JSON.parse(await ev(`JSON.stringify(${RNG})`) || '[]');
  ok(rng.includes('session'), `THIS SESSION beside the window: ${rng.join(' ')}`);
  await ev(`${V}.querySelector('.trkTop [data-rng="session"]').click(); true`);
  ok(await until(`${CHIPN('vdl2')} === 1 && (${SC}).session`, 5000), `THIS SESSION + OFF: only ours first heard since the map opened (VDL2 ${await ev(CHIPN('vdl2'))}; OLD3, first heard 4000 s ago, gone)`);
  await ev(`${SCOPE}.click(); true`);
  ok(await until(`${CHIPN('vdl2')} === 3`, 4000), `THIS SESSION + ON: ours, the other radio's and the feed's new ones (${await ev(CHIPN('vdl2'))})`);
  ok(/this session/.test(await ev(`${V}?.querySelector('.trkTop .scopeNote')?.textContent || ''`)), 'the line says THIS SESSION');
  await shot('session');
  await ev(`${V}.querySelector('.trkTop [data-rng]:not([data-rng="session"])').click(); true`);
  ok(await until(`${CHIPN('vdl2')} === 4 && !(${SC}).session`, 4000), 'another window key leaves the session: everything back');
  // AIR & SEA LIVE pressed while lit: the map stays, the mode carries on
  await ev(`document.getElementById('mMode')?.click()`); await sleep(500);
  await ev(`document.getElementById('mModeAirSea')?.click()`);
  await sleep(1500);
  ok(await ev(`(${AS})?.open && (${AS}).mode === 'ais'`), 'AIR & SEA LIVE pressed while lit: the map (re)opens, still AIS (leaving is EXIT)');
  ok(/^EXIT /.test(await ev(`(${AS}).exit`) || ''), `the map's EXIT is the way out: "${await ev(`(${AS}).exit`)}"`);
  // the viewer: no switch, no THIS SESSION
  await ev(`document.querySelector('.trkView:not([hidden]) .trkTop .exit').click()`);
  await until(`!(${AS})?.open`, 8000);
  ok(await ev(`window.openAirSeaViewer === window.__vibeOpenAirSeaViewer && typeof window.openAirSeaViewer === 'function'`), 'window.openAirSeaViewer and __vibeOpenAirSeaViewer are one function (b96-sbsfeed)');
  await ev(`window.openAirSeaViewer(); true`);
  ok(await until(`(${AS})?.open`, 5000), 'the viewer opens');
  await sleep(1500);
  ok(await ev(`!${SCOPE} || ${SCOPE}.closest('.scope').hidden`), 'the viewer: no OTHER RADIOS chip');
  ok(!(JSON.parse(await ev(`JSON.stringify(${RNG})`) || '[]')).some((r) => r.startsWith('session')), 'the viewer: no THIS SESSION');
  ok(await ev(`${CHIPN('vdl2')}`) === 4, `the viewer shows every source (${await ev(CHIPN('vdl2'))})`);
});

console.log(`test-web-map-scope: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
