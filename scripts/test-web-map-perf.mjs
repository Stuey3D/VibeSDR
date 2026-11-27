// test-web-map-perf.mjs <url> [WxH] [open|shut] [seconds] [tag] — THE MAP TEMPLATE'S PERF BUDGET (docs/v12/MAP-TEMPLATE.md):
// the AIR & SEA viewer left alone, real GPU (Metal): main-thread busy %, rAF/s, frames with WebGL draws/s, layouts/s,
// style recalcs/s, timers/s, requests/s, JS heap. Exits 1 over budget: busy > BUDGET_BUSY (6 %), GL frames > 30/s,
// or any styleimagemissing (window.__vsMissingImages). Muted; Edge killed in finally. ZOOM=n jumps to zoom n first.
import fs from 'node:fs'; import os from 'node:os'; import path from 'node:path'; import net from 'node:net'; import { spawn } from 'node:child_process';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const [URL0, SZ = '1440x900', LIST = 'open', SECS = '30', TAG = ''] = process.argv.slice(2);
const [W, H] = SZ.split('x').map(Number), mob = W < 600;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const port = await new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'mc-idle-'));
const e = spawn(EDGE, ['--headless=new', '--mute-audio', '--enable-gpu', '--use-angle=metal', `--remote-debugging-port=${port}`, `--user-data-dir=${dir}`, '--no-first-run', 'about:blank'], { stdio: 'ignore' });
const HOOK = `(() => {
  const C = window.__idle = { raf: 0, glFrames: 0, draws: 0, timers: 0, fetch: 0, ws: 0, mlRender: 0, rafBy: {} };
  let drewThisFrame = false;
  const oRaf = window.requestAnimationFrame.bind(window);
  window.requestAnimationFrame = (cb) => { const st = (new Error().stack || '').split('\\n').slice(2, 4).map(s => s.trim().replace(/https?:\\/\\/[^\\/]+/, '').slice(0, 80)).join(' < '); return oRaf((t) => { C.raf++; const k = (cb.name || 'anon') + ' ' + st; C.rafBy[k] = (C.rafBy[k] || 0) + 1; cb(t); }); };
  (function frameWatch() { oRaf(() => { if (drewThisFrame) C.glFrames++; drewThisFrame = false; frameWatch(); }); })();
  for (const P of [window.WebGLRenderingContext?.prototype, window.WebGL2RenderingContext?.prototype]) if (P) for (const fn of ['drawElements', 'drawArrays']) { const o = P[fn]; P[fn] = function (...a) { C.draws++; drewThisFrame = true; return o.apply(this, a); }; }
  const oST = window.setTimeout; window.setTimeout = function (f, ms, ...a) { return oST(typeof f === 'function' ? (...b) => { C.timers++; f(...b); } : f, ms, ...a); };
  const oSI = window.setInterval; window.setInterval = function (f, ms, ...a) { return oSI(typeof f === 'function' ? (...b) => { C.timers++; f(...b); } : f, ms, ...a); };
  const oF = window.fetch; window.fetch = function (...a) { C.fetch++; return oF.apply(this, a); };
})();`;
try {
  await sleep(2500);
  const l = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  const ws = new WebSocket(l.find((t) => t.type === 'page').webSocketDebuggerUrl);
  let id = 0; const pend = new Map(); let reqs = 0, wsFrames = 0;
  ws.onmessage = (m) => { const j = JSON.parse(m.data); if (j.id && pend.has(j.id)) { pend.get(j.id)(j); pend.delete(j.id); }
    if (j.method === 'Network.requestWillBeSent') reqs++; if (j.method === 'Network.webSocketFrameReceived') wsFrames++; };
  await new Promise((r) => { ws.onopen = r; });
  const send = (method, params = {}) => new Promise((r) => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.exception?.description; };
  await send('Runtime.enable'); await send('Performance.enable'); await send('Network.enable');
  await send('Page.enable'); await send('Page.addScriptToEvaluateOnNewDocument', { source: HOOK });
  await send('Emulation.setDeviceMetricsOverride', { width: W, height: H, deviceScaleFactor: mob ? 3 : 2, mobile: mob });
  if (mob) await send('Emulation.setTouchEmulationEnabled', { enabled: true, maxTouchPoints: 5 });
  await send('Page.navigate', { url: URL0 });
  for (let i = 0; i < 60 && !(await ev(`!!document.getElementById('splashAirSea') && !document.getElementById('splashAirSea').hidden`)); i++) await sleep(300);
  await ev(`document.getElementById('splashAirSea').click(); 1`);
  await sleep(10000);
  const V = `document.querySelector('.trkView:not([hidden])')`;
  // LIST to the state asked (the chip toggles; older pages: the card button / the list's minimise)
  await ev(`(() => { const b = ${V}.querySelector('[data-cbtn="list"]'); if (!b) return 'no chip'; const open = b.getAttribute('aria-expanded') === 'true'; if (open !== ${LIST === 'open'}) b.click(); return 1; })()`);
  if (process.env.ZOOM) console.log('zoom:', await ev(`(() => { const v = window.__vibeTrkView; const m = v.vm && (v.vm.map || v.vm._gl || v.vm.gl); if (!m || !m.jumpTo) return 'no map ' + Object.keys(v.vm || {}).join(','); const f = document.querySelector('.trkView:not([hidden]) .trkCard2[data-card="map"] .fit'); if (f && f.getAttribute('aria-pressed') !== 'false') f.click(); m.jumpTo({ center: [-0.2, 51.5], zoom: ${process.env.ZOOM} }); return 'ok ' + m.getZoom(); })()`));
  await sleep(4000);
  const n = await ev(`(() => { const t = ${V}.querySelector('.trkList .hd .tab[data-tab="aircraft"] .tn'); return t ? t.textContent : document.querySelectorAll('.trkRow').length; })()`);
  await ev(`(() => { const C = window.__idle; for (const k of Object.keys(C)) if (typeof C[k] === 'number') C[k] = 0; C.rafBy = {}; return 1; })()`);
  reqs = 0; wsFrames = 0;
  const m0 = Object.fromEntries((await send('Performance.getMetrics')).result.metrics.map((x) => [x.name, x.value]));
  const S = Number(SECS);
  await sleep(S * 1000);
  const m1 = Object.fromEntries((await send('Performance.getMetrics')).result.metrics.map((x) => [x.name, x.value]));
  const C = JSON.parse(await ev(`JSON.stringify(window.__idle)`));
  const d = (k) => m1[k] - m0[k];
  const top = Object.entries(C.rafBy).sort((a, b) => b[1] - a[1]).slice(0, 4).map(([k, v]) => `${(v / S).toFixed(1)}/s ${k}`);
  console.log(`${TAG} ${SZ} LIST ${LIST} (${n} aircraft) over ${S}s: main thread busy ${(d('TaskDuration') / S * 100).toFixed(1)}% · script ${(d('ScriptDuration') / S * 100).toFixed(1)}% · rAF ${(C.raf / S).toFixed(1)}/s · frames with GL draws ${(C.glFrames / S).toFixed(1)}/s (${(C.draws / S).toFixed(0)} draw calls/s) · layouts ${(d('LayoutCount') / S).toFixed(1)}/s · style ${(d('RecalcStyleCount') / S).toFixed(1)}/s · timers ${(C.timers / S).toFixed(1)}/s · requests ${(reqs / S).toFixed(2)}/s · ws frames ${(wsFrames / S).toFixed(1)}/s · JS heap ${Math.round(m1.JSHeapUsedSize / 1048576)} MB`);
  for (const t of top) console.log('   rAF by', t);
  const busy = d('TaskDuration') / S * 100, gl = C.glFrames / S, miss = JSON.parse(await ev(`JSON.stringify(window.__vsMissingImages || [])`));
  const budget = Number(process.env.BUDGET_BUSY || 6), fails = [];
  if (busy > budget) fails.push(`main thread ${busy.toFixed(1)} % > ${budget} %`);
  if (gl > 30) fails.push(`GL frames ${gl.toFixed(1)}/s > 30`);
  if (miss.length) fails.push(`style images missing: ${miss.join(', ')}`);
  console.log(fails.length ? 'FAIL ' + fails.join(' · ') : 'ok   within the map perf budget');
  process.exitCode = fails.length ? 1 : 0;
  ws.close();
} finally { e.kill('SIGKILL'); await sleep(300); fs.rmSync(dir, { recursive: true, force: true }); }
