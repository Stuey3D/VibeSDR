// ★★★ WHAT PLAYS WHEN A LISTENER CHANGES MULTIPLEX WITHOUT NAMING A STATION — end to end (2026-11-12).
// Stuart: "last on MUX is good, but with one extra rule … in the case where a station is on multiple multiplexes then
// switch to that one instead" (Nation 80s on 10C and 10D). And the squeal and "100 % frames lost" he heard and saw
// before any station was chosen — the old service's sub-channel decoded from the new block. The server decides
// (vibe_dab_last_service.h); every listener sees its choice as the report's `sid` (the shared-dial contract).
//   1. entering DAB with no station: silent and counting nothing until the list is read, then the FIRST in the list;
//      a second listener's report names the same station;
//   2. a station chosen, then a step to another block that carries it (fake-rtl-tcp plays the clip on every Band III
//      block — the same ensemble on two blocks, as Nation 80s on 10C and 10D): the SAME station, by SId;
//   3. a step that names a station: that one wins;
//   4. a restart: entering DAB on a block with no station named plays the LAST one played there.
// The DX sweep / scans (the server's own moves) and the rule's order are held by test-dab-station-choice and
// test-dab-last-service (C++, the same code without a socket).
// ★ Real server, real multiplex: the bench clip (VIBE_DAB_CLIP, or vibeserver/bench-clip/dab-bench-clean.vbu8). Without
//   it or VIBESERVER_BIN: NOT RUN (exit 3). Silent (no audio socket), loopback only.
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_DAB_CLIP || path.join(root, 'vibeserver/bench-clip/dab-bench-clean.vbu8');
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN)'); process.exit(3); }
if (!fs.existsSync(CLIP)) { console.log('   (not run — no bench clip; VIBE_DAB_CLIP= or vibeserver/bench-clip/make-clip.sh)'); process.exit(3); }

const hdr = fs.readFileSync(CLIP).subarray(0, 28);
const centre = hdr.readDoubleLE(16);
const BAND3 = { '5A': 174928000, '5B': 176640000, '5C': 178352000, '5D': 180064000, '12B': 225648000, '12C': 227360000, '12D': 229072000, '11D': 222064000, '10C': 213360000, '10D': 215072000 };
const home = Object.keys(BAND3).find((k) => Math.abs(BAND3[k] - centre) < 1000);
if (!home) { console.log(`   FAIL the clip's centre ${centre} is not a block this test knows`); process.exit(1); }
const away = home === '10D' ? '10C' : '10D';

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-dabchoice-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'DAB choice', locator: 'IO92nh',
  adminPass: 'dab-choice-admin', radios: [{ serial: 'dabchoice', label: 'T', enabled: true, configured: true, users: 3 }] }));
const raw = path.join(dir, 'clip.u8');
fs.writeFileSync(raw, fs.readFileSync(CLIP).subarray(28));   // VIBEBU81 header: magic + rate + centre + sid
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000', '--dab', raw], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
let srv = null;
const log = fs.openSync(path.join(dir, 'server.log'), 'a');
async function startServer() {
  srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', '3'],
              { env, stdio: ['ignore', log, log] });
  for (let i = 0; i < 100; i++) {
    if (await new Promise((r) => { const s = net.connect(port, '127.0.0.1'); s.on('connect', () => { s.end(); r(true); }); s.on('error', () => r(false)); })) return;
    await sleep(100);
  }
}
async function stopServer() {
  if (!srv) return;
  const done = new Promise((r) => srv.once('exit', r));
  srv.kill('SIGTERM');
  await Promise.race([done, sleep(8000)]);
  srv = null;
}
function client(tag) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=dc${tag}${Math.random().toString(16).slice(2, 8)}&bins=256&proto=1`);
  const c = { ws, msgs: [], opened: null };
  c.opened = new Promise((r) => { ws.onopen = r; });
  ws.onmessage = (e) => { if (typeof e.data === 'string') { try { c.msgs.push(JSON.parse(e.data)); } catch {} } };
  c.send = (o) => ws.send(JSON.stringify(o));
  c.dab = (from = 0) => c.msgs.slice(from).filter((m) => m.type === 'dab' && m.channel !== undefined);
  c.last = (from = 0) => c.dab(from).at(-1);
  return c;
}
const until = async (f, ms = 20000) => { for (let i = 0; i < ms / 100; i++) { if (f()) return true; await sleep(100); } return !!f(); };

try {
  await startServer();
  const a = client('a'), b = client('b');
  await a.opened; await b.opened;

  console.log(`── 1. into DAB on ${home}, no station named ──`);
  let mark = a.msgs.length;
  a.send({ type: 'dab', on: 1, channel: home });
  ok(await until(() => a.dab(mark).some((m) => m.channel === home && m.locked)), `${home} locks`);
  const early = a.dab(mark).filter((m) => m.channel === home && !(m.services?.length));
  ok(early.every((m) => !m.sid && !m.sfTried && !m.mp2In && !m.auIn),
     `before the list: no station, nothing decoded (${early.length} reports)`);
  ok(await until(() => (a.last(mark)?.sid ?? 0) > 0), 'a station is chosen once the list has arrived');
  const r1 = a.last(mark);
  const list = (r1.services ?? []).map((s) => s.sid);
  ok(list.length >= 3 && r1.sid === list[0], `the first in the list (${r1.sid?.toString(16)} of ${list.map((s) => s.toString(16)).join(' ')})`);
  ok(await until(() => b.last()?.sid === r1.sid, 5000), 'the other listener\'s report names the same station (shared dial)');
  ok(await until(() => (a.last()?.sfOk ?? 0) > 0 || (a.last()?.mp2In ?? 0) > 0, 10000), '…and it decodes');

  console.log(`── 2. the same station on ${away} ──`);
  const third = list[2];
  a.send({ type: 'dab_service', sid: third });
  ok(await until(() => a.last()?.sid === third, 8000), `a station chosen (${third.toString(16)})`);
  await sleep(2000);
  mark = a.msgs.length;
  a.send({ type: 'dab', on: 1, channel: away });
  ok(await until(() => a.dab(mark).some((m) => m.channel === away && m.locked)), `${away} locks (the same ensemble)`);
  ok(await until(() => a.dab(mark).some((m) => m.channel === away && m.sid > 0)), 'a station is chosen');
  ok(a.dab(mark).filter((m) => m.channel === away && m.sid > 0).at(-1)?.sid === third, 'the SAME station, by SId — not the first');

  console.log(`── 3. a step that names a station ──`);
  const second = list[1];
  mark = a.msgs.length;
  a.send({ type: 'dab', on: 1, channel: home, sid: second });
  ok(await until(() => a.dab(mark).some((m) => m.channel === home && m.sid === second)), `the named station plays (${second.toString(16)})`);
  await sleep(1500);
  ok(a.dab(mark).filter((m) => m.channel === home && m.sid > 0).every((m) => m.sid === second), '…and nothing else was chosen first');
  await sleep(2000);   // remembered for this multiplex

  console.log('── 4. a restart: the last one played here ──');
  a.ws.close(); b.ws.close();
  await stopServer();
  ok(fs.readdirSync(path.join(dir, 'data')).some((f) => f.startsWith('dab-last')), `the store is on disk (${fs.readdirSync(path.join(dir, 'data')).filter((f) => f.startsWith('dab-')).join(', ')})`);
  await startServer();
  const c = client('c'); await c.opened;
  await sleep(1500);
  mark = c.msgs.length;
  c.send({ type: 'dab', on: 1, channel: home });
  ok(await until(() => c.dab(mark).some((m) => m.channel === home && m.sid > 0), 25000), 'a station is chosen');
  ok(c.dab(mark).filter((m) => m.channel === home && m.sid > 0).at(-1)?.sid === second, `the last one played on ${home} (${second.toString(16)}), not the first`);
  c.ws.close();
} finally {
  await stopServer();
  try { rtl.kill('SIGTERM'); } catch {}
}
console.log(`test-server-dab-station-choice: ${pass} passed, ${fail} failed${fail ? ` (log: ${path.join(dir, 'server.log')})` : ''}`);
if (!fail) fs.rmSync(dir, { recursive: true, force: true });
process.exit(fail ? 1 : 0);
