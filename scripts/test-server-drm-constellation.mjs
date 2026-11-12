// test-server-drm-constellation.mjs — V12 THE DRM CONSTELLATION through the REAL server (2026-11-12, docs/v12/DRM.md
// "The constellation"). A fake rtl_tcp replays a SYNTHETIC DRM signal (vibeserver/drm-synth-clip.cpp: the test
// transmitter, mode B, 16-QAM MSC, 4-QAM SDC, 24 dB) 30 kHz above the dongle's centre — so this runs on any machine, with
// no off-air recording. The contract (local_sdr_shim.cpp drm_scopes, vibe_drm_listener.h setCellsSink):
//   • a listener who has NOT asked gets no {"type":"drm_iq"} at all — an older client never sees the new message;
//   • {"type":"drm_scopes","on":1} → drm_iq about once per 1.2 s super frame (never faster), each within the budget
//     (≤ 256 MSC, 64 SDC, 65 FAC points), every value an integer in ±127, with the scale and the QAM sizes;
//   • the cells CLUSTER on the QAM grid: 16 MSC clusters, 4 FAC and 4 SDC, tight at 24 dB;
//   • {"on":0} → they stop; a newly connected listener starts with them off until it asks;
//   • on a SHARED DIAL one listener's pane does not send them to another who did not ask;
//   • on a RANGE-LOCKED radio (each listener's own receiver) likewise;
//   • the `drm` report carries the decoder's SNR the meter shows (snr), locked.
// Silent: audio frames are counted, never played.
//   VIBESERVER_BIN=…/vibeserver VIBE_DRM_SYNTH=…/drm-synth.cu8 node scripts/test-server-drm-constellation.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_DRM_SYNTH || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) { console.log('   not run — set VIBESERVER_BIN and VIBE_DRM_SYNTH'); process.exit(3); }
const RATE = 288000, CENTRE = 9540000, STATION = 9570000;
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 200; i++) { if (await f()) return true; await sleep(200); } return !!(await f()); };

/** A minimal WebSocket client (test-server-drm.mjs's), recording text frames and counting binary ones. */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], n: 0, open: false };
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
      if (op === 0x2) st.n++;
      if (op === 0x1) st.txt.push({ t: Date.now(), s: p.toString() });
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
const msgs = (st, type, since = 0) => st.txt.filter((m) => m.t >= since && m.s.includes(`"type":"${type}"`)).map((m) => ({ t: m.t, ...JSON.parse(m.s) }));

async function server(args, tag) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-drmc-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'DRM constellation test', locator: 'IO92nh', adminPass: 'drmc-test-admin',
    radios: [{ serial: 'drmc', label: 'Test radio', enabled: true, configured: true }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', CLIP, '--hf-centre', String(CENTRE)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), ...args], { env, stdio: ['ignore', log, log] });
  const status = async () => { try { return await (await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) })).json(); } catch { return null; } };
  const up = await until(async () => !!(await status()), 15000);
  return { up, port, stop: async () => {
    srv.kill('SIGTERM'); rtl.kill('SIGTERM');
    if (process.env.VIBE_KEEP) { console.log('   kept', dir); return; }
    await sleep(500); try { fs.rmSync(dir, { recursive: true, force: true }); } catch {}
  } };
}
async function listener(port, sid) {
  const L = {};
  L.spec = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`);
  await sleep(400);
  L.aud = ws(port, `/ws/audio?user_session_id=${sid}`);
  await sleep(400);
  L.spec.send({ type: 'tune', frequency: STATION, mode: 'drm' });
  await sleep(600);
  L.close = () => { L.spec.close(); L.aud.close(); };
  return L;
}

/** Nearest ideal point of a unit-power `qam` grid (in the message's units, unit power = scale) → [cluster, distance]. */
function nearest(qam, scale, x, y) {
  const lv = qam === 64 ? 3 : qam === 16 ? 2 : 1, m = (1 << lv) - 1;
  const a = scale / Math.sqrt(qam === 64 ? 42 : qam === 16 ? 10 : 2);
  const ax = (v) => { const r = Math.min(m, Math.max(0, Math.round((m - v / a) / 2))); return [r, (m - 2 * r) * a]; };
  const [ri, ix] = ax(x), [rq, iy] = ax(y);
  return [ri * (m + 1) + rq, Math.hypot(x - ix, y - iy), a];
}
function judge(arr, qam, scale) {
  const cl = new Set(); let s2 = 0, n = 0, half = 0;
  for (let i = 0; i + 1 < arr.length; i += 2) { const [c, d, a] = nearest(qam, scale, arr[i], arr[i + 1]); cl.add(c); s2 += d * d; n++; half = a; }
  return { n, clusters: cl.size, rms: n ? Math.sqrt(s2 / n) : 0, half };
}

async function scenario(label, args, tag) {
  console.log(`── ${label} ──`);
  const S = await server(args, tag);
  try {
    ok(S.up, 'the server is up');
    const A = await listener(S.port, `drmc${tag}a01`);
    const lastDrm = () => { const r = msgs(A.spec, 'drm'); return r[r.length - 1]; };
    ok(await until(() => lastDrm() && lastDrm().locked && lastDrm().msc, 45000), `locked, the MSC decoding (${JSON.stringify(lastDrm() || {}).slice(0, 160)})`);
    const r = lastDrm() || {};
    ok(r.label === 'VIBE TEST' && r.mscQam === 16 && typeof r.snr === 'number' && r.snr > 15,
       `the synthetic station: "${r.label}", ${r.mscQam}-QAM, the decoder's SNR ${r.snr} dB (what the meter's box shows)`);
    // ── nobody asked: nothing ──
    await sleep(4000);
    ok(msgs(A.spec, 'drm_iq').length === 0, `not asked: no drm_iq in ${((Date.now() - A.spec.txt[0].t) / 1000).toFixed(0)} s of DRM`);
    // ── a second listener (shared dial / range-locked), never asking ──
    const B = tag === 'one' ? null : await listener(S.port, `drmc${tag}b02`);
    // ── asked ──
    const t0 = Date.now();
    A.spec.send({ type: 'drm_scopes', on: 1 });
    ok(await until(() => msgs(A.spec, 'drm_iq').length > 0, 4000), `asked: the first drm_iq after ${((msgs(A.spec, 'drm_iq')[0]?.t ?? Date.now()) - t0)} ms`);
    await sleep(8000);
    const iq = msgs(A.spec, 'drm_iq', t0);
    const span = (iq[iq.length - 1].t - iq[0].t) / 1000;
    ok(iq.length >= 6 && iq.length <= 9 && (iq.length - 1) / span < 1.0,
       `about one per 1.2 s super frame: ${iq.length} in ${span.toFixed(1)} s (${((iq.length - 1) / span).toFixed(2)}/s)`);
    const within = iq.every((m) => (m.msc?.length ?? 0) <= 512 && (m.sdc?.length ?? 0) <= 128 && (m.fac?.length ?? 0) <= 130
      && [m.msc, m.sdc, m.fac].every((a) => !a || (a.length % 2 === 0 && a.every((v) => Number.isInteger(v) && v >= -127 && v <= 127))));
    const bytes = Math.max(...A.spec.txt.filter((m) => m.t >= t0 && m.s.includes('"drm_iq"')).map((m) => m.s.length));
    ok(within && bytes < 3200, `every one within the budget (≤ 256 / 64 / 65 points, ±127): the largest ${bytes} bytes`);
    const L = iq[iq.length - 1];
    ok(L.scale === 60 && L.mscQam === 16 && L.sdcQam === 4 && L.msc?.length === 512 && L.sdc?.length === 128 && L.fac?.length === 130,
       `the scale and the QAM sizes: scale ${L.scale}, MSC ${L.mscQam}-QAM ×${(L.msc?.length ?? 0) / 2}, SDC ${L.sdcQam}-QAM ×${(L.sdc?.length ?? 0) / 2}, FAC ×${(L.fac?.length ?? 0) / 2}`);
    const m = judge(L.msc || [], 16, L.scale), sd = judge(L.sdc || [], 4, L.scale), fa = judge(L.fac || [], 4, L.scale);
    ok(m.clusters === 16 && m.rms < 0.5 * m.half, `the MSC clusters on the 16-QAM grid: ${m.clusters}/16, rms ${m.rms.toFixed(1)} against a half-spacing of ${m.half.toFixed(1)}`);
    ok(sd.clusters === 4 && fa.clusters === 4 && fa.rms < 0.5 * fa.half, `SDC ${sd.clusters}/4, FAC ${fa.clusters}/4 (rms ${fa.rms.toFixed(1)})`);
    if (B) ok(msgs(B.spec, 'drm_iq').length === 0, 'the other listener, who did not ask, got none');
    // ── off ──
    A.spec.send({ type: 'drm_scopes', on: 0 });
    await sleep(1500);
    const tOff = Date.now();
    await sleep(4000);
    ok(msgs(A.spec, 'drm_iq', tOff).length === 0, 'drm_scopes off: they stop');
    ok(lastDrm() && lastDrm().locked && msgs(A.spec, 'drm', tOff).length >= 2, 'the ordinary reports carry on');
    // ── a socket that closes while looking ──
    if (B) {
      B.spec.send({ type: 'drm_scopes', on: 1 });
      ok(await until(() => msgs(B.spec, 'drm_iq').length > 0, 4000), 'the other listener asks: it gets them');
      const tB = Date.now(); await sleep(3000);
      ok(msgs(A.spec, 'drm_iq', tB).length === 0, '…and the first, who switched off, does not');
      B.close(); await sleep(500);
      A.spec.send({ type: 'drm_scopes', on: 0 });   // (still off — restating is harmless)
    }
    A.close();
  } finally { await S.stop(); }
}

await scenario('one-pipeline radio (the shared receiver, one listener)', ['--freq', String(STATION), '--mode', 'drm', '--users', '1'], 'one');
await scenario('shared dial (one receiver, two listeners)', ['--freq', String(STATION), '--mode', 'drm', '--users', '4'], 'dial');
await scenario('range-locked radio (each listener\'s own receiver)', ['--freq', String(CENTRE), '--lock-freq', String(CENTRE), '--users', '4', '--mode', 'am', '--idle-grace', '0'], 'lock');

console.log(`\n${fail ? '✗' : '✓'} test-server-drm-constellation: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
