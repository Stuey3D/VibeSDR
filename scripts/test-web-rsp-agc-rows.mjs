// test-web-rsp-agc-rows.mjs — the web page's RADIO section on an SDRplay: the AGC row and what every AGC row says, per
// mode (the radio menu's AGC choice, 2026-10-30). Stuart, on his unlocked RSP1A with VibeAGC running: "i assume this isnt
// vibeagc" / "no option for it here either" — IF AGC AUTO read as the SDRplay's own, and AGC TARGET said "−30 · not in
// use" while VibeAGC held −20.
//
// ★ No RSP is needed: the REAL server runs on the fake rtl_tcp, and a shim in the page dresses its hwinfo as an RSP1A
//   (radio.vagcMode / vagcAuto) and feeds rspstat as a server running — or not running — VibeAGC would. The admin
//   unlock is the page's own (this test's throwaway password), and the AGC keys go to the real server, which logs them.
//   Pictures land in the out dir (argv[2]); the checks are the gate.
//     VIBESERVER_BIN=…/vibeserver node scripts/test-web-rsp-agc-rows.mjs <out-dir>
// ★ SILENT: headless Edge with --mute-audio.
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const OUT = process.argv[2] || fs.mkdtempSync(path.join(os.tmpdir(), 'vs-agcrows-out-'));
if (!BIN || !fs.existsSync(BIN) || !fs.existsSync(EDGE)) { console.log('   not run — VIBESERVER_BIN and Edge needed'); process.exit(3); }
fs.mkdirSync(OUT, { recursive: true });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const ADMIN = 'agcrows-web-admin';
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-agcrows-'));
for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'AGC rows', adminPass: ADMIN,
  radios: [{ serial: 'agcrows-web', label: 'Test radio', enabled: true, configured: true, users: 3 }] }));
const LOG = path.join(dir, 'server.log');
const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
delete env.VIBE_RSP_VIBEAGC;
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', '3'],
                  { env, stdio: ['ignore', fs.openSync(LOG, 'w'), fs.openSync(LOG, 'a')] });
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run',
  '--no-default-browser-check', `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=520,1200', 'about:blank'], { stdio: 'ignore' });

/* ★ THE SHIM: hwinfo's `radio` becomes an RSP1A with the AGC choice; rspstat arrives 5×/s as window.__vagc says. A key
 *  press is seen on its way out (lastSent) and becomes the choice, as the server's re-sent hwinfo would make it. */
const SHIM = `(() => {
  const V = window.__vagc = { mode: 0, auto: true, running: 1, vtgt: -20, lastSent: null, socks: [], hw: 0 };
  const radio = () => ({ driver: 'sdrplay', model: 'RSP1A', lnaStates: 10, ifGrMin: 20, ifGrMax: 59, agcSetPoint: true,
    ranges: [[1000, 2000000000]], rfNotch: true, dabNotch: true, biasT: true, biasTOn: false, antennas: [], antennaLocked: false,
    hdr: false, amNotch: false, extRef: false, vagcMode: V.mode, vagcAuto: V.auto });
  const N = window.WebSocket;
  const dress = (ev) => {
    if (typeof ev.data !== 'string' || ev.data[0] !== '{') return ev;
    let m; try { m = JSON.parse(ev.data); } catch { return ev; }
    if (m.type !== 'hwinfo') return ev;
    V.hw++;
    m.radio = radio(); m.gains = []; m.agcLocked = false; m.gainLocked = false;
    return new MessageEvent('message', { data: JSON.stringify(m) });
  };
  class W extends N {
    constructor(u, p) {
      super(u, p);
      this.__spec = String(u).includes('spectrum');
      if (this.__spec) V.socks.push(this);
    }
    set onmessage(fn) { this.__h = fn; super.onmessage = fn ? (ev) => fn.call(this, this.__spec ? dress(ev) : ev) : null; }
    get onmessage() { return this.__h; }
    addEventListener(t, fn, o) {
      if (t === 'message' && this.__spec) return super.addEventListener(t, (ev) => fn.call(this, dress(ev)), o);
      return super.addEventListener(t, fn, o);
    }
    send(d) {
      if (typeof d === 'string' && d.includes('vagcmode')) { try { const m = JSON.parse(d); V.lastSent = m.vagcmode; V.mode = m.vagcmode; } catch {} }
      return super.send(d);
    }
  }
  window.WebSocket = W;
  setInterval(() => {
    const st = { type: 'rspstat', sysGain: 18.9, lna: 4, ifgr: 31, overload: 0, settling: 0, rfNotch: 0, dabNotch: 0, autoNotch: 0,
      userNotch: 1, rfAgc: 1, agcSet: -30, adcPeak: -21.4, adcClip: 0, lnaN: 7, agcReinit: 0, agcInit: 0, gainStuck: 0,
      vagc: V.running, vst: V.running ? 2 : 0, vrs: 0, vtgt: V.vtgt, vwpm: 12, vmiss: 0, vblind: 0, vrail: 0 };
    for (const s of V.socks) if (s.readyState === 1 && s.__h) s.__h.call(s, new MessageEvent('message', { data: JSON.stringify(st) }));
  }, 200);
})();`;

let code = 0;
const deadline = setTimeout(() => { console.log('   deadline'); try { edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } catch {} process.exit(1); }, 150000);
const browser = async () => {
  const ver = await (await fetch(`http://127.0.0.1:${dbg}/json/version`)).json();
  const bw = new WebSocket(ver.webSocketDebuggerUrl);
  await new Promise((r) => { bw.onopen = r; });
  let bid = 0; const bp = new Map();
  bw.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && bp.has(m.id)) { bp.get(m.id)(m); bp.delete(m.id); } };
  const bsend = (method, params = {}) => new Promise((res) => { const i = ++bid; bp.set(i, res); bw.send(JSON.stringify({ id: i, method, params })); });
  return { bw, bsend };
};
const drive = async (wsUrl, initMode = 0) => {
  const c = new WebSocket(wsUrl);
  let id = 0; const pend = new Map();
  c.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  await new Promise((r) => { c.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); c.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.text; };
  const until = async (x, ms = 5000) => { for (let i = 0; i < ms / 100; i++) { if (await ev(x)) return true; await sleep(100); } return false; };
  // ★ The element's own click — an overlay (a notice, the first-visit card) must not decide which control a test presses.
  const click = (sel) => ev(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return false; e.click(); return true; })()`);
  await send('Page.enable'); await send('Runtime.enable');
  await send('Emulation.setDeviceMetricsOverride', { width: 520, height: 1200, deviceScaleFactor: 1, mobile: false });
  await send('Page.addScriptToEvaluateOnNewDocument', { source: SHIM.replace('{ mode: 0,', `{ mode: ${initMode},`) });
  await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  await until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 15000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim())); b && b.click(); return !!b; })()`);
  // ★ The audio gate needs a REAL gesture (the browser's autoplay rule), so this one is a mouse press, not .click().
  if (await until(`!!document.querySelector('#audioGate button')`, 8000)) {
    const r = JSON.parse(await ev(`JSON.stringify(document.querySelector('#audioGate button').getBoundingClientRect())`));
    const x = r.x + r.width / 2, y = r.y + r.height / 2;
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
  }
  await sleep(1500);
  await until(`!document.getElementById('rspCtls')?.hidden`, 15000);
  await click('#mMenu');
  await sleep(600);
  const shot = async (name) => {
    await ev(`document.getElementById('rspSysGain').scrollIntoView({ block: 'start' })`);
    await sleep(300);
    const q = JSON.parse(await ev(`JSON.stringify((() => { const a = document.getElementById('rspSysGain').closest('.mrow').getBoundingClientRect();
      const b = (document.getElementById('adminRow').hidden ? document.getElementById('rowRspNotch') : document.getElementById('adminRow')).getBoundingClientRect();
      const m = document.getElementById('menu').getBoundingClientRect();
      return { x: m.x, y: Math.max(0, a.y - 34), w: m.width, h: Math.min(1100, b.bottom - a.y + 44) }; })())`));
    const sh = await send('Page.captureScreenshot', { format: 'png', clip: { x: q.x, y: q.y, width: q.w, height: q.h, scale: 1 } });
    const file = path.join(OUT, `${name}.png`);
    fs.writeFileSync(file, Buffer.from(sh.result.data, 'base64'));
    console.log(`   wrote ${file}`);
  };
  const txt = (id) => ev(`(document.getElementById('${id}')?.textContent || '')`);
  return { c, send, ev, until, click, shot, txt };
};

try {
  await sleep(2500);
  const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
  const A = await drive(list.find((t) => t.type === 'page').webSocketDebuggerUrl);

  console.log('── the owner, unlocked: VibeAGC running under AUTO ──');
  await A.click('#adminUnlock');
  await A.until(`!document.getElementById('adminPwRow').hidden`);
  await A.ev(`(() => { const i = document.getElementById('adminPwInput'); i.value = ${JSON.stringify(ADMIN)}; return true; })()`);
  await A.click('#adminPwGo');
  ok(await A.until(`document.getElementById('adminUnlock').textContent === 'UNLOCKED'`, 6000), 'unlocked through the page');
  await A.until(`(document.getElementById('rspIfAgcBy')?.textContent || '') === ' · VIBEAGC'`, 4000);
  ok(await A.ev(`!document.getElementById('rowRspVagc').hidden && document.getElementById('rspVagcSeg').style.display !== 'none'`), 'AGC row with its keys');
  ok(await A.ev(`[...document.querySelectorAll('#rspVagcSeg button')].map(b => b.textContent + (b.classList.contains('on') ? '*' : '')).join(' ')`) === 'AUTO* VIBEAGC SDRPLAY',
     'AUTO · VIBEAGC · SDRPLAY, AUTO lit');
  ok(await A.txt('rspVagcVal') === '→ VibeAGC (OpenRSP driver)', `says what AUTO resolved to: "${await A.txt('rspVagcVal')}"`);
  ok(await A.txt('rspIfAgcBy') === ' · VIBEAGC', `IF AGC · VIBEAGC ("${await A.txt('rspIfAgcBy')}")`);
  ok(await A.txt('rspRfAgcBy') === ' · VIBEAGC', `RF AGC · VIBEAGC ("${await A.txt('rspRfAgcBy')}")`);
  ok((await A.txt('rspIfGrVal')).endsWith('· VibeAGC'), `IF readout names VibeAGC ("${await A.txt('rspIfGrVal')}")`);
  ok(await A.txt('rspAgcSetVal') === '−20 dBFS · VibeAGC' && await A.txt('rspAgcSetBy') === ' · VIBEAGC',
     `★ AGC TARGET · VIBEAGC is VibeAGC's real target, read-only ("${await A.txt('rspAgcSetVal')}")`);
  ok(/set per band by VibeAGC/.test(await A.ev(`document.getElementById('rspAgcSet').title`)), '…and its title says VibeAGC sets it');
  ok(await A.ev(`document.getElementById('rspAgcSet').classList.contains('agc') && Number(document.getElementById('rspAgcSet').value) === -20`),
     '…the slider sits at −20, greyed');
  ok(await A.ev(`!document.getElementById('rspIfAgc').classList.contains('agc')`), '★ the IF AGC AUTO/MANUAL key stays live under VibeAGC');
  await A.shot('1-owner-auto-vibeagc');

  console.log('── the owner chooses SDRPLAY: sent, and the rows follow the hand-back ──');
  await A.click('#rspVagcSeg button[data-vagc="2"]');
  ok(await A.until(`window.__vagc.lastSent === 2`, 2000), 'rsp_control vagcmode 2 sent');
  ok(await (async () => { for (let i = 0; i < 30; i++) { if (/the owner chose the SDRplay's own IF AGC/.test(fs.readFileSync(LOG, 'utf8'))) return true; await sleep(100); } return false; })(),
     '…and the real server took it from the unlocked owner (log)');
  await A.ev(`window.__vagc.running = 0`);
  await A.until(`(document.getElementById('rspIfAgcBy')?.textContent || '') === ' · SDRPLAY'`, 4000);
  ok(await A.ev(`document.querySelector('#rspVagcSeg button[data-vagc="2"]').classList.contains('on')`), 'SDRPLAY lit');
  ok(await A.txt('rspVagcVal') === '', 'no AUTO note when AUTO is not chosen');
  ok(await A.txt('rspIfAgcBy') === ' · SDRPLAY', `IF AGC · SDRPLAY ("${await A.txt('rspIfAgcBy')}")`);
  ok(await A.txt('rspRfAgcBy') === '', 'RF AGC: no VibeAGC tag (our walker under the SDRplay\'s AGC)');
  ok(await A.txt('rspAgcSetBy') === '', 'AGC TARGET: no VibeAGC tag');
  ok((await A.txt('rspIfGrVal')).endsWith('· IF AGC'), `IF readout: "${await A.txt('rspIfGrVal')}"`);
  ok(await A.until(`/^-30 dBfs/.test(document.getElementById('rspAgcSetVal').textContent)`, 3000) && await A.ev(`!document.getElementById('rspAgcSet').classList.contains('agc')`),
     `AGC TARGET is the SDRplay's set point again, and live ("${await A.txt('rspAgcSetVal')}")`);
  await A.shot('2-owner-sdrplay');

  console.log('── the owner chooses VIBEAGC ──');
  await A.click('#rspVagcSeg button[data-vagc="1"]');
  await A.ev(`window.__vagc.running = 1`);
  ok(await A.until(`window.__vagc.lastSent === 1 && document.querySelector('#rspVagcSeg button[data-vagc="1"]').classList.contains('on')`, 3000), 'VIBEAGC sent and lit');
  ok(await A.until(`document.getElementById('rspAgcSetVal').textContent === '−20 dBFS · VibeAGC'`, 3000), 'AGC TARGET back to VibeAGC\'s');

  console.log('── a listener (not unlocked): the choice in words, no keys ──');
  const { bsend } = await browser();
  const ctx = (await bsend('Target.createBrowserContext')).result.browserContextId;
  const tid = (await bsend('Target.createTarget', { url: 'about:blank', browserContextId: ctx })).result.targetId;
  /* ★ The owner chose VIBEAGC above. On an RSP the server's re-sent hwinfo carries that to everybody; the fake radio's
   *  hwinfo has no vagcMode, so the server sees nothing new to send this listener — its page starts dressed as mode 1. */
  const B = await drive(`ws://127.0.0.1:${dbg}/devtools/page/${tid}`, 1);
  await B.until(`!document.getElementById('rowRspVagc').hidden && document.getElementById('rspVagcVal').textContent === 'VibeAGC'`, 6000);
  ok(await B.ev(`!document.getElementById('rowRspVagc').hidden`), 'the AGC row is there for a listener');
  ok(await B.ev(`document.getElementById('rspVagcSeg').style.display === 'none'`), '★ …without the keys (the server would refuse them)');
  ok(await B.txt('rspVagcVal') === 'VibeAGC', `…the choice in words ("${await B.txt('rspVagcVal')}")`);
  ok(await B.txt('rspIfAgcBy') === ' · VIBEAGC', 'IF AGC · VIBEAGC for the listener too');
  await B.shot('3-listener-locked');
} catch (e) { console.log(`   driver: ${e && e.stack || e}`); code = 1; }
finally {
  clearTimeout(deadline);
  edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(600);
  fs.rmSync(dir, { recursive: true, force: true });
}
console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(code || fail ? 1 : 0);
