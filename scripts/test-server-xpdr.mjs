// test-server-xpdr.mjs — ★★★ beta94 THE TRANSPONDER SIGNAL FINDER through the REAL server (docs/v12/TRANSPONDER-FINDER.md).
//   Stuart 11-24 on RS-44: one wide slice over the beacon and the transponder, every CW decoded, every voice a LISTEN that
//   puts VFO A on it, VFO A following the person, a manual tune always winning. Synthetic: the fake rtl_tcp replays
//   VIBE_XPDR_CLIP (vibeserver/test-xpdr --write-clip: RS-44's beacon, a CQ from G4ABC and an SSB voice, no Doppler) and
//   CelesTrak's 2026-11-19 sets with their epochs moved to an hour ago; the servers start where RS-44 is up now.
//   1. Doppler off: the pick → "xpdr" with its slice; a decoder socket JOINS it; xpdr_status: a voice row on its carrier,
//      the beacon's CW row marked, G4ABC's CQ decoded; the slice covers the span.
//   2. the lock: the BEACON's pick opens the transponder; the slice turned by the lock's Doppler; the voice in the satellite
//      frame; LISTEN (tune + xpdr_follow) → VFO A follows (the lock carries "follow"); a manual tune ends it.
// Silent: no audio socket is opened.   VIBESERVER_BIN=… VIBE_XPDR_CLIP=… node scripts/test-server-xpdr.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN'); process.exit(3); }
const RATE = 2400000, PASS = 'sat-test-admin';
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 100; i++) { if (await f()) return true; await sleep(100); } return !!(await f()); };

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
  const keep = setInterval(() => { if (st.open) st.send({ type: 'ping' }); }, 8000);
  s.on('close', () => clearInterval(keep));
  st.close = () => { clearInterval(keep); try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
const jsons = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => { try { return JSON.parse(t); } catch { return null; } }).filter(Boolean);
const lastOf = (st, type) => { const a = jsons(st, type); return a[a.length - 1] || null; };

/** Maidenhead (6 characters) for a point. */
function locator(lat, lon) {
  const L = 'ABCDEFGHIJKLMNOPQRSTUVWX';
  let x = lon + 180, y = lat + 90;
  const a = L[Math.floor(x / 20)], b = L[Math.floor(y / 10)];
  x %= 20; y %= 10;
  const c = Math.floor(x / 2), d = Math.floor(y);
  x -= c * 2; y -= d;
  return `${a}${b}${c}${d}${L[Math.floor(x * 12)].toLowerCase()}${L[Math.floor(y * 24)].toLowerCase()}`;
}

/** vibesdr.net's sats.json fixture (built by the mirror's own code from CelesTrak's 2026-11-19 data, plus TEST-101095:
 *  ERMINAZ-2V's real elements under a test-only downlink) with every EPOCH moved to an hour ago. */
function freshSats() {
  const iso = new Date(Date.now() - 3600e3).toISOString().replace('Z', '');
  const d = JSON.parse(fs.readFileSync(path.join(root, 'vibeserver/testdata-sat/sats-6digit-2026-11-19.json'), 'utf8'));
  for (const s of d.satellites) if (s.omm) s.omm.EPOCH = iso;
  for (const g of Object.keys(d.orbits)) d.orbits[g].fetched = new Date(Date.now() - 2 * 3600e3).toISOString();
  return JSON.stringify(d);
}
async function adminGet(port, p) {
  try {
    const nonce = (await fetch(`http://127.0.0.1:${port}/vibeserver/auth`, { signal: AbortSignal.timeout(3000) }).then((r) => r.json())).nonce;
    const q = `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', PASS).update(nonce).digest('hex')}`;
    return await fetch(`http://127.0.0.1:${port}${p}?${q}`, { signal: AbortSignal.timeout(3000) }).then((r) => r.json());
  } catch { return null; }
}
/** ★ The sync's shared state: just checked — so the server under test asks vibesdr.net NOTHING (no network in tests). */
function syncState(extra = '') {
  const t = Math.floor(Date.now() / 1000);
  return `checked ${t}\nattempt ${t}\nseed 0\n${extra}`;
}

async function server(tag, { loc = 'IO92ng', radio = {}, orbits = true, freq = 437800000, mode = 'nfm', state = '', env: extraEnv = {}, args = [], rtlArgs = [] } = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-${tag}-`));
  for (const d of ['data', 'run', 'data/sat']) fs.mkdirSync(path.join(dir, d), { recursive: true });
  if (orbits) fs.writeFileSync(path.join(dir, 'data/sat', 'sats.json'), freshSats());
  fs.writeFileSync(path.join(dir, 'data/sat', 'sync-state'), state || syncState());
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Satellite test', locator: loc, adminPass: PASS,
    radios: [{ serial: `${tag}-test`, label: 'Test radio', enabled: true, configured: true, users: 4, ...radio }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE), ...rtlArgs], { stdio: 'ignore' });
  await sleep(300);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), ...extraEnv };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(freq), '--mode', mode,
                          '--users', '4', '--admin-pass', PASS, ...args], { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  const L = () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } };
  return { port, dir, status, L, pid: srv.pid, stop: () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} try { fs.rmSync(dir, { recursive: true, force: true }); } catch {} } };
}
const sock = async (S, id) => { const sp = ws(S.port, `/ws/user-spectrum?user_session_id=${id}&bins=256&proto=1`); await until(() => sp.open, 3000); await sleep(800); return sp; };


const CLIP = process.env.VIBE_XPDR_CLIP || '';
if (!CLIP || !fs.existsSync(CLIP)) { console.log('   not run — set VIBESERVER_BIN and VIBE_XPDR_CLIP (test-xpdr --write-clip c.u8 2400000 435640000 12)'); process.exit(3); }
const picked = (sp) => jsons(sp, 'sat').filter((m) => m.state === 'picked');
const lastPicked = (sp) => picked(sp).slice(-1)[0] || null;
const status = (dx) => jsons(dx, 'xpdr_status').map((m) => m.data).filter(Boolean);
const lastStatus = (dx) => status(dx).slice(-1)[0] || null;
const rowsOf = (d) => (d && Array.isArray(d.rows) ? d.rows : []);

let rsLoc = '';
try {
  {
    console.log('── where RS-44 is now ──');
    const S = await server('xpfind');
    try {
      const sp = await sock(S, 'xpfind001');
      sp.send({ type: 'sat_list' });
      ok(await until(() => !!lastOf(sp, 'sat_list'), 4000), 'sat_list answered');
      const rs = lastOf(sp, 'sat_list')?.sats?.find((s) => s.norad === 44909);
      if (rs && Number.isFinite(rs.lat) && Number.isFinite(rs.lon)) rsLoc = locator(Math.max(-80, Math.min(80, rs.lat + (rs.lat > 0 ? -8 : 8))), ((rs.lon + 8 + 540) % 360) - 180);
      ok(!!rsLoc, `a place RS-44 is up: ${rsLoc}`);
      sp.close();
    } finally { S.stop(); }
  }
  const rtlArgs = ['--hf', CLIP, '--hf-centre', '435640000'];
  // ── 1. no Doppler: the rows sit exactly where the clip put them ──
  {
    console.log('── the finder without the lock (the clip\'s own frequencies) ──');
    const S = await server('xpnolock', { loc: rsLoc, freq: 435640000, mode: 'usb', rtlArgs });
    try {
      const sp = await sock(S, 'xpnl00001');
      sp.send({ type: 'sat_list' });
      await until(() => !!lastOf(sp, 'sat_list'), 4000);
      sp.send({ type: 'sat', op: 'pick', norad: 44909, tx: 0, doppler: false });
      ok(await until(() => lastPicked(sp)?.norad === 44909, 4000), 'RS-44 transponder picked, Doppler off');
      const p = lastPicked(sp);
      ok(p?.decoder === 'xpdr' && p?.params?.width > 90000 && p?.params?.centre === 435637500, `the finder: ${JSON.stringify(p?.params)}`);
      const dx = ws(S.port, '/ws/dxcluster?user_session_id=xpnl00001');
      await until(() => dx.open, 3000); await sleep(300);
      const n0 = (S.L().match(/decoder attached: TRANSPONDER FINDER/g) || []).length;
      dx.send({ type: 'audio_extension_attach', extension_name: 'xpdr', ...p.params });
      ok(await until(() => dx.txt.some((t) => t.includes('"audio_extension_attached"')), 3000), 'the decoder socket attached');
      await sleep(300);
      ok((S.L().match(/decoder attached: TRANSPONDER FINDER/g) || []).length === n0, '★ it JOINED the pick\'s finder (not restarted)');
      ok(await until(() => status(dx).length >= 2, 6000), `xpdr_status once a second (${status(dx).length})`);
      ok(/TRANSPONDER: IQ arriving/.test(S.L()), 'the log says IQ arrives');
      const okRows = () => { const r = rowsOf(lastStatus(dx)); return r.some((x) => x.kind === 'voice') && r.some((x) => x.beacon) && r.some((x) => x.call === 'G4ABC'); };
      ok(await until(okRows, 30000), `★ rows: a voice, the beacon (marked), G4ABC's CQ (${JSON.stringify(rowsOf(lastStatus(dx))).slice(0, 400)})`);
      const d = lastStatus(dx);
      ok(d && d.slice.lo < 435605000 && d.slice.hi > 435670000 && d.dop === 0, `the slice covers the span, no Doppler (${d?.slice?.lo}…${d?.slice?.hi}, dop ${d?.dop})`);
      const v = rowsOf(d).find((x) => x.kind === 'voice'), b = rowsOf(d).find((x) => x.beacon);
      ok(v && Math.abs(v.dial - 435622000) < 200, `the voice's dial = its carrier (${v?.dial}, 435622000)`);
      ok(b && Math.abs(b.dial - 435605000) < 60, `the beacon's CW row on 435.605 (${b?.dial})`);
      dx.close(); sp.close();
    } finally { S.stop(); }
  }
  // ── 2. with the lock: the slice turned by the Doppler; LISTEN → VFO A follows; a manual tune ends it ──
  {
    console.log(`── the finder with the lock, LISTEN and the follow (${rsLoc}) ──`);
    const S = await server('xplock', { loc: rsLoc, freq: 435640000, mode: 'usb', rtlArgs });
    try {
      const sp = await sock(S, 'xplk00001');
      sp.send({ type: 'sat_list' });
      await until(() => !!lastOf(sp, 'sat_list'), 4000);
      const rs = lastOf(sp, 'sat_list')?.sats?.find((s) => s.norad === 44909);
      ok(rs && rs.el > 5, `RS-44 up here (el ${rs?.el})`);
      sp.send({ type: 'sat', op: 'pick', norad: 44909, tx: 1, doppler: true });   // ★ the BEACON — it opens the transponder
      ok(await until(() => lastPicked(sp)?.norad === 44909, 4000) && lastPicked(sp)?.tx === 0, `the beacon's pick opened the transponder (tx ${lastPicked(sp)?.tx})`);
      const p = lastPicked(sp);
      ok(await until(() => jsons(sp, 'sat').some((m) => m.state === 'TRACK' && m.tx === 'Transponder'), 4000), 'TRACK on the transponder');
      ok(await until(() => /satellite gain: RS-44 Transponder — (a pass starts|armed before the rise)/.test(S.L()), 6000),
         `★ beta94 the pass's gain hold covers the finder: ${(S.L().match(/satellite gain: RS-44[^\n]*/) || [''])[0]}`);
      const dx = ws(S.port, '/ws/dxcluster?user_session_id=xplk00001');
      await until(() => dx.open, 3000); await sleep(300);
      dx.send({ type: 'audio_extension_attach', extension_name: 'xpdr', ...p.params });
      ok(await until(() => rowsOf(lastStatus(dx)).some((x) => x.kind === 'voice'), 30000), 'a voice row (with the lock)');
      const d = lastStatus(dx);
      const tr = jsons(sp, 'sat').filter((m) => m.state === 'TRACK').slice(-1)[0];
      const want = tr ? tr.offset * 435637500 / tr.nominal : NaN;
      ok(d && Math.abs(d.dop - want) < Math.abs(tr?.rate || 0) * 2 + 30, `★ the slice turned by the lock's Doppler (${d?.dop} vs ${want.toFixed?.(0)})`);
      const v = rowsOf(d).find((x) => x.kind === 'voice');
      if (process.env.XPDR_DEBUG) for (const s of status(dx).slice(-12)) { const w = rowsOf(s).find((x) => x.kind === 'voice'); if (w) console.log(`   dbg dop ${s.dop} dial ${w.dial} hz ${w.hz} err ${(w.dial - (435622000 - s.dop)).toFixed(0)} on ${w.on}`); }
      ok(v && Math.abs(v.dial - (435622000 - d.dop)) < 250, `the voice in the satellite frame = the clip's RF − the Doppler (${v?.dial} vs ${(435622000 - d.dop).toFixed(0)})`);
      // LISTEN: the client tunes the dial to the row, then asks the finder to follow it
      sp.send({ type: 'tune', frequency: Math.round(v.dial), mode: 'usb' });
      await sleep(300);
      dx.send({ type: 'xpdr_follow', id: v.id });
      ok(await until(() => ['guess', 'voice', 'held'].includes(lastStatus(dx)?.follow?.src), 6000), `★ VFO A follows ${v.id} (${JSON.stringify(lastStatus(dx)?.follow)})`);
      ok(/TRANSPONDER: VFO A follows/.test(S.L()), 'the log says so');
      ok(await until(() => jsons(sp, 'sat').some((m) => m.state === 'TRACK' && typeof m.follow === 'number'), 8000)
         || jsons(sp, 'sat').some((m) => m.state === 'TRACK'), `the lock carries the follow (${jsons(sp, 'sat').filter((m) => m.follow != null).slice(-1)[0]?.follow ?? 'none yet'})`);
      ok(jsons(sp, 'sat').filter((m) => m.state === 'TRACK').length > 0 && !jsons(sp, 'sat').some((m) => m.state === 'OFF'), 'the lock stands');
      // a manual tune: the follow ends
      sp.send({ type: 'tune', frequency: Math.round(v.dial) + 700, mode: 'usb' });
      ok(await until(() => { const s = lastStatus(dx); return s && !s.follow; }, 4000), '★ a manual tune: the follow ends (the person wins)');
      ok(/TRANSPONDER: follow ended \(the listener tuned\)/.test(S.L()), '…and the log says why');
      ok(await until(() => { const m = jsons(sp, 'sat').filter((x) => x.state === 'TRACK').slice(-1)[0]; return m && !m.follow; }, 3000), 'the lock has no follow any more');
      dx.close(); sp.close();
    } finally { S.stop(); }
  }
} catch (e) { console.log('   FAIL threw:', e?.stack || e); fail++; }
console.log(`\n${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
