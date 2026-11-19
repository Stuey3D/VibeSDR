// test-server-vorsar.mjs — V12 VOR / ILS and 406 MHz beacons through the REAL server (vibe_nav_run.h, vibe_sarsat_run.h,
// docs/v12/VOR-ILS.md, docs/v12/SARSAT.md): a fake rtl_tcp replaying our own synthetic clips —
//   • a VOR "LON" on radial 247.5° 25 kHz above the clip's centre (test-vor clip), tuned 113.60 MHz AM: the radial, the
//     ident, the published station (LON London) and its variation, on a one-pipeline radio and per-listener; the owner's
//     "the radial passes … from this server" only when signed in;
//   • three 406 MHz bursts (test-sarsat clip: a standard-location EPIRB with a position, an aviation-user ELT, a self-test),
//     tuned 406.037 MHz: on a SHARED dial the signed-in owner gets the Hex ID, the MMSI, the registration and the position;
//     a listener's mirror of the same decoder never does (★ checked against everything it was sent); per-listener, nobody
//     signed in: the public line only;
//   • the owner's switches make vibeserver.json false and refuse the attach.
// Silent: no audio is played (the audio socket's frames are dropped unread). Every socket sends the client's ping.
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const VOR_CLIP = process.env.VIBE_VOR_CLIP || '', SAR_CLIP = process.env.VIBE_SARSAT_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !VOR_CLIP || !fs.existsSync(VOR_CLIP) || !SAR_CLIP || !fs.existsSync(SAR_CLIP)) {
  console.log('   not run — set VIBESERVER_BIN, VIBE_VOR_CLIP (test-vor clip vor.cu8 288000 40 25000 247.5) and VIBE_SARSAT_CLIP (test-sarsat clip sar.cu8 288000 25000)');
  process.exit(3);
}
const RATE = 288000, PASS = 'vorsar-test-admin';
const VOR = 113600000, SAR = 406037000;   // ★ each clip's signal is 25 kHz above its centre
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 200; i++) { if (await f()) return true; await sleep(200); } return !!(await f()); };

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
  // ★ THE REAL CLIENT'S KEEPALIVE: the server releases a proto-1 session silent for 30 s
  const keep = setInterval(() => { if (st.open) st.send({ type: 'ping' }); }, 8000);
  s.on('close', () => clearInterval(keep));
  st.close = () => { clearInterval(keep); try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
const jsons = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => { try { return JSON.parse(t); } catch { return null; } }).filter(Boolean);
const lastOf = (st, type) => jsons(st, type).at(-1) || null;
const text01 = (st) => st.bin.filter((b) => b[0] === 0x01 && b.length >= 13).map((b) => b.subarray(13, 13 + b.readUInt32BE(9)).toString()).join('');
/** Everything a socket was sent, as one string (text frames and binary frames as latin1) — for "never sent" checks. */
const everything = (st) => st.txt.join('\n') + '\n' + st.bin.map((b) => b.toString('latin1')).join('\n');

async function server(args, tag, centre, clip, radioExtra = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-vorsar-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'VOR test', locator: 'IO92nh', adminPass: PASS,
    radios: [{ serial: `vorsar-${tag}`, label: 'Test radio', enabled: true, configured: true, ...radioExtra }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', clip, '--hf-centre', String(centre)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--admin-pass', PASS, ...args],
                    { env, stdio: ['ignore', log, log] });
  const getJson = async (p) => { try { const r = await fetch(`http://127.0.0.1:${port}${p}`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  const status = () => getJson('/vibeserver.json');
  await until(async () => !!(await status()), 15000);
  return { port, dir, status, getJson, stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}
async function listener(port, sid, dial, mode = 'am') {
  const L = {};
  L.spec = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256&proto=1`);
  await sleep(400);
  L.aud = ws(port, `/ws/audio?user_session_id=${sid}`); L.aud.dropBin = true;   // ★ silent: frames dropped unread
  L.dx = ws(port, `/ws/dxcluster?user_session_id=${sid}`);
  await sleep(400);
  L.spec.send({ type: 'tune', frequency: dial, mode });
  await sleep(600);
  return L;
}
async function signIn(S, sock) {
  const nonce = (await S.getJson('/vibeserver/auth'))?.nonce || '';
  sock.send({ type: 'admin_unlock', nonce, token: crypto.createHmac('sha256', PASS).update(nonce).digest('hex') });
  return until(() => sock.txt.some((m) => m.includes('"type":"admin","ok":true')), 4000);
}
const close = (L) => { for (const k of ['spec', 'aud', 'dx']) L[k]?.close(); };


const angDiff = (a, b) => Math.abs(((a - b + 540) % 360) - 180);

// ── 1. VOR on a one-pipeline radio ──
{
  console.log('── one-pipeline radio, AM on 113.60 MHz: a VOR "LON" on radial 247.5° ──');
  const S = await server(['--freq', String(VOR), '--mode', 'am', '--users', '1'], 'vor', VOR - 25000, VOR_CLIP);
  try {
    const st = await S.status();
    ok(st && st.vor === true && st.sarsat === true, `vibeserver.json: vor ${JSON.stringify(st && st.vor)}, sarsat ${JSON.stringify(st && st.sarsat)}`);
    ok(st && Array.isArray(st.modes) && st.modes.includes('vor') && st.modes.includes('sarsat'), 'modes lists vor and sarsat');
    const L = await listener(S.port, 'vorone1', VOR);
    L.dx.send({ type: 'audio_extension_attach', extension_name: 'vor' });
    const held = () => { const j = lastOf(L.dx, 'vor'); return j && j.data.kind === 'vor' && j.data.ident === 'LON'; };
    ok(await until(held, 40000), `a bearing held and the ident LON confirmed (${JSON.stringify(lastOf(L.dx, 'vor')?.data.q)})`);
    const j = lastOf(L.dx, 'vor');
    ok(j && Math.abs(j.data.radial - 247.5) < 1.0, `radial ${j && j.data.radial}° (sent 247.5°), σ ${j && j.data.sigma}°`);
    ok(j && j.data.station && j.data.station.ident === 'LON' && /London/i.test(j.data.station.name) && j.data.station.cc === 'GB', `the station: ${JSON.stringify(j && j.data.station)}`);
    ok(j && typeof j.data.trueBrg === 'number' && angDiff(j.data.trueBrg, 247.5 + j.data.station.var) < 1.0, `true bearing ${j && j.data.trueBrg}° (radial + the station's variation)`);
    ok(j && j.data.expect === 'vor' && j.data.chan === '113.60' && j.data.rate >= 24000, `channel ${j && j.data.chan}, expect ${j && j.data.expect}, channel IQ at ${j && j.data.rate} Hz`);
    const t = text01(L.dx);
    ok(/VOR LON London \(113\.60 MHz\)\s+radial 24[78]\.\d° FROM/.test(t), `the line: ${JSON.stringify(t.split('\n').find((l) => /VOR LON/.test(l)))}`);
    ok(!/passes .* km from this server/.test(everything(L.dx)), '★ not signed in: no "the radial passes … from this server"');
    const states = L.dx.bin.filter((b) => b[0] === 0x03).map((b) => b[1]);
    ok(states.includes(1), `state 1 (bearing held) sent (${[...new Set(states)].join(',')})`);
    // the owner signed in gets the check
    ok(await signIn(S, L.spec), 'the owner signs in');
    L.dx.bin.length = 0; L.dx.txt.length = 0;
    L.dx.send({ type: 'audio_extension_attach', extension_name: 'vor' });
    // ★ the same decoder joined (the line comes with the once-a-minute repeat)
    ok(await until(() => /the radial passes [\d.]+ km from this server/.test(text01(L.dx)), 75000), `signed in: ${JSON.stringify(text01(L.dx).split('\n').find((l) => /passes/.test(l)) || '')}`);
    L.dx.send({ type: 'audio_extension_detach' });
    await sleep(1500);
    const n = L.dx.txt.filter((x) => x.includes('"type":"vor"')).length;
    await sleep(4000);
    ok(L.dx.txt.filter((x) => x.includes('"type":"vor"')).length === n, 'detached: the status stops');
    close(L);
  } finally { S.stop(); }
}

// ── 2. VOR on a range-locked radio (per-listener channels) ──
{
  console.log('── range-locked radio (per-listener channels), AM on 113.60 MHz ──');
  const C = VOR - 25000;
  const S = await server(['--freq', String(C), '--lock-freq', String(C), '--users', '4', '--mode', 'am', '--idle-grace', '0'], 'vlock', C, VOR_CLIP);
  try {
    const L = await listener(S.port, 'vorlck1', VOR);
    L.dx.send({ type: 'audio_extension_attach', extension_name: 'vor' });
    ok(await until(() => { const j = lastOf(L.dx, 'vor'); return j && j.data.kind === 'vor' && Math.abs(j.data.radial - 247.5) < 1.0; }, 40000),
       `per-listener: radial ${lastOf(L.dx, 'vor')?.data.radial}° at ${lastOf(L.dx, 'vor')?.data.rate} Hz`);
    close(L);
  } finally { S.stop(); }
}

// ── 3. 406 MHz on a shared dial: the owner sees identity and position, a listener never ──
{
  console.log('── shared dial, NFM on 406.037 MHz: three bursts (standard location EPIRB, aviation ELT, a self-test) ──');
  const S = await server(['--freq', String(SAR), '--mode', 'nfm', '--users', '4'], 'sar', SAR - 25000, SAR_CLIP, { users: 4 });
  try {
    const O = await listener(S.port, 'sarown1', SAR, 'nfm');
    const G = await listener(S.port, 'sargst1', SAR, 'nfm');
    ok(await signIn(S, O.spec), 'the owner signs in (on the spectrum socket)');
    O.dx.send({ type: 'audio_extension_attach', extension_name: 'sarsat' });
    await sleep(500);
    G.dx.send({ type: 'audio_extension_attach', extension_name: 'sarsat' });
    ok(await until(() => /authorities/.test(text01(G.dx)), 8000), 'the note on attach: real alerts go to the authorities');
    const three = (st) => /EPIRB · United Kingdom \(232\) · Standard location/.test(text01(st)) && /ELT · United Kingdom \(232\) · Aviation user/.test(text01(st))
                         && /SELF-TEST/.test(text01(st));
    ok(await until(() => three(O.dx) && three(G.dx) && /MMSI 232123456/.test(text01(O.dx)), 70000), 'both: the EPIRB, the ELT and the self-test heard');
    const ot = text01(O.dx);
    ok(/Hex ID [0-9A-F]{15}/.test(ot) && /MMSI 232123456/.test(ot) && /position 50\.71\d\d° N 1\.34\d\d° W/.test(ot) && /registration G-ABCD/.test(ot) && /shown to you only/.test(ot),
       `the owner: Hex ID, MMSI, position, registration (${JSON.stringify(ot.split('\n').find((l) => /MMSI/.test(l)))})`);
    const ge = everything(G.dx);
    ok(!/Hex ID|MMSI|232123456|G-ABCD|50\.71|position|1D043C/.test(ge), '★ the listener was sent no identity and no position at all');
    const j = lastOf(G.dx, 'sarsat');
    ok(j && j.data.bursts >= 3 && j.data.tests >= 1 && !JSON.stringify(j).match(/"lat"|"lon"|hex|MMSI|position/i), `{"type":"sarsat"}: ${JSON.stringify(j && j.data)}`);
    ok(/406\.03\d\d MHz/.test(text01(G.dx)), 'the frequency named');
    close(O); close(G);
  } finally { S.stop(); }
}

// ── 4. 406 MHz, per-listener, nobody signed in ──
{
  console.log('── range-locked radio, 406.037 MHz, nobody signed in ──');
  const C = SAR - 25000;
  const S = await server(['--freq', String(C), '--lock-freq', String(C), '--users', '4', '--mode', 'nfm', '--idle-grace', '0'], 'slock', C, SAR_CLIP);
  try {
    const L = await listener(S.port, 'sarlck1', SAR, 'nfm');
    L.dx.send({ type: 'audio_extension_attach', extension_name: 'sarsat' });
    ok(await until(() => /EPIRB · United Kingdom/.test(text01(L.dx)), 70000), 'per-listener: the EPIRB heard');
    ok(!/MMSI|Hex ID|position/.test(everything(L.dx)), '★ no identity or position without the owner');
    close(L);
  } finally { S.stop(); }
}

// ── 5. the owner's switches ──
{
  console.log('── the owner blocked both ──');
  const S = await server(['--freq', String(VOR), '--mode', 'am', '--users', '1'], 'off', VOR - 25000, VOR_CLIP, { blockedModes: 'vor,sarsat' });
  try {
    const st = await S.status();
    ok(st && st.vor === false && st.sarsat === false, `vibeserver.json: vor ${JSON.stringify(st && st.vor)}, sarsat ${JSON.stringify(st && st.sarsat)}`);
    const L = await listener(S.port, 'voroff1', VOR);
    L.dx.send({ type: 'audio_extension_attach', extension_name: 'vor' });
    await sleep(8000);
    ok(!lastOf(L.dx, 'vor'), 'an attach is refused: no VOR status');
    close(L);
  } finally { S.stop(); }
}

console.log(`\n${fail ? '✗' : '✓'} test-server-vorsar: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
