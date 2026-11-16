// test-server-balloon.mjs — V12 BALLOONS through the REAL server (vibe_balloon_run.h, docs/v12/BALLOONS.md): a fake rtl_tcp
// replaying test-balloon's synthetic clip — our own transmitter, a payload on 434.200 MHz USB sending, on a ~28 s loop, two
// UKHAS sentences at 300 Bd 8N2 (VIBE1), the SSDV packets of a 96 × 64 picture at 600 Bd (VIBE1 picture 7) and two Horus
// v2 packets (ID 256) — and the contract:
//   • vibeserver.json says "balloon": true (an RTL reaches 70 cm), and a radio whose owner blocked it says false;
//   • 'balloon' on a ONE-PIPELINE radio and on a RANGE-LOCKED one: {"type":"balloon"} reports with VIBE1's two sentences
//     and HORUS-256's two packets, every field as sent; the picture as 0x0B frames, ending complete, a whole JPEG;
//     a text line per report; states 1 then 3; nothing that was not sent;
//   • the manual pins: a WRONG speed pinned (50 Bd) finds no UKHAS but Horus still; the right one (300 Bd, 8 bits) finds it;
//     "horus":false runs no Horus;
//   • detached: the reports stop.
// Silent: no audio is played (the audio socket's frames are dropped unread). Every socket sends the client's ping.
//   VIBESERVER_BIN=…/vibeserver VIBE_BALLOON_CLIP=…/clip.cu8 node scripts/test-server-balloon.mjs   (test-balloon clip clip.cu8 288000 60)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_BALLOON_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_BALLOON_CLIP (make one: test-balloon clip clip.cu8 288000 60)');
  process.exit(3);
}
const RATE = 288000, CENTRE = 434175000, DIAL = CENTRE + 25000;   // ★ the clip's payload: USB on 434.200 MHz
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
  // ★ THE REAL CLIENT'S KEEPALIVE: the server releases a proto-1 session silent for 30 s, and this test waits longer
  const keep = setInterval(() => { if (st.open) st.send({ type: 'ping' }); }, 8000);
  s.on('close', () => clearInterval(keep));
  st.close = () => { clearInterval(keep); try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
const jsons = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => { try { return JSON.parse(t); } catch { return null; } }).filter(Boolean);
const lastOf = (st, type) => jsons(st, type).at(-1) || null;
const text01 = (st) => st.bin.filter((b) => b[0] === 0x01 && b.length >= 13).map((b) => b.subarray(13, 13 + b.readUInt32BE(9)).toString()).join('');
const pictures = (st) => st.bin.filter((b) => b[0] === 0x0b).map((b) => {
  const kl = b[1], p = 2 + kl;
  return { key: b.subarray(2, 2 + kl).toString(), have: b.readUInt16BE(p), expected: b.readUInt16BE(p + 2), complete: b[p + 4] === 1,
           w: b.readUInt16BE(p + 5), h: b.readUInt16BE(p + 7), jpeg: b.subarray(p + 9) };
});

async function server(args, tag, radioExtra = {}, horusList = null) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-balloon-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  /* ★ V12 Horus payload names (vibe_horus_names.h): the server fetches Project Horus's list at most once a day. A test
   *  never touches the network — "checked" in the future means today's try is done; `horusList` seeds the copy on disk. */
  fs.mkdirSync(path.join(dir, 'data', 'horus'));
  fs.writeFileSync(path.join(dir, 'data', 'horus', 'checked'), String(Math.floor(Date.now() / 1000) + 10 * 86400));
  if (horusList) fs.writeFileSync(path.join(dir, 'data', 'horus', 'payload_id_list.txt'), horusList);
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Balloon test', locator: 'JO02ae', adminPass: 'balloon-test-admin',
    radios: [{ serial: `balloon-${tag}`, label: 'Test radio', enabled: true, configured: true, ...radioExtra }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', CLIP, '--hf-centre', String(CENTRE)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), ...args], { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  return { port, dir, status, stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}
async function listener(port, sid) {
  const L = {};
  L.spec = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256&proto=1`);
  await sleep(400);
  L.aud = ws(port, `/ws/audio?user_session_id=${sid}`); L.aud.dropBin = true;   // ★ silent: frames dropped unread
  L.dx = ws(port, `/ws/dxcluster?user_session_id=${sid}`);
  await sleep(400);
  L.spec.send({ type: 'tune', frequency: DIAL, mode: 'usb' });
  await sleep(600);
  return L;
}
const payload = (D, call) => (lastOf(D, 'balloon')?.data.payloads ?? []).find((p) => p.call === call);
const sawCount = (D, call, n) => jsons(D, 'balloon').some((m) => m.data.payloads.some((p) => p.call === call && p.count === n));

async function checkFull(D, label) {
  const all = async () => sawCount(D, 'VIBE1', 1) && sawCount(D, 'VIBE1', 2) && sawCount(D, 'HORUS-256', 501) && sawCount(D, 'HORUS-256', 502)
                         && pictures(D).some((p) => p.complete);
  ok(await until(all, 75000), `${label}: both UKHAS sentences, both Horus packets and the whole picture within 75 s`);
  const seen = jsons(D, 'balloon').flatMap((m) => m.data.payloads.map((p) => `${p.call}#${p.count}`));
  const want = new Set(['VIBE1#1', 'VIBE1#2', 'HORUS-256#501', 'HORUS-256#502']);
  ok(seen.every((s) => want.has(s)), `${label}: nothing reported that was not sent (${[...new Set(seen)].join(' ')})`);
  const u = jsons(D, 'balloon').flatMap((m) => m.data.payloads).find((p) => p.call === 'VIBE1' && p.count === 2);
  ok(u && u.src === 'ukhas' && u.time === '10:00:05' && Math.abs(u.lat - 52.20601) < 1e-6 && Math.abs(u.lon - 0.1195) < 1e-6 && u.alt === 31050
     && u.extra.join(',') === '9,-12' && /^\$\$VIBE1,2,10:00:05,/.test(u.sentence), `${label}: VIBE1 #2 as sent: ${JSON.stringify(u)}`);
  const h = jsons(D, 'balloon').flatMap((m) => m.data.payloads).find((p) => p.call === 'HORUS-256' && p.count === 502);
  ok(h && h.src === 'horus' && h.time === '10:00:35' && Math.abs(h.lat - 52.211) < 1e-4 && Math.abs(h.lon - 0.121) < 1e-4 && h.alt === 31250
     && h.speed === 42 && h.sats === 11 && h.temp === -31 && Math.abs(h.batt - 2.94) < 0.01, `${label}: HORUS-256 #502 as sent: ${JSON.stringify(h)}`);
  const last = lastOf(D, 'balloon');
  ok(/300 Bd · 8N1/.test(last.data.rtty.settings) || /600 Bd · 8N1/.test(last.data.rtty.settings), `${label}: RTTY named "${last.data.rtty.settings}"`);
  const pic = pictures(D).find((p) => p.complete);
  ok(pic && pic.key === 'VIBE1#7' && pic.w === 96 && pic.h === 64 && pic.have === 3 && pic.expected === 3, `${label}: the picture's frame: ${pic && JSON.stringify({ ...pic, jpeg: pic.jpeg.length })}`);
  ok(pic && pic.jpeg[0] === 0xff && pic.jpeg[1] === 0xd8 && pic.jpeg.at(-2) === 0xff && pic.jpeg.at(-1) === 0xd9, `${label}: …a whole JPEG (${pic && pic.jpeg.length} bytes)`);
  const im = last.data.images.find((i) => i.key === 'VIBE1#7');
  ok(im && im.complete && im.mcus === im.total && im.total === 24, `${label}: the report says the picture is complete (${im && JSON.stringify(im)})`);
  const t = text01(D);
  ok(/VIBE1 #2  52\.20601, 0\.11950  31050 m · UKHAS/.test(t) && /HORUS-256 #501/.test(t) && /SSDV  VIBE1 picture 7 complete/.test(t), `${label}: the text lines`);
  const states = D.bin.filter((b) => b[0] === 0x03).map((b) => b[1]);
  ok(states.includes(3), `${label}: state 3 (decoding) sent (${[...new Set(states)].join(',')})`);
}

// ── 1. one-pipeline radio ──
{
  console.log('── one-pipeline radio (1 listener): balloons on 434.200 MHz USB ──');
  const S = await server(['--freq', String(DIAL), '--mode', 'usb', '--users', '1'], 'one');
  try {
    const st = await S.status();
    ok(st && st.balloon === true, `vibeserver.json: balloon ${JSON.stringify(st && st.balloon)} ${st && st.balloonWhy || ''}`);
    ok(st && Array.isArray(st.modes) && st.modes.includes('balloon'), 'modes lists balloon');
    const L = await listener(S.port, 'balloon01');
    L.dx.send({ type: 'audio_extension_attach', extension_name: 'balloon' });
    await checkFull(L.dx, 'one-pipeline');
    // ★ the manual pins: a wrong speed finds no UKHAS, Horus still runs
    L.dx.bin.length = 0; L.dx.txt.length = 0;
    L.dx.send({ type: 'audio_extension_attach', extension_name: 'balloon', baud_rate: 50, bits: 8 });
    ok(await until(() => sawCount(L.dx, 'HORUS-256', 501) || sawCount(L.dx, 'HORUS-256', 502), 40000), 'pinned to 50 Bd: Horus still decodes');
    await sleep(15000);
    ok(!jsons(L.dx, 'balloon').some((m) => m.data.payloads.some((p) => p.src === 'ukhas')), 'pinned to 50 Bd: no UKHAS sentence (it sends at 300)');
    ok((lastOf(L.dx, 'balloon')?.data.rtty.candidates ?? -1) === 2, `…and the bank is just the pinned speed × both polarities (${lastOf(L.dx, 'balloon')?.data.rtty.candidates})`);
    L.dx.bin.length = 0; L.dx.txt.length = 0;
    L.dx.send({ type: 'audio_extension_attach', extension_name: 'balloon', baud_rate: 300, bits: 8, horus: false });
    ok(await until(() => sawCount(L.dx, 'VIBE1', 1) || sawCount(L.dx, 'VIBE1', 2), 40000), 'pinned to 300 Bd 8 bits: the UKHAS sentences');
    await sleep(8000);
    ok(!jsons(L.dx, 'balloon').some((m) => m.data.payloads.some((p) => p.src === 'horus')) && jsons(L.dx, 'balloon').every((m) => m.data.horus.tones === null),
       '"horus": false: no Horus at all');
    L.dx.send({ type: 'audio_extension_detach' });
    await sleep(1500);
    const n = jsons(L.dx, 'balloon').length;
    await sleep(7000);
    ok(jsons(L.dx, 'balloon').length === n, `detached: the reports stop (${n} → ${jsons(L.dx, 'balloon').length})`);
    // ★ the gallery: the finished picture as a JPEG, and the reports as a text run (written when the decoder stopped)
    const g = await (await fetch(`http://127.0.0.1:${S.port}/vibeserver/gallery.json?offset=0&limit=100&kind=balloon`)).json();
    const items = g.items || [];
    const jpg = items.find((i) => /\.jpg$/.test(i.f) && /SSDV · VIBE1 · 96×64/.test(i.title || ''));
    ok(!!jpg, `the gallery keeps the picture (${items.map((i) => i.f + ' "' + (i.title || '') + '"').join(' | ')})`);
    if (jpg) {
      const r = await fetch(`http://127.0.0.1:${S.port}/vibeserver/gallery/file/${encodeURIComponent(jpg.f)}`);
      const b = Buffer.from(await r.arrayBuffer());
      ok(r.headers.get('content-type') === 'image/jpeg' && b[0] === 0xff && b[1] === 0xd8 && b.at(-1) === 0xd9, `…served as image/jpeg, a whole JPEG (${b.length} bytes)`);
    }
    const runs = items.filter((i) => /\.txt$/.test(i.f) && /Balloon telemetry/.test(i.title || ''));
    ok(runs.length > 0, `the gallery keeps the reports as "Balloon telemetry" runs (${runs.length}: one per run of the decoder)`);
    let t = '';
    for (const r of runs) t += await (await fetch(`http://127.0.0.1:${S.port}/vibeserver/gallery/file/${encodeURIComponent(r.f)}`)).text();
    ok(/VIBE1 #2  52\.20601, 0\.11950/.test(t) && /HORUS-256 #50[12]/.test(t), '…with the report lines in them');
    for (const k of ['spec', 'aud', 'dx']) L[k].close();
  } finally { S.stop(); }
}

// ── 2. range-locked radio ──
{
  console.log('── range-locked radio (per-listener channels): balloons ──');
  const S = await server(['--freq', String(CENTRE), '--lock-freq', String(CENTRE), '--users', '4', '--mode', 'usb', '--idle-grace', '0'], 'lock');
  try {
    const A = await listener(S.port, 'balloonl1');
    A.dx.send({ type: 'audio_extension_attach', extension_name: 'balloon' });
    await checkFull(A.dx, 'per-listener');
    for (const k of ['spec', 'aud', 'dx']) A[k].close();
  } finally { S.stop(); }
}

// ── 2b. Horus payload names (Project Horus's list, the server's own copy) ──
{
  console.log('── Horus payload names: 256 named by the list on disk ──');
  const S = await server(['--freq', String(DIAL), '--mode', 'usb', '--users', '1'], 'names', {}, '# test list\n0, 4FSKTEST\n256, VIBE-HAB\n');
  try {
    const L = await listener(S.port, 'balloon09');
    const ping = setInterval(() => L.spec.send({ type: 'ping' }), 2000);   // ★ keepalive
    L.dx.send({ type: 'audio_extension_attach', extension_name: 'balloon' });
    ok(await until(() => sawCount(L.dx, 'VIBE-HAB', 501) || sawCount(L.dx, 'VIBE-HAB', 502), 60000), 'payload 256 reported as VIBE-HAB, its name in the list');
    ok(!jsons(L.dx, 'balloon').some((m) => m.data.payloads.some((p) => p.call === 'HORUS-256')), '…never as HORUS-256');
    ok(/VIBE-HAB #50[12]/.test(text01(L.dx)), 'the text line names it too');
    ok(!fs.existsSync(path.join(S.dir, 'data', 'horus', 'payload_id_list.download')), 'no download attempted (today\'s try is done)');
    clearInterval(ping);
    for (const k of ['spec', 'aud', 'dx']) L[k].close?.();
  } finally { S.stop(); }
}

// ── 3. the owner's switch ──
{
  console.log('── the owner blocked it ──');
  const S = await server(['--freq', String(DIAL), '--mode', 'usb', '--users', '1'], 'off', { blockedModes: 'balloon' });
  try {
    const st = await S.status();
    ok(st && st.balloon === false && !st.balloonWhy, `vibeserver.json: balloon false, no reason (the owner's choice): ${JSON.stringify(st && st.balloon)}`);
  } finally { S.stop(); }
}

console.log(`\n${fail ? '✗' : '✓'} test-server-balloon: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
