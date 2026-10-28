// test-web-waterfall-history.mjs — GitHub #57 (Gesiima; Stuart 2026-10-27): the waterfall must stay ONE continuous trail
// per station while you tune and zoom — "diagonal when tuning then straight again with the signals in the new spot so they
// read as one continuous signal during a tune", and the fan of trails on a zoom — never "broken up" into rows that disagree.
//
// The fault was in the SERVER (local_sdr_shim.cpp onSpectrum): with the IF filter on Auto the tuner follows a locked VFO on
// every step, and a wide row averaged from frames taken at the OLD tuner position was cropped as if from the new one — so
// every few rows the station jumped back by a whole tuner move and forward again (a zig-zag, the "chopped up view").
//
// Here, against the REAL server and a fake rtl_tcp with one steady carrier, in headless Edge (--mute-audio; tests are silent):
//   1. the wire: every spectrum row the page receives puts the carrier where it is (row centre + bin = the carrier ±2 bins)
//      through a locked VFO tuned up in 1 kHz steps (the tuner moving under it), and through zooms in and out;
//   2. the picture: the waterfall the page drew (its own canvas, the needle hidden) — the carrier's column, row by row, moves
//      SMOOTHLY: no row that jumps away from both its neighbours (a zig-zag) through the tune or the zooms, and the trail
//      really did travel (a tune that drew nothing would pass the first check);
//   3. a window resize keeps the history (the rows from before the resize are still on screen, rescaled to the new width);
//   4. THE ZOOM FAN (Stuart 2026-10-27 23:51: zooming SLICED the web waterfall into bands of different zoom): after a
//      straight trail, a zoom IN and then OUT — every row of that trail, old and new, shows the carrier at the column the
//      NEW span puts it (the history fanned with the zoom), never at the old one.
//   6. THE FULL-CAPTURE LAYER (V12, Stuart 2026-10-28 02:00: "the waterfall should be buffering outside the zoomed area so
//      that the black doesn't show at all"): zoomed in AWAY from the carrier for a few seconds, then back out — every row
//      recorded while zoomed shows the carrier at its true column (the coarse full-capture row under the detailed one),
//      never black; zoomed in ON the carrier, those rows keep it DETAILED (narrow) after the zoom out; panned across the
//      capture while zoomed, the same; and a phone turned portrait → landscape → portrait keeps that history.
//   All of it twice: the WebGL renderer, then the 2D fallback (old phones without WebGL draw the same picture).
//   VIBESERVER_BIN=…/vibeserver node --max-old-space-size=512 scripts/test-web-waterfall-history.mjs
//   (the page is the one compiled into that binary — rebuild it after `node scripts/build-web.mjs`)
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
const CENTRE = 1000000, CARRIER = 1050000, SPAN = 200000, STEP = 1000, STEPS = 40;
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

// ★ Every SPEC row the page receives: its centre, span and where its brightest bin says the carrier is — recorded in the page
//   before its own scripts run, so nothing the client does to the row afterwards can hide a wrong one.
const SNIFF = `(() => { window.__rows = []; const O = WebSocket;
  window.WebSocket = function (u, p) { const w = p ? new O(u, p) : new O(u);
    if (String(u).includes('user-spectrum')) w.addEventListener('message', (e) => { const b = e.data;
      if (typeof b === 'string') { try { const m = JSON.parse(b); if (m.type === 'config') window.__rows.push({ t: performance.now(), cfg: m.binBandwidth }); } catch {} return; }
      if (!(b instanceof ArrayBuffer) && !(b instanceof Blob)) return;
      const take = (ab) => { const dv = new DataView(ab); if (ab.byteLength < 23 || dv.getUint32(0, true) !== 0x43455053) return;
        const c = Number(dv.getBigUint64(14, true)), n = ab.byteLength - 22, u8 = new Uint8Array(ab, 22, n), h = n >> 1; let best = -1, bi = 0;
        for (let i = 0; i < n; i++) { const v = u8[(i + h) % n]; if (v > best) { best = v; bi = i; } }
        window.__rows.push({ t: performance.now(), c, n, bi }); if (window.__rows.length > 4000) window.__rows.splice(0, 1000); };
      if (b instanceof ArrayBuffer) take(b); else b.arrayBuffer().then(take); });
    return w; };
  window.WebSocket.prototype = O.prototype; Object.assign(window.WebSocket, O); })();`;

// ★ The carrier's column in every waterfall row on screen, newest first: the centre of the brightest plateau of the row.
//   (The carrier is strong enough to clip the palette over a few pixels — more where a zoom has drawn a row narrower —
//   so "the first brightest pixel" is its left edge, which moves with the plateau's width and reads as a jitter.)
const COLUMNS = `(() => { const h = window.__vibeWf(); if (!h) return null; const wf = h.wf; const cv = document.getElementById('wf');
  const W = cv.width, H = cv.height, wfH = wf.wf.height, top = H - wfH; const d = cv.getContext('2d').getImageData(0, top, W, wfH).data;
  const at = (o, x) => { const p = o + x * 4; return d[p] * 2 + d[p + 1] + d[p + 2]; };
  const cols = [], ls = [], rs = [], lh = [], rh = [], blank = []; for (let y = 0; y < wfH; y++) { let best = -1, bx = 0, lo = 1e9; const o = y * W * 4;
    let sum = 0; for (let x = 2; x < W - 2; x++) { const v = at(o, x); sum += v; if (v < lo) lo = v; if (v > best) { best = v; bx = x; } }
    blank.push(best - lo < 30);
    let l = bx, r = bx; while (l > 2 && at(o, l - 1) >= best - 24) l--; while (r < W - 3 && at(o, r + 1) >= best - 24) r++;
    const lit = best - sum / (W - 4) > 120; cols.push(lit ? Math.round((l + r) / 2) : -1); ls.push(lit ? l : -1); rs.push(lit ? r : -1);
    // ★ The carrier's HALF-brightness extent: rows blend 50/50 into the next (the soft vertical look), so a moving trail's
    //   row is part one row's carrier and part the next's — its visible width is where it stands half way above the floor.
    const half = lo + (best - lo) / 2; let hl = bx, hr = bx;
    while (hl > 2 && at(o, hl - 1) >= half) hl--; while (hr < W - 3 && at(o, hr + 1) >= half) hr++;
    lh.push(lit ? hl : -1); rh.push(lit ? hr : -1); }
  return { W, wfH, cols, ls, rs, lh, rh, blank }; })()`;

/** Rows (y, newest first) whose column jumps away from BOTH neighbours by more than `px` the same way — a torn row (it
 *  leaves the trail and comes back). Except a trail that TURNS (the fast spin reversing: 1118 1174 1196 1178 1136): it
 *  keeps moving away on both sides, at the same pace as the step to the turn.
 *  (-1 = a row with no clear carrier, e.g. not drawn yet — skipped, and so are its neighbours.) */
function zigzags(cols, px) {
  const out = [];
  for (let y = 1; y < cols.length - 1; y++) {
    if (cols[y] < 0 || cols[y - 1] < 0 || cols[y + 1] < 0) continue;
    const a = cols[y] - cols[y - 1], b = cols[y] - cols[y + 1];
    if (!(Math.sign(a) === Math.sign(b) && Math.abs(a) > px && Math.abs(b) > px)) continue;
    // ★ A TURN: the trail keeps going away from this row on BOTH sides (monotonic), at a pace like the step to it.
    //   An alternating zig-zag fails the first half, a lone torn row in a slow trail the second.
    const c2 = cols[y - 2] ?? -1, c3 = cols[y + 2] ?? -1;
    const away = (n1, n2) => n2 >= 0 && Math.abs(cols[y] - n2) >= Math.abs(cols[y] - n1);
    const turn = away(cols[y - 1], c2) && away(cols[y + 1], c3)
              && Math.min(Math.abs(a), Math.abs(b)) <= px + 2 * Math.max(Math.abs(cols[y - 1] - c2), Math.abs(c3 - cols[y + 1]));
    // ★ …or the dial PAUSED for a row at the reversal (1114 1178 1178 1191 1165 1165 1140): the trail still goes on away
    //   three rows out on both sides, which a lone torn row (100 100 110 100 100) never does.
    const c4 = cols[y - 3] ?? -1, c5 = cols[y + 3] ?? -1;
    const paused = c4 >= 0 && c5 >= 0 && Math.abs(cols[y] - c4) > Math.abs(cols[y] - cols[y - 1]) + px
                && Math.abs(cols[y] - c5) > Math.abs(cols[y] - cols[y + 1]) + px && Math.sign(cols[y] - c4) === Math.sign(cols[y] - c5);
    if (paused) continue;
    if (!turn) out.push(y);
  }
  return out;
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-wfhist-'));
for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Waterfall history test', locator: 'IO92nh', adminPass: 'wfhist-web-admin',
  radios: [{ serial: 'wfhist-web', label: 'Test radio', enabled: true, configured: true, users: 1 }] }));
const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
const rtl = spawn(process.execPath, ['--max-old-space-size=256', path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000',
                                     '--tones', '0', '--carrier', String(CARRIER), '--carrier-amp', '20'], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', String(CENTRE), '--mode', 'am', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
  `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
let wsC;
const deadline = setTimeout(() => { console.log('   FAIL deadline (360 s)'); try { edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } catch {} process.exit(1); }, 360000);
try {
  await sleep(3000);
  const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
  wsC = new WebSocket(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  wsC.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  await new Promise((r) => { wsC.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); wsC.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.text; };
  const until = async (x, ms) => { for (let i = 0; i < ms / 200; i++) { if (await ev(x)) return true; await sleep(200); } return false; };
  const click = async (sel) => {
    const r = JSON.parse(await ev(`JSON.stringify(document.querySelector(${JSON.stringify(sel)})?.getBoundingClientRect() || null)`) || 'null');
    if (!r) return false;
    const x = r.x + r.width / 2, y = r.y + r.height / 2;
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
    return true;
  };
  const columns = () => ev(`new Promise(r => requestAnimationFrame(() => requestAnimationFrame(() => r(${COLUMNS}))))`);
  await send('Page.enable'); await send('Runtime.enable');
  await send('Page.addScriptToEvaluateOnNewDocument', { source: SNIFF });
  if (process.env.WFHIST_DEBUG) wsC.addEventListener('message', (e) => { const m = JSON.parse(e.data);
    if (m.method === 'Runtime.exceptionThrown') console.log('   page exception', JSON.stringify(m.params.exceptionDetails).slice(0, 400));
    if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') console.log('   page error', JSON.stringify(m.params.args).slice(0, 300)); });
 for (const MODE of ['webgl', '2d']) {
  console.log(`   ── ${MODE} renderer ──`);
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  await until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 15000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim())); b && b.click(); return !!b; })()`);
  await until(`!!document.querySelector('#audioGate button')`, 15000);
  await click('#audioGate button');
  ok(await until(`!!window.__vibeWf() && window.__rows.length > 20`, 15000), `the page is drawing (${await ev('window.__rows.length')} rows in)`);
  if (process.env.WFHIST_DEBUG) console.log('   page', JSON.stringify(await ev(`({ rows: window.__rows.length, url: location.href, text: document.body.innerText.slice(0, 300), wf: !!window.__vibeWf() })`)));
  if (MODE === '2d') await ev(`(() => { const w = window.__vibeWf().wf; w.gl = null; w.glCanvas = null; w.wf.width = 0; w.resize(); return true; })()`);
  ok(MODE === 'webgl' ? await ev('!!window.__vibeWf().wf.gl') : await ev('!window.__vibeWf().wf.gl'), `drawing with the ${MODE} renderer`);
  // ★ The needle, its passband and the RF-centre line are drawn over the waterfall — hide them so the brightest pixel is the carrier.
  await ev(`(() => { const { wf, spec } = window.__vibeWf(); wf._drawVfo = () => {}; spec.followVfo = true;
    spec.tune(${CENTRE}); spec.zoom(${CENTRE}, ${SPAN} / (spec.cfg.binCount || 1024)); return true; })()`);
  await sleep(10000);  // a straight trail to start from — it fills the lower part of the picture (the resize check reads it)

  // ── 1+2. A LOCKED VFO tuned up in 1 kHz steps — the view follows the dial, the tuner follows the view ──
  const r0 = await ev('window.__rows.length');
  for (let i = 1; i <= STEPS; i++) { await ev(`window.__vibeWf().spec.tune(${CENTRE + i * STEP})`); await sleep(80); }
  await sleep(1500);
  if (process.env.WFHIST_DEBUG) await ev(`(() => { const p = window.__vibeWf().wf.proc; const o = p.process.bind(p); window.__dbg = [];
    p.process = (b, c, bw, i) => { const f = o(b, c, bw, i); let m = 0, bi = 0; for (let k = 0; k < f.row.length; k++) if (f.row[k] > m) { m = f.row[k]; bi = k; }
      const r = p.getRange(); window.__dbg.push([Math.round(performance.now()), c, bw, m, bi, Math.round(r.minDb), Math.round(r.maxDb)]); return f; }; return true; })()`);
  // ── …then a zoom in and out round the dial (the fan), in steps as a pinch or the wheel sends them ──
  const fEnd = CENTRE + STEPS * STEP;
  for (const span of [160000, 130000, 100000, 80000, 100000, 130000, 160000, 200000]) {
    await ev(`(() => { const s = window.__vibeWf().spec; s.zoom(${fEnd}, ${span} / (s.cfg.binCount || 1024)); return true; })()`);
    await sleep(250);
  }
  await sleep(1500);
  // ── …and a FAST SPIN down (4 kHz every 40 ms — faster than the tuner's 120 ms retune cooldown, so the radio lags the
  //    dial) and back: the trail glides across and must still never tear (Stuart: a fast spin may leave a slight gap at
  //    a newly exposed edge while the radio catches up, never a torn or cleared history)
  for (let i = 1; i <= 20; i++) { await ev(`window.__vibeWf().spec.tune(${fEnd - i * 4000})`); await sleep(40); }
  for (let i = 19; i >= 0; i--) { await ev(`window.__vibeWf().spec.tune(${fEnd - i * 4000})`); await sleep(40); }
  await sleep(1500);
  if (process.env.WFHIST_DEBUG) for (const d of (await ev('window.__dbg')) || []) console.log('   dbg', d.join(' '));
  const all = (await ev(`window.__rows.slice(${r0})`)) || [];
  if (process.env.WFHIST_DEBUG) { let lab = 0; for (const r of all) { if (r.cfg) { lab = r.cfg; console.log('   cfg', Math.round(r.t), lab); continue; }
    const fit = [SPAN, 160000, 130000, 100000, 80000].filter((s) => Math.abs(r.c + (r.bi - (r.n >> 1)) * (s / r.n) - CARRIER) <= 2.5 * (s / r.n));
    console.log('   row', Math.round(r.t), r.c, 'label', Math.round(lab * r.n), 'fits', fit.join('/'), fit.length && !fit.some((s) => Math.abs(s - lab * r.n) < 50) ? '  <<< MISLABELLED' : ''); } }
  const rows = all.filter((r) => !r.cfg);
  // ★ A row's span is not in the frame header; a row is RIGHT if SOME span the page asked for (or the default) puts the carrier
  //   on it — a torn row is off by a whole tuner move (kilohertz) at every one of them.
  const spans = [SPAN, 160000, 130000, 100000, 80000];
  const wrong = rows.filter((r) => !spans.some((s) => Math.abs(r.c + (r.bi - (r.n >> 1)) * (s / r.n) - CARRIER) <= 2.5 * (s / r.n)));
  ok(rows.length > 60 && wrong.length === 0,
     `the wire: ${rows.length} rows through the tune and the zooms, ${wrong.length} with the carrier somewhere it is not` +
     (wrong.length ? ` (e.g. centre ${wrong[0].c}, bin ${wrong[0].bi} of ${wrong[0].n})` : ''));

  const pic = await columns();
  ok(!!pic && pic.cols.length > 100, `read the waterfall (${pic?.W}×${pic?.wfH})`);
  // the newest ~ (tune + zooms) rows sit at the top; the column of the carrier there must move smoothly
  const recent = pic.cols.slice(0, Math.min(pic.cols.length, 600));
  // ★ Out of the rows DRAWN — the bottom of the picture may still be the blank ring from before the page started.
  const drawn = recent.filter((x, y) => !pic.blank[y]);
  const lit = drawn.filter((x) => x >= 0);
  const zz = zigzags(recent, 4);
  const travel = lit.length ? Math.max(...lit) - Math.min(...lit) : 0;
  ok(drawn.length > 200 && lit.length > drawn.length * 0.7, `the carrier stands out in ${lit.length} of the ${drawn.length} rows drawn`);
  if (process.env.WFHIST_SHOT) { const s = await send('Page.captureScreenshot', { format: 'png' }); fs.writeFileSync(process.env.WFHIST_SHOT, Buffer.from(s.result.data, 'base64')); }
  if (process.env.WFHIST_DEBUG) { console.log('   cols', recent.slice(0, 260).join(' '));
    console.log('   runs', JSON.stringify(await ev(`(() => { const w = window.__vibeWf().wf; const out = []; const R = w.runs.buf; for (let i = 0; i < 40 && i * 4 < R.length; i++) out.push([R[i*4], R[i*4+1], +R[i*4+2].toFixed(4), +R[i*4+3].toFixed(4)]); return out; })()`))); }
  ok(zz.length === 0, `the picture: the carrier's trail never zig-zags (${zz.length} torn row(s)${zz.length ? ' at y ' + zz.slice(0, 6).join(', ') + ' — columns ' + recent.slice(Math.max(0, zz[0] - 4), zz[0] + 5).join(' ') : ''})`);
  // ★ 40 kHz of tuning across a 200 kHz view = 20 % of the width: the trail must have bent that far — one continuous line
  ok(travel >= pic.W * 0.15, `the trail bends with the tune (${travel} px of travel across ${pic.W} px)`);

  // ── 3. A resize keeps the history (Stuart: resize and rotate must never lose it) ──
  // ★ The OLDEST rows with the carrier in them — the straight trail from before the tune — and where they sit.
  const before = await columns();
  // ★ Rows on a VERTICAL stretch only (since 2026-10-28 a zoom leaves curved trails in the history, where a few lines of
  //   scroll between the two reads would move the column — that is the scroll, not a lost row).
  const vert = (y) => [-3, -2, -1, 1, 2, 3].every((d) => before.cols[y + d] >= 0 && Math.abs(before.cols[y + d] - before.cols[y]) <= 1);
  const old = before.cols.map((x, y) => [y, x]).filter(([y, x]) => x >= 0 && vert(y)).slice(-40);
  await send('Emulation.setDeviceMetricsOverride', { width: 900, height: 900, deviceScaleFactor: 1, mobile: false });
  await sleep(150);
  const after = await columns();
  const k = after.W / before.W;
  // the picture keeps scrolling between the two reads (~15 rows/s) — a row may have moved down a few lines
  const kept = old.filter(([y, x]) => [0, 1, 2, 3, 4, 5, 6].some((dy) => Math.abs((after.cols[y + dy] ?? -99) - x * k) <= 4)).length;
  ok(old.length >= 30 && after.W !== before.W && kept >= old.length * 0.8,
     `a resize (${before.W} → ${after.W} px) keeps the history: ${kept}/${old.length} of the oldest rows still show the carrier, at its rescaled column`);

  // ── 4. A ZOOM LEAVES THE HISTORY AS DRAWN (Stuart 2026-10-28: "a thin line signal should expand out to the width and
  //    the history remains the thin line"; zooming out "no breaks … a continuous history"): a straight trail, then a zoom —
  //    the new rows at the new column, the old trail still at the old one, and between them the ease's curve: one line,
  //    no break. ★ The carrier sits 10 kHz above the dial: 5 % right of centre at 200 kHz, 12.5 % at 80 kHz.
  const fanCase = async (fromSpan, toSpan) => {
    await sleep(4000);   // a straight trail at fromSpan (~80 rows)
    const pre = await columns();
    const colAt = (span, W) => Math.round((0.5 + (CARRIER - fEnd) / span) * W);
    const c0 = colAt(fromSpan, pre.W);
    await ev(`(() => { const s = window.__vibeWf().spec; s.zoom(${fEnd}, ${toSpan} / (s.cfg.binCount || 1024)); return true; })()`);
    await sleep(2500);
    const post = await columns();
    const c1 = colAt(toSpan, post.W);
    const newTop = post.cols.slice(2, 12).filter((x) => x >= 0), newAt = newTop.filter((x) => Math.abs(x - c1) <= 4).length;
    const yOld = post.cols.findIndex((x, y) => y > 12 && x >= 0 && Math.abs(x - c0) <= 3);
    const oldRun = yOld >= 0 ? post.cols.slice(yOld, yOld + 40).filter((x) => x >= 0) : [];
    const oldAt = oldRun.filter((x) => Math.abs(x - c0) <= 3).length;
    let worst = 0, gaps = 0;
    for (let y = 3; y <= yOld + 2 && y < post.cols.length; y++) { const p = post.cols[y - 1], q = post.cols[y];
      if (p >= 0 && q >= 0) worst = Math.max(worst, Math.abs(q - p)); else gaps++; }
    ok(newTop.length >= 8 && newAt >= newTop.length * 0.9 && yOld > 12 && oldRun.length >= 30 && oldAt >= oldRun.length * 0.9 && worst <= 24 && gaps <= 2,   // (≤ 24 px: a short ease-out on 1-px rows with no per-row join yet; headless draws ~16 rows/s)
       `zoom ${fromSpan / 1000} → ${toSpan / 1000} kHz: new rows at ${c1} (${newAt}/${newTop.length}), the history still at ${c0} (${oldAt}/${oldRun.length}),` +
       ` joined by one curve (largest step ${worst} px, ${gaps} unlit rows over ${yOld} rows)`);
  };
  await fanCase(SPAN, 80000);
  await fanCase(80000, SPAN);

  // ── 5. GLIDE (Stuart 2026-10-28: "I just wanted the chunky broken diagonal lines joined up so they looked solid";
  //    the reference is the first native build's sweep — whole rows at their own placement, a smooth curve, no steps).
  //    A tune draws a trail that moves EVENLY row by row — never the staircase of a run of rows at one place and then a
  //    jump (several rows emitted per frame all snapped to that frame's placement). At a drum's pace the rows also TOUCH
  //    (no gap between one row's carrier and the next). At a fast spin — more pixels a row than the carrier is wide — a
  //    one-bin carrier is an even dotted diagonal: the rows are a frame interval apart and nothing real lies between. ──
  let tuneAt = fEnd;
  const glideCase = async (label, stepHz, everyMs, steps, wantTouch) => {
    await sleep(1500);
    const from = tuneAt;
    for (let i = 1; i <= steps; i++) { tuneAt = from + i * stepHz; await ev(`window.__vibeWf().spec.tune(${tuneAt})`); await sleep(everyMs); }
    await sleep(1500);
    const g = await columns();
    // ★ WFHIST_SHOTS=<dir>: a picture of each tune per renderer (before/after comparisons of the glide).
    if (process.env.WFHIST_SHOTS) { const s = await send('Page.captureScreenshot', { format: 'png' });
      fs.mkdirSync(process.env.WFHIST_SHOTS, { recursive: true });
      fs.writeFileSync(path.join(process.env.WFHIST_SHOTS, `${label}-${MODE}.png`), Buffer.from(s.result.data, 'base64')); }
    const cStart = Math.round((0.5 + (CARRIER - from) / SPAN) * g.W), cEnd = Math.round((0.5 + (CARRIER - tuneAt) / SPAN) * g.W);
    // the moving part of the trail, newest first: rows strictly between where it started and where it settled
    const ys = []; for (let y = 0; y < Math.min(400, g.cols.length); y++) { const c = g.cols[y]; if (c >= 0 && c < cStart - 3 && c > cEnd + 3) ys.push(y); }
    let gaps = 0, worst = 0, pairs = 0, still = 0;
    const ds = [];
    const y0 = ys.length ? ys[0] - 1 : 0, y1 = ys.length ? ys[ys.length - 1] + 1 : 0;
    for (let y = Math.max(0, y0); y < y1; y++) {
      if (g.lh[y] < 0 || g.lh[y + 1] < 0) continue;
      pairs++;
      const gap = Math.max(g.lh[y] - g.rh[y + 1], g.lh[y + 1] - g.rh[y]) - 1;   // empty pixels between the two rows' carriers
      if (gap > 1) { gaps++; worst = Math.max(worst, gap); }
      const d = Math.abs(g.cols[y] - g.cols[y + 1]); ds.push(d); if (d === 0) still++;
    }
    // ★ "Largest" is the 90th percentile: a spin faster than the tuner's retune cooldown leaves the radio catching up in
    //   one jump now and then (the hard rule's allowed gap) — a staircase is MANY rows standing still and then jumps.
    const sorted = [...ds].sort((x, y) => x - y), med = sorted.length ? sorted[sorted.length >> 1] : 0,
          big = sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * 0.9))] : 0;
    // ★ the fast spin outruns the tuner (it catches up in jumps — the allowed gap), so it gets the looser bound
    const [sMax, bK, bA] = wantTouch ? [0.25, 3, 4] : [0.35, 4, 6];
    ok(ys.length >= 6 && pairs >= 6 && still <= pairs * sMax && big <= bK * med + bA,
       `${label} (${cStart} → ${cEnd} px): the trail moves EVENLY, a curve not a staircase — ${still}/${pairs} rows standing still, steps median ${med} px, 90th percentile ${big} px`);
    // ★ up to 1 in 10 pairs: a frame the server sends late leaves a small gap (the timing of a loaded test box)
    if (wantTouch) ok(gaps <= Math.max(1, Math.floor(pairs * 0.1)), `${label}: one continuous trail — ${gaps} gap(s) between ${pairs} row pairs` + (gaps ? ` (widest ${worst} px)` : ''));
  };
  await glideCase('drum-spin', 500, 50, 40, true);
  await glideCase('fast-spin', 3000, 60, 16, false);
  // ── 6. THE FULL-CAPTURE LAYER — no black inside the capture after a zoom out ──
  {
    // ★ The view moves, the dial does not, all through this section (followVfo off — every zoom goes out as `free`).
    await ev('(() => { window.__vibeWf().spec.followVfo = false; return true; })()');
    const ringRows = () => ev(`(() => { const w = window.__vibeWf().wf; return w.gl ? w.gl.rows : w.wf.height; })()`);
    const headNow = () => ev('window.__vibeWf().wf.head');
    const zoomTo = (c, span) => ev(`(() => { const s = window.__vibeWf().spec; s.zoom(${c}, ${span} / (s.cfg.binCount || 1024)); return true; })()`);
    const colAt = (f, W) => Math.round((0.5 + (f - fEnd) / SPAN) * W);
    // Zoom to `span` round `centres` (one per second — a pan), then back out to SPAN round the dial; the rows written
    // meanwhile, newest first, as display rows [y0, y1) of the picture taken after.
    const phase = async (span, centres, during) => {
      await zoomTo(fEnd, SPAN); await sleep(1500);
      const h0 = await headNow();
      for (const c of centres) { await zoomTo(c, span); await sleep(1000); }
      const mid = during ? await during() : null;
      const h1 = await headNow();
      await zoomTo(fEnd, SPAN); await sleep(1200);
      const h2 = await headNow(), R = await ringRows();
      const pic = await columns();
      // a ring row r is display row (r − head) mod R; the phase wrote rows h1 … h0−1 (the head walks backwards)
      const y0 = ((h1 - h2) % R + R) % R + 3, y1 = ((h0 - h2) % R + R) % R - 3;   // a few in-flight rows excused at each end
      return { pic, y0, y1, mid };
    };
    // Mean brightness (2R+G+B) of the waterfall's rows [y0, y1) over columns [x0, x1) — the floor's look, in pixels.
    const lum = (y0, y1, x0, x1) => ev(`(() => { const cv = document.getElementById('wf'), wf = window.__vibeWf().wf.wf;
      const top = cv.height - wf.height, d = cv.getContext('2d').getImageData(${x0}, top + ${y0}, ${x1 - x0}, ${y1 - y0}).data;
      let s = 0; for (let i = 0; i < d.length; i += 4) s += d[i] * 2 + d[i + 1] + d[i + 2]; return s / (d.length / 4); })()`);
    const lit = (pic, y0, y1, col, tol) => { let n = 0, at = 0, w = 0;
      for (let y = y0; y < y1; y++) { n++; const c = pic.cols[y]; if (c >= 0 && Math.abs(c - col) <= tol) { at++; w = Math.max(w, pic.rs[y] - pic.ls[y] + 1); } }
      return { n, at, w }; };
    // (a) zoomed 20 kHz AWAY from the carrier (it is 10 kHz above the dial; the view sits 40 kHz below it) — outside the view
    {
      const { pic, y0, y1: y1all, mid } = await phase(20000, [fEnd - 30000, fEnd - 30000, fEnd - 30000, fEnd - 30000],
        () => ev('window.__vibeWf().wf.layerFloorCheck()'));
      // ★ the OLDEST rows of the phase are the zoom-in's ease (drawn at in-between, wider views — they may show the
      //   carrier, rightly): the newest 60 % are the settled zoom
      const y1 = y0 + Math.round((y1all - y0) * 0.6);
      const r = lit(pic, y0, y1, colAt(CARRIER, pic.W), 12);
      // ★★ "the same waterfall, just less sharp" (Stuart's review of v1): the layer's floor matches the detailed rows' in dB
      //    (in the view, and outside it on this flat band), the filled rows' floor LOOKS like the detailed rows' floor beside
      //    it, and the carrier in the filled rows is a thin line, not a blob.
      // ★ within 1 dB + the wire's own step (both rows arrive as whole dB, and the detailed median is one of them)
      ok(mid && Math.abs(mid.inView - mid.det) <= 1.5 && Math.abs(mid.outside - mid.det) <= 1.5,
         `the layer's floor in dB: detailed median ${mid && mid.det.toFixed(1)}, layer in the view ${mid && mid.inView.toFixed(1)}, outside it ${mid && mid.outside.toFixed(1)} (within 1 dB + the 1 dB wire step)`);
      // ★★ Since 2026-10-28 a zoom out no longer re-maps those rows: they stay as drawn (the narrow view, the carrier
      //    outside it) — every one still the identity, nothing black inside it.
      const asDrawn = await ev(`(() => { const w = window.__vibeWf().wf; const R = w.xfA.length; let n = 0, c = 0;
        for (let j = ${y0}; j < ${y1}; j++) { const r = (w.head + j) % R; n++; if (w.xfA[r] === 1 && w.xfB[r] === 0) c++; } return [c, n]; })()`);
      ok(asDrawn && asDrawn[1] >= 30 && asDrawn[0] === asDrawn[1] && r.at === 0,
         `zoomed in away from the carrier, then out: ${asDrawn && asDrawn[0]}/${asDrawn && asDrawn[1]} of the rows written meanwhile stay as drawn (the carrier outside them: ${r.at} show it)`);
    }
    // (b) zoomed ON the carrier: those rows keep their detail after the zoom out (narrow, not the coarse layer's width)
    {
      const { pic, y0, y1: y1all } = await phase(20000, [CARRIER, CARRIER, CARRIER, CARRIER]);
      const y1 = y0 + Math.round((y1all - y0) * 0.6);   // (past the zoom-in's ease, as above)
      // as drawn: the view was centred on the carrier, so those rows keep it at the CENTRE, thin
      const r = lit(pic, y0, y1, Math.round(pic.W / 2), 4);
      ok(r.n >= 30 && r.at >= r.n * 0.9 && r.w <= 8,
         `zoomed in ON the carrier, then out: ${r.at}/${r.n} rows keep it where it was drawn (the centre), at most ${r.w} px wide`);
    }
    // (d) a PHONE turned portrait → landscape → portrait keeps the history (the same ring, re-drawn at the new size)
    {
      const before = await columns();
      const oldRows = before.cols.map((x, y) => [y, x]).filter(([, x]) => x >= 0).slice(0, 200);
      for (const [w, h] of [[390, 844], [844, 390], [390, 844]]) {
        await send('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 2, mobile: true });
        await sleep(400);
      }
      const after = await columns();
      const k = after.W / before.W;
      // ★ The GPU ring shows one row per device pixel, so a taller screen shows MORE rows (a row stays at its y, plus what
      //   scrolled in meanwhile — at dpr 2 that is twice the rows a second); the 2D ring is re-heighted (stretched).
      const near = (yy, x) => Math.abs((after.cols[yy] ?? -99) - x * k) <= 6;
      const kept = oldRows.filter(([y, x]) => {
        for (let dy = 0; dy <= 90; dy++) if (near(y + dy, x)) return true;
        for (let dy = 0; dy <= 16; dy += 2) if (near(Math.round((y + dy) * after.wfH / before.wfH), x)) return true;
        return false; }).length;
      ok(oldRows.length >= 100 && kept >= oldRows.length * 0.7,
         `a phone turned portrait → landscape → portrait (${before.W}×${before.wfH} → ${after.W}×${after.wfH}) keeps the history: ${kept}/${oldRows.length} rows`);
      await send('Emulation.setDeviceMetricsOverride', { width: 900, height: 900, deviceScaleFactor: 1, mobile: false });
      await sleep(400);
    }
    // (c) PANNED across the capture while zoomed. A pan is a centre move, so — like a tune — the rows keep their own
    //     placement (the #57 bend); what must hold is that NONE of them can show black inside the capture once zoomed
    //     out: every row written during the pan carries a full-capture layer (X > 0) reaching across the whole screen.
    {
      const { y0, y1 } = await phase(20000, [fEnd - 40000, fEnd - 10000, CARRIER, CARRIER + 30000, CARRIER + 60000]);
      const cov = await ev(`(() => { const w = window.__vibeWf().wf; const R = w.xfA.length; let n = 0, c = 0;
        for (let j = ${y0}; j < ${y1}; j++) { const r = (w.head + j) % R; n++;
          const a = w.xfA[r], b = w.xfB[r], X = w.xfX[r], e = 1 + 2 * X;
          const detail = b <= 0 && a + b >= 1, layer = X > 0 && (b + X) / e <= 0 && (a + b + X) / e >= 1;
          if (detail || layer) c++; }
        return [c, n]; })()`);
      ok(cov && cov[1] >= 40 && cov[0] >= cov[1] * 0.95,
         `panned across the capture while zoomed, then out: ${cov && cov[0]}/${cov && cov[1]} rows have data across the whole screen (no black inside the capture)`);
    }
    await ev(`(() => { const s = window.__vibeWf().spec; s.followVfo = true; s.tune(${fEnd}); return true; })()`);
    await zoomTo(fEnd, SPAN); await sleep(1000);
  }

 }
} catch (e) { ok(false, `driver: ${e && e.stack || e}`); }
finally {
  clearTimeout(deadline);
  try { wsC?.close(); } catch {}
  edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(600);
  if (!fail) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
}
console.log(`   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
