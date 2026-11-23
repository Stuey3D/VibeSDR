// test-web-band-skim.mjs — V12 beta92 THE BAND SKIMMER in the web client, through a real vibeserver and the REAL built
// web page (Stuart, 2026-11-23, on the Lenovo RSP1A). A fake rtl_tcp replays the CW bench's synthetic HF clip (`cw_bench
// clip`: five CW stations around 7.030 MHz, each "CQ CQ DE <call> <call> K"); the page starts BAND on 40 m. Headless Edge:
//   1. the key says BAND SKIMMER (never DIGI / DIGITAL), the box title "BAND SKIMMER · 40 M";
//   2. a CW signal heard before its callsign is read is a row with its FREQUENCY and "waiting for callsign" (a
//      MutationObserver keeps whatever the list ever drew — the moment passes once a call is read);
//   3. the stations are spotted as CW rows; EXPANDED, a CW row reads its message ("CQ CQ DE G4ABC …"), and EXPLAIN / RAW
//      (the CW box's key, in this box's header too) reads it in plain English and back;
//   4. the waterfall MARKS the CW signals (the label layer: src 'skim', by callsign, CW's teal);
//   5. the VFO is drawn to the skimmer's width: Band's span widened to 40 m's CW window (7000 kHz, from band_skim);
//   6. at PHONE WIDTH (390 px) with EXPAND on, nothing scrolls sideways; no exception on the page.
// An OLDER server's frames (none, or fields missing) are scripts/test_band_skim.ts; test-web-spots runs Band on 20 m.
// ★ SILENT: headless Edge with --mute-audio and --disable-audio-output.
//   VIBESERVER_BIN=…/vibeserver VIBE_CW_CLIP=…/clip.cu8 node scripts/test-web-band-skim.mjs
//   (make the clip: scripts/test-cw.sh clip clip.cu8)
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
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_CW_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_CW_CLIP)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }
const RATE = 960000, CENTRE = 7030000;
// ★ The clip's stations inside 40 m's Region 1 CW window (7000–7040): tools/cw-bench/cw_bench.cpp clip()
const CALLS = ['G4ABC', 'DL1XYZ', 'EA3QP', 'K1ABC'];

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webbskim-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Band skimmer web test', locator: 'IO92nh', adminPass: 'wbs-admin',
  radios: [{ serial: 'wbs', label: 'Test radio', enabled: true, configured: true }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--hf', CLIP, '--hf-centre', String(CENTRE)], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(CENTRE), '--mode', 'usb', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webbskim-edge-'));
process.on('exit', () => { try { fs.rmSync(prof, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
let edge = null;
const watchdog = setTimeout(() => { console.log('   FAIL driver: no answer within 300 s');
  for (const c of [edge, srv, rtl]) { try { c?.kill('SIGKILL'); } catch { /* gone */ } }
  process.exit(1); }, 300000);
const shots = [];
try {
  for (let i = 0; i < 150; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  const st = await fetch(`${H}/vibeserver.json`).then((r) => r.json());
  ok(st.spotsBand === true && Array.isArray(st.spotBands) && st.spotBands.some((b) => b.id === '40m'), `vibeserver.json: Band offered with 40 m (${JSON.stringify(st.spotsBand)})`);

  edge = spawn(EDGE, ['--headless=new', `--remote-debugging-port=${cdp}`, '--no-first-run', '--no-default-browser-check', '--mute-audio',
    '--disable-audio-output', `--user-data-dir=${prof}`, '--window-size=1280,800', 'about:blank'], { stdio: 'ignore' });
  let page = null;
  for (let i = 0; i < 50 && !page; i++) { await sleep(200); page = await fetch(`http://127.0.0.1:${cdp}/json/list`).then((r) => r.json()).then((l) => l.find((t) => t.type === 'page')).catch(() => null); }
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  let id = 0; const pend = new Map(); const errs = [];
  ws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); }
                          if (m.method === 'Runtime.exceptionThrown') errs.push(m.params.exceptionDetails?.exception?.description || 'exception'); };
  await new Promise((r) => { ws.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => (await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true })).result?.result?.value;
  const until = async (x, ms) => { for (let t = 0; t < ms; t += 250) { if (await ev(x)) return true; await sleep(250); } return !!(await ev(x)); };
  const shot = async (name) => {
    const r = await send('Page.captureScreenshot', { format: 'png' });
    const p = path.join(SHOTS, name);
    fs.writeFileSync(p, Buffer.from(r.result.data, 'base64'));
    shots.push(p);
    return p;
  };
  const trustedClick = async (sel) => {
    const r = JSON.parse(await ev(`JSON.stringify(document.querySelector(${JSON.stringify(sel)})?.getBoundingClientRect() || {x:0,y:0,width:0,height:0})`));
    const x = r.x + r.width / 2, y = r.y + r.height / 2;
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
  };
  await send('Page.enable'); await send('Runtime.enable');
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await send('Page.navigate', { url: `${H}/index.html?join` });
  await sleep(4000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  await until(`!!document.querySelector('#audioGate button')`, 15000);
  await trustedClick('#audioGate button');   // ★ a trusted click starts audio (silent: --mute-audio)
  ok(await until(`!document.getElementById('audioGate') || document.getElementById('audioGate').style.display === 'none' || !document.getElementById('audioGate').isConnected`, 8000),
     'START RADIO pressed (the splash is gone)');
  await sleep(2500);

  console.log('── 1. the name ──');
  ok(await until(`!document.getElementById('spotsBandBtn').hidden`, 8000), 'the band skimmer\'s key is offered');
  const key = await ev(`document.getElementById('spotsBandBtn').textContent.trim()`);
  ok(/BAND SKIM/.test(key) && !/DIGI/.test(key), `its key: "${key}"`);
  ok(/BAND SKIMMER/.test(await ev(`document.getElementById('spotsBandBtn').title`)), 'its tooltip names the BAND SKIMMER (and its CW and RTTY)');
  // ★ a MutationObserver keeps every "waiting for callsign" row the list ever draws (the moment passes once a call is read)
  await ev(`(() => { window.__skw = []; new MutationObserver(() => { for (const r of document.querySelectorAll('#spotList .sres.spot.skimwait'))
                       window.__skw.push({ f: r.querySelector('.t')?.textContent || '', c: r.querySelector('.call')?.textContent || '', m: r.querySelector('.mode')?.textContent || '' }); })
                       .observe(document.getElementById('spotList'), { childList: true, subtree: true }); return 1; })()`);
  await ev(`document.getElementById('spotsBandBtn').click()`);
  ok(await until(`document.getElementById('spotsBandBtn').classList.contains('on')`, 3000), 'BAND SKIMMER lit');
  // ★ onto 40 m (the band this clip is on): the arrows step bands — tune 40 m's place directly if Band landed elsewhere
  await until(`/40 M/.test(document.getElementById('decTitle').textContent)`, 4000);
  if (!/40 M/.test(await ev(`document.getElementById('decTitle').textContent`))) {
    for (let i = 0; i < 6 && !/40 M/.test(await ev(`document.getElementById('decTitle').textContent`)); i++) { await trustedClick('#mVfoDown'); await sleep(900); }
  }
  ok(await until(`document.getElementById('decTitle').textContent === 'BAND SKIMMER \\u00b7 40 M'`, 4000), `the box title: "${await ev(`document.getElementById('decTitle').textContent`)}"`);

  console.log('── 2. "waiting for callsign", then the spots ──');
  const spotted = `(() => { const t = [...document.querySelectorAll('#spotList .sres.spot:not(.skimwait)')].filter(r => /CW/.test(r.querySelector('.mode')?.textContent || '')).map(r => r.querySelector('.call').textContent).join('|');
                            return ${JSON.stringify(CALLS)}.filter(c => t.includes(c)).length >= 2; })()`;
  ok(await until(spotted, 150000), `CW stations spotted as rows (${await ev(`[...document.querySelectorAll('#spotList .sres.spot:not(.skimwait)')].map(r => r.querySelector('.mode').textContent + ' ' + r.querySelector('.call').textContent).join(', ')`)})`);
  const skw = JSON.parse(await ev(`JSON.stringify(window.__skw.slice(0, 400))`));
  const w0 = skw.find((x) => /^CW$/.test(x.m.replace(/^./, '')) && /^70\d\d\.\d\d$/.test(x.f));
  ok(!!w0 && w0.c === 'waiting for callsign', `a CW signal with no call yet was a row: its frequency and "waiting for callsign" (${w0 ? `${w0.f} kHz · ${w0.c}` : `none of ${skw.length} seen`})`);

  console.log('── 3. EXPAND reads the CW; EXPLAIN / RAW ──');
  await ev(`(() => { const b = document.getElementById('sfExpand'); if (b.textContent.trim() === 'EXPAND') b.click(); return 1; })()`);
  ok(await until(`[...document.querySelectorAll('#spotList .sres.spot .sskim')].some(s => /CQ CQ DE (${CALLS.join('|')})/.test(s.textContent))`, 8000),
     `expanded, a CW row reads its message: "${await ev(`([...document.querySelectorAll('#spotList .sres.spot .sskim')].find(s => /DE/.test(s.textContent)) || {}).textContent || ''`)}"`);
  ok(await ev(`[...document.querySelectorAll('#spotList .sres.spot .sskim')].every(s => getComputedStyle(s).whiteSpace !== 'nowrap')`), 'the message WRAPS (to be read, not scanned)');
  // ★ never two rows for one station: a "waiting" row never sits on a CW station already spotted (150 Hz)
  const dup = await ev(`(() => { const sp = [...document.querySelectorAll('#spotList .sres.spot:not(.skimwait)')].filter(r => /CW/.test(r.querySelector('.mode').textContent)).map(r => r.title.match(/([0-9.]+) MHz/)).filter(Boolean).map(m => parseFloat(m[1]) * 1e6);
    const w = [...document.querySelectorAll('#spotList .sres.spot.skimwait')].filter(r => /CW/.test(r.querySelector('.mode').textContent)).map(r => parseFloat(r.querySelector('.t').textContent) * 1e3);
    return w.filter(h => sp.some(s => Math.abs(s - h) <= 1000)).length; })()`);
  ok(dup === 0, `no "waiting for callsign" row duplicates a spotted station (${dup})`);
  ok(await ev(`getComputedStyle(document.getElementById('cwPlain')).display !== 'none'`), 'EXPLAIN is in the band skimmer\'s header');
  const plainWas = await ev(`(() => { try { return localStorage.getItem('vibe.cwPlain'); } catch { return null; } })()`);
  if (await ev(`document.getElementById('cwPlain').textContent`) === 'RAW') await ev(`document.getElementById('cwPlain').click()`);
  await ev(`document.getElementById('cwPlain').click()`);
  ok(await until(`[...document.querySelectorAll('#spotList .sres.spot .sskim')].some(s => /calling anyone this is/.test(s.textContent))`, 4000),
     `EXPLAIN: the CW in plain English ("${await ev(`([...document.querySelectorAll('#spotList .sres.spot .sskim')].find(s => /this is/.test(s.textContent)) || {}).textContent || ''`)}")`);
  ok(await ev(`document.getElementById('cwPlain').textContent`) === 'RAW', '…and the key now says RAW');
  await ev(`document.getElementById('cwPlain').click()`);
  ok(await until(`[...document.querySelectorAll('#spotList .sres.spot .sskim')].some(s => /CQ CQ DE/.test(s.textContent))`, 4000), 'RAW: the CW as sent again');
  if (plainWas !== null) await ev(`(() => { try { localStorage.setItem('vibe.cwPlain', ${JSON.stringify(plainWas)}); } catch {} return 1; })()`);
  await shot('band-skim-expanded-desktop.png');

  console.log('── 4. the waterfall marks ──');
  const marks = JSON.parse(await ev(`JSON.stringify(((window.__vibeWf() || {}).wf?.knownSignals || []).filter(s => s.src === 'skim').map(s => ({ hz: s.hz, label: s.label, mode: s.skim.mode })))`));
  ok(marks.length >= 2 && marks.every((m) => m.mode === 'CW' || m.mode === 'RTTY'), `CW signals marked on the waterfall (${marks.map((m) => m.label + '@' + (m.hz / 1e3).toFixed(1)).join(', ')})`);
  ok(marks.some((m) => CALLS.includes(m.label)), '…labelled with their callsigns');
  ok(marks.every((m) => m.hz >= 7000000 && m.hz <= 7050000), '…at their own frequencies, inside the CW / RTTY windows');
  // ★ drawn: the waterfall laid the marks out (its placed labels include one)
  ok(await until(`(() => { const w = window.__vibeWf()?.wf; return !!w && (w.knownPlaced || []).some(p => p.sig && p.sig.src === 'skim'); })()`, 4000), '…and the waterfall placed at least one');

  console.log('── 5. the VFO shows the skimmer\'s width ──');
  const vfo = JSON.parse(await ev(`JSON.stringify((() => { const v = window.__vibeWf(); return v ? { f: v.spec.frequency, lo: v.wf.filterLow, hi: v.wf.filterHigh } : null; })())`));
  ok(!!vfo && vfo.f + vfo.lo <= 7000000 + 1, `the Band VFO reaches down to 40 m's CW window, 7000 kHz (drawn ${vfo && ((vfo.f + vfo.lo) / 1e3).toFixed(1)}–${vfo && ((vfo.f + vfo.hi) / 1e3).toFixed(1)} kHz)`);
  ok(!!vfo && vfo.f + vfo.hi >= 7074000, '…and still holds the band\'s digital dials (FT8 7.074)');

  console.log('── 6. phone width ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1500);
  const sw = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth, w: window.innerWidth,
    rowMax: Math.max(0, ...[...document.querySelectorAll('#spotList .sres.spot')].map(r => r.getBoundingClientRect().right)) })`));
  ok(sw.doc <= sw.w + 1 && sw.rowMax <= sw.w + 1, `nothing scrolls sideways at 390 px (page ${sw.doc}, rows to ${Math.round(sw.rowMax)})`);
  await shot('band-skim-expanded-390.png');
  ok(errs.length === 0, `no exception on the page${errs.length ? ': ' + errs.slice(0, 2).join(' | ') : ''}`);
  console.log(`   screenshots: ${shots.join('  ')}`);
} finally {
  clearTimeout(watchdog);
  try { edge?.kill('SIGTERM'); } catch {}
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
}
console.log(`\n${fail ? '✗' : '✓'} test-web-band-skim: ${pass} passed, ${fail} failed`);
if (!fail) try { fs.rmSync(dir, { recursive: true, force: true }); } catch {}
process.exit(fail ? 1 : 0);
