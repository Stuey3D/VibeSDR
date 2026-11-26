// test-server-maritime.mjs — V12 beta96 THE MARITIME COMBINATION (AIS + DSC channel 70 from one wide capture) through the REAL
// server (docs/v12/MARITIME-COMBO.md). A fake rtl_tcp replays a synthetic 8 MS/s clip centred on 159.2625 MHz
// (`test-maritime-combo --write-clip m.cu8 8000000`: a DSC distress alert on channel 70 + four ships on both AIS channels).
// The fake is given an RSP's rate list (VIBE_TEST_TCP_RATES — tests only), so the server treats it as a radio that runs 8 MS/s.
//   1. an RTL at 2.4 MS/s: AIS offered, the combination NOT (no aisDsc, mapModes aisdsc hw false), and asking for it is
//      refused with the reason — AIS alone unchanged;
//   2. the 8 MS/s radio: aisDsc offered; entering puts the radio at 159.2625 MHz; the AIS report says combo ais+dsc with the four
//      ships and the call; the gallery holds the DSC distress item, pinned, and channel 70's day log; AIS alone moves the radio
//      to 162.000 keeping the ships; leaving gives the radio back;
//   3. KEEP RUNNING AIS + DSC (landing map avmar, landingModes aisdsc, mapKeep): entered at start with nobody connected;
//   4. SINGLE MODE AIS + DSC: kept from the start, a listener lands in it, every other mode blocked (AIS and DSC not).
// Silent: no audio socket is opened.
//   VIBESERVER_BIN=…/vibeserver VIBE_MARITIME_CLIP=…/m.cu8 node scripts/test-server-maritime.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_MARITIME_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_MARITIME_CLIP (test-maritime-combo --write-clip m.cu8 8000000)');
  process.exit(3);
}
const RSP_RATES = '8000000,6000000,5000000,4000000,3000000,2048000,2000000';
const PASS = 'maritime-test-admin';
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 200; i++) { if (await f()) return true; await sleep(200); } return !!(await f()); };
const MMSI = [235012301, 235012302, 244660003, 211444004];

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], open: false };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
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
      if (op === 0x1) { st.txt.push(p.toString()); if (st.txt.length > 400) st.txt.splice(0, 100); }
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b]));
  };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
const jsons = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => { try { return JSON.parse(t); } catch { return null; } }).filter(Boolean);
const lastOf = (st, type) => { const a = jsons(st, type); return a[a.length - 1] || null; };

async function server(tag, radioExtra = {}, opt = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Maritime test', locator: 'IO90bs', adminPass: PASS, galleryMb: 20,
    radios: [{ serial: `${tag}-test`, label: 'Test radio', enabled: true, configured: true, users: 4, ...radioExtra }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rate = opt.rate || 8000000;
  const rtl = spawn(process.execPath, ['--max-old-space-size=256', path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(rate),
                                       '--hf', CLIP, '--hf-centre', '159262500'], { stdio: 'ignore' });
  await sleep(500);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), ...(rate > 3e6 ? { VIBE_TEST_TCP_RATES: RSP_RATES } : {}) };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(rate), '--freq', '96100000', '--mode', 'wfm',
                          '--users', '4', '--admin-pass', PASS], { env, stdio: ['ignore', log, log] });
  const getJson = async (p) => { try { const r = await fetch(`http://127.0.0.1:${port}${p}`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  const status = async () => await getJson('/vibeserver.json');
  await until(async () => !!(await status()), 15000);
  const L = () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } };
  const stop = () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); setTimeout(() => { try { fs.rmSync(dir, { recursive: true, force: true }); } catch {} }, 1500); };
  return { port, dir, status, getJson, L, stop };
}
const spectrum = (port) => ws(port, `/ws/user-spectrum?user_session_id=${crypto.randomUUID().slice(0, 12)}&bins=256&proto=1`);
/** The Setup page's facts (GET /vibeserver/hardware — mapModes, the landing gate). */
const hwMode = async (S, id) => { const h = await S.getJson('/vibeserver/hardware'); return h && Array.isArray(h.mapModes) ? h.mapModes.find((m) => m.id === id) : null; };

try {
  // ── 1. an RTL at 2.4 MS/s: AIS as before, no combination ─────────────────────────────────────────────────────────────
  console.log('── 1. RTL at 2.4 MS/s: AIS offered, AIS + DSC not ──');
  {
    const S = await server('mar24', {}, { rate: 2400000 });
    try {
      const st = await S.status();
      ok(st && st.ais === true && st.aisDsc === undefined, `vibeserver.json: ais ${st && st.ais}, aisDsc ${st && st.aisDsc} (absent — no MARITIME key)`);
      const sp = spectrum(S.port);
      await until(() => sp.open && !!lastOf(sp, 'hwinfo'), 5000);
      const m = await hwMode(S, 'aisdsc');
      ok(m && m.hw === false && (await hwMode(S, 'ais'))?.hw === true, `hwinfo mapModes: aisdsc hw ${m && m.hw} (not drawn), ais hw true; minRate ${m && m.minRate}`);
      sp.send({ type: 'ais', on: 1, band: 'ais+dsc' });
      ok(await until(() => !!lastOf(sp, 'ais_error'), 4000) && /6\.2 MS\/s/.test(lastOf(sp, 'ais_error')?.why || ''), `asked anyway: refused — "${lastOf(sp, 'ais_error')?.why}"`);
      ok(!/\[AIS\] mode ON/.test(S.L()), 'the radio did not move');
      sp.close();
    } finally { S.stop(); }
  }

  // ── 2. the 8 MS/s radio ─────────────────────────────────────────────────────────────────────────────────────────────
  console.log('── 2. a radio at 8 MS/s: MARITIME (AIS + DSC) ──');
  {
    const S = await server('mar8');
    try {
      const st = await S.status();
      ok(st && st.ais === true && st.aisDsc === true, `vibeserver.json: ais ${st && st.ais}, aisDsc ${st && st.aisDsc}`);
      const sp = spectrum(S.port);
      await until(() => sp.open && !!lastOf(sp, 'hwinfo'), 5000);
      const m = await hwMode(S, 'aisdsc');
      ok(m && m.hw === true && !m.why && m.minRate > 6.1e6 && m.minRate < 6.2e6, `hwinfo mapModes aisdsc: ${JSON.stringify(m)}`);
      await sleep(800);
      sp.send({ type: 'ais', on: 1, band: 'ais+dsc' });
      ok(await until(() => lastOf(sp, 'ais')?.combo === 'ais+dsc', 8000), 'an AIS report with combo "ais+dsc"');
      const cfg = lastOf(sp, 'config');
      ok(cfg && Math.abs(cfg.centerFreq - 159.2625e6) < 1, `the radio at 159.2625 MHz (config centre ${cfg && cfg.centerFreq})`);
      ok(/\[AIS\] mode ON: radio at 159\.2625 MHz, 8\.000 MS\/s .*DSC channel 70/.test(S.L()), 'the log says so');
      const four = () => { const r = lastOf(sp, 'ais'); return r && MMSI.every((x) => r.vessels.some((v) => v.mmsi === x)); };
      ok(await until(four, 30000), `the four ships, both channels: ${(lastOf(sp, 'ais')?.vessels || []).map((v) => v.mmsi).join(' ')}`);
      const callOk = () => { const r = lastOf(sp, 'ais'); return r && r.dsc && r.dsc.calls >= 1 && r.dsc.recent.some((c) => c.self === '235012345' || JSON.stringify(c).includes('235012345')); };
      ok(await until(callOk, 20000), `channel 70's distress alert in the report (calls ${lastOf(sp, 'ais')?.dsc?.calls})`);
      const r = lastOf(sp, 'ais');
      ok(r && r.dsc && r.dsc.calls === 1 && r.dsc.hz === 156525000, `ONE call though the clip repeats it every second (repeats folded: ${r && r.dsc && r.dsc.calls})`);
      ok(r && r.centre === 162000000 && r.chA > 0 && r.chB > 0 && r.dropped === 0, `AIS's own fields as before (centre ${r && r.centre}, chA ${r && r.chA}, chB ${r && r.chB}, dropped ${r && r.dropped})`);
      // the gallery: channel 70's day log and the distress item, pinned (what the SHIPPING map's DSC layer and the community map read)
      let g = null;
      const alertItem = (x) => x.lv === 3 && /^DISTRESS /.test(x.title);
      ok(await until(async () => { g = await S.getJson('/vibeserver/gallery.json?kind=dsc&limit=50'); return g && g.items && g.items.some(alertItem); }, 15000),
         `the gallery holds the distress item (${g && g.items ? g.items.map((x) => x.title.slice(0, 40)).join(' | ') : 'none'})`);
      const di = g && g.items.find(alertItem);
      ok(di && Math.abs(di.plat - 50.2) < 0.01 && Math.abs(di.plon + 4.1667) < 0.01, `…pinned at its position (${di && JSON.stringify([di.plat, di.plon, di.o])})`);
      ok(await until(async () => { g = await S.getJson('/vibeserver/gallery.json?kind=dsc&limit=50'); return g && g.items && g.items.some((x) => /^DSC 156\.525 MHz · /.test(x.title)); }, 70000),
         `…and channel 70's day log (${g && g.items ? (g.items.find((x) => /^DSC 156/.test(x.title)) || {}).title : ''})`);
      // AIS alone: the same mode, the radio moves, the ships stay
      sp.send({ type: 'ais', on: 1 });
      ok(await until(() => { const a = lastOf(sp, 'ais'); return a && !a.combo; }, 6000), 'AIS (alone): the report has no combo any more');
      ok(Math.abs((lastOf(sp, 'config')?.centerFreq || 0) - 162e6) < 1, `…the radio at 162.000 MHz (${lastOf(sp, 'config')?.centerFreq})`);
      ok(MMSI.every((x) => (lastOf(sp, 'ais')?.vessels || []).some((v) => v.mmsi === x)), '…the ships kept');
      sp.send({ type: 'ais', on: 1, band: 'ais+dsc' });
      ok(await until(() => lastOf(sp, 'ais')?.combo === 'ais+dsc', 6000), 'and back to AIS + DSC in place');
      sp.send({ type: 'ais', on: 0 });
      ok(await until(() => !!lastOf(sp, 'ais_off'), 5000), 'ais_off on leaving');
      ok(await until(() => Math.abs((lastOf(sp, 'config')?.centerFreq || 0) - 96.1e6) < 1, 5000), `the radio given back (96.1 MHz: ${lastOf(sp, 'config')?.centerFreq})`);
      const st2 = await S.status();
      ok(st2 && st2.aisDsc === true && !st2.aisDscOn, 'still offered, no longer on');
      sp.close();
    } finally { S.stop(); }
  }

  // ── 3. KEEP RUNNING AIS + DSC ───────────────────────────────────────────────────────────────────────────────────────
  console.log('── 3. keep running AIS + DSC (landing map) ──');
  {
    const S = await server('markeep', { landingMap: 'avmar', landingModes: 'aisdsc', mapKeep: true });
    try {
      ok(await until(() => /\[AIS\] mode ON: radio at 159\.2625 MHz/.test(S.L()), 12000), 'entered at start with nobody connected (radio at 159.2625 MHz)');
      ok(/\[MAP\] keep running: .*AIS \+ DSC/.test(S.L()), 'the log names the kept mode "AIS + DSC"');
      const sp = spectrum(S.port);
      ok(await until(() => lastOf(sp, 'ais')?.combo === 'ais+dsc', 6000), 'a listener joins it (AIS report, combo ais+dsc)');
      sp.close();
      await sleep(3000);
      ok(!/\[AIS\] mode OFF/.test(S.L()), 'the last listener leaving does not take it away');
    } finally { S.stop(); }
  }

  // ── 4. SINGLE MODE AIS + DSC ────────────────────────────────────────────────────────────────────────────────────────
  console.log('── 4. single mode: AIS + DSC ──');
  {
    const S = await server('marsingle', { singleMode: 'aisdsc', blockedModes: 'dsc' });
    try {
      ok(await until(() => /\[AIS\] mode ON: radio at 159\.2625 MHz/.test(S.L()), 12000), 'kept from the start (the owner had DSC off — single mode AIS + DSC needs it, so it stands on both)');
      const sp = spectrum(S.port);
      await until(() => !!lastOf(sp, 'hwinfo') && lastOf(sp, 'ais')?.combo === 'ais+dsc', 6000);
      const hw = lastOf(sp, 'hwinfo') || {};
      const bl = new Set(hw.blocked || []);
      ok(hw.singleMode === 'aisdsc' && bl.has('wfm') && bl.has('adsb') && bl.has('avdata') && !bl.has('ais') && !bl.has('dsc'), `hwinfo: singleMode aisdsc, others blocked, AIS and DSC not (${[...bl].join(' ')})`);
      ok(lastOf(sp, 'ais')?.combo === 'ais+dsc', 'a listener lands in AIS + DSC');
      sp.send({ type: 'adsb', on: 1 });
      await sleep(1200);
      ok(!/\[ADS-B\] mode ON/.test(S.L()), 'cannot switch to ADS-B');
      sp.close();
    } finally { S.stop(); }
  }
} catch (e) { fail++; console.log('   FAIL ' + (e && e.stack || e)); }

console.log(`\nmaritime (server): ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
