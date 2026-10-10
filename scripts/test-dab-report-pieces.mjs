// ★★★ THE DAB REPORT IN PIECES, END TO END (2026-10-10). Stuart: "make the connection as efficient as possible …
// light on CPU on client and server and light on data". Measured on the XCover: the `dab` report was 74 % of everything a
// DAB listener received (32 KB/s; 12 KB of each report the station list, 1.8 KB the scopes). A `dab=2` socket now gets
// the live report, the list / radio text only when they change, the scopes only while its Signal pane is open.
//   1. a LEGACY socket (no dab=2) gets the full report, unchanged — Jr and older apps are untouched;
//   2. a dab=2 socket's live report carries no list and no scopes, and names the revisions it belongs to;
//   3. reassembled (src/services/dabAssemble.ts, the clients' own code) it EQUALS the legacy report's list —
//      so no screen can tell the difference;
//   4. it is a fraction of the bytes;
//   5. dab_scopes on → ir/iq arrive; off → they stop;
//   6. dab_resync → the list and radio text are sent again; leaving and re-entering DAB → sent again.
// ★ Real server, real multiplex: the bench clip (vibeserver/bench-clip/dab-bench-clean.vbu8, 12D — built by
//   make-clip.sh, not in git). Without it or VIBESERVER_BIN this reports NOT RUN, never a pass. Silent, loopback only.
import * as esbuild from 'esbuild';
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
const CLIP = path.join(root, 'vibeserver/bench-clip/dab-bench-clean.vbu8');
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN)'); process.exit(3); }
if (!fs.existsSync(CLIP)) { console.log('   (not run — no bench clip; vibeserver/bench-clip/make-clip.sh builds it)'); process.exit(3); }

// The clients' own assembler, bundled exactly as they import it.
const built = await esbuild.build({ entryPoints: [path.join(root, 'src/services/dabAssemble.ts')], bundle: true, write: false,
  format: 'esm', platform: 'neutral', logLevel: 'silent' });
const { DabAssembler } = await import('data:text/javascript;base64,' + Buffer.from(built.outputFiles[0].text).toString('base64'));

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-dabparts-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'DAB pieces', locator: 'IO92nh',
  adminPass: 'dab-pieces-admin', radios: [{ serial: 'dabparts', label: 'T', enabled: true, configured: true, users: 3 }] }));
const raw = path.join(dir, 'clip.u8');
fs.writeFileSync(raw, fs.readFileSync(CLIP).subarray(28));   // VIBEBU81 header: magic + rate + centre + sid
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000', '--dab', raw], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const log = fs.openSync(path.join(dir, 'server.log'), 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', '3'],
                  { env, stdio: ['ignore', log, log] });

function client(v2) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=dp${v2 ? 'v2' : 'v1'}${Math.random().toString(16).slice(2, 8)}&bins=256&proto=1${v2 ? '&dab=2' : ''}`);
  const c = { ws, msgs: [], bytes: { dab: 0, dab_list: 0, dab_dls: 0 }, counting: false, asm: new DabAssembler(), report: null, opened: null };
  c.opened = new Promise((r) => { ws.onopen = r; });
  ws.onmessage = (e) => {
    if (typeof e.data !== 'string') return;
    const j = JSON.parse(e.data);
    c.msgs.push(j);
    if (c.counting && j.type in c.bytes) c.bytes[j.type] += Buffer.byteLength(e.data);
    const r = c.asm.ingest(j, Date.now());
    if (r.report) c.report = r.report;
    if (r.resync) ws.send('{"type":"dab_resync"}');
  };
  c.send = (o) => ws.send(JSON.stringify(o));
  c.since = (i) => c.msgs.slice(i);
  return c;
}
const until = async (f, ms = 15000) => { for (let i = 0; i < ms / 100; i++) { if (f()) return true; await sleep(100); } return !!f(); };

try {
  for (let i = 0; i < 100; i++) {
    if (await new Promise((r) => { const s = net.connect(port, '127.0.0.1'); s.on('connect', () => { s.end(); r(true); }); s.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const old = client(false), neu = client(true);
  await old.opened; await neu.opened;
  const BLOCKS = ['5A','5B','5C','5D','6A','6B','6C','6D','7A','7B','7C','7D','8A','8B','8C','8D','9A','9B','9C','9D','10A','10N','10B','10C','10D','11A','11N','11B','11C','11D','12A','12N','12B','12C','12D'];
  neu.send({ type: 'dab', on: 1, channel: BLOCKS.indexOf('12D') });
  const full = () => old.msgs.filter((m) => m.type === 'dab' && Array.isArray(m.services) && m.services.length >= 10).at(-1);
  ok(await until(() => !!full(), 25000), `the clip locks and lists its services (legacy: ${full()?.services?.length ?? 0})`);
  await sleep(4000);   // let the list settle (services keep arriving while the FIC is read)

  console.log('── a legacy socket ──');
  const lg = full();
  ok(lg && lg.v === undefined && Array.isArray(lg.services) && !old.msgs.some((m) => m.type === 'dab_list' || m.type === 'dab_dls'),
     'gets the full report, unchanged (no v, the list inside, no pieces)');

  console.log('── a dab=2 socket ──');
  const live = neu.msgs.filter((m) => m.type === 'dab').at(-1);
  ok(live?.v === 2 && !('services' in live) && !('iq' in live) && !('ir' in live) && /^[0-9a-f]{16}$/.test(live.listRev ?? ''),
     `the live report: no list, no scopes, names its revisions (listRev ${live?.listRev})`);
  const lgNow = full();
  const strip = (s) => { const { dls, dlsAge, dlp, dlpRunning, ...rest } = s; return rest; };
  const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
  ok(same((neu.report?.services ?? []).map(strip), (lgNow?.services ?? []).map(strip)),
     `reassembled, its station list EQUALS the legacy one (${neu.report?.services?.length} services)`);
  ok(same(neu.report?.blocks, lgNow?.blocks) && same(neu.report?.dataSvcs, lgNow?.dataSvcs) && same(neu.report?.licensed, lgNow?.licensed),
     '…and blocks, data services and licensed sites too');
  const texts = (r) => (r?.services ?? []).filter((s) => s.dls).map((s) => `${s.sid}:${s.dls}`).sort().join('|');
  ok(texts(neu.report) === texts(lgNow), `…and every service's radio text (${(lgNow?.services ?? []).filter((s) => s.dls).length} with text)`);

  // ── bytes over the same 8 s ──
  old.counting = neu.counting = true;
  await sleep(8000);
  old.counting = neu.counting = false;
  const ob = old.bytes.dab, nb = neu.bytes.dab + neu.bytes.dab_list + neu.bytes.dab_dls;
  ok(nb < ob * 0.6, `a fraction of the bytes over 8 s: legacy ${ob} B, dab=2 ${nb} B (${Math.round(100 * nb / ob)} %)`);

  console.log('── the Signal pane ──');
  let mark = neu.msgs.length;
  const t0 = Date.now();
  neu.send({ type: 'dab_scopes', on: 1 });
  ok(await until(() => neu.since(mark).some((m) => m.type === 'dab' && Array.isArray(m.iq) && Array.isArray(m.ir)), 5000),
     'dab_scopes on: the constellation and impulse response arrive');
  // ★ At once, not at the next half-second tick — no empty constellation box when the pane opens (look and feel).
  ok(Date.now() - t0 < 250, `…immediately, not at the next tick (${Date.now() - t0} ms)`);
  ok(Array.isArray(neu.report?.iq), '…and reach the assembled report');
  neu.send({ type: 'dab_scopes', on: 0 });
  await sleep(1200);
  mark = neu.msgs.length;
  await sleep(1500);
  ok(neu.since(mark).filter((m) => m.type === 'dab').every((m) => !('iq' in m)), 'dab_scopes off: they stop');

  console.log('── resync, and a new DAB session ──');
  mark = neu.msgs.length;
  neu.send({ type: 'dab_resync' });
  ok(await until(() => neu.since(mark).some((m) => m.type === 'dab_list') && neu.since(mark).some((m) => m.type === 'dab_dls'), 5000),
     'dab_resync: the list and radio text are sent again');
  mark = neu.msgs.length;
  neu.send({ type: 'dab', on: 0 });
  ok(await until(() => neu.since(mark).some((m) => m.type === 'dab_off'), 8000), 'leaving DAB: dab_off');
  ok(neu.report !== null && (new DabAssembler()).ingest({ type: 'dab', v: 2, channel: '12D' }, 0).report.services.length === 0,
     '(an assembler with no list yields an empty list, never a stale one)');
  mark = neu.msgs.length;
  neu.send({ type: 'dab', on: 1, channel: BLOCKS.indexOf('12D') });
  ok(await until(() => neu.since(mark).some((m) => m.type === 'dab_list'), 15000), 're-entering DAB: the list is sent again from scratch');
  old.ws.close(); neu.ws.close();
} finally {
  try { srv.kill('SIGTERM'); } catch {}
  try { rtl.kill('SIGTERM'); } catch {}
}
console.log(`test-dab-report-pieces: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
