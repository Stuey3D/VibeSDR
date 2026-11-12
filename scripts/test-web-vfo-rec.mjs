// test-web-vfo-rec.mjs — V12 PER-VFO RECORDING in the WEB client against the REAL server (Stuart, 2026-11-07: "a 2nd
// record line in the audio menu for the B VFO, both can be triggered independently and both have their own clocks and the
// signal is recorded as it should be before we push it to one ear"). Headless Edge, --mute-audio (tests are silent):
//   • VFO B off: one REC, no REC B row, nothing per-VFO asked of the server
//   • VFO B on (A 6.91 MHz = 400 Hz tone, B 7.03 MHz = 620 Hz): REC reads REC A and the REC B row appears
//   • REC A, a moment later REC B: each its own clock (A's ahead of B's), the server asked for recA then recB
//   • STOP A then STOP B: two recordings, _VFO-A and _VFO-B, each MONO (AM), A's holding 400 Hz and none of B's 620 Hz,
//     B's the reverse — recorded before the pan, not one ear of the mix
//   • with the REC BUFFER on, the streams are asked for while A/B is on (the pre-roll), and a recording starts with it
//   • VFO B off while REC B runs: the recording is kept, the row goes, the server is told to stop
// Screenshots to $VFOAB_SHOTS (default $TMPDIR) as webrec-*.png.
//   VIBESERVER_BIN=…/vibeserver node scripts/test-web-vfo-rec.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !fs.existsSync(EDGE)) { console.log('   not run — VIBESERVER_BIN and Edge needed'); process.exit(3); }
const SHOTS = process.env.VFOAB_SHOTS || os.tmpdir();
fs.mkdirSync(SHOTS, { recursive: true });
const CENTRE = 7000000;
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

// ★ Every message the page sends on any socket, recorded before the page's own scripts run.
const SNIFF = `(() => { window.__sent = []; const o = WebSocket.prototype.send;
  WebSocket.prototype.send = function (d) { try { if (typeof d === 'string') window.__sent.push(d); } catch {} return o.apply(this, arguments); }; })();`;

async function session(tag, radioExtra, run) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-webrec-${tag}-`));
  // ★ The browser profile (~0.5 GB) never outlives the run — a failed or timed-out run keeps its logs, not Edge's cache
  //   (53 kept waterfall-history runs held 25 GB of /var/folders, 2026-10-31).
  process.on('exit', () => { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
  for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'VFO A/B web test', locator: 'IO92nh', adminPass: 'vfoab-web-admin',
    radios: [{ serial: `${tag}-web`, label: 'Test radio', enabled: true, configured: true, users: 1, ...radioExtra }] }));
  const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--tones', '3', '--tones-at', String(CENTRE + 30000)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', String(CENTRE - 90000), '--mode', 'am', '--users', '1'],
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
    const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(SHOTS, `webrec-${name}.png`); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
    const until = async (x, ms) => { for (let i = 0; i < ms / 200; i++) { if (await ev(x)) return true; await sleep(200); } return false; };
    const click = async (sel) => {
      // ★ Scrolled into view first ('nearest' leaves a visible one alone): the A/B BALANCE row pushed VFO STEREO below the
      //   audio panel's fold, and a click at off-screen coordinates hits whatever is there instead.
      const r = JSON.parse(await ev(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return 'null';
        e.scrollIntoView({ block: 'nearest' }); return JSON.stringify(e.getBoundingClientRect()); })()`) || 'null');
      if (!r) return false;
      const x = r.x + r.width / 2, y = r.y + r.height / 2;
      await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
      await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
      return true;
    };
    await send('Page.enable'); await send('Runtime.enable');
    await send('Page.addScriptToEvaluateOnNewDocument', { source: SNIFF });
    await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
    await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
    await until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 15000);
    await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim())); b && b.click(); return !!b; })()`);
    await until(`!!document.querySelector('#audioGate button')`, 15000);
    await click('#audioGate button');
    ok(await until(`!document.getElementById('audioGate') || document.getElementById('audioGate').style.display === 'none' || !document.getElementById('audioGate').isConnected`, 8000), `${tag}: START RADIO pressed`);
    await sleep(2500);
    await run({ ev, send, shot, until, click });
  } catch (e) { ok(false, `${tag} driver: ${e && e.stack || e}`); }
  finally {
    try { wsC?.close(); } catch {}
    edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM');
    await sleep(600);
    if (fail === failBefore) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
  }
}
const sentWatch = `window.__sent.filter(s => s.includes('"type":"watch"')).map(s => JSON.parse(s))`;
// ★ Every recording in the page's store, each WAV read back: channels, seconds, and the two tones' strength (Goertzel).
const RECS = `(async () => {
  const db = await new Promise((r, j) => { const q = indexedDB.open('vibesdr', 1); q.onsuccess = () => r(q.result); q.onerror = () => j(q.error); });
  const all = await new Promise((r) => { const t = db.transaction('recordings').objectStore('recordings').getAll(); t.onsuccess = () => r(t.result); });
  const g = (x, f) => { const w = 2 * Math.PI * f / 48000, cw = 2 * Math.cos(w); let s1 = 0, s2 = 0; for (const v of x) { const s = v + cw * s1 - s2; s2 = s1; s1 = s; } return Math.sqrt(Math.max(0, s1 * s1 + s2 * s2 - cw * s1 * s2)) / Math.max(1, x.length); };
  const out = [];
  for (const r of all) {
    const b = new DataView(await r.blob.arrayBuffer());
    const ch = b.getUint16(22, true), n = (b.byteLength - 44) / 2 / ch;
    const x = []; for (let i = Math.max(0, n - 48000); i < n; i++) x.push(b.getInt16(44 + i * ch * 2, true) / 32768);
    out.push({ name: r.name, ch, sec: n / 48000, t400: g(x, 400), t620: g(x, 620) });
  }
  return JSON.stringify(out);
})()`;
const B_HZ = CENTRE + 30000;   // 620 Hz

console.log('── per-VFO recording in the web client ──');
await session('rec', {}, async ({ ev, shot, until, click }) => {
  const S = `window.__vibeWf().spec`;
  ok(await until(`window.__vibeAB && ${'window.__vibeAB()'}?.max === 1`, 8000), 'hwinfo offers VFO A/B');
  await click('#mAudio'); await sleep(500);
  await click('[data-rb="0"]'); await sleep(200);   // ★ the REC BUFFER off first (its default is 30 s) — tested at the end
  ok(await ev(`document.getElementById('mRecBRow').hidden && getComputedStyle(document.getElementById('mRecBRow')).display === 'none' && document.getElementById('mRecLbl').textContent === 'REC'`), 'B off: one REC, no REC B row');
  await ev(`${S}.setAB(true), 0`);
  ok(await until(`${S}.abOn && ${S}.watchRecVfo`, 4000), 'B on, and the server offers per-VFO recording');
  await ev(`${S}.tuneVfo('B', ${B_HZ}, 'am'), 0`);
  await sleep(3000);   // B's AGC settles
  // ★ the audio panel still open from above — open it only if it is not (a second tap closes it)
  if (!(await ev(`!!document.getElementById('mPanelRec').closest('.open')`))) { await click('#mAudio'); await sleep(500); }
  ok(await until(`!document.getElementById('mRecBRow').hidden && document.getElementById('mRecLbl').textContent === 'REC A'`, 3000), 'B on: REC reads REC A and the REC B row is there');
  await shot('rows');
  await ev(`window.__sent.length = 0, 0`);
  const tA0 = Date.now();
  await click('#mPanelRec');
  ok(await until(`${sentWatch}.some(m => m.recA === true && m.recB === false)`, 2000), 'REC A asks the server for VFO A\'s own audio');
  await sleep(1500);
  const tB0 = Date.now();
  await click('#mPanelRecB');
  ok(await until(`${sentWatch}.some(m => m.recA === true && m.recB === true)`, 2000), 'REC B asks for B\'s too');
  await sleep(3000);
  const la = await ev(`document.getElementById('mRecLbl').textContent`), lb = await ev(`document.getElementById('mRecLblB').textContent`);
  const secs = (t) => { const m = /(\d\d):(\d\d)/.exec(t || ''); return m ? +m[1] * 60 + +m[2] : -1; };
  ok(/^STOP A/.test(la) && /^STOP B/.test(lb) && secs(la) > secs(lb), `each its own clock (${la} · ${lb})`);
  await shot('both');
  const tA1 = Date.now();
  await click('#mPanelRec');
  await sleep(800);
  ok(await until(`${sentWatch}.some(m => m.recA === false && m.recB === true)`, 2000), 'STOP A: A\'s stream stopped, B\'s carries on');
  ok(await ev(`/^STOP B/.test(document.getElementById('mRecLblB').textContent) && document.getElementById('mRecLbl').textContent === 'REC A'`), '…and B is still recording');
  await sleep(1000);
  const tB1 = Date.now();
  await click('#mPanelRecB');
  await sleep(1500);
  const recs = JSON.parse(await ev(RECS) || '[]');
  console.log('      ' + recs.map((r) => `${r.name} ${r.ch} ch ${r.sec.toFixed(1)} s 400:${r.t400.toFixed(4)} 620:${r.t620.toFixed(4)}`).join('\n      '));
  const A = recs.find((r) => /_VFO-A\.wav$/.test(r.name)), B = recs.find((r) => /_VFO-B\.wav$/.test(r.name));
  ok(!!A && !!B, 'two recordings, _VFO-A and _VFO-B');
  ok(A && A.ch === 1 && B && B.ch === 1, 'each MONO (AM) — its VFO as it is, not a stereo mix');
  ok(A && A.t400 > 0.002 && A.t400 > 30 * A.t620, 'VFO A\'s file: A\'s 400 Hz, none of B\'s 620 Hz');
  ok(B && B.t620 > 0.002 && B.t620 > 30 * B.t400, 'VFO B\'s file: B\'s 620 Hz, none of A\'s 400 Hz');
  const runA = (tA1 - tA0) / 1000, runB = (tB1 - tB0) / 1000;
  ok(A && B && Math.abs(A.sec - runA) < 0.8 && Math.abs(B.sec - runB) < 0.8,
     `each as long as its own run (A ${A && A.sec.toFixed(1)} s of ${runA.toFixed(1)}, B ${B && B.sec.toFixed(1)} s of ${runB.toFixed(1)})`);
  // ── the REC BUFFER: the streams flow while A/B is on, and a recording starts with what is held
  await click('[data-rb="30"]');
  ok(await until(`${sentWatch}.slice(-1)[0]?.recA === true && ${sentWatch}.slice(-1)[0]?.recB === true`, 2000), 'REC BUFFER on: both streams asked for while A/B is on');
  await sleep(4000);
  await click('#mPanelRecB');
  await sleep(1200);
  ok(/^STOP B 00:0[4-6]/.test(await ev(`document.getElementById('mRecLblB').textContent`)), `REC B starts with the buffered seconds (${await ev(`document.getElementById('mRecLblB').textContent`)})`);
  // ── B off mid-recording
  await ev(`${S}.setAB(false), 0`);
  await sleep(1500);
  ok(await ev(`document.getElementById('mRecBRow').hidden && getComputedStyle(document.getElementById('mRecBRow')).display === 'none' && document.getElementById('mRecLbl').textContent === 'REC'`), 'B off: the row goes, REC is plain REC again');
  ok(await until(`${sentWatch}.some(m => m.off === true)`, 1000), '…the server told B is off');
  const recs2 = JSON.parse(await ev(RECS) || '[]');
  ok(recs2.filter((r) => /_VFO-B\.wav$/.test(r.name)).length === 2, 'and the recording that was running is kept');
  await click('[data-rb="0"]');
});
console.log(`\n${fail ? 'FAILED' : 'all passed'} — ${pass} ok, ${fail} failed`);
process.exit(fail ? 1 : 0);
