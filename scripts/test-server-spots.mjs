// test-server-spots.mjs — V12 DIGITAL SPOTS through the REAL server (2026-10-26): a fake rtl_tcp replays a synthetic,
// UTC-ANCHORED 20 m clip (`test-digi-spots clip`: three FT8 stations on the FT8 dial, two FT4 on the FT4 dial, one FT8
// station on the FT8 dial's LOWER sideband, a JS8 heartbeat on the JS8 dial, an FT2 CQ on the FT2 dial), and the contract
// on the decoder socket:
//   • vibeserver.json lists the modes (spotModes FT8, FT4, FT2, JS8, WSPR), offers Band (spotsBand true) and names 20 m
//     with its span;
//   • "Digital spots: Band" ({type:'subscribe_digital_spots', scope:'band'}) on a ONE-PIPELINE radio (vibe_wide_tap.h)
//     spots every station with its MODE at its RF, and never the lower-sideband one;
//   • the same on a RANGE-LOCKED radio (per-listener channels: one more slice of the shared FFT);
//   • the owner's switch ("spotsband" blocked): Band greyed with its reason in vibeserver.json, a subscribe refused in words.
// Silent: an audio socket is opened only so the listener has audio, and its frames are dropped unread.
//   VIBESERVER_BIN=…/vibeserver VIBE_DIGI_BIN=…/test-digi-spots node scripts/test-server-spots.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '', DIGI = process.env.VIBE_DIGI_BIN || '';
if (!BIN || !fs.existsSync(BIN) || !DIGI || !fs.existsSync(DIGI)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_DIGI_BIN (the built test-digi-spots, which writes the clip)');
  process.exit(3);
}
const RATE = 960000, CENTRE = 14083700;   // ★ 20 m's Band centre with FT8 FT4 FT2 JS8 WSPR JTTY PSK registered (digi_bands.h spanOf)
const STATIONS = [['FT8', 'G4ABC', 14074600], ['FT8', 'DL1XYZ', 14075450], ['FT8', 'K1ABC', 14076300],
                  ['FT4', 'JA1QRS', 14080800], ['FT4', 'EA4TUV', 14081700],
                  ['JS8', 'G4JSN', 14079000], ['FT2', 'IU8AAA', 14084900],
                  ['JTTY', 'G4JTY', 14091500], ['PSK31', 'G4PSK', 14072400]];   // ★ V12 2026-10-27
const TOL = { FT2: 21, JTTY: 5, PSK31: 6 };   // ★ RF to a quarter of the tone step: FT2's tones are 41.7 Hz apart
const MODES = '["FT8","FT4","FT2","JS8","WSPR","JTTY","PSK"]';
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-spots-'));
const CLIP = path.join(tmp, '20m.cu8');
execFileSync(DIGI, ['clip', CLIP, String(RATE), String(CENTRE)], { stdio: 'ignore' });
// ★★ V12 2026-10-27: 40 m, where JTTY (7.090) lies beyond the 48 kHz cluster and gets an EXTRA slice of its own
const CENTRE40 = 7060600, CLIP40 = path.join(tmp, '40m.cu8');
execFileSync(DIGI, ['clip40', CLIP40, String(RATE), String(CENTRE40)], { stdio: 'ignore' });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 200; i++) { if (await f()) return true; await sleep(200); } return !!(await f()); };

/** A minimal WebSocket client (the probes' own), recording text and binary frames. */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { bin: [], txt: [], open: false };
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
      if (op === 0x2 && !st.dropBin) st.bin.push(Buffer.from(p));
      if (op === 0x1) st.txt.push(p.toString());
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
const jsons = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => JSON.parse(t));
const cwText = (st) => st.bin.filter((b) => b[0] === 0x01 && b.length >= 13).map((b) => b.subarray(13, 13 + b.readUInt32BE(9)).toString()).join('');

async function server(args, tag, radioExtra = {}, clip = CLIP, centre = CENTRE) {
  const dir = fs.mkdtempSync(path.join(tmp, `${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Spots test', locator: 'IO92nh', adminPass: 'spots-test-admin',
    radios: [{ serial: 'spots-test', label: 'Test radio', enabled: true, configured: true, ...radioExtra }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', clip, '--hf-centre', String(centre), '--hf-utc'], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), ...args], { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  return { port, dir, status, log: () => fs.readFileSync(path.join(dir, 'server.log'), 'utf8'), stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}
async function listener(port, sid, f, mode) {
  const L = {};
  L.spec = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`);
  await sleep(400);
  L.aud = ws(port, `/ws/audio?user_session_id=${sid}`); L.aud.dropBin = true;
  L.dx = ws(port, `/ws/dxcluster?user_session_id=${sid}`);
  await sleep(400);
  L.spec.send({ type: 'tune', frequency: f, mode });
  await sleep(600);
  return L;
}
function checkSpots(spots, label) {
  for (const [mode, call, rf] of STATIONS) {
    const s = spots.find((x) => x.data.callsign === call);
    ok(!!s && s.data.mode === mode && Math.abs(s.data.frequency - rf) <= (TOL[mode] || 10) && s.data.band === '20m',
       `${label}: ${call} spotted as ${mode} at ${rf}${s ? ` (got ${s.data.mode} ${s.data.frequency} ${s.data.band})` : ' — not spotted'}`);
  }
  ok(!spots.some((x) => x.data.callsign === 'ZL3LSB'), `${label}: the lower-sideband station is never spotted`);
  // (JTTY and PSK have no slots: their spots carry the moment the frame / the call began)
  // ★ M7EIW: a spot's timestamp is its slot's START (WSJT-X's time) — on FT8/JS8's 15 s, FT4's 7.5 s, FT2's 3.75 s grid
  const P = { FT8: 15000, JS8: 15000, FT4: 7500, FT2: 3750 };
  const off = spots.filter((x) => P[x.data.mode] && x.data.timestamp % P[x.data.mode] !== 0);
  ok(off.length === 0, `${label}: every spot is stamped with its slot's start${off.length ? ' — not: ' + off.map((x) => x.data.mode + ' ' + x.data.timestamp).join(', ') : ''}`);
  const bad = spots.filter((x) => !STATIONS.some(([, c]) => c === x.data.callsign));
  ok(bad.length === 0, `${label}: no false spots ${bad.map((x) => x.data.callsign).join(' ')}`);
}
const allSpotted = (L) => () => STATIONS.every(([, c]) => jsons(L.dx, 'digital_spot').some((x) => x.data.callsign === c));

try {
  {
    console.log('── one-pipeline radio (1 listener): Band ──');
    const S = await server(['--freq', String(CENTRE), '--mode', 'usb', '--users', '1'], 'one');
    try {
      const st = await S.status();
      ok(st && JSON.stringify(st.spotModes) === MODES, `vibeserver.json spotModes ${JSON.stringify(st && st.spotModes)}`);
      ok(st && st.spotsBand === true && !st.spotsBandWhy, `Band offered (spotsBand ${JSON.stringify(st && st.spotsBand)})`);
      const b20 = st && (st.spotBands || []).find((b) => b.id === '20m');
      ok(!!b20 && Math.abs(b20.hz - CENTRE) < 1 && b20.hi - b20.lo > 27000 && b20.hi - b20.lo < 28500
         && JSON.stringify(b20.modes) === MODES,
         `20 m listed: centre ${b20 && b20.hz}, span ${b20 && b20.lo}–${b20 && b20.hi}, modes ${b20 && JSON.stringify(b20.modes)}`);
      ok(st && (st.spotBands || []).map((b) => b.id).join(' ').startsWith('160m 80m') /* ★ WSPR's 2200 m / 630 m lie below this radio */, `bands in step order: ${st && (st.spotBands || []).map((b) => b.id).join(' ')}`);
      const L = await listener(S.port, 'spone001', CENTRE, 'usb');
      L.dx.send({ type: 'subscribe_digital_spots', scope: 'band' });
      ok(await until(allSpotted(L), 50000), `every station spotted within 50 s (${jsons(L.dx, 'digital_spot').map((x) => x.data.mode + ' ' + x.data.callsign).join(', ')})`);
      await sleep(1500);
      checkSpots(jsons(L.dx, 'digital_spot'), 'one-pipeline Band');
      // ★★ V12 governor: the mode chips, and JTTY/PSK saying what the station was doing (+ every spot's country)
      const sts = jsons(L.dx, 'digital_spots_status');
      const last = sts.length ? sts[sts.length - 1].data : null;
      ok(!!last && last.scope === 'band' && last.modes.length === 7 && last.modes.every((m) => ['active', 'light', 'idle', 'shed', 'off'].includes(m.state)),
         `the spots box's mode chips arrive (${last ? last.modes.map((m) => m.mode + ' ' + m.state).join(', ') : 'none'})`);
      ok(!!last && last.modes.some((m) => m.mode === 'WSPR' && m.live === 0), 'WSPR\'s empty window was never decoded (the activity gate, from the slice\'s own bins)');
      const j = jsons(L.dx, 'digital_spot').find((x) => x.data.callsign === 'G4JTY'), p = jsons(L.dx, 'digital_spot').find((x) => x.data.callsign === 'G4PSK');
      ok(j && j.data.how === 'CQ' && /England/.test(j.data.country), `JTTY's spot says CQ and the country (${j && j.data.how} ${j && j.data.country})`);
      ok(p && (p.data.how === 'CQ' || p.data.how === 'DE') && p.data.mode === 'PSK31', `PSK's spot is PSK31, how ${p && p.data.how}`);
      ok(/digital spots: Band started/.test(S.log()), 'the server logged "digital spots: Band started"');
      // ★ Single on the same radio: back to the button there always was — the listener's own audio (FT8 dial in USB).
      L.dx.txt.length = 0;
      L.spec.send({ type: 'tune', frequency: 14074000, mode: 'usb' });
      L.dx.send({ type: 'subscribe_digital_spots' });
      ok(await until(() => jsons(L.dx, 'digital_spot').some((x) => x.data.callsign === 'G4ABC'), 40000), 'Single mode (dial 14.074 USB): G4ABC spotted on the listener\'s audio');
      const g = jsons(L.dx, 'digital_spot').find((x) => x.data.callsign === 'G4ABC');
      ok(g && g.data.mode === 'FT8' && Math.abs(g.data.frequency - 14074600) <= 10, `Single: FT8 at ${g && g.data.frequency}`);
      ok(/digital spots: Single mode started/.test(S.log()), 'the server logged "digital spots: Single mode started"');
      for (const k of ['spec', 'aud', 'dx']) L[k].close();
    } finally { S.stop(); }
  }
  {
    console.log('── range-locked radio (per-listener channels): Band ──');
    const S = await server(['--freq', String(CENTRE), '--lock-freq', String(CENTRE), '--users', '4', '--mode', 'usb', '--idle-grace', '0'], 'lock');
    try {
      const A = await listener(S.port, 'splock01', CENTRE, 'usb');
      A.dx.send({ type: 'subscribe_digital_spots', scope: 'band' });
      ok(await until(allSpotted(A), 50000), `every station spotted within 50 s (${jsons(A.dx, 'digital_spot').map((x) => x.data.mode + ' ' + x.data.callsign).join(', ')})`);
      await sleep(1500);
      checkSpots(jsons(A.dx, 'digital_spot'), 'per-listener Band');
      for (const k of ['spec', 'aud', 'dx']) A[k].close();
    } finally { S.stop(); }
  }
  for (const [layout, extra] of [['one-pipeline', ['--users', '1']], ['range-locked', ['--lock-freq', String(CENTRE40), '--users', '4', '--idle-grace', '0']]]) {
    // ★★ V12 2026-10-27 (Stuart: "we need to get JTTY to the full band decoder mode"): 40 m's JTTY on its own slice
    console.log(`── ${layout} radio: 40 m Band — JTTY on the band's extra slice ──`);
    const S = await server(['--freq', String(CENTRE40), '--mode', 'usb', ...extra], '40' + layout.slice(0, 3), {}, CLIP40, CENTRE40);
    try {
      const st = await S.status();
      const b40 = st && (st.spotBands || []).find((b) => b.id === '40m');
      ok(!!b40 && b40.modes.includes('JTTY') && Array.isArray(b40.extra) && b40.extra.some(([lo, hi]) => lo < 7090100 && hi > 7093450),
         `40 m lists JTTY, with its extra slice (${b40 && JSON.stringify(b40.modes)} ${b40 && JSON.stringify(b40.extra)})`);
      const L = await listener(S.port, 'sp40' + layout.slice(0, 4), CENTRE40, 'usb');
      L.dx.send({ type: 'subscribe_digital_spots', scope: 'band' });
      const got = (c) => jsons(L.dx, 'digital_spot').find((x) => x.data.callsign === c);
      ok(await until(() => got('G4JTY') && got('G4ABC'), 50000), `${layout}: FT8 G4ABC and JTTY G4JTY spotted (${jsons(L.dx, 'digital_spot').map((x) => x.data.mode + ' ' + x.data.callsign + ' ' + x.data.frequency).join(', ')})`);
      const j = got('G4JTY');
      ok(j && j.data.mode === 'JTTY' && Math.abs(j.data.frequency - 7091500) <= 5 && j.data.band === '40m', `${layout}: JTTY at its RF on 40 m (${j && j.data.frequency})`);
      await sleep(1200);
      const sts = jsons(L.dx, 'digital_spots_status'); const last = sts.length ? sts[sts.length - 1].data : null;
      const jc = last && last.modes.find((m) => m.mode === 'JTTY');
      ok(!!jc && ['active', 'light', 'idle'].includes(jc.state), `${layout}: JTTY has its chip in the Band row (${last ? last.modes.map((m) => m.mode + ' ' + m.state).join(', ') : 'none'})`);
      for (const k of ['spec', 'aud', 'dx']) L[k].close();
    } finally { S.stop(); }
  }
  {
    // ★★ V12 governor: the owner's word per mode — PSK OFF in Band, WSPR ALWAYS (decoded though its window is empty)
    console.log('── the owner\'s word per mode (spotsband-psk, spotsband+wspr) ──');
    const S = await server(['--freq', String(CENTRE), '--mode', 'usb', '--users', '1'], 'own', { blockedModes: 'spotsband-psk,spotsband+wspr' });
    try {
      const L = await listener(S.port, 'spown001', CENTRE, 'usb');
      L.dx.send({ type: 'subscribe_digital_spots', scope: 'band' });
      ok(await until(() => jsons(L.dx, 'digital_spot').some((x) => x.data.callsign === 'G4JTY'), 50000), 'Band runs (JTTY spotted)');
      await sleep(16000);   // ★ PSK's call would long have been heard twice
      const sp = jsons(L.dx, 'digital_spot');
      ok(!sp.some((x) => x.data.callsign === 'G4PSK'), 'PSK switched off for Band: G4PSK never spotted');
      const sts = jsons(L.dx, 'digital_spots_status'); const last = sts.length ? sts[sts.length - 1].data : null;
      const m = (n) => last && last.modes.find((x) => x.mode === n);
      ok(m('PSK') && m('PSK').state === 'off', `PSK's chip says off (${m('PSK') && m('PSK').state})`);
      ok(m('WSPR') && (m('WSPR').state === 'active' || m('WSPR').state === 'light'), `WSPR ALWAYS: decoding though its window is empty (${m('WSPR') && m('WSPR').state})`);
      for (const k of ['spec', 'aud', 'dx']) L[k].close();
    } finally { S.stop(); }
  }
  {
    console.log('── the owner switched Band off ("spotsband") ──');
    const S = await server(['--freq', String(CENTRE), '--mode', 'usb', '--users', '1'], 'off', { blockedModes: 'spotsband' });
    try {
      const st = await S.status();
      ok(st && st.spotsBand === false && /switched Band spots off/.test(st.spotsBandWhy || ''), `greyed with its reason: "${st && st.spotsBandWhy}"`);
      ok(st && (st.spotBands || []).length > 0, 'the bands are still listed (the button is shown greyed, never hidden)');
      const L = await listener(S.port, 'spoff001', CENTRE, 'usb');
      L.dx.send({ type: 'subscribe_digital_spots', scope: 'band' });
      ok(await until(() => jsons(L.dx, 'decoder_refused').length > 0, 5000), 'a Band subscribe is refused');
      const r = jsons(L.dx, 'decoder_refused')[0];
      ok(r && r.what === 'spots' && /Band spots are not available/.test(r.message), `…in words: "${r && r.message}"`);
      for (const k of ['spec', 'aud', 'dx']) L[k].close();
    } finally { S.stop(); }
  }
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });   // ★ the 29 MB clip and the servers' folders
}
console.log(`\n${fail ? '✗' : '✓'} test-server-spots: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
