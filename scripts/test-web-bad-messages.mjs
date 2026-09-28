/**
 * test-web-bad-messages.mjs — the web client must survive malformed traffic.
 *
 *   node scripts/build-web.mjs && node scripts/test-web-bad-messages.mjs [secs]
 *
 * Starts the mock VibeServer with --bad-messages (bad JSON, a bare null, truncated / bad-magic /
 * header-only SPEC frames, gzip garbage, wrong-typed rds / rdsx / hwinfo / health fields, and
 * truncated, short and unknown-format audio frames, interleaved with the good stream), opens the
 * REAL built page in headless Edge, joins, and checks over time that:
 *   1. the spectrum keeps arriving and the render loop keeps running (the status line's fps and
 *      KB/s keep updating — it is written from inside the rAF loop);
 *   2. audio bytes keep arriving (the audio half of the KB/s tooltip);
 *   3. every bad message was DROPPED AND COUNTED (window.__vibeFaults()), never silently;
 *   4. `[fault]` console lines are rate-limited (far fewer lines than drops);
 *   5. no uncaught exception reached the page.
 * ★ SILENT: --mute-audio. One throwaway profile dir under the OS temp dir, deleted at the end.
 */
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';

const SECS = Number(process.argv[2] || 24);
const PORT = 48010;
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const CDP = 9345;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const prof = mkdtempSync(path.join(tmpdir(), 'vibesdr-badmsg-'));

const mock = spawn(process.execPath, ['scripts/mock-vibeserver.mjs', '--no-pin', '--bad-messages', '--port', String(PORT)],
                   { stdio: 'ignore' });
const edge = spawn(EDGE, ['--headless=new', `--remote-debugging-port=${CDP}`, '--no-first-run', '--no-default-browser-check',
  '--mute-audio', `--user-data-dir=${prof}`, '--window-size=1280,900', '--autoplay-policy=no-user-gesture-required', 'about:blank'],
  { stdio: 'ignore' });
let fails = 0;
const ok = (c, m) => { console.log((c ? '  ok   ' : '  FAIL ') + m); if (!c) fails++; };
const cleanup = () => { try { edge.kill(); } catch (e) { console.error(e); } try { mock.kill(); } catch (e) { console.error(e); }
                        setTimeout(() => { try { rmSync(prof, { recursive: true, force: true }); } catch (e) { console.error('profile cleanup', e); } }, 800); };

try {
  await sleep(2500);
  const page = (await (await fetch(`http://127.0.0.1:${CDP}/json/list`)).json()).find((t) => t.type === 'page');
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  const uncaught = []; const faultLines = [];
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); return; }
    if (m.method === 'Runtime.exceptionThrown') uncaught.push(m.params.exceptionDetails?.exception?.description ?? m.params.exceptionDetails?.text);
    if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') {
      const t = (m.params.args || []).map((a) => a.value ?? a.description ?? '').join(' ');
      if (t.includes('[fault]')) faultLines.push(t.split('\n')[0]);
    }
  };
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  await new Promise((r) => { ws.onopen = r; });
  await send('Page.enable'); await send('Runtime.enable');
  const ev = async (expr) => { const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true }); return r.result?.result?.value; };
  await send('Page.navigate', { url: `http://localhost:${PORT}/index.html?join` });
  await sleep(5000);
  // The splash may still want a click (no-PIN servers show START).
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);

  const snap = () => ev(`JSON.stringify({ status: document.getElementById('status')?.textContent, title: document.getElementById('status')?.title,
    faults: window.__vibeFaults ? window.__vibeFaults() : null })`).then((s) => JSON.parse(s || '{}'));
  await sleep(6000);
  if (process.env.DEBUG) {
    console.log('url:', await ev('location.href'), 'ids:', await ev(`[...document.querySelectorAll('[id]')].filter(e => e.offsetParent).slice(0, 40).map(e => e.id).join(' ')`));
    console.log('body:', (await ev('document.body.innerText'))?.slice(0, 400));
  }
  const a = await snap();
  console.log('t=~11s status:', a.status);
  await sleep(Math.max(4000, (SECS - 11) * 1000));
  const b = await snap();
  console.log(`t=~${SECS}s status:`, b.status);
  const fps = (s) => Number((/([\d.]+)\s*fps/.exec(s || '') || [])[1] || 0);
  const kbAudio = (t) => Number((/audio ([\d.]+) KB\/s/.exec(t || '') || [])[1] || 0);
  ok(fps(a.status) > 5 && fps(b.status) > 5, `1. spectrum still arriving and the loop still drawing (fps ${fps(a.status)} → ${fps(b.status)})`);
  ok(a.status !== b.status, '1. status line still being rewritten by the render loop');
  ok(kbAudio(b.title) > 5, `2. audio still arriving (${kbAudio(b.title)} KB/s)`);
  const f = b.faults || { total: 0, faults: [] };
  console.log('   contained faults:', f.total);
  for (const e of f.faults) console.log(`     ${e.source.padEnd(18)} ${e.kind.padEnd(16)} x${e.count}  ${e.firstError.slice(0, 90)}`);
  const kinds = new Set(f.faults.map((e) => e.source + '/' + e.kind));
  ok(f.total >= 10, '3. bad messages were dropped AND counted');
  for (const k of ['web-spec/bad-json', 'web-spec/short-frame', 'web-spec/bad-magic', 'web-spec/empty-frame'])
    ok(kinds.has(k), `3. counted: ${k}`);
  ok([...kinds].some((k) => k.startsWith('web-audio')), '3. a bad audio frame was counted');
  ok(faultLines.length > 0 && faultLines.length < f.total, `4. logged, rate-limited: ${faultLines.length} [fault] lines for ${f.total} drops`);
  ok(uncaught.length === 0, `5. no uncaught exception reached the page (${uncaught.length})${uncaught.length ? ': ' + uncaught.slice(0, 3).join(' | ') : ''}`);
  try { await send('Browser.close'); } catch (e) { console.error(e); }
} catch (e) {
  console.error(e); fails++;
} finally {
  cleanup();
}
console.log(fails ? `\n${fails} FAILED` : '\nall passed');
setTimeout(() => process.exit(fails ? 1 : 0), 1200);
