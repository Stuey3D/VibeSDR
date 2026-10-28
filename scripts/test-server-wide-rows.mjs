// test-server-wide-rows.mjs — V12 THE FULL-CAPTURE ROW, "SPWB" (Stuart 2026-10-28 02:00: "the waterfall should be buffering
// outside the zoomed area so that the black doesn't show at all"). Against the REAL server and a fake rtl_tcp with one
// carrier OUTSIDE the zoomed view:
//   1. a listener that asked `wide=1` gets NO SPWB while its view is the whole capture;
//   2. zoomed in, it gets one SPWB per kWideEvery (8) SPEC frames — at the capture's centre and span, LOW → HIGH, with the
//      one-bin carrier outside the view at its true frequency (max-hold kept it);
//   3. a listener that did NOT ask (an older client — same URL as before) gets none, zoomed or not, and the SPEC stream of
//      both is unchanged: the same frame rate zoomed as not (no extra frames, no fps raise);
//   4. zoomed back out, SPWB stops;
//   and prints the bytes per second each listener receives, zoomed and not (the bandwidth cost).
//   VIBESERVER_BIN=…/vibeserver node scripts/test-server-wide-rows.mjs      (silent: no audio socket is opened)
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — VIBESERVER_BIN needed'); process.exit(3); }
const RATE = 2400000, CENTRE = 1000000, CARRIER = 1400000, ZSPAN = 200000;
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-wide-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Wide rows test', locator: 'IO92nh', adminPass: 'wide-rows-admin',
  radios: [{ serial: 'wide-rows', label: 'Test radio', enabled: true, configured: true, users: 2 }] }));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, ['--max-old-space-size=256', path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--tones', '0', '--carrier', String(CARRIER), '--carrier-amp', '20'], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(CENTRE), '--mode', 'am', '--users', '2'],
                  { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });

/** A spectrum listener: counts SPEC and SPWB frames and bytes in the current window, keeps the last SPWB. */
function listener(sid, wide) {
  const L = { spec: 0, wide: 0, bytes: 0, lastWide: null, hw: null };
  const w = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=${sid}&mode=binary8&bins=1024&proto=1${wide ? '&wide=1' : ''}`);
  w.binaryType = 'arraybuffer';
  w.onmessage = (e) => {
    if (typeof e.data === 'string') { L.bytes += e.data.length; try { const m = JSON.parse(e.data); if (m.type === 'hwinfo') L.hw = m; } catch {} return; }
    const b = new Uint8Array(e.data); L.bytes += b.length;
    const tag = String.fromCharCode(b[0], b[1], b[2], b[3]);
    if (tag === 'SPEC') L.spec++;
    else if (tag === 'SPWB') { L.wide++; L.lastWide = b; }
  };
  L.ws = w;
  L.reset = () => { L.spec = 0; L.wide = 0; L.bytes = 0; };
  L.send = (o) => w.send(JSON.stringify(o));
  L.open = new Promise((r) => { w.onopen = r; });
  return L;
}
const parseWide = (b) => {
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const F = dv.getUint16(26, true), C = dv.getUint16(28, true), P = dv.getUint16(30, true);
  const floor = b.subarray(32, 32 + F), spread = b.subarray(32 + F, 32 + 2 * F), peaks = [];
  let cell = -1;
  for (let i = 0, o = 32 + 2 * F; i < P; i++, o += 3) { cell += b[o]; if (b[o + 1]) peaks.push({ cell, lvl: b[o + 1], shape: b[o + 2] }); }
  return { ver: b[4], centre: Number(dv.getBigUint64(14, true)), span: dv.getUint32(22, true), F, C, floor, spread, peaks, bytes: b.length };
};

const deadline = setTimeout(() => { console.log('   FAIL deadline'); srv.kill('SIGTERM'); rtl.kill('SIGTERM'); process.exit(1); }, 90000);
try {
  for (let i = 0; i < 60; i++) { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`); if (r.ok) break; } catch {} await sleep(250); }
  const A = listener('wideA0001', true), B = listener('wideB0001', false);
  await Promise.all([A.open, B.open]);
  await sleep(1500);
  A.send({ type: 'tune', frequency: CENTRE, mode: 'am' });
  await sleep(2500);
  ok(A.hw && A.hw.wide === 2, `hwinfo advertises the full-capture row (wide: ${A.hw && A.hw.wide})`);

  // ── 1. the whole capture in view: no SPWB ──
  A.reset(); B.reset(); await sleep(4000);
  const full = { aSpec: A.spec / 4, bSpec: B.spec / 4, aKBs: A.bytes / 4 / 1024, bKBs: B.bytes / 4 / 1024, aWide: A.wide };
  ok(full.aSpec > 3 && full.aWide === 0, `whole capture in view: ${full.aSpec.toFixed(1)} SPEC/s and NO SPWB (${full.aWide})`);

  // ── 2/3. zoomed to 200 kHz around the dial: SPWB every 8th SPEC frame for A, none for B ──
  A.send({ type: 'zoom', frequency: CENTRE, binBandwidth: ZSPAN / 1024 });
  await sleep(1500);
  A.reset(); B.reset(); await sleep(6000);
  const z = { aSpec: A.spec / 6, bSpec: B.spec / 6, aWide: A.wide / 6, bWide: B.wide, aKBs: A.bytes / 6 / 1024, bKBs: B.bytes / 6 / 1024 };
  ok(z.aWide > 0 && Math.abs(z.aWide - z.aSpec / 4) <= Math.max(0.5, z.aSpec * 0.05),
     `zoomed: ${z.aWide.toFixed(2)} SPWB/s for ${z.aSpec.toFixed(1)} SPEC/s — one per 4`);
  ok(z.bWide === 0, `a listener that did not ask (an older client) gets none: ${z.bWide}`);
  ok(Math.abs(z.aSpec - full.aSpec) <= full.aSpec * 0.15 && Math.abs(z.bSpec - full.bSpec) <= full.bSpec * 0.15,
     `no extra frames: SPEC ${full.aSpec.toFixed(1)} → ${z.aSpec.toFixed(1)}/s (A), ${full.bSpec.toFixed(1)} → ${z.bSpec.toFixed(1)}/s (B)`);
  const w = A.lastWide && parseWide(A.lastWide);
  ok(!!w && w.ver === 2 && w.span === RATE && Math.abs(w.centre - CENTRE) <= RATE / 4 && w.F === 128 && w.C === 1024,
     `SPWB v2: the capture's span ${w?.span} Hz at centre ${w?.centre} Hz, ${w?.F} floor bins, ${w?.C} cells, ${w?.peaks.length} peaks, ${w?.bytes} bytes`);
  if (w) {
    const sorted = [...w.floor].sort((x, y) => x - y), med = sorted[64];
    const sig = [...w.spread].sort((x, y) => x - y)[64] / 10;
    ok(sig > 0.5 && sig < 8, `the floor carries its spread (σ ${sig.toFixed(1)} dB — the speckle the client averages)`);
    ok(w.floor.every((v) => v > 0) && sorted[120] - sorted[8] <= 6, `the floor is the floor: every bin has data, flat across the capture (${sorted[8]}…${sorted[120]}, median ${med})`);
    const binHz = w.span / w.C, lo = w.centre - w.span / 2;
    const at = w.peaks.map((p) => ({ f: lo + (p.cell + ((p.shape & 15) + 0.5) / 16) * binHz, p }));
    const hit = at.filter((x) => Math.abs(x.f - CARRIER) <= binHz / 8);
    ok(hit.length === 1 && (hit[0].p.shape >> 4) <= 1 && hit[0].p.lvl - med >= 20,
       `the one-bin carrier OUTSIDE the view is a THIN peak at its true frequency: ${hit.length ? (hit[0].f / 1e3).toFixed(2) : '—'} kHz (carrier ${CARRIER / 1e3}), width ${hit.length ? (hit[0].p.shape >> 4) + 1 : '—'}/16 of a cell, ${hit.length ? hit[0].p.lvl - med : '—'} over the floor`);
  }
  console.log(`   bandwidth: whole view A ${full.aKBs.toFixed(2)} KB/s, B ${full.bKBs.toFixed(2)} KB/s · zoomed A ${z.aKBs.toFixed(2)} KB/s (with SPWB), B ${z.bKBs.toFixed(2)} KB/s (without) → +${(z.aKBs - z.bKBs).toFixed(2)} KB/s (+${(100 * (z.aKBs - z.bKBs) / z.bKBs).toFixed(1)} %)`);
  // ★ The budget at its worst: a full row (140 peaks) every 4th frame against this listener's SPEC bytes.
  { const specB = z.bKBs * 1024 / Math.max(1, z.bSpec), worst = (32 + 2 * 128 + 110 * 3) / 4;
    ok(worst / specB <= 0.15 && (z.aKBs - z.bKBs) / z.bKBs <= 0.15,
       `within the budget: measured +${(100 * (z.aKBs - z.bKBs) / z.bKBs).toFixed(1)} %; a busy band's worst row (110 peaks) would be +${(100 * worst / specB).toFixed(1)} % of ${specB.toFixed(0)} B/frame`); }

  // ── 4. zoomed back out: SPWB stops ──
  A.send({ type: 'zoom', frequency: CENTRE, binBandwidth: RATE / 1024 });
  await sleep(1500);
  A.reset(); await sleep(3000);
  ok(A.spec > 6 && A.wide === 0, `zoomed back out: ${A.spec} SPEC frames, ${A.wide} SPWB`);
  A.ws.close(); B.ws.close();
} catch (e) { ok(false, `driver: ${e && e.stack || e}`); }
finally {
  clearTimeout(deadline);
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(500);
  if (!fail) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
}
console.log(`   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
